/**
 * @file WebPanel.cpp
 * @brief WebPanel: HTTP server, routes, login, API handlers, OTA upload, live logs.
 */
#include "WebPanel.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <algorithm>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "mbedtls/pkcs5.h"

#include "Secrets.h"
#include "LogBuffer.h"
#include "StringOutput.h"
#include "WebAssets.h"

#define LOG_TAG     "WebPanel"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::web; // factory settings of web.json

#ifndef SECRET_WEB_PASS
#define SECRET_WEB_PASS "admin"
#define WEB_PASS_FALLBACK 1
#endif

static constexpr const char* AUTH_FILE = "web_auth.json";
static constexpr int PBKDF2_ITERATIONS = 10000;
static constexpr int MAX_FAILURES = 5;
static constexpr int64_t LOCKOUT_US = 60LL * 1000000;
static constexpr size_t MAX_BODY = 512;
static constexpr size_t PASS_MIN = 8;
static constexpr size_t PASS_MAX = 64;

// Bytes -> lower-case hex string (zero-terminated).
static void toHex(const uint8_t* data, size_t len, char* out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

// Hex string -> len bytes; false on a wrong length or character.
static bool fromHex(const char* s, uint8_t* out, size_t len)
{
    if (s == nullptr || strlen(s) != 2 * len) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

// PBKDF2-HMAC-SHA256 of the password with the salt (fixed number of iterations).
static bool pbkdf2(const char* password, const uint8_t salt[16], uint8_t out[32])
{
    return mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password),
                                         strlen(password), salt, 16, PBKDF2_ITERATIONS, 32, out) == 0;
}

// Constant-time comparison (no early exit).
static bool equalConstTime(const uint8_t* a, const uint8_t* b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

WebPanel::WebPanel(const GlobalSettings& global, WifiManager& wifi, ConfigStore& store, const CommandExecutor& executor,
                   const SystemCommands& system, const ServiceManager& services, const DhcpServer& dhcp,
                   PortForwarder& portfwd, const NatManager& nat, DnsServer& dns, const DdnsClient& ddns,
                   Firewall& firewall)
    : global(global),
      wifi(wifi),
      store(store),
      executor(executor),
      system(system),
      services(services),
      dhcp(dhcp),
      portfwd(portfwd),
      nat(nat),
      dns(dns),
      ddns(ddns),
      firewall(firewall)
{
}

// ---- configuration ----

void WebPanel::loadConfig(ConfigReader& r)
{
    cfg.port = r.integer("port", dflt::PORT, dflt::PORT_MIN, dflt::PORT_MAX);
    cfg.sessionTimeoutS = r.integer("session_timeout_s", dflt::SESSION_TIMEOUT_S, dflt::SESSION_TIMEOUT_S_MIN, dflt::SESSION_TIMEOUT_S_MAX);
    cfg.maxSessions = r.integer("max_sessions", dflt::MAX_SESSIONS, dflt::MAX_SESSIONS_MIN, dflt::MAX_SESSIONS_MAX);
}

cJSON* WebPanel::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "port", cfg.port);
    cJSON_AddNumberToObject(root, "session_timeout_s", cfg.sessionTimeoutS);
    cJSON_AddNumberToObject(root, "max_sessions", cfg.maxSessions);
    return root;
}

void WebPanel::loadAuth()
{
    cJSON* json = nullptr;
    bool ok = false;
    if (store.load(AUTH_FILE, &json) == ConfigStore::LoadResult::OK) {
        const cJSON* s = cJSON_GetObjectItemCaseSensitive(json, "salt");
        const cJSON* h = cJSON_GetObjectItemCaseSensitive(json, "hash");
        const cJSON* it = cJSON_GetObjectItemCaseSensitive(json, "iterations");
        std::lock_guard<std::mutex> lock(mutex);
        ok = cJSON_IsString(s) && cJSON_IsString(h) && cJSON_IsNumber(it) && it->valueint == PBKDF2_ITERATIONS &&
             fromHex(s->valuestring, salt, sizeof(salt)) && fromHex(h->valuestring, hash, sizeof(hash));
        hasHash = ok;
        if (!ok) {
            LOG_E("%s uszkodzony - obowiazuje haslo z Secrets.h", AUTH_FILE);
        }
    }
    cJSON_Delete(json);

    if (!ok) {
#ifdef WEB_PASS_FALLBACK
        LOG_W("!!! Brak SECRET_WEB_PASS w Secrets.h - domyslne haslo WWW: \"admin\" - zmien je !!!");
#else
        LOG_W("Panel WWW uzywa domyslnego hasla z Secrets.h - zmien je w panelu albo: web passwd <nowe>");
#endif
    }
}

bool WebPanel::checkPassword(const char* password) const
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!hasHash) {
        size_t a = strlen(password), b = strlen(SECRET_WEB_PASS);
        return a == b && equalConstTime(reinterpret_cast<const uint8_t*>(password),
                                        reinterpret_cast<const uint8_t*>(SECRET_WEB_PASS), a);
    }
    uint8_t out[32];
    return pbkdf2(password, salt, out) && equalConstTime(out, hash, sizeof(out));
}

