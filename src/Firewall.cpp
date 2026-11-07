/**
 * @file Firewall.cpp
 * @brief Firewall: rules, resolving clients, log of blocked packets, "fw" command.
 */
#include "Firewall.h"

#include "Defaults.h"
#include "i18n/I18n.h"

#include <cstdlib>
#include <cstring>

#include "esp_netif.h"

#define LOG_TAG     "Firewall"
#define LOG_ENABLED 1
#include "Log.h"

namespace dflt = defaults::firewall; // factory settings of firewall.json

using namespace PacketFilter;

static constexpr size_t MAX_RECENT = 20;

// ---- field parsing ----

// Rule name: 1-24 characters a-z A-Z 0-9 - _.
static bool validName(const char* s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 24) {
        return false;
    }
    for (; *s; ++s) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

// DHCP reservation name – as in port forwarding.
static bool validReservationName(const char* s)
{
    size_t n = strlen(s);
    if (n == 0 || n > 32) {
        return false;
    }
    for (; *s; ++s) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

// "IP" or "IP/mask"; "any" when allowAny.
static bool parseNet(const char* s, uint32_t* ip, uint32_t* mask, bool allowAny)
{
    if (allowAny && strcmp(s, "any") == 0) {
        *ip = 0;
        *mask = 0;
        return true;
    }
    return PacketFilter::parseNet(s, ip, mask);
}

// "aa:bb:cc:dd:ee:ff" (also with '-') -> 6 bytes.
static bool parseMac(const char* s, uint8_t mac[6])
{
    unsigned v[6];
    char tail;
    if (strlen(s) != 17 || sscanf(s, "%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x%*[:-]%2x%c", &v[0], &v[1], &v[2], &v[3],
                                  &v[4], &v[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = static_cast<uint8_t>(v[i]);
    }
    return true;
}

// Port list "25,6881-6889" (max 8 items); empty = all.
static bool parsePorts(const char* s, PortRange* out, uint8_t* count)
{
    *count = 0;
    if (*s == '\0') {
        return true;
    }
    const char* p = s;
    while (true) {
        if (*count == MAX_PORT_RANGES) {
            return false;
        }
        char* end = nullptr;
        long lo = strtol(p, &end, 10);
        if (end == p || lo < 1 || lo > 65535) {
            return false;
        }
        long hi = lo;
        if (*end == '-') {
            const char* q = end + 1;
            hi = strtol(q, &end, 10);
            if (end == q || hi < lo || hi > 65535) {
                return false;
            }
        }
        out[(*count)++] = { static_cast<uint16_t>(lo), static_cast<uint16_t>(hi) };
        if (*end == '\0') {
            return true;
        }
        if (*end != ',') {
            return false;
        }
        p = end + 1;
    }
}

// "tcp" / "udp" / "both" / "icmp" / "all" -> PacketFilter::Proto bits.
static bool parseProto(const char* s, uint8_t* proto)
{
    static const struct {
        const char* name;
        uint8_t value;
    } table[] = { { "tcp", P_TCP }, { "udp", P_UDP }, { "both", P_TCP | P_UDP }, { "icmp", P_ICMP }, { "all", P_ALL } };
    for (const auto& t : table) {
        if (strcmp(s, t.name) == 0) {
            *proto = t.value;
            return true;
        }
    }
    return false;
}

// PacketFilter::Proto bits -> name.
static const char* protoName(uint8_t proto)
{
    switch (proto) {
    case P_TCP:
        return "tcp";
    case P_UDP:
        return "udp";
    case P_TCP | P_UDP:
        return "both";
    case P_ICMP:
        return "icmp";
    default:
        return "all";
    }
}

// "internet" / "router" / "any" -> PacketFilter::Dest.
static bool parseDest(const char* s, uint8_t* dest)
{
    if (strcmp(s, "internet") == 0) {
        *dest = D_INTERNET;
    } else if (strcmp(s, "router") == 0) {
        *dest = D_ROUTER;
    } else if (strcmp(s, "any") == 0) {
        *dest = D_ANY;
    } else {
        return false;
    }
    return true;
}

// PacketFilter::Dest -> name.
static const char* destName(uint8_t dest)
{
    return dest == D_INTERNET ? "internet" : dest == D_ROUTER ? "router" : "any";
}

// "block" / "allow".
static bool parseAction(const char* s, bool* block)
{
    if (strcmp(s, "block") == 0) {
        *block = true;
    } else if (strcmp(s, "allow") == 0) {
        *block = false;
    } else {
        return false;
    }
    return true;
}

// IP protocol number -> name for the log.
static const char* ipProtoName(uint8_t p)
{
    return p == 6 ? "tcp" : p == 17 ? "udp" : p == 1 ? "icmp" : "ip";
}

// Address in network byte order -> "a.b.c.d".
static void ipStr(uint32_t ip, char* buf, size_t size)
{
    esp_ip4_addr_t a = { ip };
    snprintf(buf, size, IPSTR, IP2STR(&a));
}

Firewall::Firewall(DhcpServer& dhcp, const GlobalSettings& global)
    : dhcp(dhcp),
      global(global)
{
}

// ---- configuration ----

const char* Firewall::validate(const Rule& r) const
{
    uint32_t ip, mask;
    uint8_t mac[6];
    PortRange ports[MAX_PORT_RANGES];
    uint8_t n;
    if (!validName(r.name.c_str())) {
        return tr("nazwa: 1-24 znaki a-z A-Z 0-9 - _", "name: 1-24 chars a-z A-Z 0-9 - _");
    }
    if (r.client != "all" && !parseNet(r.client.c_str(), &ip, &mask, false) && !parseMac(r.client.c_str(), mac) &&
        !validReservationName(r.client.c_str())) {
        return tr("client: all, IP, IP/maska, MAC albo nazwa rezerwacji DHCP", "client: all, IP, IP/mask, MAC or DHCP reservation name");
    }
    if (!parsePorts(r.ports.c_str(), ports, &n)) {
        return tr("porty: 1-65535, zakres od-do, lista po przecinku (max 8)", "ports: 1-65535, range from-to, comma-separated list (max 8)");
    }
    if (n > 0 && (r.proto == P_ICMP || r.proto == P_ALL)) {
        return tr("porty tylko dla tcp, udp i both", "ports only for tcp, udp and both");
    }
    if (!parseNet(r.destIp.c_str(), &ip, &mask, true)) {
        return tr("dest_ip: any, IP albo IP/maska", "dest_ip: any, IP or IP/mask");
    }
    for (const Rule& o : rules) {
        if (o.name == r.name && o.id != r.id) {
            return tr("powtórzona nazwa", "duplicate name");
        }
    }
    return nullptr;
}

// Rule from a JSON object (file and panel); missing fields = default values.
static const char* ruleFromJson(const cJSON* e, std::string* name, bool* enabled, std::string* client, uint8_t* proto,
                                std::string* ports, uint8_t* dest, std::string* destIp, bool* block)
{
    const cJSON* f;
    if (!cJSON_IsObject(e)) {
        return tr("oczekiwano obiektu", "object expected");
    }
    f = cJSON_GetObjectItemCaseSensitive(e, "name");
    if (!cJSON_IsString(f)) {
        return tr("brak nazwy", "name missing");
    }
    *name = f->valuestring;
    f = cJSON_GetObjectItemCaseSensitive(e, "enabled");
    if (f != nullptr && !cJSON_IsBool(f)) {
        return tr("enabled: true/false", tr("enabled: true/false", "enabled: true/false"));
    }
    *enabled = f == nullptr || cJSON_IsTrue(f);
    f = cJSON_GetObjectItemCaseSensitive(e, "client");
    if (f != nullptr && !cJSON_IsString(f)) {
        return "client: tekst";
    }
    *client = (f && f->valuestring[0]) ? f->valuestring : "all";
    f = cJSON_GetObjectItemCaseSensitive(e, "proto");
    *proto = P_TCP | P_UDP;
    if (f != nullptr && (!cJSON_IsString(f) || !parseProto(f->valuestring, proto))) {
        return tr("proto: tcp, udp, both, icmp albo all", "proto: tcp, udp, both, icmp or all");
    }
    f = cJSON_GetObjectItemCaseSensitive(e, "ports");
    if (f != nullptr && !cJSON_IsString(f) && !cJSON_IsNumber(f)) {
        return "ports: tekst, np. \"25,6881-6889\"";
    }
    *ports = f == nullptr ? "" : cJSON_IsNumber(f) ? std::to_string(f->valueint) : f->valuestring;
    f = cJSON_GetObjectItemCaseSensitive(e, "dest");
    *dest = D_INTERNET;
    if (f != nullptr && (!cJSON_IsString(f) || !parseDest(f->valuestring, dest))) {
        return tr("dest: internet, router albo any", "dest: internet, router or any");
    }
    f = cJSON_GetObjectItemCaseSensitive(e, "dest_ip");
    if (f != nullptr && !cJSON_IsString(f)) {
        return "dest_ip: tekst";
    }
    *destIp = (f && f->valuestring[0]) ? f->valuestring : "any";
    f = cJSON_GetObjectItemCaseSensitive(e, "action");
    *block = true;
    if (f != nullptr && (!cJSON_IsString(f) || !parseAction(f->valuestring, block))) {
        return tr("action: block albo allow", "action: block or allow");
    }
    return nullptr;
}

void Firewall::loadConfig(ConfigReader& r)
{
    std::lock_guard<std::mutex> lock(mutex);
    logBlocked = r.boolean("log_blocked", dflt::LOG_BLOCKED);
    rules.clear();

    const cJSON* list = r.list("rules");
    int index = 0;
    const cJSON* e = nullptr;
    cJSON_ArrayForEach(e, list)
    {
        Rule rule = {};
        rule.id = nextId++;
        const char* bad = ruleFromJson(e, &rule.name, &rule.enabled, &rule.client, &rule.proto, &rule.ports, &rule.dest,
                                       &rule.destIp, &rule.block);
        if (bad == nullptr) {
            bad = validate(rule);
        }
        if (bad == nullptr && rules.size() >= MAX_RULES) {
            bad = tr("przekroczony limit 32 reguł", "limit of 32 rules exceeded");
        }
        if (bad != nullptr) {
            r.rejectEntry("rules", index, bad);
        } else {
            rules.push_back(rule);
        }
        ++index;
    }
}

cJSON* Firewall::saveConfig() const
{
    cJSON* root = cJSON_CreateObject();
    std::lock_guard<std::mutex> lock(mutex);
    cJSON_AddBoolToObject(root, "log_blocked", logBlocked);
    cJSON* list = cJSON_AddArrayToObject(root, "rules");
    for (const Rule& r : rules) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", r.name.c_str());
        cJSON_AddBoolToObject(e, "enabled", r.enabled);
        cJSON_AddStringToObject(e, "client", r.client.c_str());
        cJSON_AddStringToObject(e, "proto", protoName(r.proto));
        cJSON_AddStringToObject(e, "ports", r.ports.c_str());
        cJSON_AddStringToObject(e, "dest", destName(r.dest));
        cJSON_AddStringToObject(e, "dest_ip", r.destIp.c_str());
        cJSON_AddStringToObject(e, "action", r.block ? "block" : "allow");
        cJSON_AddItemToArray(list, e);
    }
    return root;
}

bool Firewall::applyLive(const std::string& path)
{
    if (path == "log_blocked") {
        std::lock_guard<std::mutex> lock(mutex);
        PacketFilter::setLogBlocked(logBlocked);
        return true;
    }
    if (path == "rules") {
        compile();
        return true;
    }
    return false;
}

// ---- table in the hook ----

void Firewall::begin()
{
    {
        std::lock_guard<std::mutex> lock(mutex);
        PacketFilter::setLogBlocked(logBlocked);
    }
    dhcp.setChangeListener(&onDhcpChange, this);
    esp_timer_create_args_t args = {};
    args.callback = &timerCb;
    args.arg = this;
    args.name = "fw_log";
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_periodic(timer, 1000000);
    }
    compile();
}

void Firewall::onDhcpChange(void* ctx)
{
    static_cast<Firewall*>(ctx)->compile();
}

void Firewall::compile()
{
    std::lock_guard<std::mutex> lock(mutex);
    compiledCount = 0;
    for (Rule& r : rules) {
        r.error = nullptr;
        r.resolvedIp = 0;
        if (!r.enabled) {
            continue;
        }
        PacketFilter::Rule c = {};
        c.id = r.id;
        uint8_t mac[6];
        if (r.client == "all") {
            c.clientIp = 0;
            c.clientMask = 0;
        } else if (parseNet(r.client.c_str(), &c.clientIp, &c.clientMask, false)) {
            // address or subnet
        } else {
            uint32_t ip = 0;
            char host[33];
            bool ok = parseMac(r.client.c_str(), mac) ? dhcp.lookup(mac, &ip, host, sizeof(host))
                                                      : dhcp.reservationIp(r.client.c_str(), &ip);
            if (!ok) {
                r.error = parseMac(r.client.c_str(), mac) ? tr("klient bez dzierżawy DHCP", "client without a DHCP lease") : tr("brak rezerwacji DHCP", "no DHCP reservation");
                continue;
            }
            r.resolvedIp = ip;
            c.clientIp = ip;
            c.clientMask = 0xFFFFFFFF;
        }
        parseNet(r.destIp.c_str(), &c.destIp, &c.destMask, true);
        parsePorts(r.ports.c_str(), c.ports, &c.portCount);
        c.dest = r.dest;
        c.proto = r.proto;
        c.block = r.block;
        compiled[compiledCount++] = c;
    }
    // The mutex is held until the swap ends – the compiled array does not change meanwhile.
    esp_netif_tcpip_exec(&installInTcpip, this);
}

esp_err_t Firewall::installInTcpip(void* ctx)
{
    auto* self = static_cast<Firewall*>(ctx);
    PacketFilter::installRules(self->compiled, self->compiledCount);
    return ESP_OK;
}

// Runs in the tcpip thread: resets the hook counters.
static esp_err_t resetInTcpip(void*)
{
    PacketFilter::resetCounters();
    return ESP_OK;
}

void Firewall::timerCb(void* arg)
{
    static_cast<Firewall*>(arg)->drainLog();
}

// Blocked packets from the hook's ring: log (max 1/s per rule – limited in the hook) and the list for the panel.
void Firewall::drainLog()
{
    PacketFilter::Blocked b;
    while (PacketFilter::popBlocked(&b)) {
        const char* ruleName = "?";
        std::lock_guard<std::mutex> lock(mutex);
        for (const Rule& r : rules) {
            if (r.id == b.ruleId) {
                ruleName = r.name.c_str();
                break;
            }
        }
        char src[16], dst[16];
        ipStr(b.src, src, sizeof(src));
        ipStr(b.dst, dst, sizeof(dst));
        if (b.port != 0) {
            LOG_W("Zablokowano [%s] %s -> %s %s/%u", ruleName, src, dst, ipProtoName(b.proto), b.port);
        } else {
            LOG_W("Zablokowano [%s] %s -> %s %s", ruleName, src, dst, ipProtoName(b.proto));
        }
        if (recent.size() >= MAX_RECENT) {
            recent.erase(recent.begin());
        }
        recent.push_back({ time(nullptr), b });
    }
}

// ---- service ----

esp_err_t Firewall::start()
{
    compile();
    PacketFilter::setFirewallEnabled(true);
    std::lock_guard<std::mutex> lock(mutex);
    LOG_I("Zapora wlaczona, regul: %u", (unsigned)rules.size());
    return ESP_OK;
}

esp_err_t Firewall::stop()
{
    PacketFilter::setFirewallEnabled(false);
    LOG_I("Zapora zatrzymana - ruch przepuszczany");
    return ESP_OK;
}

void Firewall::describe(IOutput& out) const
{
    std::lock_guard<std::mutex> lock(mutex);
    int active = 0;
    for (const Rule& r : rules) {
        active += r.enabled && r.error == nullptr;
    }
    out.printf(tr(" reguł aktywnych %d z %u", " active rules %d of %u"), active, (unsigned)rules.size());
}

// ---- commands ----

void Firewall::registerCommands(CommandExecutor& exec)
{
    exec.add(nullptr,
             { "fw", "[add|del|enable|disable|move|reset-counters ...]",
               { "zapora dla klientów AP: stan i reguły; 'fw ?' – składnia", "firewall for AP clients: status and rules; 'fw ?' – syntax" }, &cmdFw, this });
}

static const char* const FW_USAGE_PL =
    "  fw                                   stan i reguły z licznikami\r\n"
    "  fw add <nazwa> <tcp|udp|both|icmp|all> [porty] [client=...] [dest=internet|router|any]\r\n"
    "         [dest_ip=any|IP|IP/maska] [action=block|allow] [pos=N]\r\n"
    "         porty: 25 | 6881-6889 | 25,465,587 (max 8); client: all | IP | IP/maska | MAC | nazwa rezerwacji\r\n"
    "  fw del <nazwa|nr>                    usuwa regułę\r\n"
    "  fw enable|disable <nazwa|nr>         włącza / wyłącza regułę\r\n"
    "  fw move <nazwa|nr> <pozycja>         zmienia kolejność (pierwsza pasująca decyduje)\r\n"
    "  fw reset-counters                    zeruje liczniki\r\n"
    "Zmiany działają od razu, trwałe po 'save'. Usługa: service start|stop firewall\r\n";
static const char* const FW_USAGE_EN =
    "  fw                                   status and rules with counters\r\n"
    "  fw add <name> <tcp|udp|both|icmp|all> [ports] [client=...] [dest=internet|router|any]\r\n"
    "         [dest_ip=any|IP|IP/mask] [action=block|allow] [pos=N]\r\n"
    "         ports: 25 | 6881-6889 | 25,465,587 (max 8); client: all | IP | IP/mask | MAC | reservation name\r\n"
    "  fw del <name|no>                     deletes a rule\r\n"
    "  fw enable|disable <name|no>          enables / disables a rule\r\n"
    "  fw move <name|no> <position>         changes the order (the first match decides)\r\n"
    "  fw reset-counters                    resets the counters\r\n"
    "Changes apply immediately, permanent after 'save'. Service: service start|stop firewall\r\n";

int Firewall::findRule(const char* token) const
{
    char* end = nullptr;
    long nr = strtol(token, &end, 10);
    if (end != token && *end == '\0') {
        return (nr >= 1 && nr <= (long)rules.size()) ? static_cast<int>(nr - 1) : -1;
    }
    for (size_t i = 0; i < rules.size(); ++i) {
        if (rules[i].name == token) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void Firewall::printRules(IOutput& out) const
{
    out.printf(tr("Zapora: %s; log zablokowanych: %s\r\n", "Firewall: %s; blocked log: %s\r\n"),
               isRunning() ? tr("działa", "running")
                           : tr("zatrzymana – ruch przepuszczany (service start firewall)",
                                "stopped – traffic allowed (service start firewall)"),
               logBlocked ? tr("tak", "yes") : tr("nie", "no"));
    out.printf(tr("Ping z WAN na adres STA: %s (global.wan_ping), odrzucone: %u\r\n",
                  "Ping from WAN to the STA address: %s (global.wan_ping), dropped: %u\r\n"),
               global.values().wanPing ? tr("dozwolony", "allowed") : tr("blokowany", "blocked"),
               (unsigned)PacketFilter::droppedPing());
    out.printf(tr("Przekierowania z ograniczonym źródłem (pf src): odrzucone %u\r\n",
                  "Forwards with a restricted source (pf src): dropped %u\r\n"),
               (unsigned)PacketFilter::droppedPf());
    if (rules.empty()) {
        out.print(tr("Brak reguł (fw add ...)\r\n", "No rules (fw add ...)\r\n"));
        return;
    }
    out.print(tr("  nr nazwa                    klient             proto porty           cel      dest_ip            "
                 "akcja trafienia\r\n",
                 "  no name                     client             proto ports           dest     dest_ip            "
                 "act.  hits\r\n"));
    for (size_t i = 0; i < rules.size(); ++i) {
        const Rule& r = rules[i];
        char client[40];
        if (r.resolvedIp != 0) {
            char ip[16];
            ipStr(r.resolvedIp, ip, sizeof(ip));
            snprintf(client, sizeof(client), "%s(%s)", r.client.c_str(), ip);
        } else {
            strlcpy(client, r.client.c_str(), sizeof(client));
        }
        const char* state = !r.enabled ? tr("  [wyłączona]", "  [disabled]") : r.error ? "  [" : "";
        out.printf("  %-2u %-24s %-18s %-5s %-15s %-8s %-18s %-5s %u%s%s%s\r\n", (unsigned)(i + 1), r.name.c_str(),
                   client, protoName(r.proto), r.ports.empty() ? "*" : r.ports.c_str(), destName(r.dest),
                   r.destIp.c_str(), r.block ? "block" : "allow", (unsigned)PacketFilter::hits(r.id), state,
                   (r.enabled && r.error) ? r.error : "", (r.enabled && r.error) ? "]" : "");
    }
}

CmdResult Firewall::cmdFw(void* ctx, int argc, char** argv, IOutput& out)
{
    auto* self = static_cast<Firewall*>(ctx);
    if (argc == 1) {
        std::lock_guard<std::mutex> lock(self->mutex);
        self->printRules(out);
        return CmdResult::OK;
    }
    const char* sub = argv[1];
    if (strcmp(sub, "add") == 0) {
        return self->cmdAdd(argc, argv, out);
    }
    if (strcmp(sub, "reset-counters") == 0 && argc == 2) {
        esp_netif_tcpip_exec(&resetInTcpip, nullptr);
        out.print(tr("Liczniki wyzerowane\r\n", "Counters reset\r\n"));
        return CmdResult::OK;
    }
    if ((strcmp(sub, "del") == 0 || strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) && argc == 3) {
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            int i = self->findRule(argv[2]);
            if (i < 0) {
                out.print(tr("Nie znaleziono reguły\r\n", "Rule not found\r\n"));
                return CmdResult::ERR_FAIL;
            }
            if (sub[0] == 'd' && sub[1] == 'e') {
                self->rules.erase(self->rules.begin() + i);
            } else {
                self->rules[i].enabled = sub[0] == 'e';
            }
        }
        self->compile();
        out.print(sub[0] == 'e' ? tr("Reguła włączona. Trwałe po 'save'\r\n", "Rule enabled. Permanent after 'save'\r\n")
                  : sub[1] == 'e' ? tr("Reguła usunięta. Trwałe po 'save'\r\n", "Rule deleted. Permanent after 'save'\r\n")
                                  : tr("Reguła wyłączona. Trwałe po 'save'\r\n", "Rule disabled. Permanent after 'save'\r\n"));
        return CmdResult::OK;
    }
    if (strcmp(sub, "move") == 0 && argc == 4) {
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            int i = self->findRule(argv[2]);
            char* end = nullptr;
            long pos = strtol(argv[3], &end, 10);
            if (i < 0) {
                out.print(tr("Nie znaleziono reguły\r\n", "Rule not found\r\n"));
                return CmdResult::ERR_FAIL;
            }
            if (end == argv[3] || *end != '\0' || pos < 1 || pos > (long)self->rules.size()) {
                out.printf(tr("Pozycja: 1-%u\r\n", "Position: 1-%u\r\n"), (unsigned)self->rules.size());
                return CmdResult::ERR_FAIL;
            }
            Rule r = self->rules[i];
            self->rules.erase(self->rules.begin() + i);
            self->rules.insert(self->rules.begin() + (pos - 1), r);
        }
        self->compile();
        out.print(tr("Kolejność zmieniona. Trwałe po 'save'\r\n", "Order changed. Permanent after 'save'\r\n"));
        return CmdResult::OK;
    }
    out.print(tr(FW_USAGE_PL, FW_USAGE_EN));
    return CmdResult::ERR_FAIL;
}

CmdResult Firewall::cmdAdd(int argc, char** argv, IOutput& out)
{
    if (argc < 4) {
        out.print(tr(FW_USAGE_PL, FW_USAGE_EN));
        return CmdResult::ERR_FAIL;
    }
    Rule rule = {};
    rule.enabled = true;
    rule.name = argv[2];
    rule.client = "all";
    rule.dest = D_INTERNET;
    rule.destIp = "any";
    rule.block = true;
    int pos = 0;
    if (!parseProto(argv[3], &rule.proto)) {
        out.print(tr("Protokół: tcp, udp, both, icmp albo all\r\n", "Protocol: tcp, udp, both, icmp or all\r\n"));
        return CmdResult::ERR_FAIL;
    }
    for (int i = 4; i < argc; ++i) {
        const char* a = argv[i];
        const char* eq = strchr(a, '=');
        if (eq == nullptr) {
            if (i != 4) {
                out.printf(tr("Nieznany argument '%s'\r\n", "Unknown argument '%s'\r\n"), a);
                return CmdResult::ERR_FAIL;
            }
            rule.ports = a;
            continue;
        }
        std::string key(a, eq - a);
        const char* v = eq + 1;
        if (key == "client") {
            rule.client = v;
        } else if (key == "dest") {
            if (!parseDest(v, &rule.dest)) {
                out.print(tr("dest: internet, router albo any\r\n", "dest: internet, router or any\r\n"));
                return CmdResult::ERR_FAIL;
            }
        } else if (key == "dest_ip") {
            rule.destIp = v;
        } else if (key == "action") {
            if (!parseAction(v, &rule.block)) {
                out.print(tr("action: block albo allow\r\n", "action: block or allow\r\n"));
                return CmdResult::ERR_FAIL;
            }
        } else if (key == "pos") {
            pos = atoi(v);
            if (pos < 1) {
                out.print(tr("pos: od 1\r\n", "pos: from 1\r\n"));
                return CmdResult::ERR_FAIL;
            }
        } else {
            out.printf(tr("Nieznany argument '%s'\r\n", "Unknown argument '%s'\r\n"), a);
            return CmdResult::ERR_FAIL;
        }
    }
    return storeRule(-1, rule, pos, out);
}

// index -1 = new rule (pos 0 = at the end, otherwise position from 1), otherwise replace.
CmdResult Firewall::storeRule(int index, Rule rule, int pos, IOutput& out)
{
    bool warnRouter = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= static_cast<int>(rules.size())) {
            out.print(tr("Nie ma takiej reguły\r\n", "No such rule\r\n"));
            return CmdResult::ERR_FAIL;
        }
        rule.id = index >= 0 ? rules[index].id : nextId++;
        const char* bad = validate(rule);
        if (bad != nullptr) {
            out.printf("%s\r\n", bad);
            return CmdResult::ERR_FAIL;
        }
        if (index < 0 && rules.size() >= MAX_RULES) {
            out.print(tr("Limit 32 reguł\r\n", "Limit of 32 rules\r\n"));
            return CmdResult::ERR_FAIL;
        }
        if (index >= 0) {
            rules[index] = rule;
        } else if (pos >= 1 && pos <= static_cast<int>(rules.size())) {
            rules.insert(rules.begin() + (pos - 1), rule);
        } else {
            rules.push_back(rule);
        }
        warnRouter = rule.block && rule.dest == D_ROUTER && rule.client == "all" && rule.destIp == "any";
    }
    compile();
    if (warnRouter) {
        out.print(tr("Uwaga: reguła blokuje dostęp wszystkich klientów do routera – może odciąć panel i Telnet "
                     "(konsola COM działa; wyłączenie: service stop firewall)\r\n",
                     "Note: the rule blocks all clients from the router – it may cut off the panel and Telnet "
                     "(the COM console works; to undo: service stop firewall)\r\n"));
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const Rule& r : rules) {
            if (r.id == rule.id && r.error != nullptr) {
                out.printf(tr("Uwaga: %s '%s' – reguła czeka\r\n", "Note: %s '%s' – the rule is waiting\r\n"), r.error,
                           rule.client.c_str());
            }
        }
    }
    if (!isRunning()) {
        out.print(tr("Uwaga: usługa firewall zatrzymana – reguły nie działają (service start firewall)\r\n",
                     "Note: the firewall service is stopped – rules do not apply (service start firewall)\r\n"));
    }
    out.print(index >= 0 ? tr("Zmieniono regułę. Trwałe po 'save'; stan: fw\r\n", "Rule changed. Permanent after 'save'; state: fw\r\n")
                         : tr("Dodano regułę. Trwałe po 'save'; stan: fw\r\n", "Rule added. Permanent after 'save'; state: fw\r\n"));
    return CmdResult::OK;
}

