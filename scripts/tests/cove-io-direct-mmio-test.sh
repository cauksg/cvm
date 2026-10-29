#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
source "$top/scripts/tests/cove-io-mmio-test-lib.sh"
mmio_test_headers "$work"
awk '/^\/\* Simulator direct BAR0 mappings/ { active=1 }
     /^static bool cove_io_private_page_owned_by/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm.c" >"$work/cove-io-direct-mmio.inc"
awk '/^static int cove_io_reclaim_mmio\(/ { active=1 }
     /^static bool cove_io_uses_trusted_iommu\(/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm.c" >"$work/cove-io-reclaim-mmio.inc"
awk '/^\/\* MMIO bitmap bypass must never/ { active=1 }
     /^static void context_switch_to_cvm\(/ { active=0 }
     active { print }' "$top/opensbi/lib/sbi/sbi_cvm.c" >"$work/cove-io-mmio-context.inc"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
	-I"$work" -I"$top/opensbi/include" \
	"$top/opensbi/lib/sbi/sbi_cvm_mmio.c" \
	"$top/scripts/tests/cove-io-direct-mmio-test.c" -o "$work/test"
"$work/test"
