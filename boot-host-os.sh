#!/bin/bash
./qemu/build/qemu-system-riscv64 -cpu rv64 -M virt -m 1024M -nographic -bios opensbi/build/platform/generic/firmware/fw_jump.bin -kernel ./build-riscv64/arch/riscv/boot/Image -initrd ./rootfs_kvm_riscv64.img -append "root=/dev/ram rw console=ttyS0 earlycon=sbi"

#./qemu/build/qemu-system-riscv64 -cpu rv64 -smp 2,cores=1,threads=1,sockets=2 -M virt -m 1024M -nographic -bios opensbi/build/platform/generic/firmware/fw_jump.bin -kernel ./build-riscv64/arch/riscv/boot/Image -initrd ./rootfs_kvm_riscv64.img -append "root=/dev/ram rw console=ttyS0 earlycon=sbi"
