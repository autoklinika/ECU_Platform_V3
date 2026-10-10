#!/usr/bin/env bash
# ECU Platform V3 — Krok 1: automatyczne sprawdzenie właściciela CAN
# na stanowisku (CM5 + sterownik na can0).
#
# Uruchomienie (z katalogu repozytorium, po zbudowaniu):
#   sudo tools/bench/krok1.sh 2>&1 | tee krok1.log
#
# Bezpieczeństwo: skrypt NIGDY nie ustawia can0 w tryb normal na prędkości,
# której nie znamy. Łącze jest konfigurowane wyłącznie w trybie listen-only
# (CM5 tylko słucha, nie nadaje i nie wysyła ramek błędów). Na końcu i przy
# każdym przerwaniu łącze zostaje wyłączone (DOWN).
#
# Zmienne (opcjonalne): IFACE=can0  BITRATE=500000  RUNTIME=<ścieżka>
# Testy bez sprzętu (tests/bench_krok1_sim.sh) ustawiają dodatkowo LOCKDIR
# i ECU_BENCH_ALLOW_NON_ROOT=1 — na stanowisku ich nie używaj.

set -uo pipefail

IFACE="${IFACE:-can0}"
BITRATE="${BITRATE:-500000}"
OTHER_BITRATE=250000
[ "$BITRATE" = 250000 ] && OTHER_BITRATE=500000
repo="$(cd "$(dirname "$0")/../.." && pwd)"
R="${RUNTIME:-$repo/build/apps/ecu_bench_runtime/ecu_bench_runtime}"
LOCKDIR="${LOCKDIR:-/run/ecu-bench-test}"
work="$(mktemp -d /tmp/ecu-v3-krok1-XXXXXX)"
results=()
failed=0
runtime_pid=""

say() { printf '%s\n' "$*"; }
indent() { sed 's/^/    /'; }
section() { printf '\n=== %s ===\n' "$*"; }
pass() { results+=("PASS  $1"); say "PASS: $1"; }
fail() { results+=("FAIL  $1 — $2"); say "FAIL: $1 — $2"; failed=1; }

# Administrative UP flag, e.g. <NOARP,UP,LOWER_UP,ECHO>.
link_up() { ip -o link show "$IFACE" | grep -qE '[<,]UP[,>]'; }
link_bitrate() {
  ip -details link show "$IFACE" | grep -oE 'bitrate [0-9]+' | head -1 | cut -d' ' -f2
}
link_listen_only() { ip -details link show "$IFACE" | grep -q 'LISTEN-ONLY'; }
berr() { ip -details -statistics link show "$IFACE" | grep -oE 'berr-counter tx [0-9]+ rx [0-9]+' | head -1; }

# True if the runtime's own "acquired" event reports the verified safe state.
# Checked together with the link itself, immediately after acquisition, so
# the periodic foreign-change correction cannot mask a missing forced DOWN.
acquired_safe() {
  grep '"event":"acquired"' "$1" | grep -q '"owner":"safe".*"up":false'
}

# Waits (max 3 s) for an event line in a runtime output file.
wait_event() {
  local i
  for i in $(seq 1 60); do
    grep -q "\"event\":\"$2\"" "$1" 2>/dev/null && return 0
    sleep 0.05
  done
  return 1
}

# Starts the runtime in the background; waits for its "acquired" event.
start_runtime() {
  local out="$1"
  shift
  "$R" --interface "$IFACE" --lock-dir "$LOCKDIR" "$@" >"$out" 2>&1 &
  runtime_pid=$!
  local i
  for i in $(seq 1 60); do
    grep -q '"event":"acquired"' "$out" 2>/dev/null && return 0
    kill -0 "$runtime_pid" 2>/dev/null || return 1
    sleep 0.05
  done
  return 1
}

stop_runtime() {
  local signal="${1:-TERM}"
  [ -n "$runtime_pid" ] || return 0
  kill "-$signal" "$runtime_pid" 2>/dev/null
  wait "$runtime_pid" 2>/dev/null
  runtime_pid=""
}

# Brings the link up in listen-only mode: the controller never transmits,
# so a wrong bitrate cannot disturb the bus.
foreign_listen_up() {
  ip link set "$IFACE" down &&
    ip link set "$IFACE" type can bitrate "$1" listen-only on fd off &&
    ip link set "$IFACE" up
}

cleanup() {
  stop_runtime TERM
  ip link set "$IFACE" down 2>/dev/null
  rm -rf "$work"
}
trap cleanup EXIT
trap 'say ""; say "Przerwano."; exit 130' INT TERM

