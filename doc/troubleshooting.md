# Esp32Router – troubleshooting / typowe problemy

[English](#english) | [Polski](#polski)

## English

Applies to version 0.2.0. The most useful: `status`, `service list` and the logs (COM console or panel → System → Diagnostics → Live logs).

### The AP network is not visible

- The router did not start correctly: connect USB and open the COM console (`pio device monitor`; opening the port resets the ESP) – the start-up log shows the AP name and any errors.
- The network name ends with a part of the MAC address: `Esp32Router-A1B2` (base name from `Secrets.h`).
- An AP password shorter than 8 characters is rejected – the AP starts as an **open** network (warning in the log).
- After changing the AP network without **Save**, the previous name and password return after a restart.
- The AP works on the channel of the upstream network. Older devices may not see channels 12–13.
- If everything else fails: `config factory-reset yes` from the COM console – the AP returns to the settings from `Secrets.h`.

### A client is connected but has no internet

1. `status` – is the STA connected and does it have an address. "not connected" with a reason (wrong password, network not found) – fix it with `wifi sta "SSID" password` or Network → Internet (STA).
2. Is NAT enabled: `status` (`NAT: enabled`), panel Routing → NAT. NAT is enabled only after the STA connects.
3. Did the client get an address from the pool (192.168.4.100–.199): `dhcp leases`. If not – `service list` (is the `dhcp` service running?), the pool in Network → LAN / DHCP.
4. DNS: `dns lookup google.com` – is there an upstream DNS (`dns upstream`); is the `dns` service running? A client with a manually set DNS bypasses the router.
5. Firewall: `fw` – does a rule block the client (the hit counter grows). Quick test: `service stop firewall`.
6. Filtering: one site does not work, others do – `dns block check <domain>`.

### The web panel does not load

- Open <http://192.168.4.1> (http, not https) from a device in the AP network. From the upstream network the panel works only with `mgmt_from_sta = true`.
- A phone with mobile data enabled may send traffic over LTE when the AP network "has no internet" – disable mobile data or confirm staying on the network.
- `service list` – is the `web` service running; `service start web`. Autostart: `config set global services.web true` + `save`.
- The message "Nothing matches the given URI" – firmware 0.14.0–0.16.0 (bug fixed in 0.16.1); upload newer firmware over USB or OTA from PlatformIO.
- Panel port changed (`web.json` `port`) – address `http://192.168.4.1:<port>`.
- The page looks like an old version – reload bypassing the cache (Ctrl+F5).

### Forgotten panel password

- COM console: `web passwd reset` – the factory password from `Secrets.h` returns (immediately, all sessions logged out). Then set a new one in the panel.
- Or `web passwd <new>` – a new password immediately (8–64 characters).
- After 5 failed attempts login is blocked for 60 s – wait.

### The firewall cut off the panel or Telnet

A `block` rule with destination "router" for all clients also blocks the panel and Telnet. The COM console always works:
```
service stop firewall
fw
fw del <name>
service start firewall
save
```
If the firewall autostart is enabled and the rule was saved, the problem returns after a restart until the rule is deleted and saved.

### OTA update does not work

- The computer must be in the AP network (or `mgmt_from_sta = true`), and the panel must respond.
- "Haslo z … nie pasuje" (password does not match) – after changing the password in the panel set the `ESP32ROUTER_WEB_PASS` variable (the password in `Secrets.h` is only the factory one).
- "Logowanie zablokowane po 5 blednych probach" (login blocked after 5 attempts) – wait 60 s.
- "Obraz odrzucony" (image rejected) – the file is not ESP32 firmware or comes from another project.
- **Rollback**: after the restart the device has the previous version – the new firmware did not start correctly and the bootloader returned to the previous one. Check the start-up log on COM; `sys ota` shows the partition state ("rejected").
- No response after 60 s – connect to the AP network again and check the version in the panel (Status).
- When in doubt: uploading over USB (`pio run -t upload`) always works.

### Port forwarding does not work from the internet

- Forwarding works on the router's **STA address**, i.e. its address in the upstream network. From the internet you also need to forward the same port on the **upstream router** to the ESP STA address (double NAT). Test from a computer in the upstream network first.
- `pf list` – rule state: "waiting for STA IP", "NAT disabled", "no DHCP reservation with this name".
- The **Source** field limits who may use it – a connection from another address is dropped (counter in `fw`). A phone on LTE changes its address.
- **CGNAT** (the operator gives a private address, common on LTE and some fibre networks): connections from the internet will not arrive at all – you need a public address from the operator or a VPN/tunnel.

### DDNS does not update

- The `ddns` service must run (`service start ddns`, autostart `services.ddns`), the account must be **Enabled** (disabled by default) and complete (`ddns` shows "incomplete: …").
- "permanent error" (e.g. `badauth`, `nohost`, `KO`) – wrong token, login or name; the account waits for a fix or `ddns update`.
- `ddns test` – is the public IP read. With CGNAT the public address belongs to the operator – DDNS will not help.
- No-IP: a free host must be confirmed every 30 days.

### Ping from the upstream network gets no answer

By default the router does not answer ping to its STA address (`wan_ping = false`). Enable it in System → General or `config set global wan_ping true`.

### Garbled characters in the console

- By default the console sends Polish letters without diacritics. If you see characters like `Ä…` – `console_utf8` is enabled in a terminal without UTF-8: `config set global console_utf8 false`.
- Console in Polish and you want English (or the other way round): `lang en` / `lang pl`.

---

## Polski

Dotyczy wersji 0.2.0. Najwięcej mówią: `status`, `service list` i logi (konsola COM albo panel → System → Diagnostyka → Logi na żywo).

### Nie widać sieci AP

- Router nie uruchomił się poprawnie: podłącz USB i otwórz konsolę COM (`pio device monitor`; otwarcie portu resetuje ESP) – log startowy pokazuje nazwę AP i ewentualne błędy.
- Nazwa sieci ma końcówkę z adresu MAC: `Esp32Router-A1B2` (nazwa bazowa z `Secrets.h`).
- Hasło AP krótsze niż 8 znaków jest odrzucane – AP startuje jako sieć **otwarta** (ostrzeżenie w logu).
- Po zmianie sieci AP bez **Zapisz** po restarcie wraca poprzednia nazwa i hasło.
- AP pracuje na kanale sieci nadrzędnej. Starsze urządzenia mogą nie widzieć kanałów 12–13.
- Wszystko inne zawiodło: `config factory-reset yes` z konsoli COM – AP wraca do ustawień z `Secrets.h`.

### Klient jest połączony, ale nie ma internetu

1. `status` – czy STA jest połączony i ma adres. „brak połączenia” z przyczyną (złe hasło, nie znaleziono sieci) – popraw `wifi sta "SSID" haslo` albo Sieć → Internet (STA).
2. Czy NAT jest włączony: `status` (`NAT: włączony`), panel Routing → NAT. NAT włącza się dopiero po połączeniu STA.
3. Czy klient dostał adres z puli (192.168.4.100–.199): `dhcp leases`. Jeśli nie – `service list` (usługa `dhcp` działa?), pula w Sieć → LAN / DHCP.
4. DNS: `dns lookup google.com` – czy jest DNS nadrzędny (`dns upstream`); usługa `dns` działa? Klient z ręcznie ustawionym DNS omija router.
5. Firewall: `fw` – czy jakaś reguła nie blokuje klienta (licznik trafień rośnie). Szybki test: `service stop firewall`.
6. Filtrowanie: strona nie działa, inne tak – `dns block check <domena>`.

### Panel WWW się nie ładuje

- Otwórz <http://192.168.4.1> (http, nie https) z urządzenia w sieci AP. Od strony sieci nadrzędnej panel działa tylko z `mgmt_from_sta = true`.
- Telefon z włączonymi danymi komórkowymi może wysyłać ruch przez LTE, gdy sieć AP „nie ma internetu” – wyłącz dane albo potwierdź pozostanie w sieci.
- `service list` – czy usługa `web` działa; `service start web`. Autostart: `config set global services.web true` + `save`.
- Komunikat „Nothing matches the given URI” – firmware 0.14.0–0.16.0 (błąd poprawiony w 0.16.1); wgraj nowszy firmware przez USB albo OTA z PlatformIO.
- Port panelu zmieniony (`web.json` `port`) – adres `http://192.168.4.1:<port>`.
- Strona wygląda na starą wersję – odśwież z pominięciem pamięci podręcznej (Ctrl+F5).

### Zapomniane hasło panelu

- Konsola COM: `web passwd reset` – wraca hasło fabryczne z `Secrets.h` (od razu, wszystkie sesje wylogowane). Potem ustaw nowe w panelu.
- Albo `web passwd <nowe>` – nowe hasło od razu (8–64 znaki).
- Po 5 błędnych próbach logowanie jest zablokowane na 60 s – odczekaj.

### Firewall odciął panel albo Telnet

Reguła `block` z celem „router” dla wszystkich klientów blokuje także panel i Telnet. Konsola COM działa zawsze:
```
service stop firewall
fw
fw del <nazwa>
service start firewall
save
```
Jeśli autostart firewalla jest włączony, a reguła zapisana – po restarcie problem wraca, dopóki reguła nie zostanie usunięta i zapisana.

### Aktualizacja OTA nie działa

- Komputer musi być w sieci AP (albo `mgmt_from_sta = true`), panel musi odpowiadać.
- „Haslo z … nie pasuje” – po zmianie hasła w panelu ustaw zmienną `ESP32ROUTER_WEB_PASS` (hasło z `Secrets.h` jest tylko fabryczne).
- „Logowanie zablokowane po 5 blednych probach” – odczekaj 60 s.
- „Obraz odrzucony” – plik nie jest firmware ESP32 albo pochodzi z innego projektu.
- **Rollback**: po restarcie urządzenie ma poprzednią wersję – nowy firmware nie wystartował poprawnie i bootloader wrócił do poprzedniego. Sprawdź log startowy na COM; `sys ota` pokazuje stan partycji („odrzucony”).
- Brak odpowiedzi po 60 s – połącz się ponownie z siecią AP i sprawdź wersję w panelu (Stan).
- W razie wątpliwości: wgranie przez USB (`pio run -t upload`) działa zawsze.

### Przekierowanie portów nie działa z internetu

- Przekierowanie działa na **adres STA** routera, czyli adres w sieci nadrzędnej. Z internetu trzeba jeszcze przekierować ten sam port na **routerze nadrzędnym** na adres STA ESP (podwójny NAT). Najpierw sprawdź z komputera w sieci nadrzędnej.
- `pf list` – stan reguły: „czeka na IP STA”, „NAT wyłączony”, „brak rezerwacji DHCP o tej nazwie”.
- Pole **Źródło** ogranicza, kto może korzystać – z innego adresu połączenie jest odrzucane (licznik w `fw`). Telefon w LTE zmienia adres.
- **CGNAT** (operator daje adres prywatny, częste w LTE i części sieci światłowodowych): połączenia z internetu nie dotrą wcale – potrzebny publiczny adres od operatora albo VPN/tunel.

### DDNS nie aktualizuje

- Usługa `ddns` musi działać (`service start ddns`, autostart `services.ddns`), konto – **Włączone** (domyślnie wyłączone) i kompletne (`ddns` pokazuje „niekompletne: …”).
- „błąd trwały” (np. `badauth`, `nohost`, `KO`) – zły token, login albo nazwa; konto czeka na poprawkę albo `ddns update`.
- `ddns test` – czy publiczny IP jest odczytywany. Przy CGNAT publiczny adres należy do operatora – DDNS nic nie da.
- No-IP: darmowy host trzeba potwierdzać co 30 dni.

### Ping z sieci nadrzędnej nie odpowiada

Domyślnie router nie odpowiada na ping na adres STA (`wan_ping = false`). Włącz w System → Ogólne albo `config set global wan_ping true`.

### Krzaki w konsoli

- Konsola domyślnie wysyła polskie znaki bez ogonków. Jeśli widać znaki `Ä…` itp. – włączone `console_utf8` w terminalu bez UTF-8: `config set global console_utf8 false`.
- Konsola po angielsku, a chcesz po polsku: `lang pl`.
