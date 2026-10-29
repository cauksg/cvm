#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/sbi"
cat >"$work/sbi/sbi_types.h" <<'EOF'
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
EOF
cat >"$work/sbi/sbi_cvm.h" <<'EOF'
#include <sbi/sbi_cvm_cpu.h>
#include <sbi/riscv_encoding.h>
#define SBI_EXT_COVG 0x434f5647
#define SBI_EXT_COVG_STOP_INTERFACE 1031
#define SBI_EXT_COVG_SIM_PREPARE_DMA 1088
EOF
cat >"$work/sbi/sbi_string.h" <<'EOF'
#include <string.h>
#define sbi_memset memset
EOF
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
 -I"$work" -I"$top/opensbi/include" -D__riscv_xlen=64 \
 "$top/opensbi/lib/sbi/sbi_cvm_state.c" \
 "$top/scripts/tests/cvm-private-state-test.c" -o "$work/test"
"$work/test"