# --------------------------------------------------------------------------
section "Przygotowanie"
[ "$(id -u)" -eq 0 ] || [ "${ECU_BENCH_ALLOW_NON_ROOT:-0}" = 1 ] ||
  { say "Uruchom przez sudo: sudo $0"; exit 2; }
[ -x "$R" ] || { say "Brak programu $R — zbuduj: cmake --build build"; exit 2; }
command -v candump >/dev/null || { say "Brak candump: sudo apt install can-utils"; exit 2; }
ip link show "$IFACE" >/dev/null 2>&1 || { say "Brak interfejsu $IFACE"; exit 2; }
for service in ecu-kiosk ecu-sac-connect-v1 ecu-platform-v2-bench-agent ecu-bench-runtime; do
  if systemctl is-active --quiet "$service" 2>/dev/null; then
    say "Usługa $service działa i może sterować $IFACE."
    say "Zatrzymaj ją na czas testu: sudo systemctl stop $service"
    exit 2
  fi
done
if pgrep -x ecu_bench_runtime >/dev/null; then
  say "Działa już inny proces ecu_bench_runtime — zatrzymaj go."
  exit 2
fi
mkdir -p "$LOCKDIR"
git_info="$(git -c safe.directory="$repo" -C "$repo" log -1 --format='%h %s' 2>/dev/null)"
say "Data:       $(date '+%Y-%m-%d %H:%M:%S %z')"
say "System:     $(uname -srm)"
say "Commit:     $git_info"
say "Interfejs:  $IFACE, prędkość sterownika: $BITRATE"
say "Stan łącza przed testem:"
ip -details -statistics link show "$IFACE" | indent

# --------------------------------------------------------------------------
section "1. Start wymusza stan bezpieczny (DOWN)"
if foreign_listen_up "$BITRATE" && link_up; then
  say "  przed startem runtime: UP (listen-only, $BITRATE)"
  if start_runtime "$work/1.out"; then
    if link_up; then
      fail "1. start wymusza DOWN" "łącze nadal UP zaraz po przejęciu"
    elif ! acquired_safe "$work/1.out"; then
      fail "1. start wymusza DOWN" "zdarzenie acquired nie potwierdza stanu bezpiecznego"
    else
      pass "1. start wymusza DOWN"
    fi
  else
    fail "1. start wymusza DOWN" "runtime nie wystartował"
  fi
  stop_runtime TERM
  if grep -q '"event":"signal".*SIGTERM' "$work/1.out" &&
     grep -q '"event":"released"' "$work/1.out"; then
    pass "1. SIGTERM: sprzątanie i zwolnienie"
  else
    fail "1. SIGTERM: sprzątanie i zwolnienie" "brak zdarzeń signal/released"
  fi
  indent <"$work/1.out"
else
  fail "1. start wymusza DOWN" "nie udało się przygotować łącza UP"
fi

# --------------------------------------------------------------------------
section "2. Drugi właściciel jest odrzucany"
if start_runtime "$work/2a.out"; then
  "$R" --interface "$IFACE" --lock-dir "$LOCKDIR" >"$work/2b.out" 2>&1
  code=$?
  if [ "$code" -eq 75 ] && grep -q '"event":"lock_busy"' "$work/2b.out"; then
    pass "2. drugi właściciel odrzucony (exit 75)"
  else
    fail "2. drugi właściciel odrzucony" "exit=$code"
  fi
  indent <"$work/2b.out"
else
  fail "2. drugi właściciel odrzucony" "pierwszy runtime nie wystartował"
fi
stop_runtime TERM

