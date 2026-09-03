#include "kvm/devices.h"
#include "kvm/cove-io.h"
#include "kvm/fdt.h"
#include "kvm/irq.h"
#include "kvm/kvm.h"
#include "kvm/kvm-cpu.h"
#include "kvm/util-init.h"
#include "kvm/vfio.h"
#include "kvm/virtio-mmio.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <linux/byteorder.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

struct isa_ext_info {
	const char *name;
	unsigned long ext_id;
	bool single_letter;
	bool min_enabled;
};

struct isa_ext_info isa_info_arr[] = {
	/* single-letter ordered canonically as "IEMAFDQCLBJTPVNSUHKORWXYZG" */
	{"i",		KVM_RISCV_ISA_EXT_I,	.single_letter = true, .min_enabled = true},
	{"m",		KVM_RISCV_ISA_EXT_M,	.single_letter = true, .min_enabled = true},
	{"a",		KVM_RISCV_ISA_EXT_A,	.single_letter = true, .min_enabled = true},
	{"f",		KVM_RISCV_ISA_EXT_F,	.single_letter = true, .min_enabled = true},
	{"d",		KVM_RISCV_ISA_EXT_D,	.single_letter = true, .min_enabled = true},
	{"c",		KVM_RISCV_ISA_EXT_C,	.single_letter = true, .min_enabled = true},
	{"v",		KVM_RISCV_ISA_EXT_V,	.single_letter = true},
	{"h",		KVM_RISCV_ISA_EXT_H,	.single_letter = true},
	/* multi-letter sorted alphabetically */
	{"smnpm",	KVM_RISCV_ISA_EXT_SMNPM},
	{"smstateen",	KVM_RISCV_ISA_EXT_SMSTATEEN},
	{"ssaia",	KVM_RISCV_ISA_EXT_SSAIA},
	{"sscofpmf",	KVM_RISCV_ISA_EXT_SSCOFPMF},
	{"ssnpm",	KVM_RISCV_ISA_EXT_SSNPM},
	{"sstc",	KVM_RISCV_ISA_EXT_SSTC},
	{"svade",	KVM_RISCV_ISA_EXT_SVADE},
	{"svadu",	KVM_RISCV_ISA_EXT_SVADU},
	{"svinval",	KVM_RISCV_ISA_EXT_SVINVAL},
	{"svnapot",	KVM_RISCV_ISA_EXT_SVNAPOT},
	{"svpbmt",	KVM_RISCV_ISA_EXT_SVPBMT},
	{"svvptc",	KVM_RISCV_ISA_EXT_SVVPTC},
	{"zabha",	KVM_RISCV_ISA_EXT_ZABHA},
	{"zacas",	KVM_RISCV_ISA_EXT_ZACAS},
	{"zawrs",	KVM_RISCV_ISA_EXT_ZAWRS},
	{"zba",		KVM_RISCV_ISA_EXT_ZBA},
	{"zbb",		KVM_RISCV_ISA_EXT_ZBB},
	{"zbc",		KVM_RISCV_ISA_EXT_ZBC},
	{"zbkb",	KVM_RISCV_ISA_EXT_ZBKB},
	{"zbkc",	KVM_RISCV_ISA_EXT_ZBKC},
	{"zbkx",	KVM_RISCV_ISA_EXT_ZBKX},
	{"zbs",		KVM_RISCV_ISA_EXT_ZBS},
	{"zca",		KVM_RISCV_ISA_EXT_ZCA},
	{"zcb",		KVM_RISCV_ISA_EXT_ZCB},
	{"zcd",		KVM_RISCV_ISA_EXT_ZCD},
	{"zcf",		KVM_RISCV_ISA_EXT_ZCF},
	{"zcmop",	KVM_RISCV_ISA_EXT_ZCMOP},
	{"zfa",		KVM_RISCV_ISA_EXT_ZFA},
	{"zfh",		KVM_RISCV_ISA_EXT_ZFH},
	{"zfhmin",	KVM_RISCV_ISA_EXT_ZFHMIN},
	{"zicbom",	KVM_RISCV_ISA_EXT_ZICBOM},
	{"zicboz",	KVM_RISCV_ISA_EXT_ZICBOZ},
	{"ziccrse",	KVM_RISCV_ISA_EXT_ZICCRSE},
	{"zicntr",	KVM_RISCV_ISA_EXT_ZICNTR},
	{"zicond",	KVM_RISCV_ISA_EXT_ZICOND},
	{"zicsr",	KVM_RISCV_ISA_EXT_ZICSR},
	{"zifencei",	KVM_RISCV_ISA_EXT_ZIFENCEI},
	{"zihintntl",	KVM_RISCV_ISA_EXT_ZIHINTNTL},
	{"zihintpause",	KVM_RISCV_ISA_EXT_ZIHINTPAUSE},
	{"zihpm",	KVM_RISCV_ISA_EXT_ZIHPM},
	{"zimop",	KVM_RISCV_ISA_EXT_ZIMOP},
	{"zknd",	KVM_RISCV_ISA_EXT_ZKND},
	{"zkne",	KVM_RISCV_ISA_EXT_ZKNE},
	{"zknh",	KVM_RISCV_ISA_EXT_ZKNH},
	{"zkr",		KVM_RISCV_ISA_EXT_ZKR},
	{"zksed",	KVM_RISCV_ISA_EXT_ZKSED},
	{"zksh",	KVM_RISCV_ISA_EXT_ZKSH},
	{"zkt",		KVM_RISCV_ISA_EXT_ZKT},
	{"ztso",	KVM_RISCV_ISA_EXT_ZTSO},
	{"zvbb",	KVM_RISCV_ISA_EXT_ZVBB},
	{"zvbc",	KVM_RISCV_ISA_EXT_ZVBC},
	{"zvfh",	KVM_RISCV_ISA_EXT_ZVFH},
	{"zvfhmin",	KVM_RISCV_ISA_EXT_ZVFHMIN},
	{"zvkb",	KVM_RISCV_ISA_EXT_ZVKB},
	{"zvkg",	KVM_RISCV_ISA_EXT_ZVKG},
	{"zvkned",	KVM_RISCV_ISA_EXT_ZVKNED},
	{"zvknha",	KVM_RISCV_ISA_EXT_ZVKNHA},
	{"zvknhb",	KVM_RISCV_ISA_EXT_ZVKNHB},
	{"zvksed",	KVM_RISCV_ISA_EXT_ZVKSED},
	{"zvksh",	KVM_RISCV_ISA_EXT_ZVKSH},
	{"zvkt",	KVM_RISCV_ISA_EXT_ZVKT},
};

