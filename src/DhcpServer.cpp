/**
 * @file DhcpServer.cpp
 * @brief DhcpServer: DHCP protocol, pool, reservations, leases, "dhcp" commands.
 */
#include "DhcpServer.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <cerrno>
#include <cstring>
#include <strings.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define LOG_TAG     "DhcpServer"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::dhcp; // factory settings of dhcp.json

static constexpr uint16_t SERVER_PORT = 67;
static constexpr uint16_t CLIENT_PORT = 68;
static constexpr int64_t OFFER_HOLD_US = 60LL * 1000000; // how long an address is held after OFFER
static constexpr int STOP_TIMEOUT_MS = 1000;

// DHCP message types and options (RFC 2131/2132).
enum : uint8_t {
    DHCPDISCOVER = 1,
    DHCPOFFER = 2,
    DHCPREQUEST = 3,
    DHCPDECLINE = 4,
    DHCPACK = 5,
    DHCPNAK = 6,
    DHCPRELEASE = 7,
    DHCPINFORM = 8,
};
enum : uint8_t {
    OPT_PAD = 0,
    OPT_SUBNET_MASK = 1,
    OPT_ROUTER = 3,
    OPT_DNS = 6,
    OPT_HOSTNAME = 12,
    OPT_REQUESTED_IP = 50,
    OPT_LEASE_TIME = 51,
    OPT_MSG_TYPE = 53,
    OPT_SERVER_ID = 54,
    OPT_T1 = 58,
    OPT_T2 = 59,
    OPT_END = 255,
};
static constexpr uint32_t MAGIC_COOKIE = 0x63825363;
static constexpr int BOOTP_HEADER = 236; // up to the magic cookie
static constexpr int BOOTP_MIN_LEN = 300;

// Reads 4 bytes as an address in network byte order (unaligned access).
static uint32_t readU32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v; // network byte order, like esp_ip4_addr_t
}

// Address in network byte order -> "a.b.c.d".
static void ipStr(uint32_t ip, char* buf, size_t size)
{
    esp_ip4_addr_t a = { ip };
    snprintf(buf, size, IPSTR, IP2STR(&a));
}

// "a.b.c.d" -> address in network byte order.
static bool parseIp(const char* s, uint32_t* ip)
{
    esp_ip4_addr_t a = {};
    if (esp_netif_str_to_ip4(s, &a) != ESP_OK) {
        return false;
    }
    *ip = a.addr;
    return true;
}

// Accepts aa:bb:cc:dd:ee:ff and aa-bb-cc-dd-ee-ff, upper and lower case.
static bool parseMac(const char* s, uint8_t mac[6])
{
    unsigned v[6];
    char sep1, sep2, sep3, sep4, sep5, extra;
    if (sscanf(s, "%2x%c%2x%c%2x%c%2x%c%2x%c%2x%c", &v[0], &sep1, &v[1], &sep2, &v[2], &sep3, &v[3], &sep4, &v[4],
               &sep5, &v[5], &extra) != 11) {
        return false;
    }
    char seps[] = { sep1, sep2, sep3, sep4, sep5 };
    for (char c : seps) {
        if (c != ':' && c != '-') {
            return false;
        }
    }
    if (strlen(s) != 17) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(v[i]);
    }
    return true;
}

