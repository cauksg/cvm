#!/bin/sh

set -eu
. "$(dirname "$0")/cove-io-test-lib.sh"

WAIT_SECS=${WAIT_SECS:-120}
STRESS_LOOPS=${STRESS_LOOPS:-5}
LOG_DIR=${LOG_DIR:-$(mktemp -d /tmp/cove-io-lifecycle.XXXXXX)}
active_pids=""
for number in "$WAIT_SECS" "$STRESS_LOOPS"; do
	case "$number" in ''|*[!0-9]*|0) echo 'FAIL: waits and loops must be positive' >&2; exit 1 ;; esac
done
mkdir -p "$LOG_DIR"
LOG_DIR=$(cd "$LOG_DIR" && pwd)
run_dir=$(mktemp -d "$LOG_DIR/run.XXXXXX")
echo "Lifecycle logs: $run_dir"

cleanup()
{
	for child in $active_pids; do
		kill -KILL "-$child" 2>/dev/null || true
		wait "$child" 2>/dev/null || true
	done
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fail()
{
	echo "FAIL: $* (logs: $run_dir)" >&2
	for log in "$run_dir"/*.log; do
		echo "----- $log -----" >&2
		tail -30 "$log" >&2 || true
	done
	exit 1
}

launch_guest()
{
	local name="$1" gate="${2:-}"
	: >"$run_dir/$name.log"
	setsid env GUEST_COVE_IO_AUTORUN=covg GUEST_COVE_IO_AUTORUN_POWEROFF=1 \
		GUEST_MEMORY="${GUEST_MEMORY:-512}" COVE_IO_TEST_START_GATE="$gate" \
		GUEST_LKVM_EXTRA_ARGS="${GUEST_LKVM_EXTRA_ARGS:-} --name cove-$$-$name" \
		busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh \
		>"$run_dir/$name.log" 2>&1 </dev/null &
	launched_pid=$!
	active_pids="$active_pids $launched_pid"
}

reap_guest()
{
	local child="$1" result=0
	wait "$child" || result=$?
	active_pids=$(printf '%s\n' "$active_pids" | sed "s/ $child\( \|$\)/ /g")
	return "$result"
}

check_log()
{
	local log="$run_dir/$1.log"
	cove_io_check_guest_log "$log" covg || fail "$log: completion/teardown failed"
	for marker in 'CoVE-IO COVG selftest: PASS' 'owner-scoped interfaces' \
		'stop transaction revoked runtime access'; do
		grep -q "$marker" "$log" || fail "$log: missing $marker"
	done
}

wait_for_gate()
{
	local name="$1" child="$2" elapsed=0
	while ! grep -q 'COVE-IO test: waiting for start gate' "$run_dir/$name.log"; do
		kill -0 "$child" 2>/dev/null || fail "$name exited before TDI setup"
		[ "$elapsed" -lt "$WAIT_SECS" ] || fail "$name never reached start gate"
		sleep 1
		elapsed=$((elapsed + 1))
	done
}

previous_generation=0
loop=1
while [ "$loop" -le "$STRESS_LOOPS" ]; do
	launch_guest "sequential-$loop"
	reap_guest "$launched_pid" || fail "sequential iteration $loop failed"
	check_log "sequential-$loop"
	generation=$(sed -n 's/.*tdi=[0-9][0-9]* generation=\([0-9][0-9]*\)->.*/\1/p' "$run_dir/sequential-$loop.log" | head -n 1)
	[ -n "$generation" ] && [ "$generation" -gt "$previous_generation" ] ||
		fail "generation did not increase ($previous_generation -> $generation)"
	previous_generation=$generation
	loop=$((loop + 1))
done

echo "CHECK: sequential lifecycle passed loops=$STRESS_LOOPS generation=$previous_generation"
launch_guest forced-exit "$run_dir/never-release"
forced_pid=$launched_pid
wait_for_gate forced-exit "$forced_pid"
grep -q 'COVE-IO TDI [0-9][0-9]* bound' "$run_dir/forced-exit.log" || fail 'forced exit had no TDI'
kill -KILL "-$forced_pid"
forced_status=0
reap_guest "$forced_pid" || forced_status=$?
[ "$forced_status" -eq 137 ] || fail "expected SIGKILL status 137, got $forced_status"
launch_guest post-forced-exit
reap_guest "$launched_pid" || fail 'post-forced-exit recovery failed'
check_log post-forced-exit
echo 'CHECK: SIGKILL after TDI binding and subsequent CVM recovery passed'

launch_guest independent-a "$run_dir/release"
pid_a=$launched_pid
wait_for_gate independent-a "$pid_a"
launch_guest independent-b "$run_dir/release"
pid_b=$launched_pid
wait_for_gate independent-b "$pid_b"
kill -0 "$pid_a" && kill -0 "$pid_b" || fail 'CVM lifetimes did not overlap'
for name in independent-a independent-b; do
	sed -n 's/.*COVE-IO TDI \([0-9][0-9]*\) bound.*/\1/p' "$run_dir/$name.log" | sort -u >"$run_dir/$name.ids"
	[ -s "$run_dir/$name.ids" ] || fail "$name has no TDI IDs"
done
for tdi_id in $(cat "$run_dir/independent-a.ids"); do
	if grep -qx "$tdi_id" "$run_dir/independent-b.ids"; then
		fail "simultaneously bound CVMs share TDI ID $tdi_id"
	fi
done
touch "$run_dir/release"
reap_guest "$pid_a" || fail 'concurrent CVM A failed'
reap_guest "$pid_b" || fail 'concurrent CVM B failed'
check_log independent-a
check_log independent-b
echo "PASS: COVG lifecycle loops=$STRESS_LOOPS generation=$previous_generation forced_exit_recovery=passed concurrent_bound_ids=distinct"
echo 'Coverage: overlapping bound CVMs, Guest selftests and per-TDI teardown; not a proof of cross-VM confidentiality or zero leaks'
