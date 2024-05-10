#include <sbi/sbi_cvm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_heap.h>

spinlock_t spin_lock_cm_list;
spinlock_t spin_lock_bitmap;
spinlock_t spin_lock_page_own_table;

struct list_head* free_mem_list_head;
//page_own_table_t *page_own_table = sbi_calloc(1, 4);

//todo: array is too big !
page_own_table_t page_own_table[PAGE_NUM];
uint64_t bitmap[BITMAP_64BITS_COUNT];

int sbi_cvm_print(unsigned long reg){
    sbi_printf("cvm_print %ld!\n", reg);
    return 0;
}


#ifndef container_of
#define container_of(ptr, type, member) (type *)((char*)(ptr) - (char*)&((type *)0)->member)
#endif
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_first_entry(ptr,type,member) list_entry((ptr)->next, type, member)

//list operation
static inline int list_empty(const struct list_head *head){
    return head->next == head;
}
static inline void list_add_head(struct list_head *entry, struct list_head *head)
{
    if(list_empty(head)){
        head->next = entry;
        entry->prev = head;
    }
    else{
        struct list_head* first_data_node = head->next;
        head->next = entry;
        entry->next = first_data_node;
        first_data_node->prev = entry;
        entry->prev = head;
    }
}
static inline void list_del(struct list_head *entry){
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    next->prev = prev;
    prev->next = next;
}

/* CVM Life Cycle ------------------------------------------------------------------------------ */

struct cvm_node *iie_cvm_list_head = NULL;
cpu_state_t cpus[MAX_HARTS];

// #define LOCK_DEBUG   0
static spinlock_t cvm_metadata_lock = SPIN_LOCK_INITIALIZER;


/* TODO: memory allocated should be in confidential memory region */
static struct cvm_node* alloc_cvm_node()
{
	return (struct cvm_node *)sbi_malloc(sizeof(struct cvm_node));
}

/* TODO: memory allocated should be in confidential memory region */
static struct cvm_vcpu_node* alloc_cvm_vcpu_node()
{
	return (struct cvm_vcpu_node *)sbi_malloc(sizeof(struct cvm_vcpu_node));
}

static uint32_t get_keyID(struct kvm_vmid *vmid)
{
	/* TODO */
	static int cnt = 1;
	return ++ cnt;
}

static void cvm_insert_node(struct cvm_node *node)
{
	node->next = iie_cvm_list_head->next;
	iie_cvm_list_head->next = node;
}

static void cvm_insert_vcpu_node(struct cvm_vcpu_node *list_head, struct cvm_vcpu_node *node)
{
	node->next = list_head->next;
	list_head->next = node;
}

