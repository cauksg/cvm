#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Run inside the RISC-V host guest after boot-host-os.sh has started a host
# with a VFIO-capable PCI test device. This validates that kvmtool no longer
# hides MSI/MSI-X from a CVM VFIO PCI device and that an MSI/MSI-X GSI selected
# by the nested guest is dynamically bound into the COVE-IO TDI. It also runs
# the deterministic VFIO COVE-IO probe path, including target device,
# target-vCPU, and IMSIC-IID negative checks, before the real MSI/MSI-X run.

set -eu

VFIO_BDF="${VFIO_BDF:-0000:00:02.0}"
VFIO_BACKEND="${VFIO_BACKEND:-${COVE_IO_TEST_VFIO_BACKEND:-legacy}}"
MSI_MODE="${MSI_MODE:-${COVE_IO_TEST_MSI_MODE:-basic}}"
MRIF_RETARGET_LOOPS="${MRIF_RETARGET_LOOPS:-${COVE_IO_TEST_MRIF_RETARGET_LOOPS:-}}"
WAIT_SECS="${WAIT_SECS:-30}"
RUN_LOG="${RUN_LOG:-/tmp/lkvm-cove-io-vfio-msi.log}"
BIND_LOG="${BIND_LOG:-$RUN_LOG.bind}"
PROBE_LOG="${PROBE_LOG:-$RUN_LOG.probe}"
MSI_LOG="${MSI_LOG:-$RUN_LOG.run}"
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
	echo "probe log: $PROBE_LOG" >&2
	if [ -f "$PROBE_LOG" ]; then
		echo "----- probe log tail -----" >&2
		tail -80 "$PROBE_LOG" >&2 || true
	fi
	echo "MSI log: $MSI_LOG" >&2
	if [ -f "$MSI_LOG" ]; then
		echo "----- MSI log tail -----" >&2
		tail -80 "$MSI_LOG" >&2 || true
	fi
	echo "dmesg log: $DMESG_LOG" >&2
	if [ -f "$DMESG_LOG" ]; then
		echo "----- dmesg log tail -----" >&2
		tail -80 "$DMESG_LOG" >&2 || true
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

command -v grep >/dev/null 2>&1 || fail "missing command: grep"

rm -f "$RUN_LOG" "$BIND_LOG" "$PROBE_LOG" "$MSI_LOG" "$DMESG_LOG"
: >"$BIND_LOG"

if [ -x ./vfio-bind-pci.sh ]; then
	VFIO_ALLOW_COVE_IO_ISOLATED_MSI=${VFIO_ALLOW_COVE_IO_ISOLATED_MSI:-1} \
	VFIO_ALLOW_COVE_IO_MRIF=${VFIO_ALLOW_COVE_IO_MRIF:-1} \
	VFIO_ALLOW_UNSAFE_INTERRUPTS=${VFIO_ALLOW_UNSAFE_INTERRUPTS:-0} \
		./vfio-bind-pci.sh "$VFIO_BDF" >"$BIND_LOG" 2>&1 ||
		fail "failed to bind VFIO PCI device"
fi

export COVE_IO_TEST_VFIO_BACKEND="$VFIO_BACKEND"
export GUEST_VFIO_PCI="$VFIO_BDF"

export COVE_IO_TEST_PROBE=1
if [ "$MSI_MODE" = "retarget-stress" ]; then
	MRIF_RETARGET_LOOPS="${MRIF_RETARGET_LOOPS:-8}"
	GUEST_CPUS="${GUEST_CPUS:-4}"
elif [ "$MSI_MODE" = "retarget" ]; then
	MRIF_RETARGET_LOOPS="${MRIF_RETARGET_LOOPS:-1}"
	GUEST_CPUS="${GUEST_CPUS:-2}"
else
	MRIF_RETARGET_LOOPS="${MRIF_RETARGET_LOOPS:-1}"
	GUEST_CPUS="${GUEST_CPUS:-1}"
fi
case "$MRIF_RETARGET_LOOPS" in
''|*[!0-9]*)
	MRIF_RETARGET_LOOPS=1
	;;
esac
[ "$MRIF_RETARGET_LOOPS" -gt 0 ] || MRIF_RETARGET_LOOPS=1
export COVE_IO_TEST_MRIF_RETARGET_LOOPS="$MRIF_RETARGET_LOOPS"
GUEST_CPUS="$GUEST_CPUS" \
run_timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$PROBE_LOG" 2>&1 ||
	fail "deterministic VFIO COVE-IO probe run failed"
unset COVE_IO_TEST_PROBE

grep -q "COVE-IO probe vfio-irq-target-iid: allow" "$PROBE_LOG" ||
	fail "target-vCPU/IID positive IRQ probe did not pass"
grep -q "COVE-IO probe vfio-irq-target-device: allow" "$PROBE_LOG" ||
	fail "target device IRQ probe did not pass"
grep -q "COVE-IO probe vfio-irq-target-wrong-device: deny" "$PROBE_LOG" ||
	fail "wrong device IRQ probe was not denied"
grep -q "COVE-IO probe vfio-irq-target-wrong-iid: deny" "$PROBE_LOG" ||
	fail "wrong IMSIC IID IRQ probe was not denied"
