/**
 * @file Defaults.h
 * @brief Factory settings of all modules and the ranges of numeric fields.
 *
 * Values used when the file /config/<module>.json or a field is missing, when a value is invalid,
 * and after a factory reset.
 *
 * Every value can be overridden at build time without changing the code, e.g. in platformio.ini:
 *   build_flags = -DDEFAULT_DHCP_LEASE_TIME=3600 -DDEFAULT_GLOBAL_HOSTNAME=\"router\"
 * A value out of range stops the build (static_assert at the end of the file).
 *
 * Secrets (default AP SSID and password, panel password) are in include/Secrets.h (not in git).
 * The table in doc/configuration.md is generated from this file by tools/defaults_table.py –
 * run it after a change.
 *
 * Format (read by the script): macro DEFAULT_<MODULE>_<NAME> with the value; in namespace
 * defaults::<module> the constant <NAME> = macro and optionally <NAME>_MIN / <NAME>_MAX.
 */
#pragma once

#include <cstdint>

#include "esp_netif_ip_addr.h"
#include "esp_wifi_types.h"
#include "Secrets.h"

// Fallbacks when Secrets.h does not define the values.
#ifndef SECRET_AP_SSID
#define SECRET_AP_SSID "Esp32Router"
#endif
#ifndef SECRET_AP_PASS
#define SECRET_AP_PASS ""
#endif

// ---- global.json ----
#ifndef DEFAULT_GLOBAL_HOSTNAME
#define DEFAULT_GLOBAL_HOSTNAME "esp32router"
#endif
#ifndef DEFAULT_GLOBAL_LANGUAGE
#define DEFAULT_GLOBAL_LANGUAGE "pl"
#endif
#ifndef DEFAULT_GLOBAL_CONSOLE_UTF8
#define DEFAULT_GLOBAL_CONSOLE_UTF8 false // COM/Telnet console in UTF-8 (false = Polish letters as ASCII)
#endif
#ifndef DEFAULT_GLOBAL_LOG_LEVEL
#define DEFAULT_GLOBAL_LOG_LEVEL "info"
#endif
#ifndef DEFAULT_GLOBAL_LOG_OUTPUT
#define DEFAULT_GLOBAL_LOG_OUTPUT "console"
#endif
#ifndef DEFAULT_GLOBAL_NTP_ENABLED
#define DEFAULT_GLOBAL_NTP_ENABLED true
#endif
#ifndef DEFAULT_GLOBAL_NTP_SERVER
#define DEFAULT_GLOBAL_NTP_SERVER "pool.ntp.org"
#endif
#ifndef DEFAULT_GLOBAL_TIMEZONE
#define DEFAULT_GLOBAL_TIMEZONE "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef DEFAULT_GLOBAL_MGMT_FROM_STA
#define DEFAULT_GLOBAL_MGMT_FROM_STA false
#endif
#ifndef DEFAULT_GLOBAL_WAN_PING
#define DEFAULT_GLOBAL_WAN_PING false
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_SERIAL
#define DEFAULT_GLOBAL_AUTOSTART_SERIAL true
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_TELNET
#define DEFAULT_GLOBAL_AUTOSTART_TELNET true
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_DHCP
#define DEFAULT_GLOBAL_AUTOSTART_DHCP true
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_DNS
#define DEFAULT_GLOBAL_AUTOSTART_DNS true
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_DDNS
#define DEFAULT_GLOBAL_AUTOSTART_DDNS false
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_FIREWALL
#define DEFAULT_GLOBAL_AUTOSTART_FIREWALL false
#endif
#ifndef DEFAULT_GLOBAL_AUTOSTART_WEB
#define DEFAULT_GLOBAL_AUTOSTART_WEB true
#endif

