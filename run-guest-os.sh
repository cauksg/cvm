#!/bin/sh
set -e

INITRD=${INITRD:-/rootfs.cpio}
GUEST_CPUS=${GUEST_CPUS:-1}
GUEST_NETWORK=${GUEST_NETWORK:-mode=none}
GUEST_APPEND=${GUEST_APPEND:-unaligned_scalar_speed=fast}
INITRD_ARG=
if [ -f "$INITRD" ]; then
	INITRD_ARG="-i $INITRD"
fi

./apps/lkvm-static run -m 512 -c "$GUEST_CPUS" --console serial \
	-p "root=/dev/ram rw console=ttyS0 earlycon=uart8250,mmio,0x10000000 $GUEST_APPEND" \
	-k ./apps/Image $INITRD_ARG -n "$GUEST_NETWORK" --debug --cvm