// New password: random salt, PBKDF2-HMAC-SHA256, atomic write to web_auth.json.
bool WebPanel::setPassword(const char* password)
{
    if (password == nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        hasHash = false;
        return store.remove(AUTH_FILE);
    }

    uint8_t newSalt[16], newHash[32];
    esp_fill_random(newSalt, sizeof(newSalt));
    if (!pbkdf2(password, newSalt, newHash)) {
        return false;
    }
    char hex[65];
    cJSON* json = cJSON_CreateObject();
    toHex(newSalt, sizeof(newSalt), hex);
    cJSON_AddStringToObject(json, "salt", hex);
    toHex(newHash, sizeof(newHash), hex);
    cJSON_AddStringToObject(json, "hash", hex);
    cJSON_AddNumberToObject(json, "iterations", PBKDF2_ITERATIONS);
    bool ok = store.save(AUTH_FILE, json);
    cJSON_Delete(json);
    if (ok) {
        std::lock_guard<std::mutex> lock(mutex);
        memcpy(salt, newSalt, sizeof(salt));
        memcpy(hash, newHash, sizeof(hash));
        hasHash = true;
    }
    return ok;
}

void WebPanel::logoutAll(const Session* except)
{
    for (Session& s : sessions) {
        if (&s != except) {
            s.used = false;
        }
    }
}

// ---- service ----

esp_err_t WebPanel::start()
{
    if (server != nullptr) {
        return ESP_OK;
    }
    active = cfg;

    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.server_port = active.port;
    c.max_open_sockets = 6;
    c.lru_purge_enable = true;
    c.uri_match_fn = httpd_uri_match_wildcard;
    c.max_uri_handlers = 48; // 33 routes (0.14.0) + reserve; no room = the route does not work (e.g. "/*" -> 404)
    c.stack_size = 8192; // PBKDF2 and cJSON

    esp_err_t err = httpd_start(&server, &c);
    if (err != ESP_OK) {
        LOG_E("Nie udalo sie uruchomic serwera HTTP: %s", esp_err_to_name(err));
        server = nullptr;
        return err;
    }

    // Order matters: WebSocket and API before the generic "/*" for files
    // (otherwise "/*" takes the address and the registration fails).
    httpd_uri_t ws = {};
    ws.uri = "/ws/log";
    ws.method = HTTP_GET;
    ws.handler = &handleLogWs;
    ws.user_ctx = this;
    ws.is_websocket = true;
    int failed = 0;
    if (httpd_register_uri_handler(server, &ws) != ESP_OK) {
        LOG_E("Nie zarejestrowano %s", ws.uri);
        ++failed;
    }
    const httpd_uri_t uris[] = {
        { "/api/session", HTTP_GET, &handleSession, this },
        { "/api/login", HTTP_POST, &handleLogin, this },
        { "/api/logout", HTTP_POST, &handleLogout, this },
        { "/api/passwd", HTTP_POST, &handlePasswd, this },
        { "/api/status", HTTP_GET, &handleStatus, this },
        { "/api/config/*", HTTP_GET, &handleConfigGet, this },
        { "/api/config/*", HTTP_POST, &handleConfigPost, this },
        { "/api/save", HTTP_POST, &handleSave, this },
        { "/api/revert", HTTP_POST, &handleRevert, this },
        { "/api/cmd", HTTP_POST, &handleCmd, this },
        { "/api/services", HTTP_GET, &handleServices, this },
        { "/api/wifi/scan", HTTP_GET, &handleScan, this },
        { "/api/wifi/sta", HTTP_POST, &handleWifiSet, this },
        { "/api/wifi/ap", HTTP_POST, &handleWifiSet, this },
        { "/api/sys/ota", HTTP_GET, &handleOtaInfo, this },
        { "/api/ota", HTTP_POST, &handleOtaUpload, this },
        { "/api/backup", HTTP_GET, &handleBackup, this },
        { "/api/restore", HTTP_POST, &handleRestore, this },
        { "/api/reboot", HTTP_POST, &handleReboot, this },
        { "/api/clients", HTTP_GET, &handleJsonGet, this },
        { "/api/dhcp/leases", HTTP_GET, &handleJsonGet, this },
        { "/api/dhcp/reservations", HTTP_GET, &handleJsonGet, this },
        { "/api/pf", HTTP_GET, &handleJsonGet, this },
        { "/api/pf", HTTP_POST, &handlePfSet, this },
        { "/api/nat", HTTP_GET, &handleJsonGet, this },
        { "/api/sys/mem", HTTP_GET, &handleJsonGet, this },
        { "/api/ddns", HTTP_GET, &handleJsonGet, this },
        { "/api/firewall", HTTP_GET, &handleJsonGet, this },
        { "/api/firewall", HTTP_POST, &handleFwSet, this },
        { "/api/dns/block/import", HTTP_POST, &handleBlockImport, this },
        { "/api/dns/*", HTTP_GET, &handleDnsGet, this },
        { "/*", HTTP_GET, &handleAsset, this },
    };
    for (const httpd_uri_t& u : uris) {
        esp_err_t e = httpd_register_uri_handler(server, &u);
        if (e != ESP_OK) {
            LOG_E("Nie zarejestrowano %s: %s", u.uri, esp_err_to_name(e));
            ++failed;
        }
    }
    if (failed != 0) {
        LOG_E("Panel WWW: %d tras bez obslugi (max_uri_handlers?)", failed);
    }
    LOG_I("Panel WWW na porcie %u (%s)", active.port,
          global.values().mgmtFromSta ? "dostep z AP i STA" : "dostep tylko z AP");
    return ESP_OK;
}

esp_err_t WebPanel::stop()
{
    if (server == nullptr) {
        return ESP_OK;
    }
    if (logTask != nullptr) {
        logTaskStop = true;
        for (int i = 0; i < 50 && logTask != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    {
        std::lock_guard<std::mutex> lock(logMutex);
        for (LogClient& c : logClients) {
            c.fd = 0;
        }
    }
    esp_err_t err = httpd_stop(server);
    server = nullptr;
    std::lock_guard<std::mutex> lock(mutex);
    logoutAll(nullptr);
    return err;
}

void WebPanel::describe(IOutput& out) const
{
    if (server == nullptr) {
        return;
    }
    int count = 0;
    int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Session& s : sessions) {
            count += s.used && now - s.lastUsedUs < static_cast<int64_t>(active.sessionTimeoutS) * 1000000;
        }
    }
    out.printf(tr(" port %u, sesje %d/%d", " port %u, sessions %d/%d"), active.port, count, active.maxSessions);
}

