// #define DEBUG_WSW
//#define PROG_LBL
#include <sbi/sbi_cvm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_tlb.h>
#include <sm/attest.h>
#include <sm/gm/SM2_sv.h>
#include <mkey/multi_key.h>



spinlock_t spin_lock_bitmap;
spinlock_t spin_lock_page_own_table;
spinlock_t spin_lock_root_pt_list;
spinlock_t spin_lock_get_cvm_vcpu_node;
spinlock_t spin_lock_chunk_list;
spinlock_t spin_lock_metadata_list;


struct cvm_mem_chunk_node *cvm_chunk_list_head;
struct list_head *cvm_metadata_list_head;
struct list_head *free_root_pt_list_head;


page_own_table_t *page_own_table;
unsigned int ownership_table_level;
unsigned long own_max_num;

unsigned long *bitmap;
// unsigned int bitmap_level;
unsigned long bitmap_max_ele;

const int metadataPage=12;
unsigned long PAGE_LEVEL;

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
        entry->next = NULL;
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
    if(!entry){
        sbi_printf("empty entry!\n");
        return;
    }
    struct list_head *prev = entry->prev;
    struct list_head *next = entry->next;
    if(next){
        next->prev = prev;
        prev->next = next;
    }else{
        prev->next = prev;
        
    }
}

struct cvm_node *iie_cvm_list_head = NULL;
// cpu_state_t cpus[MAX_HARTS];

// #define LOCK_DEBUG   0
static spinlock_t cvm_metadata_lock = SPIN_LOCK_INITIALIZER;


/* TODO: memory allocated should be in confidential memory region */
static struct cvm_node* alloc_cvm_node(paddr_t* vmid_addr, paddr_t *free_page)
{
    //return (struct cvm_node *)sbi_malloc(sizeof (struct cvm_node));
    int ret=0;
    ret = malloc_cvm_empty_page_only(vmid_addr, free_page);
    return ret;
}

/* TODO: memory allocated should be in confidential memory region */
static int alloc_cvm_vcpu_node(paddr_t* vmid_addr, paddr_t *free_page)
{
    //return (struct cvm_vcpu_node *)sbi_malloc(sizeof (struct cvm_vcpu_node));
    int ret=0;
    ret = malloc_cvm_empty_page_only(vmid_addr, free_page);
    return ret;
}

static uint32_t get_keyID(struct kvm_vmid *vmid)
{
	/* TODO */
	// static int cnt = 1;
	// return ++ cnt;
    return 0; 
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

static int cvm_delete_all_vcpu_node(struct cvm_vcpu_node *list_head);
static int cvm_delete_node(struct cvm_node *node)
{
    if(!iie_cvm_list_head)
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM List is Empty.\n", __func__);
        return -1;
    }
    bool node_exist = false;
    if(node)
    {
        for(struct cvm_node *cur = iie_cvm_list_head; cur; cur = cur->next)
        {
            struct cvm_node *nxt = cur->next;
            if(nxt == node) 
            {
                sbi_printf("[IIE CVM Monitor@%s] deleting CVM %ld\n", __func__, *nxt->cvm.vmid);
                node_exist = true;
                /* 1. delete logically, maintain cvm list and vcpu list. */ 
                cur->next = nxt->next;
                nxt->next = NULL;
                release_key_id(nxt->cvm.KeyID);
                cvm_delete_all_vcpu_node(nxt->cvm.cvm_vcpu_list_head);
                break;
            }
        }
        if(node_exist)
        {
            /* 2. reclaim resources, vcpu and memory. Clean memory and registers. */
            mfree_cvm_page_only(node, &node->cvm.vmid->vmid);
            // sbi_free(&node->cvm);
            // sbi_free(node);
        }
        else 
        {
            sbi_printf("[IIE CVM Monitor@%s] This cvm does not exist.\n", __func__);
            return -1;
        }
    }
    else
    {
        sbi_printf("[IIE CVM Monitor@%s] Empty cvm parameters.\n", __func__);
        return -1;
    }
    sbi_printf("[IIE CVM Monitor@%s] Delete CVM Successfully.\n", __func__);
    return 0;
}

static int cvm_delete_vcpu_node(struct cvm_vcpu_node *list_head, struct cvm_vcpu_node *node)
{
    /* TODO 
        1. free single vcpu: 
            1.1 logically delete from vcpu list
            1.2 free memory and register resources
        2. free all vcpus recursively
    */
    if(!list_head)
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM vcpu list_head is Empty.\n", __func__);
        return -1;
    }
    bool node_exist = false;
    if(!node)
    {
        sbi_printf("[IIE CVM Monitor@%s] Empty vcpu parameters.\n", __func__);
        return -2;
    }
    int i = 0;
    for(struct cvm_vcpu_node* cur = list_head; cur; cur = cur->next)
    {
        struct cvm_vcpu_node* nxt = cur->next;
        if(nxt == node)
        {
            node_exist = true;
            /* 1. delete logically, maintain cvm list and vcpu list. */ 
            cur->next = nxt->next;
            nxt->next = NULL;
            sbi_printf("[IIE CVM Monitor@%s] deleting vcpu %d\n", __func__, *nxt->vcpu.vcpu_id);
            break;
        }
    }
    if(node_exist)
    {
        /* 2. reclaim resources, vcpu context. Clean memory and registers. */
        mfree_cvm_page_only(node, &node->vcpu.cvm->vmid->vmid);
        // sbi_free(&node->vcpu);
        // sbi_free(node);
    }
    else 
    {
        sbi_printf("[IIE CVM Monitor@%s] This vcpu does not exist.\n", __func__);
        return -3;
    }
    sbi_printf("[IIE CVM Monitor@%s] Delete VCPU Successfully.\n", __func__);
    return 0;
}

static int cvm_delete_all_vcpu_node(struct cvm_vcpu_node *list_head)
{
    if(!list_head)
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM vcpu list_head is Empty.\n", __func__);
        return -1;
    }

    // for(struct cvm_vcpu_node* cur = list_head; cur; cur = cur->next)
    while(list_head->next)
    {
        struct cvm_vcpu_node* cur = list_head->next;
        int ret = cvm_delete_vcpu_node(list_head, cur);
        
        if(ret)
        {
            sbi_printf("[IIE CVM Monitor@%s] Vcpu delete failed with error %d, Continue deleting...\n", __func__, ret);
            continue;
        }
    }
    return 0;
}


static uint32_t get_cmode(int vcpu_id)
{
	if(vcpu_id < 0 || vcpu_id >= MAX_HARTS) return 0;
	return 1;
	// return cpus[vcpu_id].cmode;
}

struct cvm_node *get_cvm(unsigned long vmid)
{
	if(iie_cvm_list_head == NULL) 
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM list is empty.\n", __func__);
        return NULL;
    }
    // acquire_big_metadata_lock(__func__);

	for(struct cvm_node *cur = iie_cvm_list_head; cur->next; cur = cur->next)
	{
		struct cvm_node *node = cur->next;
        // sbi_printf("[IIE CVM Monitor@%s] node vmid id = %d\tvmid = %d\n", __func__, node->cvm.vmid->vmid, vmid);
		if(node->cvm.vmid->vmid == vmid)
			return node;
	}

    // release_big_metadata_lock(__func__);
    sbi_printf("[IIE CVM Monitor@%s] CVM %d does not exist.\n", __func__, vmid);
	return NULL;
}

struct cvm_vcpu_node *get_cvm_vcpu_node(unsigned long vmid, int vcpu_id)
{
	struct cvm_node *cvm_node = get_cvm(vmid);
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] cvm_node is NULL!\n", __func__);
		return NULL;
	}
	struct cvm_vcpu_node *vcpu_list_head = cvm_node->cvm.cvm_vcpu_list_head;
    if(!vcpu_list_head)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu_list_head is NULL!\n", __func__);
		return NULL;
	}

	// spin_lock(&spin_lock_get_cvm_vcpu_node);
	for(struct cvm_vcpu_node *cur = vcpu_list_head; cur->next; cur = cur->next)
	{
		struct cvm_vcpu_node *node = cur->next;
        // sbi_printf("[IIE CVM Monitor@%s] node vcpu id = %d\tvcpu id = %d\n", __func__, *node->vcpu.vcpu_id, vcpu_id);
		if(*node->vcpu.vcpu_id == vcpu_id)
			return node;
	}

    // spin_unlock(&spin_lock_get_cvm_vcpu_node);
	return NULL;
}


/* CVM Ctx Switch ------------------------------------------------------------------------------ */
cpu_state_t cpus[2] = {{0,}, };
// struct cvm_vcpu cvm_vcpu[4] = {{0,}, };

void init_cpus()
{
    for(int i = 0; i < 2; i++)
    {
        cpus[i].cmode = 0;
        cpus[i].cvmid = 0;
        cpus[i].vcpuid = 0;
    }
}

void init_cvm_vcpu(struct cvm_vcpu* cvm_vcpu)
{
    // reset guest_context and guest_csr values
    cvm_vcpu->guest_csr.scounteren = 0x7;

    cvm_vcpu->guest_mie = 0;
    cvm_vcpu->guest_mie |= MIP_VSTIP | MIP_MTIP | MIP_VSSIP | MIP_MSIP | MIP_SEIP | MIP_VSEIP;
    
    cvm_vcpu->guest_mstatus = 0;
    cvm_vcpu->guest_mstatus |= MSTATUS_MPIE | MSTATUS_SPIE | MSTATUS_FS;
    cvm_vcpu->guest_mstatus |= PRV_S << MSTATUS_MPP_SHIFT;
    cvm_vcpu->guest_mstatus |= MSTATUS_MPV | _ULL(0xa00000000);

    cvm_vcpu->guest_mideleg = 0;
    cvm_vcpu->guest_mideleg |= MIP_SGEIP | MIP_STIP | MIP_VSTIP | MIP_VSSIP | MIP_VSEIP;
    
    cvm_vcpu->guest_medeleg = 0;
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_MISALIGNED_FETCH);
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_BREAKPOINT);
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_USER_ECALL);
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_FETCH_PAGE_FAULT);
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_LOAD_PAGE_FAULT);
    cvm_vcpu->guest_medeleg |= (1UL << CAUSE_STORE_PAGE_FAULT);

    cvm_vcpu->guest_hgatp = 0;
    cvm_vcpu->guest_hgatp |= SATP_MODE_SV57 << HGATP64_MODE_SHIFT;
    cvm_vcpu->guest_hgatp |= (cvm_vcpu->cvm->vmid->vmid) << HGATP64_VMID_SHIFT;
    cvm_vcpu->guest_hgatp |= (cvm_vcpu->cvm->root_pt >> RISCV_PGSHIFT) & HGATP64_PPN;

    //Tag protected context
    cvm_vcpu->cxt_tag = 0x00000000FFFFFFFF;
}

