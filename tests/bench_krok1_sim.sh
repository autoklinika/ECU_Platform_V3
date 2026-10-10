#!/bin/bash
# Verifies tools/bench/krok1.sh on a simulated bench: a correct runtime must
# PASS and every injected runtime fault must FAIL exactly the right check.
set -u
repo="$(cd "$(dirname "$0")/.." && pwd)"
sim="$repo/tests/bench_sim"
work="$(mktemp -d /tmp/ecu-v3-krok1-sim-XXXXXX)"
trap 'rm -rf "$work"' EXIT
failures=0

run() {  # $1 = fault, $2 = expected exit, $3 = expected FAIL lines (regex)
  rm -f "$work/state"
  PATH="$sim:$PATH" FAKE_STATE="$work/state" FAKE_BUG="$1" \
    RUNTIME="$sim/fake_runtime" LOCKDIR="$work/lock" ECU_BENCH_ALLOW_NON_ROOT=1 \
    timeout 60 bash "$repo/tools/bench/krok1.sh" >"$work/out" 2>&1
  local code=$?
  local fails
  fails="$(grep -E '^  FAIL' "$work/out" | sed 's/ — .*//' | tr -s ' ' | tr '\n' ';')"
  if [ "$code" -ne "$2" ] || ! [[ "$fails" =~ ^$3$ ]]; then
    echo "FAIL fault=$1: exit=$code (expected $2), failures: '$fails'"
    sed 's/^/    /' "$work/out"
    failures=$((failures + 1))
  fi
}

run none       0 ''
run no_down    1 ' FAIL 1\. start wymusza DOWN; FAIL 5\. nowy właściciel po awarii;'
run no_correct 1 ' FAIL 4\. obca zmiana korygowana;'
run no_release 1 ' FAIL 3\. SIGTERM przy aktywnej konfiguracji;'

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