static u64 cvm_cove_io_platform_irq_num(int nr_irqs)
{
	struct device_header *dev_hdr;
	u64 first_virtio_irq = KVM_IRQ_OFFSET + nr_irqs;
	bool found_virtio = false;

	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		struct virtio_mmio *vmmio = virtio_mmio__from_dev_hdr(dev_hdr);

		if (vmmio) {
			found_virtio = true;
			if (vmmio->irq < first_virtio_irq)
				first_virtio_irq = vmmio->irq;
		}

		dev_hdr = device__next_dev(dev_hdr);
	}

	if (!found_virtio)
		return nr_irqs;
	if (first_virtio_irq <= KVM_IRQ_OFFSET)
		return 0;

	return first_virtio_irq - KVM_IRQ_OFFSET;
}

static void cvm_cove_io_start_default_tdi(struct kvm *kvm)
{
	int nr_irqs = irq__get_nr_allocated_lines();
	u64 platform_irq_num = cvm_cove_io_platform_irq_num(nr_irqs);
	u64 tdi_id;

	if (cove_io__test_skip("default")) {
		pr_info("COVE-IO test: skipping default TDI");
		return;
	}

	tdi_id = cove_io__start_tdi(kvm, COVE_IO_DEFAULT_TDI_ID,
			      RISCV_IRQCHIP,
			      KVM_VIRTIO_MMIO_AREA - RISCV_IRQCHIP,
			      KVM_COVE_IO_DEVICE_ANY,
			      0, 0,
			      COVE_IO_IOMMU_GROUP_NONE,
			      platform_irq_num > 0 ? KVM_IRQ_OFFSET : 0,
			      platform_irq_num);

	pr_info("COVE-IO default TDI %llu started: mmio=0x%llx+0x%llx irq=%llu+%llu",
		(unsigned long long)tdi_id,
		(unsigned long long)RISCV_IRQCHIP,
		(unsigned long long)(KVM_VIRTIO_MMIO_AREA - RISCV_IRQCHIP),
		(unsigned long long)(platform_irq_num > 0 ? KVM_IRQ_OFFSET : 0),
		(unsigned long long)platform_irq_num);
}

