/**
 * @file PortForwarder.h
 * @brief Port forwarding from the STA address to AP clients.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_event.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "DhcpServer.h"
#include "GlobalSettings.h"
#include "NatManager.h"
#include "TelnetServer.h"
#include "WifiManager.h"

/**
 * @brief Port forwarding from the STA address to AP clients (lwIP ip_portmap, works together with NAPT).
 *
 * Rules in /config/portfwd.json; changes by commands apply immediately, permanently after "save".
 * The rules are installed again after every change of the STA address, the rules or the NAT state
 * (lwIP calls run in the tcpip thread). The src field limits who from outside may use a
 * forward (PacketFilter hook).
 */
class PortForwarder : public IConfigurable {
public:
    static constexpr int MAX_ENTRIES = 32; ///< IP_PORTMAP_MAX; "both" takes 2 entries.

    PortForwarder(WifiManager& wifi, NatManager& nat, DhcpServer& dhcp, const GlobalSettings& global,
                  const TelnetServer& telnet);

    const char* configName() const override { return "portfwd"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Rules apply immediately (installed again).
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string& path) const override { return path == "rules"; }

    /// @brief Registers for STA IP events and NAT changes, installs the rules.
    void start();
    /// @brief Registers the "pf" command group.
    void registerCommands(CommandExecutor& exec);

    /// @return Web panel: rules with state (JSON, freed by the caller).
    cJSON* rulesJson() const;
    /**
     * @brief Web panel: adds/changes a rule from JSON
     * {index?, name, proto, ext_port, target, int_port?, enabled?, src?} (index missing/-1 = new).
     * Messages go to out like for "pf add".
     */
    CmdResult setRuleJson(const cJSON* json, IOutput& out);

private:
    /// Protocol bits.
    enum Proto : uint8_t { TCP = 1, UDP = 2, BOTH = 3 };
    /// Rule state shown in "pf list" and the panel.
    enum class State : uint8_t { DISABLED, PENDING, ACTIVE, ERROR };

    /// Forwarding rule (portfwd.json "rules").
    struct Rule {
        std::string name;    ///< Optional name.
        uint8_t proto;       ///< Proto bits.
        uint16_t extPort;    ///< External port on the STA address.
        std::string target;  ///< IPv4 or DHCP reservation name.
        uint16_t intPort;    ///< Port on the client.
        bool enabled;        ///< Rule enabled.
        std::string src;     ///< Allowed sources "IP[/mask],..." (max 4), empty = everyone.
        State state;         ///< State after the last apply.
        uint32_t resolvedIp; ///< Network byte order, 0 = not resolved.
        const char* error;   ///< Reason of ERROR (translated text) or nullptr.
    };

    /// Event handler: STA got/lost IP – install again.
    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    /// NatManager listener: NAT state changed – install again.
    static void onNatChange(void* arg);
    /// @return Current STA address (network byte order) or 0.
    uint32_t currentStaIp() const;
    /// Resolves targets and installs all rules in lwIP for the STA address (via the tcpip thread).
    void apply(uint32_t staIp);
    /// Runs in the tcpip thread: removes old portmap entries, adds the active ones, installs sources.
    static esp_err_t applyInTcpip(void* ctx);
    /// Passes the src lists of active rules to PacketFilter (tcpip thread).
    static void installSources(const std::vector<Rule>& rules);

    /// @return Number of lwIP entries used by the rules (both = 2), optionally without one rule.
    int entriesUsed(const Rule* except) const;
    /// Target -> IP (literal address or DHCP reservation name).
    bool resolveTarget(const std::string& target, uint32_t* ip) const;
    /// Rule by number or name.
    Rule* findRule(const char* token);
    /// Warns when the external port equals the Telnet port reachable from the STA side.
    void warnTelnetCollision(const Rule& r, IOutput* out) const;

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdList(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdAdd(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdDel(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdEnable(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdDisable(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdApply(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdSrc(void* ctx, int argc, char** argv, IOutput& out);
    /// @}
    /// Shared part of "pf enable/disable".
    CmdResult setEnabled(int argc, char** argv, bool enabled, IOutput& out);
    /// Validates and stores a rule (index -1 = new, otherwise replace); then apply.
    CmdResult storeRule(int index, Rule rule, IOutput& out);

    WifiManager& wifi;            ///< STA address and interface, AP subnet.
    NatManager& nat;              ///< NAT state (forwarding needs NAPT).
    DhcpServer& dhcp;             ///< Reservation names.
    const GlobalSettings& global; ///< mgmt_from_sta (Telnet collision warning).
    const TelnetServer& telnet;   ///< Telnet port.

    mutable std::mutex mutex; ///< Rules (consoles vs. network events).
    std::vector<Rule> rules;  ///< Rules in order.

    /// Entry installed in lwIP (removed on the next apply).
    struct Applied {
        uint8_t ipProto;  ///< IP protocol number.
        uint16_t extPort; ///< External port.
    };
    std::vector<Applied> applied; ///< Entries currently installed in lwIP.
    uint32_t applyStaIp = 0;      ///< STA address for the current apply (used in the tcpip context).
    bool applyNatOn = false;      ///< NAT state for the current apply.

};
