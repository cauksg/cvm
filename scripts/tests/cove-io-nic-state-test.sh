#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
python3 - "$top" "$work" <<'EXTRACT'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'linux/arch/riscv/kernel/cove_io_nic.c').read_text()
parts=[s[s.index('struct cove_io_nic_state {'):s.index('static int __init')]]
for name in ('static int nic_prepare(', 'int cove_io_nic_enable('):
 start=s.index(name)
 parts.append(s[start:s.index('\n}',start)+2])
(Path(sys.argv[2])/'nic.inc').write_text('\n'.join(parts))
EXTRACT
cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined \
 -I"$work" "$top/scripts/tests/cove-io-nic-state-test.c" -o "$work/test"
"$work/test"
