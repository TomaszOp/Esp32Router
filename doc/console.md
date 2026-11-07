# Esp32Router – console / konsola (COM, Telnet)

[English](#english) | [Polski](#polski)

## English

Applies to version 0.2.0. The same commands work in the COM console, in Telnet and in the panel terminal (System → Diagnostics).

### 1. Connection

#### COM (USB)

- USB cable to the **USB** connector of the board (native USB of the ESP32-S3, not the UART connector). The computer sees a serial port (Linux `/dev/ttyACM0`, Windows `COMx`).
- Program: `pio device monitor`, PuTTY, minicom, screen… The speed does not matter (USB); PlatformIO uses 115200.
- **Note: opening the port resets the ESP** (DTR/RTS signals) – the router starts again and clients lose the connection for a moment.
- ESP-IDF logs appear on the port as well. The COM console always works (also when the firewall or settings cut off the panel and Telnet) – unless the `serial` service was stopped.

#### Telnet

- `telnet 192.168.4.1` (port 23), from a computer in the AP network. No password – therefore only from the AP side; from the upstream network after enabling `mgmt_from_sta` (`config set global mgmt_from_sta true`).
- Up to 2 sessions at a time, disconnect after 10 min of inactivity (`telnet.json`). `exit` or Ctrl+D on an empty line ends the session.

#### Polish characters

By default the console sends Polish letters without diacritics (works in any terminal). In a UTF-8 terminal you can enable the letters with diacritics: `config set global console_utf8 true` (immediately; permanent after `save`). Only ASCII characters can be typed.

### 2. Line editing

- Enter – execute, Backspace – delete a character, Ctrl+C – discard the line.
- **Up/down** arrows – history of the last 10 commands. Lines up to 127 characters.
- An argument with spaces in quotes: `wifi sta "My network" password`.
- Commands and subcommands can be **abbreviated** when unambiguous: `dh le` = `dhcp leases`, `serv li` = `service list`.
- `help` – list of commands; `help <command>` or `<group>` – subcommands; `<command> ?` – syntax and description.
- Message language: `lang en` / `lang pl` (together with the panel).

### 3. Commands

`<…>` – required, `[…]` – optional, `a|b` – one of. "Permanent after save" – the change applies immediately but disappears after a restart without `save`. Argument placeholders in the built-in syntax (e.g. `<modul> <pole>`) are Polish words.

#### General

| Command | Description |
|---|---|
| `help [command]` | list of commands or description of a command/group |
| `status` | firmware version, STA/AP state, NAT, clients, uptime, memory |
| `version` | firmware version, build date, ESP-IDF version |
| `clients` | list of AP clients (MAC, IP, name, signal) |
| `lang [pl\|en]` | panel and console language (without argument – current); permanent after save |
| `save [module]` | saves the changed configuration (all modules or one) |
| `reboot` | restarts the device |
| `exit` | ends a Telnet session |

#### Configuration – `config`

| Command | Description |
|---|---|
| `config show [module]` | current configuration with the source and range of each value |
| `config get <module> <field>` | value of one field (`field.subfield`) |
| `config set <module> <field> <value>` | changes a field (permanent after save) |
| `config unset <module> <field>` | restores the default value of a field |
| `config diff [module]` | fields changed but not saved |
| `config revert [module]` | discards unsaved changes (reloads from the file) |
| `config fs` | LittleFS state and the list of files in `/config` |
| `config cat <file>` | contents of a file in `/config` |
| `config factory-reset yes` | deletes the configuration files and restarts |

Modules: `global`, `wifi`, `nat`, `dhcp`, `dns`, `portfwd`, `firewall`, `ddns`, `telnet`, `web`. Fields: [`configuration.md`](configuration.md#english).

#### Wi-Fi – `wifi`

| Command | Description |
|---|---|
| `wifi sta "<ssid>" [password]` | upstream network, connects immediately; `""` = do not connect (permanent after save) |
| `wifi ap "<ssid>" [password]` | own network; no password = open, password min. 8 characters; disconnects the clients (permanent after save) |
| `wifi scan` | Wi-Fi networks in range |

#### NAT – `nat`

| Command | Description |
|---|---|
| `nat on` / `nat off` | enables / disables internet sharing (permanent after save) |

#### DHCP – `dhcp`

| Command | Description |
|---|---|
| `dhcp leases` | active leases |
| `dhcp reservations` | list of reservations |
| `dhcp reserve <mac> [ip] [name]` | reservation; without ip = the client's current address (permanent after save) |
| `dhcp unreserve <mac\|ip\|no>` | deletes a reservation (permanent after save) |
| `dhcp release <mac\|ip>` | removes a lease (the client will get an address again) |

#### DNS – `dns`, `dns block`

| Command | Description |
|---|---|
| `dns list` | local names with their source |
| `dns add <name> <ip>` | static entry (permanent after save) |
| `dns del <name>` | deletes a static entry (permanent after save) |
| `dns upstream [auto \| <ip> [ip2]]` | upstream DNS: show or change (permanent after save) |
| `dns lookup <name>` | where the answer for a name will come from |
| `dns cache [clear]` | cache usage or clearing it |
| `dns stats` | query statistics |
| `dns block add <domain>` | blocks a domain and its subdomains (permanent after save) |
| `dns block del <domain>` | removes a domain from the list (permanent after save) |
| `dns block list [phrase]` | list (max 50 items) |
| `dns block check <name>` | whether a name is blocked and by which entry |
| `dns block on` / `off` | enables / disables blocking (permanent after save) |
| `dns block reload` | reloads the list from the file (discards unsaved changes) |
| `dns block stats` | block count and most blocked domains |

#### Port forwarding – `pf`

| Command | Description |
|---|---|
| `pf list` | rules and their state |
| `pf add <tcp\|udp\|both> <ext_port> <ip\|reservation_name> [int_port] [name] [src=IP[/mask],...]` | adds a rule (permanent after save); `src` – who from outside may use it (max 4) |
| `pf del <no\|name>` | deletes a rule (permanent after save) |
| `pf enable` / `disable <no\|name>` | enables / disables a rule |
| `pf src <no\|name> <IP[/mask],...\|->` | allowed sources of a rule (`-` = everyone) |
| `pf apply` | installs the rules again (e.g. after changing DHCP reservations) |

#### Firewall – `fw`

| Command | Description |
|---|---|
| `fw` | state, ping from WAN, drop counters, rules with hit counters |
| `fw add <name> <tcp\|udp\|both\|icmp\|all> [ports] [client=…] [dest=internet\|router\|any] [dest_ip=any\|IP\|IP/mask] [action=block\|allow] [pos=N]` | adds a rule (at the end or at position N from 1) |
| `fw del <name\|no>` | deletes a rule |
| `fw enable` / `disable <name\|no>` | enables / disables a rule |
| `fw move <name\|no> <position>` | changes the order (the first match decides) |
| `fw reset-counters` | resets the counters |
| `fw ?` | full syntax |

`ports`: `25`, `6881-6889`, `25,465,587` (max 8); `client`: `all`, IP, IP/mask, MAC or reservation name. Service: `service start|stop firewall`.

#### DDNS – `ddns`

| Command | Description |
|---|---|
| `ddns` | state: service, address source, public IP, result of each account |
| `ddns update [1-3]` | update now (all accounts or one) |
| `ddns test` | only checks the public IP |

Accounts are set with `config set ddns <n>.<field> <value>`.

#### Services – `service`

| Command | Description |
|---|---|
| `service list` | service state and autostart |
| `service start <name>` | starts a service |
| `service stop <name> [yes]` | stops a service (`yes` when it is the last running console) |
| `service restart <name>` | restarts a service with the current configuration |

Services: `serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`. Autostart: `config set global services.<name> true|false` + `save`.

#### Telnet and panel – `telnet`, `web`

| Command | Description |
|---|---|
| `telnet sessions` | list of Telnet sessions |
| `telnet kick <no>` | disconnects a session |
| `web passwd <new>` | changes the panel password (8–64 characters, saved immediately) |
| `web passwd reset` | factory password from `Secrets.h` |
| `web sessions` | logged-in panel sessions |
| `web logout-all` | logs out all sessions |

#### System – `sys`

| Command | Description |
|---|---|
| `sys ota` | firmware partitions: running, for update, versions |
| `sys mem` | SRAM/PSRAM memory, flash, lwIP, task stacks |
| `sys log [bytes]` | recent logs from the buffer (default 2048 B) |

### 4. Examples

**Connecting to the upstream network**
```
wifi scan
wifi sta "HomeWiFi" secretpassword
status
save
```

**Fixed address for a printer** (MAC from `dhcp leases` or `clients`)
```
dhcp reserve aa:bb:cc:11:22:33 192.168.4.50 drukarka
save
```
The printer gets the address when the lease is renewed (immediately: `dhcp release aa:bb:cc:11:22:33`); in DNS as `drukarka.lan`.

**SSH to a Raspberry Pi, only from the office**
```
dhcp reserve dc:a6:32:00:00:01 192.168.4.10 pi
pf add tcp 2222 pi 22 ssh-pi src=203.0.113.7
pf list
save
```
From the upstream network: `ssh -p 2222 <router STA address>`. From the internet – also forward port 2222 on the upstream router to the ESP STA address.

**Camera without internet access**
```
dhcp reserve 11:22:33:44:55:66 192.168.4.60 kamera
fw add kamera all client=kamera
service start firewall
config set global services.firewall true
save
```

**DuckDNS account**
```
config set ddns 1.provider duckdns
config set ddns 1.hostname myrouter
config set ddns 1.secret TOKEN-FROM-DUCKDNS
config set ddns 1.enabled true
service start ddns
config set global services.ddns true
save
ddns
```

**Showing and reverting changes**
```
config diff
config revert dhcp
```

---

## Polski

Dotyczy wersji 0.2.0. Te same polecenia działają w konsoli COM, w Telnecie i w terminalu panelu (System → Diagnostyka).

### 1. Połączenie

#### COM (USB)

- Kabel USB do złącza **USB** płytki (natywne USB ESP32-S3, nie złącze UART). Komputer widzi port szeregowy (Linux `/dev/ttyACM0`, Windows `COMx`).
- Program: `pio device monitor`, PuTTY, minicom, screen… Prędkość nie ma znaczenia (USB), w PlatformIO ustawione 115200.
- **Uwaga: otwarcie portu resetuje ESP** (sygnały DTR/RTS) – router uruchamia się od nowa, klienci na chwilę tracą połączenie.
- Na porcie są też logi ESP-IDF. Konsola COM działa zawsze (także gdy firewall albo ustawienia odcięły panel i Telnet) – chyba że usługę `serial` zatrzymano.

#### Telnet

- `telnet 192.168.4.1` (port 23), z komputera w sieci AP. Bez hasła – dlatego tylko od strony AP; od strony sieci nadrzędnej po włączeniu `mgmt_from_sta` (`config set global mgmt_from_sta true`).
- Do 2 sesji jednocześnie, rozłączenie po 10 min bezczynności (`telnet.json`). `exit` albo Ctrl+D na pustej linii kończy sesję.

#### Polskie znaki

Domyślnie konsola wysyła polskie znaki jako litery bez ogonków (działa w każdym terminalu). W terminalu z UTF-8 można włączyć znaki z ogonkami: `config set global console_utf8 true` (od razu; trwałe po `save`). Wpisywać można tylko znaki ASCII.

### 2. Edycja linii

- Enter – wykonanie, Backspace – usunięcie znaku, Ctrl+C – porzucenie linii.
- Strzałki **góra/dół** – historia ostatnich 10 poleceń. Linia do 127 znaków.
- Argument ze spacjami w cudzysłowie: `wifi sta "Moja siec" haslo`.
- Polecenia i podpolecenia można **skracać**, jeśli są jednoznaczne: `dh le` = `dhcp leases`, `serv li` = `service list`.
- `help` – lista poleceń; `help <polecenie>` albo `<grupa>` – podpolecenia; `<polecenie> ?` – składnia i opis.
- Język komunikatów: `lang pl` / `lang en` (razem z panelem).

### 3. Polecenia

`<…>` – wymagane, `[…]` – opcjonalne, `a|b` – jedno z. „Trwałe po save” – zmiana działa od razu, ale znika po restarcie bez `save`.

#### Ogólne

| Polecenie | Opis |
|---|---|
| `help [polecenie]` | lista poleceń lub opis polecenia/grupy |
| `status` | wersja firmware, stan STA/AP, NAT, klienci, czas pracy, pamięć |
| `version` | wersja firmware, data kompilacji, wersja ESP-IDF |
| `clients` | lista klientów AP (MAC, IP, nazwa, sygnał) |
| `lang [pl\|en]` | język panelu i konsoli (bez argumentu – bieżący); trwałe po save |
| `save [moduł]` | zapisuje zmienioną konfigurację (wszystkich modułów albo jednego) |
| `reboot` | restart urządzenia |
| `exit` | koniec sesji Telnet |

#### Konfiguracja – `config`

| Polecenie | Opis |
|---|---|
| `config show [moduł]` | bieżąca konfiguracja ze źródłem i zakresem każdej wartości |
| `config get <moduł> <pole>` | wartość jednego pola (`pole.podpole`) |
| `config set <moduł> <pole> <wartość>` | zmiana pola (trwałe po save) |
| `config unset <moduł> <pole>` | przywrócenie wartości domyślnej pola |
| `config diff [moduł]` | pola zmienione, a niezapisane |
| `config revert [moduł]` | porzuca niezapisane zmiany (wczytuje z pliku) |
| `config fs` | stan LittleFS i lista plików w `/config` |
| `config cat <plik>` | zawartość pliku z `/config` |
| `config factory-reset yes` | usuwa pliki konfiguracji i restartuje |

Moduły: `global`, `wifi`, `nat`, `dhcp`, `dns`, `portfwd`, `firewall`, `ddns`, `telnet`, `web`. Pola: [`configuration.md`](configuration.md#polski).

#### Wi-Fi – `wifi`

| Polecenie | Opis |
|---|---|
| `wifi sta "<ssid>" [hasło]` | sieć nadrzędna, łączy od razu; `""` = nie łącz (trwałe po save) |
| `wifi ap "<ssid>" [hasło]` | własna sieć; bez hasła = otwarta, hasło min. 8 znaków; rozłącza klientów (trwałe po save) |
| `wifi scan` | sieci Wi-Fi w zasięgu |

#### NAT – `nat`

| Polecenie | Opis |
|---|---|
| `nat on` / `nat off` | włącza / wyłącza udostępnianie internetu (trwałe po save) |

#### DHCP – `dhcp`

| Polecenie | Opis |
|---|---|
| `dhcp leases` | aktywne dzierżawy |
| `dhcp reservations` | lista rezerwacji |
| `dhcp reserve <mac> [ip] [nazwa]` | rezerwacja; bez ip = bieżący adres klienta (trwałe po save) |
| `dhcp unreserve <mac\|ip\|nr>` | usuwa rezerwację (trwałe po save) |
| `dhcp release <mac\|ip>` | usuwa dzierżawę (klient dostanie adres na nowo) |

#### DNS – `dns`, `dns block`

| Polecenie | Opis |
|---|---|
| `dns list` | nazwy lokalne ze źródłem |
| `dns add <nazwa> <ip>` | wpis statyczny (trwałe po save) |
| `dns del <nazwa>` | usuwa wpis statyczny (trwałe po save) |
| `dns upstream [auto \| <ip> [ip2]]` | DNS nadrzędny: podgląd albo zmiana (trwałe po save) |
| `dns lookup <nazwa>` | skąd przyjdzie odpowiedź na nazwę |
| `dns cache [clear]` | zajętość cache albo wyczyszczenie |
| `dns stats` | statystyki zapytań |
| `dns block add <domena>` | blokuje domenę i poddomeny (trwałe po save) |
| `dns block del <domena>` | usuwa domenę z listy (trwałe po save) |
| `dns block list [fraza]` | lista (max 50 pozycji) |
| `dns block check <nazwa>` | czy nazwa jest blokowana i przez który wpis |
| `dns block on` / `off` | włącza / wyłącza blokowanie (trwałe po save) |
| `dns block reload` | wczytuje listę z pliku (porzuca niezapisane zmiany) |
| `dns block stats` | liczba blokad i najczęściej blokowane domeny |

#### Przekierowanie portów – `pf`

| Polecenie | Opis |
|---|---|
| `pf list` | reguły i ich stan |
| `pf add <tcp\|udp\|both> <ext_port> <ip\|nazwa_rez> [int_port] [nazwa] [src=IP[/maska],...]` | dodaje regułę (trwałe po save); `src` – kto z zewnątrz może korzystać (max 4) |
| `pf del <nr\|nazwa>` | usuwa regułę (trwałe po save) |
| `pf enable` / `disable <nr\|nazwa>` | włącza / wyłącza regułę |
| `pf src <nr\|nazwa> <IP[/maska],...\|->` | dozwolone źródła reguły (`-` = wszyscy) |
| `pf apply` | zakłada reguły od nowa (np. po zmianie rezerwacji DHCP) |

#### Firewall – `fw`

| Polecenie | Opis |
|---|---|
| `fw` | stan, ping z WAN, liczniki odrzuceń, reguły z licznikami trafień |
| `fw add <nazwa> <tcp\|udp\|both\|icmp\|all> [porty] [client=…] [dest=internet\|router\|any] [dest_ip=any\|IP\|IP/maska] [action=block\|allow] [pos=N]` | dodaje regułę (na końcu albo na pozycji N od 1) |
| `fw del <nazwa\|nr>` | usuwa regułę |
| `fw enable` / `disable <nazwa\|nr>` | włącza / wyłącza regułę |
| `fw move <nazwa\|nr> <pozycja>` | zmienia kolejność (decyduje pierwsza pasująca) |
| `fw reset-counters` | zeruje liczniki |
| `fw ?` | pełna składnia |

`porty`: `25`, `6881-6889`, `25,465,587` (max 8); `client`: `all`, IP, IP/maska, MAC albo nazwa rezerwacji. Usługa: `service start|stop firewall`.

#### DDNS – `ddns`

| Polecenie | Opis |
|---|---|
| `ddns` | stan: usługa, źródło adresu, publiczny IP, wynik każdego konta |
| `ddns update [1-3]` | aktualizacja teraz (wszystkich albo jednego konta) |
| `ddns test` | tylko sprawdzenie publicznego IP |

Konta ustawia się przez `config set ddns <n>.<pole> <wartość>`.

#### Usługi – `service`

| Polecenie | Opis |
|---|---|
| `service list` | stan usług i autostart |
| `service start <nazwa>` | uruchamia usługę |
| `service stop <nazwa> [yes]` | zatrzymuje usługę (`yes`, gdy to ostatnia działająca konsola) |
| `service restart <nazwa>` | restart usługi z bieżącą konfiguracją |

Usługi: `serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`. Autostart: `config set global services.<nazwa> true|false` + `save`.

#### Telnet i panel – `telnet`, `web`

| Polecenie | Opis |
|---|---|
| `telnet sessions` | lista sesji Telnet |
| `telnet kick <nr>` | rozłącza sesję |
| `web passwd <nowe>` | zmiana hasła panelu (8–64 znaki, zapisane od razu) |
| `web passwd reset` | hasło fabryczne z `Secrets.h` |
| `web sessions` | zalogowane sesje panelu |
| `web logout-all` | wylogowuje wszystkie sesje |

#### System – `sys`

| Polecenie | Opis |
|---|---|
| `sys ota` | partycje firmware: uruchomiona, do aktualizacji, wersje |
| `sys mem` | pamięć SRAM/PSRAM, flash, lwIP, stosy zadań |
| `sys log [bajty]` | ostatnie logi z bufora (domyślnie 2048 B) |

### 4. Przykłady

**Połączenie z siecią nadrzędną**
```
wifi scan
wifi sta "DomoweWiFi" tajnehaslo
status
save
```

**Stały adres dla drukarki** (MAC z `dhcp leases` albo `clients`)
```
dhcp reserve aa:bb:cc:11:22:33 192.168.4.50 drukarka
save
```
Drukarka dostanie adres przy odnowieniu dzierżawy (od razu: `dhcp release aa:bb:cc:11:22:33`); w DNS jako `drukarka.lan`.

**Przekierowanie SSH do Raspberry Pi tylko z biura**
```
dhcp reserve dc:a6:32:00:00:01 192.168.4.10 pi
pf add tcp 2222 pi 22 ssh-pi src=203.0.113.7
pf list
save
```
Z sieci nadrzędnej: `ssh -p 2222 <adres STA routera>`. Z internetu – także przekierowanie portu 2222 na routerze nadrzędnym na adres STA ESP.

**Kamera bez dostępu do internetu**
```
dhcp reserve 11:22:33:44:55:66 192.168.4.60 kamera
fw add kamera all client=kamera
service start firewall
config set global services.firewall true
save
```

**Konto DuckDNS**
```
config set ddns 1.provider duckdns
config set ddns 1.hostname mojrouter
config set ddns 1.secret TOKEN-Z-DUCKDNS
config set ddns 1.enabled true
service start ddns
config set global services.ddns true
save
ddns
```

**Podgląd i cofnięcie zmian**
```
config diff
config revert dhcp
```
