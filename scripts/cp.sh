#!/bin/bash
cp -f ./howto/configs/busybox/fstab busybox-1.33.1/_install/etc/fstab
cp -f ./howto/configs/busybox/rcS busybox-1.33.1/_install/etc/init.d/rcS
cp -f ./howto/configs/busybox/motd busybox-1.33.1/_install/etc/motd
cp -f ./howto/configs/busybox/inittab busybox-1.33.1/_install/etc/inittab
cp -f ./kvmtool/lkvm-static busybox-1.33.1/_install/apps
cp -f ./build-riscv64/arch/riscv/boot/Image busybox-1.33.1/_install/apps
cp -f run-guest-os.sh busybox-1.33.1/_install
cp -f ./vfio-bind-pci.sh busybox-1.33.1/_install
mkdir -p busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-host-autorun.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-guest-autorun.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-lazy-fault-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-multi-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-msi-test.sh busybox-1.33.1/_install/scripts
cp -f ./scripts/cove-io-vfio-pri-test.sh busybox-1.33.1/_install/scripts
chmod +x busybox-1.33.1/_install/scripts/cove-io-*.sh
rm -f rootfs_guest_riscv64.cpio busybox-1.33.1/_install/rootfs.cpio
(
	cd busybox-1.33.1/_install
	find ./ \( \
		-path './rootfs.cpio' -o \
		-path './apps' -o -path './apps/*' -o \
		-path './lib' -o -path './lib/*' \
		\) -prune -o -print | cpio -o -H newc > ../../rootfs_guest_riscv64.cpio
)
cp -f rootfs_guest_riscv64.cpio busybox-1.33.1/_install/rootfs.cpio
cd busybox-1.33.1/_install; find ./ | cpio -o -H newc > ../../rootfs_kvm_riscv64.img; cd -