// ---- HTTP helpers ----

// Like Telnet: a local connection address equal to the AP address means the AP side.
// IPv4 address of the server socket (sockets may be IPv6 with an IPv4-mapped address).
static uint32_t sockIp(int fd, bool peer)
{
    sockaddr_in6 addr = {};
    socklen_t len = sizeof(addr);
    int r = peer ? getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len)
                 : getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
    if (r != 0) {
        return 0;
    }
    if (addr.sin6_family == AF_INET) {
        return reinterpret_cast<sockaddr_in*>(&addr)->sin_addr.s_addr;
    }
    return addr.sin6_addr.un.u32_addr[3];
}

bool WebPanel::allowed(httpd_req_t* req) const
{
    if (global.values().mgmtFromSta) {
        return true;
    }
    esp_netif_ip_info_t ap = {};
    esp_netif_get_ip_info(wifi.apNetif(), &ap);
    return sockIp(httpd_req_to_sockfd(req), false) == ap.ip.addr;
}

uint32_t WebPanel::peerIp(httpd_req_t* req)
{
    return sockIp(httpd_req_to_sockfd(req), true);
}

esp_err_t WebPanel::sendJson(httpd_req_t* req, const char* status, cJSON* body)
{
    char* text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    cJSON_free(text);
    return err;
}

esp_err_t WebPanel::sendError(httpd_req_t* req, const char* status, const char* message)
{
    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "error", message);
    return sendJson(req, status, body);
}

// Request body as JSON; large bodies (backup restore) in PSRAM when available.
cJSON* WebPanel::readJsonBody(httpd_req_t* req, size_t limit)
{
    if (req->content_len == 0 || req->content_len > limit) {
        return nullptr;
    }
    char* buf = static_cast<char*>(heap_caps_malloc(req->content_len + 1, MALLOC_CAP_8BIT));
    if (buf == nullptr) {
        return nullptr;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return nullptr;
        }
        got += n;
    }
    buf[got] = '\0';
    cJSON* json = cJSON_Parse(buf);
    free(buf);
    return json;
}

// Session from the "sid" cookie: valid, not expired and from the same IP address. Refreshes its time.
WebPanel::Session* WebPanel::currentSession(httpd_req_t* req)
{
    char token[40];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, "sid", token, &len) != ESP_OK || strlen(token) != 32) {
        return nullptr;
    }
    uint32_t ip = peerIp(req);
    int64_t now = esp_timer_get_time();
    for (Session& s : sessions) {
        if (!s.used || !equalConstTime(reinterpret_cast<const uint8_t*>(s.token),
                                       reinterpret_cast<const uint8_t*>(token), 32)) {
            continue;
        }
        if (s.ip != ip || now - s.lastUsedUs > static_cast<int64_t>(active.sessionTimeoutS) * 1000000) {
            s.used = false;
            return nullptr;
        }
        s.lastUsedUs = now;
        return &s;
    }
    return nullptr;
}

// ---- handlers ----

esp_err_t WebPanel::handleAsset(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->allowed(req)) {
        return sendError(req, "403 Forbidden", tr("Dostęp tylko z sieci AP", "Access only from the AP network"));
    }

    char path[64];
    strlcpy(path, req->uri, sizeof(path));
    char* query = strchr(path, '?');
    if (query != nullptr) {
        *query = '\0';
    }
    if (strcmp(path, "/") == 0) {
        strlcpy(path, "/index.html", sizeof(path));
    }

    for (size_t i = 0; i < WEB_ASSET_COUNT; ++i) {
        const WebAsset& a = WEB_ASSETS[i];
        if (strcmp(a.path, path) != 0) {
            continue;
        }
        httpd_resp_set_hdr(req, "ETag", a.etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
        char match[48];
        if (httpd_req_get_hdr_value_str(req, "If-None-Match", match, sizeof(match)) == ESP_OK &&
            strcmp(match, a.etag) == 0) {
            httpd_resp_set_status(req, "304 Not Modified");
            return httpd_resp_send(req, nullptr, 0);
        }
        httpd_resp_set_type(req, a.mime);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
        return httpd_resp_send(req, reinterpret_cast<const char*>(a.data), a.size);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, tr("Nie znaleziono", "Not found"));
}

