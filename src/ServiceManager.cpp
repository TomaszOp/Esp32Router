/**
 * @file ServiceManager.cpp
 * @brief ServiceManager: service registry, autostart and "service" commands.
 */
#include "ServiceManager.h"

#include "i18n/I18n.h"

#include <algorithm>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "StringOutput.h"

#define LOG_TAG     "ServiceManager"
#define LOG_ENABLED 1
#include "Log.h"

// Delay before stopping a service from its own session – so that the response gets out.
static constexpr int DEFERRED_DELAY_MS = 300;

// Operation name for logs and messages.
static const char* opName(int op)
{
    static const char* const names[] = { "start", "stop", "restart" };
    return names[op];
}

ServiceManager::ServiceManager(const GlobalSettings& global)
    : global(global)
{
}

void ServiceManager::add(IService& service)
{
    auto it = std::lower_bound(services.begin(), services.end(), service.name(),
                               [](const IService* s, const char* name) { return strcmp(s->name(), name) < 0; });
    services.insert(it, &service);
}

void ServiceManager::startAutostart()
{
    std::lock_guard<std::mutex> lock(mutex);
    const ServiceSettings& auto_ = global.values().services;

    bool anyConsole = false;
    for (IService* s : services) {
        if (s->isConsole() && auto_.autostart(s->name())) {
            anyConsole = true;
        }
    }

    for (IService* s : services) {
        bool start = auto_.autostart(s->name());
        if (!start && !anyConsole && strcmp(s->name(), "serial") == 0) {
            LOG_W("Autostart wylaczony dla wszystkich konsol - uruchamiam mimo to 'serial'");
            start = true;
        }
        if (start) {
            esp_err_t err = s->start();
            if (err != ESP_OK) {
                LOG_E("Usluga %s nie wystartowala: %s", s->name(), esp_err_to_name(err));
            }
        } else {
            LOG_I("Usluga %s: autostart wylaczony", s->name());
        }
    }
}

// Service name or unambiguous prefix; binary search.
IService* ServiceManager::find(const char* token, IOutput& out) const
{
    auto it = std::lower_bound(services.begin(), services.end(), token,
                               [](const IService* s, const char* t) { return strcmp(s->name(), t) < 0; });
    size_t len = strlen(token);
    if (it != services.end() && strncmp((*it)->name(), token, len) == 0) {
        bool exact = strcmp((*it)->name(), token) == 0;
        bool ambiguous = !exact && (it + 1) != services.end() && strncmp((*(it + 1))->name(), token, len) == 0;
        if (!ambiguous) {
            return *it;
        }
    }
    out.printf(tr("Nieznana usługa '%s'. Dostępne:", "Unknown service '%s'. Available:"), token);
    for (const IService* s : services) {
        out.printf(" %s", s->name());
    }
    out.print("\r\n");
    return nullptr;
}

bool ServiceManager::otherConsoleRunning(const IService* except) const
{
    for (const IService* s : services) {
        if (s != except && s->isConsole() && s->isRunning()) {
            return true;
        }
    }
    return false;
}

esp_err_t ServiceManager::perform(IService* s, Op op)
{
    esp_err_t err = ESP_OK;
    if (op == Op::STOP || op == Op::RESTART) {
        err = s->stop();
    }
    if (err == ESP_OK && (op == Op::START || op == Op::RESTART)) {
        err = s->start();
    }
    if (err == ESP_OK) {
        LOG_I("Usluga %s: %s", s->name(), opName(static_cast<int>(op)));
    } else {
        LOG_E("Usluga %s: %s nie powiodl sie: %s", s->name(), opName(static_cast<int>(op)), esp_err_to_name(err));
    }
    return err;
}

void ServiceManager::deferredTask(void* arg)
{
    auto* job = static_cast<Job*>(arg);
    vTaskDelay(pdMS_TO_TICKS(DEFERRED_DELAY_MS));
    {
        std::lock_guard<std::mutex> lock(job->self->mutex);
        job->self->perform(job->service, job->op);
    }
    delete job;
    vTaskDelete(nullptr);
}

