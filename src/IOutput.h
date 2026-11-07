/**
 * @file IOutput.h
 * @brief Destination of command output (COM console, Telnet session, text for the web panel).
 */
#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

/**
 * @brief Where the response of a command goes: COM console, Telnet session, StringOutput etc.
 *
 * Command handlers write only through this interface, so the same handler works for every
 * console. Text is UTF-8; console outputs convert it to ASCII when needed (i18n::consoleWrite).
 */
class IOutput {
public:
    virtual ~IOutput() = default;

    /// @brief Writes raw bytes (implemented by each output).
    virtual void write(const char* data, size_t len) = 0;

    /// @brief Writes a zero-terminated string.
    void print(const char* text) { write(text, strlen(text)); }

    /// @brief printf-style output; lines up to 159 bytes use a stack buffer, longer ones a temporary heap buffer.
    void printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char buf[160];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        if (n < 0) {
            return;
        }
        if (static_cast<size_t>(n) < sizeof(buf)) {
            write(buf, n);
            return;
        }
        // Rare case of a long line.

        char* big = static_cast<char*>(malloc(n + 1));
        if (big == nullptr) {
            write(buf, sizeof(buf) - 1);
            return;
        }
        va_start(ap, fmt);
        vsnprintf(big, n + 1, fmt, ap);
        va_end(ap);
        write(big, n);
        free(big);
    }
};
