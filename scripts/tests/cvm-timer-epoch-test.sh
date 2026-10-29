#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
python3 - "$top" "$work" <<'EXTRACT'
from pathlib import Path
import sys
s=(Path(sys.argv[1])/'linux/arch/riscv/kvm/vcpu_timer.c').read_text()
for name,out in [('static u64 kvm_riscv_current_cycles(', 'cycles.inc'),('void kvm_riscv_guest_timer_init(', 'init.inc')]:
 start=s.index(name); body=s.index('{',start); depth=1; end=body+1
 while depth:
  depth += (s[end]=='{')-(s[end]=='}');end+=1
 (Path(sys.argv[2])/out).write_text(s[start:end])
EXTRACT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined -I"$work" \
 "$top/scripts/tests/cvm-timer-epoch-test.c" -o "$work/test"
"$work/test"