// ---- wifi.json ----
#ifndef DEFAULT_WIFI_STA_SSID
#define DEFAULT_WIFI_STA_SSID "" // empty = do not connect
#endif
#ifndef DEFAULT_WIFI_STA_PASS
#define DEFAULT_WIFI_STA_PASS ""
#endif
#ifndef DEFAULT_WIFI_AP_SSID
#define DEFAULT_WIFI_AP_SSID SECRET_AP_SSID // + "-XXXX" (last 2 bytes of the AP MAC)
#endif
#ifndef DEFAULT_WIFI_AP_PASS
#define DEFAULT_WIFI_AP_PASS SECRET_AP_PASS // empty = open network
#endif
#ifndef DEFAULT_WIFI_AP_MAX_CLIENTS
#define DEFAULT_WIFI_AP_MAX_CLIENTS 8
#endif
#ifndef DEFAULT_WIFI_AP_IP
#define DEFAULT_WIFI_AP_IP ESP_IP4TOADDR(192, 168, 4, 1)
#endif
#ifndef DEFAULT_WIFI_AP_MASK
#define DEFAULT_WIFI_AP_MASK ESP_IP4TOADDR(255, 255, 255, 0)
#endif

// ---- nat.json ----
#ifndef DEFAULT_NAT_ENABLED
#define DEFAULT_NAT_ENABLED true
#endif

// ---- dhcp.json ----
#ifndef DEFAULT_DHCP_POOL_FIRST_HOST
#define DEFAULT_DHCP_POOL_FIRST_HOST 100 // pool: <AP network>.100 – .199
#endif
#ifndef DEFAULT_DHCP_POOL_LAST_HOST
#define DEFAULT_DHCP_POOL_LAST_HOST 199
#endif
#ifndef DEFAULT_DHCP_LEASE_TIME
#define DEFAULT_DHCP_LEASE_TIME 7200
#endif
#ifndef DEFAULT_DHCP_DNS
#define DEFAULT_DHCP_DNS "auto"
#endif

// ---- dns.json ----
#ifndef DEFAULT_DNS_DOMAIN
#define DEFAULT_DNS_DOMAIN "lan"
#endif
#ifndef DEFAULT_DNS_LOCAL_TTL
#define DEFAULT_DNS_LOCAL_TTL 300
#endif
#ifndef DEFAULT_DNS_CACHE_SIZE
#define DEFAULT_DNS_CACHE_SIZE 256
#endif
#ifndef DEFAULT_DNS_CACHE_SWEEP_S
#define DEFAULT_DNS_CACHE_SWEEP_S 60
#endif
#ifndef DEFAULT_DNS_DHCP_NAMES
#define DEFAULT_DNS_DHCP_NAMES true
#endif
#ifndef DEFAULT_DNS_BLOCK_ENABLED
#define DEFAULT_DNS_BLOCK_ENABLED true
#endif
#ifndef DEFAULT_DNS_BLOCK_RESPONSE
#define DEFAULT_DNS_BLOCK_RESPONSE "zero"
#endif

// ---- telnet.json ----
#ifndef DEFAULT_TELNET_PORT
#define DEFAULT_TELNET_PORT 23
#endif
#ifndef DEFAULT_TELNET_MAX_SESSIONS
#define DEFAULT_TELNET_MAX_SESSIONS 2
#endif
#ifndef DEFAULT_TELNET_IDLE_TIMEOUT_S
#define DEFAULT_TELNET_IDLE_TIMEOUT_S 600 // 0 = no limit
#endif

// ---- web.json ----
#ifndef DEFAULT_WEB_PORT
#define DEFAULT_WEB_PORT 80
#endif
#ifndef DEFAULT_WEB_SESSION_TIMEOUT_S
#define DEFAULT_WEB_SESSION_TIMEOUT_S 1800
#endif
#ifndef DEFAULT_WEB_MAX_SESSIONS
#define DEFAULT_WEB_MAX_SESSIONS 4
#endif