static void cvm_cove_io_start_pci_tdi(struct kvm *kvm)
{
	u64 tdi_id;

	if (cove_io__test_skip("pci")) {
		pr_info("COVE-IO test: skipping PCI TDI");
		return;
	}

	tdi_id = cove_io__start_tdi(kvm, COVE_IO_PCI_TDI_ID,
			      KVM_PCI_CFG_AREA, RISCV_PCI_CFG_SIZE,
			      KVM_COVE_IO_DEVICE_ANY,
			      0, 0, COVE_IO_IOMMU_GROUP_NONE, 0, 0);

	pr_info("COVE-IO PCI TDI %llu started: mmio=0x%llx+0x%llx",
		(unsigned long long)tdi_id,
		(unsigned long long)KVM_PCI_CFG_AREA,
		(unsigned long long)RISCV_PCI_CFG_SIZE);
}

static u64 cvm_cove_io_start_virtio_tdis(struct kvm *kvm, u64 first_tdi_id)
{
	struct device_header *dev_hdr;
	u64 tdi_id = first_tdi_id;
	bool skip_virtio = cove_io__test_skip("virtio") ||
			   cove_io__test_skip("virtio-mmio");
	bool skip_dma = cove_io__test_skip("virtio-dma");
	bool skip_irq = cove_io__test_skip("virtio-irq");

	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		struct virtio_mmio *vmmio = virtio_mmio__from_dev_hdr(dev_hdr);

		if (vmmio) {
			if (skip_virtio) {
				pr_info("COVE-IO test: skipping virtio-mmio TDI %llu for mmio=0x%llx irq=%llu",
					(unsigned long long)tdi_id,
					(unsigned long long)vmmio->addr,
					(unsigned long long)vmmio->irq);
				tdi_id++;
				dev_hdr = device__next_dev(dev_hdr);
				continue;
			}

			tdi_id = cove_io__start_tdi(kvm, tdi_id,
					      vmmio->addr, VIRTIO_MMIO_IO_SIZE,
					      cove_io__virtio_mmio_device_id(vmmio->addr),
					      CVM_VIRTIO_RMEM_ADDR,
					      skip_dma ? 0 : CVM_VIRTIO_RMEM_SIZE,
					      COVE_IO_IOMMU_GROUP_NONE,
					      vmmio->irq,
					      skip_irq ? 0 : 1);
			pr_info("COVE-IO virtio-mmio TDI %llu started: mmio=0x%llx+0x%llx dma=0x%llx+0x%llx irq=%llu+%llu",
				(unsigned long long)tdi_id,
				(unsigned long long)vmmio->addr,
				(unsigned long long)VIRTIO_MMIO_IO_SIZE,
				(unsigned long long)CVM_VIRTIO_RMEM_ADDR,
				(unsigned long long)(skip_dma ? 0 : CVM_VIRTIO_RMEM_SIZE),
				(unsigned long long)vmmio->irq,
				(unsigned long long)(skip_irq ? 0 : 1));
			tdi_id++;
		}

		dev_hdr = device__next_dev(dev_hdr);
	}

	return tdi_id;
}

