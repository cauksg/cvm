#!/bin/bash
git submodule update --init --recursive

#Compiling the toolchain
mkdir ~/riscv
export RISCV=~/riscv
sudo apt install autoconf automake autotools-dev curl libmpc-dev libmpfr-dev libgmp-dev \
                 libusb-1.0-0-dev gawk build-essential bison flex texinfo gperf libtool \
                 patchutils bc zlib1g-dev device-tree-compiler pkg-config libexpat-dev  \
                 libncurses5-dev libncursesw5-dev git
#git clone https://github.com/riscv/riscv-gnu-toolchain
cd riscv-gnu-toolchain
./configure --prefix=$RISCV --enable-multilib
make -j $(nproc)
make -j $(nproc) linux
echo 'export RISCV=~/riscv/bin' >> ~/.bashrc
echo 'export PATH="$PATH:$RISCV"' >> ~/.bashrc
source ~/.bashrc
cd ..

#Compiling QEMU for RSIC-V with virtualization extensions
sudo apt install ninja-build pkg-config libglib2.0-dev libpixman-1-dev libtirpc-dev unzip
#git clone https://github.com/kvm-riscv/qemu.git
cd qemu
./configure --target-list="riscv32-softmmu riscv64-softmmu"
make -j $(nproc)
cd ..

#M-mode runtime for boot: OpenSBI Firmware
#git clone https://github.com/riscv/opensbi.git
cd opensbi
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
make -j $(nproc) PLATFORM=generic
cd ..

#compile Linux with RISC-V KVM support
#git clone https://github.com/kvm-riscv/linux.git
mkdir build-riscv64
export ARCH=riscv
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
make -C linux O=`pwd`/build-riscv64 defconfig
echo "CONFIG_KVM=y" >> ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

#KVM tool depends on dtc at runtime. So we first compile it.
#git clone git://git.kernel.org/pub/scm/utils/dtc/dtc.git
cd dtc
export ARCH=riscv
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
export CC="${CROSS_COMPILE}gcc -mabi=lp64d -march=rv64gc"
TRIPLET=$($CC -dumpmachine)
SYSROOT=$($CC -print-sysroot)
make libfdt
make EXTRA_CFLAGS="-mabi=lp64d" DESTDIR=$SYSROOT PREFIX=/usr LIBDIR=/usr/lib64/lp64d install-lib install-includes
cd ..

#build kvmtool
#git clone https://github.com/kvm-riscv/kvmtool.git
export ARCH=riscv
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
cd kvmtool
make lkvm-static -j $(nproc)
${CROSS_COMPILE}strip lkvm-static
cd ..

#build a root FS for our OS
wget https://busybox.net/downloads/busybox-1.33.1.tar.bz2
tar -C . -xvf busybox-1.33.1.tar.bz2
export ARCH=riscv
export CROSS_COMPILE=riscv64-unknown-linux-gnu-
make -C busybox-1.33.1 defconfig
cd busybox-1.33.1
echo 'CONFIG_CROSS_COMPILER_PREFIX="riscv64-unknown-linux-gnu-' >> .config
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

