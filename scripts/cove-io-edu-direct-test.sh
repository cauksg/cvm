#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# Single EDU, legacy VFIO, one CVM/vCPU. Requires dma_mask=0xffffffff.
set -euo pipefail
. "$(dirname "$0")/cove-io-test-lib.sh"
VFIO_BDF=${VFIO_BDF:-0000:00:02.0}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-edu-direct.log}
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$VFIO_BDF"
if ! COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 \
	COVE_IO_TEST_VFIO_BACKEND=legacy GUEST_CPUS=1 \
	GUEST_VFIO_PCI="$VFIO_BDF" GUEST_COVE_IO_AUTORUN=edu-direct \
	GUEST_APPEND='unaligned_scalar_speed=fast cove_io_edu_test=1 cvm_cpu_state_test=1' \
	busybox timeout -s KILL "${WAIT_SECS:-120}" ./run-guest-os.sh 2>&1 | tee "$RUN_LOG"; then
	cat "$RUN_LOG"
	echo 'FAIL: EDU direct execution' >&2
	exit 1
fi
cat "$RUN_LOG"
cove_io_check_guest_log "$RUN_LOG" edu-direct
for marker in 'COVE-IO direct BAR negative checks: partial/stale/RID denied' \
	'private DMA roundtrip verified bytes=256' \
	'pending_write=blocked interrupt=blocked'; do
	grep -Fq "$marker" "$RUN_LOG" || { echo "FAIL: missing $marker" >&2; exit 1; }
done
# The direct mode aborts on any userspace BAR access, preventing silent fallback.
echo 'PASS: EDU direct BAR private DMA real IRQ and Guest STOP'
