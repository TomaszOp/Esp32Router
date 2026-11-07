/**
 * @file DhcpServer.h
 * @brief DHCP server on the AP interface: address pool, MAC->IP reservations, leases.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "IService.h"
#include "WifiManager.h"

/**
 * @brief Own DHCP server on the AP interface (replaces the built-in one).
 *
 * Address pool, MAC->IP reservations and host names reported by clients. Leases are kept only
 * in RAM. Configuration in /config/dhcp.json; service "dhcp" (ServiceManager). Packets are
 * handled in its own FreeRTOS task (UDP 67, socket bound to the AP interface); reservations and
 * leases are guarded by a mutex because the consoles and DnsServer read them from other tasks.
 */
class DhcpServer : public IConfigurable, public IService {
public:
    static constexpr int MAX_POOL = 128;        ///< Maximum number of addresses in the pool.
    static constexpr int MAX_RESERVATIONS = 32; ///< Maximum number of reservations.

    explicit DhcpServer(WifiManager& wifi);

    const char* configName() const override { return "dhcp"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Reservations apply immediately; the other fields after "service restart dhcp".
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string& path) const override { return path == "reservations"; }

    const char* name() const override { return "dhcp"; }
    /// Copies the configuration to active, opens the socket and starts the task.
    esp_err_t start() override;
    /// Asks the task to finish and waits for it (leases are kept).
    esp_err_t stop() override;
    bool isRunning() const override { return running; }
    /// " pool a-b, leases N" for "service list".
    void describe(IOutput& out) const override;

    /// @brief Registers the "dhcp" command group.
    void registerCommands(CommandExecutor& exec);

    /// @brief For "clients": IP (network byte order) and host name from the lease/reservation of a MAC. @return false when unknown.
    bool lookup(const uint8_t mac[6], uint32_t* ip, char* host, size_t hostSize) const;
    /// @brief Address of the reservation with the given name (for port forwarding and firewall rules). @return false when none.
    bool reservationIp(const char* name, uint32_t* ip) const;

    /**
     * @brief For DnsServer: name (without domain, case-insensitive) -> IP, from a reservation first, then from an active lease.
     * @param fromReservation Receives whether the name came from a reservation.
     */
    bool resolveName(const char* name, uint32_t* ip, bool* fromReservation) const;
    /// @brief Reverse lookup IP -> name (PTR). @return false when unknown.
    bool nameForIp(uint32_t ip, char* name, size_t size) const;
    /// Name known from DHCP.
    struct NameEntry {
        std::string name; ///< Host or reservation name.
        uint32_t ip;      ///< Address, network byte order.
        bool reservation; ///< From a reservation (otherwise from a lease).
    };
    /// @return All names known from reservations and leases ("dns list", panel).
    std::vector<NameEntry> names() const;

    /// @name Web panel (JSON freed by the caller)
    /// @{
    /// Active leases with type (oferta / rezerwacja / dynamiczny) and remaining time.
    cJSON* leasesJson() const;
    /// Reservations.
    cJSON* reservationsJson() const;
    /// @}
    /// @return Whether the MAC has a reservation.
    bool isReserved(const uint8_t mac[6]) const;

    /// @brief Function called after a change of leases or reservations (the firewall resolves MACs and names to IPs). Called without the DHCP mutex.
    void setChangeListener(void (*fn)(void*), void* ctx)
    {
        listenerCtx = ctx;
        listener = fn;
    }

    /// @brief DNS service on the AP address: while it runs, "dns": "auto" hands out the AP address (otherwise the upstream DNS).
    void setLocalDns(const IService* dns) { localDns = dns; }

private:
    /// MAC -> IP reservation.
    struct Reservation {
        uint8_t mac[6];   ///< Client MAC.
        uint32_t ip;      ///< Reserved address, network byte order.
        std::string name; ///< Optional name (DNS, rules).
    };

    /// Lease state.
    enum class LeaseState : uint8_t { OFFERED, BOUND };

    /// Address assigned to a client.
    struct Lease {
        uint8_t mac[6];    ///< Client MAC.
        uint32_t ip;       ///< Address, network byte order.
        LeaseState state;  ///< Offered (DHCPOFFER) or bound (DHCPACK).
        int64_t expiresUs; ///< Expiry time (esp_timer).
        char host[33];     ///< Host name reported by the client (option 12).
    };

