#!/bin/bash
# Test-only platform primitives. No hardware implementation is linked into firmware.
mmio_test_headers()
{
	mkdir -p "$1/sbi"
	cat >"$1/sbi/sbi_types.h" <<'EOF'
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint64_t u64;
#define __aligned(x) __attribute__((aligned(x)))
EOF
	cat >"$1/sbi/sbi_error.h" <<'EOF'
#define SBI_EINVAL -3
#define SBI_ENOTSUPP -2
#define SBI_EDENIED -4
#define SBI_EALREADY -6
#define SBI_ENOSPC -1003
EOF
	cat >"$1/sbi/sbi_hart.h" <<'EOF'
unsigned int sbi_hart_count(void);
EOF
	cat >"$1/sbi/sbi_hfence.h" <<'EOF'
void __sbi_hfence_gvma_all(void);
void __sbi_hfence_vvma_all(void);
EOF
	cat >"$1/sbi/sbi_tlb.h" <<'EOF'
void __sbi_sfence_vma_all(void);
EOF
	cat >"$1/sbi/sbi_string.h" <<'EOF'
#include <string.h>
#define sbi_memset memset
EOF
	cat >"$1/sbi/riscv_barrier.h" <<'EOF'
#include <stdatomic.h>
#define mb() atomic_thread_fence(memory_order_seq_cst)
EOF
}
