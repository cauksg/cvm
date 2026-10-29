/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __SBI_CVM_MMIO_H__
#define __SBI_CVM_MMIO_H__

#include <sbi/sbi_types.h>

/* Current direct-EDU platform aperture; separate from the RAM bitmap. */
#define SBI_CVM_MMIO_BASE		0x40000000UL
#define SBI_CVM_MMIO_END		0x80000000UL
#define SBI_CVM_MMIO_PAGE_SHIFT	12
#define SBI_CVM_MMIO_PAGE_SIZE	(1UL << SBI_CVM_MMIO_PAGE_SHIFT)
#define SBI_CVM_MMIO_MAX_RESERVATIONS 16

/*
 * Trusted platform integration, NOT a standard RISC-V CSR ABI. Register once
 * during M-mode platform initialization, before starting any CVM. The bitmap
 * and these callbacks reside in Host-inaccessible Monitor memory. This version
 * deliberately supports one physical hart only.
 *
 * init: attach a little-endian u64 bitmap (bit 0 protects the first 4K page).
 *       Failure must leave hardware detached and Host checking enabled.
 * claim: verify a trusted device resource description, reserve whole physical
 *        pages (including padding), and freeze BAR/configuration aliases against
 *        Host changes. Failure must leave no resource claim behind.
 * sync: publish bitmap stores and synchronously invalidate hardware bitmap
 *       caches. Must complete or halt; returning without enforcement is unsafe.
 * set_trusted: switch only this hart's CPU permission. false denies protected
 *              MMIO to Host; true relies on Monitor-owned G-stage mappings.
 *              Must complete or halt; Host must never inherit a bypass.
 * quiesce: trusted stop/drain/scrub and DMA/IRQ revocation before returning the
 *          resource to Host. Failure keeps the claim and protection intact.
 * unclaim: release configuration protection after the bitmap has been cleared.
 *
 * M-mode accesses remain permitted. DMA/P2P protection is a separate platform
 * responsibility. No callback may call back into the TDI state machine.
 */
struct sbi_cvm_mmio_bitmap_ops {
	int (*init)(const u64 *bitmap, unsigned long base, unsigned long size);
	int (*claim)(u64 device_id, u64 hpa, u64 size);
	void (*sync)(void);
	void (*set_trusted)(bool trusted);
	int (*quiesce)(u64 device_id, u64 hpa, u64 size);
	void (*unclaim)(u64 device_id, u64 hpa, u64 size);
};

int sbi_cvm_mmio_bitmap_register(const struct sbi_cvm_mmio_bitmap_ops *ops);
bool sbi_cvm_mmio_bitmap_available(void);
bool sbi_cvm_mmio_bitmap_active(void);

/* Serialized by cove_io_lock, with local interrupts disabled in M-mode. */
int sbi_cvm_mmio_reserve(const void *owner, u64 device_id, u64 hpa, u64 size,
			 bool protect);
int sbi_cvm_mmio_release(const void *owner);
bool sbi_cvm_mmio_reserved(const void *owner, u64 hpa, u64 size, bool protect);
void sbi_cvm_mmio_set_trusted(bool trusted);

#endif
