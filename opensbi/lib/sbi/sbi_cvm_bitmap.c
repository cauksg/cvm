/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/sbi_cvm_bitmap.h>
#include <sbi/sbi_csr_detect.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/riscv_barrier.h>

#define CSR_MBMC 0xbc2
#define CSR_MBMCMIO 0xbc3
#define MBMC_CMODE 1UL
#define MBMC_BCLEAR 2UL
#define MBMC_BME 4UL
#define RAM_BASE 0x80000000UL
#define MMIO_BASE 0x40000000UL
#define MMIO_END 0x80000000UL

static bool ram_active, mmio_active;

bool sbi_cvm_bitmap_active(void)
{
	return ram_active;
}

int sbi_cvm_bitmap_init(unsigned long bitmap_addr)
{
	struct sbi_trap_info trap = { 0 };
	unsigned long old = csr_read_allowed(CSR_MBMC, &trap);
	/* An ordinary CPU continues to use the existing software-only path. */
	if (trap.cause)
		return 0;
	if (sbi_hart_count() != 1)
		return SBI_ENOTSUPP;
	if (old & MBMC_BME)
		return SBI_EALREADY;
	if ((bitmap_addr & 63) || bitmap_addr < (RAM_BASE >> 15))
		return SBI_EINVAL;
	/* Existing Monitor bitmaps index from RAM_BASE; NEMU indexes from PA=0.
	 * Bias the CSR base so that its absolute PFN lookup reaches our first
	 * byte at PA=RAM_BASE. Only the resulting lookup addresses are read.
	 */
	unsigned long value = (bitmap_addr - (RAM_BASE >> 15)) | MBMC_BME;
	mb();
	csr_write(CSR_MBMC, value);
	if (csr_read(CSR_MBMC) != value)
		sbi_panic("MBMC: RAM bitmap attachment failed\n");
	ram_active = true;
	sbi_printf("MBMC: RAM bitmap enabled, Host checked, one hart\n");
	return 0;
}

void sbi_cvm_bitmap_sync(void)
{
	if (ram_active) {
		mb();
		csr_set(CSR_MBMC, MBMC_BCLEAR);
	}
}

void sbi_cvm_bitmap_set_trusted(bool trusted)
{
	if (!ram_active)
		return;
	mb();
	if (trusted)
		csr_set(CSR_MBMC, MBMC_CMODE);
	else
		csr_clear(CSR_MBMC, MBMC_CMODE);
}

int sbi_cvm_bitmap_mmio_init(const u64 *bitmap, unsigned long base,
			     unsigned long size)
{
	struct sbi_trap_info trap = { 0 };
	unsigned long old = csr_read_allowed(CSR_MBMCMIO, &trap);
	unsigned long value = (unsigned long)bitmap | MBMC_BME;

	if (trap.cause || sbi_hart_count() != 1)
		return SBI_ENOTSUPP;
	if (old & MBMC_BME)
		return SBI_EALREADY;
	if (!bitmap || ((unsigned long)bitmap & 63) || base != MMIO_BASE ||
	    size != MMIO_END - MMIO_BASE)
		return SBI_EINVAL;
	mb();
	csr_write(CSR_MBMCMIO, value);
	if (csr_read(CSR_MBMCMIO) != value)
		sbi_panic("MBMC: MMIO bitmap attachment failed\n");
	mmio_active = true;
	return 0;
}

void sbi_cvm_bitmap_mmio_sync(void)
{
	if (mmio_active) {
		mb();
		csr_set(CSR_MBMCMIO, MBMC_BCLEAR);
	}
}

void sbi_cvm_bitmap_mmio_set_trusted(bool trusted)
{
	if (!mmio_active)
		return;
	mb();
	if (trusted)
		csr_set(CSR_MBMCMIO, MBMC_CMODE);
	else
		csr_clear(CSR_MBMCMIO, MBMC_CMODE);
}
