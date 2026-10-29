// SPDX-License-Identifier: GPL-2.0
/* Opt-in integration test for the xs-cvm single-EDU direct I/O prototype. */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/pci.h>
#include <asm/sbi.h>

#define EDU_BUFFER 0x40000
#define EDU_DMA_IRQ 0x100
#define TEST_BYTES 256
#define DIRECT_MMIO BIT_ULL(4)

static bool enabled;
static int __init cove_io_edu_setup(char *str)
{
	return kstrtobool(str, &enabled);
}
early_param("cove_io_edu_test", cove_io_edu_setup);

struct edu_test {
	void __iomem *bar;
	struct completion done;
	atomic_t irqs;
	unsigned long page;
	struct sbi_cove_io_interface_report report;
};

static irqreturn_t edu_test_irq(int irq, void *opaque)
{
	struct edu_test *test = opaque;
	u32 status = readl(test->bar + 0x24);

	if (!status)
		return IRQ_NONE;
	writel(status, test->bar + 0x64);
	atomic_inc(&test->irqs);
	complete(&test->done);
	return IRQ_HANDLED;
}

static void edu_submit(struct edu_test *test, phys_addr_t pa, bool to_guest)
{
	reinit_completion(&test->done);
	writeq(to_guest ? EDU_BUFFER : pa, test->bar + 0x80);
	writeq(to_guest ? pa : EDU_BUFFER, test->bar + 0x88);
	writeq(TEST_BYTES, test->bar + 0x90);
	writeq(to_guest ? 7 : 5, test->bar + 0x98);
}

static int edu_transfer(struct edu_test *test, phys_addr_t pa, bool to_guest)
{
	int attempt;

	/* EDU does not retry a transaction dropped on the first IOMMU fault.
	 * Reissue after recovery; every attempt must reach the real IRQ handler.
	 */
	for (attempt = 0; attempt < 3; attempt++) {
		edu_submit(test, pa, to_guest);
		if (!wait_for_completion_timeout(&test->done, msecs_to_jiffies(3000)))
			return -ETIMEDOUT;
		if (readq(test->bar + 0x98) & 1)
			return -EBUSY;
		msleep(100);
	}
	return 0;
}

static int edu_find_interface(struct pci_dev *pdev, struct edu_test *test,
			      unsigned long *id)
{
	struct sbiret res;
	unsigned long cursor = 0;
	int i;

	for (i = 0; i < 16; i++) {
		res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_GET_INTERFACE_ID,
				cursor, 0, 0, 0, 0, 0);
		if (res.error || !res.value)
			return -ENODEV;
		cursor = res.value;
		res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_GET_INTERFACE_REPORT,
				cursor, virt_to_phys(&test->report),
				sizeof(test->report), 0, 0, 0);
		if (res.error)
			return -EIO;
		if (test->report.mmio_gpa == pci_resource_start(pdev, 0) &&
		    test->report.mmio_size == pci_resource_len(pdev, 0) &&
		    (test->report.flags & DIRECT_MMIO) &&
		    test->report.state == SBI_COVG_INTERFACE_RUNNING) {
			*id = cursor;
			return 0;
		}
	}
	return -ENODEV;
}

