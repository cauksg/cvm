#!/bin/sh
set -e

INITRD=${INITRD:-/rootfs.cpio}
GUEST_CPUS=${GUEST_CPUS:-1}
GUEST_NETWORK=${GUEST_NETWORK:-mode=none}
GUEST_APPEND=${GUEST_APPEND:-unaligned_scalar_speed=fast}
GUEST_COVE_IO_AUTORUN=${GUEST_COVE_IO_AUTORUN:-}
GUEST_COVE_IO_AUTORUN_POWEROFF=${GUEST_COVE_IO_AUTORUN_POWEROFF:-1}
GUEST_COVE_IO_PRI_IOVA=${GUEST_COVE_IO_PRI_IOVA:-}
GUEST_COVE_IO_PRI_BAD_IOVA=${GUEST_COVE_IO_PRI_BAD_IOVA:-}
GUEST_COVE_IO_PRI_MODE=${GUEST_COVE_IO_PRI_MODE:-}
GUEST_COVE_IO_PRI_COUNT=${GUEST_COVE_IO_PRI_COUNT:-}
GUEST_COVE_IO_PRI_OVERFLOW_COUNT=${GUEST_COVE_IO_PRI_OVERFLOW_COUNT:-}
GUEST_COVE_IO_MSI_MODE=${GUEST_COVE_IO_MSI_MODE:-}
GUEST_COVE_IO_MSI_ADDR_LO=${GUEST_COVE_IO_MSI_ADDR_LO:-}
GUEST_COVE_IO_MSI_ADDR_HI=${GUEST_COVE_IO_MSI_ADDR_HI:-}
GUEST_COVE_IO_MSI_DATA=${GUEST_COVE_IO_MSI_DATA:-}
GUEST_COVE_IO_MSI_RETARGET_ADDR_LO=${GUEST_COVE_IO_MSI_RETARGET_ADDR_LO:-}
GUEST_COVE_IO_MSI_RETARGET_ADDR_HI=${GUEST_COVE_IO_MSI_RETARGET_ADDR_HI:-}
GUEST_COVE_IO_MSI_RETARGET_DATA=${GUEST_COVE_IO_MSI_RETARGET_DATA:-}
GUEST_RNG=${GUEST_RNG:-1}
GUEST_BALLOON=${GUEST_BALLOON:-0}
GUEST_VIRTIO_TRANSPORT=${GUEST_VIRTIO_TRANSPORT:-mmio}
GUEST_VFIO_PCI=${GUEST_VFIO_PCI:-}
GUEST_VFIO_PCI_VENDOR=${GUEST_VFIO_PCI_VENDOR:-0x1234}
GUEST_VFIO_PCI_DEVICE=${GUEST_VFIO_PCI_DEVICE:-0x11e8}
GUEST_LKVM_EXTRA_ARGS=${GUEST_LKVM_EXTRA_ARGS:-}
INITRD_ARG=
RNG_ARG=
BALLOON_ARG=
VIRTIO_TRANSPORT_ARG=
VFIO_PCI_ARG=
if [ -f "$INITRD" ]; then
	INITRD_ARG="-i $INITRD"
fi
if [ "$GUEST_RNG" != "0" ]; then
	RNG_ARG="--rng"
fi
if [ "$GUEST_BALLOON" != "0" ]; then
	BALLOON_ARG="--balloon"
fi
if [ -n "$GUEST_VIRTIO_TRANSPORT" ]; then
	VIRTIO_TRANSPORT_ARG="--virtio-transport $GUEST_VIRTIO_TRANSPORT"
