# Esp32Router – firewall

Projekt zatwierdzony 6.11.2026. Status: **zrealizowany w wersji 0.14.0, czeka na test na sprzęcie** (`doc/dev/projekt.md`, rozdz. 13).

## 1. Zakres

- Reguły dotyczą tylko ruchu **wysyłanego przez klientów AP**: do internetu, do routera (usługi ESP) albo dowolnie. Reguła nie ma pola kierunku (in/out).
- Ruch z zewnątrz ogranicza NAT: do klientów AP wchodzi tylko to, co jest świadomie przekierowane. Kto z zewnątrz może korzystać z przekierowania, określa pole `src` w regule pf (rozdz. 6).
- Ping z WAN (na adres STA ESP) – ustawienie globalne `wan_ping` (rozdz. 5).
- Tylko IPv4.

## 2. Działanie

- Hak lwIP `LWIP_HOOK_IP4_INPUT` – wywoływany dla każdego odebranego pakietu IPv4 **przed** NAPT i przekierowaniem portów, więc widzi prawdziwe adresy i porty klienta. Podpięty przez `ESP_IDF_LWIP_HOOK_FILENAME` (`include/lwip_hooks.h`, ustawiane dla komponentu lwip w `src/CMakeLists.txt`). Ta wersja ESP-IDF nie ma opcji `CONFIG_LWIP_HOOK_IP4_INPUT_CUSTOM` w menuconfig – to oficjalny sposób na własne haki.
- Kod haka: `src/PacketFilter.cpp`. Działa w wątku tcpip, bez alokacji i blokad. Tabele (reguły zapory, źródła pf) są podmieniane tylko w wątku tcpip (`esp_netif_tcpip_exec`), więc hak zawsze widzi spójne dane.
- Pakiet z interfejsu AP (gdy usługa `firewall` działa): reguły po kolei, **pierwsza pasująca decyduje**, brak pasującej = przepuść. Pakiet z interfejsu STA na adres STA: ping z WAN i źródła pf (zawsze, niezależnie od usługi).
- Odrzucenie = cichy drop (bez odpowiedzi ICMP/RST).
- Dopasowanie po **porcie docelowym** pakietów klienta. Blokuje także połączenia już otwarte (ich kolejne pakiety też trafiają w regułę). Odpowiedzi z internetu nie są sprawdzane – przychodzą od strony STA, a NAT dopasowuje je do połączeń klienta; zablokowanie pakietów klienta wystarcza. Ruch klienta-serwera za przekierowaniem (pf) ma port usługi jako **źródłowy**, więc reguły po porcie docelowym go nie dotykają.
- Liczniki trafień liczą pakiety (nie połączenia); przechodzą przez zmianę reguł.
- Koszt: kilka porównań na regułę, rząd 1–3 µs na pakiet przy 32 regułach; zatrzymana usługa – jedno sprawdzenie flagi.

### Ruch klient ↔ klient (sprawdzone w źródłach)

Pakiet od klienta AP do innego klienta AP, gdyby trafił do lwIP, i tak nie zostałby przekazany dalej: `ip4_forward()` nie odsyła pakietów na interfejs, z którego przyszły (`IP_FORWARD_ALLOW_TX_ON_RX_NETIF = 0`, `lwip/src/core/ipv4/ip4.c`). Skoro klienci AP mogą się nawzajem pingować, ramki między nimi przekazuje sterownik Wi-Fi (warstwa 2), z pominięciem lwIP – hak ich nie widzi. Dlatego **nie ma celu `lan`** (reguła nie mogłaby działać). Do potwierdzenia testem (rozdz. 10).

## 3. Reguła

| Pole | Wartości | Domyślnie |
|---|---|---|
| `name` | 1–24 znaki `a-z A-Z 0-9 - _`, unikalna | wymagane |
| `enabled` | true/false | true |
| `client` | `all` / IP / IP/maska / MAC / nazwa rezerwacji DHCP | `all` |
| `proto` | `tcp` / `udp` / `both` / `icmp` / `all` | `both` |
| `ports` | pusty (wszystkie) / port / zakres / lista do 8 elementów, np. `25,6881-6889`; tylko dla `tcp`/`udp`/`both` | pusty |
| `dest` | `internet` / `router` / `any` | `internet` |
| `dest_ip` | `any` / IP / IP/maska | `any` |
| `action` | `block` / `allow` | `block` |

- `internet` – adres poza siecią AP i różny od adresów ESP (ruch przez STA). `router` – adres AP ESP albo adres STA ESP (panel, Telnet, DNS). `any` – także rozgłoszenia, multicast i adresy sieci AP.
- MAC → IP z dzierżawy DHCP (albo rezerwacji tego MAC), nazwa → IP z rezerwacji; rozwiązywane przy każdej zmianie reguł, dzierżaw i rezerwacji. Nierozwiązana reguła czeka („klient bez dzierzawy DHCP” / „brak rezerwacji DHCP”) i nie działa. Klient, który sam ustawi sobie inny adres, ominie regułę po MAC – pewne przypisanie daje rezerwacja DHCP.
- `allow` służy do wyjątków przed szerszą blokadą (wyżej na liście).
- Do 32 reguł.

### Przykłady

| Cel | Reguła |
|---|---|
| Nikt nie wysyła poczty bezpośrednio | `fw add smtp tcp 25` |
| Blokada SMB do internetu i routera | `fw add smb tcp 445 dest=any` |
| Wymuszenie DNS routera (filtrowanie nie do obejścia) | `fw add wymus-dns both 53,853` |
| Tablet bez gry | `fw add tablet-gra both 2000,6881-6889 client=tablet` |
| Kamera bez internetu | `fw add kamera all client=kamera` |
| Gość bez Telnetu routera | `fw add telnet tcp 23 client=gosc dest=router` |
| Wyjątek: jeden komputer może używać zewnętrznego DNS | `fw add dns-pc both 53 client=192.168.4.10 action=allow pos=1` |

