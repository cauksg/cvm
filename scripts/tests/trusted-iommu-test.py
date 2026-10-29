#!/usr/bin/env python3
"""Execute attack probes in S-mode under the actual OpenSBI and QEMU."""
import hashlib, json, os, subprocess, tempfile
from pathlib import Path
top=Path(__file__).resolve().parents[2]
out=Path(os.environ.get('TEST_OUT',tempfile.mkdtemp(prefix='xs-trusted-iommu-')))
out.mkdir(parents=True,exist_ok=True)
src=top/'scripts/tests/trusted-iommu'
elf=out/'attack.elf'
subprocess.run(['riscv64-linux-gnu-gcc','-O2','-nostdlib','-static','-ffreestanding',
 '-Wl,--build-id=none','-fno-builtin','-fno-stack-protector','-fno-pic','-mcmodel=medany','-msmall-data-limit=0',
 '-march=rv64gch','-mabi=lp64d','-mno-relax','-T',str(src/'link.ld'),
 str(src/'start.S'),str(src/'check.c'),'-o',str(elf)],check=True)
args=[str(top/'qemu/build/qemu-system-riscv64'),'-M','virt,aia=aplic-imsic,aia-guests=7,cove-trusted=on',
 '-cpu','rv64,x-mbmc=true','-m','1024M','-nographic','-monitor','none',
 '-bios',str(top/'opensbi/build/platform/generic/firmware/fw_jump.bin'),
 '-kernel',str(elf),'-device','edu,addr=2,dma_mask=0xffffffff']
with (out/'attack.log').open('wb') as log:
 try:
  p=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT,timeout=60)
 except subprocess.TimeoutExpired:
  print((out/'attack.log').read_text(errors='replace'))
  raise
p.stdout=(out/'attack.log').read_bytes()
print(p.stdout.decode(errors='replace'))
record={'exit':p.returncode,'checks':p.stdout.count(b'PASS '),'passed':p.returncode==0 and b'TRUSTED_IOMMU_TEST_OK' in p.stdout}
record['qemu_sha256']=hashlib.sha256((top/'qemu/build/qemu-system-riscv64').read_bytes()).hexdigest()
record['firmware_sha256']=hashlib.sha256((top/'opensbi/build/platform/generic/firmware/fw_jump.bin').read_bytes()).hexdigest()
# Reject configurations that would silently weaken this fixed platform.
record['rejected_configurations']=[]
if record['passed']:
 for name, extra, marker in [
  ('bitmap-disabled',['-cpu','rv64,x-mbmc=false'],b'at least 16 PMP'),
  ('pmp-disabled',['-cpu','rv64,x-mbmc=true,pmp=false'],b'at least 16 PMP'),
  ('pmp-insufficient',['-cpu','rv64,x-mbmc=true,num-pmp-regions=8'],b'at least 16 PMP'),
  ('wrong-IMSIC',['-M','virt,aia=aplic-imsic,aia-guests=3,cove-trusted=on'],b'aia-guests=7'),
  ('wrong-RAM',['-m','512M'],b'1 GiB RAM'),
  ('alternate-IOMMU',['-device','riscv-iommu-pci'],b'forbids alternate IOMMUs'),
  ('PCI-bridge',['-device','pcie-root-port'],b'requires a fixed PCI root bus'),
  ('unroutable-RID',['-device','edu,addr=8'],b'RID < 64'),
  ('unsupported-NIC',['-net','none','-device','rtl8139,addr=3'],b'root-bus edu/e1000e'),
 ]:
  r=subprocess.run(args+extra,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=10)
  ok=r.returncode!=0 and marker in r.stdout
  record['rejected_configurations'].append({'name':name,'passed':ok})
  if not ok: print(name,r.returncode,r.stdout.decode(errors='replace'))
 record['passed'] &= all(r['passed'] for r in record['rejected_configurations'])
(out/'results.json').write_text(json.dumps(record,indent=2)+'\n')
print(out,record)
raise SystemExit(0 if record['passed'] else 1)
