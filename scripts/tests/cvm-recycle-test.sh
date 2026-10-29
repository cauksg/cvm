#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
awk '/^int recycle_one_chunk\(/ { active=1 }
     /^\/\/reset the pageownership table/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm.c" > "$work/recycle.inc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -I"$work" \
 "$top/scripts/tests/cvm-recycle-test.c" -o "$work/test"
"$work/test"
