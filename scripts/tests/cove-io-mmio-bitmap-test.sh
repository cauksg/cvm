#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
source "$top/scripts/tests/cove-io-mmio-test-lib.sh"
mmio_test_headers "$work"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror -fsanitize=undefined \
	-I"$work" -I"$top/opensbi/include" \
	"$top/opensbi/lib/sbi/sbi_cvm_mmio.c" \
	"$top/scripts/tests/cove-io-mmio-bitmap-test.c" -o "$work/test"
"$work/test"
