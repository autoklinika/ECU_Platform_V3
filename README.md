# ECU Platform V3

Stanowisko laboratoryjne do testowania i naprawy pojedynczego sterownika
(TRUCK / AGRI / OHV) na Raspberry Pi CM5.

V3 jest pisane **od nowa**. Jedynym kodem przejętym z poprzednich wersji jest
przenośny, przetestowany rdzeń protokołów **Core V2** (ISO-TP, UDS, J1939,
ISOBUS) — bez zmian, razem z jego testami. Wszystko inne (właściciel sprzętu,
warstwa Linux, sesja DUT, profile sterowników, API, GUI, instalator) powstaje
dla V3. ECU Platform V1 i V2 służą wyłącznie jako źródło wiedzy.

Najważniejsza zmiana względem V2: **jeden trwały Bench Runtime** jest jedynym
właścicielem sprzętu i sesji DUT, a GUI tylko wyświetla jego stan.

- Zasady i decyzje: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)
- Plan małymi krokami: [`docs/ROADMAP.md`](docs/ROADMAP.md)
- Pochodzenie kodu i wiedzy: [`docs/SOURCE.md`](docs/SOURCE.md)
- Sprawdzenia na stanowisku: [`docs/BENCH_CHECKS.md`](docs/BENCH_CHECKS.md)

## Stan

- **Fundament:** repozytorium, CI, zasady, Core V2 z testami.
- **Właściciel CAN:** `ecu_bench_runtime` przejmuje `can0` na wyłączność,
  wymusza stan bezpieczny, koryguje obce zmiany i sprząta przy zatrzymaniu.
  Stan łącza (prędkość, tryb, stan błędów, liczniki) czyta własnym zapytaniem
  rtnetlink.

## Build i testy

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Wymagania: CMake ≥ 3.25, kompilator C++17 (GCC 13 / Clang 18). Warstwa Linux
budowana jest tylko na Linuksie; rdzeń i runtime budują się bez niej.

## Struktura

```
src/core_v2/                 protokoły CAN/ISO-TP/UDS/J1939/ISOBUS (z V2, bez zmian)
src/runtime/                 Bench Runtime (przenośny): właściciel CAN
src/platform/linux/runtime/  Linux: blokada flock, sterowanie łączem, zapytanie rtnetlink
apps/ecu_bench_runtime/      program runtime
tools/bench/                 automatyczne sprawdzenia na stanowisku (CM5)
deploy/systemd/              unit systemd do weryfikacji na stanowisku
tests/                       testy Core V2 i V3
docs/                        architektura, plan, pochodzenie, sprawdzenia
```
