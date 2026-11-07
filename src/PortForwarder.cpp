/**
 * @file PortForwarder.cpp
 * @brief PortForwarder: port forwarding rules in lwIP, "pf" commands.
 */
#include "PortForwarder.h"

#include "i18n/I18n.h"

#include <cstdlib>
#include <cstring>

#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/lwip_napt.h"
#include "lwip/prot/ip.h"

#include "PacketFilter.h"

#define LOG_TAG     "PortForwarder"
#define LOG_ENABLED 1
#include "Log.h"

// Rule name: 0-32 characters a-z A-Z 0-9 -.
static bool validName(const std::string& s)
{
    if (s.size() > 32) {
        return false;
    }
    for (char c : s) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// "tcp" / "udp" / "both" -> Proto bits.
static bool parseProto(const char* s, uint8_t* proto)
{
    if (strcmp(s, "tcp") == 0) {
        *proto = 1;
    } else if (strcmp(s, "udp") == 0) {
        *proto = 2;
    } else if (strcmp(s, "both") == 0) {
        *proto = 3;
    } else {
        return false;
    }
    return true;
}

// Proto bits -> "tcp" / "udp" / "both".
static const char* protoName(uint8_t proto)
{
    return proto == 1 ? "tcp" : proto == 2 ? "udp" : "both";
}

// Port 1-65535.
static bool parsePort(const char* s, uint16_t* port)
{
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < 1 || v > 65535) {
        return false;
    }
    *port = static_cast<uint16_t>(v);
    return true;
}

PortForwarder::PortForwarder(WifiManager& wifi, NatManager& nat, DhcpServer& dhcp, const GlobalSettings& global,
                             const TelnetServer& telnet)
    : wifi(wifi),
      nat(nat),
      dhcp(dhcp),
      global(global),
      telnet(telnet)
{
}

// ---- configuration ----

int PortForwarder::entriesUsed(const Rule* except) const
{
    int n = 0;
    for (const Rule& r : rules) {
        if (&r != except) {
            n += r.proto == BOTH ? 2 : 1;
        }
    }
    return n;
}