uint32_t get_cvm_id()
{
	return cpus[csr_read(CSR_MHARTID)].cvmid;
}

uint32_t get_cvm_vcpu_id()
{
	return cpus[csr_read(CSR_MHARTID)].vcpuid;
}

int check_in_cmode()
{
	return cpus[csr_read(CSR_MHARTID)].cmode;
}

static void enter_cmode(uint32_t cvmid, uint32_t vcpuid)
{
	cpus[csr_read(CSR_MHARTID)].cmode = 1;
	cpus[csr_read(CSR_MHARTID)].cvmid = cvmid;
	cpus[csr_read(CSR_MHARTID)].vcpuid = vcpuid;
}

static void exit_cmode()
{
	cpus[csr_read(CSR_MHARTID)].cmode = 0;
	cpus[csr_read(CSR_MHARTID)].cvmid = -1;
	cpus[csr_read(CSR_MHARTID)].vcpuid = -1;
}

static void swap_in_context(uint64_t* next_ctx, uint64_t* prev_ctx, uint64_t* host_regs)
{
	uint64_t i;
    
    for(i = 1; i < 32; i++)
    {
        prev_ctx[i] = host_regs[i];
        host_regs[i] = next_ctx[i];
    }
}

static void swap_in_ptbr(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_HGATP, *next_val);
}

static void swap_in_sstatus(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_SSTATUS, *next_val);
}

static void swap_in_hstatus(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_HSTATUS, *next_val);
}

static void swap_in_scounteren(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_SCOUNTEREN, *next_val);
}

// static void swap_in_sscratch(uint64_t* next_val, uint64_t* prev_val)
// {
//     *prev_val = csr_swap(CSR_SSCRATCH, *next_val);
// }

static void swap_in_mepc(uint64_t* next_val, uint64_t* prev_val, struct sbi_trap_regs* host_regs)
{
    *prev_val = host_regs->mepc;
    host_regs->mepc = *next_val;

}

static void swap_in_mie(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_MIE, *next_val);
}

static void swap_in_mstatus(uint64_t* next_val, uint64_t* prev_val, struct sbi_trap_regs* host_regs)
{
    *prev_val = host_regs->mstatus;
    host_regs->mstatus = *next_val;
}

static void swap_in_mideleg(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_MIDELEG, *next_val);
}

static void swap_in_medeleg(uint64_t* next_val, uint64_t* prev_val)
{
    *prev_val = csr_swap(CSR_MEDELEG, *next_val);
}

static void context_switch_to_cvm(struct sbi_trap_regs* host_regs, struct cvm_vcpu* cvm_vcpu)
{
    swap_in_context((uint64_t*)&cvm_vcpu->guest_context, (uint64_t*)&cvm_vcpu->host_context, (uint64_t*)host_regs);
    swap_in_ptbr(&cvm_vcpu->guest_hgatp, &cvm_vcpu->host_hgatp);
    swap_in_sstatus(&cvm_vcpu->guest_context.sstatus, &cvm_vcpu->host_context.sstatus);
    swap_in_hstatus(&cvm_vcpu->guest_context.hstatus, &cvm_vcpu->host_context.hstatus);
    swap_in_scounteren(&cvm_vcpu->guest_csr.scounteren, &cvm_vcpu->host_scounteren);
    // swap_in_sscratch(&cvm_vcpu->guest_context.a0, &cvm_vcpu->host_sscratch);
    swap_in_mepc(&cvm_vcpu->guest_context.sepc, &cvm_vcpu->host_mepc, host_regs);
    swap_in_mie(&cvm_vcpu->guest_mie, &cvm_vcpu->host_mie);
    swap_in_mstatus(&cvm_vcpu->guest_mstatus, &cvm_vcpu->host_mstatus, host_regs);
    swap_in_mideleg(&cvm_vcpu->guest_mideleg, &cvm_vcpu->host_mideleg);
    swap_in_medeleg(&cvm_vcpu->guest_medeleg, &cvm_vcpu->host_medeleg);
    csr_clear(CSR_MIDELEG, MIP_SEIP);
    csr_clear(CSR_MIP, MIP_SEIP);
    // csr_set(CSR_MIE, MIP_SEIP);
}

static void context_switch_to_host(struct sbi_trap_regs* host_regs, struct cvm_vcpu* cvm_vcpu)
{
    swap_in_context((uint64_t*)&cvm_vcpu->host_context, (uint64_t*)&cvm_vcpu->guest_context, (uint64_t*)host_regs);
    swap_in_ptbr(&cvm_vcpu->host_hgatp, &cvm_vcpu->guest_hgatp);
    swap_in_sstatus(&cvm_vcpu->host_context.sstatus, &cvm_vcpu->guest_context.sstatus);
    swap_in_hstatus(&cvm_vcpu->host_context.hstatus, &cvm_vcpu->guest_context.hstatus);
    swap_in_mepc(&cvm_vcpu->host_mepc, &cvm_vcpu->guest_context.sepc, host_regs);
    swap_in_scounteren(&cvm_vcpu->host_scounteren, &cvm_vcpu->guest_csr.scounteren);
    // swap_in_sscratch(&cvm_vcpu->host_sscratch, &cvm_vcpu->guest_context.a0);
    cvm_vcpu->host_mie |= cvm_vcpu->guest_mie & (MIP_VSTIP | MIP_VSSIP | MIP_VSEIP);
    swap_in_mie(&cvm_vcpu->host_mie, &cvm_vcpu->guest_mie);
    swap_in_mstatus(&cvm_vcpu->host_mstatus, &cvm_vcpu->guest_mstatus, host_regs);
    swap_in_mideleg(&cvm_vcpu->host_mideleg, &cvm_vcpu->guest_mideleg);
    swap_in_medeleg(&cvm_vcpu->host_medeleg, &cvm_vcpu->guest_medeleg);
}

unsigned long tag_mmio_load(unsigned long htinst)
{
    unsigned long tag = 0;
    unsigned long insn;
    int reg_num = 0;
    if (htinst & 0x1){
        insn = htinst | INSN_16BIT_MASK;
    }
    else{
        return 0;
    }
    if ((insn & INSN_MASK_LW) == INSN_MATCH_LW){
        reg_num = GET_RDNUM(insn);
    }
    else if ((insn & INSN_MASK_LB) == INSN_MATCH_LB){
        reg_num = GET_RDNUM(insn);
    }
    else if ((insn & INSN_MASK_LBU) == INSN_MATCH_LBU){
        reg_num = GET_RDNUM(insn);
    }
#ifdef CONFIG_64BIT
	else if ((insn & INSN_MASK_LD) == INSN_MATCH_LD) {
        reg_num = GET_RDNUM(insn);
    }
	else if ((insn & INSN_MASK_LWU) == INSN_MATCH_LWU) {
        reg_num = GET_RDNUM(insn);
    }
#endif
    else if ((insn & INSN_MASK_LH) == INSN_MATCH_LH) {
		reg_num = GET_RDNUM(insn);
	}
    else if ((insn & INSN_MASK_LHU) == INSN_MATCH_LHU) {
		reg_num = GET_RDNUM(insn);
    }
#ifdef CONFIG_64BIT
    else if((insn & INSN_MASK_C_LD) == INSN_MATCH_C_LD){
        reg_num = GET_RDSNUM(insn);
    }
    else if ((insn & INSN_MASK_C_LDSP) == INSN_MATCH_C_LDSP){
		reg_num = GET_RDNUM(insn);
    }
#endif
    else if ((insn & INSN_MASK_C_LW) == INSN_MATCH_C_LW) {
        reg_num = GET_RDSNUM(insn);
	}
    else if ((insn & INSN_MASK_C_LWSP) == INSN_MATCH_C_LWSP) {
        reg_num = GET_RDNUM(insn);
	}
    else{
        return 0;
    }
    tag = 1UL << reg_num;
    return tag;
}

unsigned long tag_mmio_store(unsigned long htinst)
{
    unsigned long tag = 0;
    unsigned long insn;
    int reg_num = 0;
    if (htinst & 0x1){
        insn = htinst | INSN_16BIT_MASK;
    }
    else{
        return 0;
    }
    if ((insn & INSN_MASK_SW) == INSN_MATCH_SW) {
		reg_num = GET_RS2NUM(insn);
	}
    else if ((insn & INSN_MASK_SB) == INSN_MATCH_SB) {
		reg_num = GET_RS2NUM(insn);
    }
    else if ((insn & INSN_MASK_SD) == INSN_MATCH_SD) {
		reg_num = GET_RS2NUM(insn);
    }
    else if ((insn & INSN_MASK_SH) == INSN_MATCH_SH) {
		reg_num = GET_RS2NUM(insn);
    }
#ifdef CONFIG_64BIT
    else if((insn & INSN_MASK_C_SD) == INSN_MATCH_C_SD){
        reg_num = GET_RS2SNUM(insn);
    }
    else if((insn & INSN_MASK_C_SDSP) == INSN_MATCH_C_SDSP){
        reg_num = GET_RS2CNUM(insn);
    }
#endif
    else if ((insn & INSN_MASK_C_SW) == INSN_MATCH_C_SW){
        reg_num = GET_RS2SNUM(insn);
    }
    else if((insn & INSN_MASK_C_SWSP) == INSN_MATCH_C_SWSP){
        reg_num = GET_RS2CNUM(insn);
    }
    else{
        return 0;
    }
    tag = 1UL << (reg_num + 32);
    return tag;
}

