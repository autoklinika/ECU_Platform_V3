#!/bin/sh
# End-to-end test of ecu_bench_runtime without CAN hardware.
# The ip program is replaced by a recording script; the interface does not
# exist, so the owner reports it as unavailable but must still:
#   - force the link DOWN on start,
#   - refuse a second owner (exit 75) without touching the link,
#   - force the link DOWN again on SIGTERM and exit,
#   - release the lock so a new owner can start.
set -eu
runtime="$1"
work="$(mktemp -d /tmp/ecu-v3-e2e-XXXXXX)"
trap 'kill "$first" 2>/dev/null || true; rm -rf "$work"' EXIT
log="$work/ip.log"
cat > "$work/ip" <<EOF
#!/bin/sh
echo "\$*" >> "$log"
EOF
chmod 700 "$work/ip"
iface=ecuv3nolink0

fail() { echo "FAIL: $*" >&2; exit 1; }

"$runtime" --interface "$iface" --lock-dir "$work" --ip "$work/ip" \
  > "$work/first.out" 2>&1 &
first=$!
i=0
until grep -q '"event":"acquired"' "$work/first.out" 2>/dev/null; do
  i=$((i + 1)); [ "$i" -lt 100 ] || fail "first runtime did not start"
  sleep 0.05
done
grep -qx "link set $iface down" "$log" || fail "no DOWN on start"

set +e
"$runtime" --interface "$iface" --lock-dir "$work" --ip "$work/ip" \
  > "$work/second.out" 2>&1
code=$?
set -e
[ "$code" -eq 75 ] || fail "second owner exit code $code, expected 75"
grep -q '"event":"lock_busy"' "$work/second.out" || fail "no lock_busy event"
[ "$(grep -c down "$log")" -eq 1 ] || fail "second owner touched the link"

kill -TERM "$first"
wait "$first" || true
grep -q '"event":"signal".*SIGTERM' "$work/first.out" || fail "no SIGTERM event"
grep -q '"event":"released"' "$work/first.out" || fail "no released event"
[ "$(grep -c down "$log")" -eq 2 ] || fail "no DOWN on shutdown"

# The lock is free again for the next owner.
"$runtime" --interface "$iface" --lock-dir "$work" --ip "$work/ip" \
  > "$work/third.out" 2>&1 &
first=$!
i=0
until grep -q '"event":"acquired"' "$work/third.out" 2>/dev/null; do
  i=$((i + 1)); [ "$i" -lt 100 ] || fail "lock not released after shutdown"
  sleep 0.05
done
kill -KILL "$first"; wait "$first" 2>/dev/null || true

# SIGKILL leaves no cleanup, but the kernel frees the lock immediately.
set +e
"$runtime" --interface "$iface" --lock-dir "$work" --ip "$work/ip" \
  > "$work/fourth.out" 2>&1 &
first=$!
set -e
i=0
until grep -q '"event":"acquired"' "$work/fourth.out" 2>/dev/null; do
  i=$((i + 1)); [ "$i" -lt 100 ] || fail "lock not free after SIGKILL"
  sleep 0.05
done
kill -TERM "$first"; wait "$first" || true

# Invalid arguments never touch the link.
before="$(wc -l < "$log")"
set +e
"$runtime" --interface "can0;reboot" --lock-dir "$work" --ip "$work/ip" \
  > /dev/null 2>&1
code=$?
"$runtime" --interface can0 --hold 333333 --lock-dir "$work" --ip "$work/ip" \
  > /dev/null 2>&1
code2=$?
set -e
[ "$code" -eq 64 ] && [ "$code2" -eq 64 ] || fail "bad arguments accepted"
[ "$(wc -l < "$log")" -eq "$before" ] || fail "bad arguments touched link"

echo "runtime_app_e2e: PASS"