void PortForwarder::loadConfig(ConfigReader& r)
{
    WifiSettings w = wifi.configured();
    std::lock_guard<std::mutex> lock(mutex);
    rules.clear();

    const cJSON* list = r.list("rules");
    int index = 0;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, list)
    {
        Rule rule = {};
        rule.enabled = true;
        const char* bad = nullptr;
        const cJSON* name = cJSON_GetObjectItemCaseSensitive(e, "name");
        const cJSON* proto = cJSON_GetObjectItemCaseSensitive(e, "proto");
        const cJSON* ext = cJSON_GetObjectItemCaseSensitive(e, "ext_port");
        const cJSON* target = cJSON_GetObjectItemCaseSensitive(e, "target");
        const cJSON* in = cJSON_GetObjectItemCaseSensitive(e, "int_port");
        const cJSON* en = cJSON_GetObjectItemCaseSensitive(e, "enabled");
        const cJSON* src = cJSON_GetObjectItemCaseSensitive(e, "src");
        uint8_t srcCount = 0;

        if (!cJSON_IsObject(e)) {
            bad = tr("oczekiwano obiektu", "object expected");
        } else if (name != nullptr && (!cJSON_IsString(name) || !validName(name->valuestring))) {
            bad = tr("nieprawidłowa nazwa (0-32 znaki a-z A-Z 0-9 -)", "invalid name (0-32 chars a-z A-Z 0-9 -)");
        } else if (!cJSON_IsString(proto) || !parseProto(proto->valuestring, &rule.proto)) {
            bad = tr("proto: tcp, udp albo both", "proto: tcp, udp or both");
        } else if (!cJSON_IsNumber(ext) || ext->valueint < 1 || ext->valueint > 65535) {
            bad = "ext_port: 1-65535";
        } else if (!cJSON_IsString(target) || target->valuestring[0] == '\0') {
            bad = tr("brak target (IPv4 albo nazwa rezerwacji)", "target missing (IPv4 or reservation name)");
        } else if (in != nullptr && (!cJSON_IsNumber(in) || in->valueint < 1 || in->valueint > 65535)) {
            bad = "int_port: 1-65535";
        } else if (en != nullptr && !cJSON_IsBool(en)) {
            bad = "enabled: true/false";
        } else if (src != nullptr &&
                   (!cJSON_IsString(src) || !PacketFilter::parseSources(src->valuestring, nullptr, nullptr, &srcCount))) {
            bad = tr("src: IP albo IP/maska, max 4 po przecinku", "src: IP or IP/mask, max 4 comma-separated");
        }

        if (bad == nullptr) {
            rule.name = name ? name->valuestring : "";
            rule.src = src ? src->valuestring : "";
            rule.extPort = static_cast<uint16_t>(ext->valueint);
            rule.target = target->valuestring;
            rule.intPort = in ? static_cast<uint16_t>(in->valueint) : rule.extPort;
            rule.enabled = en ? cJSON_IsTrue(en) : true;

            esp_ip4_addr_t ip = {};
            bool isIp = esp_netif_str_to_ip4(rule.target.c_str(), &ip) == ESP_OK;
            if (isIp && (((ip.addr ^ w.apIp) & w.apMask) != 0 || ip.addr == w.apIp)) {
                bad = tr("target poza podsiecią AP albo równy adresowi AP", "target outside the AP subnet or equal to the AP address");
            } else if (!isIp && !validName(rule.target)) {
                bad = tr("target: IPv4 albo nazwa rezerwacji DHCP", "target: IPv4 or DHCP reservation name");
            }
        }
        if (bad == nullptr) {
            for (const Rule& o : rules) {
                if (o.extPort == rule.extPort && (o.proto & rule.proto) != 0) {
                    bad = tr("ext_port już użyty dla tego protokołu", "ext_port already used for this protocol");
                } else if (!rule.name.empty() && o.name == rule.name) {
                    bad = tr("powtórzona nazwa", "duplicate name");
                }
            }
        }
        if (bad == nullptr && entriesUsed(nullptr) + (rule.proto == BOTH ? 2 : 1) > MAX_ENTRIES) {
            bad = tr("przekroczony limit 32 wpisów (both liczy się podwójnie)", "limit of 32 entries exceeded (both counts twice)");
        }

        if (bad != nullptr) {
            r.rejectEntry("rules", index, bad);
        } else {
            rules.push_back(rule);
        }
        ++index;
    }
}

cJSON* PortForwarder::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    cJSON* list = cJSON_AddArrayToObject(root, "rules");
    std::lock_guard<std::mutex> lock(mutex);
    for (const Rule& r : rules) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", r.name.c_str());
        cJSON_AddStringToObject(e, "proto", protoName(r.proto));
        cJSON_AddNumberToObject(e, "ext_port", r.extPort);
        cJSON_AddStringToObject(e, "target", r.target.c_str());
        cJSON_AddNumberToObject(e, "int_port", r.intPort);
        cJSON_AddBoolToObject(e, "enabled", r.enabled);
        cJSON_AddStringToObject(e, "src", r.src.c_str());
        cJSON_AddItemToArray(list, e);
    }
    return root;
}

// Rules from the file (e.g. after config revert) are installed immediately.
bool PortForwarder::applyLive(const std::string& path)
{
    if (path != "rules") {
        return false;
    }
    apply(currentStaIp());
    return true;
}

// ---- installing rules ----

void PortForwarder::start()
{
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onEvent, this, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &onEvent, this, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &onEvent, this, nullptr));
    nat.setChangeListener(&onNatChange, this);
    apply(currentStaIp());
}

uint32_t PortForwarder::currentStaIp() const
{
    if (!wifi.staConnected()) {
        return 0;
    }
    esp_netif_ip_info_t info = {};
    esp_netif_get_ip_info(wifi.staNetif(), &info);
    return info.ip.addr;
}

void PortForwarder::onEvent(void* arg, esp_event_base_t base, int32_t id, void* data)
{
    auto* self = static_cast<PortForwarder*>(arg);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        // Address from the event itself – the WifiManager handler may not have set the state yet.
        self->apply(static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr);
    } else {
        self->apply(0);
    }
}

