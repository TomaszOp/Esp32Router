/**
 * @file DdnsClient.cpp
 * @brief DdnsClient: public IP check and DDNS account updates over HTTPS.
 */
#include "DdnsClient.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <algorithm>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_netif.h"
#include "esp_timer.h"

#define LOG_TAG     "DdnsClient"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::ddns; // factory settings of ddns.json

static constexpr int64_t SECOND_US = 1000000;
static constexpr int FALLBACK_AFTER_FAILURES = 3;   // failed IP checks -> "provider" mode
static constexpr int64_t FALLBACK_PERIOD_US = 3600 * SECOND_US;
static constexpr uint32_t MAX_BACKOFF_S = 1800;
static const char* const SLOT_KEYS[] = { "1", "2", "3" };

// ---- helpers ----

// ip_source: "public" / "sta" / "provider".
static bool validSource(const std::string& s)
{
    return s == "public" || s == "sta" || s == "provider";
}

// One of the supported providers.
static bool validProvider(const std::string& s)
{
    return s == "duckdns" || s == "dynu" || s == "noip" || s == "freedns" || s == "desec" || s == "custom";
}

// http:// or https:// URL.
static bool validUrl(const std::string& s)
{
    return s.rfind("https://", 0) == 0 || s.rfind("http://", 0) == 0;
}

// Empty or a URL.
static bool validUrlOrEmpty(const std::string& s)
{
    return s.empty() || validUrl(s);
}

// Host names separated by commas: letters, digits, '-', '.'.
static bool validHosts(const std::string& s)
{
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
              c == ',')) {
            return false;
        }
    }
    return true;
}

// Text is an IPv4 address.
static bool validIpv4(const std::string& s)
{
    esp_ip4_addr_t ip = {};
    return !s.empty() && s.size() <= 15 && esp_netif_str_to_ip4(s.c_str(), &ip) == ESP_OK;
}

// Percent-encoding for URL parameters.
static std::string urlEncode(const std::string& s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == ',') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// Replaces all occurrences of a placeholder (URL template).
static void replaceAll(std::string& s, const char* from, const std::string& to)
{
    size_t n = strlen(from);
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size())) {
        s.replace(p, n, to);
    }
}

// First line of a response (without CR/LF).
static std::string firstLine(const std::string& s)
{
    std::string line = s.substr(0, s.find_first_of("\r\n"));
    return line.size() > 80 ? line.substr(0, 80) : line;
}

// Text without leading and trailing whitespace.
static std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// Time since a moment as "N s/min/h ago" in the current language ("never" for 0).
static std::string ago(int64_t us, int64_t now)
{
    if (us == 0) {
        return tr("nigdy", "never");
    }
    long s = static_cast<long>((now - us) / SECOND_US);
    char buf[24];
    if (s < 120) {
        snprintf(buf, sizeof(buf), tr("%ld s temu", "%ld s ago"), s);
    } else if (s < 7200) {
        snprintf(buf, sizeof(buf), tr("%ld min temu", "%ld min ago"), s / 60);
    } else {
        snprintf(buf, sizeof(buf), tr("%ld h temu", "%ld h ago"), s / 3600);
    }
    return buf;
}

// ---- configuration ----

DdnsClient::DdnsClient(WifiManager& wifi, const TimeService& time)
    : wifi(wifi),
      time(time)
{
}