CmdResult Firewall::setRuleJson(const cJSON* json, IOutput& out)
{
    Rule rule = {};
    const char* bad = ruleFromJson(json, &rule.name, &rule.enabled, &rule.client, &rule.proto, &rule.ports, &rule.dest,
                                   &rule.destIp, &rule.block);
    if (bad != nullptr) {
        out.printf("%s\r\n", bad);
        return CmdResult::ERR_ARGS;
    }
    const cJSON* index = cJSON_GetObjectItemCaseSensitive(json, "index");
    return storeRule(cJSON_IsNumber(index) ? index->valueint : -1, rule, 0, out);
}

// ---- panel ----

cJSON* Firewall::summaryJson() const
{
    cJSON* o = cJSON_CreateObject();
    std::lock_guard<std::mutex> lock(mutex);
    int active = 0;
    uint32_t blocked = 0;
    for (const Rule& r : rules) {
        active += r.enabled && r.error == nullptr;
        if (r.block) {
            blocked += PacketFilter::hits(r.id);
        }
    }
    cJSON_AddBoolToObject(o, "running", isRunning());
    cJSON_AddNumberToObject(o, "rules", active);
    cJSON_AddNumberToObject(o, "blocked", blocked);
    cJSON_AddBoolToObject(o, "wan_ping", global.values().wanPing);
    return o;
}

