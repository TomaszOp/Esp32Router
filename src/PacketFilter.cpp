/**
 * @file PacketFilter.cpp
 * @brief PacketFilter: lwIP IP4_INPUT hook – firewall rules, ping from WAN, forward sources.
 */
#include "PacketFilter.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip.h"
#include "lwip/prot/ip4.h"

#include "lwip_hooks.h"

namespace PacketFilter {

namespace {

constexpr int64_t LOG_INTERVAL_US = 1000000; // max 1 entry per second per rule

esp_netif_t* apEsp = nullptr;          // AP interface (identifies the packet side in the hook)
esp_netif_t* staEsp = nullptr;         // STA interface
std::atomic<bool> wanPing { false };   // answer ping from WAN
std::atomic<bool> fwEnabled { false }; // firewall service running
std::atomic<bool> logBlocked { true }; // record blocked packets in the ring

// Tables changed only in the tcpip thread.
Rule rules[MAX_RULES];
int ruleCount = 0;
uint32_t ruleHits[MAX_RULES];
int64_t ruleLastLogUs[MAX_RULES];
PfSource pf[MAX_PF];
int pfCount = 0;
uint32_t pingDrops = 0;
uint32_t pfDrops = 0;

Blocked ring[LOG_RING];
std::atomic<uint32_t> ringHead { 0 }; // written by the hook
std::atomic<uint32_t> ringTail { 0 }; // read by Firewall

// Records a blocked packet in the ring (at most once per second per rule; dropped when the ring is full).
void pushBlocked(const Rule& r, int slot, uint32_t src, uint32_t dst, uint8_t proto, uint16_t port)
{
    int64_t now = esp_timer_get_time();
    if (ruleLastLogUs[slot] != 0 && now - ruleLastLogUs[slot] < LOG_INTERVAL_US) {
        return;
    }
    ruleLastLogUs[slot] = now;
    uint32_t head = ringHead.load(std::memory_order_relaxed);
    if (head - ringTail.load(std::memory_order_acquire) >= LOG_RING) {
        return; // ring full – entry skipped
    }
    ring[head % LOG_RING] = { r.id, src, dst, proto, port };
    ringHead.store(head + 1, std::memory_order_release);
}

// Whether the destination port matches the rule (no ports in the rule = any port).
bool portMatch(const Rule& r, bool hasPort, uint16_t port)
{
    if (r.portCount == 0) {
        return true;
    }
    if (!hasPort) {
        return false;
    }
    for (int i = 0; i < r.portCount; ++i) {
        if (port >= r.ports[i].lo && port <= r.ports[i].hi) {
            return true;
        }
    }
    return false;
}

// IP protocol number -> Proto bit (0 for other protocols).
uint8_t protoBit(uint8_t ipProto)
{
    switch (ipProto) {
    case IP_PROTO_TCP:
        return P_TCP;
    case IP_PROTO_UDP:
        return P_UDP;
    case IP_PROTO_ICMP:
        return P_ICMP;
    default:
        return 0;
    }
}

// Packet from an AP client: firewall rules in order, the first match decides. true = drop.
bool filterFromAp(struct netif* inp, uint32_t src, uint32_t dst, uint8_t ipProto, bool hasPort, uint16_t port)
{
    if (ruleCount == 0) {
        return false;
    }
    uint32_t apIp = netif_ip4_addr(inp)->addr;
    uint32_t apMask = netif_ip4_netmask(inp)->addr;
    uint32_t staIp = 0;
    if (staEsp != nullptr) {
        auto* sta = static_cast<struct netif*>(esp_netif_get_netif_impl(staEsp));
        if (sta != nullptr) {
            staIp = netif_ip4_addr(sta)->addr;
        }
    }
    bool router = dst == apIp || (staIp != 0 && dst == staIp);
    // The AP network, broadcasts and multicast are neither "internet" nor "router".
    bool local = !router && (((dst ^ apIp) & apMask) == 0 || dst == IPADDR_BROADCAST || ip4_addr_ismulticast((const ip4_addr_t*)&dst));
    bool internet = !router && !local;
    uint8_t bit = protoBit(ipProto);

    for (int i = 0; i < ruleCount; ++i) {
        const Rule& r = rules[i];
        if (((src ^ r.clientIp) & r.clientMask) != 0 || ((dst ^ r.destIp) & r.destMask) != 0) {
            continue;
        }
        if ((r.dest == D_INTERNET && !internet) || (r.dest == D_ROUTER && !router)) {
            continue;
        }
        if (r.proto != P_ALL && (r.proto & bit) == 0) {
            continue;
        }
        if (!portMatch(r, hasPort, port)) {
            continue;
        }
        ++ruleHits[i];
        if (r.block && logBlocked.load(std::memory_order_relaxed)) {
            pushBlocked(r, i, src, dst, ipProto, hasPort ? port : 0);
        }
        return r.block;
    }
    return false;
}

// Packets from the STA side to the STA address: ping and port-forward sources. true = drop.
bool filterFromSta(struct netif* inp, uint32_t src, uint32_t dst, uint8_t ipProto, bool hasPort, uint16_t port,
                   int icmpType)
{
    if (dst != netif_ip4_addr(inp)->addr) {
        return false;
    }
    if (ipProto == IP_PROTO_ICMP && icmpType == 8 && !wanPing.load(std::memory_order_relaxed)) {
        ++pingDrops;
        return true;
    }
    if (!hasPort) {
        return false;
    }
    uint8_t bit = protoBit(ipProto);
    for (int i = 0; i < pfCount; ++i) {
        const PfSource& s = pf[i];
        if (s.port != port || (s.proto & bit) == 0) {
            continue;
        }
        for (int k = 0; k < s.count; ++k) {
            if (((src ^ s.ip[k]) & s.mask[k]) == 0) {
                return false;
            }
        }
        ++pfDrops;
        return true;
    }
    return false;
}

}

void setNetifs(esp_netif_t* ap, esp_netif_t* sta)
{
    apEsp = ap;
    staEsp = sta;
}

void setWanPing(bool allowed)
{
    wanPing.store(allowed);
}

void setFirewallEnabled(bool enabled)
{
    fwEnabled.store(enabled);
}

bool firewallEnabled()
{
    return fwEnabled.load();
}

void setLogBlocked(bool enabled)
{
    logBlocked.store(enabled);
}

void installRules(const Rule* list, int count)
{
    uint32_t newHits[MAX_RULES];
    int64_t newLog[MAX_RULES];
    count = count > MAX_RULES ? MAX_RULES : count;
    for (int i = 0; i < count; ++i) {
        newHits[i] = 0;
        newLog[i] = 0;
        for (int k = 0; k < ruleCount; ++k) {
            if (rules[k].id == list[i].id) {
                newHits[i] = ruleHits[k];
                newLog[i] = ruleLastLogUs[k];
                break;
            }
        }
    }
    memcpy(rules, list, sizeof(Rule) * count);
    memcpy(ruleHits, newHits, sizeof(uint32_t) * count);
    memcpy(ruleLastLogUs, newLog, sizeof(int64_t) * count);
    ruleCount = count;
}

void installPfSources(const PfSource* list, int count)
{
    count = count > MAX_PF ? MAX_PF : count;
    memcpy(pf, list, sizeof(PfSource) * count);
    pfCount = count;
}

void resetCounters()
{
    memset(ruleHits, 0, sizeof(ruleHits));
    pingDrops = 0;
    pfDrops = 0;
}

uint32_t hits(uint32_t ruleId)
{
    for (int i = 0; i < ruleCount; ++i) {
        if (rules[i].id == ruleId) {
            return ruleHits[i];
        }
    }
    return 0;
}

uint32_t droppedPing()
{
    return pingDrops;
}

uint32_t droppedPf()
{
    return pfDrops;
}

bool parseNet(const char* s, uint32_t* ip, uint32_t* mask)
{
    char buf[24];
    if (strlen(s) >= sizeof(buf)) {
        return false;
    }
    strlcpy(buf, s, sizeof(buf));
    int bits = 32;
    char* slash = strchr(buf, '/');
    if (slash != nullptr) {
        *slash = '\0';
        char* end = nullptr;
        long v = strtol(slash + 1, &end, 10);
        if (end == slash + 1 || *end != '\0' || v < 0 || v > 32) {
            return false;
        }
        bits = static_cast<int>(v);
    }
    esp_ip4_addr_t a = {};
    if (esp_netif_str_to_ip4(buf, &a) != ESP_OK) {
        return false;
    }
    *mask = bits == 0 ? 0 : lwip_htonl(0xFFFFFFFFu << (32 - bits));
    *ip = a.addr & *mask;
    return true;
}

bool parseSources(const char* s, uint32_t* ip, uint32_t* mask, uint8_t* count)
{
    *count = 0;
    while (*s != '\0') {
        const char* comma = strchr(s, ',');
        size_t len = comma ? static_cast<size_t>(comma - s) : strlen(s);
        char item[24];
        uint32_t i, m;
        if (*count == MAX_PF_SOURCES || len == 0 || len >= sizeof(item)) {
            return false;
        }
        memcpy(item, s, len);
        item[len] = '\0';
        if (!parseNet(item, &i, &m)) {
            return false;
        }
        if (ip != nullptr) {
            ip[*count] = i;
            mask[*count] = m;
        }
        ++*count;
        s += len;
        if (*s == ',') {
            ++s;
            if (*s == '\0') {
                return false;
            }
        }
    }
    return true;
}

bool popBlocked(Blocked* out)
{
    uint32_t tail = ringTail.load(std::memory_order_relaxed);
    if (tail == ringHead.load(std::memory_order_acquire)) {
        return false;
    }
    *out = ring[tail % LOG_RING];
    ringTail.store(tail + 1, std::memory_order_release);
    return true;
}

}

