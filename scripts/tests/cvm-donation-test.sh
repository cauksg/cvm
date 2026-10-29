#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
awk '/^static int host_donation_range\(/ { active=1 }
     /^static int host_prepare_chunks\(/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm_host.c.inc" > "$work/donation.inc"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -I"$work" \
 "$top/scripts/tests/cvm-donation-test.c" -o "$work/test"
"$work/test"
