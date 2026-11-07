# Esp32Router – panel WWW

Projekt panelu zatwierdzony 6.11.2026. Makieta: https://claude.ai/artifact/Qt2um7F1MtXXJRTpoCuyZn

## 1. Zasady

- Panel ma dawać to samo, co konsola (COM / Telnet): wszystkie ustawienia, akcje i informacje, pogrupowane jak w routerach sprzętowych.
- Czysty HTML/CSS/JavaScript bez frameworków, pliki w katalogu `web/` (od 0.11.1):
  - `index.html` – szkielet: nagłówek, menu, pasek zmian, ekran logowania, miejsce na treść; `style.css`, `app.js` – wspólne (logowanie, menu, routing `#/strona`, nagłówek, formularze, komunikaty),
  - `pages/<nazwa>.html` – treść podstrony (zwykły HTML: nagłówki, formularze, puste tabele) i `pages/<nazwa>.js` – jej logika (dane z API, przyciski); obie pobierane dopiero przy pierwszym wejściu na podstronę, potem z pamięci przeglądarki (ETag, 304),
  - formularze ustawień opisane w HTML: `<form data-config="moduł">` z elementami `<div data-field="pole" data-label="…" data-help="…" data-options="w:etykieta,…">` – `app.js` dobudowuje pola, zakresy, znaczniki „po restarcie”, przycisk i komunikaty,
  - nowa podstrona = para plików + wpis w menu (`MENU` w `app.js`).
- Budżet (6.11.2026): **pierwsze wejście** (szkielet `index.html` + `style.css` + `app.js` + strona Stan) do **12 KB po gzip** – `tools/web_embed.py` wypisuje ten rozmiar przy kompilacji i ostrzega po przekroczeniu; suma wszystkich plików tylko informacyjnie (pozostałe podstrony pobierane dopiero przy wejściu). Po etapie III: pierwsze wejście ok. 9,5 KB, całość 31,4 KB.
- Pliki są pakowane gzipem przy kompilacji (`tools/web_embed.py`) i wkompilowane w firmware; aktualizują się razem z firmware (USB albo OTA).
- Dostęp tylko od strony AP (chyba że `mgmt_from_sta`), po zalogowaniu (hasło, sesja w ciasteczku).
- Interfejs po polsku. Dane z urządzenia (np. SSID) wstawiane jako tekst, nigdy jako HTML.

- **Wersje językowe** (0.15.0, [`doc/dev/jezyki.md`](jezyki.md)): teksty po polsku w HTML i JS; dla `en` słownik `web/i18n/en.json` (klucz = tekst polski) ładowany osobno, poza budżetem pierwszego wejścia. HTML tłumaczony automatycznie po wczytaniu (teksty i atrybuty opisu), w JS `t('…', …)` / `tn('…')`. Przełącznik PL/EN w nagłówku. `web_embed.py` wypisuje brakujące tłumaczenia.

## 2. Dane: odczyt i zmiany

| Co | Jak | Dlaczego |
|---|---|---|
| Odczyt informacji | `GET /api/...` – JSON z modułów | wyjście konsoli to tekst dla człowieka; parsowanie go w JS byłoby kruche |
| Zmiana pól konfiguracji | `POST /api/config/<moduł>` `{ "pole": wartość, ... }` | te same reguły co `config set` (`ConfigStore::setField`): walidacja, zakresy, informacja „od razu / po restarcie” |
| Akcje (rezerwacja DHCP, reguła pf, usługi, blokady, …) i terminal | `POST /api/cmd` `{ "cmd": "..." }` przez `CommandExecutor` | jedna logika i walidacja dla konsoli i panelu; odpowiedź tekstowa jako komunikat |

- Zmiany działają od razu w RAM, trwałe są po zapisie – jak w konsoli. Żółty pasek „Niezapisane zmiany: N” z przyciskami **Pokaż różnice** (strona Konfiguracja), **Cofnij** (`POST /api/revert`), **Zapisz** (`POST /api/save`).
- Formularze konfiguracji budowane z `GET /api/config/<moduł>`: wartość, źródło (plik / domyślne / zmienione), zakres, znacznik „po restarcie” dla pól, które nie działają od razu. Hasła nie są pokazywane (puste pole = bez zmian).
- Wszystkie `POST` wymagają nagłówka `X-Requested-With: esp32router` (ochrona przed CSRF), wszystkie `/api/*` poza logowaniem – ważnej sesji.

## 3. Menu

| Grupa | Strony |
|---|---|
| Stan | przegląd: urządzenie, Internet, Wi-Fi AP, NAT, pamięć, usługi (odświeżanie co 5 s) |
| Sieć | Internet (STA) – sieć nadrzędna, skanowanie, nazwa w sieci · Wi-Fi (AP) · LAN / DHCP – pula, dzierżawy, rezerwacje · Klienci – podłączone urządzenia z akcjami · DDNS – stan, ustawienia, 3 konta (0.13.0, `doc/dev/ddns.md`) |
| Routing | NAT · Firewall – reguły dla klientów AP, szablony, ostatnio zablokowane (0.14.0, `doc/dev/firewall.md`) · Przekierowanie portów (pole Źródło od 0.14.0) |
| DNS | DNS – ustawienia, upstream, wpisy, cache, statystyki, test nazwy · Filtrowanie – lista blokad, wyszukiwanie, import pliku, statystyki |
| System | Ogólne · Usługi (start/stop/restart, autostart, Telnet, panel) · Konfiguracja (różnice, zapisz/cofnij, kopia zapasowa, przywracanie, reset fabryczny) · Firmware (OTA, partycje, restart) · Hasło panelu · Diagnostyka (pamięć, zadania, terminal, logi na żywo) |

