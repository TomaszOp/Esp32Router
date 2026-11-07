/**
 * @file SystemCommands.cpp
 * @brief SystemCommands: status, clients, sys ota/mem, reboot and their JSON.
 */
#include "SystemCommands.h"

#include "i18n/I18n.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/opt.h"
#include "lwip/lwip_napt.h"

// NAT table values from lwIP (for "sys mem").
#ifdef IP_NAPT_MAX
#define IP_NAPT_MAX_VALUE IP_NAPT_MAX
#else
#define IP_NAPT_MAX_VALUE 0
#endif
#ifdef IP_PORTMAP_MAX
#define IP_PORTMAP_MAX_VALUE IP_PORTMAP_MAX
#else
#define IP_PORTMAP_MAX_VALUE 0
#endif

#define LOG_TAG     "SystemCommands"
#define LOG_ENABLED 1
#include "Log.h"

SystemCommands::SystemCommands(const GlobalSettings& global, WifiManager& wifi, NatManager& nat, TimeService& time,
                               DhcpServer& dhcp)
    : global(global),
      wifi(wifi),
      nat(nat),
      time(time),
      dhcp(dhcp)
{
}

void SystemCommands::registerCommands(CommandExecutor& exec)
{
    exec.add(nullptr, { "status", "", { "stan STA/AP, NAT, klienci, uptime, pamięć", "STA/AP status, NAT, clients, uptime, memory" }, &cmdStatus, this });
    exec.add(nullptr, { "clients", "", { "lista klientów AP (MAC, IP, nazwa, RSSI)", "list of AP clients (MAC, IP, name, RSSI)" }, &cmdClients, this });
    exec.addGroup("sys", { "informacje systemowe", "system information" });
    exec.add("sys", { "ota", "", { "partycje aplikacji: uruchomiona, do aktualizacji, wersje", "application partitions: running, for update, versions" }, &cmdOta, this });
    exec.add("sys", { "mem", "", { "pamięć SRAM/PSRAM, flash, stosy zadań", "SRAM/PSRAM memory, flash, task stacks" }, &cmdMem, this });
    exec.add(nullptr, { "reboot", "", { "restart urządzenia", "restarts the device" }, &cmdReboot, this });
    exec.add(nullptr, { "version", "", { "wersja firmware, data kompilacji, wersja ESP-IDF", "firmware version, build date, ESP-IDF version" }, &cmdVersion, this });
}

// "version": firmware version, build date and time, ESP-IDF version.
CmdResult SystemCommands::cmdVersion(void*, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    const esp_app_desc_t* app = esp_app_get_description();
    out.printf(tr("Esp32Router %s\r\nSkompilowano: %s %s\r\nESP-IDF: %s\r\n",
                  "Esp32Router %s\r\nBuilt: %s %s\r\nESP-IDF: %s\r\n"),
               app->version, app->date, app->time, app->idf_ver);
    return CmdResult::OK;
}

