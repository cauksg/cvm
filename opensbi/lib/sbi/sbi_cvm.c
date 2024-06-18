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


spinlock_t spin_lock_cm_list;
spinlock_t spin_lock_bitmap;
spinlock_t spin_lock_page_own_table;

struct list_head* free_mem_list_head;
//page_own_table_t *page_own_table = (page_own_table_t*)sbi_calloc(1, 4);


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

struct cvm_node *iie_cvm_list_head = NULL;
// cpu_state_t cpus[MAX_HARTS];

// #define LOCK_DEBUG   0
static spinlock_t cvm_metadata_lock = SPIN_LOCK_INITIALIZER;


/* TODO: memory allocated should be in confidential memory region */
static struct cvm_node* alloc_cvm_node(unsigned long vmid)
{
	return (struct cvm_node *)malloc_cvm_empty_page_only(vmid);
}

/* TODO: memory allocated should be in confidential memory region */
static struct cvm_vcpu_node* alloc_cvm_vcpu_node(unsigned long vmid)
{
	return (struct cvm_vcpu_node *)malloc_cvm_empty_page_only(vmid);
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
            mfree_cvm_page_only(node, node->cvm.vmid->vmid);
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
        mfree_cvm_page_only(node, node->vcpu.cvm->vmid->vmid);
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

	// acquire_big_metadata_lock(__func__);

	for(struct cvm_vcpu_node *cur = vcpu_list_head; cur->next; cur = cur->next)
	{
		struct cvm_vcpu_node *node = cur->next;
        // sbi_printf("[IIE CVM Monitor@%s] node vcpu id = %d\tvcpu id = %d\n", __func__, *node->vcpu.vcpu_id, vcpu_id);
		if(*node->vcpu.vcpu_id == vcpu_id)
			return node;
	}

    // release_big_metadata_lock(__func__);
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
    sbi_memcpy(&cvm_vcpu->guest_context, kvm_guest_context, sizeof(struct cpu_context));
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
    sbi_memcpy(kvm_vcpu_context, &cvm_vcpu->guest_context, sizeof(struct cpu_context));
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
	sbi_printf("[IIE CVM Monitor@%s] vcpu_id = %d\n", __func__,*cvm_sbi_params->vcpu_id_ptr);
	//sbi_printf("[IIE CVM Monitor@%s] pgd_ptr = %lx\n", __func__,cvm_sbi_params->pgd_ptr);
	//sbi_printf("[IIE CVM Monitor@%s] pgd_phys_ptr = %lx\n", __func__,cvm_sbi_params->pgd_phys_ptr);
	//sbi_printf("[IIE CVM Monitor@%s] pgd = %lx\n", __func__,*cvm_sbi_params->pgd_ptr);
	//sbi_printf("[IIE CVM Monitor@%s] pgd_phys = %lx\n", __func__,*cvm_sbi_params->pgd_phys_ptr);

    

	/* alloc a memory block for cvm struct */
	struct cvm_node* cvm_node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    if(!cvm_node)
    {
        cvm_node = alloc_cvm_node(cvm_sbi_params->vmid_ptr->vmid);
        sbi_printf("[IIE CVM Monitor@%s] cvm allocating... \r\n", __func__);
    }
    else return 0;
    
	if(!cvm_node)
	{
		sbi_printf("[IIE CVM Monitor@%s] cvm allocation is failed. \r\n", __func__);
		return TEE_NO_MEMORY;
	}
	sbi_printf("[IIE CVM Monitor@%s] cvm allocation is successfull. \r\n", __func__);
    


	/* Initialize cvm structure*/
	// sbi_printf("[IIE CVM Monitor@%s] CVM initializing. \r\n", __func__);
	cvm_node->cvm.vmid = cvm_sbi_params->vmid_ptr;
	cvm_node->cvm.KeyID = get_keyID(cvm_node->cvm.vmid);
	cvm_node->cvm.state = INITIALIZING;
	cvm_node->cvm.cvm_vcpu_list_head = alloc_cvm_vcpu_node(cvm_sbi_params->vmid_ptr->vmid);
	cvm_node->cvm.cvm_vcpu_list_head->next = NULL;
	cvm_node->cvm.cmode = 1;
    cvm_node->cvm.KeyID = gen_key_id();
    sbi_memset(cvm_node->cvm.hash, 0, sizeof(cvm_node->cvm.hash));
    