Na górze: nazwa i wersja, stan Internetu, liczba klientów, czas, wylogowanie. Na telefonie menu rozwijane przyciskiem.

## 4. API

| Metoda i adres | Opis | Etap |
|---|---|---|
| `POST /api/login` / `POST /api/logout` / `GET /api/session` / `POST /api/passwd` | logowanie, sesja (`language` także bez logowania – 0.15.0), zmiana hasła | 0.9.0 |
| `GET /api/status` | stan urządzenia, STA, AP, NAT, pamięć, usługi, liczba niezapisanych zmian, domyślne hasło | I |
| `GET /api/config/<moduł>` / `POST /api/config/<moduł>` | pola modułu / zmiana pól (wyniki per pole) | I |
| `GET /api/config/diff` | niezapisane zmiany (moduł, pole, zapisane, bieżące) | I |
| `POST /api/save` / `POST /api/revert` | `{}` = wszystkie moduły, `{ "module": "dhcp" }` = jeden | I |
| `POST /api/cmd` | polecenie konsoli (bez `reboot`) | I |
| `GET /api/services` | usługi: stan, autostart, szczegóły | I |
| `GET /api/wifi/scan` | sieci w zasięgu | I |
| `POST /api/wifi/sta` / `POST /api/wifi/ap` | `{ "ssid", "pass"? }` – brak `pass` = hasło bez zmian | I |
| `GET /api/sys/ota` / `POST /api/ota` | partycje i wersje / wgranie firmware (surowy `.bin`, strumieniowo) | I |
| `GET /api/backup` / `POST /api/restore` | kopia zapasowa konfiguracji (jeden JSON) / przywrócenie + restart | I |
| `POST /api/reboot` | restart po wysłaniu odpowiedzi | I |
| `GET /api/clients`, `/api/dhcp/leases`, `/api/dhcp/reservations`, `/api/pf`, `/api/nat`; `POST /api/pf` | dane stron etapu II; dodanie/zmiana reguły przekierowania | II |
| `GET /api/dns/names`, `/api/dns/stats`, `/api/dns/block?q=&offset=&limit=`, `/api/dns/block/export`; `POST /api/dns/block/import?mode=append\|replace` | dane, import (treść pliku, strumieniowo) i eksport listy blokad | III |
| `GET /api/ddns` | stan DDNS (publiczny IP, konta, bez sekretów); także w `/api/status` | 0.13.0 |
| `GET /api/firewall` / `POST /api/firewall` | stan zapory, reguły z licznikami, ostatnio zablokowane / dodanie lub zmiana reguły (JSON); skrót także w `/api/status` | 0.14.0 |
| `GET /api/sys/mem`, `WS /ws/log` | pamięć i zadania; logi na żywo (po połączeniu ostatnie 4 KB, potem nowe dane co 250 ms; max 3 podglądy) | III |

## 5. Nowe funkcje (także w konsoli)

- **Skanowanie Wi-Fi** – `wifi scan` / `GET /api/wifi/scan` (etap I).
- **Kopia zapasowa i przywracanie** – jeden plik JSON z zapisanymi plikami `/config` (bez `web_auth.json`); przywrócenie zapisuje pliki i restartuje (etap I).
- **OTA** – zapis strumieniowy do drugiej partycji, sprawdzenie nagłówka obrazu i nazwy projektu, weryfikacja przy końcu zapisu, przełączenie partycji startowej, restart; rollback, gdy nowy firmware nie potwierdzi startu (etap I).
- **Import listy blokad** – plik w formacie hosts, wczytywany strumieniowo (etap III).
- **Logi na żywo** – `esp_log_set_vprintf` → bufor cykliczny w PSRAM → WebSocket `/ws/log` (etap III).

## 6. Etapy

| Etap | Zakres | Status |
|---|---|---|
| przebudowa (0.11.1) | szkielet + podstrony w `pages/` ładowane przy wejściu, formularze w HTML; funkcje bez zmian | kod gotowy, czeka na test |
| I (0.10.0) | szkielet (menu, pasek zmian, logowanie), Stan, Internet (STA) ze skanowaniem, Wi-Fi (AP), System: Ogólne, Usługi, Konfiguracja (różnice, zapisz/cofnij, kopia, przywracanie, reset), Firmware (OTA), Hasło | kod gotowy, czeka na test |
| II (0.11.0) | LAN / DHCP, Klienci, NAT, Przekierowanie portów; `POST /api/pf` (dodanie/edycja reguły JSON) | kod gotowy, czeka na test |
| III (0.12.0) | DNS, Filtrowanie (z importem i eksportem), Diagnostyka (pamięć, zadania, terminal, logi na żywo) | kod gotowy, czeka na test |
