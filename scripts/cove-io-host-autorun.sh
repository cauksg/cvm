#!/bin/sh
# SPDX-License-Identifier: GPL-2.0

set -eu

CMDLINE="$(cat /proc/cmdline 2>/dev/null || true)"
TEST=""
POWEROFF=1
WAIT_SECS=""
VFIO_BDF=""
VFIO_BACKEND=""
PRI_COUNT=""
PRI_OVERFLOW_COUNT=""
MRIF_RETARGET_LOOPS=""
STRESS_LOOPS=""

cmdline_value()
{
	local key="$1"
	local token

	for token in $CMDLINE; do
		case "$token" in
		"$key="*) printf '%s\n' "${token#*=}"; return 0 ;;
		esac
	done

	return 1
}

TEST="$(cmdline_value cove_io_host_test || true)"
[ -n "$TEST" ] || exit 0

POWEROFF="$(cmdline_value cove_io_host_poweroff || printf '1\n')"
WAIT_SECS="$(cmdline_value cove_io_test_wait || true)"
VFIO_BDF="$(cmdline_value cove_io_vfio_bdf || true)"
VFIO_BACKEND="$(cmdline_value cove_io_vfio_backend || true)"
PRI_COUNT="$(cmdline_value cove_io_pri_count || true)"
PRI_OVERFLOW_COUNT="$(cmdline_value cove_io_pri_overflow_count || true)"
MRIF_RETARGET_LOOPS="$(cmdline_value cove_io_mrif_retarget_loops || true)"
STRESS_LOOPS="$(cmdline_value cove_io_stress_loops || true)"

[ -n "$WAIT_SECS" ] && export WAIT_SECS
[ -n "$VFIO_BDF" ] && export VFIO_BDF
[ -n "$VFIO_BACKEND" ] && export VFIO_BACKEND
[ -n "$PRI_COUNT" ] && export PRI_COUNT
[ -n "$PRI_OVERFLOW_COUNT" ] && export PRI_OVERFLOW_COUNT
[ -n "$MRIF_RETARGET_LOOPS" ] && export MRIF_RETARGET_LOOPS
[ -n "$STRESS_LOOPS" ] && export STRESS_LOOPS

run_test()
{
	case "$TEST" in
	covg|covg-abi)
		/scripts/cove-io-covg-test.sh
		;;
	covg-stress|lifecycle-stress)
		/scripts/cove-io-lifecycle-stress-test.sh
		;;
	vfio-msi|msi)
		/scripts/cove-io-vfio-msi-test.sh
		;;
	vfio-msi-retarget|msi-retarget)
		MSI_MODE=retarget /scripts/cove-io-vfio-msi-test.sh
		;;
	vfio-msi-retarget-stress|msi-retarget-stress)
		MSI_MODE=retarget-stress /scripts/cove-io-vfio-msi-test.sh
		;;
	vfio-lazy|lazy)
		/scripts/cove-io-vfio-lazy-fault-test.sh
		;;
	vfio-direct|direct)
		COVE_IO_DIRECT_DMA=1 DMA_IOVA="${DMA_IOVA:-0x80200000}" \
		VFIO_BACKEND=legacy /scripts/cove-io-vfio-lazy-fault-test.sh
		;;
	vfio-multi|multi)
		/scripts/cove-io-vfio-multi-test.sh
		;;
	vfio-pri|pri)
		/scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-stress|pri-stress)
		PRI_MODE=stress /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-pasid|pri-pasid)
		PRI_MODE=pasid /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-extended|pri-extended)
		PRI_MODE=extended /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-deny|pri-deny)
		PRI_MODE=deny /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-cancel|pri-cancel)
		PRI_MODE=cancel /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-stop|pri-stop)
		PRI_MODE=stop /scripts/cove-io-vfio-pri-test.sh
		;;
	vfio-pri-overflow|pri-overflow)
		PRI_MODE=overflow /scripts/cove-io-vfio-pri-test.sh
		;;
	*)
		echo "FAIL: unknown cove_io_host_test=$TEST" >&2
		return 2
		;;
	esac
}

echo "COVE-IO host autorun: test=$TEST"
if run_test >"/tmp/cove-io-host-$TEST.log" 2>&1; then
	cat "/tmp/cove-io-host-$TEST.log"
	echo "COVE-IO host autorun: PASS $TEST"
	rc=0
else
	rc=$?
	cat "/tmp/cove-io-host-$TEST.log"
	echo "COVE-IO host autorun: FAIL $TEST rc=$rc" >&2
fi

if [ "$POWEROFF" != "0" ]; then
	poweroff -f || reboot -f || true
fi

exit "$rc"