int sbi_cvm_create(struct iie_cvm_sbi_params *cvm_sbi_params)
{
	sbi_printf("[IIE CVM Monitor@%s] vmid = %d\n", __func__,cvm_sbi_params->vmid_ptr->vmid);
	sbi_printf("[IIE CVM Monitor@%s] vcpu_id = %d\n", __func__,*cvm_sbi_params->vcpu_id_ptr);
	sbi_printf("[IIE CVM Monitor@%s] pgd_ptr = %lx\n", __func__,cvm_sbi_params->pgd_ptr);
	sbi_printf("[IIE CVM Monitor@%s] pgd_phys_ptr = %lx\n", __func__,cvm_sbi_params->pgd_phys_ptr);
	sbi_printf("[IIE CVM Monitor@%s] pgd = %lx\n", __func__,*cvm_sbi_params->pgd_ptr);
	sbi_printf("[IIE CVM Monitor@%s] pgd_phys = %lx\n", __func__,*cvm_sbi_params->pgd_phys_ptr);


	/* alloc a memory block for cvm struct */
	struct cvm_node* cvm_node = alloc_cvm_node();
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] cvm allocation is failed \r\n", __func__);
		return TEE_NO_MEMORY;
	}
	sbi_printf("[IIE CVM Monitor@%s] cvm allocation is successfull. \r\n", __func__);
    
	/* Initialize cvm structure*/
	// sbi_printf("[IIE CVM Monitor@%s] CVM initializing. \r\n", __func__);
	cvm_node->cvm.vmid = cvm_sbi_params->vmid_ptr;
	cvm_node->cvm.KeyID = get_keyID(cvm_node->cvm.vmid);
	cvm_node->cvm.state = INITIALIZING;
	cvm_node->cvm.cvm_vcpu_list_head = alloc_cvm_vcpu_node();
	cvm_node->cvm.cvm_vcpu_list_head->next = NULL;
	cvm_node->cvm.cmode = 1;
	cvm_node->next = NULL;

	/* maintain the cvm List */
	// sbi_printf("[IIE CVM Monitor@%s] CVM List. \r\n", __func__);
	if(iie_cvm_list_head == NULL)
	{
		/* alloc memory first */
		iie_cvm_list_head = alloc_cvm_node();
		iie_cvm_list_head->next = cvm_node;
	}
	else cvm_insert_node(cvm_node);
	// sbi_printf("[IIE CVM Monitor@%s] print_cvm_list. \r\n", __func__);
	print_cvm_list();


	// TODO: Calculate the enclave's measurement

	// TODO: verify hash and whitelist check

	// Check page table mapping secure and not out of bound
	//put it in run_enclave for debug
/*


/*
 * If create failed for above reasons, secure memory and enclave struct
 * allocated before will never be used. So we need to free these momery.
 */

// error_out:
/*
	sbi_memset((void*)(enclave->paddr), 0, enclave->size);
	mm_free((void*)(enclave->paddr), enclave->size);
	//free enclave struct
	free_enclave(eid); //the enclave state will be set INVALID here
*/
	return CVM_ERROR;
}


int sbi_cvm_create_vcpu(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	struct kvm_vmid *vmid_ptr = cvm_sbi_params->vmid_ptr;
	int *vcpu_id_ptr = cvm_sbi_params->vcpu_id_ptr;
	struct cvm_node *cvm_node = get_cvm(vmid_ptr);
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] CVM Node is NULL, get_cvm failed!\n", __func__);
		return -1;
	}
	struct cvm_vcpu_node *vcpu_list_head = cvm_node->cvm.cvm_vcpu_list_head;
	struct cvm_vcpu_node *vcpu_node = alloc_cvm_vcpu_node();
	if(!vcpu_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] CVM vcpu Node allocated failed!\n", __func__);
		return -1;
	}
	vcpu_node->vcpu.vcpu_id = vcpu_id_ptr;
	vcpu_node->vcpu.cvm = &cvm_node->cvm;

	sbi_printf("[IIE CVM Monitor@%s] vmid = %d\n", __func__,vmid_ptr->vmid);
	sbi_printf("[IIE CVM Monitor@%s] vcpu_id = %d\n", __func__,*vcpu_id_ptr);
	

	if(vcpu_list_head->next == NULL)
	{
		vcpu_list_head->next = vcpu_node;
	}
	else cvm_insert_vcpu_node(vcpu_list_head, vcpu_node);

	

	return 0;
}

static uint32_t get_cmode(int vcpu_id)
{
	if(vcpu_id < 0 || vcpu_id >= MAX_HARTS) return 0;
	return 1;
	// return cpus[vcpu_id].cmode;
}

struct cvm_node *get_cvm(struct kvm_vmid *vmid_ptr)
{
	
    // acquire_big_metadata_lock(__func__);

	for(struct cvm_node *cur = iie_cvm_list_head; cur->next; cur = cur->next)
	{
		struct cvm_node *node = cur->next;
		if(node->cvm.vmid == vmid_ptr)
			return node;
	}

    // release_big_metadata_lock(__func__);
	return NULL;
}

