/**
 * @file DnsServer.cpp
 * @brief DnsServer: local names, block list, cache, forwarding, "dns" commands, panel API.
 */
#include "DnsServer.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_TAG     "DnsServer"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::dns; // factory settings of dns.json

static constexpr uint16_t DNS_PORT = 53;
static constexpr int64_t UPSTREAM_TIMEOUT_US = 2000000; // per server; at most 4 s in total with two
static constexpr uint32_t CACHE_TTL_MIN = 10;
static constexpr uint32_t CACHE_TTL_MAX = 3600;
static constexpr uint32_t CACHE_NEGATIVE_TTL_MAX = 60; // NXDOMAIN / no data
static constexpr int NO_PSRAM_CACHE_LIMIT = 32;
static constexpr size_t BLOCK_FILE_LIMIT = 900 * 1024; // the LittleFS partition has 1 MB
static constexpr int STOP_TIMEOUT_MS = 1000;

enum : uint16_t { TYPE_A = 1, TYPE_PTR = 12, TYPE_AAAA = 28, TYPE_ANY = 255 };
enum : uint8_t { RCODE_OK = 0, RCODE_FORMERR = 1, RCODE_SERVFAIL = 2, RCODE_NXDOMAIN = 3, RCODE_NOTIMP = 4 };

// ---- helpers ----

// Converts a string to lower case in place.
static void toLower(char* s)
{
    for (; *s; ++s) {
        *s = static_cast<char>(tolower(static_cast<unsigned char>(*s)));
    }
}

// Domain name: labels of 1-63 characters a-z 0-9 - _, max 127 in total (cache buffers).
static bool validDnsName(const std::string& s)
{
    if (s.empty() || s.size() > 127 || s.front() == '.' || s.back() == '.') {
        return false;
    }
    size_t label = 0;
    for (char c : s) {
        if (c == '.') {
            if (label == 0) {
                return false;
            }
            label = 0;
            continue;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') || ++label > 63) {
            return false;
        }
    }
    return true;
}

// Local domain: 1-32 characters a-z 0-9 - . (labels like domain names).
static bool validDomain(const std::string& s)
{
    return s.size() <= 32 && validDnsName(s);
}

// Block response: "zero" or "nxdomain".
static bool validResponse(const std::string& s)
{
    return s == "zero" || s == "nxdomain";
}

// FNV-1a hash of name and type for the cache (never 0 – 0 marks an empty slot).
static uint32_t nameHash(const char* name, uint16_t qtype)
{
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; ++p) {
        h = (h ^ static_cast<uint8_t>(*p)) * 16777619u;
    }
    h = (h ^ qtype) * 16777619u;
    return h ? h : 1;
}

// Big-endian 16-bit read.
static uint16_t rd16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// Big-endian 32-bit read.
static uint32_t rd32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

// Big-endian 16-bit write.
static void wr16(uint8_t* p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

// Big-endian 32-bit write.
static void wr32(uint8_t* p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = (v >> 16) & 0xff;
    p[2] = (v >> 8) & 0xff;
    p[3] = v & 0xff;
}

// First question of a query: name (with dots), type, end of the question section.
static bool parseQuestion(const uint8_t* buf, int len, char* name, size_t nameSize, uint16_t* qtype, int* qend)
{
    int pos = 12;
    size_t n = 0;
    while (true) {
        if (pos >= len) {
            return false;
        }
        uint8_t l = buf[pos++];
        if (l == 0) {
            break;
        }
        if (l > 63 || pos + l > len || n + l + 2 > nameSize) {
            return false; // compression in the question is not expected
        }
        if (n > 0) {
            name[n++] = '.';
        }
        memcpy(name + n, buf + pos, l);
        n += l;
        pos += l;
    }
    name[n] = '\0';
    if (pos + 4 > len) {
        return false;
    }
    *qtype = rd16(buf + pos);
    *qend = pos + 4;
    return true;
}

// Skips an encoded name (labels or a compression pointer); returns the position after it or -1.
static int skipName(const uint8_t* buf, int len, int pos)
{
    while (pos < len) {
        uint8_t l = buf[pos];
        if (l == 0) {
            return pos + 1;
        }
        if ((l & 0xC0) == 0xC0) {
            return pos + 2;
        }
        pos += l + 1;
    }
    return -1;
}

// Smallest TTL of an answer (answer section, or the authority section when empty, e.g. SOA).
static uint32_t responseTtl(const uint8_t* buf, int len)
{
    int qd = rd16(buf + 4), an = rd16(buf + 6), ns = rd16(buf + 8);
    int pos = 12;
    for (int i = 0; i < qd && pos > 0; ++i) {
        pos = skipName(buf, len, pos);
        pos = pos > 0 ? pos + 4 : -1;
    }
    uint32_t ttl = UINT32_MAX;
    int records = an > 0 ? an : ns;
    for (int i = 0; i < records && pos > 0; ++i) {
        pos = skipName(buf, len, pos);
        if (pos < 0 || pos + 10 > len) {
            break;
        }
        ttl = std::min(ttl, rd32(buf + pos + 4));
        pos += 10 + rd16(buf + pos + 8);
    }
    if (an == 0) {
        // Negative answer: TTL from SOA, but not longer than 60 s.
        ttl = std::min(ttl, CACHE_NEGATIVE_TTL_MAX);
    }
    return std::max(CACHE_TTL_MIN, std::min(ttl, CACHE_TTL_MAX));
}

// Header + question copied from the query, response flags; returns the length.
static int startResponse(uint8_t* out, const uint8_t* query, int qend, uint8_t rcode, bool authoritative)
{
    memcpy(out, query, qend);
    out[2] = 0x80 | (query[2] & 0x79) | (authoritative ? 0x04 : 0); // QR, opcode, RD, AA
    out[3] = 0x80 | rcode;                                          // RA
    wr16(out + 4, qend > 12 ? 1 : 0);
    wr16(out + 6, 0);
    wr16(out + 8, 0);
    wr16(out + 10, 0);
    return qend;
}

// Appends an answer record that points to the question name; returns the new position.
static int appendRecord(uint8_t* out, int pos, uint16_t type, uint32_t ttl, const uint8_t* rdata, uint16_t rdlen)
{
    out[pos++] = 0xC0; // pointer to the name in the question
    out[pos++] = 0x0C;
    wr16(out + pos, type);
    wr16(out + pos + 2, 1); // IN
    wr32(out + pos + 4, ttl);
    wr16(out + pos + 8, rdlen);
    memcpy(out + pos + 10, rdata, rdlen);
    wr16(out + 6, rd16(out + 6) + 1);
    return pos + 10 + rdlen;
}

// "a.b.c" -> DNS label encoding; returns the encoded length.
static int encodeName(const char* name, uint8_t* out)
{
    int n = 0;
    while (*name) {
        const char* dot = strchr(name, '.');
        int l = dot ? dot - name : strlen(name);
        out[n++] = l;
        memcpy(out + n, name, l);
        n += l;
        name += l + (dot ? 1 : 0);
    }
    out[n++] = 0;
    return n;
}

// ---- configuration ----

DnsServer::DnsServer(WifiManager& wifi, DhcpServer& dhcp, const GlobalSettings& global, ConfigStore& store)
    : wifi(wifi),
      dhcp(dhcp),
      global(global),
      store(store)
{
}

void DnsServer::loadConfig(ConfigReader& r)
{
    cfg.domain = r.str("domain", dflt::DOMAIN, 32, &validDomain, { "1-32 znaki a-z 0-9 - .", "1-32 chars a-z 0-9 - ." });
    cfg.localTtl = r.integer("local_ttl", dflt::LOCAL_TTL, dflt::LOCAL_TTL_MIN, dflt::LOCAL_TTL_MAX);
    cfg.cacheSize = r.integer("cache_size", dflt::CACHE_SIZE, dflt::CACHE_SIZE_MIN, dflt::CACHE_SIZE_MAX);
    cfg.cacheSweepS = r.integer("cache_sweep_s", dflt::CACHE_SWEEP_S, dflt::CACHE_SWEEP_S_MIN, dflt::CACHE_SWEEP_S_MAX);
    cfg.dhcpNames = r.boolean("dhcp_names", dflt::DHCP_NAMES);

    ConfigReader b = r.object("block");
    cfg.blockEnabled = b.boolean("enabled", dflt::BLOCK_ENABLED);
    cfg.blockZero = b.str("response", dflt::BLOCK_RESPONSE, 8, &validResponse, { "zero/nxdomain", "zero/nxdomain" }) == "zero";
    b.finish();

    std::lock_guard<std::mutex> lock(mutex);
    upstream.clear();
    const cJSON* list = r.list("upstream");
    int index = 0;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, list)
    {
        esp_ip4_addr_t ip = {};
        if (cJSON_IsString(e) && strcmp(e->valuestring, "auto") == 0) {
            if (cJSON_GetArraySize(list) > 1) {
                r.rejectEntry("upstream", index, tr("\"auto\" nie łączy się z adresami", "\"auto\" cannot be combined with addresses"));
            }
        } else if (!cJSON_IsString(e) || esp_netif_str_to_ip4(e->valuestring, &ip) != ESP_OK) {
            r.rejectEntry("upstream", index, tr("oczekiwano \"auto\" albo adresu IPv4", "\"auto\" or IPv4 address expected"));
        } else if (upstream.size() >= 2) {
            r.rejectEntry("upstream", index, tr("max 2 serwery", "max 2 servers"));
        } else {
            upstream.push_back(ip.addr);
        }
        ++index;
    }

    entries.clear();
    list = r.list("entries");
    index = 0;
    cJSON_ArrayForEach(e, list)
    {
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON* ip = cJSON_GetObjectItemCaseSensitive(e, "ip");
        Entry entry = {};
        esp_ip4_addr_t addr = {};
        const char* bad = nullptr;
        if (!cJSON_IsObject(e) || !cJSON_IsString(name)) {
            bad = tr("oczekiwano {name, ip}", "{name, ip} expected");
        } else {
            entry.name = name->valuestring;
            std::transform(entry.name.begin(), entry.name.end(), entry.name.begin(), ::tolower);
            if (!entry.name.empty() && entry.name.back() == '.') {
                entry.name.pop_back();
            }
            if (!validDnsName(entry.name)) {
                bad = tr("nieprawidłowa nazwa", "invalid name");
            } else if (!cJSON_IsString(ip) || esp_netif_str_to_ip4(ip->valuestring, &addr) != ESP_OK) {
                bad = tr("nieprawidłowy ip", "invalid ip");
            } else if (entries.size() >= MAX_ENTRIES) {
                bad = tr("max 64 wpisy", "max 64 entries");
            }
            for (const Entry& o : entries) {
                if (bad == nullptr && o.name == entry.name) {
                    bad = tr("powtórzona nazwa", "duplicate name");
                }
            }
        }
        if (bad != nullptr) {
            r.rejectEntry("entries", index, bad);
        } else {
            entry.ip = addr.addr;
            entries.push_back(entry);
        }
        ++index;
    }
}

