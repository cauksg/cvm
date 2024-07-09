#!/bin/bash

#git submodule update --init

export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-

#Compiling the toolchain
#mkdir ~/riscv
#export RISCV=~/riscv
#sudo apt install autoconf automake autotools-dev curl libmpc-dev libmpfr-dev libgmp-dev \
#                 libusb-1.0-0-dev gawk build-essential bison flex texinfo gperf libtool \
#                 patchutils bc zlib1g-dev device-tree-compiler pkg-config libexpat-dev  \
#                 libncurses5-dev libncursesw5-dev git
##git clone https://github.com/riscv/riscv-gnu-toolchain
#cd riscv-gnu-toolchain
#git submodule init
#git submodule update
#./configure --prefix=$RISCV --enable-multilib
#make -j $(nproc)
#make -j $(nproc) linux
#eval echo 'export RISCV=$(eval echo ~)/riscv/bin' >> ~/.bashrc
#echo 'export PATH="$PATH:$RISCV"' >> ~/.bashrc
#source ~/.bashrc
#cd ..


#Compiling QEMU for RSIC-V with virtualization extensions
#sudo apt install ninja-build pkg-config libglib2.0-dev libpixman-1-dev libtirpc-dev unzip
#sudo apt-get install python3-venv
cd qemu
git submodule init
git submodule update
./configure --target-list="riscv32-softmmu riscv64-softmmu"
make -j $(nproc)
cd ..

#M-mode runtime for boot: OpenSBI Firmware
#cd opensbi
#git submodule init
#git submodule update
#make -j $(nproc) PLATFORM=generic
#cd ..

#compile Linux with RISC-V KVM support
#git clone https://github.com/kvm-riscv/linux.git
mkdir build-riscv64
make -C linux O=`pwd`/build-riscv64 defconfig
sed -i 's|^CONFIG_NET_9P_VIRTIO=.*$|CONFIG_NET_9P_VIRTIO=n|' ./build-riscv64/.config
sed -i 's|^CONFIG_VIRTIO_NET=.*$|CONFIG_VIRTIO_NET=n|' ./build-riscv64/.config
make -C linux O=`pwd`/build-riscv64 -j $(nproc)

#KVM tool depends on dtc at runtime. So we first compile it.
cd dtc
git submodule init
git submodule update
export CC="${CROSS_COMPILE}gcc -mabi=lp64d -march=rv64gc"
TRIPLET=$($CC -dumpmachine)
SYSROOT=$($CC -print-sysroot)
make libfdt
make EXTRA_CFLAGS="-mabi=lp64d" DESTDIR=$SYSROOT PREFIX=`pwd` LIBDIR=`pwd`/lib64/lp64d install-lib install-includes
cd ..

#build kvmtool
#export LIBFDT_DIR=`pwd`/dtc/lib64/lp64d
#cd kvmtool
#git submodule init
#git submodule update
#make lkvm-static -j $(nproc)
#${CROSS_COMPILE}strip lkvm-static
#cd ..

cd howto
git submodule init
git submodule update
cd ..

#build a root FS for our OS
wget https://busybox.net/downloads/busybox-1.33.1.tar.bz2
tar -C . -xvf busybox-1.33.1.tar.bz2
cd busybox-1.33.1
make defconfig
sed -i 's|^CONFIG_CROSS_COMPILER_PREFIX=.*$|CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-"|' .config
sed -i 's|.*CONFIG_STATIC.*|CONFIG_STATIC=y|' .config
cd ..
#cd busybox-1.33.1
#make defconfig
#echo 'CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-' >> .config
#echo 'CONFIG_STATIC=y' >> .config
#cd ..
#make -C busybox-1.33.1 install
#mkdir -p busybox-1.33.1/_install/etc/init.d
#mkdir -p busybox-1.33.1/_install/dev
#mkdir -p busybox-1.33.1/_install/proc
#mkdir -p busybox-1.33.1/_install/sys
#mkdir -p busybox-1.33.1/_install/apps
#ln -sf /sbin/init busybox-1.33.1/_install/init
#cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
#cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
#cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
#cp -f ./kvmtool/lkvm-static busybox-1.33.1/_install/apps
##cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
#cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.img; cd -

