#!/bin/bash
set -e

TOP_DIR=$(cd "$(dirname "$0")" && pwd)
QEMU_DIR=${QEMU_DIR:-qemu}
OPENSBI_DIR=${OPENSBI_DIR:-opensbi}
BUILD_DIR=${BUILD_DIR:-build-riscv64}
ROOTFS_IMG=${ROOTFS_IMG:-rootfs_kvm_riscv64.img}
MACHINE=${MACHINE:-virt,aia=aplic-imsic,aia-guests=7}
QEMU_EXTRA_ARGS=${QEMU_EXTRA_ARGS:-}
HOST_RISCV_IOMMU=${HOST_RISCV_IOMMU:-0}
HOST_PCI_TEST_DEVICE=${HOST_PCI_TEST_DEVICE:-}
HOST_PCI_TEST_DEVICES=${HOST_PCI_TEST_DEVICES:-}
HOST_APPEND_EXTRA=${HOST_APPEND_EXTRA:-}
HOST_COVE_IO_TEST=${HOST_COVE_IO_TEST:-}
HOST_COVE_IO_TEST_POWER_OFF=${HOST_COVE_IO_TEST_POWER_OFF:-1}
HOST_COVE_IO_TEST_WAIT_SECS=${HOST_COVE_IO_TEST_WAIT_SECS:-}
HOST_COVE_IO_VFIO_BDF=${HOST_COVE_IO_VFIO_BDF:-}
HOST_COVE_IO_VFIO_BACKEND=${HOST_COVE_IO_VFIO_BACKEND:-}
HOST_COVE_IO_PRI_COUNT=${HOST_COVE_IO_PRI_COUNT:-}
HOST_COVE_IO_PRI_OVERFLOW_COUNT=${HOST_COVE_IO_PRI_OVERFLOW_COUNT:-}
HOST_COVE_IO_MRIF_RETARGET_LOOPS=${HOST_COVE_IO_MRIF_RETARGET_LOOPS:-}
HOST_IOMMU_ARGS=
HOST_TEST_DEVICE_ARGS=
HOST_COVE_IO_TEST_ARGS=

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

if [ "$HOST_RISCV_IOMMU" != "0" ]; then
	HOST_IOMMU_ARGS="-device riscv-iommu-pci"
fi

if [ -n "$HOST_PCI_TEST_DEVICES" ]; then
	for dev in $HOST_PCI_TEST_DEVICES; do
		HOST_TEST_DEVICE_ARGS="$HOST_TEST_DEVICE_ARGS -device $dev"
	done
elif [ -n "$HOST_PCI_TEST_DEVICE" ]; then
	HOST_TEST_DEVICE_ARGS="-device $HOST_PCI_TEST_DEVICE"
fi

if [ -n "$HOST_COVE_IO_TEST" ]; then
	HOST_COVE_IO_TEST_ARGS="cove_io_host_test=$HOST_COVE_IO_TEST cove_io_host_poweroff=$HOST_COVE_IO_TEST_POWER_OFF"
	case "$HOST_COVE_IO_TEST" in
		vfio-msi|msi|vfio-msi-retarget|msi-retarget|vfio-msi-retarget-stress|msi-retarget-stress)
			HOST_APPEND_EXTRA="$HOST_APPEND_EXTRA iommu.cove_io_isolated_msi=1 iommu.cove_io_mrif=1"
			;;
		vfio-multi|multi)
			: "${HOST_COVE_IO_VFIO_BACKEND:=iommufd}"
			;;
		vfio-pri|pri|vfio-pri-stress|pri-stress|vfio-pri-pasid|pri-pasid|vfio-pri-extended|pri-extended|vfio-pri-deny|pri-deny|vfio-pri-cancel|pri-cancel|vfio-pri-stop|pri-stop|vfio-pri-overflow|pri-overflow)
			HOST_APPEND_EXTRA="$HOST_APPEND_EXTRA iommu.cove_io_pri=1 iommu.cove_io_isolated_msi=1 iommu.cove_io_mrif=1"
			;;
	esac
	if [ -n "$HOST_COVE_IO_TEST_WAIT_SECS" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_test_wait=$HOST_COVE_IO_TEST_WAIT_SECS"
	fi
	if [ -n "$HOST_COVE_IO_VFIO_BDF" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_vfio_bdf=$HOST_COVE_IO_VFIO_BDF"
	fi
	if [ -n "$HOST_COVE_IO_VFIO_BACKEND" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_vfio_backend=$HOST_COVE_IO_VFIO_BACKEND"
	fi
	if [ -n "$HOST_COVE_IO_PRI_COUNT" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_pri_count=$HOST_COVE_IO_PRI_COUNT"
	fi
	if [ -n "$HOST_COVE_IO_PRI_OVERFLOW_COUNT" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_pri_overflow_count=$HOST_COVE_IO_PRI_OVERFLOW_COUNT"
	fi
	if [ -n "$HOST_COVE_IO_MRIF_RETARGET_LOOPS" ]; then
		HOST_COVE_IO_TEST_ARGS="$HOST_COVE_IO_TEST_ARGS cove_io_mrif_retarget_loops=$HOST_COVE_IO_MRIF_RETARGET_LOOPS"
	fi
fi

"$QEMU_DIR/build/qemu-system-riscv64" -cpu rv64 -M "$MACHINE" -m 1024M -nographic \
	-bios "$OPENSBI_DIR/build/platform/generic/firmware/fw_jump.bin" \
	-kernel "$BUILD_PATH/arch/riscv/boot/Image" \
	-initrd "$ROOTFS_IMG" \
	-append "root=/dev/ram rw console=ttyS0 earlycon=sbi $HOST_COVE_IO_TEST_ARGS $HOST_APPEND_EXTRA" \
	$HOST_IOMMU_ARGS \
	$HOST_TEST_DEVICE_ARGS \
	$QEMU_EXTRA_ARGS

#./qemu/build/qemu-system-riscv64 -cpu rv64 -smp 2,cores=1,threads=1,sockets=2 -M virt -m 1024M -nographic -bios opensbi/build/platform/generic/firmware/fw_jump.bin -kernel ./build-riscv64/arch/riscv/boot/Image -initrd ./rootfs_kvm_riscv64.img -append "root=/dev/ram rw console=ttyS0 earlycon=sbi"
