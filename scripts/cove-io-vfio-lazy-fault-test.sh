#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run inside the RISC-V host guest after boot-host-os.sh has started a host
# with HOST_RISCV_IOMMU=1 and a VFIO-capable PCI test device.

set -eu

VFIO_BDF="${VFIO_BDF:-0000:00:02.0}"
IOMMU_BDF="${IOMMU_BDF:-0000:00:01.0}"
VFIO_BACKEND="${VFIO_BACKEND:-${COVE_IO_TEST_VFIO_BACKEND:-legacy}}"
DMA_IOVA="${DMA_IOVA:-0x82d00000}"
RID="${RID:-0x10}"
WAIT_SECS="${WAIT_SECS:-90}"
RUN_LOG="${RUN_LOG:-/tmp/lkvm-lazy-vfio-recover.log}"
MULTI_DEV_LOG="${MULTI_DEV_LOG:-/tmp/lkvm-lazy-vfio-iommufd-multidev.log}"

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

need()
{
	command -v "$1" >/dev/null 2>&1 || fail "missing command: $1"
}

hex_add()
{
	printf '0x%x\n' "$(( $1 + $2 ))"
}

devmem_read()
{
	devmem "$1" 64
}

devmem_write()
{
	devmem "$1" 64 "$2" >/dev/null
}

translate()
{
	iova="$1"
	devmem_write "$TR_REQ_IOVA" "$iova"
	devmem_write "$TR_REQ_CTL" "$TR_CTL"
	sleep 1
	devmem_read "$TR_RESPONSE"
}

is_fault_response()
{
	[ "$(( $1 & 1 ))" -ne 0 ]
}

need awk
need devmem
need dmesg

[ -e "/sys/bus/pci/devices/$IOMMU_BDF/resource" ] ||
	fail "missing RISC-V IOMMU PCI device $IOMMU_BDF"

IOMMU_BAR="$(awk 'NR == 1 { print $1 }' "/sys/bus/pci/devices/$IOMMU_BDF/resource")"
[ -n "$IOMMU_BAR" ] || fail "cannot read IOMMU BAR"

TR_REQ_IOVA="$(hex_add "$IOMMU_BAR" 0x258)"
TR_REQ_CTL="$(hex_add "$IOMMU_BAR" 0x260)"
TR_RESPONSE="$(hex_add "$IOMMU_BAR" 0x268)"
TR_CTL="$(printf '0x%x\n' "$(( (RID << 40) | 1 ))")"
DMA_IOVA_2="$(hex_add "$DMA_IOVA" 0x1000)"

dmesg -c >/dev/null 2>&1 || dmesg -C || true

if [ -x ./vfio-bind-pci.sh ]; then
	VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=0 \
		./vfio-bind-pci.sh "$VFIO_BDF"
fi

if [ "$VFIO_BACKEND" = "iommufd" ] || [ "$VFIO_BACKEND" = "iommufd-compat" ]; then
	set +e
	COVE_IO_TEST_VFIO_BACKEND="$VFIO_BACKEND" \
	GUEST_VFIO_PCI="$VFIO_BDF" \
	GUEST_LKVM_EXTRA_ARGS="--vfio-pci $VFIO_BDF" \
	busybox timeout -s KILL 30 ./run-guest-os.sh >"$MULTI_DEV_LOG" 2>&1
	MULTI_DEV_RET=$?
	set -e

	[ "$MULTI_DEV_RET" -ne 0 ] ||
		fail "iommufd COVE-IO same-group per-device IOAS configuration was not rejected"
	grep -q "requires one VFIO IOMMU group per device" "$MULTI_DEV_LOG" ||
		fail "iommufd COVE-IO same-group rejection did not report per-device IOMMU-group policy"
fi

COVE_IO_TEST_SKIP=vfio-iommu-map \
COVE_IO_TEST_VFIO_BACKEND="$VFIO_BACKEND" \
GUEST_VFIO_PCI="$VFIO_BDF" \
busybox timeout -s KILL "$((WAIT_SECS + 60))" ./run-guest-os.sh >"$RUN_LOG" 2>&1 &
LKVM_PID=$!

ACCEPTED=0
LKVM_CHILD=
for second in $(seq 1 "$WAIT_SECS"); do
	if grep -q "CoVE-IO: guest accepted" "$RUN_LOG"; then
		ACCEPTED=1
		LKVM_CHILD="$(pidof lkvm-static || true)"
		break
	fi
	if ! kill -0 "$LKVM_PID" 2>/dev/null; then
		break
	fi
	sleep 1
done
if [ "$ACCEPTED" -ne 1 ]; then
	cat "$RUN_LOG" >&2 || true
	fail "guest did not accept the VFIO TDI through COVG"
fi
[ -n "$LKVM_CHILD" ] || fail "cannot identify the running lkvm-static process"

FIRST_RESP="$(translate "$DMA_IOVA")"
SECOND_RESP="$(translate "$DMA_IOVA")"
THIRD_RESP="$(translate "$DMA_IOVA_2")"
FOURTH_RESP="$(translate "$DMA_IOVA_2")"

for pid in $LKVM_CHILD; do
	kill -KILL "$pid" 2>/dev/null || true
done
wait "$LKVM_PID" 2>/dev/null || true
sleep 2

POST_EXIT_RESP="$(translate "$DMA_IOVA")"

dmesg >/tmp/cove-io-vfio-lazy-fault.dmesg

grep -q "cove_io=allow-vfio-map" /tmp/cove-io-vfio-lazy-fault.dmesg ||
	fail "authorized lazy fault was not recovered"
if [ "${COVE_IO_DIRECT_DMA:-0}" != "0" ]; then
	grep -Eq "cove_io=allow-vfio-map trusted_hpa=0x0*[1-9a-fA-F]" \
		/tmp/cove-io-vfio-lazy-fault.dmesg ||
		fail "direct DMA did not recover a Monitor-selected trusted HPA"
	grep -q "direct-dma" "$RUN_LOG" ||
		fail "kvmtool did not advertise direct-DMA monitor support"
fi

[ "$FIRST_RESP" != "$SECOND_RESP" ] ||
	fail "repeated first-page translation did not change after recovery"
[ "$THIRD_RESP" != "$FOURTH_RESP" ] ||
	fail "repeated second-page translation did not change after recovery"
is_fault_response "$SECOND_RESP" &&
	fail "first-page recovery did not produce a successful translation"
is_fault_response "$FOURTH_RESP" &&
	fail "second-page recovery did not produce a successful translation"
if [ "$POST_EXIT_RESP" = "$SECOND_RESP" ] &&
   ! is_fault_response "$POST_EXIT_RESP"; then
	fail "translation still succeeds after VM/VFIO teardown"
fi

if grep -Eiq "WARNING:|BUG:|refcount|leak|use-after-free" \
	/tmp/cove-io-vfio-lazy-fault.dmesg; then
	fail "kernel reported warning/leak-like message"
fi

echo "PASS: VFIO lazy fault recovery repeated, adjacent-page, and teardown checks passed ($VFIO_BACKEND)"
echo "first=$FIRST_RESP second=$SECOND_RESP third=$THIRD_RESP fourth=$FOURTH_RESP post_exit=$POST_EXIT_RESP"
