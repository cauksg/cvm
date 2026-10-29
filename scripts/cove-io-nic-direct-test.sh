#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# QEMU e1000e BAR0, one MSI/IMSIC-direct vector, private DMA, legacy VFIO.
set -euo pipefail
. "$(dirname "$0")/cove-io-test-lib.sh"
VFIO_BDF=${VFIO_BDF:-0000:00:02.0}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-nic-direct.log}
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$VFIO_BDF"
if ! COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 \
	COVE_IO_TEST_VFIO_BACKEND=legacy GUEST_CPUS=1 \
	GUEST_VFIO_PCI="$VFIO_BDF" GUEST_COVE_IO_AUTORUN=nic-direct \
	GUEST_APPEND='unaligned_scalar_speed=fast cove_io_nic=1 e1000e.IntMode=1 cvm_cpu_state_test=1' \
	busybox timeout -s KILL "${WAIT_SECS:-180}" ./run-guest-os.sh 2>&1 | tee "$RUN_LOG"; then
	cat "$RUN_LOG"
	echo 'FAIL: NIC direct execution' >&2
	exit 1
fi
cat "$RUN_LOG"
cove_io_check_guest_log "$RUN_LOG" nic-direct
for marker in 'COVE-IO direct BAR negative checks: partial/stale/RID denied' \
	'COVE-IO NIC: private DMA enabled' \
	'COVE-IO NIC: packet roundtrip verified'; do
	grep -Fq "$marker" "$RUN_LOG" || { echo "FAIL: missing $marker" >&2; exit 1; }
done
if grep -Eq 'cove_io=allow-vfio-map|private DMA preparation failed|NETDEV WATCHDOG' "$RUN_LOG"; then
	echo 'FAIL: NIC required DMA fault recovery or lost its queue' >&2
	exit 1
fi
echo 'PASS: e1000e direct BAR private DMA MSI packet roundtrip and teardown'
