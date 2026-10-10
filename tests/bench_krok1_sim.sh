#!/bin/bash
# Verifies tools/bench/krok1.sh on a simulated bench: a correct runtime must
# PASS and every injected runtime fault must FAIL exactly the right check.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
sim="$repo/tests/bench_sim"
work="$(mktemp -d /tmp/ecu-v3-krok1-sim-XXXXXX)"
trap 'rm -rf "$work"' EXIT
failures=0

export SYSFS_NET="$work/sys" CAPTURE_SECONDS=1
mkdir -p "$SYSFS_NET/can0/statistics"

# $1 = env assignments, $2 = expected exit, $3 = expected FAIL lines (regex),
# $4 = optional text that must appear in the output
run() {
  rm -f "$work/state"
  for c in rx_packets rx_errors rx_dropped; do
    echo 0 >"$SYSFS_NET/can0/statistics/$c"
  done
  env $1 PATH="$sim:$PATH" FAKE_STATE="$work/state" \
    RUNTIME="$sim/fake_runtime" LOCKDIR="$work/lock" ECU_BENCH_ALLOW_NON_ROOT=1 \
    timeout 120 bash "$repo/tools/bench/krok1.sh" >"$work/out" 2>&1
  local code=$?
  local fails
  fails="$(grep -E '^  FAIL' "$work/out" | sed 's/ — .*//' | tr -s ' ' | tr '\n' ';')"
  if [ "$code" -ne "$2" ] || ! [[ "$fails" =~ ^$3$ ]] ||
     grep -qE 'command not found|unbound variable|syntax error' "$work/out" ||
     { [ -n "${4:-}" ] && ! grep -qF -- "$4" "$work/out"; }; then
    echo "FAIL case '$1': exit=$code (expected $2), failures: '$fails'${4:+, expected text: $4}"
    sed 's/^/    /' "$work/out"
    failures=$((failures + 1))
  fi
}

# Correct runtime on a healthy bench.
run 'FAKE_BUG=none' 0 ''
run 'FAKE_BUG=none BITRATE=250000 FAKE_SAC_BITRATE=250000' 0 ''
# Every runtime fault fails exactly its own check.
run 'FAKE_BUG=no_down'    1 ' FAIL 1\. start wymusza DOWN; FAIL 5\. nowy właściciel po awarii;'
run 'FAKE_BUG=no_correct' 1 ' FAIL 4\. obca zmiana korygowana;'
run 'FAKE_BUG=no_release' 1 ' FAIL 3\. SIGTERM przy aktywnej konfiguracji;'
# Bench problems are diagnosed, not mistaken for runtime faults.
run 'FAKE_SAC_BITRATE=250000' 1 ' FAIL 3\. odbiór ramek FEAE;' \
    'WNIOSEK: sterownik nadaje przy 250000'
run 'FAKE_SAC_BITRATE=none' 1 ' FAIL 3\. odbiór ramek FEAE;' 'sprawdź zasilanie sterownika'
run 'FAKE_CANDUMP=refuse' 1 ' FAIL 3\. odbiór ramek;' 'Please disable ASCII'

# Preflight refuses to run while a V2 service could control the link.
printf '#!/bin/sh\nexit 0\n' >"$work/systemctl"; chmod +x "$work/systemctl"
PATH="$work:$sim:$PATH" FAKE_STATE="$work/state" RUNTIME="$sim/fake_runtime" \
  LOCKDIR="$work/lock" ECU_BENCH_ALLOW_NON_ROOT=1 \
  bash "$repo/tools/bench/krok1.sh" >"$work/out" 2>&1
code=$?
if [ "$code" -ne 2 ] || ! grep -q 'Zatrzymaj ją na czas testu' "$work/out"; then
  echo "FAIL preflight: active service not refused (exit=$code)"
  failures=$((failures + 1))
fi

[ "$failures" -eq 0 ] && echo "bench_krok1_sim: PASS"
exit "$failures"