unsigned long tag_virtual_inst(unsigned long stval)
{
    unsigned long tag = 0;
    unsigned long insn = stval;
    int rd_num = 0;
    int rs1_num = 0;
    //sbi_printf("[IIE CVM Monitor@%s] virtual_inst insn=%lu Tag=%lu\n", __func__, insn, tag);

    if (INSN_IS_16BIT(insn)){
        return 0;
    }
    switch (GET_FUNCT3(insn)){
        case GET_FUNCT3(INSN_MATCH_CSRRW):
        case GET_FUNCT3(INSN_MATCH_CSRRS):
        case GET_FUNCT3(INSN_MATCH_CSRRC):
            rd_num = GET_RDNUM(insn);
            rs1_num = GET_RS1NUM(insn);
            tag = (1UL << rd_num) | (1UL << (rs1_num + 32));
            break;
        case GET_FUNCT3(INSN_MATCH_CSRRWI):
        case GET_FUNCT3(INSN_MATCH_CSRRSI):
        case GET_FUNCT3(INSN_MATCH_CSRRCI):
            rd_num = GET_RDNUM(insn);
            tag = 1UL << rd_num;
            break;
        default:
            break;
    }
    return tag;
}

unsigned long vcpu_reg_tag(struct cpu_trap* trap)
{
    unsigned long fault_addr;
    unsigned long tag = 0;
    switch(trap->scause){
        case CAUSE_ILLEGAL_INSTRUCTION:
	    case CAUSE_MISALIGNED_LOAD:
	    case CAUSE_MISALIGNED_STORE:
            break;
        case CAUSE_VIRTUAL_INST_FAULT:
            tag = tag_virtual_inst(trap->stval);
            break;
        case CAUSE_FETCH_GUEST_PAGE_FAULT:
            break;
	    case CAUSE_LOAD_GUEST_PAGE_FAULT:
            tag = tag_mmio_load(trap->htinst);
            //sbi_printf("[IIE CVM Monitor@%s] EXCEPTION: CAUSE_LOAD_GUEST_PAGE_FAULT Tag=%lu\n", __func__, tag);
            break;
	    case CAUSE_STORE_GUEST_PAGE_FAULT:
            tag = tag_mmio_store(trap->htinst);
            //sbi_printf("[IIE CVM Monitor@%s] EXCEPTION: CAUSE_STORE_GUEST_PAGE_FAULT Tag=0x%lx\n", __func__, tag);
            break;
        case CAUSE_VIRTUAL_SUPERVISOR_ECALL:
            tag = ((unsigned long)(TAG_REG_A0 | TAG_REG_A1 | TAG_REG_A2 | TAG_REG_A3 | TAG_REG_A4 | TAG_REG_A5 | TAG_REG_A6 | TAG_REG_A7) << 32) | ((unsigned long)(TAG_REG_A0 | TAG_REG_A1 | TAG_REG_A2 | TAG_REG_A3 | TAG_REG_A4 | TAG_REG_A5 | TAG_REG_A6 | TAG_REG_A7));
            //sbi_printf("[IIE CVM Monitor@%s] EXCEPTION: CAUSE_VIRTUAL_SUPERVISOR_ECALL Tag=%lu\n", __func__, tag);
            break;
        default:
            break;
    }
    return tag;
}

void copy_regs_tagged(void *cxt_dest, void *cxt_src, uint32_t tag){
    signed long *temp1;
    signed long *temp2;
    for(int i = 0; i <= 31; i++){
        if((tag >> i) & (uint32_t)1){
            temp1 = cxt_dest + i * sizeof(unsigned long);
            temp2 = cxt_src + i * sizeof(unsigned long);
            sbi_memcpy(temp1, temp2, sizeof(unsigned long));
        }
    }
}

int cvm_vcpu_enter(struct sbi_trap_regs* host_regs, struct cvm_vcpu* cvm_vcpu, uint64_t kvm_vcpu_context, uint64_t kvm_vcpu_csr, uint64_t kvm_trap)
{
	struct sbi_cvm *cvm = cvm_vcpu->cvm;
    struct cpu_context* kvm_guest_context = (struct cpu_context*)kvm_vcpu_context;
    struct vcpu_csr* kvm_guest_csr = (struct vcpu_csr*)kvm_vcpu_csr;

    // cvm_vcpu->kvm_vcpu_trap = (struct cpu_trap*)kvm_vcpu_trap;
    cvm_vcpu->kvm_vcpu_context = kvm_guest_context;
    cvm_vcpu->kvm_vcpu_csr = kvm_guest_csr;
    cvm_vcpu->kvm_vcpu_trap = kvm_trap;

    // TODO only copy selected reg value
    cvm_vcpu->guest_context.sstatus = kvm_guest_context->sstatus;
    cvm_vcpu->guest_context.hstatus = kvm_guest_context->hstatus;
    cvm_vcpu->guest_context.sepc = kvm_guest_context->sepc;
    copy_regs_tagged(&cvm_vcpu->guest_context, kvm_guest_context, (uint32_t)cvm_vcpu->cxt_tag);
    // sbi_memcpy(&cvm_vcpu->guest_context, kvm_guest_context, sizeof(struct cpu_context));
    // sbi_memcpy(&cvm_vcpu->guest_csr, kvm_guest_csr, sizeof(struct vcpu_csr));
    context_switch_to_cvm(host_regs, cvm_vcpu);
    __sbi_hfence_vvma_all();
    // if(exit_reason == IRQ_S_EXT)
    // {
    //     sbi_printf("enter: mepc %lx mie %lx mip %lx mideleg %lx\r\n", host_regs->mepc, csr_read(CSR_MIE),  csr_read(CSR_MIP),  csr_read(CSR_MIDELEG));
    //     csr_clear(CSR_MIP, MIP_SEIP);
    //     sbi_printf("enter: mepc %lx mie %lx mip %lx mideleg %lx\r\n", host_regs->mepc, csr_read(CSR_MIE),  csr_read(CSR_MIP),  csr_read(CSR_MIDELEG));
    // }
    // sbi_printf("*******************enter*********************\r\n");
    // debug_print_csr(host_regs, cvm_vcpu);
	//TODO flush TLB
    // csr_write(CSR_MSTATUS, host_regs->mstatus);
    // sbi_printf("enter: mideleg %lx mie %lx\r\n", csr_read(CSR_MIDELEG), csr_read(CSR_MIE));
    // if(!(csr_read(CSR_MIE) & MIP_SEIP))
    //     sbi_printf("enter: mideleg %lx mie %lx\r\n", csr_read(CSR_MIDELEG), csr_read(CSR_MIE));
    // sbi_printf("enter: mepc %lx hstatus %lx mstatus %lx\r\n", host_regs->mepc, cvm_vcpu->guest_context.hstatus, cvm_vcpu->guest_mstatus);
    // sbi_printf("enter: medeleg %lx mideleg %lx mie %lx\r\n", csr_read(CSR_MEDELEG), csr_read(CSR_MIDELEG), csr_read(CSR_MIE));
    // sbi_printf("enter: hgatp %lx vsatp %lx host_hgatp %lx\r\n", csr_read(CSR_HGATP), csr_read(CSR_VSATP), cvm_vcpu->host_hgatp);
    enter_cmode(cvm->vmid->vmid, *cvm_vcpu->vcpu_id); 
    // sbi_printf("enter cvm vcpu trap %lx\n", kvm_trap);
    return 0;
}

int cvm_vcpu_exit(struct sbi_trap_regs* host_regs)
{
    uint32_t vmid = get_cvm_id();
    uint32_t vcpuid = get_cvm_vcpu_id();
    struct cvm_vcpu_node *vcpu_node;
    struct cvm_vcpu* cvm_vcpu;
    struct cpu_trap trap;
	struct cpu_context* kvm_vcpu_context;
	// struct vcpu_csr* kvm_vcpu_csr = cvm_vcpu->kvm_vcpu_csr;
    struct cpu_trap* kvm_vcpu_trap;

    vcpu_node = get_cvm_vcpu_node(vmid, vcpuid);
    if(!vcpu_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu_node is NULL!\n", __func__);
		return -1;
	}

	cvm_vcpu = &vcpu_node->vcpu;
	if(!cvm_vcpu)
	{
		sbi_printf("[IIE CVM Monitor@%s] vcpu is NULL!\n", __func__);
		return -1;
	}
    // 更新kvm_vcpu_context 和 kvm_vcpu_trap; 
    kvm_vcpu_context = cvm_vcpu->kvm_vcpu_context;
    kvm_vcpu_trap = cvm_vcpu->kvm_vcpu_trap;
    
    /* 
    trap.scause = csr_read(CSR_MCAUSE);
    trap.stval = csr_read(CSR_MTVAL);
    trap.htval = csr_read(CSR_MTVAL2);
    // trap.htinst = csr_read(CSR_MTINST);
    csr_write(CSR_SCAUSE, trap.scause);
    csr_write(CSR_STVAL, trap.stval);
    csr_write(CSR_HTVAL, trap.htval);
    // csr_write(CSR_HTINST, 0);
    // TODO 读取inst 更新CSR_HTINST
    // csr_write(CSR_HTINST, trap.htinst);
    */
    kvm_vcpu_trap->sepc = csr_read(CSR_MEPC);
    kvm_vcpu_trap->scause = csr_read(CSR_MCAUSE);
    kvm_vcpu_trap->stval = csr_read(CSR_MTVAL);
    kvm_vcpu_trap->htval = csr_read(CSR_MTVAL2);
    kvm_vcpu_trap->htinst = csr_read(CSR_MTINST);


    // set exit_reason
    // exit_reason = trap.scause & ~(1UL << (__riscv_xlen - 1));

    context_switch_to_host(host_regs, cvm_vcpu);
    host_regs->mepc +=4;

    cvm_vcpu->guest_context.hstatus &= ~HSTATUS_SPVP;
    if((cvm_vcpu->guest_mstatus & MSTATUS_MPP) == (PRV_S<< MSTATUS_MPP_SHIFT))
        cvm_vcpu->guest_context.hstatus |= HSTATUS_SPVP;

    cvm_vcpu->guest_context.hstatus &= ~HSTATUS_SPV;
    if((cvm_vcpu->guest_mstatus & MSTATUS_MPV) == MSTATUS_MPV)
        cvm_vcpu->guest_context.hstatus |= HSTATUS_SPV;

    cvm_vcpu->guest_context.hstatus &= ~HSTATUS_GVA;
    if((cvm_vcpu->guest_mstatus & MSTATUS_GVA) == MSTATUS_GVA)
        cvm_vcpu->guest_context.hstatus |= HSTATUS_GVA;

    cvm_vcpu->cxt_tag = vcpu_reg_tag(kvm_vcpu_trap);
    kvm_vcpu_context->sstatus = cvm_vcpu->guest_context.sstatus;
    kvm_vcpu_context->hstatus = cvm_vcpu->guest_context.hstatus;
    kvm_vcpu_context->sepc = cvm_vcpu->guest_context.sepc;
    copy_regs_tagged(kvm_vcpu_context, &cvm_vcpu->guest_context, (uint32_t)(cvm_vcpu->cxt_tag >> 32));

    // sbi_memcpy(kvm_vcpu_context, &cvm_vcpu->guest_context, sizeof(struct cpu_context));
    // sbi_memcpy(kvm_vcpu_csr, &cvm_vcpu->guest_csr, sizeof(struct vcpu_csr));
    // sbi_printf("*******************exit*********************\r\n");
    // debug_print_csr(host_regs, cvm_vcpu);
    // if(exit_reason == IRQ_S_EXT)
    // {
    //     sbi_printf("exit: mepc %lx mie %lx mip %lx mideleg %lx\r\n", host_regs->mepc, csr_read(CSR_MIE),  csr_read(CSR_MIP),  csr_read(CSR_MIDELEG));
    // }
    // sbi_printf("exit: trap sepc %lx scause %lx stval %lx htval %lx htinst %lx \r\n", 
    //     cvm_vcpu->guest_context.sepc, trap.scause, trap.stval, trap.htval, trap.htinst);
    // sbi_printf("exit: mepc %lx hstatus %lx mstatus %lx\r\n", host_regs->mepc, cvm_vcpu->guest_context.hstatus, cvm_vcpu->guest_mstatus);
	// sbi_printf("exit: hgatp %lx vsatp %lx host_hgatp %lx\r\n", csr_read(CSR_HGATP), csr_read(CSR_VSATP), cvm_vcpu->host_hgatp);
    //TODO flush TLB
    __sbi_hfence_vvma_all();
    exit_cmode();
    // sbi_printf("exit cvm vcpu trap %lx\n", kvm_vcpu_trap);
    return 0;
}
/* CVM Ctx Switch ------------------------------------------------------------------------------ */