// MAC -> "aa:bb:cc:dd:ee:ff".
static void macStr(const uint8_t mac[6], char* buf, size_t size)
{
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Reservation name: 0-32 characters a-z A-Z 0-9 -.
static bool validName(const std::string& s)
{
    if (s.size() > 32) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// "auto" or an IPv4 address.
static bool validDns(const std::string& s)
{
    uint32_t ip;
    return s == "auto" || parseIp(s.c_str(), &ip);
}

DhcpServer::DhcpServer(WifiManager& wifi)
    : wifi(wifi)
{
}

DhcpServer::Settings DhcpServer::defaultsFor(uint32_t apIp, uint32_t apMask) const
{
    uint32_t net = ntohl(apIp) & ntohl(apMask);
    Settings s = {};
    s.poolStart = htonl(net | dflt::POOL_FIRST_HOST);
    s.poolEnd = htonl(net | dflt::POOL_LAST_HOST);
    s.leaseTime = dflt::LEASE_TIME;
    s.dns = 0;
    return s;
}

bool DhcpServer::settingsValid(const Settings& s, uint32_t apIp, uint32_t apMask, const char** why) const
{
    uint32_t start = ntohl(s.poolStart);
    uint32_t end = ntohl(s.poolEnd);
    uint32_t net = ntohl(apIp) & ntohl(apMask);
    uint32_t bcast = net | ~ntohl(apMask);
    if ((start & ntohl(apMask)) != net || (end & ntohl(apMask)) != net) {
        *why = tr("pula poza podsiecią AP", "pool outside the AP subnet");
    } else if (start > end) {
        *why = tr("początek puli za końcem", "pool start after pool end");
    } else if (end - start + 1 > MAX_POOL) {
        *why = tr("pula większa niż 128 adresów", "pool larger than 128 addresses");
    } else if (start == net || end == bcast) {
        *why = tr("pula zawiera adres sieci lub rozgłoszeniowy", "pool contains the network or broadcast address");
    } else if (ntohl(apIp) >= start && ntohl(apIp) <= end) {
        *why = tr("pula zawiera adres AP", "pool contains the AP address");
    } else {
        return true;
    }
    return false;
}

// Reservations from the file (e.g. after config revert) apply immediately.
bool DhcpServer::applyLive(const std::string& path)
{
    if (path != "reservations") {
        return false;
    }
    notifyChange();
    return true;
}

void DhcpServer::loadConfig(ConfigReader& r)
{
    WifiSettings w = wifi.configured();
    Settings def = defaultsFor(w.apIp, w.apMask);

    cfg.poolStart = r.ipv4("pool_start", def.poolStart);
    cfg.poolEnd = r.ipv4("pool_end", def.poolEnd);
    cfg.leaseTime = r.integer("lease_time", dflt::LEASE_TIME, dflt::LEASE_TIME_MIN, dflt::LEASE_TIME_MAX);
    std::string dns = r.str("dns", dflt::DNS, 15, &validDns, { "auto albo adres IPv4", "auto or IPv4 address" });
    cfg.dns = 0;
    if (dns != "auto") {
        parseIp(dns.c_str(), &cfg.dns);
    }

    const char* why = nullptr;
    if (!settingsValid(cfg, w.apIp, w.apMask, &why)) {
        r.reject("pool_start/pool_end", why);
        cfg.poolStart = def.poolStart;
        cfg.poolEnd = def.poolEnd;
    }

    std::lock_guard<std::mutex> lock(mutex);
    reservations.clear();
    const cJSON* list = r.list("reservations");
    int index = 0;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, list)
    {
        const cJSON* mac = cJSON_GetObjectItemCaseSensitive(e, "mac");
        const cJSON* ip = cJSON_GetObjectItemCaseSensitive(e, "ip");
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(e, "name");
        Reservation res = {};
        const char* bad = nullptr;
        if (!cJSON_IsObject(e)) {
            bad = tr("oczekiwano obiektu {mac, ip, name}", "object {mac, ip, name} expected");
        } else if (!cJSON_IsString(mac) || !parseMac(mac->valuestring, res.mac)) {
            bad = tr("nieprawidłowy mac", "invalid mac");
        } else if (!cJSON_IsString(ip) || !parseIp(ip->valuestring, &res.ip)) {
            bad = tr("nieprawidłowy ip", "invalid ip");
        } else if (((res.ip ^ w.apIp) & w.apMask) != 0 || res.ip == w.apIp) {
            bad = tr("ip poza podsiecią AP albo równy adresowi AP", "ip outside the AP subnet or equal to the AP address");
        } else if (name != nullptr && (!cJSON_IsString(name) || !validName(name->valuestring))) {
            bad = tr("nieprawidłowa nazwa (0-32 znaki a-z A-Z 0-9 -)", "invalid name (0-32 chars a-z A-Z 0-9 -)");
        } else if (reservationForMac(res.mac) != nullptr || reservationForIp(res.ip) != nullptr) {
            bad = tr("powtórzony mac albo ip", "duplicate mac or ip");
        } else if (reservations.size() >= MAX_RESERVATIONS) {
            bad = tr("za dużo rezerwacji (max 32)", "too many reservations (max 32)");
        }
        if (bad != nullptr) {
            r.rejectEntry("reservations", index, bad);
        } else {
            res.name = name ? name->valuestring : "";
            reservations.push_back(res);
        }
        ++index;
    }
}

cJSON* DhcpServer::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    char buf[18];
    ipStr(cfg.poolStart, buf, sizeof(buf));
    cJSON_AddStringToObject(root, "pool_start", buf);
    ipStr(cfg.poolEnd, buf, sizeof(buf));
    cJSON_AddStringToObject(root, "pool_end", buf);
    cJSON_AddNumberToObject(root, "lease_time", cfg.leaseTime);
    if (cfg.dns == 0) {
        cJSON_AddStringToObject(root, "dns", "auto");
    } else {
        ipStr(cfg.dns, buf, sizeof(buf));
        cJSON_AddStringToObject(root, "dns", buf);
    }

    cJSON* list = cJSON_AddArrayToObject(root, "reservations");
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        cJSON* e = cJSON_CreateObject();
        macStr(r.mac, buf, sizeof(buf));
        cJSON_AddStringToObject(e, "mac", buf);
        ipStr(r.ip, buf, sizeof(buf));
        cJSON_AddStringToObject(e, "ip", buf);
        cJSON_AddStringToObject(e, "name", r.name.c_str());
        cJSON_AddItemToArray(list, e);
    }
    return root;
}

