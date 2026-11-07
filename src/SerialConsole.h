/**
 * @file SerialConsole.h
 * @brief Command console on the native USB of the ESP32-S3.
 */
#pragma once

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "CommandExecutor.h"
#include "IOutput.h"
#include "IService.h"
#include "LineEditor.h"

/**
 * @brief Command console on the native USB of the S3 (USB Serial/JTAG) in its own FreeRTOS task.
 *
 * Line editing in LineEditor (shared with Telnet). Service "serial": when stopped the console
 * accepts no commands, but ESP-IDF logs still go to USB. Output is converted to ASCII unless
 * global.console_utf8 is set.
 */
class SerialConsole : public IOutput, public IService {
public:
    explicit SerialConsole(const CommandExecutor& executor);

    const char* name() const override { return "serial"; }
    /// Installs the USB driver (once) and starts the console task.
    esp_err_t start() override;
    /// Asks the task to finish and waits for it.
    esp_err_t stop() override;
    bool isRunning() const override { return running; }
    bool isConsole() const override { return true; }
    bool owns(const IOutput& out) const override { return &out == this; }

    /// Writes to USB (stdout), converting Polish characters to ASCII unless console_utf8.
    void write(const char* data, size_t len) override;

private:
    /// Task entry.
    static void taskEntry(void* arg);
    /// Task loop: reads characters and passes them to the editor.
    void run();

    LineEditor editor;                         ///< Line editing.
    bool driverInstalled = false;              ///< USB Serial/JTAG driver installed.
    std::atomic<bool> running { false };       ///< Service running.
    std::atomic<bool> stopRequested { false }; ///< stop() asked the task to finish.
    std::atomic<bool> taskAlive { false };     ///< Task still running.
};