void DdnsClient::loadConfig(ConfigReader& r)
{
    std::lock_guard<std::mutex> lock(mutex);
    cfg.ipSource = r.str("ip_source", dflt::IP_SOURCE, 8, &validSource, { "public/sta/provider", "public/sta/provider" });
    cfg.checkUrl = r.str("check_url", dflt::CHECK_URL, 128, &validUrl, { "adres http(s)://, odpowiedź = sam adres IP", "http(s):// URL, response = bare IP address" });
    cfg.checkIntervalS = r.integer("check_interval_s", dflt::CHECK_INTERVAL_S, dflt::CHECK_INTERVAL_S_MIN, dflt::CHECK_INTERVAL_S_MAX);
    cfg.forceUpdateH = r.integer("force_update_h", dflt::FORCE_UPDATE_H, dflt::FORCE_UPDATE_H_MIN, dflt::FORCE_UPDATE_H_MAX);
    cfg.retryS = r.integer("retry_s", dflt::RETRY_S, dflt::RETRY_S_MIN, dflt::RETRY_S_MAX);

    for (int i = 0; i < ACCOUNTS; ++i) {
        ConfigReader a = r.object(SLOT_KEYS[i]);
        Account& acc = cfg.accounts[i];
        acc.enabled = a.boolean("enabled", dflt::ENABLED);
        acc.provider = a.str("provider", dflt::PROVIDER, 8, &validProvider, { "duckdns/dynu/noip/freedns/desec/custom", "duckdns/dynu/noip/freedns/desec/custom" });
        acc.hostname = a.str("hostname", dflt::HOSTNAME, 128, &validHosts, { "nazwa hosta, kilka po przecinku", "host name, several separated by commas" });
        acc.username = a.str("username", dflt::USERNAME, 64, nullptr, { "login (No-IP, Dynu, custom)", "login (No-IP, Dynu, custom)" });
        acc.secret = a.str("secret", dflt::SECRET, 128, nullptr, { "token albo hasło", "token or password" });
        acc.customUrl = a.str("custom_url", dflt::CUSTOM_URL, 256, &validUrlOrEmpty, { "http(s)://... z {host} {ip} {user} {pass}", "http(s)://... with {host} {ip} {user} {pass}" });
        a.finish();
    }
}

cJSON* DdnsClient::saveConfig() const
{
    std::lock_guard<std::mutex> lock(mutex);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip_source", cfg.ipSource.c_str());
    cJSON_AddStringToObject(root, "check_url", cfg.checkUrl.c_str());
    cJSON_AddNumberToObject(root, "check_interval_s", cfg.checkIntervalS);
    cJSON_AddNumberToObject(root, "force_update_h", cfg.forceUpdateH);
    cJSON_AddNumberToObject(root, "retry_s", cfg.retryS);
    for (int i = 0; i < ACCOUNTS; ++i) {
        const Account& acc = cfg.accounts[i];
        cJSON* a = cJSON_AddObjectToObject(root, SLOT_KEYS[i]);
        cJSON_AddBoolToObject(a, "enabled", acc.enabled);
        cJSON_AddStringToObject(a, "provider", acc.provider.c_str());
        cJSON_AddStringToObject(a, "hostname", acc.hostname.c_str());
        cJSON_AddStringToObject(a, "username", acc.username.c_str());
        cJSON_AddStringToObject(a, "secret", acc.secret.c_str());
        cJSON_AddStringToObject(a, "custom_url", acc.customUrl.c_str());
    }
    return root;
}

// Everything applies immediately: a changed account resets its state (also a permanent error) and wakes the task.
bool DdnsClient::applyLive(const std::string& path)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!path.empty() && path[0] >= '1' && path[0] <= '3') {
            state[path[0] - '1'] = {};
        } else if (path == "ip_source" || path == "check_url") {
            publicIp.clear();
            checkFailures = 0;
            checkNow = true;
        } else if (path == "check_interval_s") {
            nextCheckUs = 0;
        }
    }
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    return true;
}

// Incomplete account (e.g. no token) – not updated; nullptr = OK.
const char* DdnsClient::accountProblem(const Account& a)
{
    if (a.provider == "custom") {
        return a.customUrl.empty() ? tr("brak custom_url", "custom_url missing") : nullptr;
    }
    if (a.secret.empty()) {
        return a.provider == "freedns" ? tr("brak tokenu", "token missing") : tr("brak tokenu/hasła", "token/password missing");
    }
    if (a.provider != "freedns" && a.hostname.empty()) {
        return tr("brak hostname", "hostname missing");
    }
    if ((a.provider == "noip" || a.provider == "dynu") && a.username.empty()) {
        return tr("brak username", "username missing");
    }
    return nullptr;
}

// ---- service ----

esp_err_t DdnsClient::start()
{
    if (running) {
        return ESP_OK;
    }
    if (!eventRegistered) {
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onGotIp, this, nullptr);
        eventRegistered = true;
    }
    stopRequested = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        checkNow = true;
    }
    // Stack for TLS (mbedTLS) – buffers in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC).
    if (xTaskCreate(&taskEntry, "ddns", 8192, this, 4, &task) != pdPASS) {
        task = nullptr;
        return ESP_ERR_NO_MEM;
    }
    running = true;
    LOG_I("DDNS uruchomiony");
    return ESP_OK;
}