static void cvm_cove_io_probe_tdis(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	bool skip_virtio = cove_io__test_skip("virtio") ||
			   cove_io__test_skip("virtio-mmio");
	bool skip_dma = cove_io__test_skip("virtio-dma");
	bool skip_irq = cove_io__test_skip("virtio-irq");
	struct kvm_cove_io_tdi tdi;

	if (!cove_io__test_probe_enabled())
		return;

	memset(&tdi, 0, sizeof(tdi));
	tdi.op = KVM_COVE_IO_TDI_FIND_MMIO;
	tdi.mmio_gpa = RISCV_IRQCHIP;
	tdi.mmio_size = 4;
	cove_io__expect_probe(kvm, tdi,
				 !cove_io__test_skip("default"),
				 "default-mmio");

	memset(&tdi, 0, sizeof(tdi));
	tdi.op = KVM_COVE_IO_TDI_FIND_MMIO;
	tdi.mmio_gpa = KVM_PCI_CFG_AREA;
	tdi.mmio_size = 4;
	cove_io__expect_probe(kvm, tdi,
				 !cove_io__test_skip("pci"),
				 "pci-mmio");

	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		struct virtio_mmio *vmmio = virtio_mmio__from_dev_hdr(dev_hdr);

		if (vmmio) {
			memset(&tdi, 0, sizeof(tdi));
			tdi.op = KVM_COVE_IO_TDI_FIND_MMIO;
			tdi.mmio_gpa = vmmio->addr;
			tdi.mmio_size = 4;
			cove_io__expect_probe(kvm, tdi, !skip_virtio,
						 "virtio-mmio");

			memset(&tdi, 0, sizeof(tdi));
			tdi.op = KVM_COVE_IO_TDI_FIND_DMA;
			tdi.device_id = cove_io__virtio_mmio_device_id(vmmio->addr);
			tdi.dma_gpa = CVM_VIRTIO_RMEM_ADDR;
			cove_io__expect_probe(kvm, tdi,
						 !skip_virtio && !skip_dma,
						 "virtio-dma");

			memset(&tdi, 0, sizeof(tdi));
			tdi.op = KVM_COVE_IO_TDI_FIND_DMA;
			tdi.device_id = COVE_IO_DEVICE_TEST_WRONG;
			tdi.dma_gpa = CVM_VIRTIO_RMEM_ADDR;
			cove_io__expect_probe(kvm, tdi, false,
						 "virtio-dma-wrong-device");

			memset(&tdi, 0, sizeof(tdi));
			tdi.op = KVM_COVE_IO_TDI_FIND_DMA;
			tdi.device_id =
				cove_io__device_id(COVE_IO_DEVICE_TEST_UNKNOWN_TYPE,
						    vmmio->addr);
			tdi.dma_gpa = CVM_VIRTIO_RMEM_ADDR;
			cove_io__expect_probe(kvm, tdi, false,
						 "virtio-dma-unknown-type");

			memset(&tdi, 0, sizeof(tdi));
			tdi.op = KVM_COVE_IO_TDI_FIND_IRQ;
			tdi.irq_id = vmmio->irq;
			tdi.irq_iid = COVE_IO_IRQ_IID_ANY;
			cove_io__expect_probe(kvm, tdi,
						 !skip_virtio && !skip_irq,
						 "virtio-irq");
		}

		dev_hdr = device__next_dev(dev_hdr);
	}
}

static bool __isa_ext_disabled(struct kvm *kvm, struct isa_ext_info *info)
{
	if (kvm->cfg.arch.cpu_type == RISCV__CPU_TYPE_MIN &&
	    !info->min_enabled)
		return true;

	return kvm->cfg.arch.ext_disabled[info->ext_id];
}

static bool __isa_ext_warn_disable_failure(struct kvm *kvm, struct isa_ext_info *info)
{
	if (kvm->cfg.arch.cpu_type == RISCV__CPU_TYPE_MIN &&
	    !info->min_enabled)
		return false;

	return true;
}

static void __min_enable(const char *ext, size_t ext_len)
{
	struct isa_ext_info *info;
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(isa_info_arr); i++) {
		info = &isa_info_arr[i];
		if (strlen(info->name) != ext_len)
			continue;
		if (!strncmp(ext, info->name, ext_len))
			info->min_enabled = true;
	}
}

bool riscv__isa_extension_disabled(struct kvm *kvm, unsigned long isa_ext_id)
{
	struct isa_ext_info *info = NULL;
	unsigned long i;

	for (i = 0; i < ARRAY_SIZE(isa_info_arr); i++) {
		if (isa_info_arr[i].ext_id == isa_ext_id) {
			info = &isa_info_arr[i];
			break;
		}
	}
	if (!info)
		return true;

	return __isa_ext_disabled(kvm, info);
}