cJSON* DnsServer::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "domain", cfg.domain.c_str());
    cJSON_AddNumberToObject(root, "local_ttl", cfg.localTtl);
    cJSON_AddNumberToObject(root, "cache_size", cfg.cacheSize);
    cJSON_AddNumberToObject(root, "cache_sweep_s", cfg.cacheSweepS);
    cJSON_AddBoolToObject(root, "dhcp_names", cfg.dhcpNames);
    cJSON* b = cJSON_AddObjectToObject(root, "block");
    cJSON_AddBoolToObject(b, "enabled", cfg.blockEnabled);
    cJSON_AddStringToObject(b, "response", cfg.blockZero ? "zero" : "nxdomain");

    std::lock_guard<std::mutex> lock(mutex);
    char buf[16];
    cJSON* up = cJSON_AddArrayToObject(root, "upstream");
    if (upstream.empty()) {
        cJSON_AddItemToArray(up, cJSON_CreateString("auto"));
    }
    for (uint32_t ip : upstream) {
        esp_ip4_addr_t a = { ip };
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a));
        cJSON_AddItemToArray(up, cJSON_CreateString(buf));
    }
    cJSON* list = cJSON_AddArrayToObject(root, "entries");
    for (const Entry& e : entries) {
        cJSON* o = cJSON_CreateObject();
        esp_ip4_addr_t a = { e.ip };
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a));
        cJSON_AddStringToObject(o, "name", e.name.c_str());
        cJSON_AddStringToObject(o, "ip", buf);
        cJSON_AddItemToArray(list, o);
    }
    return root;
}

bool DnsServer::applyLive(const std::string& path)
{
    if (path == "entries" || path == "upstream") {
        return true; // lists work directly from the loaded configuration
    }
    if (path == "local_ttl" || path == "dhcp_names" || path == "block.enabled" || path == "block.response" ||
        path == "cache_sweep_s") {
        active.localTtl = cfg.localTtl;
        active.cacheSweepS = cfg.cacheSweepS;
        active.dhcpNames = cfg.dhcpNames;
        active.blockEnabled = cfg.blockEnabled;
        active.blockZero = cfg.blockZero;
        return true;
    }
    return false;
}

// The block list is written on "save", only when it changed.
bool DnsServer::saveExtra()
{
    std::lock_guard<std::mutex> lock(mutex);
    if (!blockDirty) {
        return true;
    }
    std::string text;
    text.reserve(blockArena.size() + 64);
    text += "# Lista blokowanych domen (jedna w linii, blokuje tez poddomeny)\n";
    for (size_t i = 0; i < blockOffsets.size(); ++i) {
        text += blockName(i);
        text += '\n';
    }
    if (text.size() > BLOCK_FILE_LIMIT) {
        LOG_E("Lista blokad za duza (%u B) - nie zapisano", (unsigned)text.size());
        return false;
    }
    if (!store.saveText(BLOCK_FILE, text.data(), text.size())) {
        return false;
    }
    blockDirty = false;
    return true;
}

bool DnsServer::extraDirty() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return blockDirty;
}

// "config revert dns": unsaved changes of the block list – reload the file.
void DnsServer::revertExtra()
{
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(mutex);
        dirty = blockDirty;
    }
    if (dirty) {
        loadBlocklist();
    }
}

// ---- block list ----

void DnsServer::loadBlocklist()
{
    int64_t t0 = esp_timer_get_time();
    size_t len = 0;
    char* text = store.readText(BLOCK_FILE, &len);

    std::lock_guard<std::mutex> lock(mutex);
    std::vector<char>().swap(blockArena);
    std::vector<uint32_t>().swap(blockOffsets);
    blockDirty = false;
    if (text == nullptr) {
        blockHits.clear();
        LOG_I("Brak %s - lista blokad pusta", BLOCK_FILE);
        return;
    }

    blockArena.reserve(len + 1);
    blockOffsets.reserve(len / 12 + 1);
    char* line = text;
    while (line < text + len) {
        char* eol = strchr(line, '\n');
        if (eol == nullptr) {
            eol = text + len;
        }
        *eol = '\0';

        // Format: "domain" or hosts "0.0.0.0 domain"; comments from '#'.
        char* hash = strchr(line, '#');
        if (hash != nullptr) {
            *hash = '\0';
        }
        char* save = nullptr;
        char* t1 = strtok_r(line, " \t\r", &save);
        char* t2 = t1 ? strtok_r(nullptr, " \t\r", &save) : nullptr;
        char* name = t1;
        if (t1 && (strcmp(t1, "0.0.0.0") == 0 || strcmp(t1, "127.0.0.1") == 0 || strcmp(t1, "::") == 0 ||
                   strcmp(t1, "::1") == 0)) {
            name = t2;
        }
        if (name != nullptr) {
            toLower(name);
            size_t n = strlen(name);
            if (n > 0 && name[n - 1] == '.') {
                name[--n] = '\0';
            }
            if (validDnsName(name) && strcmp(name, "localhost") != 0 && strcmp(name, "localhost.localdomain") != 0 &&
                strcmp(name, "broadcasthost") != 0 && strcmp(name, "local") != 0) {
                blockOffsets.push_back(blockArena.size());
                blockArena.insert(blockArena.end(), name, name + n + 1);
            }
        }
        line = eol + 1;
    }
    free(text);

    const char* arena = blockArena.data();
    std::sort(blockOffsets.begin(), blockOffsets.end(),
              [arena](uint32_t a, uint32_t b) { return strcmp(arena + a, arena + b) < 0; });
    blockOffsets.erase(std::unique(blockOffsets.begin(), blockOffsets.end(),
                                   [arena](uint32_t a, uint32_t b) { return strcmp(arena + a, arena + b) == 0; }),
                       blockOffsets.end());
    blockHits.assign(blockOffsets.size(), 0);

    LOG_I("Lista blokad: %u domen, %u B, wczytana w %lu ms", (unsigned)blockOffsets.size(), (unsigned)blockArena.size(),
          (unsigned long)((esp_timer_get_time() - t0) / 1000));
}