CmdResult SystemCommands::cmdStatus(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<SystemCommands*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }

    esp_netif_ip_info_t ip = {};
    const WifiSettings s = self->wifi.settings();

    char now[40];
    self->time.formatNow(now, sizeof(now));
    const esp_app_desc_t* app = esp_app_get_description();
    out.printf(tr("Wersja:  %s (skompilowano %s %s)\r\n", "Version: %s (built %s %s)\r\n"), app->version, app->date,
               app->time);
    out.printf("Host:    %s\r\n", self->global.values().hostname.c_str());

    out.printf(tr("Czas:    %s (%s)\r\n", "Time:    %s (%s)\r\n"), now,
               self->time.synced() ? tr("zsynchronizowany NTP", "NTP synchronized")
                                   : (self->global.values().time.ntpEnabled ? tr("niezsynchronizowany", "not synchronized")
                                                                            : tr("NTP wyłączony", "NTP disabled")));

    esp_netif_get_ip_info(self->wifi.staNetif(), &ip);
    if (self->wifi.staConnected()) {
        out.printf(tr("STA:     %s, IP " IPSTR "/" IPSTR ", brama " IPSTR "\r\n",
                      "STA:     %s, IP " IPSTR "/" IPSTR ", gateway " IPSTR "\r\n"),
                   s.staSsid.c_str(), IP2STR(&ip.ip), IP2STR(&ip.netmask), IP2STR(&ip.gw));
    } else {
        int err = self->wifi.lastStaError();
        out.printf(tr("STA:     %s, brak połączenia", "STA:     %s, not connected"),
                   s.staSsid.empty() ? tr("(nie ustawiono)", "(not set)") : s.staSsid.c_str());
        if (err != 0) {
            out.printf(tr(" (ostatnio: %s, kod %d)", " (last: %s, code %d)"), WifiManager::reasonText(err), err);
        }
        out.print("\r\n");
    }

    esp_netif_get_ip_info(self->wifi.apNetif(), &ip);
    wifi_sta_list_t list = {};
    esp_wifi_ap_get_sta_list(&list);
    out.printf(tr("AP:      %s (%s), IP " IPSTR "/" IPSTR ", klienci %d/%u\r\n",
                  "AP:      %s (%s), IP " IPSTR "/" IPSTR ", clients %d/%u\r\n"),
               s.apSsid.c_str(), s.apPass.size() >= 8 ? "WPA2" : tr("OTWARTA", "OPEN"), IP2STR(&ip.ip), IP2STR(&ip.netmask), list.num,
               s.apMaxClients);

    out.printf("NAT:     %s\r\n", self->nat.enabled() ? tr("włączony", "enabled") : tr("wyłączony", "disabled"));

    uint32_t up = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    out.printf("Uptime:  %lud %02lu:%02lu:%02lu\r\n", (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
               (unsigned long)(up / 60 % 60), (unsigned long)(up % 60));
    out.printf(tr("Pamięć:  wolne %lu B, minimum %lu B\r\n", "Memory:  free %lu B, minimum %lu B\r\n"), (unsigned long)esp_get_free_heap_size(),
               (unsigned long)esp_get_minimum_free_heap_size());
    return CmdResult::OK;
}

static const char* otaStateName(const esp_partition_t* p);

// Adds an address (network byte order) as "a.b.c.d" to a JSON object.
static void addIp(cJSON* obj, const char* key, uint32_t addr)
{
    char buf[16];
    esp_ip4_addr_t a = { addr };
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a));
    cJSON_AddStringToObject(obj, key, buf);
}

// Partition state in the console in the current language (the API keeps the code from otaStateName()).
static const char* otaStateText(const char* code)
{
    if (strcmp(code, "nowy") == 0) {
        return tr("nowy", "new");
    }
    if (strcmp(code, "do potwierdzenia") == 0) {
        return tr("do potwierdzenia", "pending verification");
    }
    if (strcmp(code, "poprawny") == 0) {
        return tr("poprawny", "valid");
    }
    if (strcmp(code, "odrzucony") == 0) {
        return tr("odrzucony", "rejected");
    }
    if (strcmp(code, "przerwany") == 0) {
        return tr("przerwany", "aborted");
    }
    return code;
}

