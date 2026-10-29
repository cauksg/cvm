// SPDX-License-Identifier: GPL-2.0
/* Opt-in, coherent, per-e1000e simulator DMA using private GPA as IOVA.
 * The TDI grants its RAM window until STOP. DMA API unmap/free does not revoke
 * individual pages; STOP/VM teardown revokes the complete device grant.
 */
#include <linux/cove-io-dma.h>
#include <linux/dma-direct.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/scatterlist.h>
#include <asm/sbi.h>

static bool enabled;
struct cove_io_nic_state {
	unsigned long tdi;
};

static void nic_release(struct device *dev, void *res)
{
	set_dma_ops(dev, NULL);
}

static struct cove_io_nic_state *nic_state(struct device *dev)
{
	return devres_find(dev, nic_release, NULL, NULL);
}

static int __init cove_io_nic_setup(char *str)
{
	return kstrtobool(str, &enabled);
}
early_param("cove_io_nic", cove_io_nic_setup);

static int nic_prepare(struct device *dev, phys_addr_t phys, size_t size)
{
	phys_addr_t page, end;
	struct sbiret ret;
	struct cove_io_nic_state *state = nic_state(dev);

	if (!state || !size || phys + size < phys ||
	    !dma_capable(dev, phys, size, true) ||
	    phys_to_dma(dev, phys) != phys)
		return -EINVAL;
	end = PAGE_ALIGN(phys + size);
	if (end < phys + size)
		return -EINVAL;
	for (page = phys & PAGE_MASK; page < end; page += PAGE_SIZE) {
		/* Instantiate lazy private G-stage pages before Monitor lookup. */
		(void)READ_ONCE(*(u8 *)phys_to_virt(page));
		ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_PREPARE_DMA,
				state->tdi, page, PAGE_SIZE, 0, 0, 0);
		if (ret.error) {
			dev_err_ratelimited(dev, "COVE-IO private DMA preparation failed: %ld\n",
					    ret.error);
			return -EIO;
		}
	}
	return 0;
}

static void *nic_alloc(struct device *dev, size_t size, dma_addr_t *handle,
		       gfp_t gfp, unsigned long attrs)
{
	unsigned long addr;

	if (!size || (attrs & DMA_ATTR_NO_KERNEL_MAPPING))
		return NULL;
	addr = __get_free_pages(gfp | __GFP_ZERO, get_order(size));
	if (!addr)
		return NULL;
	*handle = virt_to_phys((void *)addr);
	if (nic_prepare(dev, *handle, size)) {
		free_pages(addr, get_order(size));
		return NULL;
	}
	return (void *)addr;
}

static void nic_free(struct device *dev, size_t size, void *addr,
		     dma_addr_t handle, unsigned long attrs)
{
	free_pages((unsigned long)addr, get_order(size));
}

static dma_addr_t nic_map(struct device *dev, phys_addr_t phys, size_t size,
			 enum dma_data_direction dir, unsigned long attrs)
{
	if (attrs & DMA_ATTR_MMIO || nic_prepare(dev, phys, size))
		return DMA_MAPPING_ERROR;
	return phys;
}

static int nic_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
		      enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		dma_addr_t addr = nic_map(dev, sg_phys(sg), sg->length, dir, attrs);

		if (addr == DMA_MAPPING_ERROR)
			return -EIO;
		sg_dma_address(sg) = addr;
		sg_dma_len(sg) = sg->length;
	}
	return nents;
}

static const struct dma_map_ops nic_dma_ops = {
	.alloc = nic_alloc,
	.free = nic_free,
	.map_phys = nic_map,
	.map_sg = nic_map_sg,
	.dma_supported = dma_direct_supported,
	.get_required_mask = dma_direct_get_required_mask,
};

int cove_io_nic_enable(struct pci_dev *pdev)
{
	struct sbi_cove_io_interface_report *report;
	struct cove_io_nic_state *state;
	struct sbiret ret;
	unsigned long cursor = 0;
	int i, err = -ENODEV;

	if (!enabled)
		return 0;
	if (pdev->vendor != 0x8086 || pdev->device != 0x10d3 || nic_state(&pdev->dev) ||
	    !dev_is_dma_coherent(&pdev->dev) || get_dma_ops(&pdev->dev))
		return -EINVAL;
	report = kzalloc(sizeof(*report), GFP_KERNEL);
	if (!report)
		return -ENOMEM;
	for (i = 0; i < 16; i++) {
		ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_GET_INTERFACE_ID,
				cursor, 0, 0, 0, 0, 0);
		if (ret.error || !ret.value)
			break;
		cursor = ret.value;
		ret = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_GET_INTERFACE_REPORT,
				cursor, virt_to_phys(report), sizeof(*report), 0, 0, 0);
		if (ret.error || report->mmio_gpa != pci_resource_start(pdev, 0) ||
		    report->mmio_size != pci_resource_len(pdev, 0) ||
		    (report->flags & (BIT_ULL(1) | BIT_ULL(4))) !=
				    (BIT_ULL(1) | BIT_ULL(4)) ||
		    report->state != SBI_COVG_INTERFACE_RUNNING)
			continue;
		/* Real SBI negative checks: no arbitrary TDI, MMIO, empty request,
		 * overflow or out-of-window GPA can prepare a DMA mapping.
		 */
#define DENIED(id, gpa, size) \
	(sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_PREPARE_DMA, \
		   id, gpa, size, 0, 0, 0).error != 0)
		if (!DENIED(~0UL, report->dma_gpa, PAGE_SIZE) ||
		    !DENIED(cursor, report->mmio_gpa, PAGE_SIZE) ||
		    !DENIED(cursor, report->dma_gpa, 0) ||
		    !DENIED(cursor, ~0UL & PAGE_MASK, PAGE_SIZE) ||
		    !DENIED(cursor, report->dma_gpa + report->dma_size, PAGE_SIZE)) {
			err = -EPERM;
			break;
		}
#undef DENIED
		state = devres_alloc(nic_release, sizeof(*state), GFP_KERNEL);
		if (!state) {
			err = -ENOMEM;
			break;
		}
		state->tdi = cursor;
		devres_add(&pdev->dev, state);
		set_dma_ops(&pdev->dev, &nic_dma_ops);
		err = 0;
		dev_info(&pdev->dev, "COVE-IO NIC: private DMA enabled TDI=%lu; negative checks passed\n",
				 cursor);
		break;
	}
	kfree(report);
	return err;
}
EXPORT_SYMBOL_GPL(cove_io_nic_enable);
