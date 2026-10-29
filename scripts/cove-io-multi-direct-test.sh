#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One CVM/vCPU: e1000e RID16 and EDU RID24, separate BAR/DMA/MSI grants.
set -euo pipefail
. "$(dirname "$0")/cove-io-test-lib.sh"
RUN_LOG=/tmp/cove-io-multi-direct.log
WAIT_SECS=${WAIT_SECS:-420}
for bdf in 0000:00:02.0 0000:00:03.0; do
    VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
        VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$bdf"
done
COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 \
    COVE_IO_TEST_VFIO_BACKEND=legacy GUEST_CPUS=1 \
    GUEST_VFIO_PCI="0000:00:02.0 0000:00:03.0" GUEST_COVE_IO_AUTORUN=multi-direct \
    GUEST_APPEND='unaligned_scalar_speed=fast cove_io_nic=1 e1000e.IntMode=1 cove_io_edu_test=1 cvm_cpu_state_test=1' \
    busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh 2>&1 | tee "$RUN_LOG"
cove_io_check_guest_log "$RUN_LOG" multi-direct
for marker in 'COVE-IO EDU test: PASS direct BAR private DMA IRQ and Guest STOP' \
    'COVE-IO NIC: packet roundtrip verified' \
    'COVE-IO multi: NIC DMA/MSI survives independent EDU STOP'; do
    grep -Fq "$marker" "$RUN_LOG" || { echo "FAIL: missing $marker" >&2; exit 1; }
done
echo 'PASS: single CVM multi-device private DMA direct BAR distinct IMSIC and independent STOP'
