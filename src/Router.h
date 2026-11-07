/**
 * @file Router.h
 * @brief Application entry point: creates all modules, wires their dependencies and starts them.
 */
#pragma once

#include "CommandExecutor.h"
#include "ConfigStore.h"
#include "DdnsClient.h"
#include "DhcpServer.h"
#include "DnsServer.h"
#include "Firewall.h"
#include "GlobalSettings.h"
#include "NatManager.h"
#include "PortForwarder.h"
#include "SerialConsole.h"
#include "ServiceManager.h"
#include "SystemCommands.h"
#include "TelnetServer.h"
#include "TimeService.h"
#include "WebPanel.h"
#include "WifiManager.h"

/**
 * @brief The application: creates all modules, injects dependencies and starts them in a fixed order.
 *
 * The most important file of the project – start reading the code here (doc/source.md, ch. 2).
 * One static instance is created in app_main(). Modules are members, so they are created in
 * declaration order: a module may depend only on members declared above it.
 */
class Router {
public:
    /// @brief Creates all modules and passes their dependencies (references) through constructors.
    Router();
    /**
     * @brief Starts the router: logs, NVS, LittleFS and configuration, commands and services,
     * NAT/port forwarding/time, Wi-Fi, autostart services, then confirms the firmware for OTA rollback.
     */
    void start();

private:
    ConfigStore config;       ///< Configuration files on LittleFS.
    GlobalSettings global;    ///< global.json.
    WifiManager wifi;         ///< STA + AP.
    NatManager nat;           ///< NAPT on the AP interface.
    DhcpServer dhcp;          ///< DHCP server for AP clients.
    DnsServer dns;            ///< DNS server for AP clients.
    TimeService time;         ///< Time zone and SNTP.
    DdnsClient ddns;          ///< Dynamic DNS client.
    CommandExecutor executor; ///< Command interpreter shared by all consoles.
    SystemCommands system;    ///< status, clients, sys, reboot.
    TelnetServer telnet;      ///< Telnet console.
    PortForwarder portfwd;    ///< Port forwarding.
    Firewall firewall;        ///< Firewall for AP clients.
    WebPanel web;             ///< Web panel and its API.
    SerialConsole console;    ///< COM console (USB).
    ServiceManager services;  ///< Service registry and autostart.

};
