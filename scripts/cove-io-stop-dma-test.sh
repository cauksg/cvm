#!/bin/sh
set -eu
. "$(dirname "$0")/cove-io-test-lib.sh"

VFIO_BDF=${VFIO_BDF:-0000:00:02.0}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-stop-dma.log}
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_UNSAFE_INTERRUPTS=0 \
	./vfio-bind-pci.sh "$VFIO_BDF"
if ! GUEST_VFIO_PCI="$VFIO_BDF" GUEST_COVE_IO_AUTORUN=edu-stop-dma \
	GUEST_APPEND='quiet loglevel=3 unaligned_scalar_speed=fast' \
	COVE_IO_TEST_STOP_DMA=1 busybox timeout -s KILL "${WAIT_SECS:-120}" \
	./run-guest-os.sh >"$RUN_LOG" 2>&1; then
	cat "$RUN_LOG"
	echo 'FAIL: STOP DMA execution' >&2
	exit 1
fi
cat "$RUN_LOG"
cove_io_check_guest_log "$RUN_LOG" edu-stop-dma
for marker in 'CHECK: EDU DMA positive control completed' \
	'pending=observed in_flight_write=blocked fresh_write=blocked authorization=denied'; do
	grep -Fq "$marker" "$RUN_LOG" || { echo "FAIL: missing $marker" >&2; exit 1; }
done
echo 'PASS: STOP versus pending and fresh EDU DMA'