esp_err_t DdnsClient::stop()
{
    if (!running) {
        return ESP_OK;
    }
    stopRequested = true;
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    // An HTTPS request in progress may take up to the timeout (10 s).
    for (int i = 0; i < 600 && task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (task != nullptr) {
        return ESP_ERR_TIMEOUT;
    }
    running = false;
    LOG_I("DDNS zatrzymany");
    return ESP_OK;
}

void DdnsClient::describe(IOutput& out) const
{
    if (!running) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    int active = 0;
    for (const Account& a : cfg.accounts) {
        active += a.enabled && accountProblem(a) == nullptr;
    }
    out.printf(tr(" IP %s, konta %d", " IP %s, accounts %d"), publicIp.empty() ? "-" : publicIp.c_str(), active);
}

void DdnsClient::onGotIp(void* arg, esp_event_base_t, int32_t, void*)
{
    auto* self = static_cast<DdnsClient*>(arg);
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        self->checkNow = true; // new STA address – the public IP may have changed
    }
    if (self->task != nullptr) {
        xTaskNotifyGive(self->task);
    }
}

void DdnsClient::taskEntry(void* arg)
{
    auto* self = static_cast<DdnsClient*>(arg);
    self->run();
    self->task = nullptr;
    vTaskDelete(nullptr);
}

void DdnsClient::run()
{
    while (!stopRequested) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (stopRequested) {
            break;
        }
        // Without an IP on the STA there is no internet, and without time (NTP) TLS certificates cannot be checked.
        if (!wifi.staConnected() || !time.synced()) {
            std::lock_guard<std::mutex> lock(mutex);
            ++cycles;
            continue;
        }

        Settings s;
        bool doCheck;
        int64_t now = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(mutex);
            s = cfg;
            doCheck = s.ipSource == "public" && (checkNow || now >= nextCheckUs);
        }
        if (doCheck) {
            checkPublicIp(s);
        }

        std::string ip;
        bool providerMode = s.ipSource == "provider";
        bool fallback = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (s.ipSource == "public") {
                ip = publicIp;
                fallback = checkFailures >= FALLBACK_AFTER_FAILURES;
            }
        }
        if (s.ipSource == "sta") {
            esp_netif_ip_info_t info = {};
            esp_netif_get_ip_info(wifi.staNetif(), &info);
            char buf[16];
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip));
            ip = buf;
        }

        now = esp_timer_get_time();
        for (int i = 0; i < ACCOUNTS && !stopRequested; ++i) {
            const Account& a = s.accounts[i];
            if (!a.enabled || accountProblem(a) != nullptr) {
                continue;
            }
            State st;
            {
                std::lock_guard<std::mutex> lock(mutex);
                st = state[i];
            }
            bool need;
            if (st.forceNow) {
                need = true;
            } else if (st.fatal) {
                need = false;
            } else if (st.failures > 0) {
                need = now >= st.nextRetryUs;
            } else if (providerMode || fallback) {
                int64_t period = fallback ? FALLBACK_PERIOD_US : static_cast<int64_t>(s.checkIntervalS) * SECOND_US;
                need = st.lastAttemptUs == 0 || now - st.lastAttemptUs >= period;
            } else if (ip.empty()) {
                need = false;
            } else {
                need = ip != st.lastIp ||
                       (s.forceUpdateH > 0 && now - st.lastOkUs >= static_cast<int64_t>(s.forceUpdateH) * 3600 * SECOND_US);
            }
            if (need) {
                updateAccount(i, a, (providerMode || fallback) ? std::string() : ip);
            }
        }

        std::lock_guard<std::mutex> lock(mutex);
        ++cycles;
    }
}

// Public IP from check_url (the response is just the address).
void DdnsClient::checkPublicIp(const Settings& s)
{
    int status = 0;
    std::string body;
    bool ok = httpGet(s.checkUrl, nullptr, nullptr, &status, &body);
    std::string ip = trim(body);
    int64_t now = esp_timer_get_time();

    std::lock_guard<std::mutex> lock(mutex);
    checkNow = false;
    lastCheckUs = now;
    if (ok && status == 200 && validIpv4(ip)) {
        if (ip != publicIp) {
            LOG_I("Publiczny IP: %s%s%s", ip.c_str(), publicIp.empty() ? "" : " (byl ", publicIp.empty() ? "" : (publicIp + ")").c_str());
        }
        publicIp = ip;
        checkFailures = 0;
        checkError.clear();
        nextCheckUs = now + static_cast<int64_t>(s.checkIntervalS) * SECOND_US;
        return;
    }
    ++checkFailures;
    checkError = ok ? "HTTP " + std::to_string(status) + ": " + firstLine(body) : body;
    nextCheckUs = now + static_cast<int64_t>(std::min(s.retryS, s.checkIntervalS)) * SECOND_US;
    LOG_W("Sprawdzenie publicznego IP nie powiodlo sie (%d): %s%s", checkFailures, checkError.c_str(),
          checkFailures == FALLBACK_AFTER_FAILURES ? " - przechodze na wykrywanie przez serwis" : "");
}

