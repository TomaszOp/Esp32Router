/**
 * @file Firewall.h
 * @brief Firewall for traffic from AP clients: rules, service, log of blocked packets.
 */
#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "esp_timer.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "DhcpServer.h"
#include "GlobalSettings.h"
#include "IService.h"
#include "PacketFilter.h"

/**
 * @brief Firewall for traffic from AP clients.
 *
 * Rules in /config/firewall.json are checked in order in the lwIP hook (PacketFilter); the first
 * match decides, no match = allow. Service "firewall" (ServiceManager); a stopped service allows
 * everything. A client given as MAC or DHCP reservation name is resolved to an IP on every change
 * of rules, leases and reservations. A 1 s timer moves blocked-packet events from the hook's ring
 * to the log and to the "recently blocked" list.
 */
class Firewall : public IConfigurable, public IService {
public:
    static constexpr int MAX_RULES = PacketFilter::MAX_RULES; ///< Maximum number of rules.

    Firewall(DhcpServer& dhcp, const GlobalSettings& global);

    const char* configName() const override { return "firewall"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Rules and log_blocked apply immediately.
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string&) const override { return true; }

    const char* name() const override { return "firewall"; }
    /// Enables the rules in the hook.
    esp_err_t start() override;
    /// Disables the rules (all traffic allowed; rules stay in memory).
    esp_err_t stop() override;
    bool isRunning() const override { return PacketFilter::firewallEnabled(); }
    /// " active rules N of M" for "service list".
    void describe(IOutput& out) const override;

    /// @brief At start-up (after the configuration is loaded, before Wi-Fi starts): DHCP listener, log timer, first compile.
    void begin();
    /// @brief Registers the "fw" command.
    void registerCommands(CommandExecutor& exec);

    /// @return Web panel: state, rules with counters, recently blocked (JSON, freed by the caller).
    cJSON* statusJson() const;
    /// @return Short state for the Status page (/api/status).
    cJSON* summaryJson() const;
    /**
     * @brief Web panel: adds/changes a rule from JSON
     * {index?, name, enabled?, client?, proto?, ports?, dest?, dest_ip?, action?}.
     */
    CmdResult setRuleJson(const cJSON* json, IOutput& out);

private:
    /// Rule as configured (firewall.json "rules").
    struct Rule {
        uint32_t id;         ///< Identifier for PacketFilter counters.
        std::string name;    ///< Unique name.
        bool enabled;        ///< Rule enabled.
        std::string client;  ///< all | IP | IP/mask | MAC | reservation name.
        uint8_t proto;       ///< PacketFilter::Proto.
        std::string ports;   ///< "" or a list "25,6881-6889".
        uint8_t dest;        ///< PacketFilter::Dest.
        std::string destIp;  ///< any | IP | IP/mask.
        bool block;          ///< true = block, false = allow.
        const char* error;   ///< Unresolved client (translated reason) or nullptr.
        uint32_t resolvedIp; ///< Client IP after resolving a MAC/name (0 = none).
    };

    /// Entry of the "recently blocked" list.
    struct Recent {
        time_t when;             ///< Time of the event.
        PacketFilter::Blocked b; ///< Packet data.
    };

    /// Checks the fields of a rule. @return nullptr = valid, otherwise the error text.
    const char* validate(const Rule& r) const;
    /// Resolves clients and swaps the table in the hook (takes the mutex; call without it).
    void compile();
    /// Runs in the tcpip thread: installs the compiled table.
    static esp_err_t installInTcpip(void* ctx);
    /// DhcpServer listener: leases/reservations changed – compile again.
    static void onDhcpChange(void* ctx);
    /// 1 s timer: drains the blocked-packet ring.
    static void timerCb(void* arg);
    /// Moves events from the hook's ring to the log (max 1 per second per rule) and to recent.
    void drainLog();

    /// Rule by number (from 1) or name; with the mutex held. @return Index or -1.
    int findRule(const char* token) const;
    /// Validates and stores a rule (index -1 = new; pos = position for a new rule), then compile.
    CmdResult storeRule(int index, Rule rule, int pos, IOutput& out);
    /// Prints the state and the rule table ("fw").
    void printRules(IOutput& out) const;

    /// "fw [add|del|enable|disable|move|reset-counters]" handler (ctx = this).
    static CmdResult cmdFw(void* ctx, int argc, char** argv, IOutput& out);
    /// "fw add …".
    CmdResult cmdAdd(int argc, char** argv, IOutput& out);

    DhcpServer& dhcp;             ///< Resolving MACs and reservation names.
    const GlobalSettings& global; ///< wan_ping (shown by "fw").

    mutable std::mutex mutex;           ///< Rules, settings, recently blocked.
    std::vector<Rule> rules;            ///< Rules in order.
    bool logBlocked = true;             ///< Log blocked packets.
    uint32_t nextId = 1;                ///< Next rule id.
    std::vector<Recent> recent;         ///< Recently blocked, newest at the end.
    esp_timer_handle_t timer = nullptr; ///< Log timer.

    // Table prepared for installInTcpip.
    PacketFilter::Rule compiled[MAX_RULES]; ///< Compiled rules.
    int compiledCount = 0;                  ///< Number of compiled rules.

};
