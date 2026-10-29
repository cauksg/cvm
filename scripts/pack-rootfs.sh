#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/build-common.sh"
build_init
cd "$TOP_DIR"

mode=${1:-host}
case "$mode" in host|--guest-only) ;; *) echo "Usage: $0 [--guest-only]" >&2; exit 2 ;; esac
for tool in cpio fakeroot sha256sum git; do
	command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[ -x "$ROOTFS_DIR/bin/busybox" ] || { echo "Build/install BusyBox first: $ROOTFS_DIR" >&2; exit 1; }
if [ "$mode" = host ]; then
	for artifact in "$GUEST_BUILD_DIR/arch/riscv/boot/Image" "$GUEST_BUILD_DIR/.config" \
		"$HOST_BUILD_DIR/arch/riscv/boot/Image" "$HOST_BUILD_DIR/.config" \
		"$KVMTOOL_DIR/lkvm-static" "$OPENSBI_DIR/build/platform/generic/firmware/fw_jump.bin" \
		"$QEMU_DIR/build/qemu-system-riscv64" "$BUSYBOX_DIR/.config"; do
		[ -f "$artifact" ] || { echo "Missing artifact: $artifact" >&2; exit 1; }
	done
fi
if [ "$ROOTFS_IMG" = "$GUEST_INITRD" ] || [ "$BUILD_MANIFEST" = "$ROOTFS_IMG" ] ||
   [ "$BUILD_MANIFEST" = "$GUEST_INITRD" ]; then
	echo 'Image, initrd and manifest paths must be distinct' >&2
	exit 1
fi
for output in "$ROOTFS_IMG" "$GUEST_INITRD" "$BUILD_MANIFEST"; do
	case "$output" in
		"$ROOTFS_DIR"|"$ROOTFS_DIR"/*)
			echo 'Outputs must be outside the input rootfs tree' >&2
			exit 1
			;;
	esac
done
mkdir -p "$(dirname "$GUEST_INITRD")" "$(dirname "$ROOTFS_IMG")" "$(dirname "$BUILD_MANIFEST")"
work=$(mktemp -d "$(dirname "$ROOTFS_IMG")/.xs-cvm-pack.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
stage="$work/tree"
mkdir -p "$stage"
cp -a "$ROOTFS_DIR/." "$stage/"
rm -rf -- "$stage/apps" "$stage/lib" "$stage/scripts" "$stage/rootfs.cpio"
mkdir -p "$stage/etc/init.d" "$stage/dev/pts" "$stage/proc" "$stage/sys" "$stage/tmp" "$stage/scripts"
ln -sfn /sbin/init "$stage/init"
cp "$TOP_DIR/howto/configs/busybox/rcS" "$stage/etc/init.d/rcS"
for config in fstab motd inittab; do
	cp "$TOP_DIR/howto/configs/busybox/$config" "$stage/etc/$config"
done
cp "$TOP_DIR/run-guest-os.sh" "$TOP_DIR/vfio-bind-pci.sh" "$stage/"
cp "$TOP_DIR"/scripts/cove-io-*.sh "$stage/scripts/"
chmod +x "$stage/etc/init.d/rcS" "$stage"/*.sh "$stage/scripts"/*.sh

pack_tree()
{
	local output="$1"
	fakeroot bash -euo pipefail -c '
		cd "$1"
		rm -f dev/console
		mknod dev/console c 5 1
		LC_ALL=C find . -print0 | LC_ALL=C sort -z |
			cpio --null -o -H newc --owner=0:0 --reproducible > "$2"
	' bash "$stage" "$output"
}

pack_tree "$work/guest.cpio"
if [ "$mode" = host ]; then
	mkdir -p "$stage/apps" "$stage/lib" "$stage/etc/dropbear" "$stage/var/run"
	cp "$KVMTOOL_DIR/lkvm-static" "$stage/apps/lkvm-static"
	cp "$GUEST_BUILD_DIR/arch/riscv/boot/Image" "$stage/apps/Image"
	cp "$work/guest.cpio" "$stage/rootfs.cpio"
	cp -a "$TOP_DIR/dependencies/lib/." "$stage/lib/"
	cp -a "$TOP_DIR/dependencies/db/." "$stage/bin/"
	cp "$TOP_DIR/dependencies/ldd" "$stage/bin/"
	cp -a "$TOP_DIR/dependencies/etc/." "$stage/etc/"
	cp "$TOP_DIR/howto/configs/busybox/inittab" "$stage/etc/inittab"
	pack_tree "$work/host.cpio"
	{
		printf 'format=xs-cvm-build-v1\ncreated_utc=%s\n' "$(date -u +%FT%TZ)"
		printf 'source_revision=%s\n' "$(git -C "$TOP_DIR" rev-parse HEAD)"
		printf 'tracked_diff_sha256=%s\n' "$(git -C "$TOP_DIR" diff --binary HEAD | sha256sum | cut -d ' ' -f 1)"
		printf 'source_status_begin\n'
		git -C "$TOP_DIR" status --porcelain --untracked-files=normal
		printf 'source_status_end\nsubmodules_begin\n'
		git -C "$TOP_DIR" submodule status
		printf 'submodules_end\n'
		printf 'host_build=%s\nguest_build=%s\n' "$HOST_BUILD_DIR" "$GUEST_BUILD_DIR"
		printf 'cross_compile=%s\n' "$CROSS_COMPILE"
		if command -v "${CROSS_COMPILE}gcc" >/dev/null; then
			printf 'compiler=%s\n' "$("${CROSS_COMPILE}gcc" --version | sed -n '1p')"
		fi
		printf 'provenance=packaging snapshot; existing binaries are not asserted to match source\n'
		printf 'sha256_begin\n'
		for artifact in "$GUEST_BUILD_DIR/arch/riscv/boot/Image" "$GUEST_BUILD_DIR/.config" \
			"$HOST_BUILD_DIR/arch/riscv/boot/Image" "$HOST_BUILD_DIR/.config" \
			"$KVMTOOL_DIR/lkvm-static" "$OPENSBI_DIR/build/platform/generic/firmware/fw_jump.bin" \
			"$QEMU_DIR/build/qemu-system-riscv64" "$BUSYBOX_DIR/.config" \
			"$ROOTFS_DIR/bin/busybox" "$TOP_DIR/run-guest-os.sh" "$TOP_DIR"/scripts/cove-io-*.sh; do
			sha256sum "$artifact"
		done
		printf '%s  %s\n' "$(sha256sum "$work/guest.cpio" | cut -d ' ' -f 1)" "$GUEST_INITRD"
		printf '%s  %s\n' "$(sha256sum "$work/host.cpio" | cut -d ' ' -f 1)" "$ROOTFS_IMG"
		printf 'sha256_end\n'
	} > "$work/manifest"
	mv -f "$work/host.cpio" "$ROOTFS_IMG"
	mv -f "$work/manifest" "$BUILD_MANIFEST"
fi
mv -f "$work/guest.cpio" "$GUEST_INITRD"
printf 'Packed %s\n' "$GUEST_INITRD"
if [ "$mode" = host ]; then printf 'Packed %s\nManifest %s\n' "$ROOTFS_IMG" "$BUILD_MANIFEST"; fi
