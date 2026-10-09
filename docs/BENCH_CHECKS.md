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

Sterownik: DAF SAC 500 kbit/s. V3 nie umie jeszcze czytać DTC (krok 8),
więc w tym kroku nie porównujemy DTC — testy są krótkie, a w punkcie 3
notujemy zachowanie magistrali, które ocenimy w kroku 3.

```bash
R=./build/apps/ecu_bench_runtime/ecu_bench_runtime
sudo mkdir -p /run/ecu-bench-test
```

Proces zatrzymujemy przez `pkill -x ecu_bench_runtime`, a nie `kill %1`:
`%1` to proces `sudo`, a SIGKILL wysłany do `sudo` nie dociera do runtime.

**1. Start wymusza stan bezpieczny.**
```bash
sudo ip link set can0 type can bitrate 500000 && sudo ip link set can0 up
sudo $R --interface can0 --lock-dir /run/ecu-bench-test &
sleep 1; ip -details link show can0 | head -3     # oczekiwane: state DOWN
sudo pkill -TERM -x ecu_bench_runtime; wait
```
Oczekiwane zdarzenia: `acquired` z `"up":false`, potem `signal`, `released`.

**2. Drugi właściciel jest odrzucany.**
```bash
sudo $R --interface can0 --lock-dir /run/ecu-bench-test &
sleep 1; sudo $R --interface can0 --lock-dir /run/ecu-bench-test; echo "exit=$?"
sudo pkill -TERM -x ecu_bench_runtime; wait
```
Oczekiwane: `lock_busy`, `exit=75`.

**3. Utrzymanie konfiguracji listen-only i odbiór.**
```bash
sudo $R --interface can0 --lock-dir /run/ecu-bench-test --hold 500000:listen &
sleep 1; ip -details link show can0 | grep -E 'state|bitrate|LISTEN'
timeout 5 candump -t a can0 | grep -c '18FEAE30'   # ramki FEAE od SAC
sudo pkill -TERM -x ecu_bench_runtime; wait
```
Oczekiwane: `UP`, `bitrate 500000`, `LISTEN-ONLY`, kilka ramek FEAE.
Uwaga: w trybie listen-only CM5 nie potwierdza (ACK) ramek. Jeśli SAC jest
jedynym innym węzłem, widzi brak ACK — tak samo jak przy wyłączonym CM5.
Ramki FEAE mogą się wtedy powtarzać w `candump` (retransmisje) — zanotuj
liczbę ramek i czy się powtarzają; to dane wejściowe dla kroku 3.

**4. Obca zmiana jest korygowana.**
```bash
sudo $R --interface can0 --lock-dir /run/ecu-bench-test &
sleep 1; sudo ip link set can0 type can bitrate 250000 && sudo ip link set can0 up
sleep 2; ip -details link show can0 | head -3     # oczekiwane: state DOWN
sudo pkill -TERM -x ecu_bench_runtime; wait
```
Oczekiwane zdarzenie: `foreign_change_corrected`.

**5. Awaria procesu (SIGKILL) nie blokuje stanowiska.**
```bash
sudo $R --interface can0 --lock-dir /run/ecu-bench-test --hold 500000:normal &
sleep 1; sudo pkill -KILL -x ecu_bench_runtime; wait
ip -details link show can0 | head -3   # UP — proces nie mógł posprzątać
sudo $R --interface can0 --lock-dir /run/ecu-bench-test &
sleep 1; ip -details link show can0 | head -3   # DOWN — nowy właściciel wymusił
sudo pkill -TERM -x ecu_bench_runtime; wait
```
To jest dokładnie scenariusz, który w V2 blokował stanowisko („zajęte”).
Jako usługa systemd dodatkowo działa `ExecStopPost` (`deploy/systemd/`).

Po testach: `sudo ip link set can0 down`, uruchom ponownie usługi V2.
