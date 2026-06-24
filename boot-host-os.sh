#!/bin/bash
set -e

TOP_DIR=$(cd "$(dirname "$0")" && pwd)
QEMU_DIR=${QEMU_DIR:-qemu}
OPENSBI_DIR=${OPENSBI_DIR:-opensbi}
BUILD_DIR=${BUILD_DIR:-build-riscv64}
ROOTFS_IMG=${ROOTFS_IMG:-rootfs_kvm_riscv64.img}
MACHINE=${MACHINE:-virt,aia=aplic-imsic,aia-guests=7}
QEMU_EXTRA_ARGS=${QEMU_EXTRA_ARGS:-}

case "$QEMU_DIR" in
	/*) ;;
	*) QEMU_DIR="$TOP_DIR/$QEMU_DIR" ;;
esac
case "$OPENSBI_DIR" in
	/*) ;;
	*) OPENSBI_DIR="$TOP_DIR/$OPENSBI_DIR" ;;
esac
case "$BUILD_DIR" in
	/*) BUILD_PATH="$BUILD_DIR" ;;
	*) BUILD_PATH="$TOP_DIR/$BUILD_DIR" ;;
esac
case "$ROOTFS_IMG" in
	/*) ;;
	*) ROOTFS_IMG="$TOP_DIR/$ROOTFS_IMG" ;;
esac

"$QEMU_DIR/build/qemu-system-riscv64" -cpu rv64 -M "$MACHINE" -m 1024M -nographic \
	-bios "$OPENSBI_DIR/build/platform/generic/firmware/fw_jump.bin" \
	-kernel "$BUILD_PATH/arch/riscv/boot/Image" \
	-initrd "$ROOTFS_IMG" \
	-append "root=/dev/ram rw console=ttyS0 earlycon=sbi" \
	$QEMU_EXTRA_ARGS

#./qemu/build/qemu-system-riscv64 -cpu rv64 -smp 2,cores=1,threads=1,sockets=2 -M virt -m 1024M -nographic -bios opensbi/build/platform/generic/firmware/fw_jump.bin -kernel ./build-riscv64/arch/riscv/boot/Image -initrd ./rootfs_kvm_riscv64.img -append "root=/dev/ram rw console=ttyS0 earlycon=sbi"
