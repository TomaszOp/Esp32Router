/**
 * @file SystemCommands.h
 * @brief General commands: status, version, clients, sys ota, sys mem, reboot; status data for the web panel.
 */
#pragma once

#include "cJSON.h"

#include "CommandExecutor.h"
#include "DhcpServer.h"
#include "GlobalSettings.h"
#include "NatManager.h"
#include "TimeService.h"
#include "WifiManager.h"

/// @brief General commands (status, version, clients, sys ota, sys mem, reboot) and the matching JSON for the web panel.
class SystemCommands {
public:
    SystemCommands(const GlobalSettings& global, WifiManager& wifi, NatManager& nat, TimeService& time, DhcpServer& dhcp);

    /// @brief Registers status, version, clients, sys ota, sys mem and reboot.

    void registerCommands(CommandExecutor& exec);

    /// @name Web panel (JSON freed by the caller)
    /// @{
    /// Device, STA, AP, NAT and memory (/api/status base).
    cJSON* statusJson() const;
    /// OTA partitions: running and next, versions, states (codes), boot partition.
    cJSON* otaJson() const;
    /// AP clients with IP, name, RSSI and address type.
    cJSON* clientsJson() const;
    /// Memory, flash, lwIP limits and task stacks.
    cJSON* memJson() const;
    /// @}

private:
    /// @name Console command handlers (ctx = this)
    /// @{
    static CmdResult cmdStatus(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdClients(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdOta(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdMem(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdReboot(void* ctx, int argc, char** argv, IOutput& out);
    static CmdResult cmdVersion(void* ctx, int argc, char** argv, IOutput& out);
    /// @}

    const GlobalSettings& global; ///< Host name, NTP setting.
    WifiManager& wifi;            ///< STA/AP state.
    NatManager& nat;              ///< NAT state.
    TimeService& time;            ///< Clock.
    DhcpServer& dhcp;             ///< Client names and addresses.
};
