// SPDX-License-Identifier: GPL-2.0
/* The trusted controller protects complete 4 KiB PIO alias pages. Allocate
 * each IO resource on its own page; retain the actual hardware BAR size.
 * Monitor still rejects aliases, including Host-forged configurations.
 */
#include <linux/pci.h>
#include <linux/sizes.h>
#include <asm/sbi.h>

#ifdef CONFIG_RISCV_SBI
resource_size_t pcibios_align_resource(void *data, const struct resource *res,
				      resource_size_t size, resource_size_t align)
{
	struct sbiret ret;

	if (!(res->flags & IORESOURCE_IO))
		return res->start;
	/* Match the experimental probe used by pci-host-generic.c. */
	ret = sbi_ecall(0x20000217, 0x13,
			0, 0, 0, 0, 0, 0);
	if (ret.error || ret.value != 0x03010000)
		return res->start;
	return ALIGN(res->start, SZ_4K);
}
#endif
