#ifndef __SBI_CVM_H__
#define __SBI_CVM_H__
#include <sbi/sbi_types.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_trap.h>

/* CVM Life Cycle ------------------------------------------------------------------------------ */

#include <sbi/sbi_cvm_cpu.h>

#define TEE_NO_MEMORY 	-1
#define CVM_ERROR		-2

#define CVM_HASH_SIZE 32

#ifdef CONFIG_PHYS_ADDR_T_64BIT
typedef u64 phys_addr_t;
#else
typedef u32 phys_addr_t;
#endif

struct kvm_vmid {
	/*
	 * Writes to vmid_version and vmid happen with vmid_lock held
	 * whereas reads happen without any lock held.
	 */
	unsigned long vmid_version;
	unsigned long vmid;
};

enum cvm_state
{
    // DESTROYED,
    // INVALID ,
    // FRESH ,
    // RUNNING,
    // STOPPED,
    INITIALIZING,	
    READY,		
};

/** CVM Parameters */
struct sbi_cvm {
	/** KeyID of the CVM */
	int KeyID;
	struct kvm_vmid *vmid;
	enum cvm_state state;
	struct cvm_vcpu_node *cvm_vcpu_list_head;
	uint32_t cmode;
    unsigned long root_pt;

	//shared memory with host
	unsigned long untrusted_ptr;
	unsigned long untrusted_ptr_paddr;
	unsigned long untrusted_size;
	// enclave measurement
	unsigned char hash[CVM_HASH_SIZE];
	// hash of enclave developer's public key
	unsigned char signer[CVM_HASH_SIZE];

	uintptr_t pgd_phys;
};

/* Page Global Directory entry */
typedef struct {
	unsigned long pgd;
} pgd_t;

// typedef struct {
// 	unsigned long pte;
// } pte_t;


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


struct iie_cvm_sbi_params_load {
	/* G-stage vmid */
	struct kvm_vmid *vmid_ptr;
	unsigned long *src_hpa_array;
	unsigned long des_gpa;
	unsigned long count;
};

struct multi_key_manage_t {
	uint8_t key_id;
	uint8_t mode;
	bool tweak_flag;
	bool random_ready_flag;
	bool key_expansion_idle;
	bool last_req_accepted;
	bool key_init_req;
	/* if mode == 1, ie. key managed by software, then generate two 64-bit random numbers. */
	uint64_t rnum1, rnum2;
};

/* get vmid, generate keyID */
int sbi_cvm_create(struct iie_cvm_sbi_params * cvm_sbi_params);

/* alloc a HPA space for cvm, and mark part of it as confidential region */
int sbi_cvm_create_memory_region(struct iie_cvm_sbi_params * cvm_sbi_params);

/* measure confidential memory pages */
int sbi_cvm_hash_image(struct iie_cvm_sbi_params * cvm_sbi_params);

/* attest cvm and generate attestation report */
int sbi_cvm_attest(struct iie_cvm_sbi_params * cvm_sbi_params);

/* get vcpu id, and initialize related CSRs */
int sbi_cvm_create_vcpu(struct iie_cvm_sbi_params * cvm_sbi_params);

/* flush tlb, set CVM states to READY */
int sbi_cvm_create_finalize(struct iie_cvm_sbi_params * cvm_sbi_params);

/* build memory list */
int sbi_cvm_init_mem_pool(struct iie_cvm_sbi_params * cvm_sbi_params);

/* Convert Normal page into Confidential page */
// int sbi_cvm_convert_page(pte_t *pte);

/* Load Kernel Image into Confidential Memory Region From Normal Region */
int sbi_cvm_load_kernel_image(phys_addr_t to, phys_addr_t from, unsigned int image_size);

/* Check vcpu states  */
int sbi_cvm_run_vcpu(struct sbi_trap_regs *regs, struct iie_cvm_sbi_params * cvm_sbi_params, uint64_t kvm_vcpu_context, uint64_t kvm_vcpu_csr, uint64_t kvm_trap);

/* Destroy CVM, clean and reclaim vcpus, memory and registers. */
int sbi_cvm_destroy(struct iie_cvm_sbi_params * cvm_sbi_params);

/* Test functions */
int sbi_cvm_test(struct iie_cvm_sbi_params * cvm_sbi_params);

struct cvm_node {
	struct sbi_cvm cvm;
	struct cvm_node *next;
};

struct cvm_node *get_cvm(unsigned long vmid);

/* CVM Life Cycle ------------------------------------------------------------------------------ */


#include <sbi/riscv_locks.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_heap.h>

typedef unsigned long uint64_t;
typedef uint64_t pt_entry_t;
typedef uint64_t vaddr_t;
typedef uint64_t paddr_t;
typedef unsigned long uintptr_t;
typedef unsigned long size_t;