using namespace PacketFilter;

// lwIP IP4_INPUT hook (include/lwip_hooks.h), called in the tcpip thread for every received IPv4 packet,
// before NAPT and port forwarding. Returns 1 when the packet was dropped (pbuf freed), 0 to continue.
extern "C" int esp32router_ip4_input_hook(struct pbuf* p, struct netif* inp)

{
    if (p == nullptr || p->len < IP_HLEN) {
        return 0;
    }
    const auto* iph = static_cast<const struct ip_hdr*>(p->payload);
    uint16_t hlen = IPH_HL_BYTES(iph);
    if (hlen < IP_HLEN || p->len < hlen) {
        return 0; // lwIP itself drops a bad header
    }
    uint8_t ipProto = IPH_PROTO(iph);
    uint32_t src = iph->src.addr;
    uint32_t dst = iph->dest.addr;
    // Ports and ICMP type only in the first fragment.
    bool first = (lwip_ntohs(IPH_OFFSET(iph)) & IP_OFFMASK) == 0;
    const uint8_t* l4 = static_cast<const uint8_t*>(p->payload) + hlen;
    bool hasPort = first && (ipProto == IP_PROTO_TCP || ipProto == IP_PROTO_UDP) && p->len >= hlen + 4;
    uint16_t port = hasPort ? static_cast<uint16_t>((l4[2] << 8) | l4[3]) : 0;
    int icmpType = (first && ipProto == IP_PROTO_ICMP && p->len > hlen) ? l4[0] : -1;

    bool drop = false;
    if (apEsp != nullptr && inp == esp_netif_get_netif_impl(apEsp)) {
        drop = fwEnabled.load(std::memory_order_relaxed) && filterFromAp(inp, src, dst, ipProto, hasPort, port);
    } else if (staEsp != nullptr && inp == esp_netif_get_netif_impl(staEsp)) {
        drop = filterFromSta(inp, src, dst, ipProto, hasPort, port, icmpType);
    }
    if (drop) {
        pbuf_free(p);
        return 1;
    }
    return 0;
}
