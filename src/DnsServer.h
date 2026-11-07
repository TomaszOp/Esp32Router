/**
 * @file DnsServer.h
 * @brief DNS server on the AP interface: local names, domain blocking, cache, forwarding upstream.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "lwip/sockets.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "ConfigStore.h"
#include "DhcpServer.h"
#include "GlobalSettings.h"
#include "IService.h"
#include "WifiManager.h"

/**
 * @brief DNS server on the AP interface.
 *
 * Local names (static entries, router name, names from DHCP), domain blocking
 * (/config/dns_block.txt), answer cache and forwarding to the upstream DNS. Configuration in
 * /config/dns.json; service "dns" (ServiceManager). Queries are handled in its own task
 * (UDP 53 bound to the AP interface plus a separate socket to the upstream servers); entries,
 * upstream, block list and cache are guarded by a mutex (DNS task vs. consoles and the panel).
 */
class DnsServer : public IConfigurable, public IService {
public:
    static constexpr int MAX_ENTRIES = 64;                     ///< Maximum number of static entries.
    static constexpr const char* BLOCK_FILE = "dns_block.txt"; ///< Block list file in /config.

    DnsServer(WifiManager& wifi, DhcpServer& dhcp, const GlobalSettings& global, ConfigStore& store);

    const char* configName() const override { return "dns"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Applies entries, upstream, TTL, DHCP names, blocking and sweep interval immediately (domain and cache size after restart).
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string& path) const override
    {
        return path == "entries" || path == "upstream" || path == "local_ttl" || path == "dhcp_names" ||
               path == "block.enabled" || path == "block.response" || path == "cache_sweep_s";
    }
    /// Writes dns_block.txt when the list changed.
    bool saveExtra() override;
    /// Reloads the block list from the file.
    void revertExtra() override;
    /// Whether the block list has unsaved changes.
    bool extraDirty() const override;

    const char* name() const override { return "dns"; }
    /// Copies the configuration, allocates the cache (PSRAM when available), opens sockets, starts the task.
    esp_err_t start() override;
    /// Stops the task and frees the cache.
    esp_err_t stop() override;
    bool isRunning() const override { return running; }
    /// " cache used/size, blocked N" for "service list".
    void describe(IOutput& out) const override;

    /// @brief Loads the block list from the file (at start-up and on "dns block reload").
    void loadBlocklist();

    /// @name Web panel (JSON freed by the caller)
    /// @{
    /// Local names with source.
    cJSON* namesJson() const;
    /// Query statistics, cache, upstream servers, block list summary and most blocked domains.
    cJSON* statsJson() const;
    /// Page of the block list filtered by a fragment.
    cJSON* blockListJson(const char* filter, int offset, int limit) const;
    /// @}
    /// @return Whole block list as text, one domain per line (export).
    std::string blockListText() const;

    /**
     * @brief Streaming import of the block list (hosts format or one domain per line).
     *
     * The new list is built aside and swapped in importEnd(), so DNS keeps working meanwhile.
     * Called only from the HTTP server task.
     * @param replace true = replace the list, false = append.
     */
    void importBegin(bool replace);
    /// @brief Processes the next piece of the file (lines may span pieces).
    void importChunk(const char* data, size_t len);
    /// @brief Finishes the import and swaps the list. @return Number of domains after import; *added receives the change.
    int importEnd(int* added);
    /// @brief Registers the "dns" and "dns block" command groups.
    void registerCommands(CommandExecutor& exec);

private:
    /// Static entry (dns.json "entries").
    struct Entry {
        std::string name; ///< Lower case, no trailing dot.
        uint32_t ip;      ///< Network byte order.
    };

    /// Server settings (dns.json).
    struct Settings {
        std::string domain;   ///< Local domain (e.g. "lan").
        uint32_t localTtl;    ///< TTL of local answers [s].
        int cacheSize;        ///< Number of cache slots.
        uint32_t cacheSweepS; ///< How often to purge expired cache entries [s].
        bool dhcpNames;       ///< Answer with names from DHCP.
        bool blockEnabled;    ///< Domain blocking enabled.
        bool blockZero;       ///< true = 0.0.0.0 / ::, false = NXDOMAIN.
    };

