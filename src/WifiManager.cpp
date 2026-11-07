/**
 * @file WifiManager.cpp
 * @brief WifiManager: APSTA mode, STA retries, AP, scan, "wifi" commands.
 */
#include "WifiManager.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <algorithm>
#include <cstring>

#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"

#include "PacketFilter.h"

#define LOG_TAG     "WifiManager"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::wifi; // factory settings of wifi.json

static constexpr size_t SSID_MAX = 32;
static constexpr size_t PASS_MAX = 63;

// SSID from Secrets.h + "-XXXX" (last 2 bytes of the AP interface MAC).
static std::string defaultApSsid()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "-%02X%02X", mac[4], mac[5]);
    std::string base = dflt::AP_SSID;
    return base.substr(0, SSID_MAX - strlen(suffix)) + suffix;
}

// Subnet mask: valid IPv4, contiguous, at most /30.
static bool validMask(const std::string& s)
{
    esp_ip4_addr_t m = {};
    if (esp_netif_str_to_ip4(s.c_str(), &m) != ESP_OK) {
        return false;
    }
    uint32_t host = ~lwip_ntohl(m.addr);
    return m.addr != 0 && (host & (host + 1)) == 0 && host >= 3; // contiguous mask, at least /30
}

// AP password: empty (open network) or 8-63 characters.
static bool validApPass(const std::string& s)
{
    return s.empty() || s.size() >= 8;
}

// Address in network byte order -> "a.b.c.d".
static void ipToStr(uint32_t addr, char* buf, size_t size)
{
    esp_ip4_addr_t ip = { addr };
    snprintf(buf, size, IPSTR, IP2STR(&ip));
}

WifiManager::WifiManager(const GlobalSettings& global)
    : global(global)
{
}

void WifiManager::loadConfig(ConfigReader& r)
{
    std::lock_guard<std::mutex> lock(mutex);

    ConfigReader s = r.object("sta");
    config.staSsid = s.str("ssid", dflt::STA_SSID, SSID_MAX, nullptr, { "tekst, max 32 (puste = nie łącz)", "text, max 32 (empty = do not connect)" });
    config.staPass = s.str("pass", dflt::STA_PASS, PASS_MAX, nullptr, { "tekst, max 63", "text, max 63" });
    s.finish();

    ConfigReader a = r.object("ap");
    config.apSsid = a.str("ssid", defaultApSsid(), SSID_MAX, [](const std::string& v) { return !v.empty(); },
                          { "1-32 znaki", "1-32 chars" });
    config.apPass = a.str("pass", dflt::AP_PASS, PASS_MAX, &validApPass, { "puste (otwarta) albo 8-63 znaki", "empty (open) or 8-63 chars" });
    config.apMaxClients = a.integer("max_clients", dflt::AP_MAX_CLIENTS, dflt::AP_MAX_CLIENTS_MIN, dflt::AP_MAX_CLIENTS_MAX);

    char def[16];
    ipToStr(dflt::AP_MASK, def, sizeof(def));
    std::string mask = a.str("mask", def, 15, &validMask, { "maska ciągła, max /30", "contiguous mask, max /30" });
    esp_ip4_addr_t m = {};
    esp_netif_str_to_ip4(mask.c_str(), &m);
    config.apMask = m.addr;
    config.apIp = a.ipv4("ip", dflt::AP_IP);
    a.finish();
}

cJSON* WifiManager::saveConfig() const
{
    std::lock_guard<std::mutex> lock(mutex);
    cJSON* root = cJSON_CreateObject();

    cJSON* s = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddStringToObject(s, "ssid", config.staSsid.c_str());
    cJSON_AddStringToObject(s, "pass", config.staPass.c_str());

    cJSON* a = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(a, "ssid", config.apSsid.c_str());
    cJSON_AddStringToObject(a, "pass", config.apPass.c_str());
    cJSON_AddNumberToObject(a, "max_clients", config.apMaxClients);
    char buf[16];
    ipToStr(config.apIp, buf, sizeof(buf));
    cJSON_AddStringToObject(a, "ip", buf);
    ipToStr(config.apMask, buf, sizeof(buf));
    cJSON_AddStringToObject(a, "mask", buf);
    return root;
}

WifiSettings WifiManager::settings() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return active;
}

WifiSettings WifiManager::configured() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return config;
}

