/**
 * @file GlobalSettings.h
 * @brief Global settings from /config/global.json (name, language, logs, time, management, service autostart).
 */
#pragma once

#include <cstring>
#include <string>

#include "ConfigReader.h"

/// Time settings (global.json "time").
struct TimeSettings {
    bool ntpEnabled;       ///< Synchronize the clock with SNTP after the STA gets an address.
    std::string ntpServer; ///< NTP server name.
    std::string timezone;  ///< Time zone in POSIX TZ format.
};

/// Autostart of services at device start (global.json "services").
struct ServiceSettings {
    bool serial = true;    ///< COM console.
    bool telnet = true;    ///< Telnet server.
    bool dhcp = true;      ///< DHCP server.
    bool dns = true;       ///< DNS server.
    bool ddns = false;     ///< DDNS client.
    bool firewall = false; ///< Firewall.
    bool web = true;       ///< Web panel.

    /// @return Whether the service with the given name starts automatically.
    bool autostart(const char* name) const
    {
        return (strcmp(name, "serial") == 0 && serial) || (strcmp(name, "telnet") == 0 && telnet) ||
               (strcmp(name, "dhcp") == 0 && dhcp) ||
               (strcmp(name, "dns") == 0 && dns) || (strcmp(name, "ddns") == 0 && ddns) ||
               (strcmp(name, "firewall") == 0 && firewall) ||
               (strcmp(name, "web") == 0 && web);
    }
};

/// All values of global.json.
struct GlobalValues {
    std::string hostname;     ///< Device name (DHCP hostname on the STA side, name in local DNS).
    std::string language;     ///< pl|en – panel and console (doc/dev/jezyki.md).
    bool consoleUtf8 = false; ///< COM/Telnet in UTF-8 (otherwise Polish letters as ASCII).
    std::string logLevel;     ///< none/error/warn/info/debug.
    std::string logOutput;    ///< Log output ("console").
    TimeSettings time;        ///< NTP and time zone.
    bool mgmtFromSta = false; ///< Telnet/panel also available from the STA side.
    bool wanPing = false;     ///< Answer ping from the STA side (WAN).
    ServiceSettings services; ///< Service autostart.
};

/**
 * @brief Global settings from /config/global.json.
 *
 * Two copies: "config" (loaded, changed by "config set", written by "save") and "active"
 * (used by running modules). active = config at start-up, afterwards only the fields applied
 * immediately (language, console_utf8, log.level, mgmt_from_sta, wan_ping) are copied.
 */
class GlobalSettings : public IConfigurable {
public:
    const char* configName() const override { return "global"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Applies language, console_utf8, log level, mgmt_from_sta and wan_ping immediately.
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string& path) const override
    {
        return path == "language" || path == "console_utf8" || path == "log.level" || path == "log.output" || path == "mgmt_from_sta" ||
               path == "wan_ping";
    }

    /// @brief Takes the loaded configuration as active: log level, language, console UTF-8, ping from WAN.
    void activate();

    /// @return Active values (used by running modules).
    const GlobalValues& values() const { return active; }
    /// @return Configured values (e.g. autostart for the next start).
    const GlobalValues& configured() const { return config; }

private:
    /// Sets the ESP-IDF log level from active.logLevel.
    void applyLogLevel() const;
    /// Sets the current language (i18n) from active.language (default from Defaults.h on a bad value).
    void applyLanguage() const;

    GlobalValues config; ///< Configured copy.
    GlobalValues active; ///< Active copy.
};
