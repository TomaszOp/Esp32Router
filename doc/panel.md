# Esp32Router – web panel / panel WWW

[English](#english) | [Polski](#polski)

## English

Applies to version 0.2.0. Panel design (for developers, Polish): [`dev/panel.md`](dev/panel.md). Names below are as shown in the English panel.

### 1. Login

- Address: <http://192.168.4.1> (the router's AP address; after changing `ap.ip` – the new address). HTTP only.
- The panel is available only from the AP network. From the upstream network – after enabling "Management (Telnet, panel) also from the upstream network" (System → General, `mgmt_from_sta`).
- Password: the factory one from `Secrets.h` (`SECRET_WEB_PASS`), then your own (System → Panel password). While the factory password works, a yellow bar "You are using the default password" is shown below the header.
- After 5 wrong passwords, login from that address is blocked for 60 s.
- A session expires after 30 min of inactivity (System → Services → Web panel); up to 4 sessions at a time.
- Forgotten password: `web passwd reset` in the COM console restores the factory password.

### 2. Header, menu, changes bar

- **Header**: router name and version, internet state (STA address or "none"), number of clients, time, language switch **PL | EN** (bold – current), **Log out**. Refreshed every 5 s.
- **Menu** (on a phone under the ☰ button):
  - Status
  - Network: Internet (STA), Wi-Fi (AP), LAN / DHCP, Clients, DDNS
  - Routing: NAT, Firewall, Port forwarding
  - DNS: DNS, Filtering
  - System: General, Services, Configuration, Firmware, Panel password, Diagnostics
- **Unsaved changes bar** (yellow, below the header) – appears after every settings change: **Show differences** (System → Configuration), **Revert** (back to the saved files), **Save** (saves the changes permanently). Without saving, changes disappear after a restart.
- **Settings forms**: next to a field a hint with the allowed values; the **after restart** badge – the change takes effect after a service or device restart; **unsaved** – the field was changed but not saved. The **Apply** button sends only the changed fields and shows the result for each (✓ / ✗ with the reason).

### 3. Pages

#### Status

Cards: **Device** (name, version, uptime, time), **Internet (STA)** (network, state or the reason of no connection, address, gateway, DNS, signal, DDNS), **Wi-Fi (AP)** (network, address, clients, NAT, firewall), **Memory**, **Services** (state of each service, link "Manage services"). Refreshed every 5 s.

#### Network → Internet (STA)

- **Status** of the connection to the upstream network.
- **Upstream network**: SSID, Password (empty = current), **Connect**. The panel waits up to 20 s and shows the result ("Connected to …" or the reason, e.g. wrong password). Empty SSID = disconnect. Then **Save**.
- **Networks in range**: **Scan** – list (SSID, signal, channel, security), **Select** puts the SSID into the form. During the scan AP clients may have a short interruption.
- **Network name**: device name (hostname).

#### Network → Wi-Fi (AP)

- **AP network**: SSID, Password (empty = current, 8–63 characters), **Apply**. The change disconnects all clients, including your computer: connect to the new network, open the panel and click **Save** – otherwise the old network returns after a restart. An open network (without a password) is set in the console: `wifi ap "SSID"`.
- **AP settings**: maximum number of clients, AP IP address, mask (after restart).

#### Network → LAN / DHCP

- **DHCP settings**: pool start and end, lease time, DNS for clients (`auto` = the router).
- **Leases**: name, MAC, IP, remaining time, type (dynamic / reservation); **Reserve** (fixed address for this device), **Release** (the client gets an address again on renewal).
- **Reservations**: list with **Edit** and **Delete**; form: MAC, IP address (empty = the client's current address), name (for DNS and rules). Apply immediately, permanent after Save.

#### Network → Clients

Connected devices: name, MAC, IP, signal, address type; **Reserve address**, **Forward port** (opens Port forwarding with the target filled in); **Refresh**.

#### Network → DDNS

- **Status**: service, address source, public IP and check time, check error, next check; **Check IP**, **Update all now**.
- **Common settings**: address source (public IP / detected by the service / STA address), check URL, check interval, forced update every [h], retry after error.
- **Account 1–3**: Enabled (no by default), service, host name, login, token/password, update URL (for a custom service). Fields and a short instruction change with the service. Below the account – the result of the last update (permanent error in red).
- The ddns service is enabled in System → Services (start and autostart). To reach devices behind the router from the internet you also need port forwarding on the upstream router – [`troubleshooting.md`](troubleshooting.md#english).

#### Routing → NAT

Switch **Internet sharing (NAPT)** (immediately, permanent after Save), state and usage of the connection table (TCP / UDP / ICMP, evicted when the table is full).

#### Routing → Firewall

- **Service state** with the **Start / Stop** button (autostart in System → Services), switch **Log blocked packets**, ping from WAN and drop counters.
- **Rules**: order with the ↑ ↓ arrows (the first match decides; no match = allow), hits, **Edit**, **Enable/Disable**, **Delete**.
- **New rule**: name, client (list: all clients, clients by IP and MAC, reservations), protocol, destination ports (e.g. `25`, `6881-6889`, `25,465,587`), destination (internet / router / any), destination address, action (block / allow). **Templates**: SMTP 25, SMB 445, Force router DNS, Telnet for a client. A warning when a rule would cut everyone off from the router.
- **Recently blocked**: time, rule, client, destination, protocol (when the log is enabled).

#### Routing → Port forwarding

- **Rules**: name, protocol, external port, target, internal port, source, state; **Edit**, **Enable/Disable**, **Delete**; **Re-create rules** (e.g. after a reservation change).
- **New rule**: name, protocol (TCP / UDP / TCP + UDP), external port (on the STA address), target (IP or reservation name – suggestion list), internal port (empty = same as external), source (empty = everyone; up to 4 addresses or subnets).

#### DNS → DNS

**Statistics** of queries, **Cache** (usage, **Clear cache**), **Upstream DNS** (auto or 1–2 fixed servers, **Apply**), **Local names** (list with source; static entries – **Edit**, **Delete**, form name + IP), **Check name** (where the answer will come from), **Settings** (local domain, TTL, names from DHCP, cache size, purge interval).

#### DNS → Filtering

**Domain blocking** (on/off, response for blocked names), summary, **List** with search and pages (**Delete** next to a domain), **Add domain** (also blocks subdomains), **Check domain**, **Most blocked**, **Import and export**: text file (one domain per line or hosts format), append or **replace** the list, **Download current list**. The list applies immediately; on the device it is written after Save (max approx. 900 KB).

#### System → General

Device name, panel and console language, COM/Telnet console in UTF-8, time synchronization (NTP), NTP server, time zone, log level, management from the upstream network, answer ping from WAN.

#### System → Services

Table of services (`serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`): state, details, **Autostart** (from the next start, permanent after Save), **Start / Stop / Restart**. Stopping `web` closes the panel. **Telnet** and **Web panel** settings (port, sessions, inactivity time – after a service restart).

#### System → Configuration

**Unsaved changes** (module, field, saved, current; **Save**, **Revert**), **Backup** (**Download backup** – saved files without the panel password; contains Wi-Fi passwords and DDNS tokens), **Restore** (backup file → **Restore from file**, restart), **Factory reset** (deletes the whole configuration, restart). Description: [`configuration.md`](configuration.md#english).

#### System → Firmware

- **Partitions**: running and for update – version, date, state.
- **Update**: choose `firmware.bin` (from the build: `.pio/build/esp32-s3-devkitc-1/firmware.bin`), **Upload firmware** – progress, image check, restart; the panel reloads itself after start. If the new firmware does not start, the router returns to the previous version. An image of another project is rejected.
- **Restart**: **Restart device**.

#### System → Panel password

Current password, new (8–64 characters), repeat, **Change password**. Other sessions are logged out.

#### System → Diagnostics

- **Live logs**: recent logs and new ones as they come (**Pause**, **Clear view**); up to 3 viewers at a time.
- **Terminal**: console commands as on COM/Telnet ([`console.md`](console.md#english)), up/down arrows – history.
- **Memory**: SRAM, DMA, PSRAM, flash, lwIP, tasks and minimum free stack (**Refresh**).

### 4. Language

The **PL | EN** switch in the header or System → General → Panel and console language. The change applies immediately (the panel reloads), permanent after **Save**. The language also applies to the console. The default language is set before building – [`source.md`](source.md#english), ch. 5.

---

## Polski

Dotyczy wersji 0.2.0. Projekt panelu (dla programisty): [`dev/panel.md`](dev/panel.md).

### 1. Logowanie

- Adres: <http://192.168.4.1> (adres AP routera; po zmianie `ap.ip` – nowy adres). Tylko HTTP.
- Panel jest dostępny tylko z sieci AP. Od strony sieci nadrzędnej – po włączeniu „Zarządzanie także od strony sieci nadrzędnej” (System → Ogólne, `mgmt_from_sta`).
- Hasło: fabryczne z `Secrets.h` (`SECRET_WEB_PASS`), potem własne (System → Hasło panelu). Dopóki działa hasło fabryczne, pod nagłówkiem jest żółty pasek „Używasz domyślnego hasła”.
- Po 5 błędnych hasłach logowanie z tego adresu jest blokowane na 60 s.
- Sesja wygasa po 30 min bezczynności (System → Usługi → Panel WWW); jednocześnie do 4 sesji.
- Zapomniane hasło: w konsoli COM `web passwd reset` przywraca hasło fabryczne.

### 2. Nagłówek, menu, pasek zmian

- **Nagłówek**: nazwa i wersja routera, stan internetu (adres STA albo „brak”), liczba klientów, czas, przełącznik języka **PL | EN** (pogrubiony – bieżący), **Wyloguj**. Odświeżany co 5 s.
- **Menu** (na telefonie pod przyciskiem ☰):
  - Stan
  - Sieć: Internet (STA), Wi-Fi (AP), LAN / DHCP, Klienci, DDNS
  - Routing: NAT, Firewall, Przekierowanie portów
  - DNS: DNS, Filtrowanie
  - System: Ogólne, Usługi, Konfiguracja, Firmware, Hasło panelu, Diagnostyka
- **Pasek niezapisanych zmian** (żółty, pod nagłówkiem) – pojawia się po każdej zmianie ustawień: **Pokaż różnice** (System → Konfiguracja), **Cofnij** (wraca do zapisanych plików), **Zapisz** (zapisuje zmiany na stałe). Bez zapisu zmiany znikają po restarcie.
- **Formularze ustawień**: przy polu podpowiedź z dozwolonymi wartościami; znacznik **po restarcie** – zmiana zadziała po restarcie usługi albo urządzenia; **niezapisane** – pole zmienione, a niezapisane. Przycisk **Zastosuj** wysyła tylko zmienione pola i pokazuje wynik dla każdego (✓ / ✗ z przyczyną).

### 3. Podstrony

#### Stan

Karty: **Urządzenie** (nazwa, wersja, czas pracy, czas), **Internet (STA)** (sieć, stan albo przyczyna braku połączenia, adres, brama, DNS, sygnał, DDNS), **Wi-Fi (AP)** (sieć, adres, klienci, NAT, firewall), **Pamięć**, **Usługi** (stan każdej usługi, link „Zarządzaj usługami”). Odświeżane co 5 s.

#### Sieć → Internet (STA)

- **Stan** połączenia z siecią nadrzędną.
- **Sieć nadrzędna**: SSID, Hasło (puste = dotychczasowe), **Połącz**. Panel czeka do 20 s i pokazuje wynik („Połączono z …” albo przyczyna, np. złe hasło). Puste SSID = rozłącz. Potem **Zapisz**.
- **Sieci w zasięgu**: **Skanuj** – lista (SSID, sygnał, kanał, zabezpieczenia), **Wybierz** wpisuje SSID do formularza. Podczas skanowania klienci AP mogą mieć krótką przerwę.
- **Nazwa w sieci**: nazwa urządzenia (hostname).

#### Sieć → Wi-Fi (AP)

- **Sieć AP**: SSID, Hasło (puste = dotychczasowe, 8–63 znaki), **Zastosuj**. Zmiana rozłącza wszystkich klientów, także Twój komputer: połącz się z nową siecią, otwórz panel i kliknij **Zapisz** – inaczej po restarcie wróci stara sieć. Sieć otwartą (bez hasła) ustawia się w konsoli: `wifi ap "SSID"`.
- **Ustawienia AP**: maksymalna liczba klientów, adres IP AP, maska (po restarcie).

#### Sieć → LAN / DHCP

- **Ustawienia DHCP**: początek i koniec puli, czas dzierżawy, DNS dla klientów (`auto` = router).
- **Dzierżawy**: nazwa, MAC, IP, pozostały czas, typ (dynamiczny / rezerwacja); **Zarezerwuj** (stały adres dla tego urządzenia), **Zwolnij** (klient dostanie adres na nowo przy odnowieniu).
- **Rezerwacje**: lista z **Edytuj** i **Usuń**; formularz: MAC, adres IP (pusty = bieżący adres klienta), nazwa (do DNS i reguł). Działają od razu, trwałe po Zapisz.

#### Sieć → Klienci

Podłączone urządzenia: nazwa, MAC, IP, sygnał, rodzaj adresu; **Zarezerwuj adres**, **Przekieruj port** (otwiera Przekierowanie portów z wpisanym celem); **Odśwież**.

#### Sieć → DDNS

- **Stan**: usługa, źródło adresu, publiczny IP i czas sprawdzenia, błąd sprawdzania, następne sprawdzenie; **Sprawdź IP**, **Aktualizuj wszystkie teraz**.
- **Ustawienia wspólne**: źródło adresu (publiczny IP / wykrywa serwis / adres STA), adres sprawdzania, częstotliwość sprawdzania, wymuszona aktualizacja co [h], ponowienie po błędzie.
- **Konto 1–3**: Włączone (domyślnie nie), serwis, nazwa hosta, login, token/hasło, adres aktualizacji (dla własnego serwisu). Pola i krótka instrukcja zmieniają się z serwisem. Pod kontem – wynik ostatniej aktualizacji (błąd trwały na czerwono).
- Usługę ddns włącza się w System → Usługi (start i autostart). Żeby wejść z internetu do urządzeń za routerem, potrzebne jest też przekierowanie portu na routerze nadrzędnym – [`troubleshooting.md`](troubleshooting.md#polski).

#### Routing → NAT

Przełącznik **Udostępnianie internetu (NAPT)** (od razu, trwały po Zapisz), stan i zajętość tablicy połączeń (TCP / UDP / ICMP, wypchnięte przy pełnej tablicy).

#### Routing → Firewall

- **Stan usługi** z przyciskiem **Uruchom / Zatrzymaj** (autostart w System → Usługi), przełącznik **Zapisuj zablokowane w logu**, ping z WAN i liczniki odrzuceń.
- **Reguły**: kolejność strzałkami ↑ ↓ (decyduje pierwsza pasująca; brak pasującej = przepuść), trafienia, **Edytuj**, **Włącz/Wyłącz**, **Usuń**.
- **Nowa reguła**: nazwa, klient (lista: wszyscy, klienci po IP i MAC, rezerwacje), protokół, porty docelowe (np. `25`, `6881-6889`, `25,465,587`), cel (internet / router / dowolny), adres celu, akcja (blokuj / zezwól). **Szablony**: SMTP 25, SMB 445, Wymuś DNS routera, Telnet dla klienta. Ostrzeżenie, gdy reguła odetnie wszystkim dostęp do routera.
- **Ostatnio zablokowane**: czas, reguła, klient, cel, protokół (gdy log włączony).

#### Routing → Przekierowanie portów

- **Reguły**: nazwa, protokół, port zewnętrzny, cel, port wewnętrzny, źródło, stan; **Edytuj**, **Włącz/Wyłącz**, **Usuń**; **Załóż reguły ponownie** (np. po zmianie rezerwacji).
- **Nowa reguła**: nazwa, protokół (TCP / UDP / TCP + UDP), port zewnętrzny (na adresie STA), cel (IP albo nazwa rezerwacji – lista podpowiedzi), port wewnętrzny (pusty = jak zewnętrzny), źródło (puste = wszyscy; do 4 adresów lub podsieci).

#### DNS → DNS

**Statystyki** zapytań, **Cache** (zajętość, **Wyczyść cache**), **DNS nadrzędny** (auto albo 1–2 stałe serwery, **Zastosuj**), **Nazwy lokalne** (lista ze źródłem; wpisy statyczne – **Edytuj**, **Usuń**, formularz nazwa + IP), **Sprawdź nazwę** (skąd przyjdzie odpowiedź), **Ustawienia** (domena lokalna, TTL, nazwy z DHCP, rozmiar cache, czyszczenie).

#### DNS → Filtrowanie

**Blokowanie domen** (włącz/wyłącz, odpowiedź dla zablokowanych), podsumowanie, **Lista** z wyszukiwaniem i stronami (**Usuń** przy domenie), **Dodaj domenę** (blokuje też poddomeny), **Sprawdź domenę**, **Najczęściej blokowane**, **Import i eksport**: plik tekstowy (jedna domena w linii albo format hosts), dopisanie albo **zastąpienie** listy, **Pobierz obecną listę**. Lista działa od razu, na urządzeniu zapisuje się po Zapisz (max ok. 900 KB).

#### System → Ogólne

Nazwa urządzenia, język panelu i konsoli, konsola COM/Telnet w UTF-8, synchronizacja czasu (NTP), serwer NTP, strefa czasowa, poziom logów, zarządzanie od strony sieci nadrzędnej, odpowiedź na ping z WAN.

#### System → Usługi

Tabela usług (`serial`, `telnet`, `dhcp`, `dns`, `ddns`, `firewall`, `web`): stan, szczegóły, **Autostart** (od następnego uruchomienia, trwały po Zapisz), **Start / Stop / Restart**. Zatrzymanie `web` zamyka panel. Ustawienia **Telnet** i **Panel WWW** (port, sesje, czas bezczynności – po restarcie usługi).

#### System → Konfiguracja

**Niezapisane zmiany** (moduł, pole, zapisane, bieżące; **Zapisz**, **Cofnij**), **Kopia zapasowa** (**Pobierz kopię** – zapisane pliki bez hasła panelu; zawiera hasła Wi-Fi i tokeny DDNS), **Przywracanie** (plik kopii → **Przywróć z pliku**, restart), **Reset fabryczny** (usuwa całą konfigurację, restart). Opis: [`configuration.md`](configuration.md#polski).

#### System → Firmware

- **Partycje**: uruchomiona i do aktualizacji – wersja, data, stan.
- **Aktualizacja**: wybierz `firmware.bin` (z kompilacji: `.pio/build/esp32-s3-devkitc-1/firmware.bin`), **Wgraj firmware** – postęp, sprawdzenie obrazu, restart; panel sam przeładuje się po starcie. Jeśli nowy firmware nie wystartuje, router wróci do poprzedniej wersji. Obraz innego projektu jest odrzucany.
- **Restart**: **Restartuj urządzenie**.

#### System → Hasło panelu

Obecne hasło, nowe (8–64 znaki), powtórzenie, **Zmień hasło**. Pozostałe sesje zostają wylogowane.

#### System → Diagnostyka

- **Logi na żywo**: ostatnie logi i nowe na bieżąco (**Wstrzymaj**, **Wyczyść widok**); do 3 podglądów jednocześnie.
- **Terminal**: polecenia konsoli jak w COM/Telnet ([`console.md`](console.md#polski)), strzałki góra/dół – historia.
- **Pamięć**: SRAM, DMA, PSRAM, flash, lwIP, zadania i minimum wolnego stosu (**Odśwież**).

### 4. Język

Przełącznik **PL | EN** w nagłówku albo System → Ogólne → Język panelu i konsoli. Zmiana działa od razu (panel przeładowuje się), trwała po **Zapisz**. Język dotyczy też konsoli. Domyślny język ustawia się przed kompilacją – [`source.md`](source.md#polski), rozdz. 5.
