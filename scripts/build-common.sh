#!/bin/bash

TOP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

build_path()
{
	case "$1" in
		/*) realpath -m -- "$1" ;;
		*) realpath -m -- "$TOP_DIR/$1" ;;
	esac
}

build_init()
{
	export ARCH=${ARCH:-riscv}
	if [ -z "${CROSS_COMPILE:-}" ]; then
		if [ -n "${RISCV:-}" ]; then
			CROSS_COMPILE="$RISCV/bin/riscv64-unknown-linux-gnu-"
		else
			CROSS_COMPILE=riscv64-linux-gnu-
		fi
	fi
	export CROSS_COMPILE
	JOBS=${JOBS:-16}
	case "$JOBS" in ''|*[!0-9]*|0) echo 'JOBS must be positive' >&2; return 1 ;; esac
	LINUX_DIR=$(build_path "${LINUX_DIR:-linux}")
	HOST_BUILD_DIR=$(build_path "${HOST_BUILD_DIR:-${BUILD_DIR:-build-riscv64}}")
	GUEST_BUILD_DIR=$(build_path "${GUEST_BUILD_DIR:-build-riscv64-guest}")
	KVMTOOL_DIR=$(build_path "${KVMTOOL_DIR:-kvmtool}")
	OPENSBI_DIR=$(build_path "${OPENSBI_DIR:-opensbi}")
	QEMU_DIR=$(build_path "${QEMU_DIR:-qemu}")
	BUSYBOX_DIR=$(build_path "${BUSYBOX_DIR:-busybox-1.33.1}")
	ROOTFS_DIR=$(build_path "${ROOTFS_DIR:-$BUSYBOX_DIR/_install}")
	ROOTFS_IMG=$(build_path "${ROOTFS_IMG:-rootfs_kvm_riscv64.img}")
	GUEST_INITRD=$(build_path "${GUEST_INITRD:-rootfs_guest_riscv64.cpio}")
	BUILD_MANIFEST=$(build_path "${BUILD_MANIFEST:-$ROOTFS_IMG.manifest}")
	LIBFDT_DIR=$(build_path "${LIBFDT_DIR:-build-libfdt}")
	export LINUX_DIR HOST_BUILD_DIR GUEST_BUILD_DIR KVMTOOL_DIR OPENSBI_DIR
	export QEMU_DIR BUSYBOX_DIR ROOTFS_DIR ROOTFS_IMG GUEST_INITRD BUILD_MANIFEST LIBFDT_DIR
	if [ "$HOST_BUILD_DIR" = "$GUEST_BUILD_DIR" ] ||
	   [ "$HOST_BUILD_DIR" = "$LINUX_DIR" ] || [ "$GUEST_BUILD_DIR" = "$LINUX_DIR" ]; then
		echo 'Host, Guest and Linux source directories must be distinct' >&2
		return 1
	fi
}

build_kernel()
{
	local output_dir="$1" role="$2" option
	mkdir -p "$output_dir"
	if [ ! -f "$output_dir/.config" ]; then
		make -C "$LINUX_DIR" O="$output_dir" defconfig
	fi
	for option in NET_9P_VIRTIO VIRTIO_NET DMA_RESTRICTED_POOL KVM \
		IOMMU_SUPPORT RISCV_IOMMU COVE_IO_GUEST COVE_IO_NIC E1000E IOMMUFD VFIO VFIO_PCI \
		VFIO_GROUP VFIO_CONTAINER; do
		"$LINUX_DIR/scripts/config" --file "$output_dir/.config" -e "$option"
	done
	"$LINUX_DIR/scripts/config" --file "$output_dir/.config" \
		--set-str INITRAMFS_SOURCE '' -d VFIO_NOIOMMU -d RISCV_PMU_SBI
	if [ "$role" = guest ]; then
		"$LINUX_DIR/scripts/config" --file "$output_dir/.config" -e COVE_IO_EDU_TEST
	fi
	if [ "$role" = host ]; then
		"$LINUX_DIR/scripts/config" --file "$output_dir/.config" -e TUN
		if [ "${COVE_IO_FAULT_INJECTION:-0}" = 1 ]; then
			"$LINUX_DIR/scripts/config" --file "$output_dir/.config" -e FAULT_INJECTION
		fi
	fi
	make -C "$LINUX_DIR" O="$output_dir" olddefconfig
	make -C "$LINUX_DIR" O="$output_dir" -j "$JOBS" Image
}

build_busybox()
{
	if [ ! -f "$BUSYBOX_DIR/.config" ]; then
		make -C "$BUSYBOX_DIR" defconfig
	fi
	"$LINUX_DIR/scripts/config" --file "$BUSYBOX_DIR/.config" \
		--set-str CROSS_COMPILER_PREFIX "$CROSS_COMPILE" \
		-e STATIC -d TC -d FEATURE_TC_INGRESS
	make -C "$BUSYBOX_DIR" oldconfig < <(yes '') || return $?
	make -C "$BUSYBOX_DIR" -j "$JOBS" CONFIG_PREFIX="$ROOTFS_DIR" install
}
