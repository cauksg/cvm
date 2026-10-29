#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One hart, two live CVMs, exclusive e1000e and EDU, distinct IMSIC files.
set -euo pipefail
. "$(dirname "$0")/cove-io-test-lib.sh"
A=/tmp/cove-io-cvm-a.log
B=/tmp/cove-io-cvm-b.log
WAIT_SECS=${WAIT_SECS:-360}
for bdf in 0000:00:02.0 0000:00:03.0; do
    VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
        VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$bdf"
done
COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 COVE_IO_TEST_VFIO_BACKEND=legacy \
    GUEST_CPUS=1 GUEST_MEMORY=256 GUEST_VFIO_PCI=0000:00:02.0 \
    GUEST_COVE_IO_AUTORUN=nic-direct \
    GUEST_APPEND='unaligned_scalar_speed=fast cove_io_nic=1 e1000e.IntMode=1 cvm_cpu_state_test=1 cove_io_guest_hold=120' \
    busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$A" 2>&1 &
apid=$!
trap 'kill "$apid" 2>/dev/null || true' EXIT
n=0
until grep -Fq 'COVE-IO guest hold: ready' "$A"; do
    if ! kill -0 "$apid" 2>/dev/null || [ "$n" -ge 180 ]; then
        cat "$A"; echo 'FAIL: CVM A did not reach overlap barrier'; exit 1
    fi
    sleep 1; n=$((n + 1))
done
echo 'COVE-IO multi-CVM: A alive at barrier, starting B'
if ! COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 COVE_IO_TEST_VFIO_BACKEND=legacy \
    GUEST_CPUS=1 GUEST_MEMORY=256 GUEST_VFIO_PCI=0000:00:03.0 \
    GUEST_COVE_IO_AUTORUN=edu-direct \
    GUEST_APPEND='unaligned_scalar_speed=fast cove_io_edu_test=1 cvm_cpu_state_test=1' \
    busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$B" 2>&1; then
    cat "$A" "$B"; echo 'FAIL: CVM B execution'; exit 1
fi
kill -0 "$apid" 2>/dev/null || { cat "$A" "$B"; echo 'FAIL: no overlapping lifetime'; exit 1; }
echo 'COVE-IO multi-CVM: B destroyed while A remains alive'
if ! wait "$apid"; then cat "$A" "$B"; echo 'FAIL: CVM A execution'; exit 1; fi
trap - EXIT
cat "$A" "$B"
cove_io_check_guest_log "$A" nic-direct
cove_io_check_guest_log "$B" edu-direct
echo 'PASS: two live single-hart CVMs independent private DMA IMSIC and teardown'