struct list_head {
    struct list_head *next,*prev;
};

typedef struct free_mem{
    paddr_t paddr;
    struct list_head free_mem_list;
}free_mem_t;

//every cvm should have
typedef struct cvm_lifecycle{
    uint64_t id;
    pt_entry_t* root_pt;
}cvm_lifecycle_t;

//CM free page list head node



typedef struct page_own_table{
    // uint64_t id     : 63 ;
    // uint64_t share  : 1  ;
    uint64_t id;
}page_own_table_t;


//gpa to hpa transfer 
#define PGLEVEL_BITS    9
#define PAGE_VPN_FIRST_LEVEL	11
#define PAGE_PFN_MASK   ((1 << PGLEVEL_BITS) - 1)
#define PGDIR_SHIFT     48
#define PUDIR_SHIFT     39
#define P4DIR_SHIFT     30
#define PMD_SHIFT       21
#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE      (1UL << PAGE_SHIFT)
#endif
#define pgd_index(a)    (((a) >> PGDIR_SHIFT) & PAGE_VPN_FIRST_LEVEL)
#define pud_index(a)    (((a) >> PUDIR_SHIFT) & PAGE_PFN_MASK)
#define p4d_index(a)    (((a) >> P4DIR_SHIFT) & PAGE_PFN_MASK)
#define pmd_index(a)    (((a) >> PMD_SHIFT) & PAGE_PFN_MASK)
#define pte_index(a)    (((a) >> PAGE_SHIFT) & PAGE_PFN_MASK)
#define PAGE_PFN_SHIFT  10


union mcvm
{
    struct{
        uint64_t  BMA       :   62;
        uint64_t  CMODE     :   1;
        uint64_t  BME       :   1;
    }fields;
    uint64_t bits;
};

struct cvm_list_params {
	unsigned long addr;
	unsigned long ele_num;
	unsigned long page_num;
};

//KeyID 
#define KEYID_OFFSET    24
#define KEYID_MASK      ((0x1FUL<<KEYID_OFFSET))

//bitmap declaration
//need 2MB space for 64GB
#define MAX_MEM_SPACE           (1UL<<29)
#define BITMAP_64BITS_COUNT     (MAX_MEM_SPACE / PAGE_SIZE / 64)
#define BITMAP_OFFSET           6


//page own table declaration
//need 128MB space for 64GB
#define PAGE_NUM                (MAX_MEM_SPACE / PAGE_SIZE)

#define SWIOTLB_ADDR 0x82c00000
#define SWIOTLB_SIZE 0x200000

//vcpu exit reason
#define SWIOTLB			14

//memory manage function
paddr_t malloc_cvm_empty_page(struct sbi_cvm* cvm, vaddr_t gpa);
paddr_t mreclaim_cvm_page(struct sbi_cvm* cvm, struct iie_cvm_sbi_params *cvm_sbi_params);
int add_cvm_share_pages(paddr_t root_pt, paddr_t gpa, paddr_t hpa);
int convert_cvm_pages(struct cvm_list_params* cm_pool_list, struct cvm_list_params* root_pt_list, struct cvm_list_params* bitmap, struct cvm_list_params* page_own_table);
int load_file(struct iie_cvm_sbi_params_load *load_file);
void set_bitmap(paddr_t page_address);
void reset_bitmap(paddr_t page_address);
paddr_t* init_bitmap(struct cvm_list_params* bmp);
int set_page_own_table(paddr_t page_address, uint64_t id);
int reset_page_own_table(paddr_t page_address, uint64_t id);
page_own_table_t* init_page_own_table(struct cvm_list_params* own_table);
paddr_t malloc_cvm_empty_page_only(unsigned long vmid);
int mfree_cvm_page_only(paddr_t paddr, unsigned long vmid);
void mfree_cvm_page(struct sbi_cvm* cvm);

// /** cvm functions */
void init_cpus();
void init_cvm_vcpu(struct cvm_vcpu* cvm_vcpu);
int check_in_cmode();
int sbi_cvm_print(unsigned long keyID);
int cvm_vcpu_enter(struct sbi_trap_regs* host_regs, struct cvm_vcpu* cvm_vcpu, uint64_t kvm_vcpu_context, uint64_t kvm_vcpu_csr, uint64_t kvm_trap);
int cvm_vcpu_exit(struct sbi_trap_regs* host_regs);
int cvm_trap_redirect_to_hs(struct sbi_trap_regs* host_regs);
int cvm_trap_virtual_inst(struct sbi_trap_regs* host_regs);
int cvm_trap_gstage_page_fault(struct sbi_trap_regs* host_regs);
int cvm_trap_sbi_ecall(struct sbi_trap_regs* host_regs);


#endif