esp_err_t WebPanel::handleSession(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->allowed(req)) {
        return sendError(req, "403 Forbidden", tr("Dostęp tylko z sieci AP", "Access only from the AP network"));
    }
    cJSON* body = cJSON_CreateObject();
    bool auth;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        auth = self->currentSession(req) != nullptr;
    }
    cJSON_AddBoolToObject(body, "auth", auth);
    cJSON_AddStringToObject(body, "language", i18n::code(i18n::language())); // also for the login screen
    if (auth) {
        cJSON_AddStringToObject(body, "hostname", self->global.values().hostname.c_str());
        cJSON_AddStringToObject(body, "version", esp_app_get_description()->version);
        cJSON_AddNumberToObject(body, "uptime", static_cast<double>(esp_timer_get_time() / 1000000));
        cJSON_AddBoolToObject(body, "default_password", self->usingDefaultPassword());
    }
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleLogin(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->allowed(req)) {
        return sendError(req, "403 Forbidden", tr("Dostęp tylko z sieci AP", "Access only from the AP network"));
    }
    uint32_t ip = peerIp(req);
    int64_t now = esp_timer_get_time();

    // Lockout after 5 failed attempts from this address.
    Failures* f = nullptr;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        Failures* oldest = &self->failures[0];
        for (Failures& x : self->failures) {
            if (x.ip == ip) {
                f = &x;
            } else if (x.lockedUntilUs < oldest->lockedUntilUs) {
                oldest = &x;
            }
        }
        if (f != nullptr && f->lockedUntilUs > now) {
            char msg[64];
            snprintf(msg, sizeof(msg), tr("Za dużo prób – spróbuj za %ld s", "Too many attempts – try again in %ld s"),
                     static_cast<long>((f->lockedUntilUs - now) / 1000000 + 1));
            return sendError(req, "429 Too Many Requests", msg);
        }
        if (f == nullptr) {
            f = oldest;
            *f = { ip, 0, 0 };
        }
    }

    cJSON* json = readJsonBody(req);
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool ok = cJSON_IsString(pass) && self->checkPassword(pass->valuestring);
    cJSON_Delete(json);

    std::lock_guard<std::mutex> lock(self->mutex);
    if (!ok) {
        if (++f->count >= MAX_FAILURES) {
            f->count = 0;
            f->lockedUntilUs = now + LOCKOUT_US;
            esp_ip4_addr_t a = { ip };
            LOG_W("Panel WWW: %d blednych logowan z " IPSTR " - blokada na 60 s", MAX_FAILURES, IP2STR(&a));
        }
        return sendError(req, "401 Unauthorized", tr("Nieprawidłowe hasło", "Wrong password"));
    }
    f->count = 0;

    // New session; when there is no room the least recently used one is dropped.
    Session* s = nullptr;
    for (int i = 0; i < self->active.maxSessions; ++i) {
        Session& x = self->sessions[i];
        bool expired = now - x.lastUsedUs > static_cast<int64_t>(self->active.sessionTimeoutS) * 1000000;
        if (!x.used || expired) {
            s = &x;
            break;
        }
        if (s == nullptr || x.lastUsedUs < s->lastUsedUs) {
            s = &x;
        }
    }
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    toHex(raw, sizeof(raw), s->token);
    s->used = true;
    s->ip = ip;
    s->createdUs = s->lastUsedUs = now;

    char cookie[96];
    snprintf(cookie, sizeof(cookie), "sid=%s; Path=/; HttpOnly; SameSite=Strict", s->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    esp_ip4_addr_t a = { ip };
    LOG_I("Panel WWW: zalogowano z " IPSTR, IP2STR(&a));
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleLogout(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        Session* s = self->currentSession(req);
        if (s != nullptr) {
            s->used = false;
        }
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handlePasswd(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->allowed(req)) {
        return sendError(req, "403 Forbidden", tr("Dostęp tylko z sieci AP", "Access only from the AP network"));
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->currentSession(req) == nullptr) {
            return sendError(req, "401 Unauthorized", tr("Sesja wygasła – zaloguj się ponownie", "Session expired – log in again"));
        }
    }

    cJSON* json = readJsonBody(req);
    const cJSON* oldPass = cJSON_GetObjectItemCaseSensitive(json, "old");
    const cJSON* newPass = cJSON_GetObjectItemCaseSensitive(json, "new");
    esp_err_t result;
    if (!cJSON_IsString(oldPass) || !cJSON_IsString(newPass)) {
        result = sendError(req, "400 Bad Request", tr("Brak pól old/new", "Missing old/new fields"));
    } else if (!self->checkPassword(oldPass->valuestring)) {
        result = sendError(req, "403 Forbidden", tr("Nieprawidłowe obecne hasło", "Wrong current password"));
    } else if (strlen(newPass->valuestring) < PASS_MIN || strlen(newPass->valuestring) > PASS_MAX) {
        result = sendError(req, "400 Bad Request", tr("Nowe hasło musi mieć 8-64 znaki", "New password must have 8-64 characters"));
    } else if (!self->setPassword(newPass->valuestring)) {
        result = sendError(req, "500 Internal Server Error", tr("Nie udało się zapisać hasła", "Could not save the password"));
    } else {
        // Other sessions are logged out; the current one stays.
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            self->logoutAll(self->currentSession(req));
        }
        LOG_I("Panel WWW: haslo zmienione");
        cJSON* body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "ok", true);
        result = sendJson(req, "200 OK", body);
    }
    cJSON_Delete(json);
    return result;
}

// ---- panel API ----

