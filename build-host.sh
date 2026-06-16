#!/bin/bash
set -e

export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
export LIBFDT_DIR=`pwd`/dtc/lib64/lp64d

cd build-riscv64
#make clean
cd ..
make -C linux O=`pwd`/build-riscv64 defconfig
sed -i 's|.*CONFIG_INITRAMFS_SOURCE.*$|CONFIG_INITRAMFS_SOURCE=""|' ./build-riscv64/.config
sed -i 's|.*CONFIG_NET_9P_VIRTIO.*$|CONFIG_NET_9P_VIRTIO=y|' ./build-riscv64/.config
sed -i 's|.*CONFIG_VIRTIO_NET.*$|CONFIG_VIRTIO_NET=y|' ./build-riscv64/.config
sed -i 's|.*CONFIG_DMA_RESTRICTED_POOL.*$|CONFIG_DMA_RESTRICTED_POOL=y|' ./build-riscv64/.config
# using rdcycle in user space
sed -i 's|.*CONFIG_RISCV_PMU_SBI.*$|# CONFIG_RISCV_PMU_SBI is not set|' ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

#compile kvmtool
cd kvmtool
make clean
make lkvm-static -j $(nproc)
${CROSS_COMPILE}strip lkvm-static
cd ..

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
ln -sf /sbin/init busybox-1.33.1/_install/init
cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
cp -f ./kvmtool/lkvm-static busybox-1.33.1/_install/apps
cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
cp -f ./run-guest-os.sh busybox-1.33.1/_install

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
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.img | fakeroot; cd -

#compile Linux with RISC-V KVM support
sed -i 's|.*CONFIG_INITRAMFS_SOURCE=.*$|CONFIG_INITRAMFS_SOURCE=""|' ./build-riscv64/.config
# using rdcycle in user space
sed -i 's|.*CONFIG_RISCV_PMU_SBI.*$|# CONFIG_RISCV_PMU_SBI is not set|' ./build-riscv64/.config
# Compile Host Kernel with TUN/TAP support for Guest virtual network
sed -i 's|# CONFIG_TUN is not set.*$|CONFIG_TUN=y|' ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)


#compile opensbi
cd opensbi
make -j $(nproc) PLATFORM=generic
cd ..
