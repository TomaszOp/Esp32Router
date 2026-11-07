/**
 * @file WebPanel.h
 * @brief Web panel: HTTP server, panel files, JSON API, login and sessions, OTA, live logs.
 */
#pragma once

#include <cstdint>
#include <mutex>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "ConfigStore.h"
#include "GlobalSettings.h"
#include "DdnsClient.h"
#include "DhcpServer.h"
#include "DnsServer.h"
#include "Firewall.h"
#include "IService.h"
#include "NatManager.h"
#include "PortForwarder.h"
#include "ServiceManager.h"
#include "SystemCommands.h"
#include "Defaults.h"
#include "WifiManager.h"

/**
 * @brief Web panel (esp_http_server, HTTP).
 *
 * Files from web/ compiled into the firmware (gzip), password login, sessions in a cookie,
 * password change, JSON API for all pages, OTA upload, live logs over WebSocket. Access only from
 * the AP side (unless global.json mgmt_from_sta = true). Service "web" (ServiceManager).
 * Settings in /config/web.json; the password hash (PBKDF2) separately in /config/web_auth.json.
 * Handlers run in the httpd task; sessions and the password are guarded by a mutex.
 */
class WebPanel : public IConfigurable, public IService {
public:
    static constexpr int MAX_SESSIONS_LIMIT = defaults::web::MAX_SESSIONS_MAX; ///< Size of the session table.

    WebPanel(const GlobalSettings& global, WifiManager& wifi, ConfigStore& store, const CommandExecutor& executor,
             const SystemCommands& system, const ServiceManager& services, const DhcpServer& dhcp, PortForwarder& portfwd,
             const NatManager& nat, DnsServer& dns, const DdnsClient& ddns, Firewall& firewall);