bool WebPanel::guard(httpd_req_t* req)
{
    if (!allowed(req)) {
        sendError(req, "403 Forbidden", tr("Dostęp tylko z sieci AP", "Access only from the AP network"));
        return false;
    }
    if (req->method == HTTP_POST) {
        // CSRF protection: the browser does not add a custom header to a request from a foreign page.
        char hdr[24];
        if (httpd_req_get_hdr_value_str(req, "X-Requested-With", hdr, sizeof(hdr)) != ESP_OK ||
            strcmp(hdr, "esp32router") != 0) {
            sendError(req, "403 Forbidden", tr("Brak nagłówka X-Requested-With", "Missing X-Requested-With header"));
            return false;
        }
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (currentSession(req) == nullptr) {
        sendError(req, "401 Unauthorized", tr("Sesja wygasła – zaloguj się ponownie", "Session expired – log in again"));
        return false;
    }
    return true;
}

// esp_timer callback: restarts the device.
static void rebootTimer(void*)
{
    esp_restart();
}

// Restart after the response is sent.
void WebPanel::scheduleReboot(int delayMs)
{
    static esp_timer_handle_t timer = nullptr;
    if (timer == nullptr) {
        esp_timer_create_args_t args = {};
        args.callback = &rebootTimer;
        args.name = "reboot";
        esp_timer_create(&args, &timer);
    }
    esp_timer_start_once(timer, static_cast<uint64_t>(delayMs) * 1000);
}

// Module name from the address /api/config/<module>.
static bool moduleFromUri(httpd_req_t* req, char* name, size_t size)
{
    const char* prefix = "/api/config/";
    if (strncmp(req->uri, prefix, strlen(prefix)) != 0) {
        return false;
    }
    strlcpy(name, req->uri + strlen(prefix), size);
    char* q = strchr(name, '?');
    if (q != nullptr) {
        *q = '\0';
    }
    return name[0] != '\0';
}

esp_err_t WebPanel::handleStatus(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* body = self->system.statusJson();
    cJSON_AddItemToObject(body, "services", self->services.listJson());
    cJSON* diff = self->store.diffJson();
    cJSON_AddNumberToObject(body, "unsaved", cJSON_GetObjectItem(diff, "count")->valueint);
    cJSON_Delete(diff);
    cJSON_AddBoolToObject(body, "default_password", self->usingDefaultPassword());
    cJSON_AddItemToObject(body, "ddns", self->ddns.statusJson());
    cJSON_AddItemToObject(body, "firewall", self->firewall.summaryJson());
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleConfigGet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    char name[24];
    if (!moduleFromUri(req, name, sizeof(name))) {
        return sendError(req, "404 Not Found", tr("Brak modułu", "Module missing"));
    }
    cJSON* body = strcmp(name, "diff") == 0 ? self->store.diffJson() : self->store.moduleJson(name);
    if (body == nullptr) {
        return sendError(req, "404 Not Found", tr("Nieznany moduł", "Unknown module"));
    }
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleConfigPost(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    char name[24];
    cJSON* fields = readJsonBody(req, 2048);
    cJSON* body = (fields && moduleFromUri(req, name, sizeof(name))) ? self->store.setFieldsJson(name, fields) : nullptr;
    cJSON_Delete(fields);
    if (body == nullptr) {
        return sendError(req, "400 Bad Request", tr("Nieznany moduł albo złe dane", "Unknown module or bad data"));
    }
    return sendJson(req, "200 OK", body);
}

// {"module": "dhcp"} or {} = all modules.
static const char* moduleArg(const cJSON* json)
{
    const cJSON* m = cJSON_GetObjectItemCaseSensitive(json, "module");
    return cJSON_IsString(m) && m->valuestring[0] ? m->valuestring : nullptr;
}

esp_err_t WebPanel::handleSave(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    cJSON* body = self->store.saveJson(moduleArg(json));
    cJSON_Delete(json);
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleRevert(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    cJSON* body = self->store.revertJson(moduleArg(json));
    cJSON_Delete(json);
    return sendJson(req, "200 OK", body);
}

// Any console command (actions and terminal): {"cmd": "..."} -> {"ok", "output"}.
esp_err_t WebPanel::handleCmd(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    const cJSON* cmd = cJSON_GetObjectItemCaseSensitive(json, "cmd");
    if (!cJSON_IsString(cmd) || strlen(cmd->valuestring) >= 200) {
        cJSON_Delete(json);
        return sendError(req, "400 Bad Request", tr("Brak pola cmd", "Missing cmd field"));
    }
    if (strncmp(cmd->valuestring, "reboot", 6) == 0) {
        cJSON_Delete(json);
        return sendError(req, "400 Bad Request", tr("Użyj /api/reboot", "Use /api/reboot"));
    }
    char line[200];
    strlcpy(line, cmd->valuestring, sizeof(line));
    cJSON_Delete(json);

    StringOutput out;
    self->currentCmdOutput = &out;
    CmdResult r = self->executor.execute(line, out);
    self->currentCmdOutput = nullptr;

    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", r == CmdResult::OK);
    cJSON_AddStringToObject(body, "output", out.text.c_str());
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleServices(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "services", self->services.listJson());
    return sendJson(req, "200 OK", body);
}

esp_err_t WebPanel::handleScan(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    std::vector<WifiManager::ScanResult> list;
    if (!self->wifi.scan(list)) {
        return sendError(req, "503 Service Unavailable", tr("Skanowanie nie powiodło się (albo już trwa)", "Scan failed (or already running)"));
    }
    cJSON* body = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(body, "networks");
    for (const WifiManager::ScanResult& n : list) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", n.ssid.c_str());
        cJSON_AddNumberToObject(o, "rssi", n.rssi);
        cJSON_AddNumberToObject(o, "channel", n.channel);
        cJSON_AddStringToObject(o, "auth", n.auth);
        cJSON_AddBoolToObject(o, "open", n.open);
        cJSON_AddItemToArray(arr, o);
    }
    return sendJson(req, "200 OK", body);
}

// {"ssid", "pass"}; no pass field = password unchanged (STA: when the SSID is the same).
esp_err_t WebPanel::handleWifiSet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    const cJSON* ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON* pass = cJSON_GetObjectItemCaseSensitive(json, "pass");
    if (!cJSON_IsString(ssid) || (pass != nullptr && !cJSON_IsString(pass))) {
        cJSON_Delete(json);
        return sendError(req, "400 Bad Request", tr("Brak pola ssid", "Missing ssid field"));
    }
    StringOutput out(512);
    bool sta = strcmp(req->uri, "/api/wifi/sta") == 0;
    const char* p = pass ? pass->valuestring : nullptr;
    CmdResult r = sta ? self->wifi.setSta(ssid->valuestring, p, out) : self->wifi.setAp(ssid->valuestring, p, out);
    cJSON_Delete(json);
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", r == CmdResult::OK);
    cJSON_AddStringToObject(body, "output", out.text.c_str());
    return sendJson(req, r == CmdResult::OK ? "200 OK" : "400 Bad Request", body);
}

esp_err_t WebPanel::handleOtaInfo(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    return sendJson(req, "200 OK", self->system.otaJson());
}

// Firmware upload: raw .bin file in the request body, streamed to the other partition,
// verification (header, project name, checksum in esp_ota_end), switch and restart.
esp_err_t WebPanel::handleOtaUpload(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (part == nullptr || req->content_len == 0 || req->content_len > part->size) {
        return sendError(req, "400 Bad Request", tr("Brak pliku albo plik większy niż partycja (3 MB)", "No file or file larger than the partition (3 MB)"));
    }

    esp_ota_handle_t handle = 0;
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK) {
        return sendError(req, "500 Internal Server Error", tr("Nie udało się rozpocząć aktualizacji", "Could not start the update"));
    }
    LOG_I("OTA: zapis %u B do %s", (unsigned)req->content_len, part->label);

    const size_t chunk = 4096;
    char* buf = static_cast<char*>(malloc(chunk));
    size_t received = 0;
    const char* error = buf ? nullptr : tr("Brak pamięci", "Out of memory");
    while (error == nullptr && received < req->content_len) {
        int n = httpd_req_recv(req, buf, std::min(chunk, req->content_len - received));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            error = tr("Przerwane przesyłanie", "Upload interrupted");
            break;
        }
        if (received == 0) {
            // First block: ESP image header and the application description with the project name.
            size_t descOff = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const auto* desc = reinterpret_cast<const esp_app_desc_t*>(buf + descOff);
            if (static_cast<uint8_t>(buf[0]) != ESP_IMAGE_HEADER_MAGIC || static_cast<size_t>(n) < descOff + sizeof(*desc)) {
                error = tr("To nie jest plik firmware ESP (.bin)", "This is not an ESP firmware file (.bin)");
                break;
            }
            if (strncmp(desc->project_name, esp_app_get_description()->project_name, sizeof(desc->project_name)) != 0) {
                error = tr("Firmware innego projektu niż Esp32Router", "Firmware of a project other than Esp32Router");
                break;
            }
            LOG_I("OTA: wersja %s", desc->version);
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            error = tr("Błąd zapisu do flash", "Flash write error");
            break;
        }
        received += n;
    }
    free(buf);

    if (error != nullptr) {
        esp_ota_abort(handle);
        LOG_E("OTA przerwane: %s", error);
        return sendError(req, "400 Bad Request", error);
    }
    esp_err_t err = esp_ota_end(handle);
    if (err != ESP_OK) {
        LOG_E("OTA: obraz odrzucony: %s", esp_err_to_name(err));
        return sendError(req, "400 Bad Request",
                         err == ESP_ERR_OTA_VALIDATE_FAILED ? tr("Obraz uszkodzony albo dla innego układu", "Image corrupted or for another chip") : tr("Błąd końca zapisu", "Error finishing the write"));
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return sendError(req, "500 Internal Server Error", tr("Nie udało się ustawić partycji startowej", "Could not set the boot partition"));
    }

    esp_app_desc_t desc = {};
    esp_ota_get_partition_description(part, &desc);
    LOG_W("OTA: firmware %s zapisany w %s - restart", desc.version, part->label);
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    cJSON_AddStringToObject(body, "version", desc.version);
    cJSON_AddStringToObject(body, "partition", part->label);
    esp_err_t res = sendJson(req, "200 OK", body);
    self->scheduleReboot(1500);
    return res;
}

