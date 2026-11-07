# Esp32Router – wersje językowe

Projekt zatwierdzony 7.11.2026. Status: **etap 1 (panel + API) – 0.15.0, etap 2 (konsola) – 0.16.0; kod gotowy, czeka na test na sprzęcie**.

## 1. Zakres

- Języki: polski (`pl`) i angielski (`en`). Jeden język dla panelu i konsoli – `global.json` `language`.
- Nazwy poleceń i parametrów konsoli, pola JSON i wartości w API (np. `tcp`, `block`, `dynamiczny`) zostają bez zmian – to kody, nie teksty.
- Logi (`LOG_x`) nie są tłumaczone (po polsku, bez polskich znaków).
- Wpisywanie polskich znaków w konsoli (`LineEditor`) – bez zmian (tylko ASCII).

## 2. Ustawienie języka

- `global.json` `language`: `pl` / `en`, **działa od razu**, trwałe po `save`. Domyślny język: `DEFAULT_GLOBAL_LANGUAGE` w `include/Defaults.h` (`pl`, nadpisywalny z `build_flags`, np. `-DDEFAULT_GLOBAL_LANGUAGE=\"en\"`; inna wartość niż pl/en zatrzymuje kompilację). Używany przy braku pliku albo pola, przy złej wartości (ostrzeżenie w logu) i po resecie fabrycznym.
- Zmiana:
  - panel: System → Ogólne („Język panelu i konsoli”) albo przełącznik **PL | EN w nagłówku** (bieżący język wyróżniony) (zmienia ustawienie globalne i przeładowuje panel; trwałe po Zapisz – pasek niezapisanych zmian),
  - konsola: `lang` (bieżący język), `lang pl|en` (to samo co `config set global language …`), `config set global language en`.
- `GET /api/session` podaje `language` także bez logowania (ekran logowania w bieżącym języku).

## 3. Firmware – teksty

- `src/i18n/I18n.h`: `tr("po polsku", "in English")` – zwraca tekst w bieżącym języku. Obie wersje są przy miejscu użycia, we flash, bez tabeli i wyszukiwania. Polskie teksty z poprawnymi polskimi znakami (UTF-8).
- Formaty printf: kompilator sprawdza wersję polską (`format_arg`), `tools/i18n_check.py` – zgodność specyfikatorów (`%s`, `%d`…) obu wersji.
- `i18n::Text { pl, en }` – para tekstów przekazywana dalej i zapamiętywana w obu wersjach (podpowiedzi pól konfiguracji: `ConfigReader::str(…, { "1-32 znaki", "1-32 chars" })`; `config show` i formularze panelu pokazują je w bieżącym języku także po zmianie języka).
- Konsola COM i Telnet domyślnie w ASCII: wyjście zamienia polskie znaki na litery bez ogonków (`ą` → `a`, `–` → `-`, `„”` → `"`, inne znaki spoza ASCII → `?`). `global.json` `console_utf8` = true (domyślnie false, `DEFAULT_GLOBAL_CONSOLE_UTF8`, działa od razu) – UTF-8 bez zmian, dla terminali z UTF-8. Wyjście dla panelu (`/api/cmd`, komunikaty API) – zawsze UTF-8. Fragmenty przetłumaczonych tekstów w logach – zawsze ASCII.
- Wszystkie teksty konsoli przez `tr()`: wyniki poleceń, błędy, opisy poleceń (`CommandDef.help` i `addGroup()` – `i18n::Text { "pl", "en" }`), `fw ?`, powitania COM/Telnet. Opisy argumentów w składni (`<modul> <pole>`, `[porty]`) – bez zmian (część składni). Stany zapisane w API jako kody (`aktywna`, `plik`, `poprawny`…) w konsoli tłumaczone przy wypisaniu.
- Kolumny tabel konsoli z tekstem w bieżącym języku – `i18n::pad(tekst, szerokość)` (liczy znaki, nie bajty UTF-8); nagłówki tabel w obu językach mają te same pozycje kolumn.
- `tools/i18n_check.py` (uruchamiany ręcznie): zgodność specyfikatorów printf w parach `tr()` oraz literały z tekstem w `print`/`printf` poza `tr()` (wyjątki: kody w `KODY`, etykiety wspólne w `WSPOLNE`); kod wyjścia 1 przy błędach. Stan 0.16.0: 394 pary, 0 błędów, 0 tekstów bez `tr()`.