    /// Server settings (dhcp.json).
    struct Settings {
        uint32_t poolStart; ///< First pool address, network byte order.
        uint32_t poolEnd;   ///< Last pool address.
        uint32_t leaseTime; ///< Lease time [s].
        uint32_t dns;       ///< DNS for clients; 0 = auto.
    };

    /// Task entry.
    static void taskEntry(void* arg);
    /// Task loop: receives packets until stopRequested.
    void run();
    /// Handles DISCOVER / REQUEST / RELEASE / DECLINE / INFORM.
    void handlePacket(const uint8_t* buf, int len);
    /// Builds and sends OFFER / ACK / NAK (to the client address when known, otherwise broadcast).
    void sendReply(const uint8_t* req, uint8_t type, uint32_t yiaddr, bool noLeaseOptions);

    /// @name Called with the mutex held
    /// @{
    const Reservation* reservationForMac(const uint8_t mac[6]) const;
    const Reservation* reservationForIp(uint32_t ip) const;
    Lease* leaseForMac(const uint8_t mac[6]);
    /// Whether the address may be given to this MAC (in pool or reserved for it, not taken by another client).
    bool ipAllowed(uint32_t ip, const uint8_t mac[6]) const;
    /// Whether the address is in the pool.
    bool inPool(uint32_t ip) const;
    /// Chooses an address: reservation, current lease, requested address, first free one, else the oldest lease (0 = none).
    uint32_t pickAddress(const uint8_t mac[6], uint32_t requested);
    /// Creates or updates the lease of a MAC.
    Lease& setLease(const uint8_t mac[6], uint32_t ip, LeaseState state, int64_t durationUs, const char* host);
    /// Removes leases of an address (e.g. after a new reservation), except for one MAC.
    void dropLeasesOfIp(uint32_t ip, const uint8_t* exceptMac);
    /// @}

    /// Checks the pool against the AP subnet; *why receives the reason.
    bool settingsValid(const Settings& s, uint32_t apIp, uint32_t apMask, const char** why) const;
    /// Default settings for the AP subnet (pool .100-.199).
    Settings defaultsFor(uint32_t apIp, uint32_t apMask) const;

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdLeases(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdReservations(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdReserve(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdUnreserve(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdRelease(void* ctx, int argc, char** argv, IOutput& out);
    /// @}
    /// Wrapper for commands that change reservations/leases – notifies the listener after success.
    template <CommandHandler fn> static CmdResult notifying(void* ctx, int argc, char** argv, IOutput& out)
    {
        CmdResult r = fn(ctx, argc, argv, out);
        if (r == CmdResult::OK) {
            static_cast<DhcpServer*>(ctx)->notifyChange();
        }
        return r;
    }
    /// Calls the change listener (if set).
    void notifyChange() const
    {
        if (listener != nullptr) {
            listener(listenerCtx);
        }
    }

    WifiManager& wifi;                  ///< AP address and mask, upstream DNS.
    const IService* localDns = nullptr; ///< DNS service (for "dns": "auto").
    void (*listener)(void*) = nullptr;  ///< Change listener (Firewall).
    void* listenerCtx = nullptr;        ///< Listener context.

    Settings cfg = {};     ///< Loaded/changed by config set.
    Settings active = {};  ///< Used by the running server (copied in start()).
    uint32_t serverIp = 0; ///< AP address at start (network byte order).
    uint32_t netmask = 0;  ///< AP mask at start.


    mutable std::mutex mutex;              ///< Reservations and leases (DHCP task vs. consoles).
    std::vector<Reservation> reservations; ///< Reservations (dhcp.json "reservations").
    std::vector<Lease> leases;             ///< Leases (RAM only).

    int sock = -1;                             ///< UDP socket (port 67).
    std::atomic<bool> running { false };       ///< Service running.
    std::atomic<bool> stopRequested { false }; ///< stop() asked the task to finish.
    std::atomic<bool> taskAlive { false };     ///< Task still running.

    uint8_t txBuf[576]; ///< Reply buffer (minimum DHCP message size).

};
