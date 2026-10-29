#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
awk '/^void kvm_riscv_vcpu_flush_interrupts\(/ { active=1 }
     /^void kvm_riscv_vcpu_sync_interrupts\(/ { active=0 }
     active { print }' "$top/linux/arch/riscv/kvm/vcpu.c" > "$work/flush.inc"
awk '/^void kvm_arch_vcpu_put\(/ { active=1 }
     active && /^\/\*\*/ { active=0 }
     active { print }' "$top/linux/arch/riscv/kvm/vcpu.c" > "$work/put.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
	-I"$work" "$top/scripts/tests/cvm-interrupt-save-test.c" -o "$work/test"
"$work/test"