    cvm_node->cvm.pgd_phys = *cvm_sbi_params->pgd_phys_ptr;
	// sbi_printf("[IIE CVM Monitor@%s] pgd_phys_ptr = %lx, pgd_phys = %lx. \r\n", 
    //     __func__, cvm_sbi_params->pgd_phys_ptr, cvm_node->cvm.pgd_phys);
    // hash_cvm(&cvm_node->cvm, (void *)cvm_node->cvm.hash, 0);
    // sbi_printf("[IIE CVM Monitor@%s] hash value = %s. \r\n", __func__, cvm_node->cvm.hash);

	cvm_node->next = NULL;

#ifdef PROG_LBL
    init_cvm_vcpu_root_pt(&cvm_node->cvm);
#endif

	/* maintain the cvm List */
	// sbi_printf("[IIE CVM Monitor@%s] CVM List. \r\n", __func__);
	if(iie_cvm_list_head == NULL)
	{
		/* alloc memory first */
		iie_cvm_list_head = alloc_cvm_node(cvm_sbi_params->vmid_ptr->vmid);
        if(iie_cvm_list_head == NULL)
        {
            sbi_printf("[IIE CVM Monitor@%s] cvm list_head allocation is failed. \r\n", __func__);
		    return TEE_NO_MEMORY;
        }
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
        vcpu_node = alloc_cvm_vcpu_node(cvm_sbi_params->vmid_ptr->vmid);
        sbi_printf("[IIE CVM Monitor@%s] CVM %d vcpu allocating... \r\n", __func__, vmid_ptr->vmid);
    }
    else return 0;
    