fi
if [ "$GUEST_VFIO_PCI" = "auto" ]; then
	VFIO_AUTO_VENDOR=$(printf '%s' "$GUEST_VFIO_PCI_VENDOR" | tr '[:upper:]' '[:lower:]')
	VFIO_AUTO_DEVICE=$(printf '%s' "$GUEST_VFIO_PCI_DEVICE" | tr '[:upper:]' '[:lower:]')
	VFIO_AUTO_MATCHES=
	VFIO_AUTO_COUNT=0
	for dev in /sys/bus/pci/devices/*; do
		[ -e "$dev/vendor" ] || continue
		[ -e "$dev/device" ] || continue
		[ "$(cat "$dev/vendor")" = "$VFIO_AUTO_VENDOR" ] || continue
		[ "$(cat "$dev/device")" = "$VFIO_AUTO_DEVICE" ] || continue
		[ -e "$dev/iommu_group" ] || continue
		[ -e "$dev/driver" ] || continue
		[ "$(basename "$(readlink "$dev/driver")")" = "vfio-pci" ] || continue
		VFIO_AUTO_GROUP=$(basename "$(readlink "$dev/iommu_group")")
		VFIO_AUTO_GROUP_COUNT=0
		for member in /sys/kernel/iommu_groups/$VFIO_AUTO_GROUP/devices/*; do
			[ -e "$member" ] || continue
			VFIO_AUTO_GROUP_COUNT=$((VFIO_AUTO_GROUP_COUNT + 1))
		done
		[ "$VFIO_AUTO_GROUP_COUNT" -eq 1 ] || continue
		VFIO_AUTO_COUNT=$((VFIO_AUTO_COUNT + 1))
		VFIO_AUTO_MATCHES="$VFIO_AUTO_MATCHES $(basename "$dev")"
	done
	if [ "$VFIO_AUTO_COUNT" -ne 1 ]; then
		echo "run-guest-os: GUEST_VFIO_PCI=auto requires exactly one" \
			"vfio-pci device matching $VFIO_AUTO_VENDOR:$VFIO_AUTO_DEVICE" \
			"(found $VFIO_AUTO_COUNT)" >&2
		exit 1
	fi
	set -- $VFIO_AUTO_MATCHES
	GUEST_VFIO_PCI=$1
	echo "run-guest-os: auto-selected vfio-pci device $GUEST_VFIO_PCI" \
		"($VFIO_AUTO_VENDOR:$VFIO_AUTO_DEVICE)" >&2
fi
if [ -n "$GUEST_VFIO_PCI" ]; then
	VFIO_PCI_ARG="--vfio-pci $GUEST_VFIO_PCI"
fi
if [ -n "$GUEST_COVE_IO_AUTORUN" ]; then
	GUEST_APPEND="$GUEST_APPEND cove_io_guest_test=$GUEST_COVE_IO_AUTORUN cove_io_guest_poweroff=$GUEST_COVE_IO_AUTORUN_POWEROFF"
	case "$GUEST_COVE_IO_AUTORUN" in
	covg|covg-abi)
		GUEST_APPEND="$GUEST_APPEND cove_io_covg_selftest=1"
		export COVE_IO_TEST_LIFECYCLE=1
		;;
	esac
	if [ -n "$GUEST_COVE_IO_PRI_IOVA" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_pri_iova=$GUEST_COVE_IO_PRI_IOVA"
	fi
	if [ -n "$GUEST_COVE_IO_PRI_BAD_IOVA" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_pri_bad_iova=$GUEST_COVE_IO_PRI_BAD_IOVA"
	fi
	if [ -n "$GUEST_COVE_IO_PRI_MODE" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_pri_mode=$GUEST_COVE_IO_PRI_MODE"
	fi
	if [ -n "$GUEST_COVE_IO_PRI_COUNT" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_pri_count=$GUEST_COVE_IO_PRI_COUNT"
	fi
	if [ -n "$GUEST_COVE_IO_PRI_OVERFLOW_COUNT" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_pri_overflow_count=$GUEST_COVE_IO_PRI_OVERFLOW_COUNT"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_MODE" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_mode=$GUEST_COVE_IO_MSI_MODE"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_ADDR_LO" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_addr_lo=$GUEST_COVE_IO_MSI_ADDR_LO"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_ADDR_HI" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_addr_hi=$GUEST_COVE_IO_MSI_ADDR_HI"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_DATA" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_data=$GUEST_COVE_IO_MSI_DATA"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_RETARGET_ADDR_LO" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_retarget_addr_lo=$GUEST_COVE_IO_MSI_RETARGET_ADDR_LO"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_RETARGET_ADDR_HI" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_retarget_addr_hi=$GUEST_COVE_IO_MSI_RETARGET_ADDR_HI"
	fi
	if [ -n "$GUEST_COVE_IO_MSI_RETARGET_DATA" ]; then
		GUEST_APPEND="$GUEST_APPEND cove_io_guest_msi_retarget_data=$GUEST_COVE_IO_MSI_RETARGET_DATA"
	fi
fi

./apps/lkvm-static run -m 512 -c "$GUEST_CPUS" --console serial \
	-p "root=/dev/ram rw console=ttyS0 $GUEST_APPEND" \
	-k ./apps/Image $INITRD_ARG -n "$GUEST_NETWORK" \
	$RNG_ARG $BALLOON_ARG $VIRTIO_TRANSPORT_ARG $VFIO_PCI_ARG \
	$GUEST_LKVM_EXTRA_ARGS --debug --cvm
