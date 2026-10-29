#!/bin/bash
set -euo pipefail
top=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
mkdir -p "$work/sbi" "$work/sbi_utils/fdt"
for header in libfdt.h sbi/riscv_encoding.h sbi/riscv_io.h sbi/riscv_locks.h sbi/sbi_console.h sbi/sbi_domain.h sbi/sbi_error.h sbi/sbi_hart.h sbi/sbi_scratch.h sbi/sbi_string.h sbi/sbi_types.h sbi_utils/fdt/fdt_helper.h; do
 printf '#include "trusted-test-shim.h"\n' > "$work/$header"
done
cat > "$work/trusted-test-shim.h" <<'SHIM'
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef int spinlock_t;
#define SPIN_LOCK_INITIALIZER 0
static inline void spin_lock(spinlock_t *lock) { (void)lock; }
static inline void spin_unlock(spinlock_t *lock) { (void)lock; }
#define mb() __sync_synchronize()
#define sbi_memset memset
#define sbi_printf(...) ((void)0)
#define sbi_panic(...) abort()
#define CSR_HSTATUS 0
#define CSR_VSISELECT 1
#define CSR_VSIREG 2
#define HSTATUS_VGEIN (0x3fUL << 12)
static unsigned long test_csrs[3];
#define csr_read(n) test_csrs[n]
#define csr_write(n,v) (test_csrs[n]=(v))
#define csr_clear(n,v) (test_csrs[n]&=~(v))
#define SBI_EINVAL -3
#define SBI_ENOTSUPP -2
#define SBI_EDENIED -4
#define SBI_EALREADY -6
#define SBI_ETIMEDOUT -11
#define SBI_EIO -12
#define SBI_ENOSPC -1003
#define SBI_ENOMEM -1004
#define SBI_DOMAIN_MEMREGION_M_READABLE 1
#define SBI_DOMAIN_MEMREGION_M_WRITABLE 2
#define SBI_DOMAIN_MEMREGION_MMIO 4
struct sbi_scratch { unsigned long fw_start, fw_size; };
static struct sbi_scratch test_scratch = {0x80000000, 0x100000};
static inline struct sbi_scratch *sbi_scratch_thishart_ptr(void) { return &test_scratch; }
static inline unsigned int sbi_hart_count(void) { return 1; }
static inline int fdt_node_offset_by_compatible(const void *f, int n, const char *c) { return 1; }
static inline const void *fdt_getprop(const void *f, int n, const char *c, void *l) { return f; }
static inline int fdt_get_node_addr_size(const void *f, int n, int i, u64 *a, u64 *s) { *a=0x03010000;*s=4096;return 0; }
static inline int sbi_domain_root_add_memrange(u64 a,u64 s,u64 al,unsigned long f) { return 0; }
static uint64_t test_regs[512];
static uint64_t test_control[512] = { [0] = 0x43564d01 };
static unsigned char test_ecam[64 * 4096];
static bool test_command_failure;
static unsigned long test_fences;
static inline unsigned long regoff(const volatile void *p) { return (uintptr_t)p-0x03010000; }
static inline void *test_mmio(const volatile void *p) {
 uintptr_t a=(uintptr_t)p;
 if(a>=0x30000000 && a<0x30040000) return test_ecam+a-0x30000000;
 if(a>=0x03011000 && a<0x03012000) return (unsigned char *)test_control+a-0x03011000;
 assert(a>=0x03010000 && a<0x03011000);
 return (unsigned char *)test_regs+a-0x03010000;
}
static inline u64 readq(const volatile void *p) { u64 v;memcpy(&v,test_mmio(p),8);return v; }
static inline u32 readl(const volatile void *p) { u32 v;memcpy(&v,test_mmio(p),4);return v; }
static inline void writeq(u64 v, volatile void *p) { memcpy(test_mmio(p),&v,8); }
static inline void writel(u32 v, volatile void *p) {
 unsigned long off=regoff(p);
 memcpy(test_mmio(p),&v,4);
 if(off==0x48) ((u32 *)test_regs)[off/4]|=1<<16;
 if(off==0x24) {
  test_fences++;
  if(test_command_failure) ((u32 *)test_regs)[0x48/4]|=1<<8;
  else ((u32 *)test_regs)[0x20/4]=v;
 }
}
static inline unsigned char readb(const volatile void *p) { return *(unsigned char *)test_mmio(p); }
static inline unsigned short readw(const volatile void *p) { unsigned short v;memcpy(&v,test_mmio(p),2);return v; }
static inline void writeb(unsigned char v,volatile void *p) { memcpy(test_mmio(p),&v,1); }
static inline void writew(unsigned short v,volatile void *p) { memcpy(test_mmio(p),&v,2); }
SHIM
cc -D_GNU_SOURCE -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=undefined \
 -I"$work" -I"$top/opensbi/include" -I"$top/opensbi/lib/sbi" \
 "$top/scripts/tests/trusted-iommu-lifecycle-test.c" -o "$work/test"
"$work/test"