/* CVM Life Cycle ------------------------------------------------------------------------------ */

int sbi_cvm_create(struct iie_cvm_sbi_params *cvm_sbi_params)
{
	sbi_printf("[IIE CVM Monitor@%s] vmid = %ld\n", __func__,cvm_sbi_params->vmid_ptr->vmid);
    int ret=0;
    paddr_t free_page;
	/* alloc a memory block for cvm struct */
	struct cvm_node* cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    if(!cvm_node)
    {   
        ret = alloc_cvm_node(&cvm_sbi_params->vmid_ptr->vmid, &free_page);
        if(ret)
            return ret;
        cvm_node = (struct cvm_node *)free_page;
        sbi_printf("[IIE CVM Monitor@%s] cvm allocating... \r\n", __func__);
    }
    else return 0;
    
	sbi_printf("[IIE CVM Monitor@%s] cvm allocation is successfull. \r\n", __func__);
    


	/* Initialize cvm structure*/
	cvm_node->cvm.vmid = cvm_sbi_params->vmid_ptr;
	cvm_node->cvm.KeyID = get_keyID(cvm_node->cvm.vmid);
	cvm_node->cvm.state = INITIALIZING;
    ret = alloc_cvm_vcpu_node(&cvm_sbi_params->vmid_ptr->vmid, &free_page);
    if(ret)
        return ret;
	cvm_node->cvm.cvm_vcpu_list_head = (struct cvm_vcpu_node *)free_page;
	cvm_node->cvm.cvm_vcpu_list_head->next = NULL;
	cvm_node->cvm.cmode = 1;
    cvm_node->cvm.KeyID = gen_key_id();
    sbi_memset(cvm_node->cvm.hash, 0, sizeof(cvm_node->cvm.hash));
    
	cvm_node->next = NULL;


	/* maintain the cvm List */
	// sbi_printf("[IIE CVM Monitor@%s] CVM List. \r\n", __func__);
	if(iie_cvm_list_head == NULL)
	{
		/* alloc memory first */
        paddr_t free_page;
        int ret=0;
        ret = alloc_cvm_node(&cvm_sbi_params->vmid_ptr->vmid, &free_page);
        if(ret)
            return ret;
		iie_cvm_list_head = (struct cvm_node *)free_page;
		iie_cvm_list_head->next = cvm_node;
	}
	else cvm_insert_node(cvm_node);
	// sbi_printf("[IIE CVM Monitor@%s] print_cvm_list. \r\n", __func__);
#ifdef DEBUG_WSW
    print_cvm_list();
#endif

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
    int ret=0;
	struct kvm_vmid *vmid_ptr = cvm_sbi_params->vmid_ptr;
	int *vcpu_id_ptr = cvm_sbi_params->vcpu_id_ptr;
	struct cvm_node *cvm_node = get_cvm(vmid_ptr->vmid);
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] CVM Node is NULL, get_cvm failed!\n", __func__);
		return -1;
	}
	struct cvm_vcpu_node *vcpu_list_head = cvm_node->cvm.cvm_vcpu_list_head;
	struct cvm_vcpu_node *vcpu_node = get_cvm_vcpu_node(vmid_ptr->vmid, *vcpu_id_ptr);
    if(!vcpu_node)
    {
        paddr_t free_page;
        ret = alloc_cvm_vcpu_node(&cvm_sbi_params->vmid_ptr->vmid, &free_page);
        if(ret)
            return ret;
        vcpu_node = (struct cvm_vcpu_node *)free_page;
        sbi_printf("[IIE CVM Monitor@%s] CVM %d vcpu allocating... \r\n", __func__, vmid_ptr->vmid);
    }
    else return 0;
    
	if(vcpu_node == TEE_NO_MEMORY)
	{
		sbi_printf("[IIE CVM Monitor@%s] CVM %d vcpu Node allocation is failed!\n", __func__, vmid_ptr->vmid);
		return TEE_NO_MEMORY;
	}
    sbi_printf("[IIE CVM Monitor@%s] CVM %d vcpu Node allocation is successfull!\n", __func__, vmid_ptr->vmid);
	vcpu_node->vcpu.vcpu_id = vcpu_id_ptr;
	vcpu_node->vcpu.cvm = &cvm_node->cvm;

    // init vcpu ctx
    init_cvm_vcpu(&vcpu_node->vcpu);

	sbi_printf("[IIE CVM Monitor@%s] vmid = %ld\n", __func__,vmid_ptr->vmid);
	sbi_printf("[IIE CVM Monitor@%s] vcpu_id = %d\n", __func__,*vcpu_id_ptr);
	

	if(vcpu_list_head->next == NULL)
	{
		vcpu_list_head->next = vcpu_node;
	}
	else cvm_insert_vcpu_node(vcpu_list_head, vcpu_node);

#ifdef DEBUG_WSW
    print_vcpu_list(cvm_node);
#endif
	return 0;
}

int sbi_cvm_run_vcpu(struct sbi_trap_regs *regs, struct iie_cvm_sbi_params * cvm_sbi_params, uint64_t kvm_vcpu_context, uint64_t kvm_vcpu_csr, uint64_t kvm_trap)
{   
	struct cvm_vcpu_node *vcpu_node = get_cvm_vcpu_node(cvm_sbi_params->vmid_ptr->vmid, *cvm_sbi_params->vcpu_id_ptr);	
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
    if(!cvm)
	{
		sbi_printf("[IIE CVM Monitor@%s] cvm is NULL!\n", __func__);
		return -1;
	}

	if(cmode && cvm->state != READY)
	{
		/* Exception: CVM hasn't been initialized. */
		// sbi_printf("[IIE CVM Monitor@%s] Exception: CVM hasn't been initialized.\n", __func__);
		// exit(0);

        /* Here should terminate, do not delete!!! */
		// return 0xff;
	}

    // TODO 
// if(exit_reason == swiotlb_pf)
// cvm_sbi_params.hpa
// kvm_trap.htval(GPA)
// create_gpa_hpa mapping 
    if(vcpu->exit_reason == SWIOTLB){
        int ret;
        paddr_t gpa = cvm_sbi_params->gpa;
        paddr_t hpa = cvm_sbi_params->hpa;
        ret = add_cvm_share_pages(cvm, gpa, hpa, true, 0);
        if(ret == CVM_ERROR || ret == TEE_NO_MEMORY){
            sbi_printf("[IIE CVM Monitor@%s] SWIOTLB PF failed!\n", __func__);
            return ret;
        }
        vcpu->exit_reason = 0UL;
    }

    // enter cvm vcpu ctx
    cvm_vcpu_enter(regs, vcpu, kvm_vcpu_context, kvm_vcpu_csr, kvm_trap);

	return 0;
}

int sbi_cvm_create_finalize(struct iie_cvm_sbi_params * cvm_sbi_params)
{
    /* 
        SBI_TLB_INFO_INIT(&tlb_info, regs->a2, regs->a3, 0, 0, SBI_TLB_HFENCE_GVMA, source_hart);
        sbi_tlb_request(regs->a0, regs->a1, &tlb_info);
        int sbi_tlb_request(ulong hmask, ulong hbase, struct sbi_tlb_info *tinfo);
        int sbi_ipi_send_many(ulong hmask, ulong hbase, u32 event, void *data);
        a0: hmask: any value
        a1: hbase: -1UL: ignore hmask
        a2: start: 0
		a3: size: 0
            The remote fence function acts as a full TLB flush if
                • start_addr and size are both 0
                • size is equal to 2^XLEN-1
		a4: vmid = 0
        a5: gpa
        hart_mask_base can be set to -1 to indicate that hart_mask can be ignored 
        and all available harts must be considered.
    */
   	struct sbi_tlb_info tlb_info;
   	u32 source_hart = current_hartid();
    /* #define SBI_TLB_INFO_INIT(__p, __start, __size, __asid, __vmid, __type, __src) */
    SBI_TLB_INFO_INIT(&tlb_info, 0, 0, 0, 0, SBI_TLB_HFENCE_GVMA, source_hart);
    /* int sbi_tlb_request(ulong hmask, ulong hbase, struct sbi_tlb_info *tinfo); */            
	int ret = sbi_tlb_request(0, -1UL, &tlb_info);

	return ret;
}

