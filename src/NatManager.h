/**
 * @file NatManager.h
 * @brief NAPT (internet sharing) on the AP interface.
 */
#pragma once

#include <mutex>

#include "esp_event.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "WifiManager.h"

/**
 * @brief Enables NAPT on the AP interface after the STA gets an IP. Configuration in /config/nat.json.
 *
 * "nat on|off" changes the state immediately, permanently after "save". NAPT is (re)enabled from
 * the IP event handler and after an AP restart (reapply()).
 */
class NatManager : public IConfigurable {
public:
    explicit NatManager(WifiManager& wifi);

    const char* configName() const override { return "nat"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string&) const override { return true; }

    /// @brief Registers for IP_EVENT_STA_GOT_IP (before Wi-Fi starts).
    void start();
    /// @brief Registers "nat on|off".
    void registerCommands(CommandExecutor& exec);

    /// @return Whether NAPT is currently enabled on the AP interface.
    bool enabled() const;
    /// @return Web panel: state and usage of the NAT connection table (JSON, freed by the caller).
    cJSON* statusJson() const;
    /// @brief Sets the wanted state and applies it (enabled only when the STA has an IP). @return false when NAPT could not be changed.
    bool setWanted(bool on);
    /// @brief After an AP restart the interface NAPT flag is cleared – enables it again.
    void reapply();
    /// @brief Function called after a NAT state change by command (nat on/off, config set) – outside the NatManager mutex.
    void setChangeListener(void (*fn)(void*), void* ctx);

private:
    /// Event handler: STA got an IP – apply the wanted state.
    static void onGotIp(void* arg, esp_event_base_t base, int32_t id, void* data);
    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdOn(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdOff(void* ctx, int argc, char** argv, IOutput& out);
    /// @}
    /// Enables/disables NAPT according to wanted (caller holds the mutex). @return false when the NAPT call failed.
    bool applyLocked(bool staHasIp);

    WifiManager& wifi;                 ///< AP interface and STA state.
    mutable std::mutex mutex;          ///< Guards wanted and natEnabled.
    bool wanted = true;                ///< Configured state (nat.json "enabled").
    bool natEnabled = false;           ///< Actual NAPT state on the interface.
    void (*listener)(void*) = nullptr; ///< Change listener (PortForwarder).
    void* listenerCtx = nullptr;       ///< Listener context.

};