void PortForwarder::onNatChange(void* arg)
{
    auto* self = static_cast<PortForwarder*>(arg);
    self->apply(self->currentStaIp());
}

bool PortForwarder::resolveTarget(const std::string& target, uint32_t* ip) const
{
    esp_ip4_addr_t a = {};
    if (esp_netif_str_to_ip4(target.c_str(), &a) == ESP_OK) {
        *ip = a.addr;
        return true;
    }
    return dhcp.reservationIp(target.c_str(), ip);
}

void PortForwarder::warnTelnetCollision(const Rule& r, IOutput* out) const
{
    if (global.values().mgmtFromSta && (r.proto & TCP) && telnet.isRunning() && telnet.listenPort() == r.extPort) {
        if (out != nullptr) {
            out->printf(tr("Uwaga: port %u koliduje z Telnetem dostępnym od strony STA (mgmt_from_sta)\r\n", "Note: port %u collides with Telnet available from the STA side (mgmt_from_sta)\r\n"), r.extPort);
        }
        LOG_W("Regula %s: port %u koliduje z Telnetem (mgmt_from_sta)", r.name.c_str(), r.extPort);
    }
}

// Removes all entries and installs the current ones (takes the rules mutex; call without it).
void PortForwarder::apply(uint32_t staIp)
{
    bool natOn = nat.enabled();
    std::lock_guard<std::mutex> lock(mutex);
    for (Rule& r : rules) {
        r.error = nullptr;
        r.resolvedIp = 0;
        if (!r.enabled) {
            r.state = State::DISABLED;
        } else if (!resolveTarget(r.target, &r.resolvedIp)) {
            r.state = State::ERROR;
            r.error = tr("brak rezerwacji DHCP o tej nazwie", "no DHCP reservation with this name");
        } else {
            r.state = State::PENDING;
        }
    }
    applyStaIp = staIp;
    applyNatOn = natOn;
    esp_netif_tcpip_exec(&applyInTcpip, this);

    int active = 0;
    for (const Rule& r : rules) {
        active += r.state == State::ACTIVE;
        if (r.state == State::ACTIVE) {
            warnTelnetCollision(r, nullptr);
        }
    }
    if (!rules.empty()) {
        LOG_I("Przekierowania portow: aktywne %d z %u%s", active, (unsigned)rules.size(),
              staIp == 0 ? " (STA bez IP)" : (natOn ? "" : " (NAT wylaczony)"));
    }
}

// Sources of active rules with a non-empty src, to the hook (in the tcpip task).
void PortForwarder::installSources(const std::vector<Rule>& rules)
{
    PacketFilter::PfSource table[PacketFilter::MAX_PF];
    int n = 0;
    for (const Rule& r : rules) {
        if (r.state != State::ACTIVE || r.src.empty() || n == PacketFilter::MAX_PF) {
            continue;
        }
        PacketFilter::PfSource& s = table[n];
        s.proto = r.proto; // TCP=1, UDP=2 like PacketFilter::P_TCP/P_UDP
        s.port = r.extPort;
        if (PacketFilter::parseSources(r.src.c_str(), s.ip, s.mask, &s.count) && s.count > 0) {
            ++n;
        }
    }
    PacketFilter::installPfSources(table, n);
}

// Executed in the tcpip task (lwIP has no core lock in this configuration).
esp_err_t PortForwarder::applyInTcpip(void* ctx)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    for (const Applied& a : self->applied) {
        ip_portmap_remove(a.ipProto, a.extPort);
    }
    self->applied.clear();
    if (self->applyStaIp == 0 || !self->applyNatOn) {
        installSources(self->rules);
        return ESP_OK;
    }

    for (Rule& r : self->rules) {
        if (r.state != State::PENDING) {
            continue;
        }
        bool ok = true;
        const uint8_t protos[] = { IP_PROTO_TCP, IP_PROTO_UDP };
        for (int i = 0; i < 2; ++i) {
            if (!(r.proto & (1 << i))) {
                continue;
            }
            if (ip_portmap_add(protos[i], self->applyStaIp, r.extPort, r.resolvedIp, r.intPort)) {
                self->applied.push_back({ protos[i], r.extPort });
            } else {
                ok = false;
            }
        }
        r.state = ok ? State::ACTIVE : State::ERROR;
        r.error = ok ? nullptr : tr("tablica przekierowań lwIP pełna", "lwIP forwarding table full");
    }
    installSources(self->rules);
    return ESP_OK;
}

