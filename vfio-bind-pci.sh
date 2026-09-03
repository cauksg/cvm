#!/bin/sh
set -e

BDF=${1:-${VFIO_PCI_BDF:-}}
TARGET_VENDOR=${VFIO_PCI_VENDOR:-0x1234}
TARGET_DEVICE=${VFIO_PCI_DEVICE:-0x11e8}
ALLOW_UNSAFE_INTERRUPTS=${VFIO_ALLOW_UNSAFE_INTERRUPTS:-0}
ALLOW_COVE_IO_ISOLATED_MSI=${VFIO_ALLOW_COVE_IO_ISOLATED_MSI:-0}
ALLOW_COVE_IO_MRIF=${VFIO_ALLOW_COVE_IO_MRIF:-0}
ALLOW_COVE_IO_PRI=${VFIO_ALLOW_COVE_IO_PRI:-0}

die()
{
	echo "vfio-bind-pci: $*" >&2
	exit 1
}

write_param_if_present()
{
	local value=$1
	local param seen
	local wrote=0

	shift
	for param in "$@"; do
		if [ -e "$param" ]; then
			for seen in $written_params; do
				[ "$seen" = "$param" ] && continue 2
			done
			echo "$value" > "$param"
			echo "set $param=$value"
			written_params="$written_params $param"
			wrote=1
		fi
	done

	if [ "$wrote" -eq 1 ]; then
		return 0
	fi
	return 1
}

require_unsafe_interrupts_disabled()
{
	local param value

	for param in /sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts \
		     /sys/module/iommufd/parameters/allow_unsafe_interrupts; do
		[ -e "$param" ] || continue
		value=$(cat "$param")
		case "$value" in
		Y|y|1)
			die "$param is enabled; set VFIO_ALLOW_UNSAFE_INTERRUPTS=1 only for test-only insecure IRQ validation"
			;;
		esac
	done
}

normalize_bdf()
{
	case "$1" in
		????:??:??.*) printf '%s\n' "$1" ;;
		??:??.*) printf '0000:%s\n' "$1" ;;
		*) return 1 ;;
	esac
}

find_bdf_by_id()
{
	local dev vendor device match count=0

	for dev in /sys/bus/pci/devices/*; do
		[ -e "$dev/vendor" ] || continue
		vendor=$(cat "$dev/vendor")
		device=$(cat "$dev/device")
		if [ "$vendor" = "$TARGET_VENDOR" ] &&
		   [ "$device" = "$TARGET_DEVICE" ]; then
			match=$(basename "$dev")
			count=$((count + 1))
		fi
	done

	if [ "$count" -eq 1 ]; then
		printf '%s\n' "$match"
		return 0
	fi
	if [ "$count" -gt 1 ]; then
		echo "vfio-bind-pci: multiple devices matched $TARGET_VENDOR:$TARGET_DEVICE; pass a BDF explicitly" >&2
	fi
	return 1
}

[ -d /sys/bus/pci/devices ] || die "/sys/bus/pci/devices is not available"

if [ -z "$BDF" ]; then
	BDF=$(find_bdf_by_id || true)
	if [ -z "$BDF" ]; then
		echo "vfio-bind-pci: no device matched $TARGET_VENDOR:$TARGET_DEVICE" >&2
		echo "vfio-bind-pci: pass a BDF, e.g. ./vfio-bind-pci.sh 0000:00:03.0" >&2
		exit 1
	fi
fi

BDF=$(normalize_bdf "$BDF") || die "invalid BDF '$BDF'"
DEV="/sys/bus/pci/devices/$BDF"
[ -d "$DEV" ] || die "$BDF is not present under /sys/bus/pci/devices"

VENDOR=$(cat "$DEV/vendor")
DEVICE=$(cat "$DEV/device")

if [ -e "$DEV/driver" ]; then
	DRIVER=$(basename "$(readlink "$DEV/driver")")
	if [ "$DRIVER" != "vfio-pci" ]; then
		echo "$BDF" > "$DEV/driver/unbind"
	fi
fi

[ -d /sys/bus/pci/drivers/vfio-pci ] || modprobe vfio-pci 2>/dev/null || true
[ -d /sys/bus/pci/drivers/vfio-pci ] || die "vfio-pci driver is not available"

if [ "$ALLOW_UNSAFE_INTERRUPTS" != "0" ]; then
	write_param_if_present Y \
		/sys/module/vfio_iommu_type1/parameters/allow_unsafe_interrupts \
		/sys/module/iommufd/parameters/allow_unsafe_interrupts || \
		echo "vfio-bind-pci: allow_unsafe_interrupts parameters are not available; continuing" >&2
else
	require_unsafe_interrupts_disabled
fi

if [ "$ALLOW_COVE_IO_ISOLATED_MSI" != "0" ]; then
	write_param_if_present Y \
		/sys/module/riscv_iommu/parameters/cove_io_isolated_msi \
		/sys/module/iommu/parameters/cove_io_isolated_msi \
		/sys/module/*/parameters/cove_io_isolated_msi || \
		die "COVE-IO isolated MSI parameter is not available"
fi

if [ "$ALLOW_COVE_IO_MRIF" != "0" ]; then
	write_param_if_present Y \
		/sys/module/riscv_iommu/parameters/cove_io_mrif \
		/sys/module/iommu/parameters/cove_io_mrif \
		/sys/module/*/parameters/cove_io_mrif || \
		die "COVE-IO MRIF parameter is not available"
fi

if [ "$ALLOW_COVE_IO_PRI" != "0" ]; then
	write_param_if_present Y \
		/sys/module/riscv_iommu/parameters/cove_io_pri \
		/sys/module/iommu/parameters/cove_io_pri \
		/sys/module/*/parameters/cove_io_pri || \
		die "COVE-IO PRI parameter is not available"
fi

echo "$VENDOR $DEVICE" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/dev/null || true
if [ ! -e "$DEV/driver" ]; then
	echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind
fi

[ -e "$DEV/driver" ] || die "failed to bind $BDF to vfio-pci"
DRIVER=$(basename "$(readlink "$DEV/driver")")
[ "$DRIVER" = "vfio-pci" ] || die "$BDF is bound to $DRIVER, not vfio-pci"

[ -e "$DEV/iommu_group" ] || die "$BDF has no iommu_group"
GROUP=$(basename "$(readlink "$DEV/iommu_group")")

echo "bound $BDF ($VENDOR:$DEVICE) to vfio-pci"
echo "iommu_group=$GROUP"
echo "kvmtool argument: --vfio-pci $BDF"