esp_err_t DhcpServer::start()
{
    if (running) {
        return ESP_OK;
    }

    WifiSettings w = wifi.settings();
    serverIp = w.apIp;
    netmask = w.apMask;
    active = cfg;
    const char* why = nullptr;
    if (!settingsValid(active, serverIp, netmask, &why)) {
        Settings def = defaultsFor(serverIp, netmask);
        LOG_W("Pula DHCP: %s - uzywam domyslnej", why);
        active.poolStart = def.poolStart;
        active.poolEnd = def.poolEnd;
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        LOG_E("socket() nie powiodl sie: errno %d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));

    // AP interface only: requests from the upstream network do not arrive, replies go out through the AP.
    struct ifreq ifr = {};
    esp_netif_get_netif_impl_name(wifi.apNetif(), ifr.ifr_name);
    if (setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0) {
        LOG_E("Nie udalo sie przypiac gniazda do interfejsu AP: errno %d", errno);
        close(s);
        return ESP_FAIL;
    }

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOG_E("Nie udalo sie nasluchiwac na porcie %u: errno %d", SERVER_PORT, errno);
        close(s);
        return ESP_FAIL;
    }

    sock = s;
    stopRequested = false;
    taskAlive = true;
    if (xTaskCreate(&taskEntry, "dhcp", 4096, this, 5, nullptr) != pdPASS) {
        taskAlive = false;
        close(s);
        sock = -1;
        return ESP_ERR_NO_MEM;
    }
    running = true;

    char a[16], b[16];
    ipStr(active.poolStart, a, sizeof(a));
    ipStr(active.poolEnd, b, sizeof(b));
    LOG_I("DHCP: pula %s - %s, dzierzawa %lu s, rezerwacji %u", a, b, (unsigned long)active.leaseTime,
          (unsigned)reservations.size());
    return ESP_OK;
}

esp_err_t DhcpServer::stop()
{
    if (!running) {
        return ESP_OK;
    }
    stopRequested = true;
    for (int waited = 0; taskAlive && waited < STOP_TIMEOUT_MS; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (taskAlive) {
        return ESP_ERR_TIMEOUT;
    }
    running = false;
    LOG_I("DHCP zatrzymany");
    return ESP_OK;
}

void DhcpServer::describe(IOutput& out) const
{
    if (!running) {
        return;
    }
    int bound = 0;
    int64_t now = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Lease& l : leases) {
            bound += l.state == LeaseState::BOUND && l.expiresUs > now;
        }
    }
    char a[16], b[16];
    ipStr(active.poolStart, a, sizeof(a));
    ipStr(active.poolEnd, b, sizeof(b));
    out.printf(tr(" pula %s-%s, dzierżawy %d", " pool %s-%s, leases %d"), a, b, bound);
}

void DhcpServer::taskEntry(void* arg)
{
    auto* self = static_cast<DhcpServer*>(arg);
    self->run();
    close(self->sock);
    self->sock = -1;
    self->taskAlive = false;
    vTaskDelete(nullptr);
}

void DhcpServer::run()
{
    uint8_t buf[600];
    while (!stopRequested) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        timeval tv = { 0, 200 * 1000 };
        if (select(sock + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n > 0) {
            handlePacket(buf, n);
        }
    }
}

// ---- reservation and lease table (called with the mutex held) ----

const DhcpServer::Reservation* DhcpServer::reservationForMac(const uint8_t mac[6]) const
{
    for (const Reservation& r : reservations) {
        if (memcmp(r.mac, mac, 6) == 0) {
            return &r;
        }
    }
    return nullptr;
}

const DhcpServer::Reservation* DhcpServer::reservationForIp(uint32_t ip) const
{
    for (const Reservation& r : reservations) {
        if (r.ip == ip) {
            return &r;
        }
    }
    return nullptr;
}

DhcpServer::Lease* DhcpServer::leaseForMac(const uint8_t mac[6])
{
    for (Lease& l : leases) {
        if (memcmp(l.mac, mac, 6) == 0) {
            return &l;
        }
    }
    return nullptr;
}

bool DhcpServer::inPool(uint32_t ip) const
{
    uint32_t h = ntohl(ip);
    return h >= ntohl(active.poolStart) && h <= ntohl(active.poolEnd);
}

// Whether the client with this MAC may get/use the address.
bool DhcpServer::ipAllowed(uint32_t ip, const uint8_t mac[6]) const
{
    if (ip == 0 || ip == serverIp || ((ip ^ serverIp) & netmask) != 0) {
        return false;
    }
    const Reservation* own = reservationForMac(mac);
    if (own != nullptr) {
        return ip == own->ip;
    }
    if (reservationForIp(ip) != nullptr || !inPool(ip)) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    for (const Lease& l : leases) {
        if (l.ip == ip && l.expiresUs > now && memcmp(l.mac, mac, 6) != 0) {
            return false;
        }
    }
    return true;
}