int riscv__cpu_type_parser(const struct option *opt, const char *arg, int unset)
{
	struct kvm *kvm = opt->ptr;
	const char *str, *nstr;
	int len;

	if (strncmp(arg, "min", 3) && (strncmp(arg, "max", 3) || strlen(arg) != 3))
		die("Invalid CPU type %s\n", arg);

	if (!strcmp(arg, "max")) {
		kvm->cfg.arch.cpu_type = RISCV__CPU_TYPE_MAX;
	} else {
		kvm->cfg.arch.cpu_type = RISCV__CPU_TYPE_MIN;

		str = arg;
		str += 3;
		while (*str) {
			if (*str == ',') {
				str++;
				continue;
			}

			nstr = strchr(str, ',');
			if (!nstr)
				nstr = str + strlen(str);

			len = nstr - str;
			if (len) {
				__min_enable(str, len);
				str += len;
			}
		}
	}

	return 0;
}

static void dump_fdt(const char *dtb_file, void *fdt)
{
	int count, fd;

	fd = open(dtb_file, O_CREAT | O_TRUNC | O_RDWR, 0666);
	if (fd < 0)
		die("Failed to write dtb to %s", dtb_file);

	count = write(fd, fdt, FDT_MAX_SIZE);
	if (count < 0)
		die_perror("Failed to dump dtb");

	pr_debug("Wrote %d bytes to dtb %s", count, dtb_file);
	close(fd);
}

