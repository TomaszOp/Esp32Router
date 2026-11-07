/**
 * @file LogBuffer.h
 * @brief Ring buffer of recent ESP-IDF logs ("sys log", live logs in the web panel).
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "CommandExecutor.h"

/**
 * @brief Recent ESP-IDF logs in a ring buffer (PSRAM), hooked into esp_log_set_vprintf.
 *
 * Logs still go to the console as well. Readers: the web panel (WebSocket /ws/log) and
 * "sys log". All members are static – there is one buffer per device.
 */
class LogBuffer {
public:
    /// @brief Allocates the buffer (PSRAM if available) and installs the log hook. Call once, first in Router::start().
    static void install(size_t size);
    /// @return Position of the end = total number of bytes written since start.
    static uint64_t head();
    /**
     * @brief Copies data from position *pos and advances it.
     *
     * When the data at *pos was already overwritten, copying starts from the oldest available byte.
     * @return Number of bytes copied (at most max).
     */
    static size_t read(uint64_t* pos, char* out, size_t max);

    /// @brief Registers "sys log".
    static void registerCommands(CommandExecutor& exec);

private:
    /// vprintf replacement: writes to the console and to the buffer.
    static int hook(const char* fmt, va_list args);
    /// "sys log [bytes]" handler.
    static CmdResult cmdLog(void* ctx, int argc, char** argv, IOutput& out);

};
