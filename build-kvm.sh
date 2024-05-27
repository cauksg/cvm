#!/bin/bash
git submodule --init update

#git clone https://github.com/riscv/opensbi.git
cd opensbi
export CROSS_COMPILE=riscv64-linux-gnu-
make -j $(nproc) PLATFORM=generic
cd ..

#compile Linux with RISC-V KVM support
#git clone https://github.com/kvm-riscv/linux.git
mkdir build-riscv64
export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
make -C linux O=`pwd`/build-riscv64 defconfig
echo "CONFIG_KVM=y" >> ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

#KVM tool depends on dtc at runtime. So we first compile it.
#git clone git://git.kernel.org/pub/scm/utils/dtc/dtc.git
cd dtc
export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
export CC="${CROSS_COMPILE}gcc -mabi=lp64d -march=rv64gc"
TRIPLET=$($CC -dumpmachine)
SYSROOT=$($CC -print-sysroot)
make libfdt
make EXTRA_CFLAGS="-mabi=lp64d" DESTDIR=$SYSROOT PREFIX=`pwd` LIBDIR=`pwd`/lib64/lp64d install-lib install-includes
cd ..

#build kvmtool
#git clone https://github.com/kvm-riscv/kvmtool.git
export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
export LIBFDT_DIR="`pwd`/dtc/lib64/lp64d"
cd kvmtool
make lkvm-static -j $(nproc)
${CROSS_COMPILE}strip lkvm-static
cd ..

#build a root FS for our OS
export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-
wget https://busybox.net/downloads/busybox-1.33.1.tar.bz2
tar -C . -xvf busybox-1.33.1.tar.bz2
make -C busybox-1.33.1 defconfig
cd busybox-1.33.1
echo 'CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-' >> .config
echo 'CONFIG_STATIC=y' >> .config
cd ..
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
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.img; cd -