#define CPU_NAME_MAX_LEN 15
static void generate_cpu_nodes(void *fdt, struct kvm *kvm)
{
	unsigned long cbom_blksz = 0, cboz_blksz = 0, satp_mode = 0;
	int i, cpu, pos, arr_sz = ARRAY_SIZE(isa_info_arr);

	_FDT(fdt_begin_node(fdt, "cpus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x1));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x0));
	_FDT(fdt_property_cell(fdt, "timebase-frequency",
				kvm->cpus[0]->riscv_timebase));

	for (cpu = 0; cpu < kvm->nrcpus; ++cpu) {
		char cpu_name[CPU_NAME_MAX_LEN];
#define CPU_ISA_MAX_LEN (ARRAY_SIZE(isa_info_arr) * 16)
		char cpu_isa[CPU_ISA_MAX_LEN];
		struct kvm_cpu *vcpu = kvm->cpus[cpu];
		struct kvm_one_reg reg;
		unsigned long isa_ext_out = 0;

		snprintf(cpu_name, CPU_NAME_MAX_LEN, "cpu@%x", cpu);

		snprintf(cpu_isa, CPU_ISA_MAX_LEN, "rv%ld", vcpu->riscv_xlen);
		pos = strlen(cpu_isa);

		for (i = 0; i < arr_sz; i++) {
			reg.id = RISCV_ISA_EXT_REG(isa_info_arr[i].ext_id);
			reg.addr = (unsigned long)&isa_ext_out;
			if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
				continue;
			if (!isa_ext_out)
				/* This extension is not available in hardware */
				continue;

			if (__isa_ext_disabled(kvm, &isa_info_arr[i])) {
				isa_ext_out = 0;
				if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0 &&
				    __isa_ext_warn_disable_failure(kvm, &isa_info_arr[i]))
					pr_warning("Failed to disable %s ISA exension\n",
						   isa_info_arr[i].name);
				continue;
			}

			if (isa_info_arr[i].ext_id == KVM_RISCV_ISA_EXT_ZICBOM && !cbom_blksz) {
				reg.id = RISCV_CONFIG_REG(zicbom_block_size);
				reg.addr = (unsigned long)&cbom_blksz;
				if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
					die("KVM_GET_ONE_REG failed (config.zicbom_block_size)");
			}

			if (isa_info_arr[i].ext_id == KVM_RISCV_ISA_EXT_ZICBOZ && !cboz_blksz) {
				reg.id = RISCV_CONFIG_REG(zicboz_block_size);
				reg.addr = (unsigned long)&cboz_blksz;
				if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
					die("KVM_GET_ONE_REG failed (config.zicboz_block_size)");
			}

			if ((strlen(isa_info_arr[i].name) + pos + 1) >= CPU_ISA_MAX_LEN) {
				pr_warning("Insufficient space to append ISA exension %s\n",
					   isa_info_arr[i].name);
				break;
			}

			pos += snprintf(cpu_isa + pos, CPU_ISA_MAX_LEN - pos, "%s%s",
					isa_info_arr[i].single_letter ? "" : "_",
					isa_info_arr[i].name);
		}
		cpu_isa[pos] = '\0';

		reg.id = RISCV_CONFIG_REG(satp_mode);
		reg.addr = (unsigned long)&satp_mode;
		if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
			satp_mode = (vcpu->riscv_xlen == 64) ? 8 : 1;

		_FDT(fdt_begin_node(fdt, cpu_name));
		_FDT(fdt_property_string(fdt, "device_type", "cpu"));
		_FDT(fdt_property_string(fdt, "compatible", "riscv"));
		if (vcpu->riscv_xlen == 64) {
			switch (satp_mode) {
			case 10:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,sv57"));
				break;
			case 9:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,sv48"));
				break;
			case 8:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,sv39"));
				break;
			default:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,none"));
				break;
			}
		} else {
			switch (satp_mode) {
			case 1:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,sv32"));
				break;
			default:
				_FDT(fdt_property_string(fdt, "mmu-type",
							 "riscv,none"));
				break;
			}
		}
		_FDT(fdt_property_string(fdt, "riscv,isa", cpu_isa));
		if (cbom_blksz)
			_FDT(fdt_property_cell(fdt, "riscv,cbom-block-size", cbom_blksz));
		if (cboz_blksz)
			_FDT(fdt_property_cell(fdt, "riscv,cboz-block-size", cboz_blksz));
		_FDT(fdt_property_cell(fdt, "reg", cpu));
		_FDT(fdt_property_string(fdt, "status", "okay"));

		_FDT(fdt_begin_node(fdt, "interrupt-controller"));
		_FDT(fdt_property_string(fdt, "compatible", "riscv,cpu-intc"));
		_FDT(fdt_property_cell(fdt, "#interrupt-cells", 1));
		_FDT(fdt_property(fdt, "interrupt-controller", NULL, 0));
		_FDT(fdt_property_cell(fdt, "phandle",
					PHANDLE_CPU_INTC_BASE + cpu));
		_FDT(fdt_end_node(fdt));

		_FDT(fdt_end_node(fdt));
	}

	_FDT(fdt_end_node(fdt));
}

static void cvm_generate_reserved_mem_node(void *fdt)
{
	u64 virtio_rmem_reg[] = {
		cpu_to_fdt64(CVM_VIRTIO_RMEM_ADDR),
		cpu_to_fdt64(CVM_VIRTIO_RMEM_SIZE),
	};
	u64 vfio_rmem_reg[] = {
		cpu_to_fdt64(CVM_VFIO_RMEM_ADDR),
		cpu_to_fdt64(CVM_VFIO_RMEM_SIZE),
	};

	_FDT(fdt_begin_node(fdt, "reserved-memory"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));
	_FDT(fdt_property(fdt, "ranges", NULL, 0));
	_FDT(fdt_begin_node(fdt, "restricted-dma-pool@82c00000"));
	_FDT(fdt_property_string(fdt, "compatible", "restricted-dma-pool"));
	_FDT(fdt_property(fdt, "reg", virtio_rmem_reg,
			  sizeof(virtio_rmem_reg)));
	_FDT(fdt_property_cell(fdt, "phandle",
			       PHANDLE_CVM_VIRTIO_RESTRICTED_DMA));
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_begin_node(fdt, "restricted-dma-pool@82d00000"));
	_FDT(fdt_property_string(fdt, "compatible", "restricted-dma-pool"));
	_FDT(fdt_property(fdt, "reg", vfio_rmem_reg, sizeof(vfio_rmem_reg)));
	_FDT(fdt_property_cell(fdt, "phandle",
			       PHANDLE_CVM_VFIO_RESTRICTED_DMA));
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_end_node(fdt));
}

