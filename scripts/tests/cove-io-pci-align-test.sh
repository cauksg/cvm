#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/linux" "$work/asm"
for h in linux/pci.h linux/sizes.h asm/sbi.h; do
    printf '#include "shim.h"\n' > "$work/$h"
done
cat > "$work/shim.h" <<'SHIM'
#pragma once
#include <stdint.h>
typedef uint64_t resource_size_t;
struct resource { resource_size_t start; unsigned long flags; };
struct sbiret { long error, value; };
#define IORESOURCE_IO 1
#define SZ_4K 4096ULL
#define ALIGN(v,a) (((v)+(a)-1)&~((a)-1))
#define SBI_EXT_CVM 1
#define SBI_EXT_CVM_TRUSTED_IOMMU 2
static long probe_error, probe_value=0x03010000;
static struct sbiret sbi_ecall(unsigned long a,unsigned long b,unsigned long c,
 unsigned long d,unsigned long e,unsigned long f,unsigned long g,unsigned long h)
{ return (struct sbiret){probe_error,probe_value}; }
SHIM
cat > "$work/test.c" <<'TEST'
#include <assert.h>
#include <stdio.h>
#include "shim.h"
#include "cove_io_pci.c"
int main(void) {
 struct resource r={.flags=IORESOURCE_IO};
 for (unsigned i=1;i<=4;i++) {
  r.start=(i-1)*4096+32;
  assert(pcibios_align_resource(0,&r,32,32)==i*4096);
 }
 r.start=8192;assert(pcibios_align_resource(0,&r,32,32)==8192);
 r.flags=0;r.start=32;assert(pcibios_align_resource(0,&r,32,32)==32);
 r.flags=IORESOURCE_IO;probe_error=-2;
 assert(pcibios_align_resource(0,&r,32,32)==32);
 probe_error=0;probe_value=0;
 assert(pcibios_align_resource(0,&r,32,32)==32);
 puts("PASS: trusted PIO BARs use separate 4K pages; normal platform and memory BARs unchanged");
}
TEST
cc -DCONFIG_RISCV_SBI -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined \
 -I"$work" -I"$top/linux/arch/riscv/kernel" "$work/test.c" -o "$work/test"
"$work/test"