int DnsServer::blockFind(const char* name) const
{
    const char* arena = blockArena.data();
    auto it = std::lower_bound(blockOffsets.begin(), blockOffsets.end(), name,
                               [arena](uint32_t off, const char* n) { return strcmp(arena + off, n) < 0; });
    if (it != blockOffsets.end() && strcmp(arena + *it, name) == 0) {
        return it - blockOffsets.begin();
    }
    return -1;
}

// Checks the name and every parent domain (blocking a domain covers its subdomains).
int DnsServer::blockedBy(const char* qname) const
{
    for (const char* p = qname; p != nullptr; p = strchr(p, '.')) {
        if (*p == '.') {
            ++p;
        }
        int i = blockFind(p);
        if (i >= 0) {
            return i;
        }
    }
    return -1;
}

bool DnsServer::blockInsert(const char* name)
{
    if (blockFind(name) >= 0) {
        return false;
    }
    uint32_t off = blockArena.size();
    blockArena.insert(blockArena.end(), name, name + strlen(name) + 1);
    const char* arena = blockArena.data();
    auto it = std::lower_bound(blockOffsets.begin(), blockOffsets.end(), name,
                               [arena](uint32_t o, const char* n) { return strcmp(arena + o, n) < 0; });
    size_t pos = it - blockOffsets.begin();
    blockOffsets.insert(it, off);
    blockHits.insert(blockHits.begin() + pos, 0);
    blockDirty = true;
    return true;
}

bool DnsServer::blockRemove(const char* name)
{
    int i = blockFind(name);
    if (i < 0) {
        return false;
    }
    // The name bytes stay in the buffer until the list is loaded again.
    blockOffsets.erase(blockOffsets.begin() + i);
    blockHits.erase(blockHits.begin() + i);
    blockDirty = true;
    return true;
}

// ---- service ----

esp_err_t DnsServer::start()
{
    if (running) {
        return ESP_OK;
    }
    WifiSettings w = wifi.settings();
    serverIp = w.apIp;
    netmask = w.apMask;
    active = cfg;

    // Cache and query table in PSRAM; without PSRAM a smaller cache in internal memory.
    bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    uint32_t caps = psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
    int slots = active.cacheSize;
    if (!psram && slots > NO_PSRAM_CACHE_LIMIT) {
        LOG_W("Brak PSRAM - cache ograniczony do %d wpisow", NO_PSRAM_CACHE_LIMIT);
        slots = NO_PSRAM_CACHE_LIMIT;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        cache = static_cast<CacheSlot*>(heap_caps_calloc(slots, sizeof(CacheSlot), caps));
        cacheSlots = cache ? slots : 0;
    }
    pending = static_cast<Pending*>(heap_caps_calloc(MAX_PENDING, sizeof(Pending), caps));
    if (cache == nullptr || pending == nullptr) {
        LOG_E("Brak pamieci na cache DNS");
        heap_caps_free(cache);
        heap_caps_free(pending);
        cache = nullptr;
        pending = nullptr;
        cacheSlots = 0;
        return ESP_ERR_NO_MEM;
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    int u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct ifreq ifr = {};
    esp_netif_get_netif_impl_name(wifi.apNetif(), ifr.ifr_name);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DNS_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || u < 0 || setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0 ||
        bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        bind(u, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        LOG_E("Nie udalo sie otworzyc gniazd DNS: errno %d", errno);
        if (s >= 0) {
            close(s);
        }
        if (u >= 0) {
            close(u);
        }
        stop();
        heap_caps_free(cache);
        heap_caps_free(pending);
        cache = nullptr;
        pending = nullptr;
        cacheSlots = 0;
        return ESP_FAIL;
    }
    sock = s;
    upstreamSock = u;

    stopRequested = false;
    taskAlive = true;
    if (xTaskCreate(&taskEntry, "dns", 8192, this, 5, nullptr) != pdPASS) {
        taskAlive = false;
        close(s);
        close(u);
        return ESP_ERR_NO_MEM;
    }
    running = true;
    LOG_I("DNS: domena %s, cache %d wpisow (%s), blokady %u (%s)", active.domain.c_str(), cacheSlots,
          psram ? "PSRAM" : "SRAM", (unsigned)blockOffsets.size(), active.blockEnabled ? "wlaczone" : "wylaczone");
    return ESP_OK;
}

esp_err_t DnsServer::stop()
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
    {
        std::lock_guard<std::mutex> lock(mutex);
        heap_caps_free(cache);
        cache = nullptr;
        cacheSlots = 0;
    }
    heap_caps_free(pending);
    pending = nullptr;
    running = false;
    LOG_I("DNS zatrzymany");
    return ESP_OK;
}

void DnsServer::describe(IOutput& out) const
{
    if (!running) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    int used = 0;
    for (int i = 0; i < cacheSlots; ++i) {
        used += cache[i].hash != 0;
    }
    out.printf(tr(" cache %d/%d, blokady %u%s", " cache %d/%d, blocked %u%s"), used, cacheSlots, (unsigned)blockOffsets.size(),
               active.blockEnabled ? "" : tr(" (wył.)", " (off)"));
}

void DnsServer::taskEntry(void* arg)
{
    auto* self = static_cast<DnsServer*>(arg);
    self->run();
    close(self->sock);
    close(self->upstreamSock);
    self->sock = -1;
    self->upstreamSock = -1;
    self->taskAlive = false;
    vTaskDelete(nullptr);
}

void DnsServer::run()
{
    uint8_t buf[1500];
    while (!stopRequested) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        FD_SET(upstreamSock, &fds);
        timeval tv = { 0, 200 * 1000 };
        if (select(std::max(sock, upstreamSock) + 1, &fds, nullptr, nullptr, &tv) > 0) {
            sockaddr_in from = {};
            socklen_t fromLen = sizeof(from);
            if (FD_ISSET(sock, &fds)) {
                int n = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n > 0) {
                    handleQuery(buf, n, from);
                }
            }
            fromLen = sizeof(from);
            if (FD_ISSET(upstreamSock, &fds)) {
                int n = recvfrom(upstreamSock, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
                if (n > 0) {
                    handleUpstream(buf, n, from);
                }
            }
        }
        checkTimeouts();
        maintenance();
    }
}

// Every cache_sweep_s: free expired entries. Every second: a change of the STA address or of the upstream
// DNS in auto mode clears the whole cache (another network = other answers).
void DnsServer::maintenance()
{
    int64_t now = esp_timer_get_time();
    if (now - lastNetCheckUs >= 1000000) {
        lastNetCheckUs = now;
        uint32_t staIp = 0;
        if (wifi.staConnected()) {
            esp_netif_ip_info_t info = {};
            esp_netif_get_ip_info(wifi.staNetif(), &info);
            staIp = info.ip.addr;
        }
        uint32_t autoDns = wifi.upstreamDns();
        std::lock_guard<std::mutex> lock(mutex);
        bool autoMode = upstream.empty();
        if ((staIp != 0 && staIp != lastStaIp) || (autoMode && autoDns != 0 && autoDns != lastAutoDns)) {
            if (lastStaIp != 0 || lastAutoDns != 0) {
                LOG_I("Zmiana sieci STA albo DNS nadrzednego - czyszcze cache");
            }
            cacheClear();
        }
        if (staIp != 0) {
            lastStaIp = staIp;
        }
        if (autoDns != 0) {
            lastAutoDns = autoDns;
        }
    }
    if (now - lastSweepUs >= static_cast<int64_t>(active.cacheSweepS) * 1000000) {
        lastSweepUs = now;
        std::lock_guard<std::mutex> lock(mutex);
        for (int i = 0; i < cacheSlots; ++i) {
            if (cache[i].hash != 0 && cache[i].expiresUs <= now) {
                cache[i].hash = 0;
            }
        }
    }
}

