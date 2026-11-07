/**
 * @file Router.cpp
 * @brief Router: creation of modules, dependencies and start-up order.
 */
#include "Router.h"

#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_ota_ops.h"

#include "LogBuffer.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#define LOG_TAG     "Router"
#define LOG_ENABLED 1
#include "Log.h"

Router::Router()
    : wifi(global),
      nat(wifi),
      dhcp(wifi),
      dns(wifi, dhcp, global, config),
      time(global),
      ddns(wifi, time),
      system(global, wifi, nat, time, dhcp),
      telnet(executor, global, wifi),
      portfwd(wifi, nat, dhcp, global, telnet),
      firewall(dhcp, global),
      web(global, wifi, config, executor, system, services, dhcp, portfwd, nat, dns, ddns, firewall),
      console(executor),
      services(global)
{
}

void Router::start()
{
    LogBuffer::install(32 * 1024); // logs for the web panel and "sys log"
    LOG_I("Esp32Router start");

    // NVS is needed by the Wi-Fi driver; our configuration is in files on LittleFS.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // No file system does not stop the router – modules will use default values.
    config.mount();
    config.add(global);
    config.add(wifi);
    config.add(nat);
    config.add(dhcp);
    config.add(portfwd);
    config.add(firewall);
    config.add(dns);
    config.add(web);
    config.add(ddns);
    config.add(telnet);
    config.loadAll();
    global.activate();
    dns.loadBlocklist();
    web.loadAuth();
    dhcp.setLocalDns(&dns);
    firewall.begin();

    // Commands are registered before the consoles start; afterwards the command tree is only read.
    config.registerCommands(executor);
    wifi.registerCommands(executor);
    nat.registerCommands(executor);
    dhcp.registerCommands(executor);
    portfwd.registerCommands(executor);
    firewall.registerCommands(executor);
    dns.registerCommands(executor);
    web.registerCommands(executor);
    ddns.registerCommands(executor);
    system.registerCommands(executor);
    LogBuffer::registerCommands(executor);
    telnet.registerCommands(executor);
    services.add(console);
    services.add(telnet);
    services.add(dhcp);
    services.add(dns);
    services.add(web);
    services.add(ddns);
    services.add(firewall);
    services.registerCommands(executor);

    // NAT and time register for events before Wi-Fi starts, so that IP_EVENT_STA_GOT_IP is not missed.
    nat.start();
    portfwd.start();
    time.start();
    wifi.start();
    services.startAutostart();

    // AP restart: NAPT again, DHCP and DNS bind their sockets to the new AP interface.
    wifi.addApRestartListener(
        [](void* ctx) {
            auto* self = static_cast<Router*>(ctx);
            self->nat.reapply();
            self->services.restartDeferred("dhcp");
            self->services.restartDeferred("dns");
        },
        this);

    // Firmware started (Wi-Fi and consoles work) – confirmation for OTA rollback.
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        LOG_I("Firmware %s potwierdzony", esp_app_get_description()->version);
    }
}
