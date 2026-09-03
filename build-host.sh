#!/bin/bash
set -e

TOP_DIR=$(pwd)

export ARCH=${ARCH:-riscv}
if [ -z "${CROSS_COMPILE:-}" ]; then
	if [ -n "${RISCV:-}" ]; then
		export CROSS_COMPILE="$RISCV/bin/riscv64-unknown-linux-gnu-"
	else
		export CROSS_COMPILE=riscv64-linux-gnu-
	fi
fi
export LIBFDT_DIR=${LIBFDT_DIR:-$TOP_DIR/dtc/lib64/lp64d}

LINUX_DIR=${LINUX_DIR:-linux}
BUILD_DIR=${BUILD_DIR:-build-riscv64}
KVMTOOL_DIR=${KVMTOOL_DIR:-kvmtool}
OPENSBI_DIR=${OPENSBI_DIR:-opensbi}
ROOTFS_IMG=${ROOTFS_IMG:-rootfs_kvm_riscv64.img}
case "$LINUX_DIR" in
	/*) ;;
	*) LINUX_DIR="$TOP_DIR/$LINUX_DIR" ;;
esac
case "$BUILD_DIR" in
	/*) BUILD_PATH="$BUILD_DIR" ;;
	*) BUILD_PATH="$TOP_DIR/$BUILD_DIR" ;;
esac
case "$KVMTOOL_DIR" in
	/*) ;;
	*) KVMTOOL_DIR="$TOP_DIR/$KVMTOOL_DIR" ;;
esac
case "$OPENSBI_DIR" in
	/*) ;;
	*) OPENSBI_DIR="$TOP_DIR/$OPENSBI_DIR" ;;
esac
CONFIG_FILE="$BUILD_PATH/.config"

set_config_y()
{
	local opt="$1"

	if grep -q "^${opt}=" "$CONFIG_FILE"; then
		sed -i "s|^${opt}=.*$|${opt}=y|" "$CONFIG_FILE"
	elif grep -q "^# ${opt} is not set$" "$CONFIG_FILE"; then
		sed -i "s|^# ${opt} is not set$|${opt}=y|" "$CONFIG_FILE"
	else
		printf '%s=y\n' "$opt" >> "$CONFIG_FILE"
	fi
}

unset_config()
{
	local opt="$1"

	if grep -q "^${opt}=" "$CONFIG_FILE"; then
		sed -i "s|^${opt}=.*$|# ${opt} is not set|" "$CONFIG_FILE"
	elif ! grep -q "^# ${opt} is not set$" "$CONFIG_FILE"; then
		printf '# %s is not set\n' "$opt" >> "$CONFIG_FILE"
	fi
}

mkdir -p "$BUILD_PATH"
#make clean
make -C "$LINUX_DIR" O="$BUILD_PATH" defconfig
sed -i 's|.*CONFIG_INITRAMFS_SOURCE.*$|CONFIG_INITRAMFS_SOURCE=""|' "$CONFIG_FILE"
set_config_y CONFIG_NET_9P_VIRTIO
set_config_y CONFIG_VIRTIO_NET
set_config_y CONFIG_DMA_RESTRICTED_POOL
set_config_y CONFIG_KVM
set_config_y CONFIG_IOMMU_SUPPORT
set_config_y CONFIG_RISCV_IOMMU
set_config_y CONFIG_COVE_IO_GUEST
set_config_y CONFIG_VFIO
set_config_y CONFIG_VFIO_PCI
unset_config CONFIG_VFIO_NOIOMMU
# using rdcycle in user space
unset_config CONFIG_RISCV_PMU_SBI
make -C "$LINUX_DIR" O="$BUILD_PATH" -j $(nproc)

#compile kvmtool
cd "$KVMTOOL_DIR"
make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" LIBFDT_DIR="$LIBFDT_DIR" clean
make ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" LIBFDT_DIR="$LIBFDT_DIR" lkvm-static -j $(nproc)
${CROSS_COMPILE}strip lkvm-static
cd "$TOP_DIR"

#build a root FS for our OS
#cd busybox-1.33.1
#make defconfig
#sed -i 's|^CONFIG_CROSS_COMPILER_PREFIX=.*$|CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-"|' .config
#sed -i 's|.*CONFIG_STATIC.*|CONFIG_STATIC=y|' .config
rm -rf ./busybox-1.33.1/_install
#cd ..
sed -i 's|^CONFIG_TC=.*$|# CONFIG_TC is not set|' busybox-1.33.1/.config
sed -i 's|^CONFIG_FEATURE_TC_INGRESS=.*$|# CONFIG_FEATURE_TC_INGRESS is not set|' busybox-1.33.1/.config
yes "" | make -C busybox-1.33.1 oldconfig
make -C busybox-1.33.1 install
mkdir -p busybox-1.33.1/_install/etc/init.d
mkdir -p busybox-1.33.1/_install/dev
mkdir -p busybox-1.33.1/_install/proc
mkdir -p busybox-1.33.1/_install/sys
mkdir -p busybox-1.33.1/_install/apps
mkdir -p busybox-1.33.1/_install/scripts
ln -sf /sbin/init busybox-1.33.1/_install/init
cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
cp -f "$KVMTOOL_DIR/lkvm-static" busybox-1.33.1/_install/apps
cp -f "$BUILD_PATH/arch/riscv/boot/Image" busybox-1.33.1/_install/apps
cp -f ./run-guest-os.sh busybox-1.33.1/_install
cp -f ./vfio-bind-pci.sh busybox-1.33.1/_install
cp -f ./scripts/cove-io-host-autorun.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-guest-autorun.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-covg-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-lifecycle-stress-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-lazy-fault-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-multi-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-msi-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-pri-test.sh busybox-1.33.1/_install/scripts
chmod +x busybox-1.33.1/_install/scripts/cove-io-*.sh

mkdir -p busybox-1.33.1/_install/tmp
mkdir -p busybox-1.33.1/_install/dev/pts
mkdir -p busybox-1.33.1/_install/lib
mkdir -p busybox-1.33.1/_install/etc/network
mkdir -p busybox-1.33.1/_install/etc/network/if-pre-up.d
mkdir -p busybox-1.33.1/_install/etc/network/if-up.d
mkdir -p busybox-1.33.1/_install/etc/network/if-down.d
mkdir -p busybox-1.33.1/_install/etc/network/if-post-down.d
mkdir -p busybox-1.33.1/_install/etc/dropbear
mkdir -p busybox-1.33.1/_install/var
mkdir -p busybox-1.33.1/_install/var/run
cp -rf dependencies/ldd busybox-1.33.1/_install/bin/
cp -rf dependencies/db/* busybox-1.33.1/_install/bin/
cp -rf dependencies/lib/* busybox-1.33.1/_install/lib/
cp -rf dependencies/etc/* busybox-1.33.1/_install/etc/
cp -f ./howto/configs/busybox/inittab busybox-1.33.1/_install/etc/inittab

GUEST_INITRD="$TOP_DIR/rootfs_guest_riscv64.cpio"
rm -f "$GUEST_INITRD"
rm -f busybox-1.33.1/_install/rootfs.cpio
(
	cd busybox-1.33.1/_install
	find ./ \( \
		-path './rootfs.cpio' -o \
		-path './apps' -o -path './apps/*' -o \
		-path './lib' -o -path './lib/*' \
		\) -prune -o -print | cpio -o -H newc > "$GUEST_INITRD"
)
cp -f "$GUEST_INITRD" busybox-1.33.1/_install/rootfs.cpio
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > "$TOP_DIR/$ROOTFS_IMG" | fakeroot; cd -

#compile Linux with RISC-V KVM support
sed -i 's|.*CONFIG_INITRAMFS_SOURCE=.*$|CONFIG_INITRAMFS_SOURCE=""|' "$CONFIG_FILE"
# using rdcycle in user space
unset_config CONFIG_RISCV_PMU_SBI
# Compile Host Kernel with TUN/TAP support for Guest virtual network
set_config_y CONFIG_TUN
set_config_y CONFIG_KVM
set_config_y CONFIG_IOMMU_SUPPORT
set_config_y CONFIG_RISCV_IOMMU
set_config_y CONFIG_VFIO
set_config_y CONFIG_VFIO_PCI
unset_config CONFIG_VFIO_NOIOMMU
make -C "$LINUX_DIR" O="$BUILD_PATH" -j $(nproc)


#compile opensbi
cd "$OPENSBI_DIR"
make -j $(nproc) PLATFORM=generic
cd "$TOP_DIR"
