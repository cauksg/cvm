/*
 * NEMU MBMC physical page isolation, with a separate PCI MMIO bitmap.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "bitmap.h"
#include "exec/cputlb.h"
#include "exec/translation-block.h"
#include "exec/tb-flush.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "pmp.h"

RISCVException riscv_bitmap_predicate(CPURISCVState *env, int csrno)
{
    return riscv_cpu_cfg(env)->mbmc ? RISCV_EXCP_NONE : RISCV_EXCP_ILLEGAL_INST;
}

RISCVException riscv_bitmap_read(CPURISCVState *env, int csrno,
                                target_ulong *val)
{
    *val = csrno == CSR_MBMC ? env->mbmc : env->mbmcmio;
    return RISCV_EXCP_NONE;
}

RISCVException riscv_bitmap_write(CPURISCVState *env, int csrno,
                                 target_ulong val, uintptr_t ra)
{
    uint64_t *reg = csrno == CSR_MBMC ? &env->mbmc : &env->mbmcmio;
    uint64_t old = *reg;
    uint64_t mask = old & MBMC_BME ? MBMC_CMODE :
                    MBMC_BMA_MASK | MBMC_BME | MBMC_CMODE;

    /* BME/BMA lock until reset. BCLEAR is a self-clearing local command. */
    *reg = (old & ~mask) | (val & mask);
    if (old != *reg || (val & MBMC_BCLEAR)) {
        CPUState *cs = env_cpu(env);

        tlb_flush(cs);
        /* CMODE is part of the TB key. Pure domain switches can reuse code
         * translated for that domain; bitmap changes must invalidate it.
         * CSR instructions end the TB. The queued flush also kicks the CPU
         * out of execution, revoking translated code and direct TB chains
         * before it resumes. This model permits only a single hart.
         */
        if (((old ^ *reg) & ~MBMC_CMODE) || (val & MBMC_BCLEAR)) {
            queue_tb_flush(cs);
        }
    }
    return RISCV_EXCP_NONE;
}

static bool bitmap_byte(CPURISCVState *env, hwaddr addr, uint8_t *value)
{
    CPUState *cs = env_cpu(env);
    hwaddr translated, len = 1;
    pmp_priv_t allowed;
    MemoryRegion *mr;

    /* Metadata is read as M-mode, independently of MPRV, without recursion. */
    if (riscv_cpu_cfg(env)->pmp &&
        !pmp_hart_has_privs(env, addr, 1, PMP_READ, &allowed, PRV_M)) {
        return false;
    }
    mr = address_space_translate(cs->as, addr, &translated, &len, false,
                                 MEMTXATTRS_UNSPECIFIED);
    if (!len || !memory_region_is_ram(mr) || memory_region_is_rom(mr)) {
        return false;
    }
    return address_space_read(cs->as, addr, MEMTXATTRS_UNSPECIFIED,
                              value, 1) == MEMTX_OK;
}

static bool bitmap_page(CPURISCVState *env, uint64_t reg, hwaddr page)
{
    uint64_t base = reg & MBMC_BMA_MASK;
    uint64_t offset = page >> 3;
    uint8_t bits;

    return base <= UINT64_MAX - offset &&
           bitmap_byte(env, base + offset, &bits) &&
           !(bits & (1u << (page & 7)));
}

bool riscv_bitmap_check(CPURISCVState *env, hwaddr pa, int size,
                        privilege_mode_t mode)
{
    CPUState *cs = env_cpu(env);
    hwaddr last, page;

    if (!riscv_cpu_cfg(env)->mbmc || mode == PRV_M) {
        return true;
    }
    /* A zero-sized TCG probe still checks the page containing the address. */
    size = MAX(size, 1);
    if (pa > UINT64_MAX - (size - 1)) {
        return false;
    }
    last = pa + size - 1;
    /* Include PIO and MSI-X aliases. M-mode installs the owner state and
     * descriptors from the PCI model, never from untrusted Host memory. */
    for (unsigned rid = 1; rid < 64; rid++) {
        if (!env->cove_claimed_rid[rid]) continue;
        bool owner = env->virt_enabled && env->cove_guest_access &&
                     (env->mbmcmio & MBMC_CMODE) &&
                     (!env->cove_rid_owner[rid] ||
                      env->cove_rid_owner[rid] == env->cove_guest_access);
        for (unsigned i = 0; i < 7; i++) {
            if (!owner && env->cove_bar_size[rid][i] &&
                pa < env->cove_bar_addr[rid][i] + env->cove_bar_size[rid][i] &&
                last >= env->cove_bar_addr[rid][i]) return false;
        }
    }
    /* Cross-CVM MMIO is denied even with bitmap exemption and Bare G-stage. */
    for (unsigned file = 1; file < 8; file++) {
        if (env->virt_enabled && env->cove_file_owner[file] &&
            pa < 0x28000000ULL + (file + 1) * 4096 &&
            last >= 0x28000000ULL + file * 4096 &&
            env->cove_guest_access != env->cove_file_owner[file]) return false;
    }
    for (page = pa >> 12; page <= last >> 12; page++) {
        hwaddr addr = MAX(pa, page << 12);
        uint64_t reg;

        if (addr >= MBMC_MMIO_BASE && addr < MBMC_MMIO_END) {
            reg = env->mbmcmio;
            if ((reg & (MBMC_BME | MBMC_CMODE)) == MBMC_BME &&
                !bitmap_page(env, reg, page - (MBMC_MMIO_BASE >> 12))) {
                return false;
            }
        }

        reg = env->mbmc;
        if ((reg & (MBMC_BME | MBMC_CMODE)) == MBMC_BME) {
            hwaddr translated, len = MIN(last - addr + 1,
                                        4096 - (addr & 4095));
            hwaddr expected = len;
            MemoryRegion *mr = address_space_translate(cs->as, addr,
                &translated, &len, false, MEMTXATTRS_UNSPECIFIED);

            /* Original MBMC labels RAM only, using absolute physical PFNs. */
            if (memory_region_is_ram(mr) && !memory_region_is_rom(mr) &&
                (len < expected || !bitmap_page(env, reg, page))) {
                return false;
            }
        }
    }
    return true;
}