DdnsClient::Outcome DdnsClient::classify(const std::string& provider, int status, const std::string& body)
{
    std::string b = trim(body);
    if (provider == "duckdns") {
        if (b.rfind("OK", 0) == 0) {
            return Outcome::OK;
        }
        return b.rfind("KO", 0) == 0 ? Outcome::FATAL : Outcome::TEMPORARY;
    }
    if (provider == "freedns") {
        if (b.find("Updated") != std::string::npos || b.find("No IP change") != std::string::npos ||
            b.find("has not changed") != std::string::npos) {
            return Outcome::OK;
        }
        return b.find("ERROR") != std::string::npos ? Outcome::FATAL : Outcome::TEMPORARY;
    }
    // dyndns2 protocol (No-IP, Dynu, deSEC, usually custom ones too).
    if (status == 401 || status == 403) {
        return Outcome::FATAL;
    }
    std::string word = b.substr(0, b.find_first_of(" \r\n"));
    if (word == "good" || word == "nochg") {
        return Outcome::OK;
    }
    if (word == "badauth" || word == "nohost" || word == "notfqdn" || word == "abuse" || word == "!donator" ||
        word == "badagent" || word == "numhost" || word == "!yours") {
        return Outcome::FATAL;
    }
    if (word == "911" || word == "dnserr" || status >= 500) {
        return Outcome::TEMPORARY;
    }
    // Custom service with another response: 2xx is treated as success.
    return (provider == "custom" && status >= 200 && status < 300) ? Outcome::OK : Outcome::TEMPORARY;
}

void DdnsClient::updateAccount(int i, const Account& a, const std::string& ip)
{
    std::string url;
    const char* user = nullptr;
    std::string first = a.hostname.substr(0, a.hostname.find(','));
    if (a.provider == "duckdns") {
        // DuckDNS wants subdomains only (without .duckdns.org).
        std::string domains = a.hostname;
        replaceAll(domains, ".duckdns.org", "");
        url = "https://www.duckdns.org/update?domains=" + urlEncode(domains) + "&token=" + urlEncode(a.secret) + "&ip=" + ip;
    } else if (a.provider == "noip") {
        url = "https://dynupdate.no-ip.com/nic/update?hostname=" + urlEncode(a.hostname) + (ip.empty() ? "" : "&myip=" + ip);
        user = a.username.c_str();
    } else if (a.provider == "dynu") {
        url = "https://api.dynu.com/nic/update?hostname=" + urlEncode(a.hostname) + (ip.empty() ? "" : "&myip=" + ip);
        user = a.username.c_str();
    } else if (a.provider == "freedns") {
        url = "https://sync.afraid.org/u/" + urlEncode(a.secret) + "/" + (ip.empty() ? "" : "?ip=" + ip);
    } else if (a.provider == "desec") {
        url = "https://update.dedyn.io/?hostname=" + urlEncode(a.hostname) + (ip.empty() ? "" : "&myipv4=" + ip);
        user = first.c_str();
    } else {
        url = a.customUrl;
        replaceAll(url, "{host}", urlEncode(a.hostname));
        replaceAll(url, "{ip}", ip);
        replaceAll(url, "{user}", urlEncode(a.username));
        replaceAll(url, "{pass}", urlEncode(a.secret));
        if (!a.username.empty()) {
            user = a.username.c_str();
        }
    }

    int status = 0;
    std::string body;
    bool connected = httpGet(url, user, user ? a.secret.c_str() : nullptr, &status, &body);
    Outcome o = connected ? classify(a.provider, status, body) : Outcome::TEMPORARY;
    std::string answer = connected ? firstLine(trim(body)) : body;
    if (answer.empty()) {
        answer = "HTTP " + std::to_string(status);
    }

    int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(mutex);
    State& st = state[i];
    st.lastAttemptUs = now;
    st.forceNow = false;
    const char* host = a.hostname.empty() ? a.provider.c_str() : a.hostname.c_str(); // no tokens in logs
    switch (o) {
    case Outcome::OK:
        st.lastOkUs = now;
        st.lastIp = ip.empty() ? publicIp : ip;
        st.failures = 0;
        st.backoffS = 0;
        st.fatal = false;
        st.result = "OK: " + answer;
        LOG_I("DDNS %d (%s %s): %s", i + 1, a.provider.c_str(), host, answer.c_str());
        break;
    case Outcome::TEMPORARY:
        ++st.failures;
        st.backoffS = st.backoffS == 0 ? cfg.retryS : std::min<uint32_t>(st.backoffS * 2, MAX_BACKOFF_S);
        st.nextRetryUs = now + static_cast<int64_t>(st.backoffS) * SECOND_US;
        st.result = tr("błąd chwilowy: ", "temporary error: ") + answer;
        LOG_W("DDNS %d (%s %s): blad chwilowy: %s - ponowienie za %lu s", i + 1, a.provider.c_str(), host, answer.c_str(),
              (unsigned long)st.backoffS);
        break;
    case Outcome::FATAL:
        st.fatal = true;
        st.result = tr("błąd trwały: ", "permanent error: ") + answer;
        LOG_E("DDNS %d (%s %s): blad trwaly: %s - wstrzymane do zmiany konta albo 'ddns update'", i + 1,
              a.provider.c_str(), host, answer.c_str());
        break;
    }
}