// ---- commands ----

void PortForwarder::registerCommands(CommandExecutor& exec)
{
    exec.addGroup("pf", { "przekierowanie portów (z adresu STA do klientów AP)", "port forwarding (from the STA address to AP clients)" });
    exec.add("pf", { "list", "", { "reguły i ich stan", "rules and their state" }, &cmdList, this });
    exec.add("pf", { "add", "<tcp|udp|both> <ext_port> <ip|nazwa_rez> [int_port] [nazwa] [src=IP[/maska],...]",
                     { "dodaje regułę (trwałe po save); src = kto z zewnątrz może korzystać (max 4)", "adds a rule (permanent after save); src = who from outside may use it (max 4)" }, &cmdAdd, this });
    exec.add("pf", { "del", "<nr|nazwa>", { "usuwa regułę (trwałe po save)", "deletes a rule (permanent after save)" }, &cmdDel, this });
    exec.add("pf", { "enable", "<nr|nazwa>", { "włącza regułę", "enables a rule" }, &cmdEnable, this });
    exec.add("pf", { "disable", "<nr|nazwa>", { "wyłącza regułę bez usuwania", "disables a rule without deleting it" }, &cmdDisable, this });
    exec.add("pf", { "apply", "", { "zakłada reguły od nowa (np. po zmianie rezerwacji DHCP)", "re-creates the rules (e.g. after changing DHCP reservations)" }, &cmdApply, this });
    exec.add("pf", { "src", "<nr|nazwa> <IP[/maska],...|->", { "dozwolone źródła reguły (max 4; - = wszyscy)", "allowed sources of a rule (max 4; - = everyone)" }, &cmdSrc,
                     this });
}

PortForwarder::Rule* PortForwarder::findRule(const char* token)
{
    char* end = nullptr;
    long nr = strtol(token, &end, 10);
    if (end != token && *end == '\0') {
        return (nr >= 0 && nr < (long)rules.size()) ? &rules[nr] : nullptr;
    }
    for (Rule& r : rules) {
        if (!r.name.empty() && r.name == token) {
            return &r;
        }
    }
    return nullptr;
}

CmdResult PortForwarder::cmdList(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    std::lock_guard<std::mutex> lock(self->mutex);
    if (self->rules.empty()) {
        out.print(tr("Brak reguł\r\n", "No rules\r\n"));
        return CmdResult::OK;
    }
    out.printf(tr("Wpisy lwIP: %d / %d\r\n", "lwIP entries: %d / %d\r\n"), self->entriesUsed(nullptr), MAX_ENTRIES);
    out.print(tr("  nr  nazwa            proto ext    -> cel                  int    stan\r\n",
                 "  no  name             proto ext    -> target               int    state\r\n"));
    for (size_t i = 0; i < self->rules.size(); ++i) {
        const Rule& r = self->rules[i];
        char target[40];
        esp_ip4_addr_t literal = {};
        if (r.resolvedIp != 0 && esp_netif_str_to_ip4(r.target.c_str(), &literal) != ESP_OK) {
            esp_ip4_addr_t a = { r.resolvedIp };
            snprintf(target, sizeof(target), "%s(" IPSTR ")", r.target.c_str(), IP2STR(&a));
        } else {
            strlcpy(target, r.target.c_str(), sizeof(target));
        }
        const char* state = tr("wyłączona", "disabled");
        switch (r.state) {
        case State::ACTIVE:
            state = tr("aktywna", "active");
            break;
        case State::PENDING:
            state = self->applyStaIp == 0 ? tr("czeka na IP STA", "waiting for STA IP") : tr("NAT wyłączony", "NAT disabled");
            break;
        case State::ERROR:
            state = r.error ? r.error : tr("błąd", "error");
            break;
        case State::DISABLED:
            break;
        }
        out.printf("  %-3u %-16s %-5s %-6u -> %-20s %-6u %s%s%s\r\n", (unsigned)i, r.name.c_str(), protoName(r.proto),
                   r.extPort, target, r.intPort, state, r.src.empty() ? "" : tr(", źródło: ", ", source: "), r.src.c_str());
    }
    return CmdResult::OK;
}