grep -q "COVE-IO probe vfio-irq-target-wrong-vcpu: deny" "$PROBE_LOG" ||
	fail "wrong target-vCPU IRQ probe was not denied"
grep -q "COVE-IO probe vfio-irq-target-iid-unbound: deny" "$PROBE_LOG" ||
	fail "target-vCPU/IID IRQ probe remained authorized after unbind"

GUEST_COVE_IO_AUTORUN=edu-msi \
GUEST_COVE_IO_AUTORUN_POWEROFF=1 \
GUEST_COVE_IO_MSI_MODE=basic \
run_timeout -s KILL "$WAIT_SECS" ./run-guest-os.sh >"$MSI_LOG" 2>&1 || true

if command -v dmesg >/dev/null 2>&1; then
	dmesg >"$DMESG_LOG" 2>/dev/null || true
fi
cat "$BIND_LOG" "$PROBE_LOG" "$MSI_LOG" "$DMESG_LOG" >"$RUN_LOG"

if grep -q "allow_unsafe_interrupts" "$RUN_LOG"; then
	fail "VFIO test unexpectedly enabled allow_unsafe_interrupts"
fi

grep -q "cove_io_isolated_msi" "$RUN_LOG" ||
	fail "COVE-IO isolated MSI parameter was not enabled"

grep -q "cove_io_mrif" "$RUN_LOG" ||
	fail "COVE-IO MRIF parameter was not enabled"

grep -q "COVE-IO experimental MSI basic-mode remap enabled" "$RUN_LOG" ||
	fail "RISC-V IOMMU COVE-IO MSI remap was not initialized"

grep -q "COVE-IO experimental MRIF remap installed" "$RUN_LOG" ||
	fail "RISC-V IOMMU COVE-IO MRIF remap was not installed"

grep -q "using experimental COVE-IO RISC-V IOMMU MSI remap" "$RUN_LOG" ||
	fail "VFIO/iommufd did not consume the COVE-IO isolated MSI path"

if grep -qi "hiding .*MSI" "$MSI_LOG"; then
	fail "VFIO PCI MSI/MSI-X capability was hidden in CVM mode"
fi

grep -q "COVE-IO vfio-pci TDI" "$MSI_LOG" ||
	fail "VFIO PCI COVE-IO TDI was not started"

grep -q "COVE-IO dynamic MSI/MSI-X IRQ bind" "$MSI_LOG" ||
	fail "MSI/MSI-X was not dynamically bound; use a VFIO PCI device and nested guest driver that enables MSI or MSI-X"

grep -Eq "COVE-IO dynamic MSI/MSI-X IRQ bind: .*vcpu=[0-9]+ iid=[1-9][0-9]*" "$MSI_LOG" ||
	fail "MSI/MSI-X bind did not record a non-zero IMSIC IID"

case "$MSI_MODE" in
retarget|retarget-stress)
	grep -q "COVE-IO probe vfio-irq-mrif-retarget-vcpu0: allow" "$PROBE_LOG" ||
		fail "MRIF retarget probe did not authorize the initial vCPU0 target"
	grep -q "COVE-IO probe vfio-irq-mrif-retarget-old-vcpu: deny" "$PROBE_LOG" ||
		fail "MRIF retarget probe did not revoke the old vCPU target"
	grep -q "COVE-IO probe vfio-irq-mrif-retarget-vcpu1: allow" "$PROBE_LOG" ||
		fail "MRIF retarget probe did not authorize the new vCPU1 target"
	grep -q "COVE-IO probe vfio-irq-mrif-retarget-unbound: deny" "$PROBE_LOG" ||
		fail "MRIF retarget probe remained authorized after unbind"
	grep -Eq "COVE-IO experimental MRIF remap installed: .*vcpu=0" "$RUN_LOG" ||
		fail "MRIF remap was not installed for vCPU0"
	grep -Eq "COVE-IO experimental MRIF remap (installed|refreshed|pending): .*vcpu=1" "$RUN_LOG" ||
		fail "MRIF remap was not installed or safely deferred for vCPU1"
	grep -q "COVE-IO MRIF remap retarget" "$RUN_LOG" ||
		fail "MRIF retarget path did not report old-vCPU replacement"
	if grep -q "COVE-IO experimental MRIF remap pending: .*vcpu=1" "$RUN_LOG"; then
		grep -q "MSI blocked until IMSIC VS-file is available" "$RUN_LOG" ||
			fail "MRIF pending retarget did not block MSI while vCPU1 VS-file was unavailable"
	fi
	grep -Eq "COVE-IO dynamic MSI/MSI-X IRQ bind: .*vcpu=0 iid=[1-9][0-9]*" "$MSI_LOG" ||
		fail "MSI/MSI-X was not dynamically bound to vCPU0"
	if [ "$MSI_MODE" = "retarget-stress" ]; then
		last_loop=$((MRIF_RETARGET_LOOPS - 1))
		grep -q "COVE-IO probe vfio-irq-mrif-retarget-loop${last_loop}-old: deny" "$PROBE_LOG" ||
			fail "MRIF retarget stress did not deny the previous target on the last loop"
	fi
	;;
esac

echo "PASS: VFIO MSI/MSI-X dynamic COVE-IO IRQ bind mode=$MSI_MODE, device, and IID probes observed ($VFIO_BACKEND)"
