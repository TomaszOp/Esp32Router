/**
 * @file TimeService.h
 * @brief Time zone and SNTP clock synchronization.
 */
#pragma once

#include <atomic>
#include <cstddef>

#include "esp_event.h"

#include "GlobalSettings.h"

/**
 * @brief Sets the time zone and synchronizes the clock with SNTP after the STA gets an IP address.
 *
 * Settings come from global.json "time" (applied at start-up). Used by the console, the web
 * panel (header time) and DDNS (time of the last update).
 */
class TimeService {
public:
    explicit TimeService(const GlobalSettings& global);

    /// @brief Sets the time zone and registers for IP_EVENT_STA_GOT_IP (SNTP starts after the first IP).
    void start();

    /// @return true after the first successful SNTP synchronization.
    bool synced() const { return isSynced; }
    /// @brief Current local time, e.g. "2026-11-05 22:10:03 CET".
    void formatNow(char* buf, size_t size) const;

private:
    /// Event handler: starts SNTP once when the STA gets an address.
    static void onGotIp(void* arg, esp_event_base_t base, int32_t id, void* data);
    /// SNTP callback: marks the clock as synchronized.
    static void onSync(struct timeval* tv);

    static TimeService* instance; ///< The SNTP callback has no context argument.

    const GlobalSettings& global;         ///< Time settings.
    bool sntpStarted = false;             ///< SNTP already started.
    std::atomic<bool> isSynced { false }; ///< Clock synchronized at least once.
};