void WifiManager::start()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        active = config;
    }
    // AP without the built-in ESP-IDF DHCP server – addresses are given by our DhcpServer.
    esp_netif_inherent_config_t apBase = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP();
    apBase.flags = static_cast<esp_netif_flags_t>(apBase.flags & ~ESP_NETIF_DHCP_SERVER);
    ap = esp_netif_create_wifi(WIFI_IF_AP, &apBase);
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_ap_handlers());
    sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, global.values().hostname.c_str());
    PacketFilter::setNetifs(ap, sta); // the lwIP hook recognizes the AP/STA side of a packet

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onEvent, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &onEvent, this, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    WifiSettings s = settings();
    esp_netif_ip_info_t ipInfo = {};
    ipInfo.ip.addr = s.apIp;
    ipInfo.netmask.addr = s.apMask;
    ipInfo.gw.addr = s.apIp;
    if (esp_netif_set_ip_info(ap, &ipInfo) != ESP_OK) {
        LOG_E("Nie udalo sie ustawic adresu AP");
    }

    configureAp();
    configureSta();

    // Outgoing traffic (including NAT) goes through the STA.
    esp_netif_set_default_netif(sta);

    ESP_ERROR_CHECK(esp_wifi_start());
}

void WifiManager::configureAp()
{
    WifiSettings s = settings();
    wifi_config_t cfg = {};
    memcpy(cfg.ap.ssid, s.apSsid.data(), std::min(s.apSsid.size(), sizeof(cfg.ap.ssid)));
    cfg.ap.ssid_len = std::min(s.apSsid.size(), sizeof(cfg.ap.ssid));
    cfg.ap.channel = 1; // after the STA connects the AP channel follows the upstream network
    cfg.ap.max_connection = s.apMaxClients;

    char ip[16];
    ipToStr(s.apIp, ip, sizeof(ip));
    if (s.apPass.size() >= 8) {
        strlcpy(reinterpret_cast<char*>(cfg.ap.password), s.apPass.c_str(), sizeof(cfg.ap.password));
        cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        LOG_I("AP: \"%s\" (WPA2), IP %s, max klientow %u", s.apSsid.c_str(), ip, s.apMaxClients);
    } else {
        cfg.ap.authmode = WIFI_AUTH_OPEN;
        if (!s.apPass.empty()) {
            LOG_E("Haslo AP ma %u znakow (wymagane min. 8) - haslo odrzucone", (unsigned)s.apPass.size());
        }
        LOG_W("!!! AP: \"%s\" jest SIECIA OTWARTA (bez hasla), IP %s !!!", s.apSsid.c_str(), ip);
    }

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
}

void WifiManager::configureSta()
{
    WifiSettings s = settings();
    wifi_config_t cfg = {};
    strlcpy(reinterpret_cast<char*>(cfg.sta.ssid), s.staSsid.c_str(), sizeof(cfg.sta.ssid));
    strlcpy(reinterpret_cast<char*>(cfg.sta.password), s.staPass.c_str(), sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    if (s.staSsid.empty()) {
        LOG_W("Brak SSID sieci nadrzednej - STA nie laczy sie (ustaw: wifi sta \"SSID\" haslo)");
    } else {
        LOG_I("STA: \"%s\", hostname %s", s.staSsid.c_str(), global.values().hostname.c_str());
    }
}

void WifiManager::onEvent(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<WifiManager*>(arg);
    if (base == WIFI_EVENT) {
        self->handleWifiEvent(id, data);
    } else if (base == IP_EVENT) {
        self->handleIpEvent(id, data);
    }
}

void WifiManager::handleWifiEvent(int32_t id, void* data)
{
    switch (id) {
    case WIFI_EVENT_STA_START:
        if (!settings().staSsid.empty()) {
            esp_wifi_connect();
        }
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        auto* ev = static_cast<wifi_event_sta_disconnected_t*>(data);
        staHasIp = false;
        if (settings().staSsid.empty()) {
            break;
        }
        // A disconnect we asked for (network change) is not an error.
        if (ev->reason != WIFI_REASON_ASSOC_LEAVE) {
            lastDisconnectReason = ev->reason;
        }
        LOG_W("STA rozlaczone: %s (reason=%d), ponawiam polaczenie", reasonText(ev->reason), ev->reason);
        esp_wifi_connect();
        break;
    }

    case WIFI_EVENT_AP_START:
        if (++apStarts > 1) {
            LOG_W("AP uruchomiony ponownie - odtwarzam NAT, DHCP i DNS");
            for (const auto& l : apRestartListeners) {
                l.first(l.second);
            }
        }
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        auto* ev = static_cast<wifi_event_ap_staconnected_t*>(data);
        LOG_I("Klient AP polaczony " MACSTR " aid=%d", MAC2STR(ev->mac), ev->aid);
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        auto* ev = static_cast<wifi_event_ap_stadisconnected_t*>(data);
        LOG_I("Klient AP rozlaczony " MACSTR " aid=%d", MAC2STR(ev->mac), ev->aid);
        break;
    }

    default:
        break;
    }
}

