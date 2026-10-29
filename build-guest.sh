#!/bin/bash
set -euo pipefail

source "$(dirname "$0")/scripts/build-common.sh"
build_init
cd "$TOP_DIR"

build_kernel "$GUEST_BUILD_DIR" guest
build_busybox
bash "$TOP_DIR/scripts/pack-rootfs.sh" --guest-only