static u64 cvm_cove_io_first_virtio_device_id(void)
{
	struct device_header *dev_hdr;

	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		struct virtio_mmio *vmmio = virtio_mmio__from_dev_hdr(dev_hdr);

		if (vmmio)
			return cove_io__virtio_mmio_device_id(vmmio->addr);

		dev_hdr = device__next_dev(dev_hdr);
	}

	return KVM_COVE_IO_DEVICE_ANY;
}

static void cvm_set_swiotlb(struct kvm *kvm, u64 addr, u64 size,
			    u64 device_id, u64 iommu_group)
{
	struct swiotlb sw = {
		.addr = addr,
		.size = size,
		.device_id = device_id,
		.iommu_group = iommu_group,
	};

	if (ioctl(kvm->vm_fd, KVM_SET_SWIOTLB, &sw))
		die_perror("KVM_SET_SWIOTLB ioctl");
}

static void cvm_load_fdt(struct kvm *kvm)
{
	struct load_file load_fdt;
	u64 next_tdi_id;

	load_fdt.src_hva = (unsigned long)guest_flat_to_host(kvm,
							     kvm->arch.dtb_guest_start);
	load_fdt.des_gpa = kvm->arch.dtb_guest_start;
	load_fdt.file_size = FDT_MAX_SIZE;
	if (ioctl(kvm->vm_fd, KVM_LOAD_FILE, &load_fdt))
		die_perror("KVM_LOAD_FILE fdt ioctl");

	cvm_set_swiotlb(kvm, CVM_VIRTIO_RMEM_ADDR, CVM_VIRTIO_RMEM_SIZE,
			cvm_cove_io_first_virtio_device_id(),
			COVE_IO_IOMMU_GROUP_NONE);
	vfio__register_cove_io_swiotlb(kvm,
		cove_io__direct_dma_enabled() ? kvm->arch.memory_guest_start :
			CVM_VFIO_RMEM_ADDR,
		cove_io__direct_dma_enabled() ? kvm->ram_size :
			CVM_VFIO_RMEM_SIZE);

	cvm_cove_io_start_default_tdi(kvm);
	cvm_cove_io_start_pci_tdi(kvm);
	next_tdi_id = vfio__start_cove_io_tdis(kvm,
			cvm_cove_io_start_virtio_tdis(kvm,
						      COVE_IO_FIRST_DEVICE_TDI_ID));
	if (cove_io__lifecycle_test_enabled()) {
		next_tdi_id = cove_io__start_tdi(kvm, next_tdi_id,
						 COVE_IO_SIM_TEST_MMIO_GPA,
						 COVE_IO_SIM_TEST_MMIO_SIZE,
						 KVM_COVE_IO_DEVICE_ANY,
						 0, 0, COVE_IO_IOMMU_GROUP_NONE,
						 0, 0);
		pr_info("COVE-IO simulator lifecycle TDI %llu: mmio=0x%llx+0x%llx",
			(unsigned long long)next_tdi_id,
			(unsigned long long)COVE_IO_SIM_TEST_MMIO_GPA,
			(unsigned long long)COVE_IO_SIM_TEST_MMIO_SIZE);
	}
	cvm_cove_io_probe_tdis(kvm);
	vfio__probe_cove_io_tdis(kvm);
}

