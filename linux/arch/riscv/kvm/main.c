// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Anup Patel <anup.patel@wdc.com>
 */

#include <linux/errno.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/kvm_host.h>
#include <asm/csr.h>
#include <asm/hwcap.h>
#include <asm/sbi.h>
#include <linux/vmalloc.h>
#include <cvm/iie-cvm-sbi.h>
#include <linux/mm.h>

long kvm_arch_dev_ioctl(struct file *filp,
			unsigned int ioctl, unsigned long arg)
{
	return -EINVAL;
}

int kvm_arch_hardware_enable(void)
{
	unsigned long hideleg, hedeleg;

	hedeleg = 0;
	hedeleg |= (1UL << EXC_INST_MISALIGNED);
	hedeleg |= (1UL << EXC_BREAKPOINT);
	hedeleg |= (1UL << EXC_SYSCALL);
	hedeleg |= (1UL << EXC_INST_PAGE_FAULT);
	hedeleg |= (1UL << EXC_LOAD_PAGE_FAULT);
	hedeleg |= (1UL << EXC_STORE_PAGE_FAULT);
	csr_write(CSR_HEDELEG, hedeleg);

	hideleg = 0;
	hideleg |= (1UL << IRQ_VS_SOFT);
	hideleg |= (1UL << IRQ_VS_TIMER);
	hideleg |= (1UL << IRQ_VS_EXT);
	csr_write(CSR_HIDELEG, hideleg);

	/* VS should access only the time counter directly. Everything else should trap */
	csr_write(CSR_HCOUNTEREN, 0x02);

	csr_write(CSR_HVIP, 0);

	kvm_riscv_aia_enable();

	return 0;
}

void kvm_arch_hardware_disable(void)
{
	kvm_riscv_aia_disable();

	/*
	 * After clearing the hideleg CSR, the host kernel will receive
	 * spurious interrupts if hvip CSR has pending interrupts and the
	 * corresponding enable bits in vsie CSR are asserted. To avoid it,
	 * hvip CSR and vsie CSR must be cleared before clearing hideleg CSR.
	 */
	csr_write(CSR_VSIE, 0);
	csr_write(CSR_HVIP, 0);
	csr_write(CSR_HEDELEG, 0);
	csr_write(CSR_HIDELEG, 0);
}

unsigned long kernel_page_translate(unsigned long addr){
	pgd_t *pgd;
	pud_t *pud;
	p4d_t *p4d;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset_k(addr);
	if (!pgd_present(*pgd))
		return false;
	if (pgd_leaf(*pgd))
		return true;

	p4d = p4d_offset(pgd, addr);
	if (!p4d_present(*p4d))
		return false;
	if (p4d_leaf(*p4d))
		return true;

	pud = pud_offset(p4d, addr);
	if (!pud_present(*pud))
		return false;
	if (pud_leaf(*pud))
		return true;

	pmd = pmd_offset(pud, addr);
	if (!pmd_present(*pmd))
		return false;
	if (pmd_leaf(*pmd))
		return true;

	pte = pte_offset_kernel(pmd, addr);
	return (pte_val(*pte) >> _PAGE_PFN_SHIFT);
}