// ---- resolution ----

void DnsServer::upstreamServers(uint32_t servers[2], uint8_t* count) const
{
    *count = 0;
    if (upstream.empty()) {
        uint32_t dns = wifi.upstreamDns();
        if (dns != 0) {
            servers[(*count)++] = dns;
        }
        return;
    }
    for (uint32_t ip : upstream) {
        servers[(*count)++] = ip;
    }
}

// Order: static entries, router name, names from DHCP. shortName: the name without the local
// domain (or the whole single-label name), empty for external names.
DnsServer::Source DnsServer::resolveLocal(const char* qname, uint32_t* ip, std::string* shortName) const
{
    std::string name = qname;
    std::string suffix = "." + active.domain;
    shortName->clear();
    if (name.size() > suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) {
        *shortName = name.substr(0, name.size() - suffix.size());
    } else if (name.find('.') == std::string::npos) {
        *shortName = name;
    }

    for (const Entry& e : entries) {
        if (e.name == name || (!shortName->empty() && e.name == *shortName)) {
            *ip = e.ip;
            return Source::STATIC;
        }
    }
    if (!shortName->empty() && strcasecmp(global.values().hostname.c_str(), shortName->c_str()) == 0) {
        *ip = serverIp;
        return Source::ROUTER;
    }
    if (active.dhcpNames && !shortName->empty() && shortName->find('.') == std::string::npos) {
        bool reservation = false;
        if (dhcp.resolveName(shortName->c_str(), ip, &reservation)) {
            return reservation ? Source::RESERVATION : Source::LEASE;
        }
    }
    return Source::NONE;
}

bool DnsServer::resolvePtr(uint32_t ip, char* name, size_t size) const
{
    for (const Entry& e : entries) {
        if (e.ip == ip) {
            strlcpy(name, e.name.c_str(), size);
            if (strchr(name, '.') == nullptr) {
                strlcat(name, ".", size);
                strlcat(name, active.domain.c_str(), size);
            }
            return true;
        }
    }
    char host[40];
    if (ip == serverIp) {
        strlcpy(host, global.values().hostname.c_str(), sizeof(host));
    } else if (!active.dhcpNames || !dhcp.nameForIp(ip, host, sizeof(host))) {
        return false;
    }
    toLower(host);
    snprintf(name, size, "%s.%s", host, active.domain.c_str());
    return true;
}

void DnsServer::sendToClient(const uint8_t* buf, int len, const sockaddr_in& client)
{
    sendto(sock, buf, len, 0, reinterpret_cast<const sockaddr*>(&client), sizeof(client));
}

void DnsServer::sendError(const uint8_t* query, int qend, uint8_t rcode, const sockaddr_in& client)
{
    uint8_t out[300];
    if (qend > static_cast<int>(sizeof(out))) {
        qend = 12;
    }
    int n = startResponse(out, query, qend, rcode, false);
    if (rcode == RCODE_SERVFAIL) {
        stats.servfail++;
    } else if (rcode == RCODE_NXDOMAIN) {
        stats.nxdomain++;
    }
    sendToClient(out, n, client);
}

void DnsServer::handleQuery(const uint8_t* buf, int len, const sockaddr_in& client)
{
    if (len < 12 || (buf[2] & 0x80)) {
        return; // too short or it is a response
    }
    stats.queries++;
    if (((buf[2] >> 3) & 0x0F) != 0) {
        sendError(buf, 12, RCODE_NOTIMP, client);
        return;
    }
    char qname[256];
    uint16_t qtype = 0;
    int qend = 0;
    if (rd16(buf + 4) != 1 || !parseQuestion(buf, len, qname, sizeof(qname), &qtype, &qend)) {
        sendError(buf, 12, RCODE_FORMERR, client);
        return;
    }
    toLower(qname);

    uint8_t out[512];
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);

        // PTR for the AP subnet – always local.
        unsigned a, b, c, d;
        char tail[32];
        if (qtype == TYPE_PTR && sscanf(qname, "%u.%u.%u.%u.%31s", &d, &c, &b, &a, tail) == 5 &&
            strcmp(tail, "in-addr.arpa") == 0 && a < 256 && b < 256 && c < 256 && d < 256) {
            uint32_t ip = htonl((a << 24) | (b << 16) | (c << 8) | d);
            if (((ip ^ serverIp) & netmask) == 0) {
                char name[160];
                stats.local++;
                if (!resolvePtr(ip, name, sizeof(name))) {
                    sendError(buf, qend, RCODE_NXDOMAIN, client);
                    return;
                }
                uint8_t rdata[170];
                int rlen = encodeName(name, rdata);
                n = startResponse(out, buf, qend, RCODE_OK, true);
                n = appendRecord(out, n, TYPE_PTR, active.localTtl, rdata, rlen);
                sendToClient(out, n, client);
                return;
            }
        }

        uint32_t ip = 0;
        std::string shortName;
        Source src = resolveLocal(qname, &ip, &shortName);

        // Blocking after static entries and the router name, before DHCP names and upstream.
        int blocked = (src == Source::STATIC || src == Source::ROUTER || !active.blockEnabled) ? -1 : blockedBy(qname);
        if (blocked >= 0) {
            blockHits[blocked]++;
            stats.blocked++;
            if (!active.blockZero) {
                sendError(buf, qend, RCODE_NXDOMAIN, client);
                return;
            }
            static const uint8_t zero[16] = {};
            n = startResponse(out, buf, qend, RCODE_OK, true);
            if (qtype == TYPE_A || qtype == TYPE_ANY) {
                n = appendRecord(out, n, TYPE_A, active.localTtl, zero, 4);
            } else if (qtype == TYPE_AAAA) {
                n = appendRecord(out, n, TYPE_AAAA, active.localTtl, zero, 16);
            }
            sendToClient(out, n, client);
            return;
        }

        if (src != Source::NONE) {
            stats.local++;
            n = startResponse(out, buf, qend, RCODE_OK, true);
            if (qtype == TYPE_A || qtype == TYPE_ANY) {
                uint8_t rdata[4];
                memcpy(rdata, &ip, 4);
                n = appendRecord(out, n, TYPE_A, active.localTtl, rdata, 4);
            } // AAAA and other types for local names: answer without data
            sendToClient(out, n, client);
            return;
        }

        // Unknown name in the local domain (or a single-label one) – does not go outside.
        if (!shortName.empty() || qname[0] == '\0') {
            stats.local++;
            sendError(buf, qend, RCODE_NXDOMAIN, client);
            return;
        }

        CacheSlot* slot = cacheFind(qname, qtype);
        if (slot != nullptr) {
            stats.cacheHits++;
            n = slot->len;
            memcpy(out, slot->pkt, n);
            memcpy(out, buf, 2); // client's query ID
        }
    }
    if (n > 0) {
        sendToClient(out, n, client);
        return;
    }
    forward(buf, len, client, qname, qtype);
}