bool DdnsClient::httpGet(const std::string& url, const char* user, const char* pass, int* status, std::string* body)
{
    static std::string agent = std::string("Esp32Router/") + esp_app_get_description()->version +
                               " (+https://github.com/TomaszOp/Esp32Router)";
    esp_http_client_config_t c = {};
    c.url = url.c_str();
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 10000;
    c.user_agent = agent.c_str();
    c.disable_auto_redirect = false;
    if (user != nullptr) {
        c.username = user;
        c.password = pass;
        c.auth_type = HTTP_AUTH_TYPE_BASIC;
    }
    esp_http_client_handle_t client = esp_http_client_init(&c);
    if (client == nullptr) {
        *body = tr("brak pamięci na klienta HTTP", "out of memory for the HTTP client");
        return false;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        *body = std::string(tr("brak połączenia: ", "no connection: ")) + esp_err_to_name(err);
        esp_http_client_cleanup(client);
        return false;
    }
    esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    char buf[257];
    int n = esp_http_client_read(client, buf, sizeof(buf) - 1);
    body->assign(buf, n > 0 ? n : 0);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return true;
}

// ---- commands and panel ----

void DdnsClient::registerCommands(CommandExecutor& exec)
{
    exec.add(nullptr, { "ddns", "[update [1-3] | test]",
                        { "dynamiczny DNS: stan; update = aktualizacja teraz; test = sprawdzenie publicznego IP", "dynamic DNS: status; update = update now; test = check the public IP" }, &cmdDdns,
                        this });
}

// The command wakes the DDNS task and waits for the pass to finish (the task makes the HTTPS requests).
void DdnsClient::wakeAndWait(int timeoutMs)
{
    uint32_t start;
    {
        std::lock_guard<std::mutex> lock(mutex);
        start = cycles;
    }
    if (task != nullptr) {
        xTaskNotifyGive(task);
    }
    for (int waited = 0; waited < timeoutMs; waited += 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        std::lock_guard<std::mutex> lock(mutex);
        if (cycles >= start + 2) {
            return;
        }
    }
}

void DdnsClient::printStatus(IOutput& out) const
{
    int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(mutex);
    out.printf(tr("Usługa: %s, źródło adresu: %s\r\n", "Service: %s, address source: %s\r\n"), running ? tr("działa", "running") : tr("zatrzymana (service start ddns)", "stopped (service start ddns)"),
               cfg.ipSource.c_str());
    if (cfg.ipSource == "public") {
        out.printf(tr("Publiczny IP: %s (sprawdzono %s%s%s)\r\n", "Public IP: %s (checked %s%s%s)\r\n"), publicIp.empty() ? "-" : publicIp.c_str(),
                   ago(lastCheckUs, now).c_str(), checkError.empty() ? "" : tr(", błąd: ", ", error: "), checkError.c_str());
        if (checkFailures >= FALLBACK_AFTER_FAILURES) {
            out.print(tr("  sprawdzanie nie działa – aktualizacje w trybie provider (max raz na godzinę)\r\n", "  checking does not work – updates in provider mode (max once per hour)\r\n"));
        }
    }
    for (int i = 0; i < ACCOUNTS; ++i) {
        const Account& a = cfg.accounts[i];
        const State& st = state[i];
        const char* problem = accountProblem(a);
        out.printf("%d: ", i + 1);
        if (problem != nullptr && a.hostname.empty() && a.secret.empty() && a.customUrl.empty()) {
            out.print(tr("(puste)\r\n", "(empty)\r\n"));
            continue;
        }
        out.printf("%s %s%s - ", a.provider.c_str(), a.hostname.c_str(), a.enabled ? "" : tr(" (wyłączone)", " (disabled)"));
        if (problem != nullptr) {
            out.printf(tr("niekompletne: %s\r\n", "incomplete: %s\r\n"), problem);
        } else if (st.result.empty()) {
            out.print(tr("jeszcze nie aktualizowane\r\n", "not updated yet\r\n"));
        } else {
            out.printf(tr("%s, ostatnia udana: %s\r\n", "%s, last successful: %s\r\n"), st.result.c_str(), ago(st.lastOkUs, now).c_str());
        }
    }
}