cJSON* SystemCommands::statusJson() const
{
    cJSON* root = cJSON_CreateObject();
    const WifiSettings s = wifi.settings();

    char now[40];
    time.formatNow(now, sizeof(now));
    cJSON* dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddStringToObject(dev, "hostname", global.values().hostname.c_str());
    cJSON_AddStringToObject(dev, "version", esp_app_get_description()->version);
    cJSON_AddNumberToObject(dev, "uptime", static_cast<double>(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(dev, "time", now);
    cJSON_AddBoolToObject(dev, "time_synced", time.synced());

    esp_netif_ip_info_t ip = {};
    cJSON* sta = cJSON_AddObjectToObject(root, "sta");
    cJSON_AddStringToObject(sta, "ssid", s.staSsid.c_str());
    cJSON_AddBoolToObject(sta, "connected", wifi.staConnected());
    int err = wifi.lastStaError();
    if (!wifi.staConnected() && err != 0) {
        cJSON_AddNumberToObject(sta, "last_error_code", err);
        cJSON_AddStringToObject(sta, "last_error", WifiManager::reasonText(err));
    }
    if (wifi.staConnected()) {
        esp_netif_get_ip_info(wifi.staNetif(), &ip);
        addIp(sta, "ip", ip.ip.addr);
        addIp(sta, "mask", ip.netmask.addr);
        addIp(sta, "gateway", ip.gw.addr);
        addIp(sta, "dns", wifi.upstreamDns());
        int8_t rssi = 0;
        uint8_t channel = 0;
        if (wifi.staLink(&rssi, &channel)) {
            cJSON_AddNumberToObject(sta, "rssi", rssi);
            cJSON_AddNumberToObject(sta, "channel", channel);
        }
    }

    wifi_sta_list_t list = {};
    esp_wifi_ap_get_sta_list(&list);
    esp_netif_get_ip_info(wifi.apNetif(), &ip);
    cJSON* ap = cJSON_AddObjectToObject(root, "ap");
    cJSON_AddStringToObject(ap, "ssid", s.apSsid.c_str());
    cJSON_AddBoolToObject(ap, "open", s.apPass.size() < 8);
    addIp(ap, "ip", ip.ip.addr);
    addIp(ap, "mask", ip.netmask.addr);
    cJSON_AddNumberToObject(ap, "clients", list.num);
    cJSON_AddNumberToObject(ap, "max_clients", s.apMaxClients);

    cJSON_AddBoolToObject(root, "nat", nat.enabled());

    cJSON* mem = cJSON_AddObjectToObject(root, "memory");
    cJSON_AddNumberToObject(mem, "sram_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(mem, "sram_total", heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(mem, "sram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(mem, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(mem, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    return root;
}

cJSON* SystemCommands::otaJson() const
{
    cJSON* root = cJSON_CreateObject();
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const char* keys[] = { "running", "next" };
    const esp_partition_t* parts[] = { running, next };
    for (int i = 0; i < 2; ++i) {
        if (parts[i] == nullptr) {
            continue;
        }
        cJSON* p = cJSON_AddObjectToObject(root, keys[i]);
        cJSON_AddStringToObject(p, "label", parts[i]->label);
        cJSON_AddNumberToObject(p, "address", parts[i]->address);
        cJSON_AddNumberToObject(p, "size", parts[i]->size);
        cJSON_AddStringToObject(p, "state", otaStateName(parts[i]));
        cJSON_AddBoolToObject(p, "boot", parts[i] == boot);
        esp_app_desc_t desc = {};
        if (esp_ota_get_partition_description(parts[i], &desc) == ESP_OK) {
            cJSON_AddStringToObject(p, "version", desc.version);
            cJSON_AddStringToObject(p, "date", desc.date);
            cJSON_AddStringToObject(p, "time", desc.time);
        }
    }
    return root;
}

cJSON* SystemCommands::clientsJson() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "clients");
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return root;
    }
    for (int i = 0; i < list.num; ++i) {
        const uint8_t* mac = list.sta[i].mac;
        cJSON* o = cJSON_CreateObject();
        char buf[18];
        snprintf(buf, sizeof(buf), MACSTR, MAC2STR(mac));
        cJSON_AddStringToObject(o, "mac", buf);
        uint32_t addr = 0;
        char host[33] = "";
        if (dhcp.lookup(mac, &addr, host, sizeof(host))) {
            addIp(o, "ip", addr);
        }
        cJSON_AddStringToObject(o, "name", host);
        cJSON_AddNumberToObject(o, "rssi", list.sta[i].rssi);
        cJSON_AddStringToObject(o, "type", dhcp.isReserved(mac) ? "rezerwacja" : (addr ? "dynamiczny" : "-"));
        cJSON_AddItemToArray(arr, o);
    }
    return root;
}

CmdResult SystemCommands::cmdClients(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<SystemCommands*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }

    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        out.print(tr("Nie udało się odczytać listy klientów\r\n", "Could not read the client list\r\n"));
        return CmdResult::ERR_FAIL;
    }
    if (list.num == 0) {
        out.print(tr("Brak klientów\r\n", "No clients\r\n"));
        return CmdResult::OK;
    }

    out.print(tr("  MAC                IP               nazwa                RSSI\r\n",
                 "  MAC                IP               name                 RSSI\r\n"));
    for (int i = 0; i < list.num; ++i) {
        uint32_t addr = 0;
        char host[33] = "";
        char ip[16] = "-";
        if (self->dhcp.lookup(list.sta[i].mac, &addr, host, sizeof(host))) {
            esp_ip4_addr_t a = { addr };
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a));
        }
        out.printf("  " MACSTR "  %-15s  %-20s %d\r\n", MAC2STR(list.sta[i].mac), ip, host, list.sta[i].rssi);
    }
    return CmdResult::OK;
}

