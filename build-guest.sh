#!/bin/bash


export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-

cd busybox-1.33.1
make defconfig
echo 'CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-' >> .config
echo 'CONFIG_STATIC=y' >> .config
cd ..
make -C busybox-1.33.1 install
mkdir -p busybox-1.33.1/_install/etc/init.d
mkdir -p busybox-1.33.1/_install/dev
mkdir -p busybox-1.33.1/_install/proc
mkdir -p busybox-1.33.1/_install/sys
#mkdir -p busybox-1.33.1/_install/apps
ln -sf /sbin/init busybox-1.33.1/_install/init
cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
#cp -f ./kvmtool/lkvm-static busybox-1.33.1/_install/apps
#cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.cpio; cd -


#mkdir build-riscv64
echo "CONFIG_INITRAMFS_SOURCE=\"./rootfs_kvm_riscv64.cpio\"" >> ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

