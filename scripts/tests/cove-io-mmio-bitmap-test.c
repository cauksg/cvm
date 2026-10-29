/* Actual Monitor bitmap manager, exercised against a test-only hardware contract. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sbi/sbi_cvm_mmio.h>
#include <sbi/sbi_error.h>

static const u64 *map;
static u64 enforced[4096];
static unsigned int harts = 1, sfences, gfences, vfences;
static int init_error, claim_error, stop_error, claims, unclaims, syncs;
static bool trusted;
static int owners[17];

unsigned int sbi_hart_count(void) { return harts; }
void __sbi_sfence_vma_all(void) { sfences++; }
void __sbi_hfence_gvma_all(void) { gfences++; }
void __sbi_hfence_vvma_all(void) { vfences++; }

static bool bit(u64 hpa)
{
	u64 page = (hpa - SBI_CVM_MMIO_BASE) >> 12;

	return (enforced[page >> 6] >> (page & 63)) & 1;
}

static int hw_init(const u64 *bitmap, unsigned long base, unsigned long size)
{
	assert(base == 0x40000000 && size == 0x40000000);
	assert(!((uintptr_t)bitmap & 4095));
	if (init_error)
		return init_error;
	map = bitmap;
	return 0;
}

static int hw_claim(u64 device, u64 hpa, u64 size)
{
	assert(device && !(hpa & 4095) && !(size & 4095));
	if (claim_error)
		return claim_error;
	claims++;
	return 0;
}

static void hw_sync(void)
{
	assert(map);
	memcpy(enforced, map, sizeof(enforced));
	syncs++;
}

static void hw_mode(bool value) { trusted = value; }

static int hw_stop(u64 device, u64 hpa, u64 size)
{
	assert(device && size && bit(hpa) && bit(hpa + size - 4096));
	return stop_error;
}

static void hw_unclaim(u64 device, u64 hpa, u64 size)
{
	assert(device && !bit(hpa) && !bit(hpa + size - 4096));
	unclaims++;
}

int main(void)
{
	const struct sbi_cvm_mmio_bitmap_ops ops = {
		.init = hw_init, .claim = hw_claim, .sync = hw_sync,
		.set_trusted = hw_mode, .quiesce = hw_stop, .unclaim = hw_unclaim,
	};
	u64 bar = 0x50000000;
	unsigned int before;

	assert(!sbi_cvm_mmio_bitmap_available());
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, 0x100000, true) == SBI_ENOTSUPP);
	assert(sbi_cvm_mmio_bitmap_register(NULL) == SBI_EINVAL);
	harts = 2;
	assert(sbi_cvm_mmio_bitmap_register(&ops) == SBI_ENOTSUPP);
	harts = 1;
	assert(!sbi_cvm_mmio_bitmap_register(&ops));
	assert(sbi_cvm_mmio_bitmap_register(&ops) == SBI_EALREADY);
	assert(sbi_cvm_mmio_bitmap_available());
	assert(sbi_cvm_mmio_reserve(NULL, 1, bar, 4096, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar + 1, 4096, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, 1024, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, 0, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, 0x3ffff000, 4096, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, 0x80000000, 4096, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, 0x7ffff000, 8192, true) == SBI_EINVAL);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, ~0ULL, true) == SBI_EINVAL);
	init_error = -17;
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, 0x100000, true) == -17);
	assert(!map && !claims && !syncs);
	init_error = 0;
	claim_error = -18;
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar, 0x100000, true) == -18);
	assert(map && !claims && !syncs && !trusted);
	assert(!sbi_cvm_mmio_reserved(&owners[0], bar, 0x100000, true));
	claim_error = 0;
	assert(!sbi_cvm_mmio_reserve(&owners[0], 1, bar, 0x100000, true));
	assert(claims == 1 && syncs == 1);
	for (unsigned int i = 0; i < 256; i++)
		assert(bit(bar + i * 4096));
	assert(!bit(bar - 4096) && !bit(bar + 0x100000));
	assert(sbi_cvm_mmio_reserved(&owners[0], bar, 0x100000, true));
	assert(!sbi_cvm_mmio_reserved(&owners[1], bar, 0x100000, true));
	assert(!sbi_cvm_mmio_reserved(&owners[0], bar, 0x100000, false));
	assert(sbi_cvm_mmio_reserve(&owners[1], 2, bar, 4096, true) == SBI_EDENIED);
	assert(sbi_cvm_mmio_reserve(&owners[1], 2, bar + 0xff000, 8192, false) == SBI_EDENIED);
	assert(sbi_cvm_mmio_reserve(&owners[0], 1, bar + 0x100000, 4096, true) == SBI_EDENIED);
	before = sfences;
	sbi_cvm_mmio_set_trusted(true);
	assert(trusted && sfences == before + 1 && bit(bar));
	sbi_cvm_mmio_set_trusted(false);
	assert(!trusted && sfences == before + 2 && bit(bar));
	stop_error = -19;
	assert(sbi_cvm_mmio_release(&owners[0]) == -19);
	assert(bit(bar) && unclaims == 0);
	assert(sbi_cvm_mmio_reserved(&owners[0], bar, 0x100000, true));
	stop_error = 0;
	assert(!sbi_cvm_mmio_release(&owners[1]));
	assert(bit(bar));
	assert(!sbi_cvm_mmio_release(&owners[0]));
	assert(unclaims == 1 && syncs == 2 && !bit(bar));
	assert(!sbi_cvm_mmio_release(&owners[0]) && unclaims == 1);
	/* Exercise u64 bit 63 and both ends of the aperture. */
	assert(!sbi_cvm_mmio_reserve(&owners[0], 1, 0x40000000, 64 * 4096, true));
	assert(bit(0x4003f000));
	assert(!sbi_cvm_mmio_reserve(&owners[1], 2, 0x7ffff000, 4096, true));
	assert(bit(0x7ffff000));
	assert(!sbi_cvm_mmio_release(&owners[0]));
	assert(!sbi_cvm_mmio_release(&owners[1]));
	/* Adjacent reservations and capacity exhaustion, including legacy mode. */
	for (int i = 0; i < 16; i++)
		assert(!sbi_cvm_mmio_reserve(&owners[i], i + 1,
					     bar + i * 4096, 4096, i & 1));
	assert(sbi_cvm_mmio_reserve(&owners[16], 17, bar + 16 * 4096,
				     4096, true) == SBI_ENOSPC);
	for (int i = 0; i < 16; i++)
		assert(!sbi_cvm_mmio_release(&owners[i]));
	for (unsigned int i = 0; i < 4096; i++)
		assert(!enforced[i]);
	assert(sfences == gfences && gfences == vfences);
	puts("PASS: MMIO bitmap alignment, 256 pages, ownership, mode switches, rollback and quiesce-before-release");
}