## 4. Panel

- Teksty w HTML i JS są po polsku. Dla angielskiego panel pobiera słownik `web/i18n/en.json` (tylko gdy język ≠ pl, osobny plik, **poza budżetem pierwszego wejścia**; ok. 10 KB gzip). **Klucz = tekst polski**, wartość = tłumaczenie; brak klucza → tekst polski.
- HTML: po wczytaniu szkieletu i każdej podstrony `translate()` zamienia teksty (spacje zredukowane do jednej) oraz atrybuty `data-label`, `data-help`, `data-button`, `data-options` (etykiety po `:`), `placeholder`, `title`, `aria-label`. Strony nie wymagają oznaczeń.
- JS: `t('tekst', …)` – tłumaczenie z wartościami `{0}`, `{1}`… (`t('Klienci: {0}', n)`); `tn('tekst')` – tekst tylko oznaczony do słownika, tłumaczony przy wyświetleniu (etykiety menu, mapy wartości z API, np. `{ dynamiczny: tn('dynamiczny') }` → `t(TYPES[x.type])`).
- Komunikaty z firmware (błędy API, walidacja, błędy reguł, stan DDNS, wyjście poleceń `/api/cmd`, opisy usług) przychodzą już przetłumaczone.
- `tools/web_embed.py` przy każdej kompilacji zbiera klucze z HTML i JS i porównuje ze słownikiem: liczba tekstów, **braki**, nieużywane klucze, różne `{n}` w kluczu i tłumaczeniu.

## 5. Etapy

| Etap | Wersja | Zakres | Status |
|---|---|---|---|
| 1 | 0.15.0 | `language`, `lang`, przełącznik w panelu; słownik panelu (503 teksty); komunikaty firmware trafiające do panelu: błędy API (`sendError`, logowanie i blokada), walidacja `ConfigReader`/`config set`, podpowiedzi pól, błędy reguł fw/pf/DHCP/DNS, stan DDNS, OTA, kopia zapasowa; ASCII na COM/Telnet | kod gotowy, test na atrapie |
| 2 | 0.16.0 | pozostałe teksty konsoli (wyniki poleceń, opisy 73 poleceń, `fw ?`) przez `tr()` (razem 394 pary); `global.console_utf8`; `tools/i18n_check.py` wyszukuje literały bez `tr()`; `i18n::pad` i wyrównane nagłówki tabel | kod gotowy, czeka na test na sprzęcie |

## 6. Odstępstwa od zatwierdzonego projektu (etap 1)

- Słownik panelu z kluczem = tekst polski (zamiast kluczy typu `menu.status` i `web/i18n/pl.json`): HTML bez atrybutów `data-i18n`, polski nie potrzebuje słownika (zero narzutu), kod czytelny po polsku, a `web_embed.py` i tak wypisuje braki. Kontekstowe różnice (ten sam tekst, inne tłumaczenie) – w razie potrzeby inny tekst polski.
- Firmware: pary `tr("pl", "en")` przy miejscu użycia zamiast tabeli `src/i18n/Messages.h` z identyfikatorami – bez ręcznego nadawania ok. 600 identyfikatorów, tekst widoczny w kodzie, oba języki we flash jak w tabeli. Trzeci język wymagałby przejścia na tabelę (skrypt może ją wygenerować z par).
- Zamiana polskich znaków na ASCII na COM/Telnet już w etapie 1 (w projekcie: etap 2) – przetłumaczone komunikaty walidacji mają polskie znaki i trafiają też do konsoli.
