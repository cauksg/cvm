#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/scripts/build-common.sh"
build_init
cd "$TOP_DIR"

build_kernel "$GUEST_BUILD_DIR" guest
build_kernel "$HOST_BUILD_DIR" host
make -C "$KVMTOOL_DIR" ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" \
	LIBFDT_DIR="$LIBFDT_DIR" -j "$JOBS" lkvm-static
"${CROSS_COMPILE}strip" "$KVMTOOL_DIR/lkvm-static"
make -C "$OPENSBI_DIR" -j "$JOBS" PLATFORM=generic
build_busybox
bash "$TOP_DIR/scripts/pack-rootfs.sh"