// Order: reservation > previous lease of the MAC > requested address > unused pool address
// > the lease that expired longest ago.
uint32_t DhcpServer::pickAddress(const uint8_t mac[6], uint32_t requested)
{
    const Reservation* res = reservationForMac(mac);
    if (res != nullptr) {
        return res->ip;
    }
    Lease* own = leaseForMac(mac);
    if (own != nullptr && ipAllowed(own->ip, mac)) {
        return own->ip;
    }
    if (requested != 0 && ipAllowed(requested, mac)) {
        return requested;
    }

    for (uint32_t h = ntohl(active.poolStart); h <= ntohl(active.poolEnd); ++h) {
        uint32_t ip = htonl(h);
        if (!ipAllowed(ip, mac)) {
            continue;
        }
        bool used = false;
        for (const Lease& l : leases) {
            used = used || l.ip == ip;
        }
        if (!used) {
            return ip;
        }
    }

    const Lease* oldest = nullptr;
    for (const Lease& l : leases) {
        if (ipAllowed(l.ip, mac) && (oldest == nullptr || l.expiresUs < oldest->expiresUs)) {
            oldest = &l;
        }
    }
    return oldest ? oldest->ip : 0;
}

void DhcpServer::dropLeasesOfIp(uint32_t ip, const uint8_t* exceptMac)
{
    for (size_t i = 0; i < leases.size();) {
        if (leases[i].ip == ip && (exceptMac == nullptr || memcmp(leases[i].mac, exceptMac, 6) != 0)) {
            leases.erase(leases.begin() + i);
        } else {
            ++i;
        }
    }
}

DhcpServer::Lease& DhcpServer::setLease(const uint8_t mac[6], uint32_t ip, LeaseState state, int64_t durationUs,
                                        const char* host)
{
    dropLeasesOfIp(ip, mac);
    Lease* l = leaseForMac(mac);
    if (l == nullptr) {
        // Table limit: remove the lease that expired longest ago.
        int64_t now = esp_timer_get_time();
        if (leases.size() >= MAX_POOL + MAX_RESERVATIONS) {
            size_t oldest = 0;
            for (size_t i = 1; i < leases.size(); ++i) {
                if (leases[i].expiresUs < leases[oldest].expiresUs) {
                    oldest = i;
                }
            }
            if (leases[oldest].expiresUs <= now) {
                leases.erase(leases.begin() + oldest);
            }
        }
        leases.push_back({});
        l = &leases.back();
        memcpy(l->mac, mac, 6);
    }
    l->ip = ip;
    l->state = state;
    l->expiresUs = esp_timer_get_time() + durationUs;
    if (host != nullptr && host[0] != '\0') {
        strlcpy(l->host, host, sizeof(l->host));
    }
    return *l;
}

bool DhcpServer::lookup(const uint8_t mac[6], uint32_t* ip, char* host, size_t hostSize) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Lease& l : leases) {
        if (memcmp(l.mac, mac, 6) == 0 && l.state == LeaseState::BOUND) {
            *ip = l.ip;
            const Reservation* r = reservationForMac(mac);
            strlcpy(host, (r && !r->name.empty()) ? r->name.c_str() : l.host, hostSize);
            return true;
        }
    }
    const Reservation* r = reservationForMac(mac);
    if (r != nullptr) {
        *ip = r->ip;
        strlcpy(host, r->name.c_str(), hostSize);
        return true;
    }
    return false;
}

bool DhcpServer::reservationIp(const char* name, uint32_t* ip) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        if (!r.name.empty() && r.name == name) {
            *ip = r.ip;
            return true;
        }
    }
    return false;
}

bool DhcpServer::resolveName(const char* name, uint32_t* ip, bool* fromReservation) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        if (!r.name.empty() && strcasecmp(r.name.c_str(), name) == 0) {
            *ip = r.ip;
            *fromReservation = true;
            return true;
        }
    }
    int64_t now = esp_timer_get_time();
    for (const Lease& l : leases) {
        if (l.state == LeaseState::BOUND && l.expiresUs > now && l.host[0] != '\0' && strcasecmp(l.host, name) == 0) {
            *ip = l.ip;
            *fromReservation = false;
            return true;
        }
    }
    return false;
}

bool DhcpServer::nameForIp(uint32_t ip, char* name, size_t size) const
{
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        if (r.ip == ip && !r.name.empty()) {
            strlcpy(name, r.name.c_str(), size);
            return true;
        }
    }
    int64_t now = esp_timer_get_time();
    for (const Lease& l : leases) {
        if (l.ip == ip && l.state == LeaseState::BOUND && l.expiresUs > now && l.host[0] != '\0') {
            strlcpy(name, l.host, size);
            return true;
        }
    }
    return false;
}

std::vector<DhcpServer::NameEntry> DhcpServer::names() const
{
    std::vector<NameEntry> list;
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        if (!r.name.empty()) {
            list.push_back({ r.name, r.ip, true });
        }
    }
    int64_t now = esp_timer_get_time();
    for (const Lease& l : leases) {
        if (l.state == LeaseState::BOUND && l.expiresUs > now && l.host[0] != '\0') {
            list.push_back({ l.host, l.ip, false });
        }
    }
    return list;
}

