/**
 * @file Log.h
 * @brief Logging enabled per source file (LOG_E / LOG_W / LOG_I / LOG_D).
 *
 * In the .cpp file, before including this header:
 *   #define LOG_TAG     "WifiManager"
 *   #define LOG_ENABLED 1            // 0 = logs of this file disabled
 *   #include "Log.h"
 *
 * When LOG_ENABLED == 0 the macros generate no code (the arguments are only checked by the
 * compiler, so there are no unused-variable warnings). Logs go through ESP_LOGx to the console
 * (and to LogBuffer); log texts are Polish without diacritics and are not translated.
 */


#ifndef LOG_TAG
#error "Zdefiniuj LOG_TAG przed #include \"Log.h\""
#endif

#ifndef LOG_ENABLED
#define LOG_ENABLED 0
#endif

#include "esp_log.h"

#undef LOG_D
#undef LOG_I
#undef LOG_W
#undef LOG_E

#if LOG_ENABLED
#define LOG_D(fmt, ...) ESP_LOGD(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) ESP_LOGI(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) ESP_LOGW(LOG_TAG, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) ESP_LOGE(LOG_TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_NOP_(fmt, ...) do { if (0) { esp_log_write(ESP_LOG_NONE, LOG_TAG, fmt, ##__VA_ARGS__); } } while (0)
#define LOG_D(fmt, ...) LOG_NOP_(fmt, ##__VA_ARGS__)
#define LOG_I(fmt, ...) LOG_NOP_(fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) LOG_NOP_(fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) LOG_NOP_(fmt, ##__VA_ARGS__)
#endif
