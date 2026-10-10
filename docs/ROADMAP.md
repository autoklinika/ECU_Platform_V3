# Plan — małe kroki

Każdy krok kończy się działającym stanem, testami i przeglądem właściciela
przed następnym. „Tutaj” = testy bez sprzętu. „Stanowisko” = weryfikacja
na CM5 z prawdziwym sterownikiem (`docs/BENCH_CHECKS.md`).

Wszystko poza Core V2 jest pisane od nowa; V1/V2 służą tylko jako wiedza.

## Krok 0 — Fundament ✅

- Repozytorium, CMake, CI (GCC, Clang, bez warstwy Linux, ASan/UBSan).
- Core V2 z V2 bez zmian, z 20 testami regresyjnymi.
- Zasady, decyzje, pochodzenie kodu, ten plan.

## Krok 1 — Właściciel CAN ✅ (do weryfikacji na stanowisku)

- `CanOwner` (przenośny): łącze nigdy nie jest ruszane bez blokady; przejęcie
  zawsze wymusza DOWN; każda zmiana jest weryfikowana odczytem; każda porażka
  kończy się DOWN albo stanem `fault`, który blokuje konfigurację.
- Linux: blokada `flock` (zwalniana przez jądro także po SIGKILL); zmiana
  konfiguracji przez `ip` bez powłoki, z limitem czasu i pustym środowiskiem;
  własne zapytanie rtnetlink o stan łącza (prędkość, tryb, stan błędów,
  liczniki TEC/REC).
- Program `ecu_bench_runtime`: przejęcie, opcjonalne utrzymanie konfiguracji,
  korekta obcych zmian co 1 s, sprzątanie przy SIGTERM/SIGINT/SIGHUP.
- Unit systemd z `ExecStopPost` i ograniczeniami uprawnień.
- Sprawdzenie na stanowisku jednym poleceniem: `tools/bench/krok1.sh`
  (tylko listen-only), testowane w CI na symulowanym stanowisku.

## Krok 2 — Adapter SocketCAN dla Core V2

- Własna implementacja `ICanDriver` z Core V2 na gnieździe CAN_RAW:
  filtry, znaczniki czasu z jądra, ramki błędów, brak echa własnych ramek,
  ograniczona kolejka nadawania.
- Symulowana magistrala do testów: wiele węzłów, błędna prędkość, brak ACK
  w listen-only, utrata ramek.
- Tutaj: kontrakt `ICanDriver` na symulacji; adapter na `vcan`, jeśli dostępny.
- Stanowisko: odbiór ramek SAC w listen-only przez adapter.

## Krok 3 — Wykrywanie prędkości nasłuchem

- Listen-only na kolejnych prędkościach (ostatnia znana pierwsza), decyzja na
  podstawie poprawnych ramek i liczników błędów; po pierwszej poprawnej ramce
  przejście w tryb normal. Bez nadawania na niepotwierdzonej prędkości.
- Stanowisko: SAC 500k (i 250k, jeśli dostępny), `candump` bez ramek błędów.

## Krok 4 — Profil DAF SAC i identyfikacja

- Profil SAC napisany od nowa: adresy, DID, PGN, skale, warianty 250/500.
- Identyfikacja VIN/SW/HW przez UDS z Core V2 na wykrytej prędkości.
- Stanowisko: zgodność VIN/SW/HW z wartościami znanymi z V1/V2.

## Krok 5 — Sesja i pomiary na żywo

- Session Manager: `Idle → Wykrywanie → Identyfikacja → Połączony → Utracony`.
- Ciągły odbiór FEAE, cykliczny FE96, migawka z wiekiem i numerem sekwencji.
- Wartości 0xFB–0xFF jako „niedostępne”, nigdy jako ciśnienie.
- Stanowisko: stabilność 30 min, porównanie z `candump`.

## Krok 6 — API

- HTTP: komendy z polityką operacji; SSE: strumień migawek; kontrola
  Origin/Host; tylko loopback.

## Krok 7 — GUI

- Nowy cienki klient: wyświetla migawkę i wysyła komendy, bez własnej logiki
  świeżości danych ani harmonogramu odczytów.

## Krok 8 — Odczyt DTC

- Odczyt w sesji, powiązany z tożsamością; opisy z katalogu.
- Od tego kroku DTC przed/po testach na stanowisku czyta samo V3.

## Krok 9 — Kasowanie DTC za jawnym potwierdzeniem

- Dwuetapowe potwierdzenie, świeży odczyt przed, **obowiązkowy odczyt
  kontrolny po**, zapis dowodu; początkowo tylko wariant 250k.

## Krok 10 — Instalator i odtwarzalność

- Jeden instalator, aktualizacja i rollback całego wydania, test na czystym CM5.
- Usługi V2 na CM5 przestają być potrzebne — ich wyłączenie to decyzja
  właściciela.

## Później (na podstawie rzeczywistych przypadków)

- Power Manager (RS485), aktuatory EGR/VGT (wiadomości cykliczne z terminem),
  silnik sekwencji testów, baza wyników, kolejne profile (MCM, ETC3).