// OTA state of a partition as an API code ("nowy", "poprawny"…; "-" for no image).
static const char* otaStateName(const esp_partition_t* p)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) {
        return "-";
    }
    switch (st) {
    case ESP_OTA_IMG_NEW:
        return "nowy";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "do potwierdzenia";
    case ESP_OTA_IMG_VALID:
        return "poprawny";
    case ESP_OTA_IMG_INVALID:
        return "odrzucony";
    case ESP_OTA_IMG_ABORTED:
        return "przerwany";
    default:
        return "?";
    }
}

CmdResult SystemCommands::cmdOta(void*, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* boot = esp_ota_get_boot_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    const esp_partition_t* parts[] = { running, next };
    const char* roles[] = { tr("uruchomiona", "running"), tr("do aktualizacji", "for update") };
    for (int i = 0; i < 2; ++i) {
        const esp_partition_t* p = parts[i];
        if (p == nullptr) {
            out.printf("%s %s\r\n", i18n::pad(roles[i], 16).c_str(), tr("brak", "none"));
            continue;
        }
        esp_app_desc_t desc = {};
        bool hasApp = esp_ota_get_partition_description(p, &desc) == ESP_OK;
        out.printf(tr("%s %-6s 0x%06lx %4lu KB  %s%s%s  stan: %s%s\r\n", "%s %-6s 0x%06lx %4lu KB  %s%s%s  state: %s%s\r\n"),
                   i18n::pad(roles[i], 16).c_str(), p->label, (unsigned long)p->address, (unsigned long)(p->size / 1024),
                   hasApp ? desc.version : tr("(pusta)", "(empty)"), hasApp ? " " : "", hasApp ? desc.date : "",
                   otaStateText(otaStateName(p)), p == boot ? tr(", startowa", ", boot") : "");
    }
    return CmdResult::OK;
}

// One "sys mem" line for a heap with the given capabilities.
static void printHeap(IOutput& out, const char* label, uint32_t caps)
{
    size_t total = heap_caps_get_total_size(caps);
    if (total == 0) {
        out.printf("%-8s %s\r\n", label, tr("brak", "none"));
        return;
    }
    out.printf(tr("%-8s wolne %7u / %7u B, minimum %7u B, największy blok %7u B\r\n",
                  "%-8s free  %7u / %7u B, minimum %7u B, largest block %7u B\r\n"),
               label,
               (unsigned)heap_caps_get_free_size(caps), (unsigned)total, (unsigned)heap_caps_get_minimum_free_size(caps),
               (unsigned)heap_caps_get_largest_free_block(caps));
}