bool DhcpServer::isReserved(const uint8_t mac[6]) const
{
    std::lock_guard<std::mutex> lock(mutex);
    return reservationForMac(mac) != nullptr;
}

cJSON* DhcpServer::leasesJson() const
{
    cJSON* root = cJSON_CreateObject();
    char a[18], b[16];
    ipStr(active.poolStart, a, sizeof(a));
    ipStr(active.poolEnd, b, sizeof(b));
    cJSON_AddStringToObject(root, "pool_start", a);
    cJSON_AddStringToObject(root, "pool_end", b);
    cJSON_AddBoolToObject(root, "running", running);
    cJSON* list = cJSON_AddArrayToObject(root, "leases");
    int64_t now = esp_timer_get_time();
    std::lock_guard<std::mutex> lock(mutex);
    for (const Lease& l : leases) {
        if (l.expiresUs <= now) {
            continue;
        }
        cJSON* o = cJSON_CreateObject();
        macStr(l.mac, a, sizeof(a));
        cJSON_AddStringToObject(o, "mac", a);
        ipStr(l.ip, b, sizeof(b));
        cJSON_AddStringToObject(o, "ip", b);
        cJSON_AddStringToObject(o, "host", l.host);
        cJSON_AddNumberToObject(o, "remaining", static_cast<double>((l.expiresUs - now) / 1000000));
        cJSON_AddStringToObject(o, "type", l.state == LeaseState::OFFERED ? "oferta"
                                           : reservationForMac(l.mac)   ? "rezerwacja"
                                                                        : "dynamiczny");
        cJSON_AddItemToArray(list, o);
    }
    return root;
}

cJSON* DhcpServer::reservationsJson() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(root, "reservations");
    char a[18], b[16];
    std::lock_guard<std::mutex> lock(mutex);
    for (const Reservation& r : reservations) {
        cJSON* o = cJSON_CreateObject();
        macStr(r.mac, a, sizeof(a));
        cJSON_AddStringToObject(o, "mac", a);
        ipStr(r.ip, b, sizeof(b));
        cJSON_AddStringToObject(o, "ip", b);
        cJSON_AddStringToObject(o, "name", r.name.c_str());
        cJSON_AddItemToArray(list, o);
    }
    cJSON_AddNumberToObject(root, "max", MAX_RESERVATIONS);
    return root;
}

// ---- protocol ----

void DhcpServer::handlePacket(const uint8_t* buf, int len)
{
    if (len < BOOTP_HEADER + 4 || buf[0] != 1 || buf[1] != 1 || buf[2] != 6 ||
        ntohl(readU32(buf + BOOTP_HEADER)) != MAGIC_COOKIE) {
        return;
    }

    uint8_t type = 0;
    uint32_t requested = 0;
    uint32_t serverId = 0;
    char host[33] = {};
    for (int i = BOOTP_HEADER + 4; i < len;) {
        uint8_t opt = buf[i++];
        if (opt == OPT_PAD) {
            continue;
        }
        if (opt == OPT_END || i >= len) {
            break;
        }
        uint8_t olen = buf[i++];
        if (i + olen > len) {
            break;
        }
        const uint8_t* v = buf + i;
        if (opt == OPT_MSG_TYPE && olen == 1) {
            type = v[0];
        } else if (opt == OPT_REQUESTED_IP && olen == 4) {
            requested = readU32(v);
        } else if (opt == OPT_SERVER_ID && olen == 4) {
            serverId = readU32(v);
        } else if (opt == OPT_HOSTNAME) {
            size_t n = olen < sizeof(host) - 1 ? olen : sizeof(host) - 1;
            for (size_t k = 0; k < n; ++k) {
                char c = static_cast<char>(v[k]);
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
                host[k] = ok ? c : '-';
            }
        }
        i += olen;
    }

    const uint8_t* mac = buf + 28;
    uint32_t ciaddr = readU32(buf + 12);
    char macS[18], ipS[16];
    macStr(mac, macS, sizeof(macS));

    switch (type) {
    case DHCPDISCOVER: {
        uint32_t ip;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ip = pickAddress(mac, requested);
            if (ip != 0) {
                setLease(mac, ip, LeaseState::OFFERED, OFFER_HOLD_US, host);
            }
        }
        if (ip == 0) {
            LOG_W("Brak wolnych adresow w puli dla %s", macS);
            return;
        }
        sendReply(buf, DHCPOFFER, ip, false);
        break;
    }

    case DHCPREQUEST: {
        if (serverId != 0 && serverId != serverIp) {
            // The client chose another server – release the offered address.
            std::lock_guard<std::mutex> lock(mutex);
            Lease* l = leaseForMac(mac);
            if (l != nullptr && l->state == LeaseState::OFFERED) {
                l->expiresUs = 0;
            }
            return;
        }
        uint32_t ip = requested != 0 ? requested : ciaddr;
        if (ip == 0) {
            return;
        }
        bool ok;
        {
            std::lock_guard<std::mutex> lock(mutex);
            ok = ipAllowed(ip, mac);
            if (ok) {
                setLease(mac, ip, LeaseState::BOUND, static_cast<int64_t>(active.leaseTime) * 1000000, host);
            }
        }
        ipStr(ip, ipS, sizeof(ipS));
        if (ok) {
            notifyChange();
            sendReply(buf, DHCPACK, ip, false);
            LOG_I("DHCP: %s -> %s%s%s", ipS, macS, host[0] ? " " : "", host);
        } else {
            sendReply(buf, DHCPNAK, 0, true);
            LOG_W("DHCP: NAK dla %s (zadany %s)", macS, ipS);
        }
        break;
    }

    case DHCPDECLINE: {
        // Address in use on the network (e.g. static) – block it for the lease time.
        if (requested == 0) {
            return;
        }
        static const uint8_t declined[6] = {};
        std::lock_guard<std::mutex> lock(mutex);
        setLease(declined, requested, LeaseState::BOUND, static_cast<int64_t>(active.leaseTime) * 1000000, "zajety");
        ipStr(requested, ipS, sizeof(ipS));
        LOG_W("DHCP: %s odrzucil %s (adres zajety) - zablokowany", macS, ipS);
        break;
    }

    case DHCPRELEASE: {
        std::lock_guard<std::mutex> lock(mutex);
        Lease* l = leaseForMac(mac);
        if (l != nullptr && l->ip == ciaddr) {
            l->expiresUs = esp_timer_get_time(); // stays in the table so that the client gets the same address back
            LOG_I("DHCP: %s zwolnil adres", macS);
        }
        break;
    }

    case DHCPINFORM:
        sendReply(buf, DHCPACK, 0, true);
        break;

    default:
        break;
    }
}

