/**
 * @file DdnsClient.h
 * @brief Dynamic DNS client (up to 3 accounts).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CommandExecutor.h"
#include "ConfigReader.h"
#include "IService.h"
#include "TimeService.h"
#include "WifiManager.h"

/**
 * @brief Dynamic DNS client.
 *
 * Up to 3 accounts (slots 1-3) at DuckDNS, Dynu, No-IP, FreeDNS, deSEC or a custom service
 * (URL template). One public IP check shared by the accounts; an account is updated when the IP
 * changes, after force_update_h or on request. Configuration in /config/ddns.json; service "ddns".
 * Works in its own task (HTTPS requests block); settings and state are guarded by a mutex.
 */
class DdnsClient : public IConfigurable, public IService {
public:
    static constexpr int ACCOUNTS = 3; ///< Number of account slots.

    DdnsClient(WifiManager& wifi, const TimeService& time);

    const char* configName() const override { return "ddns"; }
    void loadConfig(ConfigReader& r) override;
    cJSON* saveConfig() const override;
    /// Everything applies immediately: a changed account resets its state (also a permanent error) and wakes the task.
    bool applyLive(const std::string& path) override;
    bool isLive(const std::string&) const override { return true; }

    const char* name() const override { return "ddns"; }
    /// Registers for STA IP events and starts the task.
    esp_err_t start() override;
    /// Stops the task.
    esp_err_t stop() override;
    bool isRunning() const override { return running; }
    /// " IP x, accounts N" for "service list".
    void describe(IOutput& out) const override;

    /// @brief Registers the "ddns" command.
    void registerCommands(CommandExecutor& exec);

    /// @return Web panel: state of the service and the accounts, without secrets (JSON, freed by the caller).
    cJSON* statusJson() const;

private:
    /// Account settings (ddns.json "1".."3").
    struct Account {
        bool enabled;          ///< Account enabled.
        std::string provider;  ///< duckdns / dynu / noip / freedns / desec / custom.
        std::string hostname;  ///< Host name(s), comma-separated.
        std::string username;  ///< Login (No-IP, Dynu, custom).
        std::string secret;    ///< Token or password.
        std::string customUrl; ///< URL template of a custom service.
    };

    /// Client settings (ddns.json).
    struct Settings {
        std::string ipSource;       ///< public / sta / provider.
        std::string checkUrl;       ///< Public IP check URL.
        uint32_t checkIntervalS;    ///< Check interval [s].
        uint32_t forceUpdateH;      ///< Forced update interval [h]; 0 = none.
        uint32_t retryS;            ///< Retry after a temporary error [s].
        Account accounts[ACCOUNTS]; ///< Account slots.
    };

    /// Account state (RAM only).
    struct State {
        std::string lastIp;    ///< Address sent with the last successful update.
        std::string result;    ///< Last result (text).
        int64_t lastAttemptUs; ///< Time of the last attempt.
        int64_t lastOkUs;      ///< Time of the last success.
        int64_t nextRetryUs;   ///< Next retry after a temporary error.
        uint32_t backoffS;     ///< Current retry interval (doubled up to a limit).
        int failures;          ///< Consecutive failures.
        bool fatal;            ///< Permanent error – waits for a configuration change or "ddns update".
        bool forceNow;         ///< Update on the next loop pass ("ddns update").
    };

    /// Classification of a provider response.
    enum class Outcome { OK, TEMPORARY, FATAL };

    /// Task entry.
    static void taskEntry(void* arg);
    /// Event handler: new STA address – check the public IP now.
    static void onGotIp(void* arg, esp_event_base_t base, int32_t id, void* data);
    /// Task loop: public IP check, account updates, sleep until the next deadline or a wake-up.
    void run();
    /// Reads the public IP from the check URL (ip_source "public").
    void checkPublicIp(const Settings& s);
    /// Updates one account and records the outcome.
    void updateAccount(int i, const Account& a, const std::string& ip);
    /// HTTPS GET request. @return false = no connection (body: error description).
    bool httpGet(const std::string& url, const char* user, const char* pass, int* status, std::string* body);
    /// Classifies a provider response as success, temporary or permanent error.
    static Outcome classify(const std::string& provider, int status, const std::string& body);
    /// @return Why an account is incomplete (translated text) or nullptr.
    static const char* accountProblem(const Account& a);
    /// Wakes the task and waits for one loop pass (commands wait for the result).
    void wakeAndWait(int timeoutMs);

    /// "ddns [update [1-3] | test]" handler (ctx = this).
    static CmdResult cmdDdns(void* ctx, int argc, char** argv, IOutput& out);
    /// Prints the state ("ddns").
    void printStatus(IOutput& out) const;

    WifiManager& wifi;       ///< STA address (ip_source "sta"), connection state.
    const TimeService& time; ///< Clock synchronization (HTTPS certificates).

    mutable std::mutex mutex;   ///< Settings and state (DDNS task vs. consoles and panel).
    Settings cfg = {};          ///< Settings.
    State state[ACCOUNTS] = {}; ///< Account states.
    std::string publicIp;       ///< Last public IP.
    std::string checkError;     ///< Last check error (empty = OK).
    int checkFailures = 0;      ///< Consecutive check failures (fallback to provider detection).
    int64_t lastCheckUs = 0;    ///< Time of the last check.
    int64_t nextCheckUs = 0;    ///< Time of the next check.
    bool checkNow = false;      ///< Check on the next loop pass.
    uint32_t cycles = 0;        ///< Counter of finished loop passes (to wait for a command result).

    TaskHandle_t task = nullptr;               ///< DDNS task.
    bool eventRegistered = false;              ///< IP event handler registered.
    std::atomic<bool> running { false };       ///< Service running.
    std::atomic<bool> stopRequested { false }; ///< stop() asked the task to finish.

};