// Heap statistics with the given capabilities as a JSON object (added to parent).
static cJSON* heapJson(cJSON* parent, const char* key, uint32_t caps)
{
    cJSON* o = cJSON_AddObjectToObject(parent, key);
    cJSON_AddNumberToObject(o, "total", heap_caps_get_total_size(caps));
    cJSON_AddNumberToObject(o, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(o, "min", heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(o, "largest", heap_caps_get_largest_free_block(caps));
    return o;
}

cJSON* SystemCommands::memJson() const
{
    cJSON* root = cJSON_CreateObject();
    heapJson(root, "sram", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heapJson(root, "dma", MALLOC_CAP_DMA);
    heapJson(root, "psram", MALLOC_CAP_SPIRAM);

    uint32_t chip = 0;
    esp_flash_get_size(nullptr, &chip);
    const esp_partition_t* app = esp_ota_get_running_partition();
    cJSON* flash = cJSON_AddObjectToObject(root, "flash");
    cJSON_AddNumberToObject(flash, "chip", chip);
    if (app != nullptr) {
        esp_partition_pos_t pos = { app->address, app->size };
        esp_image_metadata_t meta = {};
        cJSON_AddNumberToObject(flash, "app", esp_image_get_metadata(&pos, &meta) == ESP_OK ? meta.image_len : 0);
        cJSON_AddNumberToObject(flash, "app_partition", app->size);
    }
    size_t fsTotal = 0, fsUsed = 0;
    if (esp_littlefs_info("littlefs", &fsTotal, &fsUsed) == ESP_OK) {
        cJSON_AddNumberToObject(flash, "fs_used", fsUsed);
        cJSON_AddNumberToObject(flash, "fs_total", fsTotal);
    }
    cJSON* lw = cJSON_AddObjectToObject(root, "lwip");
    cJSON_AddNumberToObject(lw, "max_sockets", CONFIG_LWIP_MAX_SOCKETS);
    cJSON_AddNumberToObject(lw, "napt", IP_NAPT_MAX_VALUE);
    cJSON_AddNumberToObject(lw, "portmap", IP_PORTMAP_MAX_VALUE);

    cJSON* list = cJSON_AddArrayToObject(root, "tasks");
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t count = uxTaskGetNumberOfTasks() + 4;
    auto* tasks = static_cast<TaskStatus_t*>(malloc(count * sizeof(TaskStatus_t)));
    if (tasks != nullptr) {
        count = uxTaskGetSystemState(tasks, count, nullptr);
        std::sort(tasks, tasks + count,
                  [](const TaskStatus_t& a, const TaskStatus_t& b) { return strcmp(a.pcTaskName, b.pcTaskName) < 0; });
        for (UBaseType_t i = 0; i < count; ++i) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", tasks[i].pcTaskName);
            cJSON_AddNumberToObject(t, "prio", tasks[i].uxCurrentPriority);
            cJSON_AddNumberToObject(t, "stack_free_min", tasks[i].usStackHighWaterMark);
            cJSON_AddItemToArray(list, t);
        }
        free(tasks);
    }
#endif
    return root;
}

CmdResult SystemCommands::cmdMem(void*, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }

    printHeap(out, "SRAM:", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    printHeap(out, "DMA:", MALLOC_CAP_DMA);
    printHeap(out, "PSRAM:", MALLOC_CAP_SPIRAM);

    uint32_t chip = 0;
    esp_flash_get_size(nullptr, &chip);
    const esp_partition_t* app = esp_ota_get_running_partition();
    uint32_t image = 0;
    if (app != nullptr) {
        esp_partition_pos_t pos = { app->address, app->size };
        esp_image_metadata_t meta = {};
        if (esp_image_get_metadata(&pos, &meta) == ESP_OK) {
            image = meta.image_len;
        }
    }
    out.printf(tr("Flash:   układ %lu KB, aplikacja %lu / %lu KB", "Flash:   chip %lu KB, application %lu / %lu KB"), (unsigned long)(chip / 1024),
               (unsigned long)(image / 1024), app ? (unsigned long)(app->size / 1024) : 0UL);
    size_t fsTotal = 0, fsUsed = 0;
    if (esp_littlefs_info("littlefs", &fsTotal, &fsUsed) == ESP_OK) {
        out.printf(", LittleFS %u / %u KB", (unsigned)(fsUsed / 1024), (unsigned)(fsTotal / 1024));
    }
    out.print("\r\n");
    out.printf(tr("lwIP:    max gniazd %d, tablica NAT %d wpisów, przekierowań portów %d\r\n", "lwIP:    max sockets %d, NAT table %d entries, port forwards %d\r\n"), CONFIG_LWIP_MAX_SOCKETS,
               IP_NAPT_MAX_VALUE, IP_PORTMAP_MAX_VALUE);

#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    UBaseType_t count = uxTaskGetNumberOfTasks() + 4;
    auto* tasks = static_cast<TaskStatus_t*>(malloc(count * sizeof(TaskStatus_t)));
    if (tasks == nullptr) {
        return CmdResult::ERR_FAIL;
    }
    count = uxTaskGetSystemState(tasks, count, nullptr);
    std::sort(tasks, tasks + count,
              [](const TaskStatus_t& a, const TaskStatus_t& b) { return strcmp(a.pcTaskName, b.pcTaskName) < 0; });
    out.printf(tr("Zadania (%u), minimum wolnego stosu:\r\n", "Tasks (%u), minimum free stack:\r\n"), (unsigned)count);
    for (UBaseType_t i = 0; i < count; ++i) {
        // In ESP-IDF the stack is counted in bytes.
        unsigned freeMin = static_cast<unsigned>(tasks[i].usStackHighWaterMark);
        out.printf("  %-16s prio %2u  %6u B%s\r\n", tasks[i].pcTaskName, (unsigned)tasks[i].uxCurrentPriority, freeMin,
                   freeMin < 512 ? tr("  <- mało!", "  <- low!") : "");
    }
    free(tasks);
#endif
    return CmdResult::OK;
}

CmdResult SystemCommands::cmdReboot(void*, int argc, char**, IOutput& out)
{
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    out.print(tr("Restart...\r\n", "Restarting...\r\n"));
    LOG_I("Restart z konsoli");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return CmdResult::OK;
}
