/**
 * @file main.cpp
 * @brief ESP-IDF entry point – everything else starts in Router (Router.h).
 */
#include "Router.h"

/// @brief ESP-IDF application entry: creates the single Router (static – lives for the whole run) and starts it.
extern "C" void app_main(void)
{
    static Router router;
    router.start();
}
