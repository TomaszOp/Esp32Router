/**
 * @file IService.h
 * @brief Interface of a service that can be stopped and started at run time.
 */
#pragma once

#include "esp_err.h"

#include "IOutput.h"

/**
 * @brief A service that can be stopped and started at run time (serial, telnet, dhcp, dns, web, ddns, firewall).
 *
 * start()/stop() are idempotent; ServiceManager calls them under its own mutex.
 */
class IService {
public:
    virtual ~IService() = default;

    /// @return Service name used in "service" commands and global.json "services".
    virtual const char* name() const = 0;
    /// @brief Starts the service (no-op when running). @return ESP_OK on success.
    virtual esp_err_t start() = 0;
    /// @brief Stops the service (no-op when stopped). @return ESP_OK on success.
    virtual esp_err_t stop() = 0;
    /// @return Whether the service is running.
    virtual bool isRunning() const = 0;

    /// @return true for a command console – counted by the protection against losing all access.
    virtual bool isConsole() const { return false; }
    /// @return Whether this output belongs to the service (a command issued from its own session).
    virtual bool owns(const IOutput& out) const { return false; }
    /// @brief Extra information for "service list" (e.g. port, sessions).
    virtual void describe(IOutput& out) const {}
};
