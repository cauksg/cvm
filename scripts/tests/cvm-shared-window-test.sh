#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
python3 - "$top" "$work" <<'EXTRACT'
import sys
from pathlib import Path
s=(Path(sys.argv[1])/'opensbi/lib/sbi/sbi_cvm.c').read_text()
for name,next_,out in [('static bool cvm_shared_contains(', 'unsigned long PAGE_LEVEL;', 'contains.inc'), ('static void cvm_shared_revoke(', 'int cove_io_covg_ecall(', 'request.inc'), ('int add_cvm_share_pages(', 'int load_file(', 'map.inc')]:
 start=s.index(name)
 (Path(sys.argv[2])/out).write_text(s[start:s.index(next_, start)])
start=s.index('static bool cove_io_private_page_owned_by(', s.index('/* Simulator direct BAR0 mappings'))
(Path(sys.argv[2])/'private.inc').write_text(s[start:s.index('static int cove_io_copy_to_guest(',start)])
EXTRACT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined \
    -I"$work" "$top/scripts/tests/cvm-shared-window-test.c" -o "$work/test"
"$work/test"
