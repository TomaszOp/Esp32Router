/**
 * @file TelnetServer.h
 * @brief Telnet server with the command console.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "GlobalSettings.h"
#include "IOutput.h"
#include "IService.h"
#include "LineEditor.h"
#include "Defaults.h"
#include "WifiManager.h"

/**
 * @brief Telnet server with the command console.
 *
 * No password; access only from the AP side (unless global.json mgmt_from_sta = true). An accept
 * task plus one FreeRTOS task per session. Configuration in /config/telnet.json (changes apply
 * after a service or device restart). Service "telnet": start/stop/restart at run time
 * (ServiceManager).
 */
class TelnetServer : public IConfigurable, public IService {
public:
    static constexpr int MAX_SESSIONS_LIMIT = defaults::telnet::MAX_SESSIONS_MAX; ///< Size of the session table.

    TelnetServer(const CommandExecutor& executor, const GlobalSettings& global, WifiManager& wifi);

    const char* configName() const override { return "telnet"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;

    const char* name() const override { return "telnet"; }
    /// Copies the configuration, opens the listening socket and starts the accept task.
    esp_err_t start() override;
    /// Closes the listening socket and all sessions, waits for the tasks.
    esp_err_t stop() override;
    bool isRunning() const override { return running; }
    bool isConsole() const override { return true; }
    /// Whether the output is one of the Telnet sessions.
    bool owns(const IOutput& out) const override;
    /// " port N, sessions n/max" for "service list".
    void describe(IOutput& out) const override;
    /// @return Active listening port (PortForwarder collision warning).
    uint16_t listenPort() const { return port; }

    /// @brief Registers "telnet sessions/kick" and "exit".
    void registerCommands(CommandExecutor& exec);

private:
    /// One Telnet connection: output to the socket and its own line editor.
    struct Session : public IOutput {
        Session(TelnetServer& server, int sock);
        /// Writes to the socket, converting to ASCII unless console_utf8.
        void write(const char* data, size_t len) override;
        /// Sends bytes doubling 0xFF (IAC) as required by Telnet.
        void sendRaw(const char* data, size_t len);

        TelnetServer& server;    ///< Owner.
        int sock;                ///< Connected socket.
        int slot = -1;           ///< Index in sessions[].
        char peer[24] = {};      ///< "ip:port" of the client.
        int64_t connectedAt = 0; ///< Connection time (esp_timer).
        int telnetState = 0;     ///< IAC sequence parser state.
        LineEditor editor;       ///< Line editing.
    };

    /// Accept task entry.
    static void acceptTaskEntry(void* arg);
    /// Session task entry.
    static void sessionTaskEntry(void* arg);
    /// Accepts connections, rejects ones from the STA side (unless allowed) and above the session limit.
    void acceptLoop();
    /// Session loop: greeting, reading with idle timeout, closing.
    void runSession(Session* s);
    /// Skips Telnet IAC sequences, passes the rest to the line editor.
    void handleIncoming(Session* s, uint8_t c);
    /// Whether the connection came through the AP interface (or mgmt_from_sta is enabled).
    bool isAllowed(int sock) const;

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdSessions(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdKick(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdExit(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    const CommandExecutor& executor; ///< Command interpreter.
    const GlobalSettings& global;    ///< mgmt_from_sta.
    WifiManager& wifi;               ///< AP/STA interfaces (access check).

    /// telnet.json settings.
    struct Settings {
        uint16_t port;         ///< Listening port.
        int maxSessions;       ///< Maximum sessions.
        uint32_t idleTimeoutS; ///< Idle timeout [s]; 0 = none.
    };
    // cfg: loaded/changed by config set; the active fields below are copied from cfg in start().
    Settings cfg = {};                         ///< Configured settings.
    uint16_t port = 0;                         ///< Active port.
    int maxSessions = 0;                       ///< Active session limit.
    uint32_t idleTimeoutS = 0;                 ///< Active idle timeout.
    int listenSock = -1;                       ///< Listening socket.
    std::atomic<bool> running { false };       ///< Service running.
    std::atomic<bool> stopRequested { false }; ///< stop() asked the tasks to finish.
    std::atomic<bool> acceptAlive { false };   ///< Accept task still running.

    mutable std::mutex mutex;                   ///< Guards sessions[].
    Session* sessions[MAX_SESSIONS_LIMIT] = {};   ///< Active sessions.
};
