#!/bin/bash
set -euo pipefail

TOP_DIR=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

fail()
{
	echo "FAIL: $*" >&2
	exit 1
}

(
	source "$TOP_DIR/scripts/build-common.sh"
	HOST_BUILD_DIR="$work/host" GUEST_BUILD_DIR="$work/guest" build_init
	[ "$HOST_BUILD_DIR" = "$work/host" ]
	[ "$GUEST_BUILD_DIR" = "$work/guest" ]
)
if (
	source "$TOP_DIR/scripts/build-common.sh"
	HOST_BUILD_DIR="$work/same" GUEST_BUILD_DIR="$work/same/../same" build_init
) >"$work/path.log" 2>&1; then fail 'same output directory accepted'; fi

mkdir -p "$work/mock"
cat >"$work/mock/make" <<'MOCK'
#!/bin/bash
printf '%s\n' "$*" >> "$MAKE_LOG"
case " $* " in
*' oldconfig '*) read -r answer; exit "${MAKE_EXIT:-0}" ;;
*' defconfig '*) exit 99 ;;
esac
MOCK
chmod +x "$work/mock/make"
mkdir -p "$work/busybox"
printf 'CONFIG_STATIC=y\nCONFIG_CROSS_COMPILER_PREFIX="old-"\n' >"$work/busybox/.config"
export MAKE_LOG="$work/make.log"
(
	source "$TOP_DIR/scripts/build-common.sh"
	build_init
	BUSYBOX_DIR="$work/busybox" CROSS_COMPILE=test-riscv- PATH="$work/mock:$PATH" build_busybox
)
grep -q 'CONFIG_CROSS_COMPILER_PREFIX="test-riscv-"' "$work/busybox/.config" || fail 'compiler override not used'
if (
	source "$TOP_DIR/scripts/build-common.sh"
	build_init
	BUSYBOX_DIR="$work/busybox" MAKE_EXIT=7 PATH="$work/mock:$PATH" build_busybox
); then fail 'BusyBox configuration failure ignored'; fi

mkdir -p "$work/kernel"
printf 'CONFIG_LOCALVERSION="-keep-me"\nCONFIG_TUN=y\n' >"$work/kernel/.config"
(
	source "$TOP_DIR/scripts/build-common.sh"
	build_init
	PATH="$work/mock:$PATH" build_kernel "$work/kernel" host
)
grep -q 'CONFIG_LOCALVERSION="-keep-me"' "$work/kernel/.config" || fail 'custom kernel configuration lost'
grep -q 'CONFIG_KVM=y' "$work/kernel/.config" || fail 'required KVM config absent'
grep -q 'CONFIG_COVE_IO_GUEST=y' "$work/kernel/.config" || fail 'required COVG config absent'
grep -q '# CONFIG_VFIO_NOIOMMU is not set' "$work/kernel/.config" || fail 'unsafe config not disabled'

. "$TOP_DIR/scripts/cove-io-test-lib.sh"
cat >"$work/good.log" <<'LOG'
Debug: normal initialization
  Warning: KVM compatibility warning.
COVE-IO TDI 17 bound; waiting for guest COVG acceptance
COVE-IO TDI 18 bound; waiting for guest COVG acceptance
COVE-IO guest autorun: PASS covg
COVE-IO teardown verified: tdi=17 generation=1->3 state=free runtime=revoked
COVE-IO teardown verified: tdi=18 generation=1->3 state=free runtime=revoked
LOG
cove_io_check_guest_log "$work/good.log" covg
sed '/tdi=18 /d' "$work/good.log" >"$work/missing.log"
if cove_io_check_guest_log "$work/missing.log" covg; then fail 'partial teardown accepted'; fi
cp "$work/good.log" "$work/failed.log"
echo 'COVE-IO teardown MMIO revoke failed: tdi=17' >>"$work/failed.log"
if cove_io_check_guest_log "$work/failed.log" covg; then fail 'failure alongside PASS accepted'; fi
sed '/autorun: PASS/d' "$work/good.log" >"$work/incomplete.log"
if cove_io_check_guest_log "$work/incomplete.log" covg; then fail 'missing Guest completion accepted'; fi