// ---- ddns.json ----
#ifndef DEFAULT_DDNS_IP_SOURCE
#define DEFAULT_DDNS_IP_SOURCE "public"
#endif
#ifndef DEFAULT_DDNS_CHECK_URL
#define DEFAULT_DDNS_CHECK_URL "https://api.ipify.org"
#endif
#ifndef DEFAULT_DDNS_CHECK_INTERVAL_S
#define DEFAULT_DDNS_CHECK_INTERVAL_S 300
#endif
#ifndef DEFAULT_DDNS_FORCE_UPDATE_H
#define DEFAULT_DDNS_FORCE_UPDATE_H 24
#endif
#ifndef DEFAULT_DDNS_RETRY_S
#define DEFAULT_DDNS_RETRY_S 300
#endif
#ifndef DEFAULT_DDNS_ENABLED
#define DEFAULT_DDNS_ENABLED false // account; the user enables it after entering the data
#endif
#ifndef DEFAULT_DDNS_PROVIDER
#define DEFAULT_DDNS_PROVIDER "duckdns"
#endif
#ifndef DEFAULT_DDNS_HOSTNAME
#define DEFAULT_DDNS_HOSTNAME ""
#endif
#ifndef DEFAULT_DDNS_USERNAME
#define DEFAULT_DDNS_USERNAME ""
#endif
#ifndef DEFAULT_DDNS_SECRET
#define DEFAULT_DDNS_SECRET ""
#endif
#ifndef DEFAULT_DDNS_CUSTOM_URL
#define DEFAULT_DDNS_CUSTOM_URL ""
#endif

// ---- firewall.json ----
#ifndef DEFAULT_FIREWALL_LOG_BLOCKED
#define DEFAULT_FIREWALL_LOG_BLOCKED true
#endif