cJSON* Firewall::statusJson() const
{
    cJSON* root = cJSON_CreateObject();
    std::lock_guard<std::mutex> lock(mutex);
    cJSON_AddBoolToObject(root, "running", isRunning());
    cJSON_AddBoolToObject(root, "log_blocked", logBlocked);
    cJSON_AddBoolToObject(root, "wan_ping", global.values().wanPing);
    cJSON_AddNumberToObject(root, "ping_dropped", PacketFilter::droppedPing());
    cJSON_AddNumberToObject(root, "pf_dropped", PacketFilter::droppedPf());
    cJSON_AddNumberToObject(root, "max_rules", MAX_RULES);
    cJSON* list = cJSON_AddArrayToObject(root, "rules");
    for (const Rule& r : rules) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", r.name.c_str());
        cJSON_AddBoolToObject(o, "enabled", r.enabled);
        cJSON_AddStringToObject(o, "client", r.client.c_str());
        if (r.resolvedIp != 0) {
            char ip[16];
            ipStr(r.resolvedIp, ip, sizeof(ip));
            cJSON_AddStringToObject(o, "ip", ip);
        }
        cJSON_AddStringToObject(o, "proto", protoName(r.proto));
        cJSON_AddStringToObject(o, "ports", r.ports.c_str());
        cJSON_AddStringToObject(o, "dest", destName(r.dest));
        cJSON_AddStringToObject(o, "dest_ip", r.destIp.c_str());
        cJSON_AddStringToObject(o, "action", r.block ? "block" : "allow");
        cJSON_AddNumberToObject(o, "hits", PacketFilter::hits(r.id));
        if (r.enabled && r.error != nullptr) {
            cJSON_AddStringToObject(o, "error", r.error);
        }
        cJSON_AddItemToArray(list, o);
    }
    cJSON* rec = cJSON_AddArrayToObject(root, "recent");
    for (auto it = recent.rbegin(); it != recent.rend(); ++it) {
        cJSON* o = cJSON_CreateObject();
        const char* ruleName = "?";
        for (const Rule& r : rules) {
            if (r.id == it->b.ruleId) {
                ruleName = r.name.c_str();
                break;
            }
        }
        char buf[20];
        if (it->when > 1600000000) {
            struct tm t;
            localtime_r(&it->when, &t);
            strftime(buf, sizeof(buf), "%H:%M:%S", &t);
        } else {
            strlcpy(buf, "-", sizeof(buf));
        }
        cJSON_AddStringToObject(o, "time", buf);
        cJSON_AddStringToObject(o, "rule", ruleName);
        ipStr(it->b.src, buf, sizeof(buf));
        cJSON_AddStringToObject(o, "src", buf);
        ipStr(it->b.dst, buf, sizeof(buf));
        cJSON_AddStringToObject(o, "dst", buf);
        cJSON_AddStringToObject(o, "proto", ipProtoName(it->b.proto));
        cJSON_AddNumberToObject(o, "port", it->b.port);
        cJSON_AddItemToArray(rec, o);
    }
    return root;
}
