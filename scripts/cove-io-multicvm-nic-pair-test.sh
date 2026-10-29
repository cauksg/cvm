#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# One hart, two live CVMs, two exclusive e1000e devices each, distinct IMSIC files.
set -euo pipefail
. "$(dirname "$0")/cove-io-test-lib.sh"
A=/tmp/cove-io-nic-pair-a.log
B=/tmp/cove-io-nic-pair-b.log
WAIT_SECS=${WAIT_SECS:-540}
GUEST_CPUS=${COVE_IO_GUEST_CPUS:-1}
SMP_ARGS=
if [ "$GUEST_CPUS" -eq 2 ]; then
    SMP_ARGS='cove_io_irq_split=1'
fi
for bdf in 0000:00:02.0 0000:00:03.0 0000:00:04.0 0000:00:05.0; do
    VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
        VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$bdf"
done
COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 COVE_IO_TEST_VFIO_BACKEND=legacy \
    GUEST_CPUS=$GUEST_CPUS GUEST_MEMORY=256 GUEST_VFIO_PCI="0000:00:02.0 0000:00:03.0" \
    GUEST_COVE_IO_AUTORUN=nic-pair-direct \
    GUEST_APPEND="unaligned_scalar_speed=fast cove_io_nic=1 e1000e.IntMode=1,1 cvm_cpu_state_test=1 cove_io_guest_hold=240 $SMP_ARGS" \
    busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$A" 2>&1 &
apid=$!
trap 'kill "$apid" 2>/dev/null || true' EXIT
n=0
until grep -Fq 'COVE-IO guest hold: ready' "$A"; do
    if ! kill -0 "$apid" 2>/dev/null || [ "$n" -ge 180 ]; then
        cat "$A"; echo 'FAIL: CVM A did not reach overlap barrier'; exit 1
    fi
    sleep 1; n=$((n + 1))
    if [ "$((n % 20))" -eq 0 ]; then
        echo "COVE-IO boot progress: CVM A elapsed=$n"
        tail -12 "$A"
    fi
done
echo 'COVE-IO multi-CVM: A alive at barrier, starting B'
COVE_IO_DIRECT_DMA=1 COVE_IO_DIRECT_MMIO=1 COVE_IO_TEST_VFIO_BACKEND=legacy \
    GUEST_CPUS=$GUEST_CPUS GUEST_MEMORY=256 GUEST_VFIO_PCI="0000:00:04.0 0000:00:05.0" \
    GUEST_COVE_IO_AUTORUN=nic-pair-direct \
    GUEST_APPEND="unaligned_scalar_speed=fast cove_io_nic=1 e1000e.IntMode=1,1 cvm_cpu_state_test=1 $SMP_ARGS" \
    busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$B" 2>&1 &
bpid=$!
trap 'kill "$apid" "$bpid" 2>/dev/null || true' EXIT
n=0
while kill -0 "$bpid" 2>/dev/null; do
    sleep 5; n=$((n + 5))
    if [ "$((n % 20))" -eq 0 ]; then
        echo "COVE-IO boot progress: CVM B elapsed=$n"
        tail -16 "$B"
    fi
done
if ! wait "$bpid"; then
    cat "$A" "$B"; echo 'FAIL: CVM B execution'; exit 1
fi
kill -0 "$apid" 2>/dev/null || { cat "$A" "$B"; echo 'FAIL: no overlapping lifetime'; exit 1; }
echo 'COVE-IO multi-CVM: B destroyed while A remains alive'
if ! wait "$apid"; then cat "$A" "$B"; echo 'FAIL: CVM A execution'; exit 1; fi
trap - EXIT
cat "$A" "$B"
cove_io_check_guest_log "$A" nic-pair-direct
cove_io_check_guest_log "$B" nic-pair-direct
check_pair()
{
    local log="$1" expected_bdfs="$2" tdis count bdf
    for bdf in $expected_bdfs; do
        grep -F "COVE-IO vfio-pci TDI " "$log" | grep -Fq " started for $bdf:" || {
            echo "FAIL: missing device TDI: $bdf"; return 1;
        }
    done
    tdis=$(sed -n 's/.*COVE-IO vfio-pci TDI \([0-9][0-9]*\) started for .*/\1/p' "$log" | sort -u)
    count=$(printf '%s\n' "$tdis" | wc -l)
    [ "$count" -eq 2 ] || { echo "FAIL: expected two independent NIC TDIs: $tdis"; return 1; }
    [ "$(sed -n 's/.*COVE-IO NIC: private DMA enabled TDI=\([0-9][0-9]*\).*/\1/p' "$log" | sort -u | wc -l)" -eq 2 ] || {
        echo 'FAIL: private DMA backend did not authorize both TDIs'; return 1;
    }
    [ "$(sed -n 's/.*COVE-IO NIC: packet roundtrip verified interface=\([^ ]*\).*/\1/p' "$log" | sort -u | wc -l)" -eq 2 ] || {
        echo 'FAIL: packet roundtrip missing for a NIC'; return 1;
    }
    [ "$tdis" = "$(sed -n 's/.*COVE-IO NIC: private DMA enabled TDI=\([0-9][0-9]*\).*/\1/p' "$log" | sort -u)" ] || {
        echo 'FAIL: Guest DMA TDI set differs from assigned device TDI set'; return 1;
    }
    [ "$(sed -n 's/.*COVE-IO dynamic MSI\/MSI-X IRQ bind: .* vcpu=\([^ ]*\) iid=\([^ ]*\).*/\1:\2/p' "$log" | sort -u | wc -l)" -eq 2 ] || {
        echo 'FAIL: devices must use distinct (vCPU, IID) interrupt identities'; return 1;
    }
    grep -Fq 'COVE-IO NIC: all interfaces verified count=2' "$log" || return 1
    if [ "$GUEST_CPUS" -eq 2 ]; then
        grep -Fq 'CVM SMP selftest: PASS cpus=2 private-state/IPI' "$log" || return 1
        for cpu in 0 1; do
            grep -Fq "CVM SMP CPU state: cpu=$cpu mask=0" "$log" || return 1
        done
        for cpu in 0 1; do
            [ "$(grep -c "COVE-IO NIC: targeted MSI verified .*cpu=$cpu" "$log")" -eq 1 ] || return 1
        done
    fi
}
check_pair "$A" '0000:00:02.0 0000:00:03.0'
check_pair "$B" '0000:00:04.0 0000:00:05.0'
[ "$(sed -n 's/.*COVE-IO vfio-pci TDI \([0-9][0-9]*\) started for .*/\1/p' "$A" "$B" | sort -u | wc -l)" -eq 4 ] || {
    echo 'FAIL: four devices must have four distinct TDIs'; exit 1;
}
if [ "$GUEST_CPUS" -eq 2 ]; then
    echo 'PASS: two live single-hart two-vCPU CVMs two e1000e each four TDIs secondary MSI and teardown'
else
    echo 'PASS: two live single-hart CVMs two e1000e each four independent TDIs private DMA IMSIC and teardown'
fi
