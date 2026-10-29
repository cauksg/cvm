#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
awk '/^int init_cvm_vcpu_rootptAndChunk\(/ { active=1 }
     /^static int find_pte\(/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm.c" >"$work/cvm-allocation.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
	-I"$work" "$top/scripts/tests/cvm-allocation-test.c" -o "$work/test"
"$work/test"
