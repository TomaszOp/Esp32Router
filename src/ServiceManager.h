/**
 * @file ServiceManager.h
 * @brief Service registry, autostart and the "service" commands.
 */
#pragma once

#include <mutex>
#include <vector>

#include "cJSON.h"

#include "CommandExecutor.h"
#include "GlobalSettings.h"
#include "IService.h"

/**
 * @brief Registry of services (sorted, binary search as in CommandExecutor), autostart
 * according to global.json "services" and the commands service list/start/stop/restart.
 *
 * Protects against stopping the last running console and warns when a command stops the
 * service that runs the current session.
 */
class ServiceManager {
public:
    explicit ServiceManager(const GlobalSettings& global);

    /// @brief Registers a service. Only at start-up.
    void add(IService& service);
    /// @brief Starts the services with autostart enabled in the configuration.
    void startAutostart();
    /// @brief Registers the "service" command group.
    void registerCommands(CommandExecutor& exec);

    /// @return Web panel: list of services with state, autostart and details (JSON, freed by the caller).
    cJSON* listJson() const;
    /**
     * @brief Restarts a running service in a separate task.
     *
     * For callers that must not wait, e.g. the event loop (AP restart: dhcp and dns).
     */
    void restartDeferred(const char* name);

private:
    /// Operation on a service.
    enum class Op { START, STOP, RESTART };

    /// Operation executed in a separate task (stopping the service that runs the session).
    struct Job {
        ServiceManager* self; ///< Owner.
        IService* service;    ///< Target service.
        Op op;                ///< Operation.
    };

    /// Service by name or unambiguous prefix; prints the available names when not found.
    IService* find(const char* token, IOutput& out) const;
    /// @return Whether a console other than @p except is running.
    bool otherConsoleRunning(const IService* except) const;
    /// Executes the operation under the mutex and logs the result.
    esp_err_t perform(IService* s, Op op);
    /// Executes a command: directly, or in a task when it would close the current session.
    CmdResult run(IService* s, Op op, IOutput& out);
    /// Task entry for Job (frees it when done).
    static void deferredTask(void* arg);

    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdList(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdStart(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdStop(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdRestart(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    const GlobalSettings& global;    ///< Autostart settings.
    std::vector<IService*> services; ///< Sorted by name.
    mutable std::mutex mutex;        ///< start/stop must not interleave.
};