    /// Cached upstream answer.
    struct CacheSlot {
        uint32_t hash;      ///< Hash of name and type; 0 = empty.
        uint16_t qtype;     ///< Query type.
        uint16_t len;       ///< Answer length.
        int64_t expiresUs;  ///< Expiry (smallest TTL of the answer, capped).
        int64_t lastUsedUs; ///< For LRU eviction.
        char name[128];     ///< Query name.
        uint8_t pkt[512];   ///< Answer packet (ID rewritten when served).
    };

    /// Query forwarded upstream and waiting for an answer.
    struct Pending {
        bool used;           ///< Slot in use.
        uint16_t clientId;   ///< Client's query ID.
        uint16_t upstreamId; ///< New ID used upstream.
        sockaddr_in client;  ///< Client address.
        int64_t sentUs;      ///< Send time (timeout, response time).
        uint8_t serverIdx;   ///< Index of the server asked.
        uint8_t serverCount; ///< Number of servers available.
        uint32_t servers[2]; ///< Servers at the time of the query.
        uint16_t qtype;      ///< Query type (for the cache).
        uint16_t len;        ///< Query length.
        char name[128];      ///< Query name (for the cache).
        uint8_t query[512];  ///< Query copy (retry with the second server).
    };

    /// Query counters.
    struct Stats {
        uint32_t queries;   ///< All queries.
        uint32_t local;     ///< Answered from local names.
        uint32_t blocked;   ///< Blocked.
        uint32_t cacheHits; ///< Answered from the cache.
        uint32_t forwarded; ///< Forwarded upstream.
        uint32_t timeouts;  ///< Upstream timeouts.
        uint32_t nxdomain;  ///< NXDOMAIN answers.
        uint32_t servfail;  ///< SERVFAIL answers.
    };

    /// Source of a local answer.
    enum class Source { NONE, STATIC, ROUTER, RESERVATION, LEASE };

    static constexpr int MAX_PENDING = 32; ///< Maximum number of queries in flight upstream.

    /// Task entry.
    static void taskEntry(void* arg);
    /// Task loop: select() on both sockets, timeouts and maintenance.
    void run();
    /// Client query: local name, block list, cache or forward.
    void handleQuery(const uint8_t* buf, int len, const sockaddr_in& client);
    /// Upstream answer: back to the client with the original ID, into the cache.
    void handleUpstream(const uint8_t* buf, int len, const sockaddr_in& from);
    /// Retries with the second server or answers SERVFAIL after the timeout.
    void checkTimeouts();
    /// Purges expired cache entries; clears the cache after a network or upstream DNS change.
    void maintenance();
    /// Sends a query upstream under a new ID.
    void forward(const uint8_t* buf, int len, const sockaddr_in& client, const char* qname, uint16_t qtype);
    /// Sends an answer to a client.
    void sendToClient(const uint8_t* buf, int len, const sockaddr_in& client);
    /// Answers with an error code (NXDOMAIN, SERVFAIL…).
    void sendError(const uint8_t* query, int qend, uint8_t rcode, const sockaddr_in& client);

    /// @name Local resolution (called with the mutex held)
    /// @{
    /// Name -> IP from static entries, router name and DHCP; *shortName receives the name without the local domain.
    Source resolveLocal(const char* qname, uint32_t* ip, std::string* shortName) const;
    /// PTR for addresses of the AP subnet.
    bool resolvePtr(uint32_t ip, char* name, size_t size) const;
    /// Index of the block list entry matching the name or a parent domain, or -1.
    int blockedBy(const char* qname) const;
    /// Current upstream servers ("auto" = DNS from the STA DHCP).
    void upstreamServers(uint32_t servers[2], uint8_t* count) const;
    /// @}