void DnsServer::forward(const uint8_t* buf, int len, const sockaddr_in& client, const char* qname, uint16_t qtype)
{
    int qend = 12;
    char tmp[256];
    uint16_t t;
    parseQuestion(buf, len, tmp, sizeof(tmp), &t, &qend);

    uint32_t servers[2];
    uint8_t count = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        upstreamServers(servers, &count);
    }
    if (count == 0 || !wifi.staConnected() || len > static_cast<int>(sizeof(Pending::query))) {
        sendError(buf, qend, RCODE_SERVFAIL, client);
        return;
    }

    Pending* p = nullptr;
    for (int i = 0; i < MAX_PENDING && p == nullptr; ++i) {
        if (!pending[i].used) {
            p = &pending[i];
        }
    }
    if (p == nullptr) {
        LOG_W("Za duzo zapytan w toku");
        sendError(buf, qend, RCODE_SERVFAIL, client);
        return;
    }

    // New ID, so that queries of different clients do not get mixed up.
    uint16_t id;
    bool clash;
    do {
        id = static_cast<uint16_t>(esp_random());
        clash = false;
        for (int i = 0; i < MAX_PENDING; ++i) {
            clash = clash || (pending[i].used && pending[i].upstreamId == id);
        }
    } while (clash);

    p->used = true;
    p->clientId = rd16(buf);
    p->upstreamId = id;
    p->client = client;
    p->sentUs = esp_timer_get_time();
    p->serverIdx = 0;
    p->serverCount = count;
    memcpy(p->servers, servers, sizeof(servers));
    p->qtype = qtype;
    strlcpy(p->name, qname, sizeof(p->name));
    p->len = len;
    memcpy(p->query, buf, len);
    wr16(p->query, id);

    sockaddr_in dst = {};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DNS_PORT);
    dst.sin_addr.s_addr = servers[0];
    sendto(upstreamSock, p->query, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    stats.forwarded++;
}

void DnsServer::handleUpstream(const uint8_t* buf, int len, const sockaddr_in& from)
{
    if (len < 12 || ntohs(from.sin_port) != DNS_PORT) {
        return;
    }
    uint16_t id = rd16(buf);
    Pending* p = nullptr;
    for (int i = 0; i < MAX_PENDING; ++i) {
        if (pending[i].used && pending[i].upstreamId == id) {
            p = &pending[i];
        }
    }
    if (p == nullptr || (from.sin_addr.s_addr != p->servers[0] && from.sin_addr.s_addr != p->servers[1])) {
        return;
    }

    int idx = from.sin_addr.s_addr == p->servers[1] && p->serverCount == 2 ? 1 : 0;
    upstreamAnswers[idx]++;
    upstreamTimeUs[idx] += esp_timer_get_time() - p->sentUs;

    uint8_t rcode = buf[3] & 0x0F;
    bool truncated = buf[2] & 0x02;
    if (rcode == RCODE_NXDOMAIN) {
        stats.nxdomain++;
    }
    if (!truncated && (rcode == RCODE_OK || rcode == RCODE_NXDOMAIN) && len <= static_cast<int>(sizeof(CacheSlot::pkt))) {
        std::lock_guard<std::mutex> lock(mutex);
        cacheStore(p->name, p->qtype, buf, len);
    }

    // Answer to the client with its ID (the receive buffer is read-only – copy of the header).
    uint8_t hdr[2];
    wr16(hdr, p->clientId);
    uint8_t* out = static_cast<uint8_t*>(malloc(len));
    if (out != nullptr) {
        memcpy(out, buf, len);
        memcpy(out, hdr, 2);
        sendToClient(out, len, p->client);
        free(out);
    }
    p->used = false;
}

void DnsServer::checkTimeouts()
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < MAX_PENDING; ++i) {
        Pending& p = pending[i];
        if (!p.used || now - p.sentUs < UPSTREAM_TIMEOUT_US) {
            continue;
        }
        upstreamTimeouts[p.serverIdx]++;
        if (p.serverIdx == 0 && p.serverCount == 2) {
            // The second server gets another 2 s.
            p.serverIdx = 1;
            p.sentUs = now;
            sockaddr_in dst = {};
            dst.sin_family = AF_INET;
            dst.sin_port = htons(DNS_PORT);
            dst.sin_addr.s_addr = p.servers[1];
            sendto(upstreamSock, p.query, p.len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
            continue;
        }
        stats.timeouts++;
        char tmp[256];
        uint16_t t;
        int qend = 12;
        parseQuestion(p.query, p.len, tmp, sizeof(tmp), &t, &qend);
        wr16(p.query, p.clientId);
        sendError(p.query, qend, RCODE_SERVFAIL, p.client);
        p.used = false;
    }
}

// ---- cache ----

DnsServer::CacheSlot* DnsServer::cacheFind(const char* name, uint16_t qtype)
{
    uint32_t h = nameHash(name, qtype);
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < cacheSlots; ++i) {
        CacheSlot& s = cache[i];
        if (s.hash == h && s.qtype == qtype && strcmp(s.name, name) == 0) {
            if (s.expiresUs <= now) {
                s.hash = 0;
                return nullptr;
            }
            s.lastUsedUs = now;
            return &s;
        }
    }
    return nullptr;
}

// Store in the cache; when full, the least recently used entry is evicted (LRU).
void DnsServer::cacheStore(const char* name, uint16_t qtype, const uint8_t* pkt, int len)
{
    if (cache == nullptr || strlen(name) >= sizeof(CacheSlot::name)) {
        return;
    }
    uint32_t h = nameHash(name, qtype);
    int64_t now = esp_timer_get_time();
    CacheSlot* target = nullptr; // the same key > empty/expired > least recently used
    CacheSlot* freeSlot = nullptr;
    CacheSlot* lru = nullptr;
    for (int i = 0; i < cacheSlots && target == nullptr; ++i) {
        CacheSlot& s = cache[i];
        if (s.hash == h && s.qtype == qtype && strcmp(s.name, name) == 0) {
            target = &s;
        } else if (s.hash == 0 || s.expiresUs <= now) {
            freeSlot = freeSlot ? freeSlot : &s;
        } else if (lru == nullptr || s.lastUsedUs < lru->lastUsedUs) {
            lru = &s;
        }
    }
    if (target == nullptr) {
        target = freeSlot ? freeSlot : lru;
    }
    target->hash = h;
    target->qtype = qtype;
    target->len = len;
    target->expiresUs = now + static_cast<int64_t>(responseTtl(pkt, len)) * 1000000;
    target->lastUsedUs = now;
    strlcpy(target->name, name, sizeof(target->name));
    memcpy(target->pkt, pkt, len);
}

void DnsServer::cacheClear()
{
    for (int i = 0; i < cacheSlots; ++i) {
        cache[i].hash = 0;
    }
}

// ---- commands ----

void DnsServer::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("dns", { "serwer DNS (nazwy lokalne, cache, blokady)", "DNS server (local names, cache, blocking)" });
    exec.add("dns", { "list", "", { "nazwy lokalne ze źródłem", "local names with their source" }, &cmdList, this });
    exec.add("dns", { "add", "<nazwa> <ip>", { "wpis statyczny (trwałe po save)", "static entry (permanent after save)" }, &cmdAdd, this });
    exec.add("dns", { "del", "<nazwa>", { "usuwa wpis statyczny (trwałe po save)", "deletes a static entry (permanent after save)" }, &cmdDel, this });
    exec.add("dns", { "upstream", "[auto | <ip> [ip2]]", { "DNS nadrzędny: podgląd albo zmiana (trwałe po save)", "upstream DNS: show or change (permanent after save)" },
                      &cmdUpstream, this });
    exec.add("dns", { "lookup", "<nazwa>", { "skąd przyjdzie odpowiedź na nazwę", "where the answer for a name will come from" }, &cmdLookup, this });
    exec.add("dns", { "cache", "[clear]", { "zajętość cache albo wyczyszczenie", "cache usage or clearing it" }, &cmdCache, this });
    exec.add("dns", { "stats", "", { "statystyki zapytań", "query statistics" }, &cmdStats, this });
    exec.addGroup("dns block", { "blokowanie domen (/config/dns_block.txt)", "domain blocking (/config/dns_block.txt)" });
    exec.add("dns block", { "add", "<domena>", { "blokuje domenę i poddomeny (trwałe po save)", "blocks a domain and its subdomains (permanent after save)" }, &cmdBlockAdd, this });
    exec.add("dns block", { "del", "<domena>", { "usuwa domenę z listy (trwałe po save)", "removes a domain from the list (permanent after save)" }, &cmdBlockDel, this });
    exec.add("dns block", { "list", "[fraza]", { "lista (max 50 pozycji)", "list (max 50 items)" }, &cmdBlockList, this });
    exec.add("dns block", { "check", "<nazwa>", { "czy nazwa jest blokowana i przez który wpis", "whether a name is blocked and by which entry" }, &cmdBlockCheck, this });
    exec.add("dns block", { "on", "", { "włącza blokowanie (trwałe po save)", "enables blocking (permanent after save)" }, &cmdBlockOn, this });
    exec.add("dns block", { "off", "", { "wyłącza blokowanie (trwałe po save)", "disables blocking (permanent after save)" }, &cmdBlockOff, this });
    exec.add("dns block", { "reload", "", { "wczytuje listę z pliku (porzuca niezapisane zmiany)", "reloads the list from file (discards unsaved changes)" }, &cmdBlockReload, this });
    exec.add("dns block", { "stats", "", { "liczba blokad i najczęściej blokowane domeny", "block count and most blocked domains" }, &cmdBlockStats, this });
}