void DhcpServer::sendReply(const uint8_t* req, uint8_t type, uint32_t yiaddr, bool noLeaseOptions)
{
    uint8_t* p = txBuf;
    memset(p, 0, sizeof(txBuf));
    p[0] = 2; // BOOTREPLY
    p[1] = 1;
    p[2] = 6;
    memcpy(p + 4, req + 4, 4);    // xid
    memcpy(p + 10, req + 10, 2);  // flags
    if (type != DHCPNAK) {
        memcpy(p + 12, req + 12, 4); // ciaddr
    }
    memcpy(p + 16, &yiaddr, 4);
    memcpy(p + 24, req + 24, 4);  // giaddr
    memcpy(p + 28, req + 28, 16); // chaddr
    uint32_t cookie = htonl(MAGIC_COOKIE);
    memcpy(p + BOOTP_HEADER, &cookie, 4);

    int i = BOOTP_HEADER + 4;
    auto opt = [&](uint8_t code, const void* data, uint8_t len) {
        p[i++] = code;
        p[i++] = len;
        memcpy(p + i, data, len);
        i += len;
    };
    auto optU32 = [&](uint8_t code, uint32_t netOrder) { opt(code, &netOrder, 4); };

    opt(OPT_MSG_TYPE, &type, 1);
    optU32(OPT_SERVER_ID, serverIp);
    if (type != DHCPNAK) {
        if (!noLeaseOptions) {
            optU32(OPT_LEASE_TIME, htonl(active.leaseTime));
            optU32(OPT_T1, htonl(active.leaseTime / 2));
            optU32(OPT_T2, htonl(active.leaseTime / 8 * 7));
        }
        optU32(OPT_SUBNET_MASK, netmask);
        optU32(OPT_ROUTER, serverIp);
        // dns "auto": the AP address while our DnsServer runs; otherwise the upstream DNS of the STA network.
        uint32_t dns = active.dns;
        if (dns == 0) {
            dns = (localDns != nullptr && localDns->isRunning()) ? serverIp : wifi.upstreamDns();
        }
        if (dns != 0) {
            optU32(OPT_DNS, dns);
        }
    }
    p[i++] = OPT_END;
    int len = i < BOOTP_MIN_LEN ? BOOTP_MIN_LEN : i;

    // Client with an address (renewal, INFORM) – unicast; others – broadcast on the AP.
    uint32_t ciaddr = readU32(req + 12);
    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(CLIENT_PORT);
    dst.sin_addr.s_addr = (type != DHCPNAK && ciaddr != 0) ? ciaddr : htonl(INADDR_BROADCAST);
    if (sendto(sock, p, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) < 0) {
        LOG_W("sendto() nie powiodl sie: errno %d", errno);
    }
}

// ---- commands ----

