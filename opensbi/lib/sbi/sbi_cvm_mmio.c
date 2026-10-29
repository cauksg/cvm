/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/sbi_cvm_mmio.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hfence.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_tlb.h>
#include <sbi/riscv_barrier.h>

#define MMIO_PAGES ((SBI_CVM_MMIO_END - SBI_CVM_MMIO_BASE) >> \
		    SBI_CVM_MMIO_PAGE_SHIFT)

static u64 mmio_bitmap[MMIO_PAGES / 64] __aligned(4096);
static struct sbi_cvm_mmio_bitmap_ops bitmap_ops;
static bool registered, attached;

struct mmio_reservation {
	const void *owner;
	u64 device_id, hpa, size;
	bool protected;
};

static struct mmio_reservation reservations[SBI_CVM_MMIO_MAX_RESERVATIONS];

int sbi_cvm_mmio_bitmap_register(const struct sbi_cvm_mmio_bitmap_ops *ops)
{
	if (registered)
		return SBI_EALREADY;
	if (!ops || !ops->init || !ops->claim || !ops->sync ||
	    !ops->set_trusted || !ops->quiesce || !ops->unclaim)
		return SBI_EINVAL;
	if (sbi_hart_count() != 1)
		return SBI_ENOTSUPP;
	bitmap_ops = *ops;
	registered = true;
	return 0;
}

bool sbi_cvm_mmio_bitmap_available(void)
{
	return registered && sbi_hart_count() == 1;
}

bool sbi_cvm_mmio_bitmap_active(void)
{
	return attached;
}

static bool mmio_range_valid(u64 hpa, u64 size)
{
	return size && hpa >= SBI_CVM_MMIO_BASE && hpa < SBI_CVM_MMIO_END &&
	       size <= SBI_CVM_MMIO_END - hpa &&
	       !((hpa | size) & (SBI_CVM_MMIO_PAGE_SIZE - 1));
}

static void mmio_flush_cpu_translations(void)
{
	/* Include Host stage-1 translations, not just Guest G-stage entries. */
	__sbi_sfence_vma_all();
	__sbi_hfence_gvma_all();
	__sbi_hfence_vvma_all();
}

static void mmio_bitmap_update(u64 hpa, u64 size, bool protect)
{
	u64 first = (hpa - SBI_CVM_MMIO_BASE) >> SBI_CVM_MMIO_PAGE_SHIFT;
	u64 pages = size >> SBI_CVM_MMIO_PAGE_SHIFT;

	for (u64 page = first; page < first + pages; page++) {
		u64 mask = 1ULL << (page & 63);

		if (protect)
			mmio_bitmap[page >> 6] |= mask;
		else
			mmio_bitmap[page >> 6] &= ~mask;
	}
	mb();
	bitmap_ops.sync();
	mmio_flush_cpu_translations();
}

int sbi_cvm_mmio_reserve(const void *owner, u64 device_id, u64 hpa, u64 size,
			 bool protect)
{
	struct mmio_reservation *free = NULL;
	int ret;

	if (!owner || !mmio_range_valid(hpa, size))
		return SBI_EINVAL;
	if (sbi_hart_count() != 1 || (protect && !registered))
		return SBI_ENOTSUPP;
	for (unsigned int i = 0; i < SBI_CVM_MMIO_MAX_RESERVATIONS; i++) {
		struct mmio_reservation *entry = &reservations[i];

		if (!entry->owner) {
			if (!free)
				free = entry;
			continue;
		}
		/* Reject overlaps even for two TDIs owned by the same CVM. */
		if (entry->owner == owner ||
		    (hpa < entry->hpa + entry->size && entry->hpa < hpa + size))
			return SBI_EDENIED;
	}
	if (!free)
		return SBI_ENOSPC;
	if (protect) {
		if (!attached) {
			ret = bitmap_ops.init(mmio_bitmap, SBI_CVM_MMIO_BASE,
					      SBI_CVM_MMIO_END - SBI_CVM_MMIO_BASE);
			if (ret)
				return ret;
			bitmap_ops.set_trusted(false);
			mmio_flush_cpu_translations();
			attached = true;
		}
		ret = bitmap_ops.claim(device_id, hpa, size);
		if (ret)
			return ret;
		mmio_bitmap_update(hpa, size, true);
	}
	*free = (struct mmio_reservation) {
		.owner = owner, .device_id = device_id, .hpa = hpa,
		.size = size, .protected = protect,
	};
	return 0;
}

bool sbi_cvm_mmio_reserved(const void *owner, u64 hpa, u64 size, bool protect)
{
	for (unsigned int i = 0; i < SBI_CVM_MMIO_MAX_RESERVATIONS; i++) {
		struct mmio_reservation *entry = &reservations[i];

		if (owner && entry->owner == owner && entry->hpa == hpa &&
		    entry->size == size && entry->protected == protect)
			return true;
	}
	return false;
}

int sbi_cvm_mmio_release(const void *owner)
{
	for (unsigned int i = 0; i < SBI_CVM_MMIO_MAX_RESERVATIONS; i++) {
		struct mmio_reservation *entry = &reservations[i];
		int ret;

		if (!owner || entry->owner != owner)
			continue;
		if (entry->protected) {
			ret = bitmap_ops.quiesce(entry->device_id, entry->hpa,
						 entry->size);
			if (ret)
				return ret;
			mmio_bitmap_update(entry->hpa, entry->size, false);
			bitmap_ops.unclaim(entry->device_id, entry->hpa, entry->size);
		}
		sbi_memset(entry, 0, sizeof(*entry));
		return 0;
	}
	return 0;
}

void sbi_cvm_mmio_set_trusted(bool trusted)
{
	if (!attached)
		return;
	mb();
	bitmap_ops.set_trusted(trusted);
	/* Do not reuse permissions cached under the previous execution context. */
	mmio_flush_cpu_translations();
}
