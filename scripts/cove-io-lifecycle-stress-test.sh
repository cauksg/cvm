#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

WAIT_SECS="${WAIT_SECS:-120}"
STRESS_LOOPS="${STRESS_LOOPS:-5}"
LOG_DIR="${LOG_DIR:-/tmp/cove-io-lifecycle-stress}"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

case "$STRESS_LOOPS" in
''|*[!0-9]*) fail "STRESS_LOOPS must be a positive integer" ;;
0) fail "STRESS_LOOPS must be greater than zero" ;;
esac

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"

check_log()
{
	log="$1"

	grep -q "CoVE-IO COVG selftest: PASS" "$log" ||
		fail "$log: guest COVG selftest did not pass"
	grep -q "COVE-IO guest autorun: PASS covg" "$log" ||
		fail "$log: guest autorun did not pass"
	grep -q "owner-scoped interfaces" "$log" ||
		fail "$log: owner-scoped enumeration was not used"
	grep -q "stop transaction revoked runtime access" "$log" ||
		fail "$log: guest STOP transaction was not exercised"
	grep -q "COVE-IO teardown verified:" "$log" ||
		fail "$log: teardown verification was not reported"
	if grep -Eq "CoVE-IO COVG selftest: FAIL|COVE-IO (STOP|FINALIZE_STOP|UNREGISTER|final state|teardown .* revoke|owner enumeration).*failed|Initialisation failed|Unhandled trap" "$log"; then
		fail "$log: lifecycle failure marker found"
	fi
}

run_guest()
{
	log="$1"

	GUEST_COVE_IO_AUTORUN=covg \
	GUEST_COVE_IO_AUTORUN_POWEROFF=1 \
	busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$log" 2>&1
}

previous_generation=0
loop=1
while [ "$loop" -le "$STRESS_LOOPS" ]; do
	log="$LOG_DIR/sequential-$loop.log"
	run_guest "$log" || {
		cat "$log" >&2 || true
		fail "sequential lifecycle iteration $loop failed"
	}
	check_log "$log"
	generation="$(sed -n 's/.*tdi=[0-9][0-9]* generation=\([0-9][0-9]*\)->.*/\1/p' "$log" | head -n 1)"
	[ -n "$generation" ] || fail "$log: fixed TDI generation was not reported"
	[ "$generation" -gt "$previous_generation" ] ||
		fail "$log: generation did not increase ($previous_generation -> $generation)"
	previous_generation="$generation"
	loop=$((loop + 1))
done

run_guest "$LOG_DIR/independent-a.log" || {
	cat "$LOG_DIR/independent-a.log" >&2 || true
	fail "independent CVM A failed"
}
check_log "$LOG_DIR/independent-a.log"
run_guest "$LOG_DIR/independent-b.log" || {
	cat "$LOG_DIR/independent-b.log" >&2 || true
	fail "independent CVM B failed"
}
check_log "$LOG_DIR/independent-b.log"

sed -n 's/.*COVE-IO TDI \([0-9][0-9]*\) bound.*/\1/p' \
	"$LOG_DIR/independent-a.log" | sort -u >"$LOG_DIR/independent-a.ids"
sed -n 's/.*COVE-IO TDI \([0-9][0-9]*\) bound.*/\1/p' \
	"$LOG_DIR/independent-b.log" | sort -u >"$LOG_DIR/independent-b.ids"
[ -s "$LOG_DIR/independent-a.ids" ] || fail "independent CVM A exposed no TDI IDs"
[ -s "$LOG_DIR/independent-b.ids" ] || fail "independent CVM B exposed no TDI IDs"
for id in $(cat "$LOG_DIR/independent-a.ids"); do
	if grep -qx "$id" "$LOG_DIR/independent-b.ids"; then
		fail "independent CVMs reused TDI ID $id"
	fi
done

echo "PASS: COVG lifecycle stress loops=$STRESS_LOOPS generation=$previous_generation independent_owner_isolation=verified"