int sbi_cvm_create_memory_region(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

int sbi_cvm_hash_image(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	struct cvm_node* cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    hash_cvm(&cvm_node->cvm, (void *)cvm_node->cvm.hash, 0);
    sbi_printf("[IIE CVM Monitor@%s] hash value = %s. \r\n", __func__, cvm_node->cvm.hash);

	return 0;
}

int sbi_cvm_attest(struct iie_cvm_sbi_params * cvm_sbi_params)
{
    struct report_t report;
    attest_cvm(cvm_sbi_params->vmid_ptr->vmid, (uintptr_t)&report, 0);
    sbi_printf("[IIE CVM Monitor@%s] sm_report hash = %s\nsm_report signature = %s\nsm_report pub_key = %s\nenclave hash = %s\nenclave signature = %s\ndev_pub_key = %s\n", 
    __func__, 
    report.sm.hash,
    report.sm.signature,
    report.sm.sm_pub_key,
    report.enclave.hash,
    report.enclave.signature,
    report.dev_pub_key);
    // sbi_cvm_gen_mem_enc_key();
    return 0;
}

int sbi_cvm_gen_mem_enc_key()
{
    const int len = 16;
    unsigned char rand[len];
    sbi_memset(rand, 0, sizeof(rand));
    SM2_Gen_Random(rand);
    // sbi_printf("[IIE CVM Monitor@%s] rand = ", __func__);
    // for(int i = 0; i < len; ++ i) sbi_printf("%d", rand[i]);
    // sbi_printf("\n");

    return 0;
}

int sbi_cvm_test(struct iie_cvm_sbi_params * cvm_sbi_params)
{
    sbi_printf("[IIE CVM Monitor@%s] ------ Begin CVM Test ------. \r\n", __func__);

	// struct cvm_node* cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    // cvm_delete_node(cvm_node);
   	// cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);

    // sbi_cvm_create(cvm_sbi_params);
    // cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    // sbi_cvm_create_vcpu(cvm_sbi_params);
    // cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    // sbi_cvm_create_vcpu(cvm_sbi_params);
    // cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    // cvm_delete_vcpu_node();

    // /* 1010 .... 0 10 10100*/
    // uint64_t key_CSR = 0xa00000054UL;
    // sbi_printf("[IIE CVM Monitor@%s] key_id = %lx. \r\n", __func__, read_key_id(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] mode = %lx. \r\n", __func__, read_mode(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] tweak_flag = %lx. \r\n", __func__, read_tweak_flag(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] random_ready_flag = %lx. \r\n", __func__, read_random_ready_flag(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] key_expansion_idle = %lx. \r\n", __func__, read_key_expansion_idle(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] last_req_accepted = %lx. \r\n", __func__, read_last_req_accepted(&key_CSR));
    // sbi_printf("[IIE CVM Monitor@%s] cfg_succesd = %lx. \r\n", __func__, read_cfg_succesd(&key_CSR));

    // /* write: 0110 .... 1 1 11 01101*/
    // write_key_id(&key_CSR, 0b01101);
    // write_mode(&key_CSR, 0b11);
    // write_tweak_flag(&key_CSR, 0b1);
    // write_memenc_enable(&key_CSR, 0b1);
    // write_key_init_req(&key_CSR, 0b1);
    // sbi_printf("[IIE CVM Monitor@%s] key_CSR = %lx. \r\n", __func__, key_CSR);
    int VCPU_ID = *cvm_sbi_params->vcpu_id_ptr;
    multi_key_sys_init(true);
    // if(VCPU_ID) config_tweak_key();
    // else test_cfg();
    // test_cfg(); 
    sbi_printf("[IIE CVM Monitor@%s] ------ End CVM Test ------. \r\n", __func__);

    return 0;
}


int sbi_cvm_init_mem_pool(struct iie_cvm_sbi_params * cvm_sbi_params)
{
	/* TODO */
	return 0;
}

int sbi_cvm_destroy(struct iie_cvm_sbi_params * cvm_sbi_params)
{
    struct kvm_vmid *vmid_ptr = cvm_sbi_params->vmid_ptr;
	int *vcpu_id_ptr = cvm_sbi_params->vcpu_id_ptr;
	struct cvm_node *cvm_node = get_cvm(vmid_ptr->vmid);
    if(!cvm_node)
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM %ld does not exist. \r\n", __func__, vmid_ptr->vmid);
        return -1;
    }
    int ret = cvm_delete_node(cvm_node);
    if(ret)
    {
        sbi_printf("[IIE CVM Monitor@%s] Error %d, CVM %ld destroy failed. \r\n", __func__, ret, vmid_ptr->vmid);
        return -1;
    }
    return 0;
}

inline void print_cvm_list()
{
	int i = 1;
	for(struct cvm_node *cur = iie_cvm_list_head; cur->next; cur = cur->next)
	{
		struct cvm_node *node = cur->next;
		sbi_printf("[IIE CVM DEBUG@%s] vmid = %ld\tKeyID = %d\tcount = %d\n", __func__, node->cvm.vmid->vmid, node->cvm.KeyID, i ++);
	}
}

inline void print_vcpu_list(struct cvm_node *cvm_node)
{
	int i = 1;
	for(struct cvm_vcpu_node *cur = cvm_node->cvm.cvm_vcpu_list_head; cur->next; cur = cur->next)
	{
		struct cvm_vcpu_node *node = cur->next;
		sbi_printf("[IIE CVM DEBUG@%s] vcpuid = %ld\n", __func__, *node->vcpu.vcpu_id);
	}
}

/* CVM Life Cycle ------------------------------------------------------------------------------ */

//bitmap operation
//todo : here is no check about whether bitmap bit is already set(share condition).
void set_bitmap(paddr_t page_address){
    uint64_t index = (page_address - 0x80000000) >> PAGE_SHIFT >> BITMAP_OFFSET;
    if(index > bitmap_max_ele){
        sbi_printf("index is %lx, own_max_num is %lx, addr is %lx\n", index, bitmap_max_ele, page_address);
        sbi_printf("[IIE CVM Monitor@%s] phy addr index > max entry when set bitmap\n", __func__);
    }
    uint64_t offset = ((page_address - 0x80000000) >> PAGE_SHIFT) & ((1 << BITMAP_OFFSET) - 1);
    spin_lock(&spin_lock_bitmap);
    uint64_t bits = *(bitmap + index);
    *(bitmap + index) = bits | (1<<offset);
    spin_unlock(&spin_lock_bitmap);
}
void reset_bitmap(paddr_t page_address){
    uint64_t index = (page_address - 0x80000000) >> PAGE_SHIFT >> BITMAP_OFFSET;
    uint64_t offset = ((page_address - 0x80000000) >> PAGE_SHIFT) & ((1 << BITMAP_OFFSET) - 1);
    spin_lock(&spin_lock_bitmap);
    uint64_t bits = *(bitmap + index);
    *(bitmap + index) = bits & (~(1<<offset));
    spin_unlock(&spin_lock_bitmap);
}
int init_bitmap(struct cvm_list_params* bmp){
    unsigned long i;
    bitmap_max_ele = bmp->ele_num;
    //sbi_printf("bitmap addr is 0x%lx\n", (paddr_t)bmp->addr);
    bitmap = (unsigned long *)bmp->addr;
    //sbi_printf("bitmap entry is 0x%lx\n", (paddr_t)bmp->ele_num);
    spin_lock(&spin_lock_bitmap);
    for(i=0; i<bmp->ele_num; i++){
        *(bitmap + i) = 0;
    }
    spin_unlock(&spin_lock_bitmap);
    //set bitmap page
    //sbi_printf("bitmap page num is 0x%lx\n", (paddr_t)bmp->page_num);
    for(i=0; i<bmp->page_num; i++){
        set_bitmap((uint64_t)bitmap + i*PAGE_SIZE);
    }
    return 0;
}

