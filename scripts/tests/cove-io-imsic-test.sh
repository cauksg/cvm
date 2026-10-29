#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
python3 - "$top" "$work" <<'PY'
import sys
from pathlib import Path
top, work = map(Path, sys.argv[1:])
for unit, file, names in [
 ('driver', 'linux/drivers/iommu/riscv/iommu.c', [
  'riscv_iommu_cove_io_install_imsic_pte',
  'riscv_iommu_cove_io_deactivate_imsic',
  'riscv_iommu_cove_io_imsic_bind',
  'riscv_iommu_cove_io_imsic_refresh_cb']),
 ('kvm', 'linux/arch/riscv/kvm/aia_imsic.c', [
  'kvm_riscv_vcpu_aia_imsic_release',
  'kvm_riscv_vcpu_aia_imsic_update',
  'kvm_riscv_aia_imsic_direct_enable',
  'imsic_direct_clear_local'])]:
 s=(top/file).read_text()
 parts=[]
 for name in names:
  start=s.rfind('\n',0,s.index(name+'('))+1
  end=s.index('\n}',s.index(name+'('))+2
  parts.append(s[start:end])
 (work/('imsic-'+unit+'.inc')).write_text('\n\n'.join(parts))
PY
for unit in driver kvm; do
 cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined \
  -I"$work" "$top/scripts/tests/cove-io-imsic-$unit-test.c" -o "$work/$unit"
 "$work/$unit"
done
