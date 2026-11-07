# Esp32Router – documentation / dokumentacja

[English](#english) | [Polski](#polski)

## English

Applies to version 0.2.0.

### 1. What Esp32Router is

Esp32Router is a small Wi-Fi router on an ESP32-S3 board. It connects to an existing network with internet access (e.g. a home router or a phone hotspot) and at the same time runs its own Wi-Fi network. Devices connected to this network get addresses, names and internet access, and the whole router is managed from a browser or from a console.

### 2. How it works

```
  Internet
     │
 upstream router / hotspot  (upstream network, e.g. 192.168.1.0/24)
     │  Wi-Fi
 ┌───┴──────────────────────────────────────────────┐
 │ ESP32-S3                                          │
 │  STA – client of the upstream network (DHCP)      │
 │  NAT – client addresses replaced by the STA one   │
 │  AP  – own network, address 192.168.4.1           │
 │  DHCP, DNS, firewall, web panel, Telnet           │
 └───┬──────────────────────────────────────────────┘
     │  Wi-Fi
 AP clients: phone, laptop, camera…  (192.168.4.100 – .199)
```

- **STA** – the ESP connects to the upstream network like an ordinary phone and gets an address from it.
- **AP** – the ESP runs its own network (by default `Esp32Router-XXXX`, router address `192.168.4.1`).
- **NAT** – traffic of the AP clients goes to the internet with the router's STA address.
- There is one radio: the AP always works on the same channel as the upstream network. Typical throughput is a few to a dozen or so Mb/s.

### 3. Requirements

- An **ESP32-S3** board with 8 MB flash and octal PSRAM (e.g. ESP32-S3-DevKitC-1 N8R16). A different PSRAM size or no PSRAM does not prevent start-up.
- A USB cable for the first upload and for the COM console.
- To build: a computer with PlatformIO (Visual Studio Code or the command line) – [`source.md`](source.md#english).
- Note: octal PSRAM uses GPIO33–37 – do not use these pins for anything else.

### 4. Features

| Feature | Description |
|---|---|
| NAT | Shares the internet with AP clients; can be disabled (Routing → NAT, `nat on/off`). |
| DHCP | Hands out addresses from a pool; reservations – a fixed address for a chosen device (by MAC), list of leases. |
| DNS | Answers local names (`nas.lan`, device names from DHCP, static entries), forwards the rest and keeps a cache. |
| Filtering | Blocks domains from a list (ads, tracking) together with their subdomains; list import in hosts format. |
| Port forwarding | Makes a service of an AP client (e.g. a server, a camera) reachable on a port of the router's STA address; access from outside can be limited. |
| Firewall | Rules for traffic sent by AP clients: blocking ports, protocols, the internet or access to the router – for everyone or chosen devices. |
| Ping from WAN | Whether the router answers ping from the upstream network (no by default). |
| DDNS | Publishes the public IP address under a fixed name (DuckDNS, Dynu, No-IP, FreeDNS, deSEC, custom service), up to 3 accounts. |
| Web panel | All settings, status and diagnostics in a browser, password login – [`panel.md`](panel.md#english). |
| Console | The same commands over USB (COM) and Telnet – [`console.md`](console.md#english). |
| OTA | Firmware update over Wi-Fi – from the panel or from PlatformIO; if the new version does not start, the router returns to the previous one. |
| Logs | Live logs in the panel (Diagnostics) and in the console (`sys log`), adjustable log level. |
| Configuration | Settings in JSON files on the ESP: save, show changes, revert, backup, factory reset – [`configuration.md`](configuration.md#english). |
| Languages | Panel and console in English or Polish (PL / EN switch in the panel, `lang pl|en`). |

### 5. First start

1. **Enter your own defaults.** In the project folder copy `include/Secrets.h.example` as `include/Secrets.h` and set the AP network name and password and the panel password (`SECRET_AP_SSID`, `SECRET_AP_PASS`, `SECRET_WEB_PASS`). The file is not committed to git.
2. **Build and upload over USB**: `pio run -t upload` – details in [`source.md`](source.md#english).
3. **Connect to the AP network** – the name from `Secrets.h` with the end of the MAC address, e.g. `Esp32Router-A1B2`.
4. **Open the panel**: <http://192.168.4.1>, log in with the panel password. Change the password (System → Panel password) – a bar at the top reminds you.
5. **Set the upstream network**: Network → Internet (STA) → Scan → Select → password → Connect. After connecting click **Save** in the bar at the top.
6. AP clients have internet access. Further settings: [`panel.md`](panel.md#english).

The same from the COM console: `wifi sta "NetworkName" password`, then `save` – [`console.md`](console.md#english).

### 6. Other documents

| File | Contents |
|---|---|
| [`panel.md`](panel.md#english) | web panel: login, every page, saving and reverting changes, backup, OTA, logs, language, password |
| [`console.md`](console.md#english) | COM and Telnet console: connection, line editing, all commands, examples |
| [`configuration.md`](configuration.md#english) | configuration files, all fields, default values, saving, factory reset |
| [`troubleshooting.md`](troubleshooting.md#english) | common problems and solutions |
| [`source.md`](source.md#english) | for developers: code structure, building, uploading, extending |
| [`dev/`](dev/) | design documents in Polish (decisions, tests, version history up to 0.16.3; no further changes) |

---

## Polski

Dotyczy wersji 0.2.0.

### 1. Czym jest Esp32Router

Esp32Router to mały router Wi-Fi na płytce ESP32-S3. Łączy się z istniejącą siecią z internetem (np. domowym routerem albo hotspotem w telefonie) i jednocześnie wystawia własną sieć Wi-Fi. Urządzenia podłączone do tej sieci dostają adresy, nazwy i dostęp do internetu, a całym routerem zarządza się z przeglądarki albo z konsoli.

### 2. Jak działa

```
  Internet
     │
 router nadrzędny / hotspot  (sieć nadrzędna, np. 192.168.1.0/24)
     │  Wi-Fi
 ┌───┴──────────────────────────────────────────────┐
 │ ESP32-S3                                          │
 │  STA – klient sieci nadrzędnej (adres z jej DHCP) │
 │  NAT – adresy klientów zamieniane na adres STA    │
 │  AP  – własna sieć, adres 192.168.4.1             │
 │  DHCP, DNS, firewall, panel WWW, Telnet           │
 └───┬──────────────────────────────────────────────┘
     │  Wi-Fi
 klienci AP: telefon, laptop, kamera…  (192.168.4.100 – .199)
```

- **STA** – ESP łączy się z siecią nadrzędną jak zwykły telefon i dostaje od niej adres.
- **AP** – ESP wystawia własną sieć (domyślnie `Esp32Router-XXXX`, adres routera `192.168.4.1`).
- **NAT** – ruch klientów AP wychodzi do internetu z adresem STA routera.
- Radio jest jedno: AP pracuje zawsze na tym samym kanale co sieć nadrzędna. Typowa przepustowość to kilka–kilkanaście Mb/s.

### 3. Wymagania

- Płytka **ESP32-S3** z 8 MB flash i PSRAM octal (np. ESP32-S3-DevKitC-1 N8R16). Inna wielkość PSRAM albo jej brak nie blokuje startu.
- Kabel USB do pierwszego wgrania i do konsoli COM.
- Do kompilacji: komputer z PlatformIO (Visual Studio Code albo wiersz poleceń) – [`source.md`](source.md#polski).
- Uwaga: PSRAM octal zajmuje GPIO33–37 – tych pinów nie używaj do innych celów.

### 4. Funkcje

| Funkcja | Opis |
|---|---|
| NAT | Udostępnia internet klientom AP; można go wyłączyć (Routing → NAT, `nat on/off`). |
| DHCP | Rozdaje adresy z puli; rezerwacje – stały adres dla wybranego urządzenia (po MAC), lista dzierżaw. |
| DNS | Odpowiada nazwami lokalnymi (`nas.lan`, nazwy urządzeń z DHCP, wpisy statyczne), resztę przekazuje dalej i trzyma w cache. |
| Filtrowanie | Blokuje domeny z listy (reklamy, śledzenie) razem z poddomenami; import listy w formacie hosts. |
| Przekierowanie portów | Udostępnia usługę klienta AP (np. serwer, kamerę) pod portem adresu STA routera; można ograniczyć, kto z zewnątrz może korzystać. |
| Firewall | Reguły dla ruchu wysyłanego przez klientów AP: blokada portów, protokołów, internetu albo dostępu do routera – dla wszystkich albo wybranych urządzeń. |
| Ping z WAN | Czy router odpowiada na ping od strony sieci nadrzędnej (domyślnie nie). |
| DDNS | Wpisuje publiczny adres IP pod stałą nazwę (DuckDNS, Dynu, No-IP, FreeDNS, deSEC, własny serwis), do 3 kont. |
| Panel WWW | Wszystkie ustawienia, stan i diagnostyka w przeglądarce, logowanie hasłem – [`panel.md`](panel.md#polski). |
| Konsola | Te same polecenia przez USB (COM) i Telnet – [`console.md`](console.md#polski). |
| OTA | Aktualizacja firmware przez Wi-Fi – z panelu albo z PlatformIO; jeśli nowa wersja nie wystartuje, router wraca do poprzedniej. |
| Logi | Logi na żywo w panelu (Diagnostyka) i w konsoli (`sys log`), poziom logów do ustawienia. |
| Konfiguracja | Ustawienia w plikach JSON w ESP: zapis, podgląd zmian, cofanie, kopia zapasowa, reset fabryczny – [`configuration.md`](configuration.md#polski). |
| Języki | Panel i konsola po polsku albo po angielsku (przełącznik PL / EN w panelu, `lang pl|en`). |

### 5. Pierwsze uruchomienie

1. **Wpisz własne dane domyślne.** W katalogu projektu skopiuj `include/Secrets.h.example` jako `include/Secrets.h` i ustaw w nim nazwę i hasło sieci AP oraz hasło panelu (`SECRET_AP_SSID`, `SECRET_AP_PASS`, `SECRET_WEB_PASS`). Plik nie trafia do gita.
2. **Skompiluj i wgraj przez USB**: `pio run -t upload` – szczegóły w [`source.md`](source.md#polski).
3. **Połącz się z siecią AP** – nazwa z `Secrets.h` z końcówką adresu MAC, np. `Esp32Router-A1B2`.
4. **Otwórz panel**: <http://192.168.4.1>, zaloguj się hasłem panelu. Zmień hasło (System → Hasło panelu) – pasek u góry przypomina o tym.
5. **Ustaw sieć nadrzędną**: Sieć → Internet (STA) → Skanuj → Wybierz → hasło → Połącz. Po połączeniu kliknij **Zapisz** w pasku u góry.
6. Klienci AP mają internet. Kolejne ustawienia: [`panel.md`](panel.md#polski).

To samo z konsoli COM: `wifi sta "NazwaSieci" haslo`, potem `save` – [`console.md`](console.md#polski).

### 6. Pozostałe pliki

| Plik | Zawartość |
|---|---|
| [`panel.md`](panel.md#polski) | panel WWW: logowanie, każda podstrona, zapis i cofanie zmian, kopia, OTA, logi, język, hasło |
| [`console.md`](console.md#polski) | konsola COM i Telnet: połączenie, edycja linii, wszystkie polecenia, przykłady |
| [`configuration.md`](configuration.md#polski) | pliki konfiguracji, wszystkie pola, wartości domyślne, zapis, reset fabryczny |
| [`troubleshooting.md`](troubleshooting.md#polski) | typowe problemy i rozwiązania |
| [`source.md`](source.md#polski) | dla programisty: struktura kodu, kompilacja, wgrywanie, rozbudowa |
| [`dev/`](dev/) | dokumenty projektowe (decyzje, testy, historia wersji do 0.16.3; bez dalszych zmian) |