void WifiManager::handleIpEvent(int32_t id, void* data)
{
    switch (id) {
    case IP_EVENT_STA_GOT_IP: {
        auto* ev = static_cast<ip_event_got_ip_t*>(data);
        staHasIp = true;
        lastDisconnectReason = 0;
        LOG_I("STA IP: " IPSTR " brama: " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
        break;
    }

    case IP_EVENT_STA_LOST_IP:
        staHasIp = false;
        LOG_W("STA stracilo IP");
        break;

    default:
        break;
    }
}

void WifiManager::addApRestartListener(void (*fn)(void*), void* ctx)
{
    apRestartListeners.emplace_back(fn, ctx);
}

bool WifiManager::staLink(int8_t* rssi, uint8_t* channel) const
{
    wifi_ap_record_t info = {};
    if (!staHasIp || esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return false;
    }
    *rssi = info.rssi;
    *channel = info.primary;
    return true;
}

uint32_t WifiManager::upstreamDns() const
{
    esp_netif_dns_info_t dns = {};
    if (!staHasIp || esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &dns) != ESP_OK) {
        return 0;
    }
    return dns.ip.u_addr.ip4.addr;
}

void WifiManager::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("wifi", { "ustawienia Wi-Fi (zmiana od razu, trwałe po 'save')", "Wi-Fi settings (applied now, permanent after 'save')" });
    exec.add("wifi", { "sta", "\"<ssid>\" [haslo]", { "sieć nadrzędna; \"\" = nie łącz", "upstream network; \"\" = do not connect" }, &cmdSta, this });
    exec.add("wifi", { "ap", "\"<ssid>\" [haslo]", { "sieć AP; bez hasła = otwarta, hasło min. 8 znaków", "AP network; no password = open, password min. 8 chars" }, &cmdAp, this });
    exec.add("wifi", { "scan", "", { "skanowanie sieci Wi-Fi w zasięgu", "scan Wi-Fi networks in range" }, &cmdScan, this });
}

// Upstream network: changed immediately (disconnect and reconnect), permanent after "save".
// pass == nullptr: the current password is kept (when the SSID does not change).
CmdResult WifiManager::setSta(const char* ssid, const char* pass, IOutput& out)
{
    if (strlen(ssid) > SSID_MAX || (pass != nullptr && strlen(pass) > PASS_MAX)) {
        out.print(tr("SSID max 32 znaki, hasło max 63\r\n", "SSID max 32 characters, password max 63\r\n"));
        return CmdResult::ERR_ARGS;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (pass == nullptr) {
            pass = config.staSsid == ssid ? config.staPass.c_str() : "";
        }
        std::string p = pass;
        config.staSsid = ssid;
        config.staPass = p;
        active.staSsid = config.staSsid;
        active.staPass = config.staPass;
    }

    lastDisconnectReason = 0; // new attempt – the old error is no longer valid
    esp_wifi_disconnect();
    configureSta();
    if (ssid[0] != '\0') {
        esp_wifi_connect();
        out.printf(tr("Łączę z \"%s\"... (trwałe po 'save')\r\n", "Connecting to \"%s\"... (permanent after 'save')\r\n"), ssid);
    } else {
        out.print(tr("STA nie będzie się łączyć (trwałe po 'save')\r\n", "STA will not connect (permanent after 'save')\r\n"));
    }
    return CmdResult::OK;
}