CmdResult PortForwarder::cmdAdd(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    Rule rule = {};
    rule.enabled = true;
    // src=... anywhere after the target
    for (int i = 4; i < argc; ++i) {
        if (strncmp(argv[i], "src=", 4) == 0) {
            rule.src = argv[i] + 4;
            for (int k = i; k + 1 < argc; ++k) {
                argv[k] = argv[k + 1];
            }
            --argc;
            break;
        }
    }
    if (argc < 4 || argc > 6) {
        return CmdResult::ERR_ARGS;
    }
    if (!parseProto(argv[1], &rule.proto) || !parsePort(argv[2], &rule.extPort)) {
        return CmdResult::ERR_ARGS;
    }
    rule.target = argv[3];
    rule.intPort = rule.extPort;
    int next = 4;
    if (argc > next && parsePort(argv[next], &rule.intPort)) {
        ++next;
    }
    if (argc > next) {
        rule.name = argv[next++];
    }
    if (argc > next) {
        return CmdResult::ERR_ARGS;
    }
    return self->storeRule(-1, rule, out);
}

CmdResult PortForwarder::storeRule(int index, Rule rule, IOutput& out)
{
    if (!validName(rule.name)) {
        out.print(tr("Nieprawidłowa nazwa (0-32 znaki a-z A-Z 0-9 -)\r\n", "Invalid name (0-32 chars a-z A-Z 0-9 -)\r\n"));
        return CmdResult::ERR_FAIL;
    }

    WifiSettings w = wifi.settings();
    esp_ip4_addr_t ip = {};
    bool isIp = esp_netif_str_to_ip4(rule.target.c_str(), &ip) == ESP_OK;
    if (isIp && (((ip.addr ^ w.apIp) & w.apMask) != 0 || ip.addr == w.apIp)) {
        out.print(tr("Cel poza podsiecią AP albo równy adresowi AP\r\n", "Target outside the AP subnet or equal to the AP address\r\n"));
        return CmdResult::ERR_FAIL;
    }
    if (!isIp && !validName(rule.target)) {
        out.print(tr("Cel: adres IPv4 albo nazwa rezerwacji DHCP\r\n", "Target: IPv4 address or DHCP reservation name\r\n"));
        return CmdResult::ERR_FAIL;
    }
    uint8_t srcCount = 0;
    if (!PacketFilter::parseSources(rule.src.c_str(), nullptr, nullptr, &srcCount)) {
        out.print(tr("Źródło: IP albo IP/maska, max 4 po przecinku\r\n", "Source: IP or IP/mask, max 4 comma-separated\r\n"));
        return CmdResult::ERR_FAIL;
    }
    uint32_t resolved = 0;
    if (!isIp && !dhcp.reservationIp(rule.target.c_str(), &resolved)) {
        out.printf(tr("Uwaga: brak rezerwacji DHCP '%s' – reguła czeka, aż powstanie\r\n", "Note: no DHCP reservation '%s' – the rule waits until it exists\r\n"), rule.target.c_str());
    }

    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= static_cast<int>(rules.size())) {
            out.print(tr("Nie ma takiej reguły\r\n", "No such rule\r\n"));
            return CmdResult::ERR_FAIL;
        }
        const Rule* self = index >= 0 ? &rules[index] : nullptr;
        for (const Rule& o : rules) {
            if (&o == self) {
                continue;
            }
            if (o.extPort == rule.extPort && (o.proto & rule.proto) != 0) {
                out.printf(tr("Port %u (%s) jest już przekierowany\r\n", "Port %u (%s) is already forwarded\r\n"), rule.extPort, protoName(rule.proto));
                return CmdResult::ERR_FAIL;
            }
            if (!rule.name.empty() && o.name == rule.name) {
                out.print(tr("Reguła o tej nazwie już istnieje\r\n", "A rule with this name already exists\r\n"));
                return CmdResult::ERR_FAIL;
            }
        }
        if (entriesUsed(self) + (rule.proto == BOTH ? 2 : 1) > MAX_ENTRIES) {
            out.print(tr("Limit 32 wpisów (both liczy się podwójnie)\r\n", "Limit of 32 entries (both counts twice)\r\n"));
            return CmdResult::ERR_FAIL;
        }
        if (index >= 0) {
            rules[index] = rule;
        } else {
            rules.push_back(rule);
        }
    }
    warnTelnetCollision(rule, &out);
    apply(currentStaIp());
    out.print(index >= 0 ? tr("Zmieniono regułę. Trwałe po 'save'; stan: pf list\r\n", "Rule changed. Permanent after 'save'; state: pf list\r\n")
                         : tr("Dodano regułę. Trwałe po 'save'; stan: pf list\r\n", "Rule added. Permanent after 'save'; state: pf list\r\n"));
    return CmdResult::OK;
}

