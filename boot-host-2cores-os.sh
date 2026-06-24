#!/bin/bash
set -e

TOP_DIR=$(cd "$(dirname "$0")" && pwd)
QEMU_DIR=${QEMU_DIR:-qemu}
OPENSBI_DIR=${OPENSBI_DIR:-opensbi}
BUILD_DIR=${BUILD_DIR:-build-riscv64}
ROOTFS_IMG=${ROOTFS_IMG:-rootfs_kvm_riscv64.img}
MACHINE=${MACHINE:-virt}
HOST_SSH_PORT=${HOST_SSH_PORT:-2555}
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

"$QEMU_DIR/build/qemu-system-riscv64" -cpu rv64 -smp 2,cores=1,threads=1,sockets=2 \
	-M "$MACHINE" -m 512M -nographic \
	-bios "$OPENSBI_DIR/build/platform/generic/firmware/fw_jump.bin" \
	-kernel "$BUILD_PATH/arch/riscv/boot/Image" \
	-initrd "$ROOTFS_IMG" \
	-append "root=/dev/ram rw console=ttyS0 earlycon=sbi" \
	-netdev user,id=net0,net=192.168.76.0/24,dhcpstart=192.168.76.9,hostfwd=tcp::"$HOST_SSH_PORT"-:22 \
	-device virtio-net-device,netdev=net0 \
	$QEMU_EXTRA_ARGS


#./qemu/build/qemu-system-riscv64 -cpu rv64 -smp 2,cores=1,threads=1,sockets=2 -M virt -m 512M -nographic -bios opensbi/build/platform/generic/firmware/fw_jump.bin -kernel ./build-riscv64/arch/riscv/boot/Image -initrd ./rootfs_kvm_riscv64.img -append "root=/dev/ram rw console=ttyS0 earlycon=sbi memmap=64M@0x10000000" -netdev user,id=net0,net=192.168.76.0/24,dhcpstart=192.168.76.9,hostfwd=tcp::2555-:22 -device virtio-net-device,netdev=net0
