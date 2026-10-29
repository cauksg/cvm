#!/bin/sh
set -eu

VFIO_BDF=${VFIO_BDF:-0000:00:02.0}
IOMMU_BDF=${IOMMU_BDF:-0000:00:01.0}
WAIT_SECS=${WAIT_SECS:-120}
RUN_LOG=${RUN_LOG:-/tmp/cove-io-iommu-rollback.log}
fault_control=/sys/module/iommu/parameters/cove_io_test_fail_maps
child=

cleanup()
{
	[ ! -w "$fault_control" ] || echo 0 >"$fault_control"
	if [ -n "$child" ]; then
		kill -KILL "-$child" 2>/dev/null || true
		wait "$child" 2>/dev/null || true
	fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
fail()
{
	echo "FAIL: $*" >&2
	cat "$RUN_LOG" >&2
	dmesg >&2
	exit 1
}
translate()
{
	devmem "$((bar + 0x258))" 64 0x82d00000 >/dev/null
	devmem "$((bar + 0x260))" 64 "$((0x10 << 40 | 1))" >/dev/null
	sleep 1
	response=$(devmem "$((bar + 0x268))" 64)
}

: >"$RUN_LOG"
[ -w "$fault_control" ] || fail 'requires Host CONFIG_FAULT_INJECTION=y'
dmesg -c >/dev/null
VFIO_ALLOW_COVE_IO_ISOLATED_MSI=1 VFIO_ALLOW_COVE_IO_IMSIC=1 \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=0 ./vfio-bind-pci.sh "$VFIO_BDF"
bar=$(awk 'NR == 1 { print $1 }' "/sys/bus/pci/devices/$IOMMU_BDF/resource")
setsid env COVE_IO_TEST_SKIP=vfio-iommu-map GUEST_VFIO_PCI="$VFIO_BDF" \
	GUEST_COVE_IO_AUTORUN=edu-msi GUEST_COVE_IO_AUTORUN_POWEROFF=0 \
	GUEST_APPEND='quiet loglevel=3 unaligned_scalar_speed=fast' \
	busybox timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh \
	>"$RUN_LOG" 2>&1 </dev/null &
child=$!
elapsed=0
while ! grep -q 'COVE-IO guest autorun: PASS edu-msi' "$RUN_LOG"; do
	kill -0 "$child" 2>/dev/null || fail 'Guest exited before acceptance'
	[ "$elapsed" -lt "$WAIT_SECS" ] || fail 'Guest acceptance timed out'
	sleep 1
	elapsed=$((elapsed + 1))
done
echo 1 >"$fault_control"
translate
[ "$((response & 1))" -ne 0 ] || fail 'injected mapping unexpectedly translated'
[ "$(cat "$fault_control")" = 0 ] || fail 'IOMMU failure was not consumed'
dmesg >"$RUN_LOG.dmesg"
grep -Eq 'cove_io=allow-vfio-map .*rc=-12$' "$RUN_LOG.dmesg" ||
	fail 'ENOMEM did not propagate through VFIO recovery'
translate
[ "$((response & 1))" -ne 0 ] || fail 'failed transaction left an IOTLB mapping'
translate
[ "$((response & 1))" -eq 0 ] || fail 'same IOVA retry did not map'
dmesg >"$RUN_LOG.dmesg"
grep -Eq 'cove_io=allow-vfio-map .*rc=0$' "$RUN_LOG.dmesg" || fail 'missing recovery success'
kill -KILL "-$child"
status=0
wait "$child" || status=$?
child=
[ "$status" -eq 137 ] || fail "expected SIGKILL exit137, got $status"
sleep 1
echo 'PASS: IOMMU ENOMEM propagated; failed mapping left no stale IOTLB entry; same-IOVA retry passed'