esp_err_t WebPanel::handleBackup(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* backup = self->store.backupJson();
    if (backup == nullptr) {
        return sendError(req, "500 Internal Server Error", tr("Brak systemu plików", "No file system"));
    }
    cJSON_AddStringToObject(backup, "hostname", self->global.values().hostname.c_str());
    char* text = cJSON_Print(backup);
    cJSON_Delete(backup);
    if (text == nullptr) {
        return sendError(req, "500 Internal Server Error", tr("Brak pamięci", "Out of memory"));
    }
    char disposition[96];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s-config-%s.json\"",
             self->global.values().hostname.c_str(), esp_app_get_description()->version);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, text, strlen(text));
    cJSON_free(text);
    return err;
}

// Backup restore: write the files and restart (the configuration is loaded again).
esp_err_t WebPanel::handleRestore(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* backup = readJsonBody(req, 1536 * 1024);
    if (backup == nullptr) {
        return sendError(req, "400 Bad Request", tr("Plik nie jest poprawnym JSON albo jest za duży", "File is not valid JSON or is too large"));
    }
    std::string error;
    int count = self->store.restoreJson(backup, error);
    cJSON_Delete(backup);
    if (count < 0) {
        return sendError(req, "400 Bad Request", error.c_str());
    }
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    cJSON_AddNumberToObject(body, "files", count);
    esp_err_t res = sendJson(req, "200 OK", body);
    self->scheduleReboot(1500);
    return res;
}

// Reads for the panel pages – module data as JSON.
esp_err_t WebPanel::handleJsonGet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    const char* uri = req->uri;
    cJSON* body = nullptr;
    if (strncmp(uri, "/api/clients", 12) == 0) {
        body = self->system.clientsJson();
    } else if (strncmp(uri, "/api/dhcp/leases", 16) == 0) {
        body = self->dhcp.leasesJson();
    } else if (strncmp(uri, "/api/dhcp/reservations", 22) == 0) {
        body = self->dhcp.reservationsJson();
    } else if (strncmp(uri, "/api/pf", 7) == 0) {
        body = self->portfwd.rulesJson();
    } else if (strncmp(uri, "/api/nat", 8) == 0) {
        body = self->nat.statusJson();
    } else if (strncmp(uri, "/api/sys/mem", 12) == 0) {
        body = self->system.memJson();
    } else if (strncmp(uri, "/api/ddns", 9) == 0) {
        body = self->ddns.statusJson();
    } else if (strncmp(uri, "/api/firewall", 13) == 0) {
        body = self->firewall.statusJson();
    }
    if (body == nullptr) {
        return sendError(req, "404 Not Found", tr("Nie ma takich danych", "No such data"));
    }
    return sendJson(req, "200 OK", body);
}

