#!/usr/bin/env python3
"""Run the fixed single-hart CVM security suite against explicit built artifacts."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

TOP = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--out', type=Path, required=True)
p.add_argument('--rootfs', type=Path, required=True)
p.add_argument('--manifest', type=Path, required=True)
p.add_argument('--jobs', type=int, default=3, choices=range(1, 5))
a = p.parse_args()
a.out = a.out.resolve()
a.rootfs = a.rootfs.resolve()
a.manifest = a.manifest.resolve()
a.out.mkdir(parents=True, exist_ok=True)

def sha(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()

# Refuse to associate old packaged images with new executable binaries.
expected = {}
for line in a.manifest.read_text().splitlines():
    m = re.fullmatch(r'([0-9a-f]{64})  (.+)', line)
    if m:
        expected[Path(m[2]).resolve()] = m[1]
required = [a.rootfs, TOP/'qemu/build/qemu-system-riscv64',
            TOP/'opensbi/build/platform/generic/firmware/fw_jump.bin',
            TOP/'kvmtool/lkvm-static']
for path in required:
    if path not in expected or sha(path) != expected[path]:
        raise SystemExit(f'Artifact missing from manifest or changed: {path}')
before = {str(path): sha(path) for path in expected}
if any(before[str(path)] != value for path, value in expected.items()):
    raise SystemExit('Packaged manifest no longer matches artifacts; rebuild/repack first')
results = []
started = time.time()


def run(job):
    name, command, extra = job
    log = a.out/(name + '.log')
    env = os.environ.copy()
    for key in ('HOST_COVE_IO_TRUSTED', 'HOST_RISCV_IOMMU', 'HOST_PCI_TEST_DEVICE',
                'HOST_PCI_TEST_DEVICES', 'HOST_APPEND_EXTRA', 'QEMU_EXTRA_ARGS',
                'QEMU_CPU', 'HOST_BUILD_DIR', 'HOST_MEMORY', 'MACHINE'):
        env.pop(key, None)
    env.update(extra)
    begin = time.time()
    with log.open('wb') as f:
        try:
            r = subprocess.run(command, cwd=TOP, env=env, stdout=f,
                               stderr=subprocess.STDOUT, timeout=540)
            rc = r.returncode
        except subprocess.TimeoutExpired:
            rc = 124
    result = dict(name=name, command=command, environment=extra, exit=rc,
                  passed=rc == 0, seconds=round(time.time()-begin, 2),
                  log=str(log), log_sha256=sha(log))
    print(f'{name}: {"PASS" if rc == 0 else "FAIL"} ({result["seconds"]}s)', flush=True)
    return result

unit = ['cvm-private-state', 'cvm-allocation', 'cvm-interrupt-save',
        'cvm-timer-epoch', 'cvm-recycle', 'cvm-donation', 'cove-io-imsic',
        'cove-io-direct-mmio', 'cove-io-mmio-bitmap',
        'trusted-iommu-lifecycle', 'cvm-shared-window']
results.append(run(('units', ['bash', '-ec', '\n'.join(
    'bash scripts/tests/' + name + '-test.sh' for name in unit)], {})))
low = [(name, ['python3', 'scripts/tests/' + script + '-test.py'],
        {'TEST_OUT': str(a.out/name)}) for name, script in (
        ('firmware', 'trusted-iommu'), ('control', 'trusted-io-control'),
        ('bitmap', 'qemu-bitmap'))]
with ThreadPoolExecutor(max_workers=a.jobs) as pool:
    results.extend(pool.map(run, low))
if all(r['passed'] for r in results):
    jobs = []
    for name, test, trusted in (
            ('multi-trusted', 'multi-direct', True),
            ('nic-trusted', 'nic-direct', True),
            ('edu-trusted', 'edu-direct', True),
            ('nic-baseline', 'nic-direct', False),
            ('edu-baseline', 'edu-direct', False)):
        env = dict(ROOTFS_IMG=str(a.rootfs), TEST_LOG=str(a.out/(name + '.serial.log')),
                   QEMU_TIMEOUT='480', HOST_COVE_IO_TEST_WAIT_SECS='420',
                   HOST_COVE_IO_TRUSTED='1' if trusted else '0',
                   QEMU_CPU='rv64,x-mbmc=true')
        if test == 'edu-direct':
            env.update(HOST_RISCV_IOMMU='0' if trusted else '1',
                       HOST_PCI_TEST_DEVICE='edu,addr=2,dma_mask=0xffffffff')
        if test == 'multi-direct':
            env['HOST_APPEND_EXTRA'] = 'kvm.cvm_state_attack_test=1'
        jobs.append((name, ['bash', 'scripts/run-cove-io-test.sh', test], env))
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        results.extend(pool.map(run, jobs))

unchanged = all(Path(path).is_file() and sha(Path(path)) == value
                for path, value in before.items())
serial = [Path(r['environment']['TEST_LOG']) for r in results
          if 'TEST_LOG' in r['environment']]
record = dict(date='2026-10-25', scope='one hart, one CVM/vCPU, EDU + e1000e',
              elapsed_seconds=round(time.time()-started, 2),
              manifest=str(a.manifest), manifest_sha256=sha(a.manifest),
              artifacts_sha256=before, artifacts_unchanged=unchanged,
              results=results,
              serial_logs_sha256={str(path): sha(path) for path in serial},
              passed=len(results) == 9 and unchanged and all(r['passed'] for r in results))
(a.out/'validation.json').write_text(json.dumps(record, indent=2) + '\n')
print('Validation:', a.out/'validation.json', flush=True)
raise SystemExit(0 if record['passed'] else 1)
