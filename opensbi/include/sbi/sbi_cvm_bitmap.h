/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __SBI_CVM_BITMAP_H__
#define __SBI_CVM_BITMAP_H__
#include <sbi/sbi_types.h>

/* Optional experimental QEMU/NEMU MBMC hardware, one hart only. */
int sbi_cvm_bitmap_init(unsigned long bitmap_addr);
bool sbi_cvm_bitmap_active(void);
void sbi_cvm_bitmap_sync(void);
void sbi_cvm_bitmap_set_trusted(bool trusted);

/* CPU MMIO primitives for a trusted platform's bitmap_ops implementation.
 * These do not claim devices, freeze PCI configuration, or drain DMA.
 */
int sbi_cvm_bitmap_mmio_init(const u64 *bitmap, unsigned long base,
			     unsigned long size);
void sbi_cvm_bitmap_mmio_sync(void);
void sbi_cvm_bitmap_mmio_set_trusted(bool trusted);
#endif
