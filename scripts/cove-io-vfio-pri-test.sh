#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run inside the RISC-V host guest after boot-host-os.sh has started a host
# with HOST_RISCV_IOMMU=1 and a VFIO-capable PCIe edu test device. This
# validates the experimental COVE-IO PRI path: device page request, RISC-V
# IOMMU PRQ delivery, VFIO lazy DMA map recovery, and PRGR response delivery
# back to the device.

set -eu

VFIO_BDF="${VFIO_BDF:-0000:00:02.0}"
VFIO_BACKEND="${VFIO_BACKEND:-${COVE_IO_TEST_VFIO_BACKEND:-legacy}}"
DMA_IOVA="${DMA_IOVA:-0x82d00000}"
PRI_MODE="${PRI_MODE:-${COVE_IO_TEST_PRI_MODE:-basic}}"
PRI_COUNT="${PRI_COUNT:-${COVE_IO_TEST_PRI_COUNT:-16}}"
PRI_OVERFLOW_COUNT="${PRI_OVERFLOW_COUNT:-${COVE_IO_TEST_PRI_OVERFLOW_COUNT:-1024}}"
PRI_BAD_IOVA="${PRI_BAD_IOVA:-${COVE_IO_TEST_PRI_BAD_IOVA:-0x83000000}}"
WAIT_SECS="${WAIT_SECS:-300}"
RUN_LOG="${RUN_LOG:-/tmp/lkvm-cove-io-vfio-pri.log}"
BIND_LOG="${BIND_LOG:-$RUN_LOG.bind}"
PRI_LOG="${PRI_LOG:-$RUN_LOG.run}"
DMESG_LOG="${DMESG_LOG:-$RUN_LOG.dmesg}"

fail()
{
	echo "FAIL: $*" >&2
	if command -v dmesg >/dev/null 2>&1; then
		dmesg >"$DMESG_LOG" 2>/dev/null || true
	fi
	echo "bind log: $BIND_LOG" >&2
	if [ -f "$BIND_LOG" ]; then
		echo "----- bind log tail -----" >&2
		tail -80 "$BIND_LOG" >&2 || true
	fi
	echo "PRI log: $PRI_LOG" >&2
	if [ -f "$PRI_LOG" ]; then
		echo "----- PRI log tail -----" >&2
		tail -120 "$PRI_LOG" >&2 || true
	fi
	echo "dmesg log: $DMESG_LOG" >&2
	if [ -f "$DMESG_LOG" ]; then
		echo "----- dmesg log tail -----" >&2
		tail -120 "$DMESG_LOG" >&2 || true
	fi
	exit 1
}

run_timeout()
{
	timeout_cmd="${TIMEOUT_CMD:-}"

	if [ -n "$timeout_cmd" ]; then
		$timeout_cmd "$@"
	elif command -v busybox >/dev/null 2>&1; then
		busybox timeout "$@"
	elif command -v timeout >/dev/null 2>&1; then
		timeout "$@"
	else
		fail "missing command: timeout or busybox"
	fi
}

append_skip()
{
	list="$1"
	item="$2"

	if [ -z "$list" ]; then
		printf '%s\n' "$item"
		return
	fi

	case ",$list," in
	*,"$item",*) printf '%s\n' "$list" ;;
	*) printf '%s,%s\n' "$list" "$item" ;;
	esac
}

command -v grep >/dev/null 2>&1 || fail "missing command: grep"

rm -f "$RUN_LOG" "$BIND_LOG" "$PRI_LOG" "$DMESG_LOG"
: >"$BIND_LOG"

dmesg -C >/dev/null 2>&1 || dmesg -c >/dev/null 2>&1 || true

if [ -x ./vfio-bind-pci.sh ]; then
	VFIO_ALLOW_COVE_IO_PRI=${VFIO_ALLOW_COVE_IO_PRI:-1} \
	VFIO_ALLOW_COVE_IO_ISOLATED_MSI=${VFIO_ALLOW_COVE_IO_ISOLATED_MSI:-1} \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=${VFIO_ALLOW_UNSAFE_INTERRUPTS:-0} \
		./vfio-bind-pci.sh "$VFIO_BDF" >"$BIND_LOG" 2>&1 ||
		fail "failed to bind VFIO PCI device"
fi

export COVE_IO_TEST_VFIO_BACKEND="$VFIO_BACKEND"
export GUEST_VFIO_PCI="$VFIO_BDF"
export COVE_IO_TEST_SKIP="$(append_skip "${COVE_IO_TEST_SKIP:-}" vfio-iommu-map)"

GUEST_COVE_IO_AUTORUN=edu-pri \
GUEST_COVE_IO_AUTORUN_POWEROFF=1 \
GUEST_COVE_IO_PRI_IOVA="$DMA_IOVA" \
GUEST_COVE_IO_PRI_BAD_IOVA="$PRI_BAD_IOVA" \
GUEST_COVE_IO_PRI_MODE="$PRI_MODE" \
GUEST_COVE_IO_PRI_COUNT="$PRI_COUNT" \
GUEST_COVE_IO_PRI_OVERFLOW_COUNT="$PRI_OVERFLOW_COUNT" \
GUEST_APPEND="${GUEST_APPEND:-quiet loglevel=3 unaligned_scalar_speed=fast}" \
run_timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$PRI_LOG" 2>&1 ||
	fail "nested guest EDU PRI run failed"