// Source name of a local answer in the current language (index = Source).
static const char* sourceName(int src)
{
    switch (src) {
    case 1:
        return tr("statyczny", "static");
    case 2:
        return "router";
    case 3:
        return tr("rezerwacja DHCP", "DHCP reservation");
    case 4:
        return tr("dzierżawa DHCP", "DHCP lease");
    default:
        return "-";
    }
}

CmdResult DnsServer::cmdList(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    const std::string& dom = self->active.domain.empty() ? self->cfg.domain : self->active.domain;
    out.printf(tr("  %-32s %-15s  źródło\r\n", "  %-32s %-15s  source\r\n"), tr("nazwa", "name"), "IP");
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        for (const Entry& e : self->entries) {
            esp_ip4_addr_t a = { e.ip };
            char ip[16];
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
            out.printf("  %-32s %-15s  %s\r\n", e.name.c_str(), ip, sourceName(1));
        }
    }
    esp_ip4_addr_t ap = { self->wifi.settings().apIp };
    char name[80], ip[16];
    snprintf(name, sizeof(name), "%s.%s", self->global.values().hostname.c_str(), dom.c_str());
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap));
    out.printf("  %-32s %-15s  router\r\n", name, ip);
    if (self->cfg.dhcpNames) {
        for (const DhcpServer::NameEntry& n : self->dhcp.names()) {
            esp_ip4_addr_t a = { n.ip };
            snprintf(name, sizeof(name), "%s.%s", n.name.c_str(), dom.c_str());
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
            out.printf("  %-32s %-15s  %s\r\n", name, ip, sourceName(n.reservation ? 3 : 4));
        }
    }
    return CmdResult::OK;
}

CmdResult DnsServer::cmdAdd(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 3) {
        return CmdResult::ERR_ARGS;
    }
    toLower(argv[1]);
    std::string name = argv[1];
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    esp_ip4_addr_t ip = {};
    if (!validDnsName(name) || esp_netif_str_to_ip4(argv[2], &ip) != ESP_OK) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    for (Entry& e : self->entries) {
        if (e.name == name) {
            e.ip = ip.addr;
            out.print(tr("Zmieniono wpis. Trwałe po 'save'\r\n", "Entry changed. Permanent after 'save'\r\n"));
            return CmdResult::OK;
        }
    }
    if (self->entries.size() >= MAX_ENTRIES) {
        out.print(tr("Limit 64 wpisów\r\n", "Limit of 64 entries\r\n"));
        return CmdResult::ERR_FAIL;
    }
    self->entries.push_back({ name, ip.addr });
    out.print(tr("Dodano wpis. Działa od razu; trwałe po 'save'\r\n", "Entry added. Applied now; permanent after 'save'\r\n"));
    return CmdResult::OK;
}

CmdResult DnsServer::cmdDel(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    toLower(argv[1]);
    std::lock_guard<std::mutex> lock(self->mutex);
    for (size_t i = 0; i < self->entries.size(); ++i) {
        if (self->entries[i].name == argv[1]) {
            self->entries.erase(self->entries.begin() + i);
            out.print(tr("Usunięto wpis. Trwałe po 'save'\r\n", "Entry deleted. Permanent after 'save'\r\n"));
            return CmdResult::OK;
        }
    }
    out.print(tr("Nie ma takiego wpisu\r\n", "No such entry\r\n"));
    return CmdResult::ERR_FAIL;
}

CmdResult DnsServer::cmdUpstream(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc > 3) {
        return CmdResult::ERR_ARGS;
    }
    if (argc >= 2) {
        std::vector<uint32_t> list;
        if (strcmp(argv[1], "auto") != 0 || argc == 3) {
            for (int i = 1; i < argc; ++i) {
                esp_ip4_addr_t ip = {};
                if (esp_netif_str_to_ip4(argv[i], &ip) != ESP_OK) {
                    return CmdResult::ERR_ARGS;
                }
                list.push_back(ip.addr);
            }
        }
        std::lock_guard<std::mutex> lock(self->mutex);
        self->upstream = list;
        self->cacheClear(); // answers of another server
        memset(self->upstreamAnswers, 0, sizeof(self->upstreamAnswers));
        memset(self->upstreamTimeouts, 0, sizeof(self->upstreamTimeouts));
        memset(self->upstreamTimeUs, 0, sizeof(self->upstreamTimeUs));
        out.print(tr("Zmieniono DNS nadrzędny. Działa od razu; trwałe po 'save'\r\n", "Upstream DNS changed. Applied now; permanent after 'save'\r\n"));
    }

    std::lock_guard<std::mutex> lock(self->mutex);
    uint32_t servers[2];
    uint8_t count = 0;
    self->upstreamServers(servers, &count);
    out.printf(tr("Tryb: %s\r\n", "Mode: %s\r\n"), self->upstream.empty() ? tr("auto (DNS z DHCP sieci nadrzędnej)", "auto (DNS from the upstream network's DHCP)")
                                                                     : tr("stałe serwery", "fixed servers"));
    if (count == 0) {
        out.print(tr("  brak serwera (STA bez IP albo bez DNS)\r\n", "  no server (STA without IP or DNS)\r\n"));
    }
    for (int i = 0; i < count; ++i) {
        esp_ip4_addr_t a = { servers[i] };
        uint32_t answers = self->upstreamAnswers[i];
        out.printf(tr("  %d: " IPSTR "  odpowiedzi %lu, timeouty %lu, średnio %lu ms\r\n",
                      "  %d: " IPSTR "  answers %lu, timeouts %lu, average %lu ms\r\n"),
                   i + 1, IP2STR(&a),
                   (unsigned long)answers, (unsigned long)self->upstreamTimeouts[i],
                   answers ? (unsigned long)(self->upstreamTimeUs[i] / answers / 1000) : 0UL);
    }
    return CmdResult::OK;
}

CmdResult DnsServer::runLookup(const char* name, IOutput& out)
{
    uint32_t ip = 0;
    std::string shortName;
    std::lock_guard<std::mutex> lock(mutex);
    Settings saved = active;
    if (!running) {
        active = cfg; // preview also with the service stopped
        serverIp = wifi.settings().apIp;
    }
    Source src = resolveLocal(name, &ip, &shortName);
    int blocked = (src == Source::STATIC || src == Source::ROUTER || !active.blockEnabled) ? -1 : blockedBy(name);
    esp_ip4_addr_t a = { ip };
    if (blocked >= 0) {
        out.printf(tr("%s: ZABLOKOWANA przez wpis '%s' (odpowiedź %s)\r\n", "%s: BLOCKED by entry '%s' (answer %s)\r\n"), name,
                   blockName(blocked),
                   active.blockZero ? "0.0.0.0" : "NXDOMAIN");
    } else if (src != Source::NONE) {
        out.printf(tr("%s: " IPSTR " (lokalnie, %s)\r\n", "%s: " IPSTR " (local, %s)\r\n"), name, IP2STR(&a),
                   sourceName(static_cast<int>(src)));
    } else if (!shortName.empty()) {
        out.printf(tr("%s: NXDOMAIN (nieznana nazwa w domenie lokalnej)\r\n", "%s: NXDOMAIN (unknown name in the local domain)\r\n"), name);
    } else if (running && (cacheFind(name, TYPE_A) != nullptr)) {
        out.printf(tr("%s: w cache (typ A)\r\n", "%s: in cache (type A)\r\n"), name);
    } else {
        uint32_t servers[2];
        uint8_t count = 0;
        upstreamServers(servers, &count);
        esp_ip4_addr_t s = { count ? servers[0] : 0 };
        out.printf(tr("%s: brak lokalnie i w cache – zapytanie trafi do " IPSTR "\r\n",
                      "%s: not local and not in cache – the query goes to " IPSTR "\r\n"),
                   name, IP2STR(&s));
    }
    active = saved;
    return CmdResult::OK;
}

