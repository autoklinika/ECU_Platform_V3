# ECU Platform V3

Stanowisko laboratoryjne do testowania i naprawy pojedynczego sterownika
(TRUCK / AGRI / OHV) na Raspberry Pi CM5.

V3 **nie jest przepisaniem od zera**. Zachowuje przenośny, przetestowany
rdzeń z ECU Platform V2 (Core V2, Bench, profile DUT) i zmienia to, co w V2
nie pasowało do urządzenia: sposób uruchamiania. Zamiast czterech usług,
pośrednich plików i nowego procesu na każdy odczyt powstaje **jeden trwały
Bench Runtime**, który jest jedynym właścicielem sprzętu i sesji DUT,
a GUI tylko wyświetla jego stan.

Uzasadnienie i zasady: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
Plan prac małymi krokami: [`docs/ROADMAP.md`](docs/ROADMAP.md).
Pochodzenie zaimportowanego kodu: [`docs/SOURCE.md`](docs/SOURCE.md).

## Stan

- Krok 0 — szkielet: zaimportowany rdzeń V2, build, 29 testów regresyjnych.
- Krok 1 — właściciel CAN: `ecu_bench_runtime` przejmuje `can0` na wyłączność,
  wymusza stan bezpieczny, koryguje obce zmiany i sprząta przy zatrzymaniu.
  Weryfikacja na stanowisku: [`docs/BENCH_CHECKS.md`](docs/BENCH_CHECKS.md).

Sesja SAC, pomiary, API i GUI powstają w kolejnych krokach.

## Build i testy

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

Wymagania: CMake ≥ 3.25, kompilator C++17 (GCC 13 / Clang). Warstwa Linux
(SocketCAN) budowana jest automatycznie tylko na Linuksie; warstwy przenośne
budują się bez niej.

## Struktura

```
src/core_v2/              protokoły: CAN, ISO-TP, UDS, J1939, ISOBUS (przenośne)
src/bench/                sesja stanowiska, zasoby, cykl życia operacji (przenośne)
src/dut_profile/          model profilu DUT (przenośny)
src/dut_profiles/daf_sac/ profil DAF SAC 250k/500k
src/runtime/              Bench Runtime (przenośny): właściciel CAN, później sesja
src/platform/linux/v2/    adaptery Linux: SocketCAN, zegar
src/platform/linux/runtime/ Linux: blokada flock, sterowanie łączem przez ip
apps/ecu_bench_runtime/   program runtime
deploy/systemd/           unit systemd do weryfikacji na stanowisku
tests/                    testy regresyjne rdzenia
docs/                     architektura, plan, pochodzenie kodu
```