    const char* configName() const override { return "web"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;

    const char* name() const override { return "web"; }
    /// Starts the HTTP server and registers all routes (the WebSocket and API before the "/*" file route).
    esp_err_t start() override;
    /// Stops the server and the live-log task; sessions are cleared.
    esp_err_t stop() override;
    bool isRunning() const override { return server != nullptr; }
    /// " port N, sessions n/max" for "service list".
    void describe(IOutput& out) const override;
    /// A command from /api/cmd (e.g. "service stop web") – ServiceManager defers it until the response is sent.
    bool owns(const IOutput& out) const override { return &out == currentCmdOutput; }

    /// @brief Loads the password hash from web_auth.json (at start-up, after the configuration is loaded).
    void loadAuth();
    /// @brief Registers the "web" command group.
    void registerCommands(CommandExecutor& exec);

private:
    /// web.json settings.
    struct Settings {
        uint16_t port;            ///< HTTP port.
        uint32_t sessionTimeoutS; ///< Logout after inactivity [s].
        int maxSessions;          ///< Maximum logged-in sessions.
    };

    /// Logged-in session.
    struct Session {
        bool used;          ///< Slot in use.
        char token[33];     ///< 128 bits as hex (cookie "sid").
        uint32_t ip;        ///< Client address, network byte order.
        int64_t createdUs;  ///< Login time.
        int64_t lastUsedUs; ///< Last request (inactivity timeout).
    };

    /// Failed logins from one address (lockout after 5 attempts).
    struct Failures {
        uint32_t ip;           ///< Client address.
        int count;             ///< Consecutive failures.
        int64_t lockedUntilUs; ///< End of the lockout.
    };

    /// @name HTTP handlers (req->user_ctx = this)
    /// @{
    /// GET of a panel file (gzip, ETag / 304); "/" = index.html.
    static esp_err_t handleAsset(httpd_req_t* req);
    static esp_err_t handleLogin(httpd_req_t* req);
    static esp_err_t handleLogout(httpd_req_t* req);
    static esp_err_t handleSession(httpd_req_t* req);
    static esp_err_t handlePasswd(httpd_req_t* req);
    static esp_err_t handleStatus(httpd_req_t* req);
    static esp_err_t handleConfigGet(httpd_req_t* req);
    static esp_err_t handleConfigPost(httpd_req_t* req);
    static esp_err_t handleSave(httpd_req_t* req);
    static esp_err_t handleRevert(httpd_req_t* req);
    static esp_err_t handleCmd(httpd_req_t* req);
    static esp_err_t handleServices(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleWifiSet(httpd_req_t* req);
    static esp_err_t handleOtaInfo(httpd_req_t* req);
    static esp_err_t handleOtaUpload(httpd_req_t* req);
    static esp_err_t handleBackup(httpd_req_t* req);
    static esp_err_t handleRestore(httpd_req_t* req);
    static esp_err_t handleReboot(httpd_req_t* req);
    /// /api/clients, /api/dhcp/*, /api/pf, /api/nat, /api/sys/mem, /api/ddns, /api/firewall.
    static esp_err_t handleJsonGet(httpd_req_t* req);
    static esp_err_t handlePfSet(httpd_req_t* req);
    static esp_err_t handleFwSet(httpd_req_t* req);
    /// /api/dns/names, stats, block, block/export.
    static esp_err_t handleDnsGet(httpd_req_t* req);
    static esp_err_t handleBlockImport(httpd_req_t* req);
    /// WebSocket /ws/log: registers a live-log client.
    static esp_err_t handleLogWs(httpd_req_t* req);
    /// @}
    /// Live-log task entry.
    static void logTaskEntry(void* arg);
    /// Live-log task: every 250 ms sends new LogBuffer data to the WebSocket clients.
    void logPump();

    /// Access (AP side), session and, for POST, the anti-CSRF header. false = the error response was already sent.
    bool guard(httpd_req_t* req);
    /// Restarts the device after a delay (so that the response gets out).
    void scheduleReboot(int delayMs);

    /// Whether the request came from the AP side (or mgmt_from_sta is enabled).
    bool allowed(httpd_req_t* req) const;
    /// Client address of the request (network byte order).
    static uint32_t peerIp(httpd_req_t* req);
    /// Session from the "sid" cookie, valid and from the same address; refreshes its time. With the mutex held.
    Session* currentSession(httpd_req_t* req);
    /// Sends a JSON body with the given status and frees it.
    static esp_err_t sendJson(httpd_req_t* req, const char* status, cJSON* body);
    /// Sends {"error": message} with the given status.
    static esp_err_t sendError(httpd_req_t* req, const char* status, const char* message);
    /// Reads and parses the request body (nullptr when too large or not JSON).
    static cJSON* readJsonBody(httpd_req_t* req, size_t limit = 512);

    /// Checks a password against the stored hash or the factory password from Secrets.h (constant time).
    bool checkPassword(const char* password) const;
    /// Stores a new password hash; nullptr = back to the password from Secrets.h.
    bool setPassword(const char* password);
    /// @return true while the factory password is in use.
    bool usingDefaultPassword() const { return !hasHash; }
    /// Logs out all sessions except one (nullptr = all).
    void logoutAll(const Session* except);

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdPasswd(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdSessions(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdLogoutAll(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    const GlobalSettings& global;              ///< mgmt_from_sta, host name.
    WifiManager& wifi;                         ///< Wi-Fi state and settings.
    ConfigStore& store;                        ///< Configuration API, backup, password file.
    const CommandExecutor& executor;           ///< /api/cmd.
    const SystemCommands& system;              ///< Status, OTA, clients, memory.
    const ServiceManager& services;            ///< Service list.
    const DhcpServer& dhcp;                    ///< Leases and reservations.
    PortForwarder& portfwd;                    ///< Port forwarding rules.
    const NatManager& nat;                     ///< NAT state.
    DnsServer& dns;                            ///< DNS data and block list import.
    const DdnsClient& ddns;                    ///< DDNS state.
    Firewall& firewall;                        ///< Firewall state and rules.
    const IOutput* currentCmdOutput = nullptr; ///< Output of the /api/cmd command being executed.

    /// Live logs: connected WebSocket client and its position in the log buffer.
    struct LogClient {
        int fd;       ///< Socket (0 = free slot).
        uint64_t pos; ///< Position in LogBuffer.
    };
    static constexpr int MAX_LOG_CLIENTS = 3;   ///< Maximum live-log viewers.
    LogClient logClients[MAX_LOG_CLIENTS] = {}; ///< Connected viewers.
    std::mutex logMutex;                        ///< Guards logClients.
    TaskHandle_t logTask = nullptr;             ///< Live-log task.
    volatile bool logTaskStop = false;          ///< Asks the live-log task to finish.

    Settings cfg = {};               ///< Loaded/changed by config set.
    Settings active = {};            ///< Used by the running server.
    httpd_handle_t server = nullptr; ///< HTTP server (nullptr = stopped).

    mutable std::mutex mutex;                  ///< Sessions, failure counters, password hash.
    Session sessions[MAX_SESSIONS_LIMIT] = {}; ///< Session table.
    Failures failures[8] = {};                 ///< Failed logins of recent addresses.
    bool hasHash = false;                      ///< A password hash is stored (otherwise the factory password).
    uint8_t salt[16] = {};                     ///< PBKDF2 salt.
    uint8_t hash[32] = {};                     ///< PBKDF2-SHA256 hash.

};
