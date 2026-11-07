/**
 * @file PacketFilter.h
 * @brief Packet filter in the lwIP IP4_INPUT hook (firewall rules, ping from WAN, port-forward sources).
 */
#pragma once

#include <cstdint>

#include "esp_netif.h"

/**
 * @brief Packet filter in the lwIP IP4_INPUT hook (include/lwip_hooks.h).
 *
 * Firewall rules for traffic from AP clients, ping from WAN to the STA address, allowed sources
 * of port forwards. The hook runs in the tcpip thread – no allocation and no locks. Tables are
 * swapped only in the tcpip thread (install* called through esp_netif_tcpip_exec), so the hook
 * always sees consistent data.
 */
namespace PacketFilter {

/// Protocol bits of a rule.
enum Proto : uint8_t { P_TCP = 1, P_UDP = 2, P_ICMP = 4, P_ALL = 0xFF };
/// Destination class of a rule.
enum Dest : uint8_t { D_INTERNET, D_ROUTER, D_ANY };

constexpr int MAX_RULES = 32;      ///< Maximum firewall rules.
constexpr int MAX_PORT_RANGES = 8; ///< Port ranges per rule.
constexpr int MAX_PF = 32;         ///< Maximum port forwards with sources.
constexpr int MAX_PF_SOURCES = 4;  ///< Sources per port forward.
constexpr int LOG_RING = 16;       ///< Size of the blocked-packet ring.

/// Inclusive port range.
struct PortRange {
    uint16_t lo; ///< First port.
    uint16_t hi; ///< Last port.
};

/// Firewall rule after resolving names (addresses and masks in network byte order).
struct Rule {
    uint32_t id;         ///< Rule identifier in Firewall (counters survive table swaps).
    uint32_t clientIp;   ///< Client address.
    uint32_t clientMask; ///< Client mask (0 = all clients).
    uint32_t destIp;     ///< Destination address.
    uint32_t destMask;   ///< Destination mask (0 = any).
    uint8_t dest;        ///< Dest class.
    uint8_t proto;       ///< Proto bits.
    uint8_t portCount;   ///< 0 = any port.
    bool block;          ///< true = block, false = allow.
    PortRange ports[MAX_PORT_RANGES]; ///< Destination port ranges.
};

/// Port forward with a list of allowed sources (only rules with a non-empty list).
struct PfSource {
    uint8_t proto;                 ///< P_TCP / P_UDP / both.
    uint16_t port;                 ///< External port.
    uint8_t count;                 ///< Number of sources.
    uint32_t ip[MAX_PF_SOURCES];   ///< Source addresses.
    uint32_t mask[MAX_PF_SOURCES]; ///< Source masks.
};

/// Blocked packet for the log (ring filled by the hook, drained by Firewall).
struct Blocked {
    uint32_t ruleId; ///< Matching rule.
    uint32_t src;    ///< Source address.
    uint32_t dst;    ///< Destination address.
    uint8_t proto;   ///< IP protocol number.
    uint16_t port;   ///< Destination port, 0 = none.
};

/// @brief Sets the AP and STA interfaces. At start-up, before Wi-Fi is enabled.
void setNetifs(esp_netif_t* ap, esp_netif_t* sta);
/// @name Flags read by the hook (may be changed from any task)
/// @{
void setWanPing(bool allowed);
void setFirewallEnabled(bool enabled);
bool firewallEnabled();
void setLogBlocked(bool enabled);
/// @}

/// @name tcpip thread only. Counters of rules with the same id are kept.
/// @{
void installRules(const Rule* rules, int count);
void installPfSources(const PfSource* sources, int count);
void resetCounters();
/// @}

/// @name Hit counters (read from any task; single word)
/// @{
uint32_t hits(uint32_t ruleId);
uint32_t droppedPing();
uint32_t droppedPf();
/// @}

/// @brief "IP" or "IP/mask" (0-32) -> address and mask in network byte order (address cut to the mask).
bool parseNet(const char* s, uint32_t* ip, uint32_t* mask);
/// @brief Source list "IP[/mask],..." (max MAX_PF_SOURCES); empty = 0 items. ip/mask pointers may be nullptr (validation only).
bool parseSources(const char* s, uint32_t* ip, uint32_t* mask, uint8_t* count);

/// @brief Takes the next entry from the blocked ring (single consumer – Firewall timer task). @return false when empty.
bool popBlocked(Blocked* out);


}
