#!/bin/bash

export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
export LIBFDT_DIR=`pwd`/dtc/lib64/lp64d


#git clone https://github.com/riscv/opensbi.git
cd opensbi
make -j $(nproc) PLATFORM=generic
cd ..

#compile Linux with RISC-V KVM support
#git clone https://github.com/kvm-riscv/linux.git
mkdir build-riscv64
#make -C linux O=`pwd`/build-riscv64 defconfig
#echo "CONFIG_KVM=y" >> ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

#build kvmtool
#git clone https://github.com/kvm-riscv/kvmtool.git
cd kvmtool
make lkvm-static -j $(nproc)
${CROSS_COMPILE}strip lkvm-static
cd ..

#build a root FS for our OS
#make -C busybox-1.33.1 install
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
#cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
cp -f ./run-guest-os.sh busybox-1.33.1/_install/apps
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.img; cd -

