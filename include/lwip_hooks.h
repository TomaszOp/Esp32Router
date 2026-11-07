/**
 * @file lwip_hooks.h
 * @brief lwIP hooks of the project.
 *
 * Included into lwipopts.h through ESP_IDF_LWIP_HOOK_FILENAME (set for the lwip component in
 * src/CMakeLists.txt). The file is also read by C code.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct pbuf;
struct netif;

/**
 * @brief Called for every received IPv4 packet, before NAPT and port forwarding (PacketFilter.cpp).
 * @return != 0 = packet dropped (buffer freed by the hook).
 */
int esp32router_ip4_input_hook(struct pbuf* p, struct netif* inp);

#ifdef __cplusplus
}
#endif

/// lwIP hook macro: firewall, ping from WAN and port-forward sources (PacketFilter).
#define LWIP_HOOK_IP4_INPUT(p, inp) esp32router_ip4_input_hook((p), (inp))