CmdResult DnsServer::cmdLookup(void* ctx, int argc, char** argv, IOutput& out)
{
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    toLower(argv[1]);
    return static_cast<DnsServer*>(ctx)->runLookup(argv[1], out);
}

CmdResult DnsServer::cmdCache(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "clear") != 0)) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    if (argc == 2) {
        self->cacheClear();
        out.print(tr("Cache wyczyszczony\r\n", "Cache cleared\r\n"));
        return CmdResult::OK;
    }
    int used = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < self->cacheSlots; ++i) {
        used += self->cache[i].hash != 0 && self->cache[i].expiresUs > now;
    }
    out.printf(tr("Cache: %d / %d wpisów (%u B na wpis), trafienia %lu, czyszczenie wygasłych co %lu s\r\n", "Cache: %d / %d entries (%u B per entry), hits %lu, purging expired every %lu s\r\n"), used,
               self->cacheSlots, (unsigned)sizeof(CacheSlot), (unsigned long)self->stats.cacheHits,
               (unsigned long)(self->running ? self->active.cacheSweepS : self->cfg.cacheSweepS));
    return CmdResult::OK;
}

CmdResult DnsServer::cmdStats(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    const Stats& s = self->stats;
    out.printf(tr("Zapytania: %lu\r\n  lokalne %lu, zablokowane %lu, z cache %lu, przekazane %lu\r\n"
                  "  timeouty %lu, NXDOMAIN %lu, SERVFAIL %lu\r\n",
                  "Queries: %lu\r\n  local %lu, blocked %lu, from cache %lu, forwarded %lu\r\n"
                  "  timeouts %lu, NXDOMAIN %lu, SERVFAIL %lu\r\n"),
               (unsigned long)s.queries, (unsigned long)s.local, (unsigned long)s.blocked, (unsigned long)s.cacheHits,
               (unsigned long)s.forwarded, (unsigned long)s.timeouts, (unsigned long)s.nxdomain,
               (unsigned long)s.servfail);
    out.printf(tr("Lista blokad: %u domen (%s)\r\n", "Block list: %u domains (%s)\r\n"), (unsigned)self->blockOffsets.size(),
               self->cfg.blockEnabled ? tr("włączona", "enabled") : tr("wyłączona", "disabled"));
    return CmdResult::OK;
}

// Lower case, without a trailing dot; returns whether the result is a valid domain name.
static bool normalizeDomain(char* s)
{
    toLower(s);
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '.') {
        s[n - 1] = '\0';
    }
    return validDnsName(s);
}

CmdResult DnsServer::cmdBlockAdd(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 2 || !normalizeDomain(argv[1])) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    if (!self->blockInsert(argv[1])) {
        out.print(tr("Domena już jest na liście\r\n", "The domain is already on the list\r\n"));
        return CmdResult::OK;
    }
    out.printf(tr("Zablokowano %s (z poddomenami). Trwałe po 'save'\r\n", "Blocked %s (with subdomains). Permanent after 'save'\r\n"), argv[1]);
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockDel(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 2 || !normalizeDomain(argv[1])) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    if (!self->blockRemove(argv[1])) {
        out.print(tr("Nie ma takiej domeny na liście\r\n", "No such domain on the list\r\n"));
        return CmdResult::ERR_FAIL;
    }
    out.print(tr("Usunięto z listy. Trwałe po 'save'\r\n", "Removed from the list. Permanent after 'save'\r\n"));
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockList(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    const char* filter = argc == 2 ? argv[1] : "";
    std::lock_guard<std::mutex> lock(self->mutex);
    unsigned matches = 0;
    for (size_t i = 0; i < self->blockOffsets.size(); ++i) {
        const char* name = self->blockName(i);
        if (filter[0] != '\0' && strstr(name, filter) == nullptr) {
            continue;
        }
        if (++matches <= 50) {
            out.printf("  %s\r\n", name);
        }
    }
    out.printf(tr("Pasujących: %u z %u%s\r\n", "Matching: %u of %u%s\r\n"), matches, (unsigned)self->blockOffsets.size(),
               matches > 50 ? tr(" (pokazano 50)", " (50 shown)") : "");
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockCheck(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 2 || !normalizeDomain(argv[1])) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    int i = self->blockedBy(argv[1]);
    if (i < 0) {
        out.printf(tr("%s nie jest blokowana\r\n", "%s is not blocked\r\n"), argv[1]);
    } else {
        out.printf(tr("%s jest blokowana przez wpis '%s'%s\r\n", "%s is blocked by entry '%s'%s\r\n"), argv[1], self->blockName(i),
                   self->cfg.blockEnabled ? "" : tr(" (blokowanie wyłączone)", " (blocking disabled)"));
    }
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockOn(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    self->cfg.blockEnabled = self->active.blockEnabled = true;
    out.print(tr("Blokowanie włączone. Trwałe po 'save'\r\n", "Blocking enabled. Permanent after 'save'\r\n"));
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockOff(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    self->cfg.blockEnabled = self->active.blockEnabled = false;
    out.print(tr("Blokowanie wyłączone. Trwałe po 'save'\r\n", "Blocking disabled. Permanent after 'save'\r\n"));
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockReload(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    self->loadBlocklist();
    std::lock_guard<std::mutex> lock(self->mutex);
    out.printf(tr("Wczytano domen: %u\r\n", "Loaded %u domains\r\n"), (unsigned)self->blockOffsets.size());
    return CmdResult::OK;
}

CmdResult DnsServer::cmdBlockStats(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<DnsServer*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    out.printf(tr("Zablokowane zapytania: %lu, domen na liście: %u\r\n", "Blocked queries: %lu, domains on the list: %u\r\n"), (unsigned long)self->stats.blocked,
               (unsigned)self->blockOffsets.size());

    // 10 most blocked (partial selection, without copying the list).
    int top[10];
    int count = 0;
    for (size_t i = 0; i < self->blockHits.size(); ++i) {
        if (self->blockHits[i] == 0) {
            continue;
        }
        int pos = count < 10 ? count++ : 9;
        if (pos == 9 && count == 10 && self->blockHits[i] <= self->blockHits[top[9]]) {
            continue;
        }
        top[pos] = i;
        while (pos > 0 && self->blockHits[top[pos]] > self->blockHits[top[pos - 1]]) {
            std::swap(top[pos], top[pos - 1]);
            --pos;
        }
    }
    for (int i = 0; i < count; ++i) {
        out.printf("  %6lu  %s\r\n", (unsigned long)self->blockHits[top[i]], self->blockName(top[i]));
    }
    return CmdResult::OK;
}

// ---- web panel ----

cJSON* DnsServer::namesJson() const
{
    cJSON* root = cJSON_CreateObject();
    const std::string& dom = active.domain.empty() ? cfg.domain : active.domain;
    cJSON_AddStringToObject(root, "domain", dom.c_str());
    cJSON* list = cJSON_AddArrayToObject(root, "names");
    char ip[16];
    auto add = [&](const std::string& name, uint32_t addr, const char* source) {
        cJSON* o = cJSON_CreateObject();
        esp_ip4_addr_t a = { addr };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        cJSON_AddStringToObject(o, "name", name.c_str());
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddStringToObject(o, "source", source);
        cJSON_AddItemToArray(list, o);
    };
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Entry& e : entries) {
            add(e.name, e.ip, "statyczny");
        }
    }
    add(global.values().hostname + "." + dom, wifi.settings().apIp, "router");
    if (cfg.dhcpNames) {
        for (const DhcpServer::NameEntry& n : dhcp.names()) {
            add(n.name + "." + dom, n.ip, n.reservation ? "rezerwacja" : "dzierzawa");
        }
    }
    cJSON_AddNumberToObject(root, "max_static", MAX_ENTRIES);
    return root;
}

cJSON* DnsServer::statsJson() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", running);
    const Stats& s = stats;
    cJSON* q = cJSON_AddObjectToObject(root, "queries");
    cJSON_AddNumberToObject(q, "total", s.queries);
    cJSON_AddNumberToObject(q, "local", s.local);
    cJSON_AddNumberToObject(q, "blocked", s.blocked);
    cJSON_AddNumberToObject(q, "cache", s.cacheHits);
    cJSON_AddNumberToObject(q, "forwarded", s.forwarded);
    cJSON_AddNumberToObject(q, "timeouts", s.timeouts);
    cJSON_AddNumberToObject(q, "nxdomain", s.nxdomain);
    cJSON_AddNumberToObject(q, "servfail", s.servfail);

    std::lock_guard<std::mutex> lock(mutex);
    int used = 0;
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < cacheSlots; ++i) {
        used += cache[i].hash != 0 && cache[i].expiresUs > now;
    }
    cJSON* c = cJSON_AddObjectToObject(root, "cache");
    cJSON_AddNumberToObject(c, "used", used);
    cJSON_AddNumberToObject(c, "size", cacheSlots);

    uint32_t servers[2];
    uint8_t count = 0;
    upstreamServers(servers, &count);
    cJSON* up = cJSON_AddObjectToObject(root, "upstream");
    cJSON_AddBoolToObject(up, "auto", upstream.empty());
    cJSON* list = cJSON_AddArrayToObject(up, "servers");
    for (int i = 0; i < count; ++i) {
        char ip[16];
        esp_ip4_addr_t a = { servers[i] };
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddNumberToObject(o, "answers", upstreamAnswers[i]);
        cJSON_AddNumberToObject(o, "timeouts", upstreamTimeouts[i]);
        cJSON_AddNumberToObject(o, "avg_ms",
                                upstreamAnswers[i] ? static_cast<double>(upstreamTimeUs[i] / upstreamAnswers[i] / 1000) : 0);
        cJSON_AddItemToArray(list, o);
    }

    cJSON* b = cJSON_AddObjectToObject(root, "block");
    cJSON_AddBoolToObject(b, "enabled", cfg.blockEnabled);
    cJSON_AddStringToObject(b, "response", cfg.blockZero ? "zero" : "nxdomain");
    cJSON_AddNumberToObject(b, "domains", blockOffsets.size());
    cJSON_AddBoolToObject(b, "dirty", blockDirty);
    // 10 most blocked
    std::vector<int> idx;
    for (size_t i = 0; i < blockHits.size(); ++i) {
        if (blockHits[i] > 0) {
            idx.push_back(i);
        }
    }
    std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(10, idx.size()), idx.end(),
                      [this](int a, int b) { return blockHits[a] > blockHits[b]; });
    cJSON* top = cJSON_AddArrayToObject(b, "top");
    for (size_t i = 0; i < std::min<size_t>(10, idx.size()); ++i) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "domain", blockName(idx[i]));
        cJSON_AddNumberToObject(o, "hits", blockHits[idx[i]]);
        cJSON_AddItemToArray(top, o);
    }
    return root;
}