static int setup_fdt(struct kvm *kvm)
{
	struct device_header *dev_hdr;
	u8 staging_fdt[FDT_MAX_SIZE];
	u64 mem_reg_prop[]	= {
		cpu_to_fdt64(kvm->arch.memory_guest_start),
		cpu_to_fdt64(kvm->ram_size),
	};
	char *str;
	void *fdt		= staging_fdt;
	void *fdt_dest		= guest_flat_to_host(kvm,
						     kvm->arch.dtb_guest_start);
	void (*generate_mmio_fdt_nodes)(void *, struct device_header *,
					void (*)(void *, u8, enum irq_type));

	/* Create new tree without a reserve map */
	_FDT(fdt_create(fdt, FDT_MAX_SIZE));
	_FDT(fdt_finish_reservemap(fdt));

	/* Header */
	_FDT(fdt_begin_node(fdt, ""));
	_FDT(fdt_property_string(fdt, "compatible", "linux,dummy-virt"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));

	/* /chosen */
	_FDT(fdt_begin_node(fdt, "chosen"));

	/* Pass on our amended command line to a Linux kernel only. */
	if (kvm->cfg.firmware_filename) {
		if (kvm->cfg.kernel_cmdline)
			_FDT(fdt_property_string(fdt, "bootargs",
						 kvm->cfg.kernel_cmdline));
	} else if (kvm->cfg.real_cmdline) {
		_FDT(fdt_property_string(fdt, "bootargs",
					 kvm->cfg.real_cmdline));
	}

	_FDT(fdt_property_string(fdt, "stdout-path", "serial0"));

	/* Initrd */
	if (kvm->arch.initrd_size != 0) {
		u64 ird_st_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start);
		u64 ird_end_prop = cpu_to_fdt64(kvm->arch.initrd_guest_start +
					       kvm->arch.initrd_size);

		_FDT(fdt_property(fdt, "linux,initrd-start",
				   &ird_st_prop, sizeof(ird_st_prop)));
		_FDT(fdt_property(fdt, "linux,initrd-end",
				   &ird_end_prop, sizeof(ird_end_prop)));
	}

	_FDT(fdt_end_node(fdt));

	/* Memory */
	_FDT(fdt_begin_node(fdt, "memory"));
	_FDT(fdt_property_string(fdt, "device_type", "memory"));
	_FDT(fdt_property(fdt, "reg", mem_reg_prop, sizeof(mem_reg_prop)));
	_FDT(fdt_end_node(fdt));

	/* CPUs */
	generate_cpu_nodes(fdt, kvm);

	if (kvm->cfg.cmode)
		cvm_generate_reserved_mem_node(fdt);
	riscv_cove_io_restricted_dma = kvm->cfg.cmode;

	/* IRQCHIP */
	if (!riscv_irqchip_generate_fdt_node)
		die("No way to generate IRQCHIP FDT node\n");
	riscv_irqchip_generate_fdt_node(fdt, kvm);

	/* Simple Bus */
	_FDT(fdt_begin_node(fdt, "smb"));
	_FDT(fdt_property_string(fdt, "compatible", "simple-bus"));
	_FDT(fdt_property_cell(fdt, "#address-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "#size-cells", 0x2));
	_FDT(fdt_property_cell(fdt, "interrupt-parent",
			       riscv_irqchip_phandle));
	_FDT(fdt_property(fdt, "ranges", NULL, 0));

	/* Virtio MMIO devices */
	dev_hdr = device__first_dev(DEVICE_BUS_MMIO);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr,
					riscv__generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* IOPORT devices */
	dev_hdr = device__first_dev(DEVICE_BUS_IOPORT);
	while (dev_hdr) {
		generate_mmio_fdt_nodes = dev_hdr->data;
		generate_mmio_fdt_nodes(fdt, dev_hdr,
					riscv__generate_irq_prop);
		dev_hdr = device__next_dev(dev_hdr);
	}

	/* PCI host controller */
	pci__generate_fdt_nodes(fdt);

	_FDT(fdt_end_node(fdt));

	if (fdt_stdout_path) {
		str = malloc(strlen(fdt_stdout_path) + strlen("/smb") + 1);
		sprintf(str, "/smb%s", fdt_stdout_path);
		free(fdt_stdout_path);
		fdt_stdout_path = NULL;

		_FDT(fdt_begin_node(fdt, "aliases"));
		_FDT(fdt_property_string(fdt, "serial0", str));
		_FDT(fdt_end_node(fdt));
		free(str);
	}

	/* Finalise. */
	_FDT(fdt_end_node(fdt));
	_FDT(fdt_finish(fdt));

	_FDT(fdt_open_into(fdt, fdt_dest, FDT_MAX_SIZE));
	_FDT(fdt_pack(fdt_dest));

	if (kvm->cfg.arch.dump_dtb_filename)
		dump_fdt(kvm->cfg.arch.dump_dtb_filename, fdt_dest);

	if (kvm->cfg.cmode) {
		cvm_load_fdt(kvm);
		if (cove_io__test_probe_enabled())
			return INIT_LIST_STOP;
	}
	return 0;
}
late_init(setup_fdt);