## 4. Usługa i log

- Usługa `firewall` (`service start|stop|restart firewall`), autostart `global.json` `services.firewall`, **domyślnie wyłączony**. Zatrzymana usługa = cały ruch od klientów przepuszczany (reguły zostają w pamięci).
- `log_blocked` (domyślnie true): zablokowane pakiety w logu („Zablokowano [reguła] źródło -> cel proto/port”), **max 1 wpis na sekundę na regułę**. Hak zapisuje zdarzenie do małego bufora (16), timer co 1 s przenosi je do logu i listy „ostatnio zablokowane” (20 ostatnich, w RAM) dla panelu.
- Ostrzeżenie: reguła `block` z `dest=router` dla wszystkich (`client=all`, `dest_ip=any`) odetnie panel i Telnet. Konsola COM działa zawsze; wyjście: `service stop firewall`. Polecenie i panel ostrzegają przy zapisie takiej reguły.

## 5. Ping z WAN – `global.json` `wan_ping`

- `false` (domyślnie): ICMP echo request od strony STA na adres STA ESP jest odrzucany. `true`: ESP odpowiada.
- Ustawienie globalne, niezależne od usługi firewall (działa także, gdy zatrzymana). Działa od razu (`config set global wan_ping true`, panel System → Ogólne), trwałe po `save`.
- Nie dotyczy pingów wysyłanych przez klientów AP do internetu (odpowiedzi to echo reply).
- Zmiana zachowania względem 0.13.0: wcześniej ESP zawsze odpowiadał na ping od strony STA.

## 6. Źródło przekierowania – `src` w regule pf

- Przekierowany port widzi cały internet (boty skanują i próbują haseł). `src` ogranicza, kto może z niego korzystać: do 4 adresów lub podsieci po przecinku, np. `203.0.113.7,198.51.100.0/24`; puste = wszyscy (jak dotąd).
- Sprawdzane w tym samym haku: pakiet od strony STA na adres STA i port zewnętrzny aktywnej reguły pf z innego źródła → odrzucony. Zawsze aktywne, niezależnie od usługi firewall.
- Ma sens dla stałych adresów źródłowych (biuro, serwer); telefon w LTE zmienia adres.
- Działa, gdy router nadrzędny przekazuje oryginalny adres źródłowy (zwykłe przekierowanie portu – tak).
- Konsola: `pf add … src=…`, `pf src <nr|nazwa> <lista|->`; panel: pole „Źródło” i kolumna w tabeli Przekierowanie portów; `portfwd.json`: pole `"src"`.

## 7. Konfiguracja – `/config/firewall.json`

```json
{
  "log_blocked": true,
  "rules": [
    { "name": "smtp", "enabled": true, "client": "all", "proto": "tcp", "ports": "25", "dest": "internet", "dest_ip": "any", "action": "block" }
  ]
}
```

Zmiany działają od razu, trwałe po `save` / `save firewall`; `config diff` / `config revert firewall` jak w innych modułach; kopia zapasowa obejmuje `firewall.json`. Błędna reguła w pliku jest pomijana z ostrzeżeniem (jak rezerwacje DHCP).

## 8. Konsola (COM, Telnet)

| Polecenie | Opis |
|---|---|
| `fw` | stan usługi, log, ping z WAN i liczniki odrzuceń, reguły z licznikami trafień |
| `fw add <nazwa> <tcp\|udp\|both\|icmp\|all> [porty] [client=…] [dest=…] [dest_ip=…] [action=…] [pos=N]` | dodanie reguły (na końcu albo na pozycji N od 1) |
| `fw del <nazwa\|nr>` | usunięcie |
| `fw enable\|disable <nazwa\|nr>` | włączenie / wyłączenie |
| `fw move <nazwa\|nr> <pozycja>` | zmiana kolejności |
| `fw reset-counters` | zerowanie liczników (także odrzuconych pingów i pf) |
| `config set firewall log_blocked false`, `config set global wan_ping true`, `save firewall` | ustawienia |
| `service start\|stop\|restart firewall` | usługa |
| `pf add … src=…`, `pf src <nr\|nazwa> <lista\|->` | źródło przekierowania |

Numery reguł w `fw` liczone od 1 (kolejność ma znaczenie).

## 9. Panel WWW

- Routing → **Firewall** (w menu obok NAT): stan usługi z przyciskiem Uruchom/Zatrzymaj, przełącznik logu, ping z WAN i liczniki odrzuceń; tabela reguł (kolejność strzałkami, trafienia, Edytuj / Włącz / Wyłącz / Usuń); formularz z wyborem klienta z listy (klienci po IP i MAC, rezerwacje); szablony: SMTP 25, SMB 445, Wymuś DNS routera (53+853 do internetu), Telnet dla klienta; ostrzeżenie przy regule odcinającej router; ostatnio zablokowane (odświeżanie co 5 s).
- Usługa w System → Usługi; linia „Firewall” na stronie Stan (karta Wi-Fi AP); `wan_ping` w System → Ogólne.
- API: `GET /api/firewall` (stan, reguły z licznikami, ostatnio zablokowane), `POST /api/firewall` (dodanie/zmiana reguły JSON, `index` = zmiana); pozostałe akcje przez `/api/cmd`; skrót w `/api/status` (`firewall`: running, rules, blocked, wan_ping).

## 10. Testy na sprzęcie

W `doc/dev/projekt.md`, rozdz. 13 (krok 11), w tym pomiar przepustowości z firewallem i bez (iperf).