struct cvm_vcpu_node *get_cvm_vcpu_node(struct kvm_vmid *vmid_ptr, int vcpu_id)
{
	struct cvm_node *cvm_node = get_cvm(vmid_ptr);
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] cvm_node is NULL!\n", __func__);
		return -1;
	}
	struct cvm_vcpu_node *vcpu_list_head = cvm_node->cvm.cvm_vcpu_list_head;
    if(!vcpu_list_head)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu_list_head is NULL!\n", __func__);
		return -1;
	}

	// acquire_big_metadata_lock(__func__);

	for(struct cvm_vcpu_node *cur = vcpu_list_head; cur->next; cur = cur->next)
	{
		struct cvm_vcpu_node *node = cur->next;
        sbi_printf("[IIE CVM Monitor@%s] node vcpu id = %d\tvcpu id = %d\n", __func__, *node->vcpu.vcpu_id, );
		if(*node->vcpu.vcpu_id == vcpu_id)
			return node;
	}

    // release_big_metadata_lock(__func__);
	return NULL;
}

int sbi_cvm_run_vcpu(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	struct cvm_vcpu_node *vcpu_node = get_cvm_vcpu_node(cvm_sbi_params->vmid_ptr, *cvm_sbi_params->vcpu_id_ptr);	
	if(!vcpu_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu_node is NULL!\n", __func__);
		return -1;
	}
	struct cvm_vcpu *vcpu = &vcpu_node->vcpu;
	if(!vcpu)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu is NULL!\n", __func__);
		return -1;
	}
	uint32_t cmode = get_cmode(*vcpu->vcpu_id);
	struct sbi_cvm *cvm = vcpu->cvm;
	if(cmode && cvm->state != READY)
	{
		/* Exception: CVM hasn't been initialized. */
		sbi_printf("[IIE CVM Monitor@%s] Exception: CVM hasn't been initialized.\n", __func__);
		// exit(0);
		return 0xff;
	}
	return 0;
}