cJSON* DnsServer::blockListJson(const char* filter, int offset, int limit) const
{
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_AddArrayToObject(root, "items");
    std::lock_guard<std::mutex> lock(mutex);
    int matches = 0;
    for (size_t i = 0; i < blockOffsets.size(); ++i) {
        const char* name = blockName(i);
        if (filter[0] != '\0' && strstr(name, filter) == nullptr) {
            continue;
        }
        if (matches >= offset && matches < offset + limit) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "domain", name);
            cJSON_AddNumberToObject(o, "hits", blockHits[i]);
            cJSON_AddItemToArray(items, o);
        }
        ++matches;
    }
    cJSON_AddNumberToObject(root, "matches", matches);
    cJSON_AddNumberToObject(root, "total", blockOffsets.size());
    cJSON_AddNumberToObject(root, "offset", offset);
    return root;
}

std::string DnsServer::blockListText() const
{
    std::lock_guard<std::mutex> lock(mutex);
    std::string text;
    text.reserve(blockArena.size() + 64);
    text += "# Lista blokowanych domen Esp32Router (jedna w linii, blokuje tez poddomeny)\n";
    for (size_t i = 0; i < blockOffsets.size(); ++i) {
        text += blockName(i);
        text += '\n';
    }
    return text;
}

void DnsServer::importBegin(bool replace)
{
    std::vector<char>().swap(importArena);
    std::vector<uint32_t>().swap(importOffsets);
    importCarry.clear();
    std::lock_guard<std::mutex> lock(mutex);
    importBefore = blockOffsets.size();
    if (!replace) {
        // Appending to the current list: copy of the current names.
        importArena.reserve(blockArena.size() + 64 * 1024);
        importOffsets.reserve(blockOffsets.size() + 4096);
        for (size_t i = 0; i < blockOffsets.size(); ++i) {
            const char* n = blockName(i);
            importOffsets.push_back(importArena.size());
            importArena.insert(importArena.end(), n, n + strlen(n) + 1);
        }
    }
}

// Line: "domain" or hosts "0.0.0.0 domain"; comments from '#'.
void DnsServer::importLine(char* line)
{
    char* hash = strchr(line, '#');
    if (hash != nullptr) {
        *hash = '\0';
    }
    char* save = nullptr;
    char* t1 = strtok_r(line, " \t\r", &save);
    char* t2 = t1 ? strtok_r(nullptr, " \t\r", &save) : nullptr;
    char* name = t1;
    if (t1 && (strcmp(t1, "0.0.0.0") == 0 || strcmp(t1, "127.0.0.1") == 0 || strcmp(t1, "::") == 0 ||
               strcmp(t1, "::1") == 0)) {
        name = t2;
    }
    if (name == nullptr) {
        return;
    }
    toLower(name);
    size_t n = strlen(name);
    if (n > 0 && name[n - 1] == '.') {
        name[--n] = '\0';
    }
    if (validDnsName(name) && strcmp(name, "localhost") != 0 && strcmp(name, "localhost.localdomain") != 0 &&
        strcmp(name, "broadcasthost") != 0 && strcmp(name, "local") != 0) {
        importOffsets.push_back(importArena.size());
        importArena.insert(importArena.end(), name, name + n + 1);
    }
}

void DnsServer::importChunk(const char* data, size_t len)
{
    size_t start = 0;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != '\n') {
            continue;
        }
        importCarry.append(data + start, i - start);
        if (importCarry.size() < 512) {
            importLine(&importCarry[0]);
        }
        importCarry.clear();
        start = i + 1;
    }
    importCarry.append(data + start, len - start);
    if (importCarry.size() > 512) {
        importCarry.clear(); // line too long – skipped
    }
}

int DnsServer::importEnd(int* added)
{
    if (!importCarry.empty()) {
        importLine(&importCarry[0]);
        importCarry.clear();
    }
    const char* arena = importArena.data();
    std::sort(importOffsets.begin(), importOffsets.end(),
              [arena](uint32_t a, uint32_t b) { return strcmp(arena + a, arena + b) < 0; });
    importOffsets.erase(std::unique(importOffsets.begin(), importOffsets.end(),
                                    [arena](uint32_t a, uint32_t b) { return strcmp(arena + a, arena + b) == 0; }),
                        importOffsets.end());

    std::lock_guard<std::mutex> lock(mutex);
    blockArena.swap(importArena);
    blockOffsets.swap(importOffsets);
    blockHits.assign(blockOffsets.size(), 0);
    blockDirty = true;
    std::vector<char>().swap(importArena);
    std::vector<uint32_t>().swap(importOffsets);
    *added = static_cast<int>(blockOffsets.size()) - static_cast<int>(importBefore);
    LOG_I("Import listy blokad: %u domen (zmiana %+d)", (unsigned)blockOffsets.size(), *added);
    return blockOffsets.size();
}