void DhcpServer::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("dhcp", { "serwer DHCP (dzierżawy, rezerwacje MAC->IP)", "DHCP server (leases, MAC->IP reservations)" });
    exec.add("dhcp", { "leases", "", { "aktywne dzierżawy", "active leases" }, &cmdLeases, this });
    exec.add("dhcp", { "reservations", "", { "lista rezerwacji", "list of reservations" }, &cmdReservations, this });
    exec.add("dhcp", { "reserve", "<mac> [ip] [nazwa]", { "rezerwacja; bez ip = bieżący adres klienta (trwałe po save)", "reservation; without ip = client's current address (permanent after save)" },
                       &notifying<&cmdReserve>, this });
    exec.add("dhcp", { "unreserve", "<mac|ip|nr>", { "usuwa rezerwację (trwałe po save)", "deletes a reservation (permanent after save)" }, &notifying<&cmdUnreserve>, this });
    exec.add("dhcp", { "release", "<mac|ip>", { "usuwa dzierżawę", "removes a lease" }, &notifying<&cmdRelease>, this });
}

// Remaining lease time as "1h05m" / "4m30s".
static void formatRemaining(int64_t us, char* buf, size_t size)
{
    long s = us > 0 ? static_cast<long>(us / 1000000) : 0;
    if (s >= 3600) {
        snprintf(buf, size, "%ldh%02ldm", s / 3600, s / 60 % 60);
    } else {
        snprintf(buf, size, "%ldm%02lds", s / 60, s % 60);
    }
}

CmdResult DhcpServer::cmdLeases(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DhcpServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    if (!self->running) {
        out.print(tr("DHCP zatrzymany\r\n", "DHCP stopped\r\n"));
    }
    int64_t now = esp_timer_get_time();
    int count = 0;
    std::lock_guard<std::mutex> lock(self->mutex);
    for (const Lease& l : self->leases) {
        if (l.expiresUs <= now) {
            continue;
        }
        if (count++ == 0) {
            out.print(tr("  MAC                IP               nazwa                pozostało  typ\r\n",
                         "  MAC                IP               name                 remaining  type\r\n"));
        }
        char macS[18], ipS[16], rem[16];
        macStr(l.mac, macS, sizeof(macS));
        ipStr(l.ip, ipS, sizeof(ipS));
        formatRemaining(l.expiresUs - now, rem, sizeof(rem));
        const char* kind = self->reservationForMac(l.mac) ? "rez" : "dyn";
        if (l.state == LeaseState::OFFERED) {
            kind = tr("oferta", "offer");
        }
        out.printf("  %s  %-15s  %-20s %-10s %s\r\n", macS, ipS, l.host, rem, kind);
    }
    if (count == 0) {
        out.print(tr("Brak aktywnych dzierżaw\r\n", "No active leases\r\n"));
    }
    return CmdResult::OK;
}

CmdResult DhcpServer::cmdReservations(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DhcpServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    if (self->reservations.empty()) {
        out.print(tr("Brak rezerwacji\r\n", "No reservations\r\n"));
        return CmdResult::OK;
    }
    out.print(tr("  nr  MAC                IP               nazwa\r\n", "  no  MAC                IP               name\r\n"));
    for (size_t i = 0; i < self->reservations.size(); ++i) {
        const Reservation& r = self->reservations[i];
        char macS[18], ipS[16];
        macStr(r.mac, macS, sizeof(macS));
        ipStr(r.ip, ipS, sizeof(ipS));
        out.printf("  %-3u %s  %-15s  %s\r\n", (unsigned)i, macS, ipS, r.name.c_str());
    }
    return CmdResult::OK;
}

