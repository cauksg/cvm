#!/bin/bash
set -e


export ARCH=riscv
export CROSS_COMPILE=riscv64-linux-gnu-

#cd busybox-1.33.1
#make defconfig
#sed -i 's|^CONFIG_CROSS_COMPILER_PREFIX=.*$|CONFIG_CROSS_COMPILER_PREFIX="riscv64-linux-gnu-"|' .config
#sed -i 's|.*CONFIG_STATIC.*|CONFIG_STATIC=y|' .config
#sed -i 's|^CONFIG_STATIC=.*$|CONFIG_STATIC=y|' .config
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
rm -rf busybox-1.33.1/_install/apps
ln -sf /sbin/init busybox-1.33.1/_install/init
cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
#cp -f ./kvmtool/lkvm-static busybox-1.33.1/_install/apps
#cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
cd busybox-1.33.1/_install; echo "mknod dev/console c 5 1; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.cpio" | fakeroot; cd -


#mkdir build-riscv64
cd build-riscv64
sed -i 's|^CONFIG_NET_9P_VIRTIO=.*$|CONFIG_NET_9P_VIRTIO=n|' .config
sed -i 's|^CONFIG_VIRTIO_NET=.*$|CONFIG_VIRTIO_NET=n|' .config
sed -i 's|^CONFIG_INITRAMFS_SOURCE=.*$|CONFIG_INITRAMFS_SOURCE="../rootfs_kvm_riscv64.cpio"|' .config
cd ..
make -C linux O=`pwd`/build-riscv64 -j $(nproc)
