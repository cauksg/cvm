/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RISCV_BITMAP_H
#define RISCV_BITMAP_H

#include "cpu.h"
#include "csr.h"

/* Experimental NEMU ABI, not a ratified RISC-V extension. */
#define CSR_MBMC        0xbc2
/* Project extension: relative bitmap for the low PCI MMIO aperture. */
#define CSR_MBMCMIO     0xbc3
#define MBMC_CMODE      UINT64_C(1)
#define MBMC_BCLEAR     UINT64_C(2)
#define MBMC_BME        UINT64_C(4)
#define MBMC_BMA_MASK   (~UINT64_C(63))
#define MBMC_MMIO_BASE  UINT64_C(0x40000000)
#define MBMC_MMIO_END   UINT64_C(0x80000000)

RISCVException riscv_bitmap_predicate(CPURISCVState *env, int csrno);
RISCVException riscv_bitmap_read(CPURISCVState *env, int csrno,
                                target_ulong *val);
RISCVException riscv_bitmap_write(CPURISCVState *env, int csrno,
                                 target_ulong val, uintptr_t ra);
bool riscv_bitmap_check(CPURISCVState *env, hwaddr pa, int size,
                        privilege_mode_t mode);
#endif
