#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu
. "$(dirname "$0")/cove-io-test-lib.sh"

WAIT_SECS="${WAIT_SECS:-120}"
RUN_LOG="${RUN_LOG:-/tmp/lkvm-covg-selftest.log}"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

GUEST_COVE_IO_AUTORUN=covg \
GUEST_COVE_IO_AUTORUN_POWEROFF=1 \
busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$RUN_LOG" 2>&1 ||
{
	cat "$RUN_LOG" >&2 || true
	fail "nested COVG guest test failed"
}

cove_io_check_guest_log "$RUN_LOG" covg || fail 'Guest completion/teardown checks failed'
grep -q "CoVE-IO COVG selftest: PASS" "$RUN_LOG" ||
	fail "kernel COVG selftest did not pass"
grep -q "owner-scoped interfaces" "$RUN_LOG" ||
	fail "owner-scoped TDI enumeration was not used"
grep -q "stop transaction revoked runtime access" "$RUN_LOG" ||
	fail "guest STOP transaction was not exercised"
grep -q "COVE-IO teardown verified:" "$RUN_LOG" ||
	fail "host teardown transaction was not verified"
grep -q "COVE-IO guest autorun: PASS covg" "$RUN_LOG" ||
	fail "guest autorun did not pass"
if grep -q "CoVE-IO COVG selftest: FAIL" "$RUN_LOG"; then
	fail "kernel COVG selftest reported failure"
fi

echo "PASS: COVG enumeration/link/report/generation/MMIO-map/STOP checks passed; simulator-only authentication boundary remains explicit"