//page own table operation
//we should ignore check those which are uesed to CM share pages because their ids are always inconsistent;
//todo : there must be another check mechanism for CM share pages.
int set_page_own_table(paddr_t page_address, paddr_t* vmidp){
    uint64_t index = (page_address - 0x80000000) >> PAGE_SHIFT;
    if(index > own_max_num){
            sbi_printf("index is %lx, own_max_num is %lx\n", index, own_max_num);
            sbi_printf("[IIE CVM Monitor@%s] phy addr index > max entry when set page_own_table----------\n", __func__);
            return CVM_ERROR;
    }
    if(ownership_table_level == 0){
        spin_lock(&spin_lock_page_own_table); 
        if(((page_own_table + index)->vmidp) == 0){
            (page_own_table + index)->vmidp = (unsigned long)vmidp;
            spin_unlock(&spin_lock_page_own_table);
            return 0;
        }else{
            //the physical page already allocate to another cvm or remap to the same cvm
            spin_unlock(&spin_lock_page_own_table);
            sbi_printf("[IIE CVM DEBUG@%s] current id is 0x%lx, pageowntable id is 0x%lx\n", __func__, (unsigned long)vmidp, ((page_own_table+index))->vmidp);
            sbi_printf("[IIE CVM Monitor@%s] phycisal address 0x%lx set pageowntable failed because id mismatch. \n", __func__, page_address);
            return CVM_ERROR; 
        }
    }else if(ownership_table_level == 1){
        unsigned long first_level = index >> 19;
        unsigned long second_level = index & ((1 << 19) - 1);
        // sbi_printf("first_level is %lx, second_level is %lx\n", first_level, second_level);
        unsigned long first_level_addr = *((unsigned long *)page_own_table + first_level);
        spin_lock(&spin_lock_page_own_table); 
        if((((page_own_table_t *)first_level_addr + second_level)->vmidp)==0){
            ((page_own_table_t *)first_level_addr + second_level)->vmidp = (unsigned long)vmidp;
            spin_unlock(&spin_lock_page_own_table);
            return 0;
        }else{
            //the physical page already allocate to another cvm or remap to the same cvm
            spin_unlock(&spin_lock_page_own_table);
            sbi_printf("[IIE CVM DEBUG@%s] current id is 0x%lx, pageowntable id is 0x%lx\n", __func__, vmidp, ((page_own_table_t *)first_level_addr + second_level)->vmidp);
            sbi_printf("[IIE CVM Monitor@%s] phycisal address 0x%lx set pageowntable failed because id mismatch. \n", __func__, page_address);
            return CVM_ERROR; 
        }
    }else{
        sbi_printf("[IIE CVM DEBUG@%s] The value of ownership_table_level is illegal.\n", __func__);
        return CVM_ERROR;
    }
}
int reset_page_own_table(paddr_t page_address, paddr_t* vmidp){
    uint64_t index = (page_address - 0x80000000) >> PAGE_SHIFT;
    if(index > own_max_num){
        sbi_printf("paddr is %lx, index is %lx, own_max_num is %lx\n", page_address, index, own_max_num);
        sbi_printf("[IIE CVM Monitor@%s] phy addr index > max entry when reset page_own_table.\n", __func__);
        return -1;
    }
    if(ownership_table_level == 0){
        spin_lock(&spin_lock_page_own_table);
        //the physical page already allocate to another cvm or swiotlb
        if((page_own_table+index)->vmidp == (unsigned long)vmidp){
            (page_own_table+index)->vmidp = 0;
            spin_unlock(&spin_lock_page_own_table);
            return 0;
        }
        else{
            spin_unlock(&spin_lock_page_own_table);
            sbi_printf("[IIE CVM DEBUG@%s] id mismatch when resest page own table of addr 0x%lx\n", __func__, page_address);
            sbi_printf("[IIE CVM DEBUG@%s] current id is 0x%lx, pageowntable id is 0x%lx\n", __func__, vmidp, ((page_own_table+index))->vmidp);
            return CVM_ERROR;
        }
    }else if(ownership_table_level == 1){
        unsigned long first_level = index >> 19;
        unsigned long second_level = index & ((1<<19)-1);
        // sbi_printf("first_level is %lx, second_level is %lx\n", first_level, second_level);
        unsigned long first_level_addr = *((unsigned long *)page_own_table + first_level);
        // sbi_printf("first_level_addr is %lx\n", first_level_addr);
        spin_lock(&spin_lock_page_own_table); 
        if((((page_own_table_t *)first_level_addr + second_level)->vmidp) == (unsigned long)vmidp){
            ((page_own_table_t *)first_level_addr + second_level)->vmidp = 0;
            spin_unlock(&spin_lock_page_own_table);
            return 0;
        }else{
            //the physical page already allocate to another cvm or remap to the same cvm.
            spin_unlock(&spin_lock_page_own_table);
            sbi_printf("[IIE CVM DEBUG@%s] current id is 0x%lx, pageowntable id is 0x%lx\n", __func__, vmidp, ((page_own_table_t *)first_level_addr + second_level)->vmidp);
            sbi_printf("[IIE CVM Monitor@%s] phycisal address 0x%lx set pageowntable failed because id mismatch. \n", __func__, page_address);
            return CVM_ERROR; 
        }
    }else{
        sbi_printf("[IIE CVM DEBUG@%s] The value of ownership_table_level is illegal.\n", __func__);
        return CVM_ERROR;
    }
}
int init_page_own_table(struct cvm_list_params* own_table){
    unsigned long i,j;
    own_max_num = own_table->ele_num;
    page_own_table = (page_own_table_t *)own_table->addr;
    set_bitmap(own_table->addr);
    if(own_table->level == 0){
        ownership_table_level = 0;
        for(i=0; i<own_table->page_num; i++){
            set_bitmap(own_table->addr + i*PAGE_SIZE);
        }
        for(i=0; i< own_table->ele_num; i++){
            (page_own_table + i)->vmidp = 0;
        }
    }else{
        ownership_table_level = 1;
        page_own_table_t *addr;
        //own_table->level must belong to [0, 512]
        for(i=0; i<own_table->level; i++){
            // sbi_printf("------%dth page paddr is %lx\n", i, *((unsigned long *)own_table->addr + i));
            for(j=0; j<1024; j++){
                set_bitmap(*((unsigned long *)own_table->addr + i) + j*PAGE_SIZE);
            }
        }
        for(i=0; i<own_table->level; i++){
            addr = (page_own_table_t *)*((unsigned long *)own_table->addr + i);
            for(j=0;j<(2^19);j++){
                (addr + j)->vmidp = 0;
            }
        }
    } 
    return 0; 
}

static void put_chunk(struct cvm_mem_chunk_node *chunk_node){
    spin_lock(&spin_lock_chunk_list);
    chunk_node->next = cvm_chunk_list_head->next;
    cvm_chunk_list_head->next = chunk_node;
    spin_unlock(&spin_lock_chunk_list);
}

static struct cvm_mem_chunk_node* get_chunk(){
    spin_lock(&spin_lock_chunk_list);
    if(!cvm_chunk_list_head->next){
        spin_unlock(&spin_lock_chunk_list);
        return NULL;
        /* TODO: refill chunk */
    }
    struct cvm_mem_chunk_node *chunk_node = cvm_chunk_list_head->next;
    cvm_chunk_list_head->next = chunk_node->next;
    spin_unlock(&spin_lock_chunk_list);
    chunk_node->next=NULL;
    return chunk_node;
}

static void chunk2list(struct sbi_cvm *cvm, struct cvm_mem_chunk_node *chunk_node, unsigned long num){
    unsigned long i;
    chunk_node->next = cvm->used_chunk_list_head->next;
    cvm->used_chunk_list_head->next = chunk_node;
    
    for(i=0; i<512; i++){
        set_bitmap(*(chunk_node->chunk_infor->paddr_list + i));
        set_page_own_table(*(chunk_node->chunk_infor->paddr_list + i), &cvm->vmid->vmid);
        if(i>(512-num))
            put_free_page(cvm->free_mem_list_head, *(chunk_node->chunk_infor->paddr_list + i) , cvm);
    }
}

//confidential memory management
//We reset the content of pages in the function.
int get_free_page(struct list_head* free_mem, struct sbi_cvm *cvm, paddr_t *paddr){
    free_mem_t* page;
    spinlock_t lock;
    if(free_mem == cvm_metadata_list_head)
        lock = spin_lock_metadata_list;
    else if (free_mem == free_root_pt_list_head)
        lock = spin_lock_root_pt_list;
    else if(cvm && free_mem==cvm->free_mem_list_head)
        lock = cvm->free_mem_list_lock;
    else
        sbi_printf("Can't match free_mem and lock!");
    spin_lock(&lock);
    if(list_empty(free_mem) && free_mem==cvm->free_mem_list_head){
        if(free_mem==cvm->free_mem_list_head){
            /* Ideally only cvm->free_mem_list_head is empty. */
            struct cvm_mem_chunk_node *chunk_node = (struct cvm_mem_chunk_node *)get_chunk();
            if(chunk_node == NULL){
                spin_unlock(&lock);
                return TEE_NO_MEMORY;
            }
            chunk2list(cvm, chunk_node, 511);
        }else{
            sbi_printf("Unexpected list exhaustion.\n");
            return CVM_ERROR;
        }
    }
    page = list_first_entry(free_mem, free_mem_t, free_mem_list);
    *paddr = page->paddr;
    list_del(&page->free_mem_list);
    spin_unlock(&lock);
    sbi_memset((void *)*paddr, 0, PAGE_SIZE);
    return 0;
}

void put_free_page(struct list_head* free_mem, paddr_t paddr, struct sbi_cvm *cvm){
    spinlock_t lock;
    if(free_mem == cvm_metadata_list_head)
        lock = spin_lock_metadata_list;
    else if (free_mem == free_root_pt_list_head)
        lock = spin_lock_root_pt_list;
    else if(cvm && free_mem==cvm->free_mem_list_head)
        lock = cvm->free_mem_list_lock;
    else
        sbi_printf("Can't match free_mem and lock!");
    spin_lock(&lock);
    free_mem_t* page = (free_mem_t*)paddr;
    page->paddr = paddr;
    list_add_head(&page->free_mem_list, free_mem);
    spin_unlock(&lock);
}

//after we create cvm vcpu, we allocate a special page as the root_pt and allocate one chunk. 
int init_cvm_vcpu_rootptAndChunk(struct iie_cvm_sbi_params *cvm_sbi_params){
    //allocate root page table.
    paddr_t free_page;
    int ret=0;
    ret = get_free_page(free_root_pt_list_head, NULL, &free_page);
    if(ret)
        return ret;
    if(&cvm_sbi_params->vmid_ptr->vmid == NULL){
        sbi_printf("[IIE CVM DEBUG@%s] &cvm->vmid->vmid is NULL, hpa os 0x%lx.\n", __func__, free_page);
        return CVM_ERROR;
    }
    for(int i=0; i<4; i++){
        set_page_own_table(free_page+i*PAGE_SIZE, &cvm_sbi_params->vmid_ptr->vmid);
    }
    struct cvm_node *node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    
    node->cvm.root_pt = free_page;
    //allocate one chunk to the cvm.
    struct cvm_mem_chunk_node *chunk_node = (struct cvm_mem_chunk_node *)get_chunk();
    if(chunk_node == NULL){
        return TEE_NO_MEMORY;
    }
    // sbi_memset((void *)*(chunk_node->chunk_infor->paddr_list), 0, PAGE_SIZE);
    node->cvm.used_chunk_list_head = (struct cvm_mem_chunk_node *)(*(chunk_node->chunk_infor->paddr_list+1));
    node->cvm.used_chunk_list_head->next = NULL;
    node->cvm.free_mem_list_head = (struct list_head *)(*(chunk_node->chunk_infor->paddr_list+2));
    node->cvm.free_mem_list_head -> next = node->cvm.free_mem_list_head;
    //we already used first three pages, so chunk2list only have 509 pages.
    chunk2list(&node->cvm, chunk_node, 509);
    return 0;
}

