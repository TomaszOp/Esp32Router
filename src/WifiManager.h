/**
 * @file WifiManager.h
 * @brief APSTA mode: STA connection to the upstream network and the router's own AP.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "esp_event.h"
#include "esp_netif.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "GlobalSettings.h"

/// Wi-Fi settings (wifi.json).
struct WifiSettings {
    std::string staSsid;  ///< Upstream network SSID (empty = do not connect).
    std::string staPass;  ///< Upstream network password.
    std::string apSsid;   ///< Own network SSID.
    std::string apPass;   ///< Own network password (empty = open network).
    uint8_t apMaxClients; ///< Maximum number of AP clients.
    uint32_t apIp;        ///< AP address, network byte order.
    uint32_t apMask;      ///< AP subnet mask, network byte order.
};

/**
 * @brief APSTA mode: STA connection to the upstream network (with retries) and the router's own AP.
 *
 * Configuration in /config/wifi.json. The built-in ESP-IDF DHCP server is disabled on the AP
 * (addresses are given by DhcpServer). Wi-Fi and IP events are handled in the default event loop;
 * the configuration is guarded by a mutex because the console changes it from other tasks.
 */
class WifiManager : public IConfigurable {
public:
    explicit WifiManager(const GlobalSettings& global);

    const char* configName() const override { return "wifi"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;

    /// @brief Creates the netifs, starts Wi-Fi in APSTA mode and connects the STA (if an SSID is set).
    void start();
    /// @brief Registers the "wifi" command group.
    void registerCommands(CommandExecutor& exec);

    /// One network found by scan().
    struct ScanResult {
        std::string ssid; ///< Network name.
        int8_t rssi;      ///< Signal strength [dBm].
        uint8_t channel;  ///< Channel.
        const char* auth; ///< Security name (e.g. "WPA2").
        bool open;        ///< No password.
    };

    /**
     * @brief Sets the upstream network and connects immediately ("wifi sta"); permanent after "save".
     * @param ssid Empty = disconnect and do not connect.
     * @param pass nullptr = keep the current password.
     */
    CmdResult setSta(const char* ssid, const char* pass, IOutput& out);
    /**
     * @brief Changes the AP network immediately ("wifi ap") – all AP clients are disconnected; permanent after "save".
     * @param pass nullptr or empty = open network; otherwise 8-63 characters.
     */
    CmdResult setAp(const char* ssid, const char* pass, IOutput& out);
    /// @brief Scans networks in range (blocking, a few seconds; AP clients may notice a short break). @return false on error or scan in progress.
    bool scan(std::vector<ScanResult>& results);
    /// @return Last reason of a failed connection / STA disconnect (0 = none; cleared after getting an IP).
    int lastStaError() const { return lastDisconnectReason; }
    /// @return Text for a Wi-Fi disconnect reason code in the current language.
    static const char* reasonText(int reason);

    /// @brief Current STA link: RSSI and channel. @return false when not connected.
    bool staLink(int8_t* rssi, uint8_t* channel) const;

    /**
     * @brief Registers a function called after the AP is restarted (e.g. after an SSID/password change).
     *
     * ESP-IDF creates the AP interface in lwIP again (it loses NAPT and its interface number).
     * Functions are called from the event loop; registration only at start-up.
     */
    void addApRestartListener(void (*fn)(void*), void* ctx);

    /// @return Active settings (used by the driver).
    WifiSettings settings() const;
    /// @return Configured settings (e.g. for validating the DHCP pool while loading the configuration).
    WifiSettings configured() const;
    /// @return DNS of the upstream network (network byte order) or 0 when the STA has no IP.
    uint32_t upstreamDns() const;
    /// @return AP interface.
    esp_netif_t* apNetif() const { return ap; }
    /// @return STA interface.
    esp_netif_t* staNetif() const { return sta; }
    /// @return Whether the STA has an IP address.
    bool staConnected() const { return staHasIp; }

private:
    /// Event loop entry: dispatches WIFI_EVENT and IP_EVENT.
    static void onEvent(void* arg, esp_event_base_t base, int32_t id, void* data);
    /// STA connect/disconnect (with retries), AP start (restart listeners), client join/leave.
    void handleWifiEvent(int32_t id, void* data);
    /// STA got/lost IP.
    void handleIpEvent(int32_t id, void* data);
    /// Applies the active AP settings to the driver and the AP netif (address, no built-in DHCP).
    void configureAp();
    /// Applies the active STA settings and connects.
    void configureSta();

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdSta(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdAp(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdScan(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    const GlobalSettings& global; ///< Host name for the STA.
    // config: loaded/changed by config set, written by save.
    // active: used by the driver; = config at start-up, then changed by wifi sta/ap.
    mutable std::mutex mutex;              ///< Guards config and active (console vs. Wi-Fi events).
    std::mutex scanMutex;                  ///< One scan at a time.
    WifiSettings config = {};              ///< Configured copy.
    WifiSettings active = {};              ///< Active copy.
    esp_netif_t* ap = nullptr;             ///< AP interface.
    esp_netif_t* sta = nullptr;            ///< STA interface.
    volatile bool staHasIp = false;        ///< STA has an IP address.
    volatile int lastDisconnectReason = 0; ///< Last disconnect reason (0 = none).
    int apStarts = 0;                      ///< Number of WIFI_EVENT_AP_START (second and later = restart).
    std::vector<std::pair<void (*)(void*), void*>> apRestartListeners; ///< Called after an AP restart.

};
