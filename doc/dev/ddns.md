# Esp32Router – klient dynamicznego DNS (DDNS)

Projekt zatwierdzony 6.11.2026. Status: **zrealizowany w wersji 0.13.0, czeka na test na sprzęcie** (`doc/dev/projekt.md`, rozdz. 13).

## 1. Cel i ograniczenia

DDNS wpisuje pod stałą nazwę (np. `mojrouter.duckdns.org`) aktualny publiczny adres IP, żeby dało się dostać do sieci z internetu.

- ESP stoi zwykle za routerem nadrzędnym – jego adres STA (np. 192.168.100.136) jest prywatny. Publicznym adresem jest adres routera nadrzędnego, dlatego domyślnie adres sprawdzany jest przez zewnętrzną usługę (`ip_source: public`).
- Żeby z internetu dostać się do urządzeń za ESP, potrzebne są: przekierowanie portu na routerze nadrzędnym na adres STA ESP oraz reguła w Routing → Przekierowanie portów.
- Przy CGNAT (adres dzielony przez operatora, częste w LTE) DDNS nie pomoże – tego nie da się wykryć z ESP.
- Tylko IPv4.

## 2. Usługa

- `DdnsClient` – usługa „ddns” w `ServiceManager` (`service start|stop|restart ddns`), autostart `global.json` `services.ddns`, **domyślnie wyłączony** (wymaga konfiguracji). Nie jest konsolą.
- Własne zadanie FreeRTOS (stos 8 KB), połączenia HTTPS przez `esp_http_client` z wbudowanym zestawem certyfikatów (`esp_crt_bundle`), bufory mbedTLS w PSRAM (`CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC`).
- Czeka na IP na STA i na czas z NTP (bez czasu nie da się sprawdzić certyfikatów).

## 3. Serwisy

| `provider` | Serwis | Potrzebne pola | Uwagi |
|---|---|---|---|
| `duckdns` | duckdns.org | `hostname` (subdomena, np. `mojrouter`; kilka po przecinku), `secret` (token) | odpowiedź `OK` / `KO` |
| `dynu` | dynu.com | `hostname`, `username`, `secret` (hasło albo hasło IP Update) | dyndns2 |
| `noip` | noip.com | `hostname`, `username`, `secret` (hasło albo klucz DDNS) | dyndns2; darmowy host trzeba potwierdzać co 30 dni (e-mail od No-IP) |
| `freedns` | freedns.afraid.org | `secret` (token z adresu „Dynamic Update”, wersja 2) | `hostname` tylko informacyjnie |
| `desec` | desec.io (dedyn.io) | `hostname` (pełna nazwa), `secret` (token) | dyndns2, logowanie: nazwa + token |
| `custom` | własny | `custom_url` z polami `{host}`, `{ip}`, `{user}`, `{pass}`; `username` włącza logowanie Basic | odpowiedź dyndns2 albo dowolne 2xx = sukces |

Wartości wstawiane do adresu są kodowane (URL encoding). Tokeny nie trafiają do logów (log pokazuje serwis i nazwę hosta).

## 4. Działanie

- **Źródło adresu** (`ip_source`):
  - `public` (domyślnie) – sprawdzenie publicznego IP przez `check_url` (domyślnie `https://api.ipify.org`, odpowiedź = sam adres) co `check_interval_s`; jedno sprawdzenie wspólne dla wszystkich kont. Konto jest aktualizowane tylko przy zmianie adresu, po `force_update_h` (żeby wpis nie wygasł) albo na żądanie.
  - `provider` – serwis sam bierze adres, z którego przyszło zapytanie; aktualizacja co `check_interval_s` (więcej zapytań; No-IP i dyndns2 mogą częste aktualizacje bez zmiany uznać za nadużycie – lepiej ustawić dłuższy interwał).
  - `sta` – adres interfejsu STA (tylko gdy ESP ma publiczny adres bezpośrednio, np. modem w trybie bridge).
- **Awaria sprawdzania**: 3 nieudane sprawdzenia z rzędu → aktualizacje w trybie `provider`, najwyżej raz na godzinę, aż sprawdzanie znowu zadziała.
- **Nowy adres STA** (zdarzenie `IP_EVENT_STA_GOT_IP`) budzi usługę i wymusza sprawdzenie.
- Konta aktualizowane po kolei; błąd jednego nie wstrzymuje pozostałych.
- **Odpowiedzi**:
  - sukces: `good`, `nochg` (dyndns2), `OK` (DuckDNS), `Updated` / `No IP change` (FreeDNS),
  - błąd trwały: `badauth`, `nohost`, `notfqdn`, `abuse`, `!donator`, `badagent`, `numhost`, `!yours`, HTTP 401/403, `KO` (DuckDNS), `ERROR` (FreeDNS) – konto czeka na zmianę konfiguracji albo `ddns update` (żeby serwis nie zablokował konta),
  - błąd chwilowy: `911`, `dnserr`, HTTP 5xx, brak połączenia – ponowienie po `retry_s`, kolejne odstępy podwajane do 30 min.