// Add or change a port forwarding rule (JSON – names and targets without building a command).
esp_err_t WebPanel::handlePfSet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    if (json == nullptr) {
        return sendError(req, "400 Bad Request", tr("Złe dane", "Bad data"));
    }
    StringOutput out(512);
    CmdResult r = self->portfwd.setRuleJson(json, out);
    cJSON_Delete(json);
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", r == CmdResult::OK);
    cJSON_AddStringToObject(body, "output", out.text.c_str());
    return sendJson(req, r == CmdResult::OK ? "200 OK" : "400 Bad Request", body);
}

// Add or change a firewall rule (JSON, like /api/pf).
esp_err_t WebPanel::handleFwSet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    cJSON* json = readJsonBody(req);
    if (json == nullptr) {
        return sendError(req, "400 Bad Request", tr("Złe dane", "Bad data"));
    }
    StringOutput out(512);
    CmdResult r = self->firewall.setRuleJson(json, out);
    cJSON_Delete(json);
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", r == CmdResult::OK);
    cJSON_AddStringToObject(body, "output", out.text.c_str());
    return sendJson(req, r == CmdResult::OK ? "200 OK" : "400 Bad Request", body);
}

// Query parameter from the address (?q=...&offset=...).
static bool queryParam(httpd_req_t* req, const char* key, char* out, size_t size)
{
    char query[160];
    out[0] = '\0';
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, out, size) == ESP_OK;
}

esp_err_t WebPanel::handleDnsGet(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    const char* path = req->uri + strlen("/api/dns/");
    auto is = [path](const char* name) {
        size_t n = strlen(name);
        return strncmp(path, name, n) == 0 && (path[n] == '\0' || path[n] == '?');
    };
    if (is("names")) {
        return sendJson(req, "200 OK", self->dns.namesJson());
    }
    if (is("stats")) {
        return sendJson(req, "200 OK", self->dns.statsJson());
    }
    if (is("block")) {
        char q[64], off[12], lim[12];
        queryParam(req, "q", q, sizeof(q));
        int offset = queryParam(req, "offset", off, sizeof(off)) ? atoi(off) : 0;
        int limit = queryParam(req, "limit", lim, sizeof(lim)) ? atoi(lim) : 50;
        // The query from the browser is encoded (%2E etc.) – domain names are decoded simply.
        std::string filter;
        for (size_t i = 0; q[i]; ++i) {
            if (q[i] == '%' && isxdigit((unsigned char)q[i + 1]) && isxdigit((unsigned char)q[i + 2])) {
                char hex[3] = { q[i + 1], q[i + 2], 0 };
                filter += static_cast<char>(strtol(hex, nullptr, 16));
                i += 2;
            } else {
                filter += static_cast<char>(tolower((unsigned char)q[i]));
            }
        }
        return sendJson(req, "200 OK", self->dns.blockListJson(filter.c_str(), std::max(0, offset), std::min(std::max(limit, 1), 200)));
    }
    if (is("block/export")) {
        std::string text = self->dns.blockListText();
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"dns_block.txt\"");
        return httpd_resp_send(req, text.data(), text.size());
    }
    return sendError(req, "404 Not Found", tr("Nie ma takich danych", "No such data"));
}

// Block list import: file content (hosts or one domain per line) read in pieces; ?mode=replace replaces the list.
esp_err_t WebPanel::handleBlockImport(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > 8 * 1024 * 1024) {
        return sendError(req, "400 Bad Request", tr("Brak pliku albo plik większy niż 8 MB", "No file or file larger than 8 MB"));
    }
    char mode[12];
    bool replace = queryParam(req, "mode", mode, sizeof(mode)) && strcmp(mode, "replace") == 0;
    self->dns.importBegin(replace);
    char* buf = static_cast<char*>(malloc(2048));
    size_t got = 0;
    while (buf != nullptr && got < req->content_len) {
        int n = httpd_req_recv(req, buf, std::min<size_t>(2048, req->content_len - got));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        self->dns.importChunk(buf, n);
        got += n;
    }
    free(buf);
    int added = 0;
    int total = self->dns.importEnd(&added);
    std::string text = self->dns.blockListText();
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", got == req->content_len);
    cJSON_AddNumberToObject(body, "domains", total);
    cJSON_AddNumberToObject(body, "added", added);
    cJSON_AddNumberToObject(body, "file_size", text.size());
    cJSON_AddBoolToObject(body, "too_big", text.size() > 900 * 1024);
    return sendJson(req, "200 OK", body);
}

