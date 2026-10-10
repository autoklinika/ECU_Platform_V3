# Weryfikacja na stanowisku

Każdy krok ma tu listę sprawdzeń do wykonania na CM5 z prawdziwym sterownikiem.
Wyniki (wklejone wyjście poleceń) dołącz w komentarzu do PR danego kroku.

## Przygotowanie (wspólne)

```bash
git clone https://github.com/autoklinika/ECU_Platform_V3.git ~/ECU_Platform_V3
cd ~/ECU_Platform_V3 && git switch <gałąź kroku>
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure      # musi być 100%

# Usługi V2 na CM5 same sterują can0 i nie znają blokady V3.
# Nie korzystamy z nich, ale musimy je zatrzymać, żeby nie przeszkadzały:
sudo systemctl stop ecu-kiosk ecu-sac-connect-v1 ecu-platform-v2-bench-agent
# po testach: sudo systemctl start ecu-platform-v2-bench-agent ecu-sac-connect-v1 ecu-kiosk
```

## Krok 1 — właściciel CAN

Sterownik: DAF SAC 500 kbit/s, zasilony i podłączony do `can0`.
V3 nie umie jeszcze czytać DTC (krok 8), więc w tym kroku nie porównujemy
DTC. Test jest bezpieczny dla sterownika: `can0` jest konfigurowane
**wyłącznie w trybie listen-only** — CM5 tylko słucha, nic nie nadaje i nie
wysyła ramek błędów, także gdy celowo ustawiamy złą prędkość (punkt 4).

### Uruchomienie (ok. 30 s)

```bash
cd ~/ECU_Platform_V3
git pull
cmake --build build --parallel 2 && ctest --test-dir build
sudo systemctl stop ecu-kiosk ecu-sac-connect-v1 ecu-platform-v2-bench-agent
sudo tools/bench/krok1.sh 2>&1 | tee krok1.log
```

Skrypt sam sprawdza, czy usługi V2 są zatrzymane, wykonuje 5 sprawdzeń,
na końcu wyłącza `can0` i wypisuje `KROK 1: PASS` albo `KROK 1: FAIL`.
Prześlij plik `krok1.log` (albo wklej jego zawartość).

Po teście uruchom ponownie usługi V2:
```bash
sudo systemctl start ecu-platform-v2-bench-agent ecu-sac-connect-v1 ecu-kiosk
```

### Co jest sprawdzane

| # | Sprawdzenie | Dlaczego |
|---|---|---|
| 1 | Łącze pozostawione UP zostaje wyłączone **od razu przy przejęciu**; SIGTERM kończy się zwolnieniem | V2 zostawiało `can0` UP po restarcie i blokowało stanowisko |
| 2 | Drugi proces nie dostaje łącza (`exit 75`) i go nie dotyka | dwóch właścicieli jednego łącza w V2 |
| 3 | Utrzymanie `500000` + listen-only, odbiór ramek SAC, a po SIGTERM łącze DOWN | dane o magistrali do kroku 3 (patrz niżej) |
| 4 | Obca zmiana konfiguracji (inna prędkość, listen-only) jest cofana w ≤ 3 s | nikt poza runtime nie steruje łączem |
| 5 | Po `kill -9` (bez sprzątania) nowy właściciel natychmiast wymusza DOWN | awaria procesu nie może zablokować stanowiska |

Punkt 3 wypisuje dane potrzebne do zaprojektowania kroku 3 (wykrywanie
prędkości): liczbę ramek FEAE i ich średni okres, liczbę powtórzeń (ramki
tej samej treści w odstępie < 5 ms — retransmisje, gdy nikt nie potwierdza
ACK), listę identyfikatorów nadawanych przez SAC oraz liczniki błędów
kontrolera przed i po.

Skrypt jest testowany w CI na symulowanym stanowisku
(`tests/bench_krok1_sim.sh`): poprawny runtime przechodzi, a każda
wstrzyknięta usterka (brak DOWN przy przejęciu, brak korekty, brak DOWN przy
SIGTERM) daje FAIL dokładnie w odpowiednim punkcie.

### Gdy coś pójdzie nie tak

- `Usługa … działa` — zatrzymaj wskazaną usługę i uruchom skrypt ponownie.
- `Brak candump` — `sudo apt install can-utils`.
- `3. odbiór ramek FEAE — 0 ramek` — sprawdź zasilanie SAC i przewody CAN;
  jeśli to wariant 250 kbit/s: `sudo BITRATE=250000 tools/bench/krok1.sh …`.
- Przerwanie skryptu (Ctrl+C) zawsze kończy się wyłączeniem `can0`.
