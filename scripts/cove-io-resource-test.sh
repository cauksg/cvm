#!/bin/sh
set -eu

WAIT_SECS=${WAIT_SECS:-180}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-resources.log}
if ! COVE_IO_TEST_RESOURCES=1 busybox timeout -s KILL "$WAIT_SECS" \
	./run-guest-os.sh >"$RUN_LOG" 2>&1; then
	cat "$RUN_LOG"
	echo 'FAIL: resource selftest execution' >&2
	exit 1
fi
cat "$RUN_LOG"
for marker in 'CVM capacity=32' 'IRQ capacity=256' 'TDI capacity=16' \
	'PASS: COVE-IO resource selftest'; do
	grep -Fq "$marker" "$RUN_LOG" || { echo "FAIL: missing $marker" >&2; exit 1; }
done
if grep -Eq 'FAIL:|Kernel panic|(^|[[:space:]])(WARNING:|BUG:)' "$RUN_LOG"; then
	exit 1
fi
echo 'PASS: resource exhaustion and rollback'
