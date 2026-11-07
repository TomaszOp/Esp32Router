/**
 * @file NatManager.cpp
 * @brief NatManager: NAPT on the AP interface.
 */
#include "NatManager.h"

#include "i18n/I18n.h"

#include "Defaults.h"

#include "esp_netif.h"
#include "lwip/lwip_napt.h"
#include "lwip/stats.h"

#define LOG_TAG     "NatManager"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::nat; // factory settings of nat.json

NatManager::NatManager(WifiManager& wifi)
    : wifi(wifi)
{
}

void NatManager::loadConfig(ConfigReader& r)
{
    std::lock_guard<std::mutex> lock(mutex);
    wanted = r.boolean("enabled", dflt::ENABLED);
}

cJSON* NatManager::saveConfig() const
{
    std::lock_guard<std::mutex> lock(mutex);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", wanted);
    return root;
}

void NatManager::start()
{
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onGotIp, this, nullptr));
}

bool NatManager::applyLive(const std::string&)
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        applyLocked(wifi.staConnected());
    }
    if (listener != nullptr) {
        listener(listenerCtx);
    }
    return true;
}

void NatManager::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("nat", { "NAT (NAPT) na interfejsie AP", "NAT (NAPT) on the AP interface" });
    exec.add("nat", { "on", "", { "włącza NAT (trwałe po save)", "enables NAT (permanent after save)" }, &cmdOn, this });
    exec.add("nat", { "off", "", { "wyłącza NAT (trwałe po save)", "disables NAT (permanent after save)" }, &cmdOff, this });
}

bool NatManager::enabled() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return natEnabled;
}

cJSON* NatManager::statusJson() const
{
    cJSON* root = cJSON_CreateObject();
    {
        std::lock_guard<std::mutex> lock(mutex);
        cJSON_AddBoolToObject(root, "enabled", natEnabled);
        cJSON_AddBoolToObject(root, "wanted", wanted);
    }
    cJSON_AddBoolToObject(root, "sta_ip", wifi.staConnected());
    cJSON_AddNumberToObject(root, "max", IP_NAPT_MAX);
#if LWIP_STATS
    struct stats_ip_napt st = {};
    ip_napt_get_stats(&st);
    cJSON_AddNumberToObject(root, "tcp", st.nr_active_tcp);
    cJSON_AddNumberToObject(root, "udp", st.nr_active_udp);
    cJSON_AddNumberToObject(root, "icmp", st.nr_active_icmp);
    cJSON_AddNumberToObject(root, "evictions", st.nr_forced_evictions);
#endif
    return root;
}

bool NatManager::setWanted(bool on)
{
    bool ok;
    {
        std::lock_guard<std::mutex> lock(mutex);
        wanted = on;
        ok = applyLocked(wifi.staConnected());
    }
    if (listener != nullptr) {
        listener(listenerCtx);
    }
    return ok;
}

void NatManager::reapply()
{
    std::lock_guard<std::mutex> lock(mutex);
    natEnabled = false;
    applyLocked(wifi.staConnected());
}

void NatManager::setChangeListener(void (*fn)(void*), void* ctx)
{
    listenerCtx = ctx;
    listener = fn;
}

void NatManager::onGotIp(void* arg, esp_event_base_t, int32_t, void*)
{
    auto* self = static_cast<NatManager*>(arg);
    std::lock_guard<std::mutex> lock(self->mutex);
    // The event itself means the STA has an IP (the WifiManager handler may not have set the flag yet).
    self->applyLocked(true);
}

// Brings the NAPT state to the wanted one. NAPT is enabled only when the STA has an IP.
bool NatManager::applyLocked(bool staHasIp)
{
    if (wanted && !natEnabled) {
        if (!staHasIp) {
            return true; // enabled after IP_EVENT_STA_GOT_IP
        }
        esp_err_t err = esp_netif_napt_enable(wifi.apNetif());
        if (err != ESP_OK) {
            LOG_E("Nie udalo sie wlaczyc NAPT: %s", esp_err_to_name(err));
            return false;
        }
        natEnabled = true;
        LOG_I("NAPT wlaczony na interfejsie AP");
    } else if (!wanted && natEnabled) {
        esp_err_t err = esp_netif_napt_disable(wifi.apNetif());
        if (err != ESP_OK) {
            LOG_E("Nie udalo sie wylaczyc NAPT: %s", esp_err_to_name(err));
            return false;
        }
        natEnabled = false;
        LOG_I("NAPT wylaczony");
    }
    return true;
}

CmdResult NatManager::cmdOn(void* ctx, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    auto* self = static_cast<NatManager*>(ctx);
    if (!self->setWanted(true)) {
        out.print(tr("Błąd włączania NAT\r\n", "Error enabling NAT\r\n"));
        return CmdResult::ERR_FAIL;
    }
    out.print(self->enabled() ? tr("NAT włączony\r\n", "NAT enabled\r\n") : tr("NAT włączy się po połączeniu STA\r\n", "NAT will start after the STA connects\r\n"));
    return CmdResult::OK;
}

CmdResult NatManager::cmdOff(void* ctx, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    auto* self = static_cast<NatManager*>(ctx);
    if (!self->setWanted(false)) {
        out.print(tr("Błąd wyłączania NAT\r\n", "Error disabling NAT\r\n"));
        return CmdResult::ERR_FAIL;
    }
    out.print(tr("NAT wyłączony\r\n", "NAT disabled\r\n"));
    return CmdResult::OK;
}