static int __init riscv_kvm_init(void)
{
	#ifdef PROG_LBL
	unsigned long host_page_num, bitmap_page_num, page_own_table_num;
	unsigned long bitmap_page_num_log=0;
	unsigned long page_own_table_num_log=0;
	unsigned long *bitmap_addr, *page_own_table_addr;
	struct cvm_list_params *bitmap = (struct cvm_list_params *)__get_free_pages(GFP_KERNEL, 0);
	struct cvm_list_params *page_own_table = (struct cvm_list_params *)__get_free_pages(GFP_KERNEL, 0);
	int i;
	struct sbiret ret;

	host_page_num = K(get_num_physpages()) / 4;
	//each host memory page corresponds to 1 bit bitmap.
	bitmap_page_num = (host_page_num / 8 % (1UL<<PAGE_SHIFT)) ? 
							(host_page_num / 8 / (1UL<<PAGE_SHIFT) + 1) :
							(host_page_num / 8 / (1UL<<PAGE_SHIFT));
	//each host memory page corresponds to 8 byte page own table.
	page_own_table_num = (host_page_num * 8 % (1UL<<PAGE_SHIFT)) ?
							(host_page_num * 8 / (1UL<<PAGE_SHIFT) + 1) :
							(host_page_num * 8 / (1UL<<PAGE_SHIFT));
	bitmap->page_num = bitmap_page_num;
	page_own_table->page_num = page_own_table_num;
	while ( bitmap_page_num > 1){
		    bitmap_page_num /= 2;
		    bitmap_page_num_log++;
	}
	while ( page_own_table_num > 1){
		    page_own_table_num /= 2;
		    page_own_table_num_log++;
	}
	bitmap_addr = (unsigned long *)__get_free_pages(GFP_KERNEL, bitmap_page_num_log);
	page_own_table_addr = (unsigned long *)__get_free_pages(GFP_KERNEL, page_own_table_num_log);
	if(!bitmap_addr){
		printk("----------------------------------\n");
		printk("bitmap alloc failed!\n");
		printk("----------------------------------\n");
	}
	if(!page_own_table_addr){
		printk("----------------------------------\n");
		printk("page_own_table alloc failed!\n");
		printk("----------------------------------\n");
	}
	bitmap->addr = __pa((unsigned long)bitmap_addr);
	//bitmap element is unsigned long type.
	bitmap->ele_num = host_page_num / 64;
	page_own_table->addr = __pa((unsigned long)page_own_table_addr);
	//bitmap element is unsigned long type.
	page_own_table->ele_num = host_page_num;
	

	//apply for free memory and translation their hva to hpa.
	//allocate 2^14 pages(64MB) for confidential memory.
	//each page addr is 8B, so we need 2^14 * 8 / 4K = 2^5 pages to record .
	unsigned long *cm_pool = vmalloc(PAGE_SIZE << INITIAL_PAGE_NUM);

	unsigned long cm_list_page_num = (1UL << INITIAL_PAGE_NUM) * sizeof(unsigned long) / (1UL << PAGE_SHIFT);
	unsigned long cm_list_page_num_log = 0;
	while ( cm_list_page_num > 1){
		    cm_list_page_num /= 2;
		    cm_list_page_num_log++;
	}
	//printk("--------cm_list_page_num_log is %lx\n---------", cm_list_page_num_log);
	unsigned long *cm_addr_list = (unsigned long *)__get_free_pages(GFP_KERNEL, cm_list_page_num_log);
	
	unsigned long root_pt_va;
	//we already known the max number of cvm is 32
	unsigned long *root_pt_list = (unsigned long *)__get_free_pages(GFP_KERNEL, 0);
	for(i=0; i<(1<<MAX_CVM_NUM); i++){
		root_pt_va = __get_free_pages(GFP_KERNEL, 2);
		if(!root_pt_va){
			printk("----------------------------------\n");
			printk("root pt mmap failed!\n");
			printk("----------------------------------\n");
		}else{
			*(root_pt_list+i) = __pa(root_pt_va);
		}
	}
	if (!cm_pool){
		printk("----------------------------------\n");
		printk("memory mmap failed!\n");
		printk("----------------------------------\n");
	}else{
		printk("----------------------------------\n");
		printk("memory mmap successed!\n");
		printk("virtual address begin at %lx\n", (unsigned long)cm_pool);
		printk("----------------------------------\n");
		for(i=0;i<(1<<INITIAL_PAGE_NUM);i++){
			*(cm_addr_list+i) = kernel_page_translate((unsigned long)cm_pool + i*PAGE_SIZE) << PAGE_SHIFT;
			//printk("virtual address %lx is translated to pfn %lx\n", (unsigned long)hva + i*PAGE_SIZE, *(list_va+i));
		}
		struct cvm_list_params *cm_addr_l = (struct cvm_list_params *)__get_free_pages(GFP_KERNEL, 0);
		cm_addr_l->addr = __pa((unsigned long)cm_addr_list);
		cm_addr_l->ele_num = (1UL<<INITIAL_PAGE_NUM);
		struct cvm_list_params *root_pt_l = (struct cvm_list_params *)__get_free_pages(GFP_KERNEL, 0);
		root_pt_l->addr = __pa((unsigned long)root_pt_list);
		root_pt_l->ele_num = (1UL<<MAX_CVM_NUM);
		ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_INIT_PAGE_LIST, __pa((unsigned long)cm_addr_l), 
			__pa((unsigned long)root_pt_l), __pa((unsigned long)bitmap), __pa((unsigned long)page_own_table), 0, 0);
	}
	#endif

	int rc;
	const char *str;

	if (!riscv_isa_extension_available(NULL, h)) {
		kvm_info("hypervisor extension not available\n");
		return -ENODEV;
	}

	if (sbi_spec_is_0_1()) {
		kvm_info("require SBI v0.2 or higher\n");
		return -ENODEV;
	}

	if (!sbi_probe_extension(SBI_EXT_RFENCE)) {
		kvm_info("require SBI RFENCE extension\n");
		return -ENODEV;
	}

	kvm_riscv_gstage_mode_detect();

	kvm_riscv_gstage_vmid_detect();

	rc = kvm_riscv_aia_init();
	if (rc && rc != -ENODEV)
		return rc;

	kvm_info("hypervisor extension available\n");

	switch (kvm_riscv_gstage_mode()) {
	case HGATP_MODE_SV32X4:
		str = "Sv32x4";
		break;
	case HGATP_MODE_SV39X4:
		str = "Sv39x4";
		break;
	case HGATP_MODE_SV48X4:
		str = "Sv48x4";
		break;
	case HGATP_MODE_SV57X4:
		str = "Sv57x4";
		break;
	default:
		return -ENODEV;
	}
	kvm_info("using %s G-stage page table format\n", str);

	kvm_info("VMID %ld bits available\n", kvm_riscv_gstage_vmid_bits());

	if (kvm_riscv_aia_available())
		kvm_info("AIA available with %d guest external interrupts\n",
			 kvm_riscv_aia_nr_hgei);

	rc = kvm_init(sizeof(struct kvm_vcpu), 0, THIS_MODULE);
	if (rc) {
		kvm_riscv_aia_exit();
		return rc;
	}

	return 0;
}
module_init(riscv_kvm_init);

static void __exit riscv_kvm_exit(void)
{
	kvm_riscv_aia_exit();

	kvm_exit();
}
module_exit(riscv_kvm_exit);
