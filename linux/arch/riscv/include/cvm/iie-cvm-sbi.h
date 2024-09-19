
#ifndef __IIE_CVM_SBI_H__
#define __IIE_CVM_SBI_H__

// #include <sbi/sbi_types.h>
#include <linux/kvm_host.h>
#include <../uapi/linux/kvm.h>

#define SBI_EXT_CVM				0x20000217
#define SBI_EXT_CVM_CREATE 					0x0
#define SBI_EXT_CVM_CREATE_MEMORY_REGION	0x1			
#define SBI_EXT_CVM_MEASURED_PAGES			0x2	
#define SBI_EXT_CVM_CREATE_VCPU				0x3	
#define SBI_EXT_CVM_FINALIZE				0x4
#define SBI_EXT_CVM_INIT_MEM_POOL			0x5
#define SBI_EXT_CVM_RUN_VCPU				0x6
#define SBI_EXT_CVM_LOAD_FILE				0x7
#define SBI_EXT_CVM_ENTER					0x8
#define SBI_EXT_CVM_INIT_PAGE_LIST			0x9
#define SBI_EXT_CVM_HASH_IMAGE				0xa
#define SBI_EXT_CVM_ATTEST					0xb
#define SBI_EXT_CVM_ALLOC_ROOT_PT			0xc
#define SBI_EXT_CVM_DESTROY					0xd
#define SBI_EXT_CVM_INIT_SWIOTLB			0xe
#define SBI_EXT_CVM_REFILL_MEMORY_POOL 		0xf
#define SBI_EXT_CVM_RETRY_LOAD				0x10
/* define for function test */
#define SBI_EXT_CVM_TEST					0xffff


#define PROG_BYK
#define PROG_LBL
#define PROG_WSW

struct iie_cvm_vcpu_sbi_params {
	int *vcpu_id_ptr;	/* id given by userspace at creation */
	int *vcpu_idx_ptr;	/* index into kvm->vcpu_array */
	int *cpu_ptr;
};

struct iie_cvm_sbi_params {
	/* G-stage vmid */
	struct kvm_vmid *vmid_ptr;
	int *vcpu_id_ptr;	/* id given by userspace at creation */

	/* G-stage page table */
	uintptr_t *pgd_phys_ptr;
	pgd_t *pgd;
	phys_addr_t pgd_phys;
	
	/* SWIOTLB */
	unsigned long gpa;
	unsigned long hpa;
	// struct kvm_vcpu *vcpu;
	// struct iie_cvm_vcpu_sbi_params cvm_vcpu_sbi_params;
};

//used for initializing confidential memory
struct cvm_list_params {
	unsigned long addr;
	unsigned long ele_num;
	unsigned long page_num;
	unsigned long level;
};

//load file physical address array
struct iie_cvm_sbi_params_load {
	/* G-stage vmid */
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


//use for allocating confidential memory
#define INITIAL_PAGE_NUM 	14 		//we allocate 2^INITIAL_PAGE_NUM pages for confidential memory at initial time.
#define REFILL_PAGE_NUM		14		//we allocate 2^REFILL_PAGE_NUM pages to refill the confidential memory pool when it is exhausted.
#define MAX_CVM_NUM			5		//we have 2^MAX_CVM_NUM confidential virtual machines at most.
#ifndef K
#define K(x) ((x) << (PAGE_SHIFT-10))
#endif

#define TEE_NO_MEMORY 	-1
#define CVM_ERROR		-2

int create_sbi_param(struct kvm *kvm, struct iie_cvm_sbi_params * cvm_sbi_params);
int cvm_mem_manege_init(void);
unsigned long iie_kernel_page_translate(unsigned long addr);
int refill_KVM_memory_pool(void);
//int kvm_vm_ioctl_load_file(struct kvm *kvm, void *argp);

#endif