// WebSocket /ws/log: after connecting the client gets the last 4 KB of logs, then new lines as they come.
esp_err_t WebPanel::handleLogWs(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (req->method == HTTP_GET) {
        // The connection is established – check access and session (cookie from the opening request).
        bool ok = self->allowed(req);
        if (ok) {
            std::lock_guard<std::mutex> lock(self->mutex);
            ok = self->currentSession(req) != nullptr;
        }
        if (!ok) {
            return ESP_FAIL; // the server closes the connection
        }
        int fd = httpd_req_to_sockfd(req);
        uint64_t head = LogBuffer::head();
        bool added = false;
        {
            std::lock_guard<std::mutex> lock(self->logMutex);
            for (LogClient& c : self->logClients) {
                if (c.fd == 0 || c.fd == fd) {
                    c = { fd, head > 4096 ? head - 4096 : 0 };
                    added = true;
                    break;
                }
            }
        }
        if (!added) {
            return ESP_FAIL; // too many log viewers at once
        }
        if (self->logTask == nullptr) {
            self->logTaskStop = false;
            xTaskCreate(&logTaskEntry, "weblog", 4096, self, 4, &self->logTask);
        }
        return ESP_OK;
    }
    // Frames from the client (e.g. ping) – read and skipped.
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (frame.len > 0 && frame.len < 512) {
        uint8_t buf[512];
        frame.payload = buf;
        httpd_ws_recv_frame(req, &frame, frame.len);
    }
    return ESP_OK;
}

void WebPanel::logTaskEntry(void* arg)
{
    auto* self = static_cast<WebPanel*>(arg);
    self->logPump();
    self->logTask = nullptr;
    vTaskDelete(nullptr);
}

// Every 250 ms new data from the log buffer to each client; closed connections are removed.
void WebPanel::logPump()
{
    char buf[1024];
    while (!logTaskStop) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int active = 0;
        std::lock_guard<std::mutex> lock(logMutex);
        for (LogClient& c : logClients) {
            if (c.fd == 0) {
                continue;
            }
            if (server == nullptr || httpd_ws_get_fd_info(server, c.fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                c.fd = 0;
                continue;
            }
            ++active;
            size_t n = LogBuffer::read(&c.pos, buf, sizeof(buf));
            if (n == 0) {
                continue;
            }
            httpd_ws_frame_t frame = {};
            frame.type = HTTPD_WS_TYPE_TEXT;
            frame.payload = reinterpret_cast<uint8_t*>(buf);
            frame.len = n;
            if (httpd_ws_send_frame_async(server, c.fd, &frame) != ESP_OK) {
                c.fd = 0;
            }
        }
        if (active == 0) {
            break; // nobody is watching – the task ends, it comes back with the next connection
        }
    }
}

esp_err_t WebPanel::handleReboot(httpd_req_t* req)
{
    auto* self = static_cast<WebPanel*>(req->user_ctx);
    if (!self->guard(req)) {
        return ESP_OK;
    }
    LOG_W("Restart z panelu WWW");
    cJSON* body = cJSON_CreateObject();
    cJSON_AddBoolToObject(body, "ok", true);
    esp_err_t res = sendJson(req, "200 OK", body);
    self->scheduleReboot(1000);
    return res;
}

// ---- commands ----

void WebPanel::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("web", { "panel WWW", "web panel" });
    exec.add("web", { "passwd", "<nowe>|reset", { "zmiana hasła panelu; reset = hasło z Secrets.h", "changes the panel password; reset = password from Secrets.h" }, &cmdPasswd, this });
    exec.add("web", { "sessions", "", { "zalogowane sesje", "logged-in sessions" }, &cmdSessions, this });
    exec.add("web", { "logout-all", "", { "wylogowuje wszystkie sesje", "logs out all sessions" }, &cmdLogoutAll, this });
}

CmdResult WebPanel::cmdPasswd(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<WebPanel*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    bool reset = strcmp(argv[1], "reset") == 0;
    size_t len = strlen(argv[1]);
    if (!reset && (len < PASS_MIN || len > PASS_MAX)) {
        out.print(tr("Hasło musi mieć 8-64 znaki\r\n", "The password must have 8-64 characters\r\n"));
        return CmdResult::ERR_FAIL;
    }
    if (!self->setPassword(reset ? nullptr : argv[1])) {
        out.print(tr("Nie udało się zapisać hasła\r\n", "Could not save the password\r\n"));
        return CmdResult::ERR_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        self->logoutAll(nullptr);
    }
    out.print(reset ? tr("Przywrócono hasło z Secrets.h; sesje wylogowane\r\n", "Password from Secrets.h restored; sessions logged out\r\n")
                    : tr("Hasło zmienione (zapisane od razu); sesje wylogowane\r\n", "Password changed (saved immediately); sessions logged out\r\n"));
    return CmdResult::OK;
}

CmdResult WebPanel::cmdSessions(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<WebPanel*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    int64_t now = esp_timer_get_time();
    int count = 0;
    std::lock_guard<std::mutex> lock(self->mutex);
    for (const Session& s : self->sessions) {
        int64_t idle = now - s.lastUsedUs;
        if (!s.used || idle > static_cast<int64_t>(self->active.sessionTimeoutS) * 1000000) {
            continue;
        }
        esp_ip4_addr_t a = { s.ip };
        out.printf(tr("  " IPSTR "  zalogowany %lu s temu, bezczynny %lu s\r\n", "  " IPSTR "  logged in %lu s ago, idle %lu s\r\n"),
                   IP2STR(&a),
                   (unsigned long)((now - s.createdUs) / 1000000), (unsigned long)(idle / 1000000));
        ++count;
    }
    if (count == 0) {
        out.print(tr("Brak sesji\r\n", "No sessions\r\n"));
    }
    out.printf(tr("Hasło: %s\r\n", "Password: %s\r\n"), self->hasHash ? tr("ustawione (web_auth.json)", "set (web_auth.json)") : tr("domyślne z Secrets.h", "default from Secrets.h"));
    return CmdResult::OK;
}

CmdResult WebPanel::cmdLogoutAll(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<WebPanel*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    self->logoutAll(nullptr);
    out.print(tr("Wszystkie sesje wylogowane\r\n", "All sessions logged out\r\n"));
    return CmdResult::OK;
}