CmdResult DhcpServer::cmdReserve(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DhcpServer*>(ctx);
    if (argc < 2 || argc > 4) {
        return CmdResult::ERR_ARGS;
    }
    uint8_t mac[6];
    if (!parseMac(argv[1], mac)) {
        out.print(tr("Nieprawidłowy MAC (format aa:bb:cc:dd:ee:ff)\r\n", "Invalid MAC (format aa:bb:cc:dd:ee:ff)\r\n"));
        return CmdResult::ERR_ARGS;
    }

    // Second argument: IP address or (when not an IP) a name – then the address of the current lease.
    uint32_t ip = 0;
    const char* name = "";
    if (argc >= 3 && parseIp(argv[2], &ip)) {
        name = argc == 4 ? argv[3] : "";
    } else {
        if (argc == 4) {
            return CmdResult::ERR_ARGS;
        }
        name = argc == 3 ? argv[2] : "";
    }
    if (!validName(name)) {
        out.print(tr("Nieprawidłowa nazwa (0-32 znaki a-z A-Z 0-9 -)\r\n", "Invalid name (0-32 chars a-z A-Z 0-9 -)\r\n"));
        return CmdResult::ERR_FAIL;
    }

    WifiSettings w = self->wifi.settings();
    std::lock_guard<std::mutex> lock(self->mutex);
    if (ip == 0) {
        Lease* l = self->leaseForMac(mac);
        if (l == nullptr || l->state != LeaseState::BOUND) {
            out.print(tr("Klient nie ma dzierżawy – podaj adres IP\r\n", "The client has no lease – give an IP address\r\n"));
            return CmdResult::ERR_FAIL;
        }
        ip = l->ip;
    }
    if (((ip ^ w.apIp) & w.apMask) != 0 || ip == w.apIp) {
        out.print(tr("Adres poza podsiecią AP albo równy adresowi AP\r\n", "Address outside the AP subnet or equal to the AP address\r\n"));
        return CmdResult::ERR_FAIL;
    }
    const Reservation* other = self->reservationForIp(ip);
    if (other != nullptr && memcmp(other->mac, mac, 6) != 0) {
        out.print(tr("Ten adres jest już zarezerwowany dla innego MAC\r\n", "This address is already reserved for another MAC\r\n"));
        return CmdResult::ERR_FAIL;
    }

    Reservation* existing = nullptr;
    for (Reservation& r : self->reservations) {
        if (memcmp(r.mac, mac, 6) == 0) {
            existing = &r;
        }
    }
    if (existing == nullptr) {
        if (self->reservations.size() >= MAX_RESERVATIONS) {
            out.print(tr("Limit 32 rezerwacji\r\n", "Limit of 32 reservations\r\n"));
            return CmdResult::ERR_FAIL;
        }
        self->reservations.push_back({});
        existing = &self->reservations.back();
        memcpy(existing->mac, mac, 6);
    }
    existing->ip = ip;
    existing->name = name;

    // Address dynamically taken by another MAC – that lease stops being valid.
    for (const Lease& l : self->leases) {
        if (l.ip == ip && memcmp(l.mac, mac, 6) != 0) {
            char m[18];
            macStr(l.mac, m, sizeof(m));
            out.printf(tr("Uwaga: adres był przydzielony %s – dostanie inny przy odnowieniu\r\n", "Note: the address was assigned to %s – it will get another on renewal\r\n"), m);
        }
    }
    self->dropLeasesOfIp(ip, mac);
    Lease* own = self->leaseForMac(mac);
    if (own != nullptr && own->ip != ip) {
        out.print(tr("Klient ma teraz inny adres – dostanie zarezerwowany przy odnowieniu (albo: dhcp release)\r\n", "The client now has another address – it will get the reserved one on renewal (or: dhcp release)\r\n"));
    }

    char ipS[16];
    ipStr(ip, ipS, sizeof(ipS));
    out.printf(tr("Rezerwacja %s -> %s%s%s. Działa od razu; trwałe po 'save'\r\n", "Reservation %s -> %s%s%s. Applied now; permanent after 'save'\r\n"), argv[1], ipS, name[0] ? " " : "", name);
    return CmdResult::OK;
}

CmdResult DhcpServer::cmdUnreserve(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DhcpServer*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    uint8_t mac[6];
    uint32_t ip = 0;
    bool byMac = parseMac(argv[1], mac);
    bool byIp = !byMac && parseIp(argv[1], &ip);
    char* end = nullptr;
    long nr = (!byMac && !byIp) ? strtol(argv[1], &end, 10) : -1;
    if (!byMac && !byIp && (end == argv[1] || *end != '\0')) {
        return CmdResult::ERR_ARGS;
    }

    std::lock_guard<std::mutex> lock(self->mutex);
    for (size_t i = 0; i < self->reservations.size(); ++i) {
        const Reservation& r = self->reservations[i];
        if ((byMac && memcmp(r.mac, mac, 6) == 0) || (byIp && r.ip == ip) || (!byMac && !byIp && (long)i == nr)) {
            char macS[18];
            macStr(r.mac, macS, sizeof(macS));
            out.printf(tr("Usunięto rezerwację %s. Trwałe po 'save'\r\n", "Reservation %s deleted. Permanent after 'save'\r\n"), macS);
            self->reservations.erase(self->reservations.begin() + i);
            return CmdResult::OK;
        }
    }
    out.print(tr("Nie znaleziono rezerwacji\r\n", "Reservation not found\r\n"));
    return CmdResult::ERR_FAIL;
}

CmdResult DhcpServer::cmdRelease(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DhcpServer*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    uint8_t mac[6];
    uint32_t ip = 0;
    bool byMac = parseMac(argv[1], mac);
    if (!byMac && !parseIp(argv[1], &ip)) {
        return CmdResult::ERR_ARGS;
    }

    std::lock_guard<std::mutex> lock(self->mutex);
    for (size_t i = 0; i < self->leases.size(); ++i) {
        const Lease& l = self->leases[i];
        if ((byMac && memcmp(l.mac, mac, 6) == 0) || (!byMac && l.ip == ip)) {
            self->leases.erase(self->leases.begin() + i);
            out.print(tr("Dzierżawa usunięta – klient dostanie adres na nowo przy odnowieniu\r\n", "Lease removed – the client will get an address again on renewal\r\n"));
            return CmdResult::OK;
        }
    }
    out.print(tr("Nie znaleziono dzierżawy\r\n", "Lease not found\r\n"));
    return CmdResult::ERR_FAIL;
}