	if(!vcpu_node)
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
        ret = add_cvm_share_pages(cvm->root_pt, gpa, hpa);
        if(ret == 1 || ret == 2){
            sbi_printf("[IIE CVM Monitor@%s] SWIOTLB PF failed!\n", __func__);
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
    cvm_node->cvm.pgd_phys = *cvm_sbi_params->pgd_phys_ptr;
	// sbi_printf("[IIE CVM Monitor@%s] pgd_phys_ptr = %lx, pgd_phys = %lx. \r\n", 
    //     __func__, cvm_sbi_params->pgd_phys_ptr, cvm_node->cvm.pgd_phys);
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
//we reset the content of pages before allocate it!
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
    sbi_memset((void *)paddr, 0, sizeof(free_mem_t));
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

//when we create cvm vcpu, we allocate a free cm page as it's root_pt. 
int init_cvm_vcpu_root_pt(struct iie_cvm_sbi_params *cvm_sbi_params){
    spin_lock(&spin_lock_cm_list);
    paddr_t free_page = get_free_mem(free_mem_list_head);
    spin_unlock(&spin_lock_cm_list);
    if(free_page == 0){
        sbi_printf("no empty confidential memory\n");
        return 1;
    }
    set_page_own_table(free_page, cvm_sbi_params->vmid_ptr->vmid);
    struct cvm_node *node = get_cvm(cvm_sbi_params->vmid_ptr->vmid);
    node->cvm.root_pt = free_page;
    sbi_printf("vm %lx root_pt address is 0x%lx\n",cvm_sbi_params->vmid_ptr->vmid, free_page);
    return 0;
}


//only allocate a free cm page for various cvm struct without any GPA.
paddr_t malloc_cvm_empty_page_only(unsigned long vmid){
    spin_lock(&spin_lock_cm_list);
    paddr_t free_page = get_free_mem(free_mem_list_head);
    spin_unlock(&spin_lock_cm_list);
    if(free_page == 0){
        sbi_printf("no empty confidential memory\n");
        return 0;
    }
    set_page_own_table(free_page, vmid);
    return free_page;
}

static paddr_t find_pte(paddr_t root_pt, paddr_t gpa){
    pt_entry_t *pgd, *pmd, *pte, *pud, *p4d;
    pgd = (pt_entry_t*)(root_pt) + pgd_index(gpa);
    if(!((*pgd) & PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_pgd = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_pgd == 0){
            sbi_printf("no empty confidential memory\n");
            return 1;
        }
        *pgd = (free_page_pgd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pud = (pt_entry_t*)((*pgd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pud_index(gpa);
    if(!((*pud) & PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_pud = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_pud == 0){
            sbi_printf("no empty confidential memory\n");
            return 1;
        }
        *pud = (free_page_pud >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    p4d = (pt_entry_t*)((*pud) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + p4d_index(gpa);
    if(!((*p4d) & PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_p4d = get_free_mem(free_mem_list_head);
        //sbi_printf("free_page_pgd is %lx\n",free_page_p4d);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_p4d == 0){
            sbi_printf("no empty confidential memory\n");
            return 1;
        }
        *p4d = (free_page_p4d >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pmd = (pt_entry_t*)((*p4d) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pmd_index(gpa);
    if(!((*pmd) & PTE_V)){
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page_pmd = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page_pmd == 0){
            sbi_printf("no empty confidential memory\n");
            return 1;
        }
        *pmd = (free_page_pmd >> PAGE_SHIFT << PAGE_PFN_SHIFT) | PTE_V;
    }
    pte = (pt_entry_t*)((*pmd) >> PAGE_PFN_SHIFT << PAGE_SHIFT) + pte_index(gpa);
    if((*pte) & PTE_V){
        sbi_printf("[IIE CVM DEBUG@%s] address 0x%lx already has physical page 0x%lx\n",  __func__, gpa, (*pte>>PAGE_PFN_SHIFT));
    }
    return (unsigned long)pte;
}


//build the mapping between GPA and HPA
paddr_t malloc_cvm_empty_page(struct sbi_cvm* cvm, vaddr_t gpa){
    paddr_t root_pt = cvm->root_pt;
    paddr_t pte = find_pte(root_pt, gpa);
    if(pte == 1 || pte == 2){
        sbi_printf("[IIE CVM DEBUG@%s] find pte fault.", __func__);
        return pte;
    }else{
        spin_lock(&spin_lock_cm_list);
        paddr_t free_page = get_free_mem(free_mem_list_head);
        spin_unlock(&spin_lock_cm_list);
        if(free_page == 0){
            sbi_printf("no empty confidential memory\n");
            return 1;
        }
        //set page own table
        set_page_own_table(free_page, cvm->vmid->vmid);
        uint64_t ppn = free_page >> PAGE_SHIFT;
        //set KeyID and prot
        uint64_t keyid_ppn = (cvm->KeyID << KEYID_OFFSET) | ppn;
        *(pt_entry_t *)pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_X | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
    }
    return (unsigned long)*(pt_entry_t *)pte;
}


//remove the mapping between GPA and HPA, then put HPA into cm free page list.
// int mfree_cvm_page(struct sbi_cvm* cvm, struct iie_cvm_sbi_params *cvm_sbi_params){
//     paddr_t root_pt = cvm->root_pt;
//     vaddr_t gpa = cvm_sbi_params->gpa;
//     pt_entry_t *pgd, *pmd, *pte;
//     pgd = (pt_entry_t*)(root_pt) + pgd_index(gpa);
//     if(!(*pgd | PTE_V)){
//         sbi_printf("Invalid pgd\n");
//         return 0;
//     }
//     pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(gpa);
//     if(!(*pmd | PTE_V)){
//         sbi_printf("Invalid pmd\n");
//         return 0;
//     }
//     pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(gpa);
//     if(!(*pte | PTE_V)){
//         sbi_printf("Invalid pte\n");
//         return 0;
//     }
//     //invalidate pte
//     *pte = (*pte) & (~PTE_V);
//     //mask keyid and prot
//     uint64_t keyid_ppn = (*pte) >> PAGE_PFN_SHIFT;
//     uint64_t ppn = keyid_ppn & (~KEYID_MASK);
//     paddr_t paddr = ppn << PAGE_SHIFT;
//     //reset page own table
//     reset_page_own_table(paddr, cvm->vmid->vmid);
//     //reclaim
//     spin_lock(&spin_lock_cm_list);
//     put_free_page(free_mem_list_head, paddr);
//     spin_unlock(&spin_lock_cm_list);
//     return 1;
// }

int mfree_cvm_page_only(paddr_t paddr, unsigned long vmid){
    put_free_page(free_mem_list_head, paddr);
    if(!reset_page_own_table(paddr, vmid))
        sbi_printf("[IIE CVM DEBUG@%s] id mismatch when resest page own table \n", __func__);
    return 0;
}

//remove the mapping between GPA and HPA, then reclaim HPA and allocate it for normal world.
// paddr_t mreclaim_cvm_page(struct sbi_cvm* cvm, struct iie_cvm_sbi_params *cvm_sbi_params){
//     paddr_t root_pt = cvm->root_pt;
//     vaddr_t gpa = cvm_sbi_params->gpa;
//     //gpa->hpa
//     pt_entry_t *pgd, *pmd, *pte;
//     pgd = (pt_entry_t*)(root_pt) + pgd_index(gpa);
//     if(!(*pgd | PTE_V)){
//         sbi_printf("Invalid pgd\n");
//         return 0;
//     }
//     pmd = (pt_entry_t*)(*pgd >> PAGE_PFN_SHIFT) + pmd_index(gpa);
//     if(!(*pmd | PTE_V)){
//         sbi_printf("Invalid pmd\n");
//         return 0;
//     }
//     pte = (pt_entry_t*)(*pmd >> PAGE_PFN_SHIFT) + pte_index(gpa);
//     if(!(*pte | PTE_V)){
//         sbi_printf("Invalid pte\n");
//         return 0;
//     }
//     //invalidate pte
//     *pte = (*pte) & (~PTE_V);
//     //return paddr_t
//     uint64_t keyid_ppn = (*pte) >> PAGE_PFN_SHIFT;
//     uint64_t ppn = keyid_ppn & (~KEYID_MASK);
//     paddr_t paddr = ppn << PAGE_SHIFT;
//     reset_page_own_table(paddr, cvm->vmid->vmid);
//     reset_bitmap(paddr);
//     return paddr;
// }

//vaddr_t normal_gpa[count] = {vaddr1, vaddr2 ,...}
//paddr_t narmal_hpa[count] = {paddr1, paddr2 ,...}
int add_cvm_share_pages(paddr_t root_pt, paddr_t gpa, paddr_t hpa){
    //struct cvm_node *cvm = get_cvm(cvm_sbi_params_shared->vmid_ptr->vmid);
    pt_entry_t *pte;
    //unsigned long gpa = cvm_sbi_params_shared->gpa;
    //paddr_t root_pt = cvm->cvm.root_pt;
    if(hpa <= 0){
        //sbi_printf("[IIE CVM DEBUG@%s] shared page addr error.\n", __func__);
        return 2;
    }
    //we don't set keyid, bitmap, page own table because the page is shared between host and guest;
    //sbi_printf("[IIE CVM DEBUG@%s] root pt is 0x%lx.\n", __func__, root_pt);
    pte = (pt_entry_t *)find_pte(root_pt, gpa);
    if(pte == 1 || pte == 2){
        //sbi_printf("[IIE CVM DEBUG@%s] find pte fault.\n", __func__);
        return pte;
    }
    uint64_t ppn = hpa >> PAGE_SHIFT;
    uint64_t keyid_ppn = ppn;
    *pte = (keyid_ppn << PAGE_PFN_SHIFT) | PTE_X | PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
    // set_bitmap(*(hpa+i));
    // set_page_own_table(*(hpa+i), cvm->vmid->vmid);
    //pte = (pt_entry_t *)find_pte(root_pt, gpa);
    //sbi_printf("[IIE CVM DEBUG@%s] gpa is 0x%lx, hpa is 0x%lx.\n", __func__, gpa, *pte >> PAGE_PFN_SHIFT);
    //sbi_printf("\n");
    return 0;
}


//paddr_t narmal_address[count] = {paddr1, paddr2 ,...}
int convert_cvm_pages(paddr_t* normal_address, int count){
    //sbi_printf("--------------------sbi convert_cvm_pages begin!-------------------------\n");
    if(count<=0){
        return 0;
    }
    free_mem_list_head = (struct list_head *)*(normal_address);
    free_mem_list_head->next = free_mem_list_head;
    int i;
    for(i=1; i<count; i++){
        //sbi_printf("sbi function convert_cvm_pages accepted physical address %lx\n", *(normal_address+i));
        set_bitmap((unsigned long)*(normal_address+i));
        spin_lock(&spin_lock_cm_list);

        //convert normal_address to CM pool rather than CVM memory 
        put_free_page(free_mem_list_head, (unsigned long)*(normal_address+i));
        spin_unlock(&spin_lock_cm_list);
    }
    return 0;
}


int load_file(struct iie_cvm_sbi_params_load *load_file){
    sbi_printf("--------------------sbi load file begin!-------------------------\n");
    struct cvm_node *cvm_node = get_cvm(load_file->vmid_ptr->vmid);
    unsigned long count = load_file->count;
    //paddr_t root_pt = cvm_node->cvm.root_pt;
    //sbi_printf("root_pt is %lx\n", root_pt);
    vaddr_t *src = load_file->src_hpa_array;
    paddr_t des = load_file->des_gpa;
    int i;
    paddr_t des_cm_hpa;
    for(i=0;i<count;i++){
        des_cm_hpa = malloc_cvm_empty_page(&cvm_node->cvm, des+i*PAGE_SIZE) >> PAGE_PFN_SHIFT << PAGE_SHIFT;
        if(des_cm_hpa != 1 && des_cm_hpa != 2)
            sbi_memcpy((void *)des_cm_hpa, (const void *)*(src+i), PAGE_SIZE);
        else if(des_cm_hpa == 1){
            sbi_printf("no empty confidential memory when sbi load file!\n");
            return 1;
        }
        else if(des_cm_hpa == 2){
            sbi_printf("des physical page already alloc when sbi load file!\n");
            return 1;
        }
    }
    sbi_printf("--------------------sbi load file end!-------------------------\n");
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

unsigned long i=0;

int cvm_trap_gstage_page_fault(struct sbi_trap_regs* host_regs)
{
    // TODO 处理CVM 内存的PF, 分配内存页, 标记bitmap, 创建页表
    int ret=0;
    unsigned long addr = (csr_read(CSR_MTVAL2) << 2) | (csr_read(CSR_MTVAL) & 0x3);
	struct sbi_trap_info uptrap;
    ulong insn;
    
    // sbi_printf("addr gpa: %lx mtval %lx mtval2 %lx mtinst %lx mepc %lx\r\n", addr, csr_read(CSR_MTVAL), csr_read(CSR_MTVAL2), csr_read(CSR_MTINST), csr_read(CSR_MEPC));
    // sbi_printf("regs t0 %lx a1 %lx\r\n", host_regs->t0, host_regs->a1);
//TODO
//if addr< 0x8000000 || addr in swiotlb
// set exit reason;
// return cvm_exit()；
//else
// malloc——cvm
    if(!addr){
        return CVM_ERROR;
    }
    else if(addr < 0x80000000){
        cvm_vcpu_exit(host_regs);
        return ret;
    }
    else if(addr >= SWIOTLB_ADDR && addr < SWIOTLB_ADDR + SWIOTLB_SIZE){
        //sbi_printf("[IIE CVM DEBUG@%s] 0x%lxth gpa is 0x%lx\n", __func__, i, addr);
        i++;
        uint32_t vmid = get_cvm_id();
        uint32_t vcpuid = get_cvm_vcpu_id();
        struct cvm_vcpu_node *vcpu = get_cvm_vcpu_node(vmid, vcpuid);
        vcpu->vcpu.exit_reason = SWIOTLB;
        //sbi_printf("-------------swiotlb-pf, addr is 0x%lx------\n", addr);
        cvm_vcpu_exit(host_regs);
        return ret;
    }
    else{
        struct kvm_vmid kvm_id;
        kvm_id.vmid = get_cvm_id();
        struct cvm_node *cvm = get_cvm(kvm_id.vmid);
        ret = malloc_cvm_empty_page(&cvm->cvm, addr);
        // TODO resume cvm ctx
        if(ret != 1 && ret != 2)
            return 0;
        else
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