// A command from the service's own session (e.g. "service stop telnet" from Telnet) is executed
// in a separate task after a moment – the session task cannot wait for its own end.
CmdResult ServiceManager::run(IService* s, Op op, IOutput& out)
{
    if (op != Op::START && s->owns(out)) {
        out.printf(tr("Uwaga: %s usługi %s zamknie tę sesję.\r\n", "Note: %s of service %s will close this session.\r\n"),
                   opName(static_cast<int>(op)), s->name());
        auto* job = new Job { this, s, op };
        if (xTaskCreate(&deferredTask, "svc", 4096, job, 5, nullptr) != pdPASS) {
            delete job;
            out.print(tr("Brak pamięci na zadanie\r\n", "Out of memory for the task\r\n"));
            return CmdResult::ERR_FAIL;
        }
        return CmdResult::OK;
    }

    std::lock_guard<std::mutex> lock(mutex);
    esp_err_t err = perform(s, op);
    if (err != ESP_OK) {
        out.printf(tr("Błąd: %s\r\n", "Error: %s\r\n"), esp_err_to_name(err));
        return CmdResult::ERR_FAIL;
    }
    out.printf("%s: %s\r\n", s->name(), s->isRunning() ? tr("działa", "running") : tr("zatrzymana", "stopped"));
    return CmdResult::OK;
}

void ServiceManager::restartDeferred(const char* name)
{
    for (IService* s : services) {
        if (strcmp(s->name(), name) != 0 || !s->isRunning()) {
            continue;
        }
        auto* job = new Job { this, s, Op::RESTART };
        if (xTaskCreate(&deferredTask, "svc", 4096, job, 5, nullptr) != pdPASS) {
            delete job;
        }
    }
}

void ServiceManager::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("service", { "usługi: ddns, dhcp, dns, firewall, serial, telnet, web", "services: ddns, dhcp, dns, firewall, serial, telnet, web" });
    exec.add("service", { "list", "", { "stan usług i autostart", "service status and autostart" }, &cmdList, this });
    exec.add("service", { "start", "<nazwa>", { "uruchamia usługę", "starts a service" }, &cmdStart, this });
    exec.add("service", { "stop", "<nazwa> [yes]", { "zatrzymuje usługę ('yes' gdy to ostatnia konsola)", "stops a service ('yes' if it is the last console)" }, &cmdStop, this });
    exec.add("service", { "restart", "<nazwa>", { "restart usługi z bieżącą konfiguracją", "restarts a service with the current configuration" }, &cmdRestart, this });
}

CmdResult ServiceManager::cmdList(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<ServiceManager*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    // Autostart according to the current configuration (applies to the next start).
    const ServiceSettings& auto_ = self->global.configured().services;
    out.print(tr("  usługa   stan        autostart\r\n", "  service  state       autostart\r\n"));
    for (const IService* s : self->services) {
        out.printf("  %-8s %s %s", s->name(), i18n::pad(s->isRunning() ? tr("działa", "running") : tr("zatrzymana", "stopped"), 11).c_str(),
                   i18n::pad(auto_.autostart(s->name()) ? tr("tak", "yes") : tr("nie", "no"), 9).c_str());
        s->describe(out);
        out.print("\r\n");
    }
    return CmdResult::OK;
}

CmdResult ServiceManager::cmdStart(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ServiceManager*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    IService* s = self->find(argv[1], out);
    return s ? self->run(s, Op::START, out) : CmdResult::ERR_FAIL;
}

CmdResult ServiceManager::cmdStop(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ServiceManager*>(ctx);
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "yes") != 0)) {
        return CmdResult::ERR_ARGS;
    }
    IService* s = self->find(argv[1], out);
    if (s == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    if (s->isConsole() && s->isRunning() && !self->otherConsoleRunning(s) && argc != 3) {
        out.printf(tr("'%s' to ostatnia działająca konsola – po zatrzymaniu zostanie tylko restart urządzenia.\r\n"
                      "Jeśli na pewno: service stop %s yes\r\n",
                      "'%s' is the last running console – after stopping it only a device restart remains.\r\n"
                      "If you are sure: service stop %s yes\r\n"),
                   s->name(), s->name());
        return CmdResult::ERR_FAIL;
    }
    return self->run(s, Op::STOP, out);
}

CmdResult ServiceManager::cmdRestart(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<ServiceManager*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    IService* s = self->find(argv[1], out);
    return s ? self->run(s, Op::RESTART, out) : CmdResult::ERR_FAIL;
}

cJSON* ServiceManager::listJson() const
{
    cJSON* list = cJSON_CreateArray();
    const ServiceSettings& auto_ = global.configured().services;
    for (const IService* s : services) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->name());
        cJSON_AddBoolToObject(o, "running", s->isRunning());
        cJSON_AddBoolToObject(o, "autostart", auto_.autostart(s->name()));
        cJSON_AddBoolToObject(o, "console", s->isConsole());
        StringOutput details(256);
        s->describe(details);
        cJSON_AddStringToObject(o, "details", details.text.c_str());
        cJSON_AddItemToArray(list, o);
    }
    return list;
}