static int find_pte(struct sbi_cvm* cvm, paddr_t gpa, pt_entry_t **pte){
    int ret=0;
    paddr_t free_page_pgd, free_page_pud, free_page_p4d, free_page_pmd;
    pt_entry_t *pgd, *pmd, *findpte, *pud, *p4d;
    paddr_t root_pt = cvm->root_pt;
    pgd = (pt_entry_t*)(root_pt) + pgd_index(gpa);
    if(!((*pgd) & PTE_V)){
        ret = get_free_page(cvm->free_mem_list_head, cvm, &free_page_pgd);   
        if(ret)
            return ret;
        *pgd = (free_page_pgd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pud = (pt_entry_t*)((*pgd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pud_index(gpa);
    if(!((*pud) & PTE_V)){
        ret = get_free_page(cvm->free_mem_list_head, cvm, &free_page_pud);
        if(ret)
            return ret;
        *pud = (free_page_pud >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    p4d = (pt_entry_t*)((*pud) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + p4d_index(gpa);
    if(!((*p4d) & PTE_V)){
        ret = get_free_page(cvm->free_mem_list_head, cvm, &free_page_p4d);
        if(ret)
            return ret;
        *p4d = (free_page_p4d >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pmd = (pt_entry_t*)((*p4d) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pmd_index(gpa);
    if(!((*pmd) & PTE_V)){
        ret = get_free_page(cvm->free_mem_list_head, cvm, &free_page_pmd);
        if(ret)
            return ret;
        *pmd = (free_page_pmd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    findpte = (pt_entry_t*)((*pmd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pte_index(gpa);
    if((*findpte) & PTE_V)
        return CVM_ERROR;
    
    *pte = findpte;
    return 0;
}

static int find_sw_pte(struct sbi_cvm* cvm, paddr_t gpa, pt_entry_t **pte){
    pt_entry_t *pgd, *pmd, *findpte, *pud, *p4d;
    paddr_t root_pt = cvm->root_pt;
    pgd = (pt_entry_t*)(root_pt) + pgd_index(gpa);
    if(!((*pgd) & PTE_V))
        return CVM_ERROR;

    pud = (pt_entry_t*)((*pgd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pud_index(gpa);
    if(!((*pud) & PTE_V))
        return CVM_ERROR;

    p4d = (pt_entry_t*)((*pud) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + p4d_index(gpa);
    if(!((*p4d) & PTE_V))
        return CVM_ERROR;

    pmd = (pt_entry_t*)((*p4d) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pmd_index(gpa);
    if(!((*pmd) & PTE_V))
        return CVM_ERROR;

    findpte = (pt_entry_t*)((*pmd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pte_index(gpa);
    if((*findpte) & PTE_V){
        *pte = findpte;
        return 0;
    }
    else
        return 1;
}

//build the mapping between GPA and HPA
int malloc_cvm_empty_page(struct sbi_cvm* cvm, vaddr_t gpa, paddr_t *paddr){
    int ret=0;
    paddr_t root_pt = cvm->root_pt;
    pt_entry_t *pte;
    ret = find_pte(cvm, gpa, &pte);
    if(ret)
        return ret;

    paddr_t free_page;
    ret = get_free_page(cvm->free_mem_list_head, cvm, &free_page);
    if(ret)
        return ret;

    uint64_t ppn = free_page >> PAGE_SHIFT;
    //set KeyID and prot
    uint64_t keyid_ppn = (cvm->KeyID << KEYID_OFFSET) | ppn;
    *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_X | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
    *paddr = (*pte) >> PAGE_PFN_SHIFT << PAGE_SHIFT;
    return 0;
}

//It is used to allocate a free page for cvm metadata, and its vmidp is &metadataPage.
int malloc_cvm_empty_page_only(paddr_t* vmidp, paddr_t *hpa){
    int ret=0;
    paddr_t free_page;
    ret = get_free_page(cvm_metadata_list_head, NULL, &free_page);
    if(ret){
        return ret;
    }
    set_page_own_table(free_page, vmidp);
    *hpa = free_page;
    return 0;
}

void mfree_cvm_page_only(paddr_t paddr, paddr_t* vmid_addr){
    reset_page_own_table(paddr, vmid_addr);
    put_free_page(cvm_metadata_list_head, paddr, NULL);
}

void mfree_cvm_root_pt(paddr_t paddr, paddr_t* vmid_addr){
    for(int i=0; i<4; i++){
        reset_page_own_table(paddr+i*PAGE_SIZE, vmid_addr);
        sbi_memset((void *)paddr, 0, PAGE_SIZE);
    }
    put_free_page(free_root_pt_list_head, paddr, NULL);
}

int recycle_one_chunk(struct cvm_mem_chunk_node *cursor, struct cvm_mem_chunk_node *chunk_node, paddr_t* vmidp, struct cvm_list_params *ret_sbi_params){
    int ret;
    unsigned long i;
    for(i=0; i<512; i++){
        ret = reset_page_own_table(*(chunk_node->chunk_infor->paddr_list+i), vmidp);
        if (ret)
        return ret;
    }
    if(chunk_node->chunk_infor->type == 0){
        cursor->next = chunk_node->next;
        put_chunk(chunk_node);
    }else{
        cursor->next = chunk_node->next;
        for(i=0;i<512;i++)
            reset_bitmap(*(chunk_node->chunk_infor->paddr_list+i));
        reset_bitmap((unsigned long)chunk_node->chunk_infor);
        *((unsigned long *)ret_sbi_params->addr + ret_sbi_params->ele_num) = (unsigned long)chunk_node->chunk_infor->chunk_infor_vaddr;
        ret_sbi_params->ele_num += 1;
        // mfree_cvm_page_only((unsigned long)chunk_node, &metadataPage);
    }
    return 0;
}

//reset the pageownership table, then recycle the used chunk.
int mfree_cvm_page(struct sbi_cvm *cvm, struct cvm_list_params *ret_sbi_params){
    int ret=0;
    paddr_t root_pt = cvm->root_pt;
    paddr_t* vmidp = &cvm->vmid->vmid;
    struct cvm_mem_chunk_node *chunk_node, *cursor;
    paddr_t pa;
    unsigned long i;
    //for swiotlb
    pt_entry_t *sw_pte;
    unsigned long swiotlb_addr = cvm->swiotlb_addr;
    unsigned long swiotlb_size = cvm->swiotlb_size;
    for(i=swiotlb_addr; i<swiotlb_addr+swiotlb_size; i++){
        ret = find_sw_pte(cvm, i, &sw_pte);
        if(ret == 0){
            reset_page_own_table((*sw_pte) >> PAGE_PFN_SHIFT << PAGE_SHIFT, vmidp);
            *sw_pte = 0UL;
        }else if(ret == 1)
            continue;
        else
            return ret;
    }

    ret_sbi_params->ele_num = 0;
    //The recycle process begins at the second used chunk.
    cvm->free_mem_list_head = NULL;
    cursor = cvm->used_chunk_list_head;
    if(cursor->next){
        //recycle cursor until tail.
        for(chunk_node=cursor->next; (chunk_node && cursor); chunk_node=cursor->next){
            ret = recycle_one_chunk(cursor, chunk_node, vmidp, ret_sbi_params);
            if(ret)
                return ret;
        }
    }
    mfree_cvm_root_pt(root_pt, vmidp);
    cvm->root_pt = 0UL;
    sbi_printf("recycle num of chunks is 0x%lx\n", ret_sbi_params->ele_num);
    i=0;
    cursor = cvm_chunk_list_head->next;
    while(cursor){
        i++;
        cursor = cursor->next;
    }
    sbi_printf("the num of chunks in cvm_chunk_list_head is 0x%lx\n", i);
    return 0;
}

//__riscv_xlen is 64 by default.
static void init_page_level(){
    unsigned long hgatp = csr_read(CSR_HGATP);
    sbi_printf("hgatp is %lx\n", hgatp);
    unsigned long hgatp_mode = (hgatp & SATP64_MODE) >> HGATP64_MODE_SHIFT;
    sbi_printf("satp mode is %lx\n", hgatp_mode);
    if(hgatp_mode == HGATP_MODE_OFF)
        PAGE_LEVEL = 0;
    else if(hgatp_mode == HGATP_MODE_SV39X4)
        PAGE_LEVEL = 3;
    else if(hgatp_mode == HGATP_MODE_SV48X4)
        PAGE_LEVEL = 4;
    else if(hgatp_mode == SATP_MODE_SV57)
        PAGE_LEVEL = 5;
    else
        sbi_printf("hgatp mode is not support.\n");
}

static int init_cvm_memorypool(struct cvm_list_params* chunk_infor_list){
    // sbi_printf("----begin init_cvm_memorypool ----\n");
    int ret=0;
    unsigned long i,j;
    //the first chunk is used for allocating metadata of cvm.
    struct cvm_mem_chunk_infor *first_chunk_infor = (struct cvm_mem_chunk_infor *)*((unsigned long *)chunk_infor_list->addr);
    set_bitmap((unsigned long)first_chunk_infor);
    for(i=0; i<512; i++){
        set_bitmap((unsigned long)*((unsigned long *)first_chunk_infor->paddr_list+i));
        if(i==0){
            //the first page of the first chunk is used for cvm_metadata_list_head.
            set_page_own_table((unsigned long)*(first_chunk_infor->paddr_list+i), &metadataPage);
            cvm_metadata_list_head = (struct list_head *)*((unsigned long *)first_chunk_infor->paddr_list);
            cvm_metadata_list_head->next = cvm_metadata_list_head;
        }
        else{
            put_free_page(cvm_metadata_list_head, (unsigned long)*((unsigned long *)first_chunk_infor->paddr_list+i), NULL);
        }
    }
    paddr_t free_page;
    ret = malloc_cvm_empty_page_only(&metadataPage, &free_page);
    if(ret)
        return ret;
    cvm_chunk_list_head = (struct cvm_mem_chunk_node *)free_page;
    cvm_chunk_list_head->next = NULL;
    //the remaining chunks.
    struct cvm_mem_chunk_infor *remaining_chunk_infor;
    for(i=1; i<chunk_infor_list->ele_num; i++){
        remaining_chunk_infor = (struct cvm_mem_chunk_infor *)*((unsigned long *)chunk_infor_list->addr+i);
        set_bitmap((unsigned long)remaining_chunk_infor);
        struct cvm_mem_chunk_node *chunk_node = (struct cvm_mem_chunk_node *)*(remaining_chunk_infor->paddr_list);
        chunk_node->chunk_infor = remaining_chunk_infor;
        put_chunk(chunk_node);
    }
    return 0;
    // sbi_printf("----end init_cvm_memorypool ----\n");
}

static int init_root_pt_list(struct cvm_list_params* root_pt_list){
    unsigned long i,j;
    int ret=0;
    for(i=0; i<root_pt_list->ele_num; i++){
        for(j=0;j<4;j++){
            sbi_memset((unsigned long)*((unsigned long*)root_pt_list->addr + i) + j*PAGE_SIZE, 0, PAGE_SIZE);
            set_bitmap(((unsigned long)*((unsigned long*)root_pt_list->addr + i)) + j*PAGE_SIZE);
        }
        if(i==0){
            paddr_t free_page;
            ret = malloc_cvm_empty_page_only(&metadataPage, &free_page);
            if(ret)
                return ret;
            free_root_pt_list_head = (struct list_head *)free_page;
            free_root_pt_list_head->next = free_root_pt_list_head;
        }
        put_free_page(free_root_pt_list_head, (unsigned long)*((unsigned long*)root_pt_list->addr + i), NULL);
    }
    return 0;
}

//initialize bitmap, page own page, confidential memory pool, root page table list.
int convert_cvm_pages(struct cvm_list_params* chunk_infor_list, struct cvm_list_params* root_pt_list, struct cvm_list_params* bmp, struct cvm_list_params* own_table){
    int ret=0;

    ret = init_bitmap(bmp);
    if(ret)
        return ret;

    ret = init_page_own_table(own_table);
    if(ret)
        return ret;

    /* TODO: init page level to support various page table. */
    //init_page_level();

    //init confidential memory pool
    ret = init_cvm_memorypool(chunk_infor_list);
    if(ret)
        return ret;

    //init root_pt_list
    ret = init_root_pt_list(root_pt_list);
    return ret;
}

//used for swiotlb, create shared memory between cvm and hypervisor.
int add_cvm_share_pages(struct sbi_cvm* cvm, paddr_t gpa, paddr_t hpa, bool swiotlb, unsigned long KeyID){
    int ret=0;
    paddr_t root_pt = cvm->root_pt;
    pt_entry_t *pte;
    if(hpa == 0){
        return CVM_ERROR;
    }
    //we don't set keyid, bitmap,  because the page is shared between host and guest;
    ret = find_pte(cvm, gpa, &pte);
    if(ret)
        return ret;

    uint64_t ppn = hpa >> PAGE_SHIFT;

    //TODO: KeyID of swiotlb pages should be hypervisor's KeyID? Is it 0?
    uint64_t keyid_ppn = ppn;
    *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_X | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
    if(&cvm->vmid->vmid == NULL){
        sbi_printf("[IIE CVM DEBUG@%s] &cvm->vmid->vmid is NULL, hpa os 0x%lx.\n", __func__, hpa);
    }
    set_page_own_table(hpa, &cvm->vmid->vmid);
    
    return 0;
}

int load_file(struct iie_cvm_sbi_params_load *load_file){
    int ret=0;
    struct cvm_node *cvm_node = get_cvm(load_file->vmid_ptr->vmid);
    if(!cvm_node){
        sbi_printf("function %s get_cvm by id 0x%lx failed!\n", __func__, load_file->vmid_ptr->vmid);
        return CVM_ERROR;
    }
    unsigned long count = load_file->count;
    //paddr_t root_pt = cvm_node->cvm.root_pt;
    //sbi_printf("root_pt is %lx\n", root_pt);
    vaddr_t *src = load_file->src_hpa_array;
    paddr_t des = load_file->des_gpa;
    int i;
    paddr_t des_cm_hpa;
    for(i=0;i<count;i++){
        ret = malloc_cvm_empty_page(&cvm_node->cvm, des+i*PAGE_SIZE, &des_cm_hpa);
        if(ret == 0)
            sbi_memcpy((void *)des_cm_hpa, (const void *)*(src+i), PAGE_SIZE); 
        else
            return ret;
    }

    return 0;
}

int retry_load_after_refill(struct iie_cvm_sbi_params_load *load_file){
    // sbi_printf("retry_load_after_refill here\n");
    int ret=0;
    struct cvm_node *cvm_node = get_cvm(load_file->vmid_ptr->vmid);
    if(!cvm_node){
        sbi_printf("function %s get_cvm by id 0x%lx failed!\n", __func__, load_file->vmid_ptr->vmid);
        return CVM_ERROR;
    }
    unsigned long count = load_file->count;
    vaddr_t *src = load_file->src_hpa_array;
    paddr_t des = load_file->des_gpa;
    int i;
    paddr_t des_cm_hpa;
    pt_entry_t *pte;
    for(i=0;i<count;i++){
        ret = find_pte(&cvm_node->cvm, des+i*PAGE_SIZE, &pte);
        //because we retrys to load the file, wo don't deal ret is CVM_ERROR.
        if(ret == 0){
            int r=0;
            r = malloc_cvm_empty_page(&cvm_node->cvm, des+i*PAGE_SIZE, &des_cm_hpa);
            if(r == 0)
                sbi_memcpy((void *)des_cm_hpa, (const void *)*(src+i), PAGE_SIZE); 
            else
                return r;
        }else
            return ret;
    }
    // sbi_printf("retry_load_after_refill end\n");
    return 0;
}

int init_swiotlb_params(struct iie_cvm_sbi_params_swiotlb *swiotlb, struct kvm_vmid *vmid_ptr){
    if(!vmid_ptr){
        return CVM_ERROR;
    }
    if(!swiotlb){
        return CVM_ERROR;
    }
    struct cvm_node *cvm_node = get_cvm(vmid_ptr->vmid);
    if(!cvm_node){
        sbi_printf("function %s get_cvm by id 0x%lx failed!\n", __func__, *vmid_ptr);
        return CVM_ERROR;
    }
    cvm_node->cvm.swiotlb_addr = swiotlb->addr;
    cvm_node->cvm.swiotlb_size = swiotlb->size;
    return 0;
}

int refill_memory_pool(struct cvm_list_params *chunk_infor_list){
    // sbi_printf("refill_memory_pool begin\n");
    unsigned long i;
    // sbi_printf("chunk_infor_list->ele_num is %ld\n", chunk_infor_list->ele_num);
    //the remaining chunks.
    struct cvm_mem_chunk_infor *chunk_infor;
    for(i=0; i<chunk_infor_list->ele_num; i++){
        chunk_infor = (struct cvm_mem_chunk_infor *)*((unsigned long *)chunk_infor_list->addr+i);
        set_bitmap((unsigned long)chunk_infor);
        struct cvm_mem_chunk_node *chunk_node = (struct cvm_mem_chunk_node *)*(chunk_infor->paddr_list);
        chunk_node->chunk_infor = chunk_infor;
        put_chunk(chunk_node);
    }
    // sbi_printf("refill_memory_pool end\n");
    return 0;
}

int recycle_memory(struct iie_cvm_sbi_params *cvm_sbi_params, struct cvm_list_params *recycle_list){
    struct kvm_vmid *vmid_ptr = cvm_sbi_params->vmid_ptr;
	int *vcpu_id_ptr = cvm_sbi_params->vcpu_id_ptr;
	struct cvm_node *cvm_node = get_cvm(vmid_ptr->vmid);
    if(!cvm_node)
    {
        sbi_printf("[IIE CVM Monitor@%s] CVM %ld does not exist. \r\n", __func__, vmid_ptr->vmid);
        return CVM_ERROR;
    }
    mfree_cvm_page(&cvm_node->cvm, recycle_list);
    return 0;
}

/*-------------------------------CVM trap handler----------------------------------------*/
int cvm_trap_redirect_to_hs(struct sbi_trap_regs* host_regs)
{
    // 传递trap 同步上下文
    return cvm_vcpu_exit(host_regs);
}

int cvm_trap_virtual_inst(struct sbi_trap_regs* host_regs)
{
    // TODO 读取虚拟机中指令并保存到csr mtval, 目前没看到需要读CVM中指令的需求
    // 传递trap 同步上下文
    return cvm_vcpu_exit(host_regs);
}


int cvm_trap_gstage_page_fault(struct sbi_trap_regs* host_regs)
{
    // TODO 处理CVM 内存的PF, 分配内存页, 标记bitmap, 创建页表
    int ret=0;
    unsigned long addr = (csr_read(CSR_MTVAL2) << 2) | (csr_read(CSR_MTVAL) & 0x3);
	struct sbi_trap_info uptrap;
    ulong insn;
    unsigned long swiotlb_addr, swiotlb_size;
    unsigned long vmid = get_cvm_id();
    struct cvm_node *cvm_node = get_cvm(vmid);
    if(!cvm_node){
        return CVM_ERROR;
    }
    swiotlb_addr = cvm_node->cvm.swiotlb_addr;
    swiotlb_size = cvm_node->cvm.swiotlb_size;
    // sbi_printf("addr gpa: %lx mtval %lx mtval2 %lx mtinst %lx mepc %lx\r\n", addr, csr_read(CSR_MTVAL), csr_read(CSR_MTVAL2), csr_read(CSR_MTINST), csr_read(CSR_MEPC));
    // sbi_printf("regs t0 %lx a1 %lx\r\n", host_regs->t0, host_regs->a1);
    if(!addr){
        return CVM_ERROR;
    }
    else if(addr < 0x80000000){
        cvm_vcpu_exit(host_regs);
        return ret;
    }
    else if(addr >= swiotlb_addr && addr < swiotlb_addr + swiotlb_size){
        uint32_t vmid = get_cvm_id();
        uint32_t vcpuid = get_cvm_vcpu_id();
        struct cvm_vcpu_node *vcpu = get_cvm_vcpu_node(vmid, vcpuid);
        vcpu->vcpu.exit_reason = SWIOTLB;
        cvm_vcpu_exit(host_regs);
        return ret;
    }
    else{
        struct kvm_vmid kvm_id;
        paddr_t hpa;
        kvm_id.vmid = get_cvm_id();
        struct cvm_node *cvm = get_cvm(kvm_id.vmid);
        ret = malloc_cvm_empty_page(&cvm->cvm, addr, &hpa);
        // TODO resume cvm ctx
        if(ret == TEE_NO_MEMORY){
            cvm_vcpu_exit(host_regs);
            host_regs->a0 = TEE_NO_MEMORY;
            return 0;
        }else 
            return ret;
    }
    // 传递trap 同步上下文
    // sbi_printf("CSR_MTINST %lx\r\n", csr_read(CSR_MTINST));
    // insn = sbi_get_insn(host_regs->mepc, &uptrap);
    // sbi_printf("insn %lx\r\n", insn);
    // if (uptrap.cause)
    //     return sbi_trap_redirect(host_regs, &uptrap);
    // if ((insn & 3) != 3)
    // {
    //     struct sbi_trap_info trap;

    //     trap.cause = CAUSE_ILLEGAL_INSTRUCTION;
    //     trap.tval = insn;
    //     trap.tval2 = 0;
    //     trap.tinst = 0;
    //     trap.gva   = 0;

    //     return sbi_trap_redirect(host_regs, &trap);
    // }
    // csr_write(CSR_HTINST, insn);
    // sbi_printf("CSR_HTINST %lx\r\n", csr_read(CSR_HTINST));
}

int cvm_trap_sbi_ecall(struct sbi_trap_regs* host_regs)
{
    // 传递trap 同步上下文
    return cvm_vcpu_exit(host_regs);
}




