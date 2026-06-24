#ifndef __IIE_CVM_SBI_H__
#define __IIE_CVM_SBI_H__

#include <linux/kvm.h>
#include <linux/kvm_host.h>
#include <asm/sbi.h>

#define SBI_EXT_CVM				0x20000217
#define SBI_EXT_CVM_CREATE			0x0
#define SBI_EXT_CVM_CREATE_MEMORY_REGION	0x1
#define SBI_EXT_CVM_MEASURED_PAGES		0x2
#define SBI_EXT_CVM_CREATE_VCPU		0x3
#define SBI_EXT_CVM_FINALIZE			0x4
#define SBI_EXT_CVM_INIT_MEM_POOL		0x5
#define SBI_EXT_CVM_RUN_VCPU			0x6
#define SBI_EXT_CVM_LOAD_FILE			0x7
#define SBI_EXT_CVM_ENTER			0x8
#define SBI_EXT_CVM_INIT_PAGE_LIST		0x9
#define SBI_EXT_CVM_HASH_IMAGE			0xa
#define SBI_EXT_CVM_ATTEST			0xb
#define SBI_EXT_CVM_ALLOC_ROOT_PT		0xc
#define SBI_EXT_CVM_DESTROY			0xd
#define SBI_EXT_CVM_INIT_SWIOTLB		0xe
#define SBI_EXT_CVM_REFILL_MEMORY_POOL		0xf
#define SBI_EXT_CVM_RETRY_LOAD			0x10
#define SBI_EXT_RECYCLE_MEMORY			0x11
#define SBI_EXT_CVM_TEST			0xffff

struct iie_cvm_vcpu_sbi_params {
	int *vcpu_id_ptr;
	int *vcpu_idx_ptr;
	int *cpu_ptr;
};

struct iie_cvm_sbi_params {
	struct kvm_vmid *vmid_ptr;
	int *vcpu_id_ptr;

	uintptr_t *pgd_phys_ptr;
	pgd_t *pgd;
	phys_addr_t pgd_phys;

	unsigned long gpa;
	unsigned long hpa;
};

struct cvm_list_params {
	unsigned long vaddr;
	unsigned long addr;
	unsigned long ele_num;
	unsigned long page_num;
	unsigned long level;
};

struct iie_cvm_sbi_params_load {
	struct kvm_vmid *vmid_ptr;
	unsigned long *src_hpa_array;
	unsigned long des_gpa;
	unsigned long count;
};

struct swiotlb_node {
	struct swiotlb sw;
	unsigned long *vmid;
	struct swiotlb_node *next;
};

struct cvm_mem_chunk_infor {
	unsigned long chunk_vaddr;
	unsigned long chunk_infor_vaddr;
	unsigned long *paddr_list;
	unsigned int type;
	bool free;
	unsigned long *cvm_id;
};

#define INITIAL_PAGE_NUM	13
#define REFILL_PAGE_NUM		12
#define MAX_CVM_NUM		5
#define CVM_CHUNK_SIZE		21

#ifndef K
#define K(x) ((x) << (PAGE_SHIFT - 10))
#endif

#define TEE_NO_MEMORY		-1
#define CVM_ERROR		-2

int cvm_mem_manege_init(void);
int refill_KVM_memory_pool(void);
void reset_KVM_memory_pool_refill_count(void);
int kvm_vm_ioctl_swiotlb(struct kvm *kvm, void __user *argp);
int kvm_riscv_destroy_sw_node(struct kvm *kvm);

#endif