CmdResult PortForwarder::setRuleJson(const cJSON* json, IOutput& out)
{
    const cJSON* index = cJSON_GetObjectItemCaseSensitive(json, "index");
    const cJSON* name = cJSON_GetObjectItemCaseSensitive(json, "name");
    const cJSON* proto = cJSON_GetObjectItemCaseSensitive(json, "proto");
    const cJSON* ext = cJSON_GetObjectItemCaseSensitive(json, "ext_port");
    const cJSON* target = cJSON_GetObjectItemCaseSensitive(json, "target");
    const cJSON* in = cJSON_GetObjectItemCaseSensitive(json, "int_port");
    const cJSON* en = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON* src = cJSON_GetObjectItemCaseSensitive(json, "src");

    Rule rule = {};
    if (!cJSON_IsString(proto) || !parseProto(proto->valuestring, &rule.proto)) {
        out.print(tr("proto: tcp, udp albo both\r\n", "proto: tcp, udp or both\r\n"));
        return CmdResult::ERR_ARGS;
    }
    if (!cJSON_IsNumber(ext) || ext->valueint < 1 || ext->valueint > 65535 ||
        (in != nullptr && !cJSON_IsNull(in) && (!cJSON_IsNumber(in) || in->valueint < 1 || in->valueint > 65535))) {
        out.print(tr("Porty: 1-65535\r\n", "Ports: 1-65535\r\n"));
        return CmdResult::ERR_ARGS;
    }
    if (!cJSON_IsString(target) || target->valuestring[0] == '\0') {
        out.print(tr("Brak celu (IPv4 albo nazwa rezerwacji)\r\n", "Target missing (IPv4 or reservation name)\r\n"));
        return CmdResult::ERR_ARGS;
    }
    rule.name = cJSON_IsString(name) ? name->valuestring : "";
    rule.extPort = static_cast<uint16_t>(ext->valueint);
    rule.target = target->valuestring;
    rule.intPort = cJSON_IsNumber(in) ? static_cast<uint16_t>(in->valueint) : rule.extPort;
    rule.enabled = en == nullptr || cJSON_IsTrue(en);
    rule.src = cJSON_IsString(src) ? src->valuestring : "";
    return storeRule(cJSON_IsNumber(index) ? index->valueint : -1, rule, out);
}

