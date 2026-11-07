/**
 * @file TimeService.cpp
 * @brief TimeService: time zone and SNTP.
 */
#include "TimeService.h"

#include <cstdlib>
#include <ctime>
#include <sys/time.h>

#include "esp_netif_sntp.h"

#define LOG_TAG     "TimeService"
#define LOG_ENABLED 1
#include "Log.h"

TimeService* TimeService::instance = nullptr;

TimeService::TimeService(const GlobalSettings& global)
    : global(global)
{
}

void TimeService::start()
{
    instance = this;
    setenv("TZ", global.values().time.timezone.c_str(), 1);
    tzset();

    if (!global.values().time.ntpEnabled) {
        LOG_I("NTP wylaczony");
        return;
    }
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onGotIp, this, nullptr));
}

void TimeService::onGotIp(void* arg, esp_event_base_t, int32_t, void*)
{
    auto* self = static_cast<TimeService*>(arg);
    if (self->sntpStarted) {
        return; // SNTP refreshes the time every hour by itself
    }

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(self->global.values().time.ntpServer.c_str());
    cfg.sync_cb = &onSync;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        LOG_E("Nie udalo sie uruchomic SNTP: %s", esp_err_to_name(err));
        return;
    }
    self->sntpStarted = true;
    LOG_I("SNTP uruchomiony, serwer %s", self->global.values().time.ntpServer.c_str());
}

void TimeService::onSync(struct timeval*)
{
    if (instance == nullptr) {
        return;
    }
    bool first = !instance->isSynced.exchange(true);
    if (first) {
        char buf[40];
        instance->formatNow(buf, sizeof(buf));
        LOG_I("Czas zsynchronizowany: %s", buf);
    }
}

void TimeService::formatNow(char* buf, size_t size) const
{
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S %Z", &tm);
}
