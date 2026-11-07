/**
 * @file GlobalSettings.cpp
 * @brief GlobalSettings: global.json (name, language, logs, time, management, autostart).
 */
#include "GlobalSettings.h"

#include "Defaults.h"

#include <cstring>

#include "esp_log.h"

#include "PacketFilter.h"
#include "i18n/I18n.h"

#define LOG_TAG     "GlobalSettings"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::global; // factory settings of global.json

static const char* const LOG_LEVELS[] = { "none", "error", "warn", "info", "debug" };
static const esp_log_level_t LOG_LEVEL_VALUES[] = { ESP_LOG_NONE, ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG };

// Host name: 1-32 characters a-z A-Z 0-9 -.
static bool validHostname(const std::string& s)
{
    if (s.empty()) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// One of the LOG_LEVELS names.
static bool validLogLevel(const std::string& s)
{
    for (const char* l : LOG_LEVELS) {
        if (s == l) {
            return true;
        }
    }
    return false;
}

// Only "console" is supported.
static bool validLogOutput(const std::string& s)
{
    return s == "console";
}

// "pl" or "en".
static bool validLanguage(const std::string& s)
{
    i18n::Lang lang;
    return i18n::parse(s, &lang);
}

// Non-empty text.
static bool notEmpty(const std::string& s)
{
    return !s.empty();
}

void GlobalSettings::loadConfig(ConfigReader& r)
{
    r.integer("version", dflt::VERSION, 1, 1);
    config.hostname = r.str("hostname", dflt::HOSTNAME, 32, &validHostname, { "1-32 znaki a-z A-Z 0-9 -", "1-32 chars a-z A-Z 0-9 -" });
    config.language = r.str("language", dflt::LANGUAGE, 2, &validLanguage, { "pl/en", "pl/en" });
    config.consoleUtf8 = r.boolean("console_utf8", dflt::CONSOLE_UTF8);

    ConfigReader log = r.object("log");
    config.logLevel = log.str("level", dflt::LOG_LEVEL, 8, &validLogLevel, { "none/error/warn/info/debug", "none/error/warn/info/debug" });
    config.logOutput = log.str("output", dflt::LOG_OUTPUT, 16, &validLogOutput, { "console", "console" });
    log.finish();

    ConfigReader t = r.object("time");
    config.time.ntpEnabled = t.boolean("ntp_enabled", dflt::NTP_ENABLED);
    config.time.ntpServer = t.str("ntp_server", dflt::NTP_SERVER, 64, &notEmpty);
    config.time.timezone = t.str("timezone", dflt::TIMEZONE, 64, &notEmpty, { "POSIX TZ, max 64", "POSIX TZ, max 64" });
    t.finish();

    config.mgmtFromSta = r.boolean("mgmt_from_sta", dflt::MGMT_FROM_STA);
    config.wanPing = r.boolean("wan_ping", dflt::WAN_PING);

    ConfigReader svc = r.object("services");
    config.services.serial = svc.boolean("serial", dflt::AUTOSTART_SERIAL);
    config.services.telnet = svc.boolean("telnet", dflt::AUTOSTART_TELNET);
    config.services.dhcp = svc.boolean("dhcp", dflt::AUTOSTART_DHCP);
    config.services.dns = svc.boolean("dns", dflt::AUTOSTART_DNS);
    config.services.ddns = svc.boolean("ddns", dflt::AUTOSTART_DDNS);
    config.services.firewall = svc.boolean("firewall", dflt::AUTOSTART_FIREWALL);
    config.services.web = svc.boolean("web", dflt::AUTOSTART_WEB);
    svc.finish();
}

cJSON* GlobalSettings::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", dflt::VERSION);
    cJSON_AddStringToObject(root, "hostname", config.hostname.c_str());
    cJSON_AddStringToObject(root, "language", config.language.c_str());
    cJSON_AddBoolToObject(root, "console_utf8", config.consoleUtf8);

    cJSON* log = cJSON_AddObjectToObject(root, "log");
    cJSON_AddStringToObject(log, "level", config.logLevel.c_str());
    cJSON_AddStringToObject(log, "output", config.logOutput.c_str());

    cJSON* t = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(t, "ntp_enabled", config.time.ntpEnabled);
    cJSON_AddStringToObject(t, "ntp_server", config.time.ntpServer.c_str());
    cJSON_AddStringToObject(t, "timezone", config.time.timezone.c_str());

    cJSON_AddBoolToObject(root, "mgmt_from_sta", config.mgmtFromSta);
    cJSON_AddBoolToObject(root, "wan_ping", config.wanPing);

    cJSON* svc = cJSON_AddObjectToObject(root, "services");
    cJSON_AddBoolToObject(svc, "serial", config.services.serial);
    cJSON_AddBoolToObject(svc, "telnet", config.services.telnet);
    cJSON_AddBoolToObject(svc, "dhcp", config.services.dhcp);
    cJSON_AddBoolToObject(svc, "dns", config.services.dns);
    cJSON_AddBoolToObject(svc, "ddns", config.services.ddns);
    cJSON_AddBoolToObject(svc, "firewall", config.services.firewall);
    cJSON_AddBoolToObject(svc, "web", config.services.web);
    return root;
}

void GlobalSettings::activate()
{
    active = config;
    applyLogLevel();
    applyLanguage();
    i18n::consoleUtf8 = active.consoleUtf8;
    PacketFilter::setWanPing(active.wanPing);
}

bool GlobalSettings::applyLive(const std::string& path)
{
    if (path == "language") {
        active.language = config.language;
        applyLanguage();
        return true;
    }
    if (path == "console_utf8") {
        active.consoleUtf8 = config.consoleUtf8;
        i18n::consoleUtf8 = active.consoleUtf8;
        return true;
    }
    if (path == "log.level") {
        active.logLevel = config.logLevel;
        applyLogLevel();
        return true;
    }
    if (path == "log.output") {
        active.logOutput = config.logOutput;
        return true;
    }
    if (path == "mgmt_from_sta") {
        active.mgmtFromSta = config.mgmtFromSta;
        return true;
    }
    if (path == "wan_ping") {
        active.wanPing = config.wanPing;
        PacketFilter::setWanPing(active.wanPing);
        return true;
    }
    return false;
}

void GlobalSettings::applyLogLevel() const
{
    for (size_t i = 0; i < sizeof(LOG_LEVELS) / sizeof(LOG_LEVELS[0]); ++i) {
        if (active.logLevel == LOG_LEVELS[i]) {
            esp_log_level_set("*", LOG_LEVEL_VALUES[i]);
            LOG_I("Poziom logow: %s", LOG_LEVELS[i]);
            return;
        }
    }
}

void GlobalSettings::applyLanguage() const
{
    i18n::Lang lang = i18n::Lang::PL;
    if (!i18n::parse(active.language, &lang)) {
        i18n::parse(dflt::LANGUAGE, &lang);
    }
    i18n::setLanguage(lang);
    LOG_I("Jezyk: %s", i18n::code(lang));
}
