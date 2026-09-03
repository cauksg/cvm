#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run inside the RISC-V host guest after boot-host-os.sh has started a host
# with HOST_RISCV_IOMMU=1 and at least two VFIO-capable PCI test devices.
# This validates the COVE-IO multi-device iommufd policy: each VFIO device is
# attached through its own IOAS/HWPT, each RID can recover its own lazy DMA
# mapping, and teardown removes both translations.

set -eu

VFIO_BDFS="${VFIO_BDFS:-}"
VFIO_BACKEND="${VFIO_BACKEND:-${COVE_IO_TEST_VFIO_BACKEND:-iommufd}}"
IOMMU_BDF="${IOMMU_BDF:-0000:00:01.0}"
DMA_IOVA="${DMA_IOVA:-0x82d00000}"
WAIT_SECS="${WAIT_SECS:-90}"
RUN_LOG="${RUN_LOG:-/tmp/lkvm-cove-io-vfio-multi.log}"
BIND_LOG="${BIND_LOG:-$RUN_LOG.bind}"
DMESG_LOG="${DMESG_LOG:-$RUN_LOG.dmesg}"

fail()
{
	echo "FAIL: $*" >&2
	echo "bind log: $BIND_LOG" >&2
	if [ -f "$BIND_LOG" ]; then
		echo "----- bind log tail -----" >&2
		tail -120 "$BIND_LOG" >&2 || true
	fi
	echo "run log: $RUN_LOG" >&2
	if [ -f "$RUN_LOG" ]; then
		echo "----- run log tail -----" >&2
		tail -160 "$RUN_LOG" >&2 || true
	fi
	echo "dmesg log: $DMESG_LOG" >&2
	if [ -f "$DMESG_LOG" ]; then
		echo "----- dmesg log tail -----" >&2
		tail -160 "$DMESG_LOG" >&2 || true
	fi
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

rid_from_bdf()
{
	bdf="$1"
	short="${bdf#????:}"
	bus="${short%%:*}"
	rest="${short#*:}"
	dev="${rest%%.*}"
	fn="${rest#*.}"

	printf '%u\n' "$(( (0x$bus << 8) | (0x$dev << 3) | 0x$fn ))"
}

group_of()
{
	bdf="$1"
	basename "$(readlink "/sys/bus/pci/devices/$bdf/iommu_group")"
}

find_edu_bdfs()
{
	for dev in /sys/bus/pci/devices/*; do
		[ -r "$dev/vendor" ] || continue
		[ -r "$dev/device" ] || continue
		[ "$(cat "$dev/vendor")" = "0x1234" ] || continue
		[ "$(cat "$dev/device")" = "0x11e8" ] || continue
		basename "$dev"
	done
}

translate()
{
	rid="$1"
	iova="$2"
	ctl="$(printf '0x%x\n' "$(( (rid << 40) | 1 ))")"

	devmem_write "$TR_REQ_IOVA" "$iova"
	devmem_write "$TR_REQ_CTL" "$ctl"
	sleep 1
	devmem_read "$TR_RESPONSE"
}

is_fault_response()
{
	[ "$(( $1 & 1 ))" -ne 0 ]
}

need awk
need basename
need devmem
need grep
need readlink

if [ -z "$VFIO_BDFS" ]; then
	VFIO_BDFS="$(find_edu_bdfs)"
fi

set -- $VFIO_BDFS
[ "$#" -ge 2 ] || fail "need at least two VFIO-capable edu devices, found: $VFIO_BDFS"

VFIO_BDF1="$1"
VFIO_BDF2="$2"
RID1="$(rid_from_bdf "$VFIO_BDF1")"
RID2="$(rid_from_bdf "$VFIO_BDF2")"
RID1_HEX="$(printf '0x%x\n' "$RID1")"
RID2_HEX="$(printf '0x%x\n' "$RID2")"
GROUP1="$(group_of "$VFIO_BDF1")"
GROUP2="$(group_of "$VFIO_BDF2")"

[ "$VFIO_BACKEND" = "iommufd" ] || [ "$VFIO_BACKEND" = "iommufd-compat" ] ||
	fail "multi-device COVE-IO validation currently requires iommufd backend"
[ "$GROUP1" != "$GROUP2" ] ||
	fail "$VFIO_BDF1 and $VFIO_BDF2 are in the same IOMMU group $GROUP1"

[ -e "/sys/bus/pci/devices/$IOMMU_BDF/resource" ] ||
	fail "missing RISC-V IOMMU PCI device $IOMMU_BDF"

IOMMU_BAR="$(awk 'NR == 1 { print $1 }' "/sys/bus/pci/devices/$IOMMU_BDF/resource")"
[ -n "$IOMMU_BAR" ] || fail "cannot read IOMMU BAR"

TR_REQ_IOVA="$(hex_add "$IOMMU_BAR" 0x258)"
TR_REQ_CTL="$(hex_add "$IOMMU_BAR" 0x260)"
TR_RESPONSE="$(hex_add "$IOMMU_BAR" 0x268)"

rm -f "$RUN_LOG" "$BIND_LOG" "$DMESG_LOG"
: >"$BIND_LOG"
dmesg -c >/dev/null 2>&1 || dmesg -C || true

if [ -x ./vfio-bind-pci.sh ]; then
	for bdf in "$VFIO_BDF1" "$VFIO_BDF2"; do
		VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 \
		VFIO_ALLOW_UNSAFE_INTERRUPTS=0 \
			./vfio-bind-pci.sh "$bdf" >>"$BIND_LOG" 2>&1 ||
			fail "failed to bind VFIO PCI device $bdf"
	done
fi

echo "COVE-IO multi VFIO devices: $VFIO_BDF1 rid=$RID1_HEX group=$GROUP1; $VFIO_BDF2 rid=$RID2_HEX group=$GROUP2" >"$RUN_LOG"

COVE_IO_TEST_SKIP=vfio-iommu-map,vcpu-run \
COVE_IO_TEST_VFIO_BACKEND="$VFIO_BACKEND" \
COVE_IO_TEST_PAUSE_BEFORE_RUN="$WAIT_SECS" \
GUEST_VFIO_PCI="$VFIO_BDF1" \
GUEST_LKVM_EXTRA_ARGS="--vfio-pci $VFIO_BDF2" \
busybox timeout -s KILL "$((WAIT_SECS + 90))" ./run-guest-os.sh >>"$RUN_LOG" 2>&1 &
LKVM_PID=$!

sleep 20

FIRST1="$(translate "$RID1" "$DMA_IOVA")"
SECOND1="$(translate "$RID1" "$DMA_IOVA")"
FIRST2="$(translate "$RID2" "$DMA_IOVA")"
SECOND2="$(translate "$RID2" "$DMA_IOVA")"

wait "$LKVM_PID" || true

POST1="$(translate "$RID1" "$DMA_IOVA")"
POST2="$(translate "$RID2" "$DMA_IOVA")"

dmesg >"$DMESG_LOG"

grep -q "COVE-IO vfio-pci TDI .* for $VFIO_BDF1" "$RUN_LOG" ||
	fail "$VFIO_BDF1 did not start a VFIO COVE-IO TDI"
grep -q "COVE-IO vfio-pci TDI .* for $VFIO_BDF2" "$RUN_LOG" ||
	fail "$VFIO_BDF2 did not start a VFIO COVE-IO TDI"

if [ "$(grep -c "allocated COVE-IO iommufd IOAS" "$RUN_LOG" || true)" -lt 2 ]; then
	fail "kvmtool did not allocate two COVE-IO iommufd IOAS objects"
fi
if [ "$(grep -c "VFIO CVM DMA window: IOAS" "$RUN_LOG" || true)" -lt 2 ]; then
	fail "kvmtool did not map the CVM DMA window into two private IOAS objects"
fi

grep -q "devid: $RID1_HEX .*cove_io=allow-vfio-map" "$DMESG_LOG" ||
	fail "first VFIO RID $RID1_HEX did not recover a lazy DMA mapping"
grep -q "devid: $RID2_HEX .*cove_io=allow-vfio-map" "$DMESG_LOG" ||
	fail "second VFIO RID $RID2_HEX did not recover a lazy DMA mapping"

[ "$FIRST1" != "$SECOND1" ] ||
	fail "first VFIO device translation did not change after recovery"
[ "$FIRST2" != "$SECOND2" ] ||
	fail "second VFIO device translation did not change after recovery"
is_fault_response "$SECOND1" &&
	fail "first VFIO device recovery did not produce a successful translation"
is_fault_response "$SECOND2" &&
	fail "second VFIO device recovery did not produce a successful translation"
if [ "$POST1" = "$SECOND1" ] && ! is_fault_response "$POST1"; then
	fail "first VFIO device translation still succeeds after teardown"
fi
if [ "$POST2" = "$SECOND2" ] && ! is_fault_response "$POST2"; then
	fail "second VFIO device translation still succeeds after teardown"
fi

if grep -Eiq "WARNING:|BUG:|refcount|leak|use-after-free" "$DMESG_LOG"; then
	fail "kernel reported warning/leak-like message"
fi

echo "PASS: VFIO multi-device per-IOAS lazy fault recovery and teardown checks passed ($VFIO_BACKEND)"
echo "dev1=$VFIO_BDF1 rid=$RID1_HEX group=$GROUP1 first=$FIRST1 second=$SECOND1 post=$POST1"
echo "dev2=$VFIO_BDF2 rid=$RID2_HEX group=$GROUP2 first=$FIRST2 second=$SECOND2 post=$POST2"