/// Factory settings and ranges as constants (values from the DEFAULT_* macros above).
namespace defaults {

/// Factory settings of global.json; NAME_MIN / NAME_MAX = allowed range.
namespace global {
constexpr int VERSION = 1; // format version of global.json (not a setting)
constexpr const char* HOSTNAME = DEFAULT_GLOBAL_HOSTNAME;
constexpr const char* LANGUAGE = DEFAULT_GLOBAL_LANGUAGE; // pl|en
constexpr bool CONSOLE_UTF8 = DEFAULT_GLOBAL_CONSOLE_UTF8;
constexpr const char* LOG_LEVEL = DEFAULT_GLOBAL_LOG_LEVEL;
constexpr const char* LOG_OUTPUT = DEFAULT_GLOBAL_LOG_OUTPUT;
constexpr bool NTP_ENABLED = DEFAULT_GLOBAL_NTP_ENABLED;
constexpr const char* NTP_SERVER = DEFAULT_GLOBAL_NTP_SERVER;
constexpr const char* TIMEZONE = DEFAULT_GLOBAL_TIMEZONE;
constexpr bool MGMT_FROM_STA = DEFAULT_GLOBAL_MGMT_FROM_STA;
constexpr bool WAN_PING = DEFAULT_GLOBAL_WAN_PING;
constexpr bool AUTOSTART_SERIAL = DEFAULT_GLOBAL_AUTOSTART_SERIAL;
constexpr bool AUTOSTART_TELNET = DEFAULT_GLOBAL_AUTOSTART_TELNET;
constexpr bool AUTOSTART_DHCP = DEFAULT_GLOBAL_AUTOSTART_DHCP;
constexpr bool AUTOSTART_DNS = DEFAULT_GLOBAL_AUTOSTART_DNS;
constexpr bool AUTOSTART_DDNS = DEFAULT_GLOBAL_AUTOSTART_DDNS;
constexpr bool AUTOSTART_FIREWALL = DEFAULT_GLOBAL_AUTOSTART_FIREWALL;
constexpr bool AUTOSTART_WEB = DEFAULT_GLOBAL_AUTOSTART_WEB;
}

/// Factory settings of wifi.json; NAME_MIN / NAME_MAX = allowed range.
namespace wifi {
constexpr const char* STA_SSID = DEFAULT_WIFI_STA_SSID;
constexpr const char* STA_PASS = DEFAULT_WIFI_STA_PASS;
constexpr const char* AP_SSID = DEFAULT_WIFI_AP_SSID;
constexpr const char* AP_PASS = DEFAULT_WIFI_AP_PASS;
constexpr int AP_MAX_CLIENTS = DEFAULT_WIFI_AP_MAX_CLIENTS;
constexpr int AP_MAX_CLIENTS_MIN = 1;
constexpr int AP_MAX_CLIENTS_MAX = ESP_WIFI_MAX_CONN_NUM; // driver limit
constexpr uint32_t AP_IP = DEFAULT_WIFI_AP_IP;
constexpr uint32_t AP_MASK = DEFAULT_WIFI_AP_MASK;
}

/// Factory settings of nat.json; NAME_MIN / NAME_MAX = allowed range.
namespace nat {
constexpr bool ENABLED = DEFAULT_NAT_ENABLED;
}

/// Factory settings of dhcp.json; NAME_MIN / NAME_MAX = allowed range.
namespace dhcp {
constexpr uint32_t POOL_FIRST_HOST = DEFAULT_DHCP_POOL_FIRST_HOST;
constexpr uint32_t POOL_LAST_HOST = DEFAULT_DHCP_POOL_LAST_HOST;
constexpr int LEASE_TIME = DEFAULT_DHCP_LEASE_TIME;
constexpr int LEASE_TIME_MIN = 60;
constexpr int LEASE_TIME_MAX = 604800;
constexpr const char* DNS = DEFAULT_DHCP_DNS;
}

/// Factory settings of dns.json; NAME_MIN / NAME_MAX = allowed range.
namespace dns {
constexpr const char* DOMAIN = DEFAULT_DNS_DOMAIN;
constexpr int LOCAL_TTL = DEFAULT_DNS_LOCAL_TTL;
constexpr int LOCAL_TTL_MIN = 10;
constexpr int LOCAL_TTL_MAX = 86400;
constexpr int CACHE_SIZE = DEFAULT_DNS_CACHE_SIZE;
constexpr int CACHE_SIZE_MIN = 16;
constexpr int CACHE_SIZE_MAX = 4096;
constexpr int CACHE_SWEEP_S = DEFAULT_DNS_CACHE_SWEEP_S;
constexpr int CACHE_SWEEP_S_MIN = 10;
constexpr int CACHE_SWEEP_S_MAX = 3600;
constexpr bool DHCP_NAMES = DEFAULT_DNS_DHCP_NAMES;
constexpr bool BLOCK_ENABLED = DEFAULT_DNS_BLOCK_ENABLED;
constexpr const char* BLOCK_RESPONSE = DEFAULT_DNS_BLOCK_RESPONSE;
}

/// Factory settings of telnet.json; NAME_MIN / NAME_MAX = allowed range.
namespace telnet {
constexpr int PORT = DEFAULT_TELNET_PORT;
constexpr int PORT_MIN = 1;
constexpr int PORT_MAX = 65535;
constexpr int MAX_SESSIONS = DEFAULT_TELNET_MAX_SESSIONS;
constexpr int MAX_SESSIONS_MIN = 1;
constexpr int MAX_SESSIONS_MAX = 4; // size of the session table
constexpr int IDLE_TIMEOUT_S = DEFAULT_TELNET_IDLE_TIMEOUT_S;
constexpr int IDLE_TIMEOUT_S_MIN = 0;
constexpr int IDLE_TIMEOUT_S_MAX = 86400;
}

/// Factory settings of web.json; NAME_MIN / NAME_MAX = allowed range.
namespace web {
constexpr int PORT = DEFAULT_WEB_PORT;
constexpr int PORT_MIN = 1;
constexpr int PORT_MAX = 65535;
constexpr int SESSION_TIMEOUT_S = DEFAULT_WEB_SESSION_TIMEOUT_S;
constexpr int SESSION_TIMEOUT_S_MIN = 60;
constexpr int SESSION_TIMEOUT_S_MAX = 86400;
constexpr int MAX_SESSIONS = DEFAULT_WEB_MAX_SESSIONS;
constexpr int MAX_SESSIONS_MIN = 1;
constexpr int MAX_SESSIONS_MAX = 8; // size of the session table
}

/// Factory settings of ddns.json (account fields apply to each of the 3 accounts); NAME_MIN / NAME_MAX = allowed range.
namespace ddns {
constexpr const char* IP_SOURCE = DEFAULT_DDNS_IP_SOURCE;
constexpr const char* CHECK_URL = DEFAULT_DDNS_CHECK_URL;
constexpr int CHECK_INTERVAL_S = DEFAULT_DDNS_CHECK_INTERVAL_S;
constexpr int CHECK_INTERVAL_S_MIN = 300;
constexpr int CHECK_INTERVAL_S_MAX = 21600;
constexpr int FORCE_UPDATE_H = DEFAULT_DDNS_FORCE_UPDATE_H;
constexpr int FORCE_UPDATE_H_MIN = 0;
constexpr int FORCE_UPDATE_H_MAX = 720;
constexpr int RETRY_S = DEFAULT_DDNS_RETRY_S;
constexpr int RETRY_S_MIN = 60;
constexpr int RETRY_S_MAX = 3600;
constexpr bool ENABLED = DEFAULT_DDNS_ENABLED;
constexpr const char* PROVIDER = DEFAULT_DDNS_PROVIDER;
constexpr const char* HOSTNAME = DEFAULT_DDNS_HOSTNAME;
constexpr const char* USERNAME = DEFAULT_DDNS_USERNAME;
constexpr const char* SECRET = DEFAULT_DDNS_SECRET;
constexpr const char* CUSTOM_URL = DEFAULT_DDNS_CUSTOM_URL;
}

/// Factory settings of firewall.json; NAME_MIN / NAME_MAX = allowed range.
namespace firewall {
constexpr bool LOG_BLOCKED = DEFAULT_FIREWALL_LOG_BLOCKED;
}

// Values overridden by build_flags must stay within their ranges.

#define DEFAULTS_IN_RANGE(ns, n) static_assert(ns::n >= ns::n##_MIN && ns::n <= ns::n##_MAX, #ns "::" #n " poza zakresem")
DEFAULTS_IN_RANGE(wifi, AP_MAX_CLIENTS);
DEFAULTS_IN_RANGE(dhcp, LEASE_TIME);
DEFAULTS_IN_RANGE(dns, LOCAL_TTL);
DEFAULTS_IN_RANGE(dns, CACHE_SIZE);
DEFAULTS_IN_RANGE(dns, CACHE_SWEEP_S);
DEFAULTS_IN_RANGE(telnet, PORT);
DEFAULTS_IN_RANGE(telnet, MAX_SESSIONS);
DEFAULTS_IN_RANGE(telnet, IDLE_TIMEOUT_S);
DEFAULTS_IN_RANGE(web, PORT);
DEFAULTS_IN_RANGE(web, SESSION_TIMEOUT_S);
DEFAULTS_IN_RANGE(web, MAX_SESSIONS);
DEFAULTS_IN_RANGE(ddns, CHECK_INTERVAL_S);
DEFAULTS_IN_RANGE(ddns, FORCE_UPDATE_H);
DEFAULTS_IN_RANGE(ddns, RETRY_S);
#undef DEFAULTS_IN_RANGE
/// Compile-time string comparison (used by the language check below).
constexpr bool sameText(const char* a, const char* b)
{
    return *a == *b && (*a == '\0' || sameText(a + 1, b + 1));
}
static_assert(sameText(global::LANGUAGE, "pl") || sameText(global::LANGUAGE, "en"), "global::LANGUAGE: pl albo en");
static_assert(dhcp::POOL_FIRST_HOST >= 1 && dhcp::POOL_FIRST_HOST <= dhcp::POOL_LAST_HOST && dhcp::POOL_LAST_HOST <= 254,
              "dhcp::POOL_* poza zakresem 1-254");

}
