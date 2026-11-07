/**
 * @file SerialConsole.cpp
 * @brief SerialConsole: console task on USB Serial/JTAG.
 */
#include "SerialConsole.h"

#include "i18n/I18n.h"

#include <cstdio>

#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "driver/usb_serial_jtag_vfs.h"

#define LOG_TAG     "SerialConsole"
#define LOG_ENABLED 1
#include "Log.h"

static constexpr int READ_TIMEOUT_MS = 100; // how often the task checks for a stop request
static constexpr int STOP_TIMEOUT_MS = 1000;

SerialConsole::SerialConsole(const CommandExecutor& executor)
    : editor(executor, *this, false)
{
}

esp_err_t SerialConsole::start()
{
    if (running) {
        return ESP_OK;
    }
    if (!driverInstalled) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        esp_err_t err = usb_serial_jtag_driver_install(&cfg);
        if (err != ESP_OK) {
            LOG_E("Nie udalo sie zainstalowac sterownika USB Serial/JTAG: %s", esp_err_to_name(err));
            return err;
        }
        // stdout (printf, ESP-IDF logs) also goes through the driver, so that two paths are not mixed.
        usb_serial_jtag_vfs_use_driver();
        driverInstalled = true;
    }

    stopRequested = false;
    taskAlive = true;
    if (xTaskCreate(&taskEntry, "console", 4096, this, 5, nullptr) != pdPASS) {
        taskAlive = false;
        return ESP_ERR_NO_MEM;
    }
    running = true;
    return ESP_OK;
}

esp_err_t SerialConsole::stop()
{
    if (!running) {
        return ESP_OK;
    }
    stopRequested = true;
    for (int waited = 0; taskAlive && waited < STOP_TIMEOUT_MS; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (taskAlive) {
        return ESP_ERR_TIMEOUT;
    }
    running = false;
    return ESP_OK;
}

// COM console in ASCII by default: Polish characters of messages converted to letters without diacritics
// (global.console_utf8 = true – UTF-8 unchanged).
void SerialConsole::write(const char* data, size_t len)
{
    i18n::consoleWrite(data, len, [](const char* d, size_t n) { fwrite(d, 1, n, stdout); });
    fflush(stdout);
}

void SerialConsole::taskEntry(void* arg)
{
    auto* self = static_cast<SerialConsole*>(arg);
    self->run();
    self->taskAlive = false;
    vTaskDelete(nullptr);
}

void SerialConsole::run()
{
    printf(tr("\r\nEsp32Router %s – wpisz 'help'\r\n", "\r\nEsp32Router %s – type 'help'\r\n"), esp_app_get_description()->version);

    editor.prompt();

    uint8_t buf[32];
    while (!stopRequested) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(READ_TIMEOUT_MS));
        for (int i = 0; i < n; ++i) {
            editor.handleChar(static_cast<char>(buf[i]));
        }
    }
    print(tr("\r\nKonsola COM zatrzymana (logi nadal na USB). Włączenie: service start serial z Telnetu.\r\n", "\r\nCOM console stopped (logs still on USB). To enable: service start serial from Telnet.\r\n"));
}
