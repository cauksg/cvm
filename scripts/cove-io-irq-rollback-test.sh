#!/bin/sh
set -eu

VFIO_BDF=${VFIO_BDF:-0000:00:02.0}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-irq-rollback.log}
dmesg -c >/dev/null
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_UNSAFE_INTERRUPTS=0 \
	./vfio-bind-pci.sh "$VFIO_BDF"
if ! GUEST_VFIO_PCI="$VFIO_BDF" COVE_IO_TEST_IRQ_ROLLBACK=1 \
	busybox timeout -s KILL "${WAIT_SECS:-120}" ./run-guest-os.sh >"$RUN_LOG" 2>&1; then
	cat "$RUN_LOG"
	echo 'FAIL: IRQ rollback execution' >&2
	exit 1
fi
cat "$RUN_LOG"
grep -Fq 'PASS: IMSIC partial-bind rollback state=ERROR authorization=denied cleanup=FREE' \
	"$RUN_LOG" || { echo 'FAIL: missing hardware rollback evidence' >&2; exit 1; }
dmesg >"$RUN_LOG.dmesg"
grep -Eq 'COVE-IO IMSIC bind failed .*vcpu=4096 iid=5 ret=-19$' "$RUN_LOG.dmesg" ||
	{ echo 'FAIL: missing injected IMSIC failure' >&2; exit 1; }
sed '/COVE-IO IMSIC bind failed .*vcpu=4096 iid=5 ret=-19$/d' \
	"$RUN_LOG.dmesg" >"$RUN_LOG.unexpected"
if grep -Eq '(^|[[:space:]])(WARNING:|BUG:)|Kernel panic' \
	"$RUN_LOG.unexpected" "$RUN_LOG"; then
	echo 'FAIL: unexpected IRQ rollback diagnostic' >&2
	exit 1
fi
dmesg -c >/dev/null
. /scripts/cove-io-test-lib.sh
if ! GUEST_VFIO_PCI="$VFIO_BDF" GUEST_COVE_IO_AUTORUN=edu-msi \
	GUEST_APPEND='quiet loglevel=3 unaligned_scalar_speed=fast' \
	busybox timeout -s KILL "${WAIT_SECS:-120}" ./run-guest-os.sh >"$RUN_LOG.retry" 2>&1; then
	cat "$RUN_LOG.retry"
	echo 'FAIL: MSI retry after IRQ rollback' >&2
	exit 1
fi
cove_io_check_guest_log "$RUN_LOG.retry" edu-msi
grep -Fq 'COVE-IO guest edu-msi: request-ack verified' "$RUN_LOG.retry" ||
	{ echo 'FAIL: MSI retry did not exercise EDU' >&2; exit 1; }
dmesg >"$RUN_LOG.retry-dmesg"
grep -Fq 'COVE-IO IMSIC direct remap installed' "$RUN_LOG.retry-dmesg" ||
	{ echo 'FAIL: MSI retry did not install IMSIC' >&2; exit 1; }
if grep -Eq 'COVE-IO .*failed|(^|[[:space:]])(WARNING:|BUG:)|Kernel panic' \
	"$RUN_LOG.retry-dmesg"; then
	echo 'FAIL: MSI retry kernel diagnostic' >&2
	exit 1
fi
cat "$RUN_LOG.retry"
echo 'PASS: hardware IRQ bind rollback followed by MSI retry'