int sbi_cvm_create_finalize(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

int sbi_cvm_create_memory_region(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

int sbi_cvm_create_measured_pages(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

int sbi_cvm_init_mem_pool(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

inline void print_cvm_list()
{
	int i = 1;
	for(struct cvm_node *cur = iie_cvm_list_head; cur->next; cur = cur->next)
	{
		struct cvm_node *node = cur->next;
		sbi_printf("[IIE CVM DEBUG@%s] vmid = %d\tKeyID = %d\tcount = %d\n", __func__, node->cvm.vmid->vmid, node->cvm.KeyID, i ++);
	}
}

/* CVM Life Cycle ------------------------------------------------------------------------------ */

//bitmap operation
//todo : here is no check about whether bitmap bit is already set(share condition).
void set_bitmap(paddr_t page_address){
    uint64_t index = page_address >> PAGE_SHIFT >> BITMAP_OFFSET;
    uint64_t offset = (page_address >> PAGE_SHIFT) & ((1 << BITMAP_OFFSET) - 1);
    spin_lock(&spin_lock_bitmap);
    uint64_t bits = *(bitmap + index);
    *(bitmap + index) = bits | (1<<offset);
    spin_unlock(&spin_lock_bitmap);
}
void reset_bitmap(paddr_t page_address){
    uint64_t index = page_address >> PAGE_SHIFT >> BITMAP_OFFSET;
    uint64_t offset = (page_address >> PAGE_SHIFT) & ((1 << BITMAP_OFFSET) - 1);
    spin_lock(&spin_lock_bitmap);
    uint64_t bits = *(bitmap + index);
    *(bitmap + index) = bits & (~(1<<offset));
    spin_unlock(&spin_lock_bitmap);
}
paddr_t* init_bitmap(){
    spin_lock(&spin_lock_bitmap);
    for(int i=0;i<BITMAP_64BITS_COUNT;i++){
        bitmap[i] = 0;
    }
    //set bitmap page
    uint64_t bitmap_pages_num = BITMAP_64BITS_COUNT >> ((1 << PAGE_SHIFT) >> 8);
    for(int i=0;i<bitmap_pages_num;i++){
        set_bitmap((uint64_t)bitmap + i*PAGE_SIZE);
    }
    spin_unlock(&spin_lock_bitmap);
    return bitmap;
}

//page own table operation
//we should ignore check those which are uesed to CM share pages because their ids are always inconsistent;
//todo : there must be another check mechanism for CM share pages.
int set_page_own_table(paddr_t page_address, uint64_t id){
    uint64_t index = page_address >> PAGE_SHIFT;
    spin_lock(&spin_lock_page_own_table);
    //the physical page already allocate to another cvm
    if((*(page_own_table+index)).id == 0){
        (*(page_own_table+index)).id = id;
        spin_unlock(&spin_lock_page_own_table);
        return 1;
    }
    else if((*(page_own_table+index)).id != 0){
        spin_unlock(&spin_lock_page_own_table);
        return 0;
    }
    return 0;
}
int reset_page_own_table(paddr_t page_address, uint64_t id){
    uint64_t index = page_address >> PAGE_SHIFT;
    spin_lock(&spin_lock_page_own_table);
    //the physical page already allocate to another cvm
    if((*(page_own_table+index)).id == id){
        (*(page_own_table+index)).id = 0;
        spin_lock(&spin_lock_page_own_table);
        return 1;
    }else{
        spin_lock(&spin_lock_page_own_table);
        return 0;
    }
}
page_own_table_t* init_page_own_table(){
    for(paddr_t pa = (paddr_t)page_own_table; pa < (paddr_t)page_own_table+PAGE_NUM*sizeof(page_own_table_t); pa += PAGE_SIZE){
        set_bitmap(pa);
    }
    for(int i = 0; i< PAGE_NUM; i++){
        (*(page_own_table+i)).id = 0;
    }
    return page_own_table;
}

//confidential memory management
paddr_t get_free_mem(struct list_head* free_mem){
    free_mem_t* page;
    paddr_t paddr;
    if(list_empty(free_mem)){
        sbi_printf("No empty mem in confidential mem\n");
        return 0;
    }
    page = list_first_entry(free_mem, free_mem_t, free_mem_list);
    paddr = page->paddr;
    list_del(&page->free_mem_list);
    //redesign free
    sbi_memset((char *)paddr, 0, sizeof(free_mem_t));
    return paddr;
}
void put_free_page(struct list_head* free_mem, paddr_t paddr){
    //redesign malloc
    free_mem_t* page = (free_mem_t*)paddr;
    page->paddr = paddr;
    list_add_head(&page->free_mem_list, free_mem);
    return;
}
void init_free_mem(paddr_t base, uint64_t count){
    paddr_t cur = base;
    uint64_t i;
    for(i=0;i<count;i++){
        //set bitmap
        set_bitmap(cur);
        put_free_page(free_mem_list_head, cur);
        cur += PAGE_SIZE;
    }
    return;
}
void clean_free_mem(struct list_head* free_mem){
    free_mem_t* page;
    while(!list_empty(free_mem)){
        page = list_first_entry(free_mem, free_mem_t, free_mem_list);
        paddr_t paddr = page->paddr;
        list_del(&page->free_mem_list);
        //free page
        sbi_memset((char *)paddr, 0, sizeof(free_mem_t));
        //reset bitmap
        reset_bitmap(paddr);
    }
}


int malloc_cvm_empty_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle){
    pt_entry_t *pgd, *pmd, *pte;
    spin_lock(&spin_lock_cm_list);
    paddr_t free_page = get_free_mem(free_mem_list_head);
    spin_unlock(&spin_lock_cm_list);
    if(free_page == 0){
        sbi_printf("no empty confidential memory\n");
        return 0;
    }
    pgd = (pt_entry_t*)(cvm_lifecycle->root_pt) + pgd_index(gpa);
    if(!(*pgd | PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_pgd = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_pgd == 0){
            sbi_printf("no empty confidential memory\n");
            return 0;
        }
        *pgd = (free_page_pgd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(gpa);
    if(!(*pmd | PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_pmd = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_pmd == 0){
            sbi_printf("no empty confidential memory\n");
            return 0;
        }
        *pgd = (free_page_pmd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(gpa);
    if(*pte | PTE_V){
        sbi_printf("address %ld already has physical page\n", gpa);
        return 0;
    }
    //set page own table
    set_page_own_table(free_page, cvm_lifecycle->id);
    uint64_t ppn = free_page >> PAGE_SHIFT;
    //set KeyID and prot.
    uint64_t keyid_ppn = (cvm_lifecycle->id<<KEYID_OFFSET) | ppn;
    *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
    return 1;
}

int mfree_cvm_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle){
    pt_entry_t *pgd, *pmd, *pte;
    pgd = (pt_entry_t*)(cvm_lifecycle->root_pt) + pgd_index(gpa);
    if(!(*pgd | PTE_V)){
        sbi_printf("Invalid pgd\n");
        return 0;
    }
    pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(gpa);
    if(!(*pmd | PTE_V)){
        sbi_printf("Invalid pmd\n");
        return 0;
    }
    pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(gpa);
    if(!(*pte | PTE_V)){
        sbi_printf("Invalid pte\n");
        return 0;
    }
    //invalidate pte
    *pte = (*pte) & (~PTE_V);

    //mask keyid and prot
    uint64_t keyid_ppn = (*pte) >> PAGE_PFN_SHIFT;
    uint64_t ppn = keyid_ppn & (~KEYID_MASK);
    paddr_t paddr = ppn << PAGE_SHIFT;
    //reset page own table
    reset_page_own_table(paddr, cvm_lifecycle->id);
    //reset and reclaim
    sbi_memset((void*)paddr , 0, PAGE_SIZE);
    spin_lock(&spin_lock_cm_list);
    put_free_page(free_mem_list_head, paddr);
    spin_unlock(&spin_lock_cm_list);
    return 1;
}

paddr_t mreclaim_cvm_page(vaddr_t gpa, cvm_lifecycle_t* cvm_lifecycle){
    //gpa->hpa
    pt_entry_t *pgd, *pmd, *pte;
    pgd = (pt_entry_t*)(cvm_lifecycle->root_pt) + pgd_index(gpa);
    if(!(*pgd | PTE_V)){
        sbi_printf("Invalid pgd\n");
        return 0;
    }
    pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(gpa);
    if(!(*pmd | PTE_V)){
        sbi_printf("Invalid pmd\n");
        return 0;
    }
    pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(gpa);
    if(!(*pte | PTE_V)){
        sbi_printf("Invalid pte\n");
        return 0;
    }
    //invalidate pte
    *pte = (*pte) & (~PTE_V);
    //return paddr_t
    uint64_t keyid_ppn = (*pte) >> PAGE_PFN_SHIFT;
    uint64_t ppn = keyid_ppn & (~KEYID_MASK);
    paddr_t paddr = ppn << PAGE_SHIFT;
    reset_page_own_table(paddr, cvm_lifecycle->id);
    reset_bitmap(paddr);
    return paddr;
}

//vaddr_t normal_gpa[count] = {vaddr1, vaddr2 ,...}
//paddr_t narmal_hpa[count] = {paddr1, paddr2 ,...}
int add_cvm_share_pages(cvm_lifecycle_t* cvm_lifecycle,  vaddr_t* normal_gpa, paddr_t* normal_hpa, int count){
    if(count<=0){
        return 0;
    }
    pt_entry_t *pgd, *pmd, *pte;
    for(int i=0; i<count; i++){
        pgd = (pt_entry_t*)(cvm_lifecycle->root_pt) + pgd_index(*(normal_gpa+i));
        if(!(*pgd | PTE_V)){
            spin_lock(&spin_lock_cm_list);
            paddr_t free_page_pgd = get_free_mem(free_mem_list_head);
            spin_unlock(&spin_lock_cm_list);
            if(free_page_pgd == 0){
                sbi_printf("no empty confidential memory\n");
                return 0;
            }
            *pgd = (free_page_pgd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
        }
        pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(*(normal_gpa+i));
        if(!(*pmd | PTE_V)){
            spin_lock(&spin_lock_cm_list);
            paddr_t free_page_pmd = get_free_mem(free_mem_list_head);
            spin_unlock(&spin_lock_cm_list);
            if(free_page_pmd == 0){
                sbi_printf("no empty confidential memory\n");
                return 0;
            }
            *pgd = (free_page_pmd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
        }
        pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(*(normal_gpa+i));
        if(*pte | PTE_V){
            sbi_printf("address %ld already has physical page\n", *(normal_gpa+i));
            return 0;
        }
        uint64_t keyid_ppn = (cvm_lifecycle->id << KEYID_OFFSET) | (*(normal_hpa+i) >> PAGE_SHIFT);
        *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
        set_bitmap(*(normal_hpa+i));
        set_page_own_table(*(normal_hpa+i), cvm_lifecycle->id);
    }
    return 1;
}

//todo : modify it to riscv H extension
void flush_tlb(){
    asm volatile("sfence.vma");
}

//paddr_t narmal_address[count] = {paddr1, paddr2 ,...}
int convert_cvm_pages(paddr_t* normal_address, int count){
    if(count<=0){
        return 0;
    }
    int i;
    for(i=1; i<count; i++){
        set_bitmap(*(normal_address+i));
        spin_lock(&spin_lock_cm_list);
        //convert normal_address to CM pool rather than CVM memory 
        put_free_page(free_mem_list_head, *(normal_address+i));
        spin_unlock(&spin_lock_cm_list);
    }
    return 1;
}

int load_file(cvm_lifecycle_t* cvm_lifecycle, paddr_t* file_normal_hpa, vaddr_t des_cm_gpa, int pages_count){
    int i;
    paddr_t des_cm_hpa;
    for(i=0;i<pages_count;i++){
        pt_entry_t *pgd, *pmd, *pte;
        pgd = (pt_entry_t*)(cvm_lifecycle->root_pt) + pgd_index(des_cm_gpa);
        if(!(*pgd | PTE_V)){
            spin_lock(&spin_lock_cm_list);
            paddr_t free_page_pgd = get_free_mem(free_mem_list_head);
            spin_unlock(&spin_lock_cm_list);
            if(free_page_pgd == 0){
                sbi_printf("no empty confidential memory\n");
                return 0;
            }
            *pgd = (free_page_pgd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
        }
        pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(des_cm_gpa);
        if(!(*pmd | PTE_V)){
            spin_lock(&spin_lock_cm_list);
            paddr_t free_page_pmd = get_free_mem(free_mem_list_head);
            spin_unlock(&spin_lock_cm_list);
            if(free_page_pmd == 0){
                sbi_printf("no empty confidential memory\n");
                return 0;
            }
            *pgd = (free_page_pmd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
        }
        pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(des_cm_gpa);
        if(*pte | PTE_V){
            des_cm_hpa = (((*pte) >> PAGE_PFN_SHIFT) & (~KEYID_MASK)) << PAGE_SHIFT;
        }else{
            spin_lock(&spin_lock_cm_list);
            paddr_t free_page = get_free_mem(free_mem_list_head);
            spin_unlock(&spin_lock_cm_list);
            //set page own table
            set_page_own_table(free_page, cvm_lifecycle->id);
            uint64_t ppn = free_page >> PAGE_SHIFT;
            //set KeyID and prot.
            uint64_t keyid_ppn = (cvm_lifecycle->id<<KEYID_OFFSET) | ppn;
            *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
            des_cm_hpa = free_page;
        }
        sbi_memcpy((char *)des_cm_hpa, file_normal_hpa, PAGE_SIZE);
        des_cm_gpa += PAGE_SIZE;
    }
    return 1;
}