CmdResult DdnsClient::cmdDdns(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DdnsClient*>(ctx);
    if (argc == 1) {
        self->printStatus(out);
        return CmdResult::OK;
    }
    bool update = strcmp(argv[1], "update") == 0;
    bool test = strcmp(argv[1], "test") == 0;
    if ((!update && !test) || (test && argc != 2) || argc > 3) {
        return CmdResult::ERR_ARGS;
    }
    if (!self->running) {
        out.print(tr("Usługa ddns zatrzymana – service start ddns\r\n", "The ddns service is stopped – service start ddns\r\n"));
        return CmdResult::ERR_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        if (test) {
            self->checkNow = true;
        } else {
            int only = argc == 3 ? atoi(argv[2]) : 0;
            if (argc == 3 && (only < 1 || only > ACCOUNTS)) {
                return CmdResult::ERR_ARGS;
            }
            for (int i = 0; i < ACCOUNTS; ++i) {
                if (only == 0 || only == i + 1) {
                    self->state[i].forceNow = true;
                    self->state[i].fatal = false;
                }
            }
            self->checkNow = true;
        }
    }
    out.print(tr("Proszę czekać...\r\n", "Please wait...\r\n"));
    self->wakeAndWait(25000);
    self->printStatus(out);
    return CmdResult::OK;
}

cJSON* DdnsClient::statusJson() const
{
    int64_t now = esp_timer_get_time();
    cJSON* root = cJSON_CreateObject();
    std::lock_guard<std::mutex> lock(mutex);
    cJSON_AddBoolToObject(root, "running", running);
    cJSON_AddStringToObject(root, "ip_source", cfg.ipSource.c_str());
    cJSON_AddStringToObject(root, "public_ip", publicIp.c_str());
    cJSON_AddStringToObject(root, "check_error", checkError.c_str());
    cJSON_AddBoolToObject(root, "fallback", cfg.ipSource == "public" && checkFailures >= FALLBACK_AFTER_FAILURES);
    cJSON_AddNumberToObject(root, "last_check_s", lastCheckUs ? static_cast<double>((now - lastCheckUs) / SECOND_US) : -1);
    cJSON_AddNumberToObject(root, "next_check_s",
                            nextCheckUs > now ? static_cast<double>((nextCheckUs - now) / SECOND_US) : 0);
    cJSON* list = cJSON_AddArrayToObject(root, "accounts");
    for (int i = 0; i < ACCOUNTS; ++i) {
        const Account& a = cfg.accounts[i];
        const State& st = state[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "slot", i + 1);
        cJSON_AddBoolToObject(o, "enabled", a.enabled);
        cJSON_AddStringToObject(o, "provider", a.provider.c_str());
        cJSON_AddStringToObject(o, "hostname", a.hostname.c_str());
        cJSON_AddBoolToObject(o, "secret_set", !a.secret.empty());
        const char* problem = accountProblem(a);
        cJSON_AddBoolToObject(o, "empty", a.hostname.empty() && a.secret.empty() && a.customUrl.empty());
        cJSON_AddStringToObject(o, "problem", problem ? problem : "");
        cJSON_AddStringToObject(o, "result", st.result.c_str());
        cJSON_AddBoolToObject(o, "fatal", st.fatal);
        cJSON_AddStringToObject(o, "last_ok", ago(st.lastOkUs, now).c_str());
        cJSON_AddStringToObject(o, "last_ip", st.lastIp.c_str());
        cJSON_AddItemToArray(list, o);
    }
    return root;
}
