#ifndef __SBI_CVM_H__
#define __SBI_CVM_H__
#include <sbi/sbi_types.h>

/* CVM Life Cycle ------------------------------------------------------------------------------ */

#include <sbi/sbi_cvm_cpu.h>

#define TEE_NO_MEMORY 	-1
#define CVM_ERROR		-2

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
    DESTROYED,
    INVALID ,
    FRESH ,
    RUNNING,
    STOPPED,
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
};

/* Page Global Directory entry */
typedef struct {
	unsigned long pgd;
} pgd_t;

typedef struct {
	unsigned long pte;
} pte_t;

struct iie_cvm_sbi_params {
	/* G-stage vmid */
	struct kvm_vmid *vmid_ptr;
	int *vcpu_id_ptr;	/* id given by userspace at creation */

	/* G-stage page table */
	pgd_t *pgd_ptr;
	phys_addr_t *pgd_phys_ptr;

};


/* get vmid, generate keyID */
int sbi_cvm_create(struct iie_cvm_sbi_params * cvm_sbi_params);

/* alloc a HPA space for cvm, and mark part of it as confidential region */
int sbi_cvm_create_memory_region(struct iie_cvm_sbi_params * cvm_sbi_params);

/* measure confidential memory pages */
int sbi_cvm_create_measured_pages(struct iie_cvm_sbi_params * cvm_sbi_params);

/* get vcpu id, and initialize related CSRs */
int sbi_cvm_create_vcpu(struct iie_cvm_sbi_params * cvm_sbi_params);

/* flush tlb, set CVM states to READY */
int sbi_cvm_create_finalize(struct iie_cvm_sbi_params * cvm_sbi_params);

/* build memory list */
int sbi_cvm_init_mem_pool(struct iie_cvm_sbi_params * cvm_sbi_params);

/* Convert Normal page into Confidential page */
int sbi_cvm_convert_page(pte_t *pte);

/* Load Kernel Image into Confidential Memory Region From Normal Region */
int sbi_cvm_load_kernel_image(phys_addr_t to, phys_addr_t from, unsigned int image_size);

/* Check vcpu states  */
int sbi_cvm_run_vcpu(struct iie_cvm_sbi_params * cvm_sbi_params);

struct cvm_node {
	struct sbi_cvm cvm;
	struct cvm_node *next;
};

struct cvm_node *get_cvm(struct kvm_vmid *vmid);

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
#define PAGE_PFN_MASK   ((1 << PGLEVEL_BITS) - 1)
#define PGDIR_SHIFT     30
#define PMD_SHIFT       21
#ifndef PAGE_SHIFT
#define PAGE_SHIFT      12
#endif
#ifndef PAGE_SIZE
#define PAGE_SIZE      (1UL << PAGE_SHIFT)
#endif
#define pgd_index(a)    (((a) >> PGDIR_SHIFT) & PAGE_PFN_MASK)
#define pmd_index(a)    (((a) >> PMD_SHIFT) & PAGE_PFN_MASK)
#define pte_index(a)    (((a) >> PAGE_SHIFT) & PAGE_PFN_MASK)
#define PAGE_PFN_SHIFT  10

//PTE fields
#define PTE_V       0x001
#define PTE_R       0x002
#define PTE_W       0x004
#define PTE_X       0x008
#define PTE_U       0x010
#define PTE_G       0x020
#define PTE_A       0x040
#define PTE_D       0x080
#define PTE_SOFT    0x300


union mcvm
{
    struct{
        uint64_t  BMA       :   62;
        uint64_t  CMODE     :   1;
        uint64_t  BME       :   1;
    }fields;
    uint64_t bits;
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


//function
int malloc_cvm_empty_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle);
int mfree_cvm_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle);
paddr_t mreclaim_cvm_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle);
int add_cvm_share_pages(cvm_lifecycle_t* cvm_lifecycle,  vaddr_t* normal_gpa, paddr_t* normal_hpa, int count);
int convert_cvm_pages(paddr_t* normal_address, int count);

void set_bitmap(paddr_t page_address);
void reset_bitmap(paddr_t page_address);
paddr_t* init_bitmap();
int set_page_own_table(paddr_t page_address, uint64_t id);
int reset_page_own_table(paddr_t page_address, uint64_t id);
page_own_table_t* init_page_own_table();




#endif