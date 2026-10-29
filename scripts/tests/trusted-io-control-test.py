#!/usr/bin/env python3
"""Exercise real HS/VS instructions against the fixed security controller."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

top = Path(__file__).resolve().parents[2]
src = top / "scripts/tests"
out = Path(os.environ.get("TEST_OUT", tempfile.mkdtemp(prefix="xs-io-control-")))
out.mkdir(parents=True, exist_ok=True)
qemu = top / "qemu/build/qemu-system-riscv64"
results = []
for device in ("edu", "e1000e", "multi", "nic-quad"):
    elf = out / (device + ".elf")
    command = ["riscv64-linux-gnu-gcc", "-O2", "-nostdlib", "-static",
               "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
               "-fno-pic", "-mcmodel=medany", "-msmall-data-limit=0",
               "-march=rv64gcvh_zicboz", "-mabi=lp64d", "-mno-relax",
               "-Wl,--build-id=none", "-T", str(src / "qemu-bitmap/link.ld"),
               str(src / "qemu-bitmap/start.S"),
               str(src / "trusted-io-control.S"),
               str(src / ("trusted-io-nic-quad.c" if device == "nic-quad" else "trusted-io-control.c")), "-o", str(elf)]
    if device in ("e1000e", "multi"):
        command.append("-DTEST_NIC")
    if device == "multi":
        command.append("-DTEST_MULTI")
    subprocess.run(command, check=True)
    args = [str(qemu), "-M", "virt,aia=aplic-imsic,aia-guests=7,cove-trusted=on",
            "-cpu", "rv64,x-mbmc=true", "-m", "1024M", "-nographic",
            "-monitor", "none", "-bios", "none", "-kernel", str(elf),
            "-net", "none", "-device", ("e1000e" if device in ("multi", "nic-quad") else device) + ",addr=2"]
    if device == "multi":
        args += ["-device", "edu,addr=3"]
    if device == "nic-quad":
        for addr in (3, 4, 5):
            args += ["-device", f"e1000e,addr={addr}"]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
    (out / (device + ".log")).write_bytes(result.stdout)
    passed = result.returncode == 0 and b"TRUSTED_IO_CONTROL_TEST_OK" in result.stdout
    record = dict(device=device, exit=result.returncode,
                  checks=result.stdout.count(b"PASS "), passed=passed)
    results.append(record)
    print(record, flush=True)
    if not passed:
        print(result.stdout.decode(errors="replace"), flush=True)
        break
manifest = dict(qemu_sha256=hashlib.sha256(qemu.read_bytes()).hexdigest(),
                devices=results, passed=len(results) == 4 and all(r["passed"] for r in results))
(out / "results.json").write_text(json.dumps(manifest, indent=2))
raise SystemExit(0 if manifest["passed"] else 1)
