# Plan — małe kroki

Każdy krok kończy się działającym stanem, testami i krótkim przeglądem
właściciela przed następnym. „Tutaj” = testy bez sprzętu (symulowany CAN).
„Stanowisko” = weryfikacja na CM5 z prawdziwym SAC.

## Krok 0 — Szkielet ✅

- Nowe repo, import Core V2 / Bench / profili / Linux z V2 `c03a682` bez zmian.
- Build CMake, 29 testów regresyjnych przechodzi.
- Dokumenty: architektura, zasady, pochodzenie kodu, ten plan.

## Krok 1 — Właściciel CAN ✅ (do weryfikacji na stanowisku)

- `CanOwner` (przenośny): link nigdy nie jest ruszany bez blokady; przejęcie
  zawsze wymusza DOWN; każda zmiana jest weryfikowana odczytem; każda porażka
  kończy się DOWN albo stanem `fault`, który blokuje konfigurację.
- Linux: blokada `flock` (zwalniana przez jądro nawet po SIGKILL), sterowanie
  łączem przez `ip` z poleceniami sprawdzonymi w V2, bez powłoki, z limitem
  czasu i pustym środowiskiem.
- Program `ecu_bench_runtime`: przejęcie, opcjonalne utrzymanie konfiguracji,
  korekta obcych zmian co 1 s, sprzątanie przy SIGTERM/SIGINT/SIGHUP.
- Unit systemd z `ExecStopPost` i ograniczeniami (`deploy/systemd/`).
- Tutaj: 31 testów jednostkowych + test end-to-end programu (blokada, SIGTERM,
  SIGKILL, złe argumenty). Testy sprawdzone mutacyjnie.
- Stanowisko: `docs/BENCH_CHECKS.md`, sekcja „Krok 1”.

## Krok 2 — Sesja i wykrywanie prędkości

- Symulowany sterownik CAN do testów (magistrala w pamięci, model błędnej
  prędkości, brak ACK w listen-only) — przeniesiony z kroku 1.
- Session Manager: `Idle → Wykrywanie → Identyfikacja → Połączony → Utracony`.
- Wykrywanie prędkości nasłuchem (FEAE i inne ramki SAC), dopiero potem UDS
  F190/F188/F192 na wykrytej prędkości. Ostatnia znana prędkość próbowana
  pierwsza. Nasłuch możliwie krótki: po pierwszej poprawnej ramce przejście
  w tryb normal (ACK) — uwzględniając wynik punktu 3 i 6 z kroku 1.
- Tutaj: SAC 250k i 500k w symulacji, brak nadawania na złej prędkości.
- Stanowisko: identyfikacja SAC, `candump` bez ramek błędów podczas łączenia.

## Krok 3 — Pomiary na żywo

- Ciągły odbiór FEAE, cykliczny FE96 w runtime, migawka z wiekiem i sekwencją.
- Wartości 0xFB–0xFF jako „niedostępne”, nigdy jako ciśnienie.
- Tutaj: FEAE co 1 s → ciśnienie dostępne w każdej migawce po pierwszej ramce.
- Stanowisko: porównanie z `candump`, stabilność przez 30 min.

## Krok 4 — API

- HTTP: komendy (`connect`, `disconnect`, `read_dtc`, …) z polityką operacji;
  SSE: strumień migawek. Kontrola Origin/Host, tylko loopback.
- Tutaj: testy kontraktu API i odrzucania żądań z obcego Origin.

## Krok 5 — GUI

- Przeniesienie wyglądu WebGUI z V2; logika ograniczona do wyświetlania
  migawki i wysyłania komend.
- Tutaj: testy przeglądarkowe na symulowanym runtime.

## Krok 6 — Odczyt DTC

- Odczyt w ramach sesji, z opisami (katalog z V1), powiązany z tożsamością.

## Krok 7 — Kasowanie DTC za jawnym potwierdzeniem

- Dwuetapowe potwierdzenie, świeży odczyt przed, **obowiązkowy odczyt kontrolny
  po**, zapis dowodu. Najpierw tylko wariant 250k (jedyny z historią kasowania).
- Stanowisko: osobno zatwierdzony test fizyczny.

## Krok 8 — Instalator i odtwarzalność

- Jeden instalator, unit systemd z ograniczeniami (zasada 2), aktualizacja
  i rollback całego wydania, test instalacji na czystym CM5.

## Później (po kroku 8, na podstawie rzeczywistych przypadków)

- Kontrakty Power Managera (RS485), aktuatorów (EGR/VGT, wiadomości cykliczne
  z terminem), silnika sekwencji testów i bazy wyników.
- Kolejne profile: MCM, ETC3 (wiedza z V1).