    /// @name Cache (called with the mutex held)
    /// @{
    CacheSlot* cacheFind(const char* name, uint16_t qtype);
    void cacheStore(const char* name, uint16_t qtype, const uint8_t* pkt, int len);
    void cacheClear();
    /// @}

    /// @name Block list (called with the mutex held)
    /// @{
    /// Binary search; index or -1.
    int blockFind(const char* name) const;
    /// Inserts keeping the order. @return false when already present.
    bool blockInsert(const char* name);
    /// @return false when not present.
    bool blockRemove(const char* name);
    /// Name of entry i.
    const char* blockName(int i) const { return blockArena.data() + blockOffsets[i]; }
    /// @}

    /// "dns lookup": where the answer for a name would come from (also with the service stopped).
    CmdResult runLookup(const char* name, IOutput& out);

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdList(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdAdd(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdDel(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdUpstream(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdLookup(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdCache(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdStats(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockAdd(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockDel(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockList(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockCheck(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockOn(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockOff(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockReload(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdBlockStats(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    WifiManager& wifi;            ///< AP address/mask, STA address, upstream DNS.
    DhcpServer& dhcp;             ///< Names from reservations and leases.
    const GlobalSettings& global; ///< Router host name.
    ConfigStore& store;           ///< Block list file.

    Settings cfg = {};     ///< Loaded/changed by config set.
    Settings active = {};  ///< Used by the running server.
    uint32_t serverIp = 0; ///< AP address at start (network byte order).
    uint32_t netmask = 0;  ///< AP mask at start.

    /// Guards entries, upstream, block list and cache (DNS task vs. consoles).
    mutable std::mutex mutex;
    std::vector<Entry> entries;     ///< Static entries.
    std::vector<uint32_t> upstream; ///< Fixed upstream servers; empty = "auto".

    // Block list: sorted names in one buffer (large arrays go to PSRAM).
    std::vector<char> blockArena;       ///< Zero-terminated names one after another.
    std::vector<uint32_t> blockOffsets; ///< Sorted offsets of names in blockArena.
    std::vector<uint32_t> blockHits;    ///< Hit counters, parallel to blockOffsets.
    bool blockDirty = false;            ///< List changed since load/save.

    // Import state (HTTP server task only).
    std::vector<char> importArena;       ///< Names of the list being built.
    std::vector<uint32_t> importOffsets; ///< Offsets in importArena.
    std::string importCarry;             ///< Unfinished line between pieces.
    size_t importBefore = 0;             ///< Number of domains before the import.
    /// Parses one import line (domain or hosts entry, comments skipped).
    void importLine(char* line);

    CacheSlot* cache = nullptr; ///< Cache slots (PSRAM when available).
    int cacheSlots = 0;         ///< Number of slots.

    Pending* pending = nullptr;        ///< MAX_PENDING slots of queries in flight.
    Stats stats = {};                  ///< Query counters.
    uint32_t upstreamAnswers[2] = {};  ///< Answers per upstream server.
    uint32_t upstreamTimeouts[2] = {}; ///< Timeouts per upstream server.
    int64_t upstreamTimeUs[2] = {};    ///< Sum of response times per server (average = sum / answers).

    int64_t lastSweepUs = 0;    ///< Last purge of expired cache entries.
    int64_t lastNetCheckUs = 0; ///< Last check of the STA network / auto DNS.
    uint32_t lastStaIp = 0;     ///< A change of the STA network or of DNS in auto mode clears the cache.
    uint32_t lastAutoDns = 0;   ///< Upstream DNS last seen in auto mode.

    int sock = -1;                             ///< Client queries (AP, port 53).
    int upstreamSock = -1;                     ///< Queries to the upstream DNS.
    std::atomic<bool> running { false };       ///< Service running.
    std::atomic<bool> stopRequested { false }; ///< stop() asked the task to finish.
    std::atomic<bool> taskAlive { false };     ///< Task still running.
};