mkdir -p "$work/msi"
cat >"$work/msi/vfio-bind-pci.sh" <<'MOCK'
#!/bin/sh
echo 'cove_io_isolated_msi=Y cove_io_imsic=Y'
MOCK
cat >"$work/mock/dmesg" <<'MOCK'
#!/bin/sh
echo 'COVE-IO experimental MSI basic-mode remap enabled'
echo 'COVE-IO IMSIC direct remap installed'
echo 'using experimental COVE-IO RISC-V IOMMU MSI remap'
MOCK
cat >"$work/mock/timeout-test" <<'MOCK'
#!/bin/sh
exec "$4"
MOCK
cat >"$work/msi/run-guest-os.sh" <<'MOCK'
#!/bin/sh
if [ "${COVE_IO_TEST_PROBE:-0}" = 1 ]; then
	for name in iid device; do
		echo "COVE-IO probe vfio-irq-target-$name: configured=allow runtime-before-accept=deny"
	done
	for name in wrong-device wrong-iid wrong-vcpu iid-unbound; do
		echo "COVE-IO probe vfio-irq-target-$name: configured=deny runtime-before-accept=deny"
	done
	exit 0
fi
echo 'COVE-IO TDI 17 bound; waiting for guest COVG acceptance'
echo 'COVE-IO vfio-pci TDI 17'
echo 'COVE-IO dynamic MSI/MSI-X IRQ bind: vcpu=0 iid=113'
echo 'COVE-IO guest edu-msi: enabled MSI'
echo 'COVE-IO guest edu-msi: request-ack verified'
echo 'COVE-IO guest autorun: PASS edu-msi'
echo 'COVE-IO teardown verified: tdi=17 generation=1->3 state=free runtime=revoked'
exit "${MOCK_MSI_STATUS:-0}"
MOCK
chmod +x "$work/msi"/*.sh "$work/mock/dmesg" "$work/mock/timeout-test"
(
	cd "$work/msi"
	PATH="$work/mock:$PATH" TIMEOUT_CMD="$work/mock/timeout-test" RUN_LOG="$work/msi.log" \
		sh "$TOP_DIR/scripts/cove-io-vfio-msi-test.sh"
) >"$work/msi-pass.log" 2>&1 || fail 'complete MSI fixture rejected'
if (
	cd "$work/msi"
	MOCK_MSI_STATUS=137 PATH="$work/mock:$PATH" TIMEOUT_CMD="$work/mock/timeout-test" RUN_LOG="$work/msi.log" \
		sh "$TOP_DIR/scripts/cove-io-vfio-msi-test.sh"
) >"$work/msi-timeout.log" 2>&1; then fail 'MSI timeout with PASS markers accepted'; fi
grep -q 'failed or timed out' "$work/msi-timeout.log" || fail 'wrong MSI timeout failure path'

mkdir -p "$work/lifecycle"
cat >"$work/mock/busybox" <<'MOCK'
#!/bin/sh
[ "$1" = timeout ] || exit 2
shift
exec timeout "$@"
MOCK
cat >"$work/lifecycle/run-guest-os.sh" <<'MOCK'
#!/bin/sh
tdi_id=100
case "$GUEST_LKVM_EXTRA_ARGS" in *independent-b*) tdi_id=${MOCK_SECOND_ID:-200} ;; esac
echo "COVE-IO TDI $tdi_id bound; waiting for guest COVG acceptance"
if [ -n "$COVE_IO_TEST_START_GATE" ]; then
	echo "COVE-IO test: waiting for start gate $COVE_IO_TEST_START_GATE"
	while [ ! -e "$COVE_IO_TEST_START_GATE" ]; do sleep 1; done
fi
echo 'CoVE-IO COVG selftest: PASS'
echo 'owner-scoped interfaces'
echo 'stop transaction revoked runtime access'
echo 'COVE-IO guest autorun: PASS covg'
echo "COVE-IO teardown verified: tdi=$tdi_id generation=1->3 state=free runtime=revoked"
MOCK
chmod +x "$work/mock/busybox" "$work/lifecycle/run-guest-os.sh"
(
	cd "$work/lifecycle"
	PATH="$work/mock:$PATH" STRESS_LOOPS=1 WAIT_SECS=20 LOG_DIR="$work/lifecycle-pass" \
		sh "$TOP_DIR/scripts/cove-io-lifecycle-stress-test.sh"
) >"$work/lifecycle-pass.log" 2>&1 || { cat "$work/lifecycle-pass.log"; fail 'lifecycle fixture rejected'; }
grep -q 'concurrent_bound_ids=distinct' "$work/lifecycle-pass.log" || fail 'no overlap result'
if (
	cd "$work/lifecycle"
	MOCK_SECOND_ID=100 PATH="$work/mock:$PATH" STRESS_LOOPS=1 WAIT_SECS=20 LOG_DIR="$work/lifecycle-fail" \
		sh "$TOP_DIR/scripts/cove-io-lifecycle-stress-test.sh"
) >"$work/lifecycle-fail.log" 2>&1; then fail 'overlapping TDI IDs accepted'; fi
grep -q 'simultaneously bound CVMs share TDI ID' "$work/lifecycle-fail.log" || fail 'wrong collision failure path'

mkdir -p "$work/root/bin"
cp /bin/true "$work/root/bin/busybox"
export ROOTFS_DIR="$work/root" ROOTFS_IMG="$work/host.img" GUEST_INITRD="$work/guest.cpio"
bash "$TOP_DIR/scripts/pack-rootfs.sh" --guest-only
cpio -it --quiet <"$GUEST_INITRD" >"$work/archive.list"
grep -qx 'scripts/cove-io-covg-test.sh' "$work/archive.list" || fail 'COVG script missing'
grep -qx 'scripts/cove-io-lifecycle-stress-test.sh' "$work/archive.list" || fail 'lifecycle script missing'
grep -qx 'scripts/cove-io-test-lib.sh' "$work/archive.list" || fail 'test library missing'
if grep -Eq '^(apps|lib|rootfs.cpio)(/|$)' "$work/archive.list"; then fail 'Host contents leaked into Guest archive'; fi
cpio -itv --quiet <"$GUEST_INITRD" >"$work/archive.details"
grep -Eq '^c.*5, *1 .*dev/console$' "$work/archive.details" || fail 'console is not a device'
original_hash=$(sha256sum "$GUEST_INITRD")
cat >"$work/mock/cpio" <<'MOCK'
#!/bin/sh
exit 42
MOCK
chmod +x "$work/mock/cpio"
if PATH="$work/mock:$PATH" bash "$TOP_DIR/scripts/pack-rootfs.sh" --guest-only; then
	fail 'cpio failure accepted'
fi
[ "$(sha256sum "$GUEST_INITRD")" = "$original_hash" ] || fail 'failed pack overwrote existing archive'
cat >"$work/mock/find" <<'MOCK'
#!/bin/sh
exit 43
MOCK
chmod +x "$work/mock/find"
rm "$work/mock/cpio"
if PATH="$work/mock:$PATH" bash "$TOP_DIR/scripts/pack-rootfs.sh" --guest-only; then
	fail 'find failure hidden by pipeline'
fi
[ "$(sha256sum "$GUEST_INITRD")" = "$original_hash" ] || fail 'failed find overwrote archive'
if ROOTFS_IMG="$ROOTFS_DIR/host.img" bash "$TOP_DIR/scripts/pack-rootfs.sh" --guest-only; then
	fail 'recursive output path accepted'
fi
echo 'PASS: paths, preserved configs, compiler/errors, MSI timeout, lifecycle gating/collision and rootfs failure safety'
