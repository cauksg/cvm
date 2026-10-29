#!/usr/bin/env python3
"""Run real RV64 instructions against the opt-in QEMU bitmap hardware model."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile

top = Path(__file__).resolve().parents[2]
sources = Path(__file__).resolve().parent / "qemu-bitmap"
qemu = Path(os.environ.get("QEMU", str(top / "qemu/build/qemu-system-riscv64")))
prefix = os.environ.get("CROSS_COMPILE", "riscv64-linux-gnu-")
out = Path(os.environ.get("TEST_OUT", tempfile.mkdtemp(prefix="qemu-bitmap-")))
out.mkdir(parents=True, exist_ok=True)
results = []
common = [str(qemu), "-M", "virt", "-accel", "tcg", "-m", "1024M",
          "-nographic", "-monitor", "none", "-bios", "none"]
cpu = "rv64,x-mbmc=true,v=true,svnapot=true,zicboz=true"
for scenario in range(7):
    elf = out / f"check-{scenario}.elf"
    subprocess.run([prefix + "gcc", "-O2", "-g", "-nostdlib", "-static",
                    "-fno-tree-vectorize", "-ffreestanding", "-fno-builtin",
                    "-fno-stack-protector", "-fno-pic", "-mcmodel=medany",
                    "-msmall-data-limit=0", "-march=rv64gcvh_zicboz", "-mabi=lp64d",
                    "-mno-relax", f"-DSCENARIO={scenario}", "-Wl,--build-id=none",
                    "-T", str(sources / "link.ld"), str(sources / "start.S"),
                    str(sources / "check.c"), "-o", str(elf)], check=True)
    args = common + ["-cpu", cpu.replace("x-mbmc=true", "x-mbmc=false")
                    if scenario == 6 else cpu, "-kernel", str(elf)]
    if scenario in (4, 5):
        args += ["-device", "edu,addr=02.0,dma_mask=0xffffffff"]
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=45)
    log = out / f"check-{scenario}.log"
    log.write_bytes(result.stdout)
    record = dict(scenario=scenario, exit_code=result.returncode,
                  checks=result.stdout.count(b"PASS "), log=str(log),
                  passed=result.returncode == 0 and b"MBMC_TEST_OK" in result.stdout)
    results.append(record)
    print(json.dumps(record), flush=True)
    if not record["passed"]:
        print(result.stdout.decode(errors="replace"))
        raise SystemExit(1)

# Explicitly unsupported hardware configurations must fail before running code.
for args in (["-smp", "2"], ["-smp", "1,maxcpus=2"]):
    result = subprocess.run(common + ["-cpu", cpu] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            timeout=10)
    assert result.returncode and b"exactly one hart" in result.stdout, result.stdout
manifest = dict(qemu_sha256=hashlib.sha256(qemu.read_bytes()).hexdigest(),
                scenarios=results, total_checks=sum(r["checks"] for r in results),
                rejected_multihart_configs=2)
(out / "results.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(f"PASS: {manifest['total_checks']} bitmap checks; artifacts: {out}")