cJSON* PortForwarder::rulesJson() const
{
    static const char* const states[] = { tr("wyłączona", "disabled"), "czeka", "aktywna", "blad" };
    cJSON* root = cJSON_CreateObject();
    std::lock_guard<std::mutex> lock(mutex);
    cJSON_AddNumberToObject(root, "entries", entriesUsed(nullptr));
    cJSON_AddNumberToObject(root, "max_entries", MAX_ENTRIES);
    cJSON_AddBoolToObject(root, "sta_ip", applyStaIp != 0);
    cJSON_AddBoolToObject(root, "nat", applyNatOn);
    cJSON* list = cJSON_AddArrayToObject(root, "rules");
    for (const Rule& r : rules) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", r.name.c_str());
        cJSON_AddStringToObject(o, "proto", protoName(r.proto));
        cJSON_AddNumberToObject(o, "ext_port", r.extPort);
        cJSON_AddStringToObject(o, "target", r.target.c_str());
        if (r.resolvedIp != 0) {
            char buf[16];
            esp_ip4_addr_t a = { r.resolvedIp };
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&a));
            cJSON_AddStringToObject(o, "ip", buf);
        }
        cJSON_AddNumberToObject(o, "int_port", r.intPort);
        cJSON_AddBoolToObject(o, "enabled", r.enabled);
        cJSON_AddStringToObject(o, "src", r.src.c_str());
        cJSON_AddStringToObject(o, "state", states[static_cast<int>(r.state)]);
        if (r.error != nullptr) {
            cJSON_AddStringToObject(o, "error", r.error);
        }
        cJSON_AddItemToArray(list, o);
    }
    return root;
}

CmdResult PortForwarder::cmdDel(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        Rule* r = self->findRule(argv[1]);
        if (r == nullptr) {
            out.print(tr("Nie znaleziono reguły\r\n", "Rule not found\r\n"));
            return CmdResult::ERR_FAIL;
        }
        self->rules.erase(self->rules.begin() + (r - self->rules.data()));
    }
    self->apply(self->currentStaIp());
    out.print(tr("Usunięto regułę. Trwałe po 'save'\r\n", "Rule deleted. Permanent after 'save'\r\n"));
    return CmdResult::OK;
}

CmdResult PortForwarder::setEnabled(int argc, char** argv, bool enabled, IOutput& out)
{
    if (argc != 2) {
        return CmdResult::ERR_ARGS;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        Rule* r = findRule(argv[1]);
        if (r == nullptr) {
            out.print(tr("Nie znaleziono reguły\r\n", "Rule not found\r\n"));
            return CmdResult::ERR_FAIL;
        }
        r->enabled = enabled;
    }
    apply(currentStaIp());
    out.printf(tr("Reguła %s. Trwałe po 'save'\r\n", "Rule %s. Permanent after 'save'\r\n"), enabled ? tr("włączona", "enabled") : tr("wyłączona", "disabled"));
    return CmdResult::OK;
}

CmdResult PortForwarder::cmdEnable(void* ctx, int argc, char** argv, IOutput& out)
{
    return static_cast<PortForwarder*>(ctx)->setEnabled(argc, argv, true, out);
}

CmdResult PortForwarder::cmdDisable(void* ctx, int argc, char** argv, IOutput& out)
{
    return static_cast<PortForwarder*>(ctx)->setEnabled(argc, argv, false, out);
}

CmdResult PortForwarder::cmdApply(void* ctx, int argc, char**, IOutput& out)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    if (argc != 1) {
        return CmdResult::ERR_ARGS;
    }
    self->apply(self->currentStaIp());
    return cmdList(ctx, 1, nullptr, out);
}

CmdResult PortForwarder::cmdSrc(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<PortForwarder*>(ctx);
    if (argc != 3) {
        return CmdResult::ERR_ARGS;
    }
    const char* list = strcmp(argv[2], "-") == 0 ? "" : argv[2];
    uint8_t count = 0;
    if (!PacketFilter::parseSources(list, nullptr, nullptr, &count)) {
        out.print(tr("Źródło: IP albo IP/maska, max 4 po przecinku; - = wszyscy\r\n", "Source: IP or IP/mask, max 4 comma-separated; - = everyone\r\n"));
        return CmdResult::ERR_FAIL;
    }
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        Rule* r = self->findRule(argv[1]);
        if (r == nullptr) {
            out.print(tr("Nie znaleziono reguły\r\n", "Rule not found\r\n"));
            return CmdResult::ERR_FAIL;
        }
        r->src = list;
    }
    self->apply(self->currentStaIp());
    out.printf(tr("Źródło: %s. Trwałe po 'save'\r\n", "Source: %s. Permanent after 'save'\r\n"), count == 0 ? tr("wszyscy", "everyone") : list);
    return CmdResult::OK;
}