if command -v dmesg >/dev/null 2>&1; then
	dmesg >"$DMESG_LOG" 2>/dev/null || true
fi
cat "$BIND_LOG" "$PRI_LOG" "$DMESG_LOG" >"$RUN_LOG"

grep -q "cove_io_pri" "$RUN_LOG" ||
	fail "COVE-IO PRI parameter was not enabled"

grep -q "cove_io_isolated_msi" "$RUN_LOG" ||
	fail "COVE-IO isolated MSI parameter was not enabled"

grep -q "using experimental COVE-IO RISC-V IOMMU MSI remap" "$RUN_LOG" ||
	fail "VFIO did not consume the COVE-IO isolated MSI path"

grep -q "COVE-IO experimental PCIe PRI page-request recovery enabled" "$RUN_LOG" ||
	fail "PCI ATS/PRI was not enabled for the VFIO device"

grep -q "COVE-IO guest edu-pri: success mode=$PRI_MODE" "$RUN_LOG" ||
	fail "nested guest did not complete EDU PRI mode $PRI_MODE"

if [ "$PRI_MODE" != "stop" ]; then
	grep -q "COVE-IO PRQ interrupt" "$RUN_LOG" ||
		fail "host RISC-V IOMMU did not consume a PRI page-request queue entry"
fi

case "$PRI_MODE" in
basic|stress|pasid|extended|cancel)
	grep -q "cove_io=pri-vfio-map" "$RUN_LOG" ||
		fail "host RISC-V IOMMU did not recover a COVE-IO PRI page request"
	grep -q "rc=0 resp=0" "$RUN_LOG" ||
		fail "host RISC-V IOMMU did not send a successful PRI response"
	;;
stop)
	grep -q "COVE-IO guest edu-pri-stop" "$RUN_LOG" ||
		fail "PRI stop path was not exercised"
	;;
deny)
	grep -q "cove_io=pri-deny" "$RUN_LOG" ||
		fail "host RISC-V IOMMU did not deny the unauthorized PRI page request"
	grep -q "resp=1" "$RUN_LOG" ||
		fail "host RISC-V IOMMU did not send an invalid PRI response"
	grep -q "COVE-IO guest edu-pri-invalid" "$RUN_LOG" ||
		fail "nested guest did not observe PRI invalid response"
	;;
overflow)
	grep -Eq "COVE-IO PRI enqueue overflow|Queue #[0-9]+ error; .*overflow:1|COVE-IO guest edu-pri-error" "$RUN_LOG" ||
		fail "PRI overflow/error path was not observed"
	;;
*)
	fail "unknown PRI_MODE=$PRI_MODE"
	;;
esac

if [ "$PRI_MODE" = "pasid" ] || [ "$PRI_MODE" = "extended" ]; then
	grep -Eq "pasid[:=][[:space:]]*0x1 pv:1|COVE-IO PRI request accepted: .*pasid=0x1" "$RUN_LOG" ||
		fail "host did not observe PASID-valid PRI request"
	grep -Eq "COVE-IO PRI PRGR: .*pasid=0x1|COVE-IO PRI PRGR response: .*pasid=0x1" "$RUN_LOG" ||
		fail "host did not deliver PASID-valid PRGR response"
fi

if [ "$PRI_MODE" = "extended" ]; then
	grep -Eq "pasid[:=][[:space:]]*0x2 pv:1|COVE-IO PRI request accepted: .*pasid=0x2" "$RUN_LOG" ||
		fail "host did not observe second PASID-valid PRI request"
	grep -Eq "COVE-IO PRI PRGR: .*pasid=0x2|COVE-IO PRI PRGR response: .*pasid=0x2" "$RUN_LOG" ||
		fail "host did not deliver second PASID-valid PRGR response"
fi

if [ "$PRI_MODE" = "extended" ] || [ "$PRI_MODE" = "cancel" ]; then
	grep -q "COVE-IO guest edu-pri-cancel" "$RUN_LOG" ||
		fail "PRI cancel path was not exercised"
fi

if [ "$PRI_MODE" = "extended" ] || [ "$PRI_MODE" = "stop" ]; then
	grep -q "COVE-IO guest edu-pri-stop" "$RUN_LOG" ||
		fail "PRI stop path was not exercised"
fi

if grep -Eiq "WARNING:|BUG:|refcount|leak|use-after-free" "$DMESG_LOG"; then
	fail "kernel reported warning/leak-like message"
fi

echo "PASS: VFIO PRI page-request recovery mode=$PRI_MODE and PRGR response observed ($VFIO_BACKEND)"
