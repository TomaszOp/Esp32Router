# Esp32Router – configuration / konfiguracja

[English](#english) | [Polski](#polski)

## English

Applies to version 0.2.0. Settings are changed in the web panel or in the console (`config set`); this file describes where they are stored and what values they have.

### 1. Where the settings are

The settings are in JSON files on the LittleFS partition of the ESP, in the `/config` directory (in the console: `config fs` – list of files, `config cat <file>` – contents).

| File | Contents | Panel |
|---|---|---|
| `global.json` | device name, language, time (NTP), logs, management from the upstream network, ping from WAN, service autostart | System → General, System → Services |
| `wifi.json` | upstream network (STA) and own network (AP) | Network → Internet (STA), Network → Wi-Fi (AP) |
| `nat.json` | internet sharing (NAT) | Routing → NAT |
| `dhcp.json` | address pool, lease time, DNS for clients, reservations | Network → LAN / DHCP |
| `dns.json` | local domain, cache, upstream DNS, static entries, blocking settings | DNS → DNS, DNS → Filtering |
| `dns_block.txt` | list of blocked domains (text, one domain per line) | DNS → Filtering |
| `portfwd.json` | port forwards | Routing → Port forwarding |
| `firewall.json` | firewall rules, log of blocked packets | Routing → Firewall |
| `ddns.json` | dynamic DNS: common settings and 3 accounts | Network → DDNS |
| `telnet.json` | Telnet port and sessions | System → Services |
| `web.json` | web panel port and sessions | System → Services |
| `web_auth.json` | panel password hash (created after a password change) | System → Panel password |

A file does not exist until the module's settings are saved for the first time – until then the default values apply (ch. 4).

### 2. Change, save, revert

- A change (panel, `config set`, module commands, e.g. `dhcp reserve`) goes to memory first. Each field shows whether it applies **immediately** or **after restart** of the service or device.
- **Save** in the panel (the "Unsaved changes" bar at the top) or `save` in the console writes the changed files; `save <module>` – only one module (e.g. `save dhcp`). Without saving, changes disappear after a restart.
- **Differences**: System → Configuration or `config diff [module]` – fields changed but not saved (old → new value, passwords hidden).
- **Revert**: the button in the changes bar or `config revert [module]` – reloads the saved file. Exceptions: the DNS block list (`dns block reload`) and the panel password (saved immediately).
- `config show [module]` shows every field with its current value, source (`file`, `default`, `changed, unsaved`) and allowed range; `config get <module> <field>` – one field; `config unset <module> <field>` – restores the default value.
- A nested field is written with a dot: `config set global log.level debug`, `config set wifi ap.max_clients 4`, `config set ddns 2.provider noip`. Module and field names can be abbreviated when unambiguous.
- An invalid value in a file (wrong type, out of range) is replaced by the default with a warning in the log; a damaged JSON file is moved to `<file>.bad` and the module starts with default values.

### 3. Files – fields

Default values and number ranges are in the table in ch. 4. Below – the meaning of the fields and the allowed text values.

#### `global.json`

| Field | Values | Meaning |
|---|---|---|
| `hostname` | 1–32 characters `a-z A-Z 0-9 -` | router name in the upstream network and in DNS (`esp32router.lan`) |
| `language` | `pl` / `en` | panel and console language; immediately |
| `console_utf8` | true/false | COM/Telnet console in UTF-8; disabled – Polish letters without diacritics; immediately |
| `log.level` | `none` / `error` / `warn` / `info` / `debug` | log level; immediately |
| `log.output` | `console` | log output |
| `time.ntp_enabled`, `time.ntp_server`, `time.timezone` | true/false, server name, zone in POSIX TZ format | time from the internet; after restart |
| `mgmt_from_sta` | true/false | panel and Telnet also available from the upstream network (by default only from the AP network); immediately |
| `wan_ping` | true/false | answer ping from the upstream network to the router's STA address; immediately |
| `services.<service>` | true/false | service autostart: `serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`; applies to the next start |

#### `wifi.json`

| Field | Values | Meaning |
|---|---|---|
| `sta.ssid`, `sta.pass` | max 32 / max 63 characters | upstream network (with internet); empty SSID – the router does not connect |
| `ap.ssid` | 1–32 characters | own network name; by default the name from `Secrets.h` with the end of the MAC address, e.g. `Esp32Router-A1B2` |
| `ap.pass` | empty (open network) or 8–63 characters | own network password |
| `ap.max_clients`, `ap.ip`, `ap.mask` | number, IPv4, mask (max /30) | number of clients, router address and AP network mask |

The AP channel is chosen automatically – it is the same as the channel of the upstream network. Wi-Fi fields apply after a restart; to change the network immediately use `wifi sta` / `wifi ap` or the panel buttons.

#### `nat.json`

`enabled` (true/false) – internet sharing for AP clients; immediately (`nat on|off`).

#### `dhcp.json`

| Field | Values | Meaning |
|---|---|---|
| `pool_start`, `pool_end` | IPv4 in the AP network, max 128 addresses | address pool for clients |
| `lease_time` | seconds | lease time |
| `dns` | `auto` or IPv4 | DNS given to clients; `auto` = router address (while the dns service runs) |
| `reservations` | list of up to 32 × `{ "mac", "ip", "name" }` | fixed addresses for devices (`dhcp reserve`) |

#### `dns.json` and `dns_block.txt`

| Field | Values | Meaning |
|---|---|---|
| `domain` | 1–32 characters `a-z 0-9 - .` | local domain, e.g. `nas.lan` |
| `local_ttl`, `cache_size`, `cache_sweep_s` | numbers | validity of local answers, cache size, purging of expired entries |
| `dhcp_names` | true/false | answering with device names from DHCP |
| `block.enabled`, `block.response` | true/false, `zero` / `nxdomain` | domain blocking and the answer for blocked names |
| `upstream` | `["auto"]` or 1–2 IPv4 addresses | upstream DNS (`dns upstream`) |
| `entries` | list of up to 64 × `{ "name", "ip" }` | static entries (`dns add`) |

`dns_block.txt` – one domain per line (also hosts format `0.0.0.0 domain`), comments start with `#`; blocking covers subdomains.

#### `portfwd.json`

`rules` – list of rules `{ "name", "proto", "ext_port", "target", "int_port", "enabled", "src" }`:
- `proto`: `tcp` / `udp` / `both`; `ext_port`, `int_port`: 1–65535 (`int_port` defaults to `ext_port`),
- `target`: IPv4 address of an AP client or a DHCP reservation name,
- `src`: empty (everyone) or up to 4 addresses or subnets, comma-separated – who from outside may use it,
- up to 32 entries in total (`both` counts twice).

#### `firewall.json`

`log_blocked` (true/false) and `rules` – list of up to 32 rules `{ "name", "enabled", "client", "proto", "ports", "dest", "dest_ip", "action" }`:
- `client`: `all`, IP, IP/mask, MAC or DHCP reservation name,
- `proto`: `tcp` / `udp` / `both` / `icmp` / `all`; `ports`: empty (all), a port, a range, a list of up to 8, e.g. `25,6881-6889`,
- `dest`: `internet` / `router` / `any`; `dest_ip`: `any`, IP or IP/mask; `action`: `block` / `allow`.

#### `ddns.json`

Common fields `ip_source` (`public` / `provider` / `sta`), `check_url`, `check_interval_s`, `force_update_h`, `retry_s` and three accounts `"1"`, `"2"`, `"3"`, each with the fields `enabled`, `provider` (`duckdns` / `dynu` / `noip` / `freedns` / `desec` / `custom`), `hostname`, `username`, `secret`, `custom_url`. The `secret` field is hidden in `config show`, differences and the panel.

#### `telnet.json`, `web.json`

`port`, `max_sessions`, `idle_timeout_s` (Telnet) / `session_timeout_s` (panel) – apply after `service restart telnet|web`.

### 4. Default values and ranges

The table is generated from `include/Defaults.h` (`tools/defaults_table.py`) – always consistent with the firmware.

<!-- defaults-en:begin -->
| File | Field | Default | Range | Notes |
|---|---|---|---|---|
| global.json | `version` | `1` |  | format version of global.json (not a setting) |
| global.json | `hostname` | `"esp32router"` |  |  |
| global.json | `language` | `"pl"` |  | pl\|en |
| global.json | `console_utf8` | `false` |  | COM/Telnet console in UTF-8 (false = Polish letters as ASCII) |
| global.json | `log.level` | `"info"` |  |  |
| global.json | `log.output` | `"console"` |  |  |
| global.json | `time.ntp_enabled` | `true` |  |  |
| global.json | `time.ntp_server` | `"pool.ntp.org"` |  |  |
| global.json | `time.timezone` | `"CET-1CEST,M3.5.0,M10.5.0/3"` |  |  |
| global.json | `mgmt_from_sta` | `false` |  |  |
| global.json | `wan_ping` | `false` |  |  |
| global.json | `services.serial` | `true` |  |  |
| global.json | `services.telnet` | `true` |  |  |
| global.json | `services.dhcp` | `true` |  |  |
| global.json | `services.dns` | `true` |  |  |
| global.json | `services.ddns` | `false` |  |  |
| global.json | `services.firewall` | `false` |  |  |
| global.json | `services.web` | `true` |  |  |
| wifi.json | `sta.ssid` | `""` |  | empty = do not connect |
| wifi.json | `sta.pass` | `""` |  |  |
| wifi.json | `ap.ssid` | `SECRET_AP_SSID` |  | + "-XXXX" (last 2 bytes of the AP MAC) |
| wifi.json | `ap.pass` | `SECRET_AP_PASS` |  | empty = open network |
| wifi.json | `ap.max_clients` | `8` | 1 – 15 (driver limit) |  |
| wifi.json | `ap.ip` | `192.168.4.1` |  |  |
| wifi.json | `ap.mask` | `255.255.255.0` |  |  |
| nat.json | `enabled` | `true` |  |  |
| dhcp.json | `pool_start` | `<AP network>.100` |  | pool: &lt;AP network&gt;.100 – .199 |
| dhcp.json | `pool_end` | `<AP network>.199` |  |  |
| dhcp.json | `lease_time` | `7200` | 60 – 604800 |  |
| dhcp.json | `dns` | `"auto"` |  |  |
| dns.json | `domain` | `"lan"` |  |  |
| dns.json | `local_ttl` | `300` | 10 – 86400 |  |
| dns.json | `cache_size` | `256` | 16 – 4096 |  |
| dns.json | `cache_sweep_s` | `60` | 10 – 3600 |  |
| dns.json | `dhcp_names` | `true` |  |  |
| dns.json | `block.enabled` | `true` |  |  |
| dns.json | `block.response` | `"zero"` |  |  |
| telnet.json | `port` | `23` | 1 – 65535 |  |
| telnet.json | `max_sessions` | `2` | 1 – 4 (size of the session table) |  |
| telnet.json | `idle_timeout_s` | `600` | 0 – 86400 | 0 = no limit |
| web.json | `port` | `80` | 1 – 65535 |  |
| web.json | `session_timeout_s` | `1800` | 60 – 86400 |  |
| web.json | `max_sessions` | `4` | 1 – 8 (size of the session table) |  |
| ddns.json | `ip_source` | `"public"` |  |  |
| ddns.json | `check_url` | `"https://api.ipify.org"` |  |  |
| ddns.json | `check_interval_s` | `300` | 300 – 21600 |  |
| ddns.json | `force_update_h` | `24` | 0 – 720 |  |
| ddns.json | `retry_s` | `300` | 60 – 3600 |  |
| ddns.json | `<n>.enabled` | `false` |  | account; the user enables it after entering the data |
| ddns.json | `<n>.provider` | `"duckdns"` |  |  |
| ddns.json | `<n>.hostname` | `""` |  |  |
| ddns.json | `<n>.username` | `""` |  |  |
| ddns.json | `<n>.secret` | `""` |  |  |
| ddns.json | `<n>.custom_url` | `""` |  |  |
| firewall.json | `log_blocked` | `true` |  |  |
<!-- defaults-en:end -->

`SECRET_AP_SSID`, `SECRET_AP_PASS` – values from `include/Secrets.h` set before building ("First start" in [`README.md`](README.md#english)). Default values can be changed before building without changing the code – [`source.md`](source.md#english), ch. 5.

### 5. Backup and factory reset

- **Backup**: System → Configuration → Download backup – one JSON file with the saved configuration files (without the panel password). Unsaved changes are not included. The file contains Wi-Fi passwords and DDNS tokens.
- **Restore**: System → Configuration → Restore from file – replaces the files and restarts the device.
- **Factory reset**: System → Configuration → Factory reset or `config factory-reset yes` – deletes all files (including the panel password and the block list) and restarts. After the reset: the router does not connect to the upstream network, the AP has the name and password from `Secrets.h`, the panel password is from `Secrets.h`, the language and the other settings are as in the table in ch. 4.

---

## Polski

Dotyczy wersji 0.2.0. Ustawienia zmienia się w panelu WWW albo w konsoli (`config set`); ten plik opisuje, gdzie są zapisane i jakie mają wartości.

### 1. Gdzie są ustawienia

Ustawienia są w plikach JSON na partycji LittleFS w ESP, w katalogu `/config` (w konsoli: `config fs` – lista plików, `config cat <plik>` – zawartość).

| Plik | Co zawiera | Panel |
|---|---|---|
| `global.json` | nazwa urządzenia, język, czas (NTP), logi, zarządzanie od strony sieci nadrzędnej, ping z WAN, autostart usług | System → Ogólne, System → Usługi |
| `wifi.json` | sieć nadrzędna (STA) i własna sieć (AP) | Sieć → Internet (STA), Sieć → Wi-Fi (AP) |
| `nat.json` | udostępnianie internetu (NAT) | Routing → NAT |
| `dhcp.json` | pula adresów, czas dzierżawy, DNS dla klientów, rezerwacje | Sieć → LAN / DHCP |
| `dns.json` | domena lokalna, cache, DNS nadrzędny, wpisy statyczne, ustawienia blokowania | DNS → DNS, DNS → Filtrowanie |
| `dns_block.txt` | lista blokowanych domen (tekst, jedna domena w linii) | DNS → Filtrowanie |
| `portfwd.json` | przekierowania portów | Routing → Przekierowanie portów |
| `firewall.json` | reguły zapory, log zablokowanych | Routing → Firewall |
| `ddns.json` | dynamiczny DNS: ustawienia wspólne i 3 konta | Sieć → DDNS |
| `telnet.json` | port i sesje Telnetu | System → Usługi |
| `web.json` | port i sesje panelu WWW | System → Usługi |
| `web_auth.json` | skrót hasła panelu (powstaje po zmianie hasła) | System → Hasło panelu |

Pliku nie ma, dopóki ustawienia modułu nie zostaną pierwszy raz zapisane – do tego czasu obowiązują wartości domyślne (rozdz. 4).

### 2. Zmiana, zapis, cofanie

- Zmiana (panel, `config set`, polecenia modułów, np. `dhcp reserve`) trafia najpierw do pamięci. Przy każdym polu widać, czy działa **od razu**, czy **po restarcie** usługi albo urządzenia.
- **Zapisz** w panelu (pasek „Niezapisane zmiany” na górze) albo `save` w konsoli zapisuje zmienione pliki; `save <moduł>` – tylko jeden moduł (np. `save dhcp`). Bez zapisu zmiany znikają po restarcie.
- **Różnice**: System → Konfiguracja albo `config diff [moduł]` – pola zmienione, a niezapisane (stara → nowa wartość, hasła ukryte).
- **Cofnij**: przycisk w pasku zmian albo `config revert [moduł]` – wczytuje zapisany plik ponownie. Wyjątek: lista blokad DNS (`dns block reload`) i hasło panelu (zapisywane od razu).
- `config show [moduł]` pokazuje każde pole z bieżącą wartością, źródłem (`plik`, `domyślne`, `zmienione, niezapisane`) i dozwolonym zakresem; `config get <moduł> <pole>` – jedno pole; `config unset <moduł> <pole>` – przywraca wartość domyślną.
- Pole zagnieżdżone zapisuje się z kropką: `config set global log.level debug`, `config set wifi ap.max_clients 4`, `config set ddns 2.provider noip`. Nazwy modułów i pól można skracać, jeśli są jednoznaczne.
- Błędna wartość w pliku (zły typ, poza zakresem) jest zastępowana domyślną z ostrzeżeniem w logu; uszkodzony plik JSON jest przenoszony do `<plik>.bad` i moduł startuje z wartościami domyślnymi.

### 3. Pliki – pola

Wartości domyślne i zakresy liczb są w tabeli w rozdz. 4. Poniżej – znaczenie pól i dozwolone wartości tekstowe.

#### `global.json`

| Pole | Wartości | Znaczenie |
|---|---|---|
| `hostname` | 1–32 znaki `a-z A-Z 0-9 -` | nazwa routera w sieci nadrzędnej i w DNS (`esp32router.lan`) |
| `language` | `pl` / `en` | język panelu i konsoli; od razu |
| `console_utf8` | true/false | konsola COM/Telnet w UTF-8; wyłączone – polskie znaki jako litery bez ogonków; od razu |
| `log.level` | `none` / `error` / `warn` / `info` / `debug` | poziom logów; od razu |
| `log.output` | `console` | wyjście logów |
| `time.ntp_enabled`, `time.ntp_server`, `time.timezone` | true/false, nazwa serwera, strefa w formacie POSIX TZ | czas z internetu; po restarcie |
| `mgmt_from_sta` | true/false | panel i Telnet dostępne także od strony sieci nadrzędnej (domyślnie tylko z sieci AP); od razu |
| `wan_ping` | true/false | odpowiedź na ping z sieci nadrzędnej na adres STA routera; od razu |
| `services.<usługa>` | true/false | autostart usług: `serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`; dotyczy następnego uruchomienia |

#### `wifi.json`

| Pole | Wartości | Znaczenie |
|---|---|---|
| `sta.ssid`, `sta.pass` | max 32 / max 63 znaki | sieć nadrzędna (z internetem); puste SSID – router się nie łączy |
| `ap.ssid` | 1–32 znaki | nazwa własnej sieci; domyślnie nazwa z `Secrets.h` i końcówka adresu MAC, np. `Esp32Router-A1B2` |
| `ap.pass` | puste (sieć otwarta) albo 8–63 znaki | hasło własnej sieci |
| `ap.max_clients`, `ap.ip`, `ap.mask` | liczba, IPv4, maska (max /30) | liczba klientów, adres routera i maska sieci AP |

Kanał AP dobiera się sam – jest taki jak kanał sieci nadrzędnej. Pola Wi-Fi działają po restarcie; natychmiastowa zmiana sieci: `wifi sta` / `wifi ap` albo przyciski w panelu.

#### `nat.json`

`enabled` (true/false) – udostępnianie internetu klientom AP; od razu (`nat on|off`).

#### `dhcp.json`

| Pole | Wartości | Znaczenie |
|---|---|---|
| `pool_start`, `pool_end` | IPv4 w sieci AP, max 128 adresów | pula adresów dla klientów |
| `lease_time` | sekundy | czas dzierżawy |
| `dns` | `auto` albo IPv4 | DNS podawany klientom; `auto` = adres routera (gdy działa usługa dns) |
| `reservations` | lista do 32 × `{ "mac", "ip", "name" }` | stałe adresy dla urządzeń (`dhcp reserve`) |

#### `dns.json` i `dns_block.txt`

| Pole | Wartości | Znaczenie |
|---|---|---|
| `domain` | 1–32 znaki `a-z 0-9 - .` | domena lokalna, np. `nas.lan` |
| `local_ttl`, `cache_size`, `cache_sweep_s` | liczby | czas ważności odpowiedzi lokalnych, rozmiar cache, czyszczenie wygasłych wpisów |
| `dhcp_names` | true/false | odpowiadanie nazwami urządzeń z DHCP |
| `block.enabled`, `block.response` | true/false, `zero` / `nxdomain` | blokowanie domen i odpowiedź dla zablokowanych |
| `upstream` | `["auto"]` albo 1–2 adresy IPv4 | DNS nadrzędny (`dns upstream`) |
| `entries` | lista do 64 × `{ "name", "ip" }` | wpisy statyczne (`dns add`) |

`dns_block.txt` – jedna domena w linii (także format hosts `0.0.0.0 domena`), komentarze od `#`; blokada obejmuje poddomeny.

#### `portfwd.json`

`rules` – lista reguł `{ "name", "proto", "ext_port", "target", "int_port", "enabled", "src" }`:
- `proto`: `tcp` / `udp` / `both`; `ext_port`, `int_port`: 1–65535 (`int_port` domyślnie = `ext_port`),
- `target`: adres IPv4 klienta AP albo nazwa rezerwacji DHCP,
- `src`: puste (wszyscy) albo do 4 adresów lub podsieci po przecinku – kto z zewnątrz może korzystać,
- łącznie do 32 wpisów (`both` liczy się podwójnie).

#### `firewall.json`

`log_blocked` (true/false) i `rules` – lista do 32 reguł `{ "name", "enabled", "client", "proto", "ports", "dest", "dest_ip", "action" }`:
- `client`: `all`, IP, IP/maska, MAC albo nazwa rezerwacji DHCP,
- `proto`: `tcp` / `udp` / `both` / `icmp` / `all`; `ports`: pusty (wszystkie), port, zakres, lista do 8, np. `25,6881-6889`,
- `dest`: `internet` / `router` / `any`; `dest_ip`: `any`, IP albo IP/maska; `action`: `block` / `allow`.

#### `ddns.json`

Pola wspólne `ip_source` (`public` / `provider` / `sta`), `check_url`, `check_interval_s`, `force_update_h`, `retry_s` i trzy konta `"1"`, `"2"`, `"3"`, każde z polami `enabled`, `provider` (`duckdns` / `dynu` / `noip` / `freedns` / `desec` / `custom`), `hostname`, `username`, `secret`, `custom_url`. Pole `secret` jest ukrywane w `config show`, różnicach i panelu.

#### `telnet.json`, `web.json`

`port`, `max_sessions`, `idle_timeout_s` (Telnet) / `session_timeout_s` (panel) – działają po `service restart telnet|web`.

### 4. Wartości domyślne i zakresy

Tabela jest generowana z `include/Defaults.h` (`tools/defaults_table.py`) – zawsze zgodna z firmware.

<!-- defaults:begin -->
| Plik | Pole | Domyślnie | Zakres | Uwagi |
|---|---|---|---|---|
| global.json | `version` | `1` |  | wersja formatu global.json (nie ustawienie) |
| global.json | `hostname` | `"esp32router"` |  |  |
| global.json | `language` | `"pl"` |  | pl\|en |
| global.json | `console_utf8` | `false` |  | konsola COM/Telnet w UTF-8 (false = polskie znaki jako ASCII) |
| global.json | `log.level` | `"info"` |  |  |
| global.json | `log.output` | `"console"` |  |  |
| global.json | `time.ntp_enabled` | `true` |  |  |
| global.json | `time.ntp_server` | `"pool.ntp.org"` |  |  |
| global.json | `time.timezone` | `"CET-1CEST,M3.5.0,M10.5.0/3"` |  |  |
| global.json | `mgmt_from_sta` | `false` |  |  |
| global.json | `wan_ping` | `false` |  |  |
| global.json | `services.serial` | `true` |  |  |
| global.json | `services.telnet` | `true` |  |  |
| global.json | `services.dhcp` | `true` |  |  |
| global.json | `services.dns` | `true` |  |  |
| global.json | `services.ddns` | `false` |  |  |
| global.json | `services.firewall` | `false` |  |  |
| global.json | `services.web` | `true` |  |  |
| wifi.json | `sta.ssid` | `""` |  | puste = nie łącz |
| wifi.json | `sta.pass` | `""` |  |  |
| wifi.json | `ap.ssid` | `SECRET_AP_SSID` |  | + "-XXXX" (ostatnie 2 bajty MAC AP) |
| wifi.json | `ap.pass` | `SECRET_AP_PASS` |  | puste = sieć otwarta |
| wifi.json | `ap.max_clients` | `8` | 1 – 15 (limit sterownika) |  |
| wifi.json | `ap.ip` | `192.168.4.1` |  |  |
| wifi.json | `ap.mask` | `255.255.255.0` |  |  |
| nat.json | `enabled` | `true` |  |  |
| dhcp.json | `pool_start` | `<sieć AP>.100` |  | pula: &lt;sieć AP&gt;.100 – .199 |
| dhcp.json | `pool_end` | `<sieć AP>.199` |  |  |
| dhcp.json | `lease_time` | `7200` | 60 – 604800 |  |
| dhcp.json | `dns` | `"auto"` |  |  |
| dns.json | `domain` | `"lan"` |  |  |
| dns.json | `local_ttl` | `300` | 10 – 86400 |  |
| dns.json | `cache_size` | `256` | 16 – 4096 |  |
| dns.json | `cache_sweep_s` | `60` | 10 – 3600 |  |
| dns.json | `dhcp_names` | `true` |  |  |
| dns.json | `block.enabled` | `true` |  |  |
| dns.json | `block.response` | `"zero"` |  |  |
| telnet.json | `port` | `23` | 1 – 65535 |  |
| telnet.json | `max_sessions` | `2` | 1 – 4 (rozmiar tablicy sesji) |  |
| telnet.json | `idle_timeout_s` | `600` | 0 – 86400 | 0 = bez limitu |
| web.json | `port` | `80` | 1 – 65535 |  |
| web.json | `session_timeout_s` | `1800` | 60 – 86400 |  |
| web.json | `max_sessions` | `4` | 1 – 8 (rozmiar tablicy sesji) |  |
| ddns.json | `ip_source` | `"public"` |  |  |
| ddns.json | `check_url` | `"https://api.ipify.org"` |  |  |
| ddns.json | `check_interval_s` | `300` | 300 – 21600 |  |
| ddns.json | `force_update_h` | `24` | 0 – 720 |  |
| ddns.json | `retry_s` | `300` | 60 – 3600 |  |
| ddns.json | `<n>.enabled` | `false` |  | konto; włącza użytkownik (Włączone) po wpisaniu danych |
| ddns.json | `<n>.provider` | `"duckdns"` |  |  |
| ddns.json | `<n>.hostname` | `""` |  |  |
| ddns.json | `<n>.username` | `""` |  |  |
| ddns.json | `<n>.secret` | `""` |  |  |
| ddns.json | `<n>.custom_url` | `""` |  |  |
| firewall.json | `log_blocked` | `true` |  |  |
<!-- defaults:end -->

`SECRET_AP_SSID`, `SECRET_AP_PASS` – wartości z `include/Secrets.h` ustawione przed kompilacją (rozdz. „Pierwsze uruchomienie” w [`README.md`](README.md#polski)). Wartości domyślne można zmienić przed kompilacją bez zmiany kodu – [`source.md`](source.md#polski), rozdz. 5.

### 5. Kopia zapasowa i reset fabryczny

- **Kopia**: System → Konfiguracja → Pobierz kopię – jeden plik JSON z zapisanymi plikami konfiguracji (bez hasła panelu). Niezapisane zmiany nie wchodzą do kopii. Plik zawiera hasła Wi-Fi i tokeny DDNS.
- **Przywracanie**: System → Konfiguracja → Przywróć z pliku – zastępuje pliki i restartuje urządzenie.
- **Reset fabryczny**: System → Konfiguracja → Reset fabryczny albo `config factory-reset yes` – usuwa wszystkie pliki (także hasło panelu i listę blokad) i restartuje. Po resecie: router nie łączy się z siecią nadrzędną, AP z nazwą i hasłem z `Secrets.h`, hasło panelu z `Secrets.h`, język i pozostałe ustawienia jak w tabeli w rozdz. 4.