static int edu_test_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct edu_test *test;
	struct sbiret res;
	unsigned long tdi_id = 0;
	phys_addr_t pa;
	u8 *buffer;
	int i, ret, before;

	if (!enabled)
		return -ENODEV;
	test = devm_kzalloc(&pdev->dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;
	init_completion(&test->done);
	atomic_set(&test->irqs, 0);
	ret = pcim_enable_device(pdev);
	if (ret)
		goto fail;
	test->bar = pcim_iomap_region(pdev, 0, "cove-io-edu-test");
	if (IS_ERR(test->bar)) {
		ret = PTR_ERR(test->bar);
		goto fail;
	}
	ret = edu_find_interface(pdev, test, &tdi_id);
	if (ret)
		goto fail;
	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
	if (ret < 0)
		goto fail;
	ret = devm_request_irq(&pdev->dev, pci_irq_vector(pdev, 0),
			       edu_test_irq, 0, "cove-io-edu-test", test);
	if (ret)
		goto free_vectors;
	pci_set_master(pdev);
	/* Deliberately use a private CPU page, not dma_alloc_coherent/SWIOTLB.
	 * The simulator uses GPA as IOVA and the Monitor selects private HPA.
	 */
	test->page = devm_get_free_pages(&pdev->dev, GFP_KERNEL | __GFP_ZERO, 0);
	if (!test->page) {
		ret = -ENOMEM;
		goto free_irq;
	}
	buffer = (u8 *)test->page;
	pa = virt_to_phys(buffer);
	if (pa < test->report.dma_gpa ||
	    pa + PAGE_SIZE > test->report.dma_gpa + test->report.dma_size ||
	    (pa >= 0x82c00000 && pa < 0x82e00000)) {
		ret = -ERANGE;
		goto free_irq;
	}
	/* Private device G-stage starts empty. Ask the Monitor to authorize
	 * this resident private page before DMA, as the NIC DMA ops do.
	 */
	res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_SIM_PREPARE_DMA,
			tdi_id, pa, PAGE_SIZE, 0, 0, 0);
	if (res.error) {
		ret = -EIO;
		goto free_irq;
	}
	for (i = 0; i < TEST_BYTES; i++)
		buffer[i] = (u8)(i ^ 0xa7);
	ret = edu_transfer(test, pa, false);
	if (ret)
		goto free_irq;
	memset(buffer, 0x55, TEST_BYTES);
	ret = edu_transfer(test, pa, true);
	if (ret)
		goto free_irq;
	for (i = 0; i < TEST_BYTES; i++) {
		if (buffer[i] != (u8)(i ^ 0xa7)) {
			ret = -EIO;
			goto free_irq;
		}
	}
	pr_info("COVE-IO EDU test: private DMA roundtrip verified bytes=%d iova=%pa irq_handler=%d\n",
		TEST_BYTES, &pa, atomic_read(&test->irqs));

	/* Leave a real device-to-guest transaction pending across Guest STOP. */
	memset(buffer, 0x3c, TEST_BYTES);
	before = atomic_read(&test->irqs);
	edu_submit(test, pa, true);
	if (!(readq(test->bar + 0x98) & 1)) {
		ret = -EIO;
		goto free_irq;
	}
	res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_STOP_INTERFACE,
			tdi_id, 0, 0, 0, 0, 0);
	if (res.error) {
		ret = -EIO;
		goto free_irq;
	}
	/* No BAR access after STOP: Monitor has removed its G-stage mappings. */
	msleep(500);
	for (i = 0; i < TEST_BYTES; i++) {
		if (READ_ONCE(buffer[i]) != 0x3c) {
			ret = -EIO;
			goto fail;
		}
	}
	if (atomic_read(&test->irqs) != before) {
		ret = -EIO;
		goto fail;
	}
	res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_GET_INTERFACE_REPORT,
			tdi_id, virt_to_phys(&test->report), sizeof(test->report), 0, 0, 0);
	if (res.error || test->report.state != SBI_COVG_INTERFACE_ERROR ||
	    test->report.mmio_map_generation) {
		ret = -EIO;
		goto fail;
	}
	res = sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_START_INTERFACE,
			tdi_id, 0, 0, 0, 0, 0);
	if (res.error != SBI_ERR_INVALID_STATE) {
		ret = -EIO;
		goto fail;
	}
	pr_info("COVE-IO EDU test: PASS direct BAR private DMA IRQ and Guest STOP pending_write=blocked interrupt=blocked\n");
	return 0;
free_irq:
	/* Block pending DMA before devres releases the private page on failure. */
	if (tdi_id)
		sbi_ecall(SBI_EXT_COVG, SBI_EXT_COVG_STOP_INTERFACE,
			  tdi_id, 0, 0, 0, 0, 0);
	devm_free_irq(&pdev->dev, pci_irq_vector(pdev, 0), test);
free_vectors:
	pci_free_irq_vectors(pdev);
fail:
	pr_err("COVE-IO EDU test: FAIL error=%d\n", ret);
	return ret;
}

static const struct pci_device_id edu_test_ids[] = {
	{ PCI_DEVICE(0x1234, 0x11e8) },
	{ }
};
static struct pci_driver edu_test_driver = {
	.name = "cove-io-edu-test",
	.id_table = edu_test_ids,
	.probe = edu_test_probe,
};
static int __init cove_io_edu_init(void)
{
	return enabled ? pci_register_driver(&edu_test_driver) : 0;
}
late_initcall(cove_io_edu_init);