// AP network: changed immediately (AP clients are disconnected), permanent after "save". pass == nullptr: password unchanged.
CmdResult WifiManager::setAp(const char* ssid, const char* pass, IOutput& out)
{
    if (ssid[0] == '\0' || strlen(ssid) > SSID_MAX) {
        out.print(tr("SSID AP: 1-32 znaki\r\n", "AP SSID: 1-32 characters\r\n"));
        return CmdResult::ERR_ARGS;
    }
    std::string p;
    {
        std::lock_guard<std::mutex> lock(mutex);
        p = pass != nullptr ? pass : config.apPass;
    }
    if ((!p.empty() && p.size() < 8) || p.size() > PASS_MAX) {
        out.print(tr("Hasło AP musi mieć 8-63 znaki (albo brak hasła = sieć otwarta)\r\n", "The AP password must have 8-63 characters (or no password = open network)\r\n"));
        return CmdResult::ERR_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        config.apSsid = ssid;
        config.apPass = p;
        active.apSsid = config.apSsid;
        active.apPass = config.apPass;
    }

    configureAp();
    out.printf(tr("AP zmieniony na \"%s\" (%s); klienci AP zostali rozłączeni. Trwałe po 'save'.\r\n", "AP changed to \"%s\" (%s); AP clients were disconnected. Permanent after 'save'.\r\n"), ssid,
               p.empty() ? tr("OTWARTA", "OPEN") : "WPA2");
    return CmdResult::OK;
}

const char* WifiManager::reasonText(int reason)
{
    switch (reason) {
    case 0:
        return "";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return tr("nie znaleziono sieci", "network not found");
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return tr("sieć ma niezgodne zabezpieczenia", "network security mismatch");
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return tr("złe hasło", "wrong password");
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return tr("sieć odrzuciła połączenie", "network rejected the connection");
    case WIFI_REASON_AUTH_EXPIRE:
        return tr("przekroczony czas uwierzytelniania", "authentication timeout");
    case WIFI_REASON_BEACON_TIMEOUT:
        return tr("utracono sygnał sieci", "network signal lost");
    case WIFI_REASON_ASSOC_LEAVE:
        return tr("rozłączono na żądanie", "disconnected on request");
    default:
        return tr("inny błąd", "other error");
    }
}

// Security mode name for "wifi scan" and the panel.
static const char* authName(wifi_auth_mode_t a)
{
    switch (a) {
    case WIFI_AUTH_OPEN:
        return "otwarta";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "inna";
    }
}

// Scan (blocking, approx. 2-4 s); meanwhile the radio switches channels.
bool WifiManager::scan(std::vector<ScanResult>& results)
{
    std::unique_lock<std::mutex> lock(scanMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        return false; // scan already in progress
    }
    results.clear();
    if (esp_wifi_scan_start(nullptr, true) != ESP_OK) {
        return false;
    }
    uint16_t count = 20;
    wifi_ap_record_t records[20];
    if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK) {
        return false;
    }
    for (uint16_t i = 0; i < count; ++i) {
        ScanResult r;
        r.ssid = reinterpret_cast<const char*>(records[i].ssid);
        r.rssi = records[i].rssi;
        r.channel = records[i].primary;
        r.auth = authName(records[i].authmode);
        r.open = records[i].authmode == WIFI_AUTH_OPEN;
        if (!r.ssid.empty()) {
            results.push_back(r);
        }
    }
    return true;
}

CmdResult WifiManager::cmdSta(void* ctx, int argc, char** argv, IOutput& out)
{
    if (argc < 2 || argc > 3) {
        return CmdResult::ERR_ARGS;
    }
    return static_cast<WifiManager*>(ctx)->setSta(argv[1], argc == 3 ? argv[2] : "", out);
}

CmdResult WifiManager::cmdAp(void* ctx, int argc, char** argv, IOutput& out)
{
    if (argc < 2 || argc > 3) {
        return CmdResult::ERR_ARGS;
    }
    return static_cast<WifiManager*>(ctx)->setAp(argv[1], argc == 3 ? argv[2] : "", out);
}

CmdResult WifiManager::cmdScan(void* ctx, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    std::vector<ScanResult> list;
    out.print(tr("Skanowanie...\r\n", "Scanning...\r\n"));
    if (!static_cast<WifiManager*>(ctx)->scan(list)) {
        out.print(tr("Skanowanie nie powiodło się (albo już trwa)\r\n", "Scan failed (or already running)\r\n"));
        return CmdResult::ERR_FAIL;
    }
    out.print(tr("  RSSI  kanał  zabezp.     SSID\r\n", "  RSSI  chan.  security    SSID\r\n"));
    for (const ScanResult& r : list) {
        out.printf("  %4d  %5u  %-10s  %s\r\n", r.rssi, r.channel, r.auth, r.ssid.c_str());
    }
    out.printf(tr("Znaleziono sieci: %u\r\n", "Networks found: %u\r\n"), (unsigned)list.size());
    return CmdResult::OK;
}
