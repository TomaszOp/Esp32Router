/**
 * @file LogBuffer.cpp
 * @brief LogBuffer: ring buffer of ESP-IDF logs.
 */
#include "LogBuffer.h"

#include "i18n/I18n.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static char* ring = nullptr;
static size_t ringSize = 0;
static uint64_t total = 0;          // total number of bytes written
static SemaphoreHandle_t lock = nullptr;
static vprintf_like_t previous = nullptr;
static char line[256];              // formatting one line (under the mutex – does not load the task stacks)

void LogBuffer::install(size_t size)
{
    if (ring != nullptr) {
        return;
    }
    bool psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
    ringSize = psram ? size : size / 4;
    ring = static_cast<char*>(heap_caps_malloc(ringSize, psram ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT));
    lock = xSemaphoreCreateMutex();
    if (ring == nullptr || lock == nullptr) {
        ring = nullptr;
        return;
    }
    previous = esp_log_set_vprintf(&hook);
}

int LogBuffer::hook(const char* fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    int r = previous ? previous(fmt, args) : vprintf(fmt, args);

    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0) {
            size_t len = n < static_cast<int>(sizeof(line)) ? n : sizeof(line) - 1;
            for (size_t i = 0; i < len; ++i) {
                ring[(total + i) % ringSize] = line[i];
            }
            total += len;
        }
        xSemaphoreGive(lock);
    }
    va_end(copy);
    return r;
}

uint64_t LogBuffer::head()
{
    return total;
}

size_t LogBuffer::read(uint64_t* pos, char* out, size_t max)
{
    if (ring == nullptr) {
        return 0;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    if (total - *pos > ringSize) {
        *pos = total - ringSize; // overwritten – from the oldest available
    }
    size_t n = 0;
    while (*pos < total && n < max) {
        out[n++] = ring[*pos % ringSize];
        ++*pos;
    }
    xSemaphoreGive(lock);
    return n;
}

void LogBuffer::registerCommands(CommandExecutor& exec)
{
    exec.add("sys", { "log", "[bajty]", { "ostatnie logi z bufora (domyślnie 2048 B)", "recent logs from the buffer (default 2048 B)" }, &cmdLog, nullptr });
}

CmdResult LogBuffer::cmdLog(void*, int argc, char** argv, IOutput& out)
{
    if (argc > 2) {
        return CmdResult::ERR_ARGS;
    }
    long want = argc == 2 ? strtol(argv[1], nullptr, 10) : 2048;
    if (want <= 0) {
        return CmdResult::ERR_ARGS;
    }
    if (ring == nullptr) {
        out.print(tr("Bufor logów niedostępny\r\n", "Log buffer unavailable\r\n"));
        return CmdResult::ERR_FAIL;
    }
    uint64_t pos = total > static_cast<uint64_t>(want) ? total - want : 0;
    char buf[256];
    size_t n;
    while ((n = read(&pos, buf, sizeof(buf))) > 0) {
        // LF -> CRLF for the terminal
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                out.write(buf + start, i - start);
                out.print("\r\n");
                start = i + 1;
            }
        }
        out.write(buf + start, n - start);
    }
    out.printf(tr("\r\n(bufor %u B, zapisano łącznie %lu B)\r\n", "\r\n(buffer %u B, %lu B written in total)\r\n"), (unsigned)ringSize, (unsigned long)total);
    return CmdResult::OK;
}