# --------------------------------------------------------------------------
section "3. Listen-only $BITRATE i odbiór ramek sterownika"
if start_runtime "$work/3.out" --hold "$BITRATE:listen"; then
  wait_event "$work/3.out" configured
  rate="$(link_bitrate)"
  if grep '"event":"configured"' "$work/3.out" | grep -q '"result":"ok"' &&
     link_up && [ "$rate" = "$BITRATE" ] && link_listen_only; then
    pass "3. konfiguracja utrzymana: UP, $BITRATE, LISTEN-ONLY"
  else
    fail "3. konfiguracja utrzymana" "up=$(link_up && echo tak || echo nie) bitrate=$rate listen-only=$(link_listen_only && echo tak || echo nie)"
  fi
  berr_before="$(berr)"
  timeout 5 candump -e -L "$IFACE" >"$work/3.dump" 2>/dev/null
  berr_after="$(berr)"
  total="$(grep -c . "$work/3.dump")"
  feae="$(grep -cE ' [0-9A-F]{2}FEAE30#' "$work/3.dump")"
  errors="$(grep -cE ' 2[0-9A-F]{7}#' "$work/3.dump")"
  say "  w 5 s: ramek łącznie $total, FEAE od SA 0x30: $feae, ramek błędów: $errors"
  say "  liczniki błędów przed: ${berr_before:-brak}, po: ${berr_after:-brak}"
  say "  identyfikatory (liczba ramek):"
  awk '{ split($3, part, "#"); print part[1] }' "$work/3.dump" | sort | uniq -c |
    sort -rn | head -12 | indent
  awk '
    / [0-9A-F][0-9A-F]FEAE30#/ {
      t = substr($1, 2, length($1) - 2) + 0
      split($3, part, "#")
      if (n > 0) {
        dt = t - pt; sum += dt
        if (dt < 0.005 && part[2] == pd) dup++
      }
      pt = t; pd = part[2]; n++
    }
    END {
      if (n > 1)
        printf "  FEAE: średni okres %.0f ms, powtórzenia (<5 ms, te same dane): %d\n",
               sum / (n - 1) * 1000, dup + 0
    }' "$work/3.dump"
  if [ "$feae" -ge 2 ]; then
    pass "3. odbiór ramek FEAE od sterownika ($feae w 5 s)"
  else
    fail "3. odbiór ramek FEAE" "$feae ramek w 5 s — czy sterownik jest zasilony i podłączony?"
  fi
  say "  pierwsze ramki:"
  head -8 "$work/3.dump" | indent
  # Stopping while the link is UP is the case that must end in DOWN.
  stop_runtime TERM
  if link_up; then
    fail "3. SIGTERM przy aktywnej konfiguracji" "łącze nadal UP po zatrzymaniu"
  else
    pass "3. SIGTERM przy aktywnej konfiguracji: łącze DOWN"
  fi
  indent <"$work/3.out"
else
  fail "3. konfiguracja utrzymana" "runtime nie wystartował"
fi
stop_runtime TERM

# --------------------------------------------------------------------------
section "4. Obca zmiana konfiguracji jest korygowana"
if start_runtime "$work/4.out"; then
  # A foreign process (here: this script) brings the link up behind the
  # runtime's back — in listen-only, so the bus is never disturbed.
  foreign_listen_up "$OTHER_BITRATE"
  corrected=0
  for i in $(seq 1 30); do
    if ! link_up && grep -q '"event":"foreign_change_corrected"' "$work/4.out"; then
      corrected=1
      break
    fi
    sleep 0.1
  done
  if [ "$corrected" -eq 1 ]; then
    pass "4. obca zmiana wykryta i cofnięta (DOWN)"
  else
    fail "4. obca zmiana korygowana" "łącze $(link_up && echo UP || echo DOWN), brak zdarzenia korekty w 3 s"
  fi
  indent <"$work/4.out"
else
  fail "4. obca zmiana korygowana" "runtime nie wystartował"
fi
stop_runtime TERM

# --------------------------------------------------------------------------
section "5. Awaria procesu (SIGKILL) nie blokuje stanowiska"
if start_runtime "$work/5a.out" --hold "$BITRATE:listen"; then
  sleep 0.3
  stop_runtime KILL
  if link_up; then
    say "  po SIGKILL łącze UP — proces nie mógł posprzątać (oczekiwane)"
    if start_runtime "$work/5b.out"; then
      if link_up; then
        fail "5. nowy właściciel po awarii" "łącze nadal UP zaraz po przejęciu"
      elif ! acquired_safe "$work/5b.out"; then
        fail "5. nowy właściciel po awarii" "zdarzenie acquired nie potwierdza stanu bezpiecznego"
      else
        pass "5. nowy właściciel przejmuje łącze i wymusza DOWN po awarii"
      fi
    else
      fail "5. nowy właściciel po awarii" "blokada nie została zwolniona po SIGKILL"
    fi
    indent <"$work/5b.out"
    stop_runtime TERM
  else
    fail "5. awaria procesu" "po SIGKILL łącze DOWN — nie udało się odtworzyć awarii"
  fi
else
  fail "5. awaria procesu" "runtime nie wystartował"
fi

# --------------------------------------------------------------------------
section "Podsumowanie"
ip link set "$IFACE" down 2>/dev/null
for line in "${results[@]}"; do say "  $line"; done
say ""
say "Stan łącza po teście: $(link_up && echo UP || echo DOWN)"
if [ "$failed" -eq 0 ]; then
  say "KROK 1: PASS"
else
  say "KROK 1: FAIL"
fi
say ""
say "Usługi V2 możesz teraz uruchomić ponownie:"
say "  sudo systemctl start ecu-platform-v2-bench-agent ecu-sac-connect-v1 ecu-kiosk"
exit "$failed"
