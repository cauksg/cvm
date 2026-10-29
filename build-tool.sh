#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/scripts/build-common.sh"
build_init
cd "$TOP_DIR"

command -v "${CROSS_COMPILE}gcc" >/dev/null || {
	echo "Install the RISC-V cross compiler first: ${CROSS_COMPILE}gcc" >&2
	exit 1
}
for component in "$QEMU_DIR" "$TOP_DIR/dtc" "$TOP_DIR/howto"; do
	[ -f "$component/Makefile" ] || [ -f "$component/configs/busybox/inittab" ] || {
		echo "Initialize submodules first: git submodule update --init --recursive" >&2
		exit 1
	}
done
(
	cd "$QEMU_DIR"
	./configure --target-list="riscv32-softmmu riscv64-softmmu"
	make -j "$JOBS"
)
make -C "$TOP_DIR/dtc" CC="${CROSS_COMPILE}gcc" AR="${CROSS_COMPILE}ar" libfdt
mkdir -p "$LIBFDT_DIR"
cp "$TOP_DIR/dtc/libfdt/libfdt.a" "$TOP_DIR/dtc/libfdt/libfdt.h" \
	"$TOP_DIR/dtc/libfdt/libfdt_env.h" "$TOP_DIR/dtc/libfdt/fdt.h" "$LIBFDT_DIR/"
[ -f "$BUSYBOX_DIR/Makefile" ] || {
	echo "Extract BusyBox 1.33.1 into $BUSYBOX_DIR before building" >&2
	exit 1
}
build_busybox