- Stan (publiczny IP, wynik i czas ostatniej aktualizacji każdego konta) tylko w RAM.

## 5. Konfiguracja – `/config/ddns.json`

```json
{
  "ip_source": "public",
  "check_url": "https://api.ipify.org",
  "check_interval_s": 300,
  "force_update_h": 24,
  "retry_s": 300,
  "1": { "enabled": true, "provider": "duckdns", "hostname": "mojrouter", "username": "", "secret": "…", "custom_url": "" },
  "2": { … },
  "3": { … }
}
```

| Pole | Typ / zakres | Domyślnie |
|---|---|---|
| `ip_source` | `public` / `provider` / `sta` | `public` |
| `check_url` | `http(s)://…`, max 128 | `https://api.ipify.org` |
| `check_interval_s` | 300–21600 (5 min – 6 h) | 300 |
| `force_update_h` | 0–720 (0 = tylko przy zmianie IP) | 24 |
| `retry_s` | 60–3600 | 300 |
| `<n>.enabled` | true/false | false (od 0.16.3; wcześniej true) |
| `<n>.provider` | `duckdns` / `dynu` / `noip` / `freedns` / `desec` / `custom` | `duckdns` |
| `<n>.hostname` | max 128, nazwy po przecinku | puste |
| `<n>.username` | max 64 | puste |
| `<n>.secret` | max 128 – maskowany w `config show`, różnicach i panelu (tylko „ustawione / puste”) | puste |
| `<n>.custom_url` | `http(s)://…`, max 256 | puste |

- Konta to stałe sloty `1`, `2`, `3`; puste konto (bez nazwy i tokenu) jest pomijane, niekompletne pokazuje przyczynę („brak tokenu/hasla”, „brak username”…).
- Wszystkie zmiany działają od razu (zmiana pola konta zeruje jego stan, także błąd trwały); trwałe po `save` / `save ddns`; `config diff` / `config revert ddns` jak w innych modułach.
- Kopia zapasowa (`/api/backup`) zawiera `ddns.json` z tokenami (decyzja 6.11.2026) – panel przy pobieraniu kopii ostrzega, że plik zawiera hasła Wi-Fi i tokeny.

## 6. Konsola (COM, Telnet)

| Polecenie | Opis |
|---|---|
| `ddns` | stan: usługa, źródło adresu, publiczny IP (i błąd sprawdzania), każde konto: serwis, nazwa, wynik, ostatnia udana aktualizacja |
| `ddns update [1-3]` | aktualizacja teraz (wszystkich albo jednego konta), także po błędzie trwałym; czeka na wynik do 25 s |
| `ddns test` | tylko sprawdzenie publicznego IP |
| `config set ddns.2.provider noip`, `config set ddns 2.secret TOKEN`, `config show ddns`, `save ddns` | konfiguracja (zapis `moduł.pole` działa też dla innych modułów) |
| `service start|stop|restart ddns` | usługa |

## 7. Panel WWW

Sieć → **DDNS** (`web/pages/ddns.html` + `ddns.js`):
- karta stanu (usługa, źródło adresu, publiczny IP i czas sprawdzenia, błąd sprawdzania, następne sprawdzenie; odświeżanie co 5 s), przyciski **Sprawdź IP** (`ddns test`) i **Aktualizuj wszystkie teraz** (`ddns update`),
- ustawienia wspólne (interwał jako lista: 5, 10, 15, 30 min, 1, 2, 6 h),
- trzy karty kont: pola zależne od wybranego serwisu, krótka instrukcja serwisu, wynik ostatniej aktualizacji (błąd trwały na czerwono),
- podpowiedzi: przekierowanie portów na routerze nadrzędnym, CGNAT, potwierdzanie hosta No-IP co 30 dni.
- Usługa w System → Usługi (start/stop, autostart); na stronie Stan – linia „DDNS” w karcie Internet.
- API: `GET /api/ddns` (stan bez sekretów); konfiguracja przez `/api/config/ddns`, akcje przez `/api/cmd`.

## 8. Koszt

Flash: ok. +186 KB (klient HTTPS, mbedTLS, pełny zestaw certyfikatów – można zmniejszyć zestawem „common”); aplikacja 1,31 MB z 3 MB. RAM statyczny +1,2 KB; podczas połączenia TLS ok. 40 KB w PSRAM.
