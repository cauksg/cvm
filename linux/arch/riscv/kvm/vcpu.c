// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *     Anup Patel <anup.patel@wdc.com>
 */

#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/iommu.h>
#include <linux/kdebug.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/percpu.h>
#include <linux/vmalloc.h>
#include <linux/sched/signal.h>
#include <linux/fs.h>
#include <linux/kvm_host.h>
#include <asm/cacheflush.h>
#include <asm/kvm_gstage.h>
#include <asm/kvm_mmu.h>
#include <asm/kvm_nacl.h>
#include <asm/kvm_vcpu_vector.h>
#include <cvm/iie-cvm-sbi.h>

#define CREATE_TRACE_POINTS
#include "trace.h"

const struct kvm_stats_desc kvm_vcpu_stats_desc[] = {
	KVM_GENERIC_VCPU_STATS(),
	STATS_DESC_COUNTER(VCPU, ecall_exit_stat),
	STATS_DESC_COUNTER(VCPU, wfi_exit_stat),
	STATS_DESC_COUNTER(VCPU, wrs_exit_stat),
	STATS_DESC_COUNTER(VCPU, mmio_exit_user),
	STATS_DESC_COUNTER(VCPU, mmio_exit_kernel),
	STATS_DESC_COUNTER(VCPU, csr_exit_user),
	STATS_DESC_COUNTER(VCPU, csr_exit_kernel),
	STATS_DESC_COUNTER(VCPU, signal_exits),
	STATS_DESC_COUNTER(VCPU, exits),
	STATS_DESC_COUNTER(VCPU, instr_illegal_exits),
	STATS_DESC_COUNTER(VCPU, load_misaligned_exits),
	STATS_DESC_COUNTER(VCPU, store_misaligned_exits),
	STATS_DESC_COUNTER(VCPU, load_access_exits),
	STATS_DESC_COUNTER(VCPU, store_access_exits),
};

const struct kvm_stats_header kvm_vcpu_stats_header = {
	.name_size = KVM_STATS_NAME_SIZE,
	.num_desc = ARRAY_SIZE(kvm_vcpu_stats_desc),
	.id_offset = sizeof(struct kvm_stats_header),
	.desc_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE,
	.data_offset = sizeof(struct kvm_stats_header) + KVM_STATS_NAME_SIZE +
		       sizeof(kvm_vcpu_stats_desc),
};

static struct swiotlb_node *sw_list_head;
static DEFINE_SPINLOCK(sw_list);

static u64 kvm_cove_io_device_type(u64 device_id);
static bool kvm_riscv_cove_io_validate_pci_group(u64 device_id,
						 u64 expected_group);

static bool kvm_riscv_find_swiotlb(struct kvm *kvm, unsigned long gpa,
				   struct swiotlb *sw)
{
	struct swiotlb_node *cur;
	bool found = false;

	spin_lock(&sw_list);
	for (cur = sw_list_head; cur; cur = cur->next) {
		if (cur->vmid == &kvm->arch.vmid.vmid &&
		    gpa >= cur->sw.addr &&
		    gpa - cur->sw.addr < cur->sw.size) {
			*sw = cur->sw;
			found = true;
			break;
		}
	}
	spin_unlock(&sw_list);

	return found;
}

static bool kvm_riscv_find_swiotlb_by_pci_rid(u16 rid, struct kvm **kvm,
					      u64 *device_id)
{
	struct swiotlb_node *cur;
	bool found = false;

	spin_lock(&sw_list);
	for (cur = sw_list_head; cur; cur = cur->next) {
		if (kvm_cove_io_device_type(cur->sw.device_id) !=
		    KVM_COVE_IO_DEVICE_TYPE_PCI_RID)
			continue;
		if ((cur->sw.device_id & 0xffff) != rid)
			continue;
		if (!cur->kvm || !kvm_get_kvm_safe(cur->kvm))
			break;

		*kvm = cur->kvm;
		*device_id = cur->sw.device_id;
		found = true;
		break;
	}
	spin_unlock(&sw_list);

	return found;
}

static unsigned long kvm_riscv_gstage_gpa_to_hpa(struct kvm *kvm,
						 unsigned long gpa)
{
	struct kvm_gstage gstage = {
		.kvm = kvm,
		.flags = 0,
		.vmid = READ_ONCE(kvm->arch.vmid.vmid),
		.pgd = kvm->arch.pgd,
	};
	pte_t *ptep;
	u32 ptep_level;

	if (!kvm_riscv_gstage_get_leaf(&gstage, gpa, &ptep, &ptep_level))
		return 0;

	return (pte_pfn(ptep_get(ptep)) << PAGE_SHIFT) | (gpa & ~PAGE_MASK);
}

static void kvm_riscv_vcpu_context_reset(struct kvm_vcpu *vcpu,
					 bool kvm_sbi_reset)
{
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;
	struct kvm_cpu_context *cntx = &vcpu->arch.guest_context;
	void *vector_datap = cntx->vector.datap;

	memset(cntx, 0, sizeof(*cntx));
	memset(csr, 0, sizeof(*csr));
	memset(&vcpu->arch.smstateen_csr, 0, sizeof(vcpu->arch.smstateen_csr));

	/* Restore datap as it's not a part of the guest context. */
	cntx->vector.datap = vector_datap;

	if (kvm_sbi_reset)
		kvm_riscv_vcpu_sbi_load_reset_state(vcpu);

	/* Setup reset state of shadow SSTATUS and HSTATUS CSRs */
	cntx->sstatus = SR_SPP | SR_SPIE;

	cntx->hstatus |= HSTATUS_VTW;
	cntx->hstatus |= HSTATUS_SPVP;
	cntx->hstatus |= HSTATUS_SPV;
}

static void kvm_riscv_reset_vcpu(struct kvm_vcpu *vcpu, bool kvm_sbi_reset)
{
	bool loaded;

	/**
	 * The preemption should be disabled here because it races with
	 * kvm_sched_out/kvm_sched_in(called from preempt notifiers) which
	 * also calls vcpu_load/put.
	 */
	get_cpu();
	loaded = (vcpu->cpu != -1);
	if (loaded)
		kvm_arch_vcpu_put(vcpu);

	vcpu->arch.last_exit_cpu = -1;

	kvm_riscv_vcpu_context_reset(vcpu, kvm_sbi_reset);

	kvm_riscv_vcpu_fp_reset(vcpu);

	kvm_riscv_vcpu_vector_reset(vcpu);

	kvm_riscv_vcpu_timer_reset(vcpu);

	kvm_riscv_vcpu_aia_reset(vcpu);

	bitmap_zero(vcpu->arch.irqs_pending, KVM_RISCV_VCPU_NR_IRQS);
	bitmap_zero(vcpu->arch.irqs_pending_mask, KVM_RISCV_VCPU_NR_IRQS);

	kvm_riscv_vcpu_pmu_reset(vcpu);

	vcpu->arch.hfence_head = 0;
	vcpu->arch.hfence_tail = 0;
	memset(vcpu->arch.hfence_queue, 0, sizeof(vcpu->arch.hfence_queue));

	kvm_riscv_vcpu_sbi_reset(vcpu);

	/* Reset the guest CSRs for hotplug usecase */
	if (loaded)
		kvm_arch_vcpu_load(vcpu, smp_processor_id());
	put_cpu();
}

int kvm_arch_vcpu_precreate(struct kvm *kvm, unsigned int id)
{
	return 0;
}

int kvm_arch_vcpu_create(struct kvm_vcpu *vcpu)
{
	int rc;

	spin_lock_init(&vcpu->arch.mp_state_lock);

	/* Mark this VCPU never ran */
	vcpu->arch.ran_atleast_once = false;

	vcpu->arch.cfg.hedeleg = KVM_HEDELEG_DEFAULT;
	vcpu->arch.mmu_page_cache.gfp_zero = __GFP_ZERO;
	bitmap_zero(vcpu->arch.isa, RISCV_ISA_EXT_MAX);

	/* Setup ISA features available to VCPU */
	kvm_riscv_vcpu_setup_isa(vcpu);

	/* Setup vendor, arch, and implementation details */
	vcpu->arch.mvendorid = sbi_get_mvendorid();
	vcpu->arch.marchid = sbi_get_marchid();
	vcpu->arch.mimpid = sbi_get_mimpid();

	/* Setup VCPU hfence queue */
	spin_lock_init(&vcpu->arch.hfence_lock);

	spin_lock_init(&vcpu->arch.reset_state.lock);

	rc = kvm_riscv_vcpu_alloc_vector_context(vcpu);
	if (rc)
		return rc;

	/* Setup VCPU timer */
	kvm_riscv_vcpu_timer_init(vcpu);

	/* setup performance monitoring */
	kvm_riscv_vcpu_pmu_init(vcpu);

	/* Setup VCPU AIA */
	kvm_riscv_vcpu_aia_init(vcpu);

	/*
	 * Setup SBI extensions
	 * NOTE: This must be the last thing to be initialized.
	 */
	kvm_riscv_vcpu_sbi_init(vcpu);

	/* Reset VCPU */
	kvm_riscv_reset_vcpu(vcpu, false);

	if (vcpu->kvm->cmode) {
		struct iie_cvm_sbi_params *cvm_sbi_params;
		struct sbiret sbi_ret;

		cvm_sbi_params = kzalloc(sizeof(*cvm_sbi_params), GFP_KERNEL);
		if (!cvm_sbi_params)
			return -ENOMEM;

		cvm_sbi_params->vmid_ptr = (struct kvm_vmid *)__pa(&vcpu->kvm->arch.vmid);
		cvm_sbi_params->vcpu_id_ptr = (int *)__pa(&vcpu->vcpu_id);
		sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_CREATE_VCPU,
				    __pa(cvm_sbi_params), 0, 0, 0, 0, 0);
		kfree(cvm_sbi_params);
		if (sbi_ret.error)
			return sbi_ret.error;
	}

	return 0;
}

void kvm_arch_vcpu_postcreate(struct kvm_vcpu *vcpu)
{
	/**
	 * vcpu with id 0 is the designated boot cpu.
	 * Keep all vcpus with non-zero id in power-off state so that
	 * they can be brought up using SBI HSM extension.
	 */
	if (vcpu->vcpu_idx != 0)
		kvm_riscv_vcpu_power_off(vcpu);
}

void kvm_arch_vcpu_destroy(struct kvm_vcpu *vcpu)
{
	kvm_riscv_vcpu_sbi_deinit(vcpu);

	/* Cleanup VCPU AIA context */
	kvm_riscv_vcpu_aia_deinit(vcpu);

	/* Cleanup VCPU timer */
	kvm_riscv_vcpu_timer_deinit(vcpu);

	kvm_riscv_vcpu_pmu_deinit(vcpu);

	/* Free unused pages pre-allocated for G-stage page table mappings */
	kvm_mmu_free_memory_cache(&vcpu->arch.mmu_page_cache);

	/* Free vector context space for host and guest kernel */
	kvm_riscv_vcpu_free_vector_context(vcpu);
}

int kvm_cpu_has_pending_timer(struct kvm_vcpu *vcpu)
{
	return kvm_riscv_vcpu_timer_pending(vcpu);
}

int kvm_arch_vcpu_runnable(struct kvm_vcpu *vcpu)
{
	return (kvm_riscv_vcpu_has_interrupts(vcpu, -1ULL) &&
		!kvm_riscv_vcpu_stopped(vcpu) && !vcpu->arch.pause);
}

int kvm_arch_vcpu_should_kick(struct kvm_vcpu *vcpu)
{
	return kvm_vcpu_exiting_guest_mode(vcpu) == IN_GUEST_MODE;
}

bool kvm_arch_vcpu_in_kernel(struct kvm_vcpu *vcpu)
{
	return (vcpu->arch.guest_context.sstatus & SR_SPP) ? true : false;
}

#ifdef CONFIG_GUEST_PERF_EVENTS
unsigned long kvm_arch_vcpu_get_ip(struct kvm_vcpu *vcpu)
{
	return vcpu->arch.guest_context.sepc;
}
#endif

vm_fault_t kvm_arch_vcpu_fault(struct kvm_vcpu *vcpu, struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

long kvm_arch_vcpu_unlocked_ioctl(struct file *filp, unsigned int ioctl,
				  unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;

	if (ioctl == KVM_INTERRUPT) {
		struct kvm_interrupt irq;

		if (copy_from_user(&irq, argp, sizeof(irq)))
			return -EFAULT;

		if (irq.irq == KVM_INTERRUPT_SET)
			return kvm_riscv_vcpu_set_interrupt(vcpu, IRQ_VS_EXT);
		else
			return kvm_riscv_vcpu_unset_interrupt(vcpu, IRQ_VS_EXT);
	}

	return -ENOIOCTLCMD;
}

long kvm_arch_vcpu_ioctl(struct file *filp,
			 unsigned int ioctl, unsigned long arg)
{
	struct kvm_vcpu *vcpu = filp->private_data;
	void __user *argp = (void __user *)arg;
	long r = -EINVAL;

	switch (ioctl) {
	case KVM_SET_ONE_REG:
	case KVM_GET_ONE_REG: {
		struct kvm_one_reg reg;

		r = -EFAULT;
		if (copy_from_user(&reg, argp, sizeof(reg)))
			break;

		if (ioctl == KVM_SET_ONE_REG)
			r = kvm_riscv_vcpu_set_reg(vcpu, &reg);
		else
			r = kvm_riscv_vcpu_get_reg(vcpu, &reg);
		break;
	}
	case KVM_GET_REG_LIST: {
		struct kvm_reg_list __user *user_list = argp;
		struct kvm_reg_list reg_list;
		unsigned int n;

		r = -EFAULT;
		if (copy_from_user(&reg_list, user_list, sizeof(reg_list)))
			break;
		n = reg_list.n;
		reg_list.n = kvm_riscv_vcpu_num_regs(vcpu);
		if (copy_to_user(user_list, &reg_list, sizeof(reg_list)))
			break;
		r = -E2BIG;
		if (n < reg_list.n)
			break;
		r = kvm_riscv_vcpu_copy_reg_indices(vcpu, user_list->reg);
		break;
	}
	default:
		break;
	}

	return r;
}

int kvm_arch_vcpu_ioctl_get_sregs(struct kvm_vcpu *vcpu,
				  struct kvm_sregs *sregs)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_set_sregs(struct kvm_vcpu *vcpu,
				  struct kvm_sregs *sregs)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_get_fpu(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_set_fpu(struct kvm_vcpu *vcpu, struct kvm_fpu *fpu)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_translate(struct kvm_vcpu *vcpu,
				  struct kvm_translation *tr)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_get_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	return -EINVAL;
}

int kvm_arch_vcpu_ioctl_set_regs(struct kvm_vcpu *vcpu, struct kvm_regs *regs)
{
	return -EINVAL;
}

void kvm_riscv_vcpu_flush_interrupts(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;
	unsigned long mask, val;

	if (READ_ONCE(vcpu->arch.irqs_pending_mask[0])) {
		mask = xchg_acquire(&vcpu->arch.irqs_pending_mask[0], 0);
		val = READ_ONCE(vcpu->arch.irqs_pending[0]) & mask;

		csr->hvip &= ~mask;
		csr->hvip |= val;
	}

	/* Flush AIA high interrupts */
	kvm_riscv_vcpu_aia_flush_interrupts(vcpu);
}

void kvm_riscv_vcpu_sync_interrupts(struct kvm_vcpu *vcpu)
{
	unsigned long hvip;
	struct kvm_vcpu_arch *v = &vcpu->arch;
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;

	if (vcpu->kvm->cmode) {
		/*
		 * CVM runs through OpenSBI, which saves the live VS/HVIP CSRs
		 * into guest_csr before returning to KVM.  Reading the HS CSR
		 * bank here can overwrite the shadow state with stale host-side
		 * values, especially when multiple vCPUs are active.
		 */
		hvip = csr->hvip;
		goto sync_hvip;
	}

	/* Read current HVIP and VSIE CSRs */
	csr->vsie = ncsr_read(CSR_VSIE);

	/* Sync-up HVIP.VSSIP bit changes does by Guest */
	hvip = ncsr_read(CSR_HVIP);
sync_hvip:
	if ((csr->hvip ^ hvip) & (1UL << IRQ_VS_SOFT)) {
		if (hvip & (1UL << IRQ_VS_SOFT)) {
			if (!test_and_set_bit(IRQ_VS_SOFT,
					      v->irqs_pending_mask))
				set_bit(IRQ_VS_SOFT, v->irqs_pending);
		} else {
			if (!test_and_set_bit(IRQ_VS_SOFT,
					      v->irqs_pending_mask))
				clear_bit(IRQ_VS_SOFT, v->irqs_pending);
		}
	}

	/* Sync up the HVIP.LCOFIP bit changes (only clear) by the guest */
	if ((csr->hvip ^ hvip) & (1UL << IRQ_PMU_OVF)) {
		if (!(hvip & (1UL << IRQ_PMU_OVF)) &&
		    !test_and_set_bit(IRQ_PMU_OVF, v->irqs_pending_mask))
			clear_bit(IRQ_PMU_OVF, v->irqs_pending);
	}

	if (vcpu->kvm->cmode)
		return;

	/* Sync-up AIA high interrupts */
	kvm_riscv_vcpu_aia_sync_interrupts(vcpu);

	/* Sync-up timer CSRs */
	kvm_riscv_vcpu_timer_sync(vcpu);
}

int kvm_riscv_vcpu_set_interrupt(struct kvm_vcpu *vcpu, unsigned int irq)
{
	/*
	 * We only allow VS-mode software, timer, and external
	 * interrupts when irq is one of the local interrupts
	 * defined by RISC-V privilege specification.
	 */
	if (irq < IRQ_LOCAL_MAX &&
	    irq != IRQ_VS_SOFT &&
	    irq != IRQ_VS_TIMER &&
	    irq != IRQ_VS_EXT &&
	    irq != IRQ_PMU_OVF)
		return -EINVAL;

	set_bit(irq, vcpu->arch.irqs_pending);
	smp_mb__before_atomic();
	set_bit(irq, vcpu->arch.irqs_pending_mask);

	kvm_vcpu_kick(vcpu);

	return 0;
}

int kvm_riscv_vcpu_unset_interrupt(struct kvm_vcpu *vcpu, unsigned int irq)
{
	/*
	 * We only allow VS-mode software, timer, counter overflow and external
	 * interrupts when irq is one of the local interrupts
	 * defined by RISC-V privilege specification.
	 */
	if (irq < IRQ_LOCAL_MAX &&
	    irq != IRQ_VS_SOFT &&
	    irq != IRQ_VS_TIMER &&
	    irq != IRQ_VS_EXT &&
	    irq != IRQ_PMU_OVF)
		return -EINVAL;

	clear_bit(irq, vcpu->arch.irqs_pending);
	smp_mb__before_atomic();
	set_bit(irq, vcpu->arch.irqs_pending_mask);

	return 0;
}

bool kvm_riscv_vcpu_has_interrupts(struct kvm_vcpu *vcpu, u64 mask)
{
	unsigned long ie;

	ie = ((vcpu->arch.guest_csr.vsie & VSIP_VALID_MASK)
		<< VSIP_TO_HVIP_SHIFT) & (unsigned long)mask;
	ie |= vcpu->arch.guest_csr.vsie & ~IRQ_LOCAL_MASK &
		(unsigned long)mask;
	if (READ_ONCE(vcpu->arch.irqs_pending[0]) & ie)
		return true;

	/* Check AIA high interrupts */
	return kvm_riscv_vcpu_aia_has_interrupts(vcpu, mask);
}

void __kvm_riscv_vcpu_power_off(struct kvm_vcpu *vcpu)
{
	WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_STOPPED);
	kvm_make_request(KVM_REQ_SLEEP, vcpu);
	kvm_vcpu_kick(vcpu);
}

void kvm_riscv_vcpu_power_off(struct kvm_vcpu *vcpu)
{
	spin_lock(&vcpu->arch.mp_state_lock);
	__kvm_riscv_vcpu_power_off(vcpu);
	spin_unlock(&vcpu->arch.mp_state_lock);
}

void __kvm_riscv_vcpu_power_on(struct kvm_vcpu *vcpu)
{
	WRITE_ONCE(vcpu->arch.mp_state.mp_state, KVM_MP_STATE_RUNNABLE);
	kvm_vcpu_wake_up(vcpu);
}

void kvm_riscv_vcpu_power_on(struct kvm_vcpu *vcpu)
{
	spin_lock(&vcpu->arch.mp_state_lock);
	__kvm_riscv_vcpu_power_on(vcpu);
	spin_unlock(&vcpu->arch.mp_state_lock);
}

bool kvm_riscv_vcpu_stopped(struct kvm_vcpu *vcpu)
{
	return READ_ONCE(vcpu->arch.mp_state.mp_state) == KVM_MP_STATE_STOPPED;
}

int kvm_arch_vcpu_ioctl_get_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	*mp_state = READ_ONCE(vcpu->arch.mp_state);

	return 0;
}

int kvm_arch_vcpu_ioctl_set_mpstate(struct kvm_vcpu *vcpu,
				    struct kvm_mp_state *mp_state)
{
	int ret = 0;

	spin_lock(&vcpu->arch.mp_state_lock);

	switch (mp_state->mp_state) {
	case KVM_MP_STATE_RUNNABLE:
		WRITE_ONCE(vcpu->arch.mp_state, *mp_state);
		break;
	case KVM_MP_STATE_STOPPED:
		__kvm_riscv_vcpu_power_off(vcpu);
		break;
	case KVM_MP_STATE_INIT_RECEIVED:
		if (vcpu->kvm->arch.mp_state_reset)
			kvm_riscv_reset_vcpu(vcpu, false);
		else
			ret = -EINVAL;
		break;
	default:
		ret = -EINVAL;
	}

	spin_unlock(&vcpu->arch.mp_state_lock);

	return ret;
}

int kvm_arch_vcpu_ioctl_set_guest_debug(struct kvm_vcpu *vcpu,
					struct kvm_guest_debug *dbg)
{
	if (dbg->control & KVM_GUESTDBG_ENABLE) {
		vcpu->guest_debug = dbg->control;
		vcpu->arch.cfg.hedeleg &= ~BIT(EXC_BREAKPOINT);
	} else {
		vcpu->guest_debug = 0;
		vcpu->arch.cfg.hedeleg |= BIT(EXC_BREAKPOINT);
	}

	return 0;
}

static void kvm_riscv_vcpu_setup_config(struct kvm_vcpu *vcpu)
{
	const unsigned long *isa = vcpu->arch.isa;
	struct kvm_vcpu_config *cfg = &vcpu->arch.cfg;

	if (riscv_isa_extension_available(isa, SVPBMT))
		cfg->henvcfg |= ENVCFG_PBMTE;

	if (riscv_isa_extension_available(isa, SSTC))
		cfg->henvcfg |= ENVCFG_STCE;

	if (riscv_isa_extension_available(isa, ZICBOM))
		cfg->henvcfg |= (ENVCFG_CBIE | ENVCFG_CBCFE);

	if (riscv_isa_extension_available(isa, ZICBOZ))
		cfg->henvcfg |= ENVCFG_CBZE;

	if (riscv_isa_extension_available(isa, SVADU) &&
	    !riscv_isa_extension_available(isa, SVADE))
		cfg->henvcfg |= ENVCFG_ADUE;

	if (riscv_has_extension_unlikely(RISCV_ISA_EXT_SMSTATEEN)) {
		cfg->hstateen0 |= SMSTATEEN0_HSENVCFG;
		if (riscv_isa_extension_available(isa, SSAIA))
			cfg->hstateen0 |= SMSTATEEN0_AIA_IMSIC |
					  SMSTATEEN0_AIA |
					  SMSTATEEN0_AIA_ISEL;
		if (riscv_isa_extension_available(isa, SMSTATEEN))
			cfg->hstateen0 |= SMSTATEEN0_SSTATEEN0;
	}

	if (vcpu->guest_debug)
		cfg->hedeleg &= ~BIT(EXC_BREAKPOINT);
}

void kvm_arch_vcpu_load(struct kvm_vcpu *vcpu, int cpu)
{
	void *nsh;
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;
	struct kvm_vcpu_config *cfg = &vcpu->arch.cfg;

	if (kvm_riscv_nacl_sync_csr_available()) {
		nsh = nacl_shmem();
		nacl_csr_write(nsh, CSR_VSSTATUS, csr->vsstatus);
		nacl_csr_write(nsh, CSR_VSIE, csr->vsie);
		nacl_csr_write(nsh, CSR_VSTVEC, csr->vstvec);
		nacl_csr_write(nsh, CSR_VSSCRATCH, csr->vsscratch);
		nacl_csr_write(nsh, CSR_VSEPC, csr->vsepc);
		nacl_csr_write(nsh, CSR_VSCAUSE, csr->vscause);
		nacl_csr_write(nsh, CSR_VSTVAL, csr->vstval);
		nacl_csr_write(nsh, CSR_HEDELEG, cfg->hedeleg);
		nacl_csr_write(nsh, CSR_HVIP, csr->hvip);
		nacl_csr_write(nsh, CSR_VSATP, csr->vsatp);
		nacl_csr_write(nsh, CSR_HENVCFG, cfg->henvcfg);
		if (IS_ENABLED(CONFIG_32BIT))
			nacl_csr_write(nsh, CSR_HENVCFGH, cfg->henvcfg >> 32);
		if (riscv_has_extension_unlikely(RISCV_ISA_EXT_SMSTATEEN)) {
			nacl_csr_write(nsh, CSR_HSTATEEN0, cfg->hstateen0);
			if (IS_ENABLED(CONFIG_32BIT))
				nacl_csr_write(nsh, CSR_HSTATEEN0H, cfg->hstateen0 >> 32);
		}
	} else {
		csr_write(CSR_VSSTATUS, csr->vsstatus);
		csr_write(CSR_VSIE, csr->vsie);
		csr_write(CSR_VSTVEC, csr->vstvec);
		csr_write(CSR_VSSCRATCH, csr->vsscratch);
		csr_write(CSR_VSEPC, csr->vsepc);
		csr_write(CSR_VSCAUSE, csr->vscause);
		csr_write(CSR_VSTVAL, csr->vstval);
		csr_write(CSR_HEDELEG, cfg->hedeleg);
		csr_write(CSR_HVIP, csr->hvip);
		csr_write(CSR_VSATP, csr->vsatp);
		csr_write(CSR_HENVCFG, cfg->henvcfg);
		if (IS_ENABLED(CONFIG_32BIT))
			csr_write(CSR_HENVCFGH, cfg->henvcfg >> 32);
		if (riscv_has_extension_unlikely(RISCV_ISA_EXT_SMSTATEEN)) {
			csr_write(CSR_HSTATEEN0, cfg->hstateen0);
			if (IS_ENABLED(CONFIG_32BIT))
				csr_write(CSR_HSTATEEN0H, cfg->hstateen0 >> 32);
		}
	}

	kvm_riscv_mmu_update_hgatp(vcpu);

	kvm_riscv_vcpu_timer_restore(vcpu);

	kvm_riscv_vcpu_host_fp_save(&vcpu->arch.host_context);
	kvm_riscv_vcpu_guest_fp_restore(&vcpu->arch.guest_context,
					vcpu->arch.isa);
	kvm_riscv_vcpu_host_vector_save(&vcpu->arch.host_context);
	kvm_riscv_vcpu_guest_vector_restore(&vcpu->arch.guest_context,
					    vcpu->arch.isa);

	kvm_riscv_vcpu_aia_load(vcpu, cpu);

	kvm_make_request(KVM_REQ_STEAL_UPDATE, vcpu);

	vcpu->cpu = cpu;
}

void kvm_arch_vcpu_put(struct kvm_vcpu *vcpu)
{
	void *nsh;
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;

	vcpu->cpu = -1;

	kvm_riscv_vcpu_aia_put(vcpu);

	kvm_riscv_vcpu_guest_fp_save(&vcpu->arch.guest_context,
				     vcpu->arch.isa);
	kvm_riscv_vcpu_host_fp_restore(&vcpu->arch.host_context);

	kvm_riscv_vcpu_timer_save(vcpu);
	kvm_riscv_vcpu_guest_vector_save(&vcpu->arch.guest_context,
					 vcpu->arch.isa);
	kvm_riscv_vcpu_host_vector_restore(&vcpu->arch.host_context);

	if (kvm_riscv_nacl_available()) {
		nsh = nacl_shmem();
		csr->vsstatus = nacl_csr_read(nsh, CSR_VSSTATUS);
		csr->vsie = nacl_csr_read(nsh, CSR_VSIE);
		csr->vstvec = nacl_csr_read(nsh, CSR_VSTVEC);
		csr->vsscratch = nacl_csr_read(nsh, CSR_VSSCRATCH);
		csr->vsepc = nacl_csr_read(nsh, CSR_VSEPC);
		csr->vscause = nacl_csr_read(nsh, CSR_VSCAUSE);
		csr->vstval = nacl_csr_read(nsh, CSR_VSTVAL);
		csr->hvip = nacl_csr_read(nsh, CSR_HVIP);
		csr->vsatp = nacl_csr_read(nsh, CSR_VSATP);
	} else {
		csr->vsstatus = csr_read(CSR_VSSTATUS);
		csr->vsie = csr_read(CSR_VSIE);
		csr->vstvec = csr_read(CSR_VSTVEC);
		csr->vsscratch = csr_read(CSR_VSSCRATCH);
		csr->vsepc = csr_read(CSR_VSEPC);
		csr->vscause = csr_read(CSR_VSCAUSE);
		csr->vstval = csr_read(CSR_VSTVAL);
		csr->hvip = csr_read(CSR_HVIP);
		csr->vsatp = csr_read(CSR_VSATP);
	}
}

/**
 * kvm_riscv_check_vcpu_requests - check and handle pending vCPU requests
 * @vcpu:	the VCPU pointer
 *
 * Return: 1 if we should enter the guest
 *	    0 if we should exit to userspace
 */
static int kvm_riscv_check_vcpu_requests(struct kvm_vcpu *vcpu)
{
	struct rcuwait *wait = kvm_arch_vcpu_get_wait(vcpu);

	if (kvm_request_pending(vcpu)) {
		if (kvm_check_request(KVM_REQ_SLEEP, vcpu)) {
			kvm_vcpu_srcu_read_unlock(vcpu);
			rcuwait_wait_event(wait,
				(!kvm_riscv_vcpu_stopped(vcpu)) && (!vcpu->arch.pause),
				TASK_INTERRUPTIBLE);
			kvm_vcpu_srcu_read_lock(vcpu);

			if (kvm_riscv_vcpu_stopped(vcpu) || vcpu->arch.pause) {
				/*
				 * Awaken to handle a signal, request to
				 * sleep again later.
				 */
				kvm_make_request(KVM_REQ_SLEEP, vcpu);
			}
		}

		if (kvm_check_request(KVM_REQ_VCPU_RESET, vcpu))
			kvm_riscv_reset_vcpu(vcpu, true);

		if (kvm_check_request(KVM_REQ_UPDATE_HGATP, vcpu))
			kvm_riscv_mmu_update_hgatp(vcpu);

		if (kvm_check_request(KVM_REQ_FENCE_I, vcpu))
			kvm_riscv_fence_i_process(vcpu);

		if (kvm_check_request(KVM_REQ_TLB_FLUSH, vcpu))
			kvm_riscv_tlb_flush_process(vcpu);

		if (kvm_check_request(KVM_REQ_HFENCE_VVMA_ALL, vcpu))
			kvm_riscv_hfence_vvma_all_process(vcpu);

		if (kvm_check_request(KVM_REQ_HFENCE, vcpu))
			kvm_riscv_hfence_process(vcpu);

		if (kvm_check_request(KVM_REQ_STEAL_UPDATE, vcpu))
			kvm_riscv_vcpu_record_steal_time(vcpu);

		if (kvm_dirty_ring_check_request(vcpu))
			return 0;
	}

	return 1;
}

static void kvm_riscv_update_hvip(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;

	if (vcpu->kvm->cmode)
		return;

	ncsr_write(CSR_HVIP, csr->hvip);
	kvm_riscv_vcpu_aia_update_hvip(vcpu);
}

static __always_inline void kvm_riscv_vcpu_swap_in_guest_state(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_smstateen_csr *smcsr = &vcpu->arch.smstateen_csr;
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;
	struct kvm_vcpu_config *cfg = &vcpu->arch.cfg;

	vcpu->arch.host_scounteren = csr_swap(CSR_SCOUNTEREN, csr->scounteren);
	vcpu->arch.host_senvcfg = csr_swap(CSR_SENVCFG, csr->senvcfg);
	if (riscv_has_extension_unlikely(RISCV_ISA_EXT_SMSTATEEN) &&
	    (cfg->hstateen0 & SMSTATEEN0_SSTATEEN0))
		vcpu->arch.host_sstateen0 = csr_swap(CSR_SSTATEEN0,
						     smcsr->sstateen0);
}

static __always_inline void kvm_riscv_vcpu_swap_in_host_state(struct kvm_vcpu *vcpu)
{
	struct kvm_vcpu_smstateen_csr *smcsr = &vcpu->arch.smstateen_csr;
	struct kvm_vcpu_csr *csr = &vcpu->arch.guest_csr;
	struct kvm_vcpu_config *cfg = &vcpu->arch.cfg;

	csr->scounteren = csr_swap(CSR_SCOUNTEREN, vcpu->arch.host_scounteren);
	csr->senvcfg = csr_swap(CSR_SENVCFG, vcpu->arch.host_senvcfg);
	if (riscv_has_extension_unlikely(RISCV_ISA_EXT_SMSTATEEN) &&
	    (cfg->hstateen0 & SMSTATEEN0_SSTATEEN0))
		smcsr->sstateen0 = csr_swap(CSR_SSTATEEN0,
					    vcpu->arch.host_sstateen0);
}

/*
 * Actually run the vCPU, entering an RCU extended quiescent state (EQS) while
 * the vCPU is running.
 *
 * This must be noinstr as instrumentation may make use of RCU, and this is not
 * safe during the EQS.
 */
static void noinstr kvm_riscv_vcpu_enter_exit(struct kvm_vcpu *vcpu,
					      struct kvm_cpu_trap *trap)
{
	void *nsh;
	struct kvm_cpu_context *gcntx = &vcpu->arch.guest_context;
	struct kvm_cpu_context *hcntx = &vcpu->arch.host_context;

	/*
	 * We save trap CSRs (such as SEPC, SCAUSE, STVAL, HTVAL, and
	 * HTINST) here because we do local_irq_enable() after this
	 * function in kvm_arch_vcpu_ioctl_run() which can result in
	 * an interrupt immediately after local_irq_enable() and can
	 * potentially change trap CSRs.
	 */

	kvm_riscv_vcpu_swap_in_guest_state(vcpu);
	guest_state_enter_irqoff();

	if (kvm_riscv_nacl_sync_sret_available()) {
		nsh = nacl_shmem();

		if (kvm_riscv_nacl_autoswap_csr_available()) {
			hcntx->hstatus =
				nacl_csr_read(nsh, CSR_HSTATUS);
			nacl_scratch_write_long(nsh,
						SBI_NACL_SHMEM_AUTOSWAP_OFFSET +
						SBI_NACL_SHMEM_AUTOSWAP_HSTATUS,
						gcntx->hstatus);
			nacl_scratch_write_long(nsh,
						SBI_NACL_SHMEM_AUTOSWAP_OFFSET,
						SBI_NACL_SHMEM_AUTOSWAP_FLAG_HSTATUS);
		} else if (kvm_riscv_nacl_sync_csr_available()) {
			hcntx->hstatus = nacl_csr_swap(nsh,
						       CSR_HSTATUS, gcntx->hstatus);
		} else {
			hcntx->hstatus = csr_swap(CSR_HSTATUS, gcntx->hstatus);
		}

		nacl_scratch_write_longs(nsh,
					 SBI_NACL_SHMEM_SRET_OFFSET +
					 SBI_NACL_SHMEM_SRET_X(1),
					 &gcntx->ra,
					 SBI_NACL_SHMEM_SRET_X_LAST);

		__kvm_riscv_nacl_switch_to(&vcpu->arch, SBI_EXT_NACL,
					   SBI_EXT_NACL_SYNC_SRET);

		if (kvm_riscv_nacl_autoswap_csr_available()) {
			nacl_scratch_write_long(nsh,
						SBI_NACL_SHMEM_AUTOSWAP_OFFSET,
						0);
			gcntx->hstatus = nacl_scratch_read_long(nsh,
								SBI_NACL_SHMEM_AUTOSWAP_OFFSET +
								SBI_NACL_SHMEM_AUTOSWAP_HSTATUS);
		} else {
			gcntx->hstatus = csr_swap(CSR_HSTATUS, hcntx->hstatus);
		}

		trap->htval = nacl_csr_read(nsh, CSR_HTVAL);
		trap->htinst = nacl_csr_read(nsh, CSR_HTINST);
	} else {
		hcntx->hstatus = csr_swap(CSR_HSTATUS, gcntx->hstatus);

		__kvm_riscv_switch_to(&vcpu->arch);

		gcntx->hstatus = csr_swap(CSR_HSTATUS, hcntx->hstatus);

		trap->htval = csr_read(CSR_HTVAL);
		trap->htinst = csr_read(CSR_HTINST);
	}

	trap->sepc = gcntx->sepc;
	trap->scause = csr_read(CSR_SCAUSE);
	trap->stval = csr_read(CSR_STVAL);

	vcpu->arch.last_exit_cpu = vcpu->cpu;
	guest_state_exit_irqoff();
	kvm_riscv_vcpu_swap_in_host_state(vcpu);
}

int kvm_arch_vcpu_ioctl_run(struct kvm_vcpu *vcpu)
{
	int ret;
	struct kvm_cpu_trap trap;
	struct kvm_cpu_trap *trap_ptr = &trap;
	struct iie_cvm_sbi_params *cvm_sbi_params = NULL;
	struct kvm_cpu_trap *cvm_trap = NULL;
	struct kvm_run *run = vcpu->run;

	if (vcpu->kvm->cmode) {
		cvm_sbi_params = kzalloc(sizeof(*cvm_sbi_params), GFP_KERNEL);
		cvm_trap = kzalloc(sizeof(*cvm_trap), GFP_KERNEL);
		if (!cvm_sbi_params || !cvm_trap) {
			kfree(cvm_sbi_params);
			kfree(cvm_trap);
			return -ENOMEM;
		}

		cvm_sbi_params->vmid_ptr = (struct kvm_vmid *)__pa(&vcpu->kvm->arch.vmid);
		cvm_sbi_params->vcpu_id_ptr = (int *)__pa(&vcpu->vcpu_id);
		trap_ptr = cvm_trap;
	}

	if (!vcpu->arch.ran_atleast_once)
		kvm_riscv_vcpu_setup_config(vcpu);

	/* Mark this VCPU ran at least once */
	vcpu->arch.ran_atleast_once = true;

	kvm_vcpu_srcu_read_lock(vcpu);

	switch (run->exit_reason) {
	case KVM_EXIT_MMIO:
		/* Process MMIO value returned from user-space */
		ret = kvm_riscv_vcpu_mmio_return(vcpu, vcpu->run);
		break;
	case KVM_EXIT_RISCV_SBI:
		/* Process SBI value returned from user-space */
		ret = kvm_riscv_vcpu_sbi_return(vcpu, vcpu->run);
		break;
	case KVM_EXIT_RISCV_CSR:
		/* Process CSR value returned from user-space */
		ret = kvm_riscv_vcpu_csr_return(vcpu, vcpu->run);
		break;
	default:
		ret = 0;
		break;
	}
	if (ret) {
		kvm_vcpu_srcu_read_unlock(vcpu);
		kfree(cvm_sbi_params);
		kfree(cvm_trap);
		return ret;
	}

	if (!vcpu->wants_to_run) {
		kvm_vcpu_srcu_read_unlock(vcpu);
		kfree(cvm_sbi_params);
		kfree(cvm_trap);
		return -EINTR;
	}

	vcpu_load(vcpu);

	kvm_sigset_activate(vcpu);

	ret = 1;
	run->exit_reason = KVM_EXIT_UNKNOWN;
	while (ret > 0) {
		int cvm_run_error = 0;

		/* Check conditions before entering the guest */
		ret = kvm_xfer_to_guest_mode_handle_work(vcpu);
		if (ret)
			continue;
		ret = 1;

		kvm_riscv_gstage_vmid_update(vcpu);

		ret = kvm_riscv_check_vcpu_requests(vcpu);
		if (ret <= 0)
			continue;

		preempt_disable();

		/* Update AIA HW state before entering guest */
		ret = kvm_riscv_vcpu_aia_update(vcpu);
		if (ret <= 0) {
			preempt_enable();
			continue;
		}

		local_irq_disable();

		/*
		 * Ensure we set mode to IN_GUEST_MODE after we disable
		 * interrupts and before the final VCPU requests check.
		 * See the comment in kvm_vcpu_exiting_guest_mode() and
		 * Documentation/virt/kvm/vcpu-requests.rst
		 */
		vcpu->mode = IN_GUEST_MODE;

		kvm_vcpu_srcu_read_unlock(vcpu);
		smp_mb__after_srcu_read_unlock();

		/*
		 * We might have got VCPU interrupts updated asynchronously
		 * so update it in HW.
		 */
		kvm_riscv_vcpu_flush_interrupts(vcpu);

		/* Update HVIP CSR for current CPU */
		kvm_riscv_update_hvip(vcpu);

		if (kvm_riscv_gstage_vmid_ver_changed(&vcpu->kvm->arch.vmid) ||
		    kvm_request_pending(vcpu) ||
		    xfer_to_guest_mode_work_pending()) {
			vcpu->mode = OUTSIDE_GUEST_MODE;
			local_irq_enable();
			preempt_enable();
			kvm_vcpu_srcu_read_lock(vcpu);
			continue;
		}

		/*
		 * Sanitize VMID mappings cached (TLB) on current CPU
		 *
		 * Note: This should be done after G-stage VMID has been
		 * updated using kvm_riscv_gstage_vmid_ver_changed()
		 */
		kvm_riscv_local_tlb_sanitize(vcpu);

		trace_kvm_entry(vcpu);

		guest_timing_enter_irqoff();

		if (vcpu->kvm->cmode) {
			struct swiotlb sw;
			struct sbiret sbi_ret;
			bool dma_allowed = false;

			cvm_sbi_params->gpa =
				(trap_ptr->htval << 2) | (trap_ptr->stval & 0x3);
			if (kvm_riscv_find_swiotlb(vcpu->kvm,
						   cvm_sbi_params->gpa, &sw)) {
				cvm_sbi_params->device_id = sw.device_id;
				dma_allowed =
					kvm_riscv_cove_io_dma_allowed(vcpu->kvm,
								      sw.device_id,
								      cvm_sbi_params->gpa);
				if (dma_allowed) {
					cvm_sbi_params->hpa =
						kvm_riscv_gstage_gpa_to_hpa(vcpu->kvm,
									    cvm_sbi_params->gpa);
				} else {
					cvm_sbi_params->hpa = 0;
				}
			} else {
				cvm_sbi_params->device_id = KVM_COVE_IO_DEVICE_INVALID;
				cvm_sbi_params->hpa = 0;
			}

			sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_RUN_VCPU,
					    __pa(cvm_sbi_params),
					    __pa(&vcpu->arch.guest_context),
					    __pa(&vcpu->arch.guest_csr),
					    __pa(trap_ptr), 0, 0);
			if (sbi_ret.error == TEE_NO_MEMORY) {
				cvm_run_error = refill_KVM_memory_pool();
				if (!cvm_run_error)
					trap_ptr->scause = CAUSE_IRQ_FLAG;
			} else if (sbi_ret.error < 0) {
				pr_warn_ratelimited("kvm: CVM run returned %ld\n",
						    sbi_ret.error);
				cvm_run_error = sbi_ret.error;
			}
		} else {
			kvm_riscv_vcpu_enter_exit(vcpu, trap_ptr);
		}

		vcpu->mode = OUTSIDE_GUEST_MODE;
		vcpu->stat.exits++;

		/* Syncup interrupts state with HW */
		kvm_riscv_vcpu_sync_interrupts(vcpu);

		/*
		 * We must ensure that any pending interrupts are taken before
		 * we exit guest timing so that timer ticks are accounted as
		 * guest time. Transiently unmask interrupts so that any
		 * pending interrupts are taken.
		 *
		 * There's no barrier which ensures that pending interrupts are
		 * recognised, so we just hope that the CPU takes any pending
		 * interrupts between the enable and disable.
		 */
		local_irq_enable();
		local_irq_disable();

		guest_timing_exit_irqoff();

		local_irq_enable();

		trace_kvm_exit(trap_ptr);

		preempt_enable();

		kvm_vcpu_srcu_read_lock(vcpu);

		if (cvm_run_error) {
			ret = cvm_run_error;
			continue;
		}

		ret = kvm_riscv_vcpu_exit(vcpu, run, trap_ptr);
	}

	kvm_sigset_deactivate(vcpu);

	vcpu_put(vcpu);

	kvm_vcpu_srcu_read_unlock(vcpu);

	kfree(cvm_sbi_params);
	kfree(cvm_trap);
	return ret;
}

int kvm_vm_ioctl_swiotlb(struct kvm *kvm, void __user *argp)
{
	struct swiotlb_node *sw_node;
	struct sbiret sbi_ret;

	if (!kvm->cmode)
		return -EINVAL;

	sw_node = kzalloc(sizeof(*sw_node), GFP_KERNEL);
	if (!sw_node)
		return -ENOMEM;

	if (copy_from_user(&sw_node->sw, argp, sizeof(sw_node->sw))) {
		kfree(sw_node);
		return -EFAULT;
	}

	if (!kvm_riscv_cove_io_validate_pci_group(sw_node->sw.device_id,
						  sw_node->sw.iommu_group)) {
		pr_warn_ratelimited("kvm: COVE-IO SWIOTLB PCI RID/iommu_group mismatch device_id=0x%lx group=%lu\n",
				    sw_node->sw.device_id,
				    sw_node->sw.iommu_group);
		kfree(sw_node);
		return -EPERM;
	}

	sw_node->vmid = &kvm->arch.vmid.vmid;
	sw_node->kvm = kvm;

	spin_lock(&sw_list);
	sw_node->next = sw_list_head;
	sw_list_head = sw_node;
	spin_unlock(&sw_list);

	sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_INIT_SWIOTLB,
			    __pa(&sw_node->sw), __pa(&kvm->arch.vmid),
			    0, 0, 0, 0);
	if (sbi_ret.error) {
		kvm_riscv_destroy_sw_node(kvm);
		return sbi_ret.error;
	}

	return 0;
}

static u64 kvm_cove_io_device_type(u64 device_id)
{
	return (device_id & KVM_COVE_IO_DEVICE_TYPE_MASK) >>
	       KVM_COVE_IO_DEVICE_TYPE_SHIFT;
}

static bool kvm_cove_io_mrif_irq_bind_candidate(
	const struct cove_io_tdi_sbi_params *params)
{
	return params->device_id != KVM_COVE_IO_DEVICE_ANY &&
	       params->device_id != KVM_COVE_IO_DEVICE_INVALID &&
	       kvm_cove_io_device_type(params->device_id) ==
		       KVM_COVE_IO_DEVICE_TYPE_PCI_RID &&
	       params->vcpu_id != KVM_COVE_IO_VCPU_ANY &&
	       params->irq_iid != KVM_COVE_IO_IRQ_IID_ANY;
}

static bool kvm_riscv_cove_io_pci_fwspec_has_rid(struct pci_dev *pdev, u16 rid)
{
#if IS_ENABLED(CONFIG_IOMMU_API)
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(&pdev->dev);
	int i;

	if (!fwspec || !fwspec->num_ids)
		return false;

	for (i = 0; i < fwspec->num_ids; i++) {
		if (fwspec->ids[i] == rid)
			return true;
	}

	return false;
#else
	return false;
#endif
}

static bool kvm_riscv_cove_io_validate_pci_group(u64 device_id,
						 u64 expected_group)
{
	struct iommu_group *group;
	struct pci_dev *pdev;
	u16 segment, rid;
	u8 bus, devfn;
	bool valid = false;

	if (kvm_cove_io_device_type(device_id) != KVM_COVE_IO_DEVICE_TYPE_PCI_RID)
		return true;
	if (expected_group == KVM_COVE_IO_IOMMU_GROUP_INVALID)
		return false;

	segment = (device_id >> 16) & 0xffff;
	rid = device_id & 0xffff;
	bus = rid >> 8;
	devfn = rid & 0xff;

	pdev = pci_get_domain_bus_and_slot(segment, bus, devfn);
	if (!pdev)
		return false;

	group = iommu_group_get(&pdev->dev);
	if (group) {
		valid = iommu_group_id(group) == expected_group &&
			kvm_riscv_cove_io_pci_fwspec_has_rid(pdev, rid);
		iommu_group_put(group);
	}

	pci_dev_put(pdev);
	return valid;
}

int kvm_vm_ioctl_cove_io_tdi(struct kvm *kvm, void __user *argp)
{
	struct cove_io_tdi_sbi_params *params;
	struct cove_io_tdi_sbi_params mrif_params;
	struct kvm_cove_io_tdi user;
	struct sbiret sbi_ret;
	int ret = 0;

	if (!kvm->cmode)
		return -EINVAL;

	if (copy_from_user(&user, argp, sizeof(user)))
		return -EFAULT;

	if (user.op == KVM_COVE_IO_TDI_DMA_MAP &&
	    !kvm_riscv_cove_io_validate_pci_group(user.device_id,
						  user.iommu_group)) {
		pr_warn_ratelimited("kvm: COVE-IO PCI RID/iommu_group mismatch device_id=0x%llx group=%llu\n",
				    (unsigned long long)user.device_id,
				    (unsigned long long)user.iommu_group);
		return -EPERM;
	}

	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!params)
		return -ENOMEM;

	params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);
	params->op = user.op;
	params->flags = user.flags;
	params->tdi_id = user.tdi_id;
	params->generation = user.generation;
	params->mmio_gpa = user.mmio_gpa;
	params->mmio_size = user.mmio_size;
	params->dma_gpa = user.dma_gpa;
	params->dma_size = user.dma_size;
	params->irq_id = user.irq_id;
	params->irq_num = user.irq_num;
	params->vcpu_id = user.vcpu_id;
	params->irq_iid = user.irq_iid;
	params->device_id = user.device_id;
	params->state = user.state;

	sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
			    __pa(params), 0, 0, 0, 0, 0);
	if (sbi_ret.error) {
		ret = sbi_ret.error;
		goto out_free;
	}

	mrif_params = *params;
	if (user.op == KVM_COVE_IO_TDI_IRQ_BIND) {
		if (user.device_id != KVM_COVE_IO_DEVICE_ANY &&
		    user.device_id != KVM_COVE_IO_DEVICE_INVALID)
			mrif_params.device_id = user.device_id;
		mrif_params.vcpu_id = user.vcpu_id;
		if (user.irq_iid != KVM_COVE_IO_IRQ_IID_ANY)
			mrif_params.irq_iid = user.irq_iid;
	}

	if (user.op == KVM_COVE_IO_TDI_IRQ_BIND &&
	    kvm_cove_io_mrif_irq_bind_candidate(&mrif_params)) {
		ret = riscv_iommu_cove_io_mrif_bind(mrif_params.device_id, kvm,
						    mrif_params.vcpu_id,
						    mrif_params.irq_iid);
		if (ret) {
			pr_warn_ratelimited("kvm: COVE-IO MRIF bind failed device_id=0x%llx vcpu=%llu iid=%llu ret=%d\n",
					    (unsigned long long)mrif_params.device_id,
					    (unsigned long long)mrif_params.vcpu_id,
					    (unsigned long long)mrif_params.irq_iid,
					    ret);
			goto out_free;
		}
	} else if (user.op == KVM_COVE_IO_TDI_IRQ_UNBIND ||
		   user.op == KVM_COVE_IO_TDI_DMA_UNMAP ||
		   user.op == KVM_COVE_IO_TDI_UNBIND ||
		   user.op == KVM_COVE_IO_TDI_STOP ||
		   user.op == KVM_COVE_IO_TDI_UNREGISTER) {
		if (params->device_id != KVM_COVE_IO_DEVICE_ANY &&
		    params->device_id != KVM_COVE_IO_DEVICE_INVALID &&
		    kvm_cove_io_device_type(params->device_id) ==
			    KVM_COVE_IO_DEVICE_TYPE_PCI_RID)
			riscv_iommu_cove_io_mrif_unbind(params->device_id);
	}

	user.tdi_id = params->tdi_id;
	user.generation = params->generation;
	user.mmio_gpa = params->mmio_gpa;
	user.mmio_size = params->mmio_size;
	user.dma_gpa = params->dma_gpa;
	user.dma_size = params->dma_size;
	user.irq_id = params->irq_id;
	user.irq_num = params->irq_num;
	user.vcpu_id = params->vcpu_id;
	user.irq_iid = params->irq_iid;
	user.device_id = params->device_id;
	user.state = params->state;
	if (copy_to_user(argp, &user, sizeof(user)))
		ret = -EFAULT;

out_free:
	kfree(params);
	return ret;
}

bool kvm_riscv_cove_io_mmio_allowed(struct kvm *kvm, unsigned long gpa,
				    unsigned long len)
{
	struct cove_io_tdi_sbi_params *params;
	struct sbiret sbi_ret;
	bool allowed = false;

	if (!kvm->cmode)
		return true;
	if (!len)
		return false;

	params = kzalloc(sizeof(*params), GFP_ATOMIC);
	if (!params)
		return false;

	params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);
	params->op = KVM_COVE_IO_TDI_FIND_MMIO;
	params->mmio_gpa = gpa;
	params->mmio_size = len;

	sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
			    __pa(params), 0, 0, 0, 0, 0);
	allowed = !sbi_ret.error &&
		  params->state == KVM_COVE_IO_TDI_STATE_STARTED;

	kfree(params);
	return allowed;
}

bool kvm_riscv_cove_io_dma_allowed(struct kvm *kvm, unsigned long device_id,
				   unsigned long gpa)
{
	struct cove_io_tdi_sbi_params *params;
	struct sbiret sbi_ret;
	bool allowed = false;

	if (!kvm->cmode)
		return true;

	params = kzalloc(sizeof(*params), GFP_ATOMIC);
	if (!params)
		return false;

	params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);
	params->op = KVM_COVE_IO_TDI_FIND_DMA;
	params->device_id = device_id;
	params->dma_gpa = gpa;

	/*
	 * OpenSBI owns the authoritative table.  Ask it whether any started TDI
	 * for this VM covers the GPA instead of assuming a fixed TDI id.
	 */
	sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
			    __pa(params), 0, 0, 0, 0, 0);
	allowed = !sbi_ret.error &&
		  params->state == KVM_COVE_IO_TDI_STATE_STARTED;

	kfree(params);
	return allowed;
}

int kvm_riscv_cove_io_iommu_fault_check(u32 devid, unsigned long iova)
{
	struct kvm *kvm = NULL;
	u64 device_id = KVM_COVE_IO_DEVICE_INVALID;
	u16 rid = devid & 0xffff;
	bool allowed;

	if (!kvm_riscv_find_swiotlb_by_pci_rid(rid, &kvm, &device_id))
		return -ENOENT;

	allowed = kvm_riscv_cove_io_dma_allowed(kvm, device_id, iova);
	kvm_put_kvm(kvm);

	return allowed ? 1 : 0;
}
EXPORT_SYMBOL_GPL(kvm_riscv_cove_io_iommu_fault_check);

bool kvm_riscv_cove_io_irq_target_device_allowed(struct kvm *kvm,
						 unsigned int irq,
						 u64 vcpu_id,
						 u64 irq_iid,
						 u64 device_id)
{
	struct cove_io_tdi_sbi_params *params;
	struct sbiret sbi_ret;
	bool allowed = false;

	if (!kvm->cmode)
		return true;

	params = kzalloc(sizeof(*params), GFP_ATOMIC);
	if (!params)
		return false;

	params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);
	params->op = KVM_COVE_IO_TDI_FIND_IRQ;
	params->irq_id = irq;
	params->vcpu_id = vcpu_id;
	params->irq_iid = irq_iid;
	params->device_id = device_id;

	sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
			    __pa(params), 0, 0, 0, 0, 0);
	allowed = !sbi_ret.error &&
		  params->state == KVM_COVE_IO_TDI_STATE_STARTED;

	kfree(params);
	return allowed;
}

bool kvm_riscv_cove_io_irq_target_allowed(struct kvm *kvm, unsigned int irq,
					  u64 vcpu_id, u64 irq_iid)
{
	return kvm_riscv_cove_io_irq_target_device_allowed(kvm, irq,
							   vcpu_id,
							   irq_iid,
							   KVM_COVE_IO_DEVICE_ANY);
}

bool kvm_riscv_cove_io_irq_allowed(struct kvm *kvm, unsigned int irq)
{
	return kvm_riscv_cove_io_irq_target_allowed(kvm, irq,
						    KVM_COVE_IO_VCPU_ANY,
						    KVM_COVE_IO_IRQ_IID_ANY);
}

void kvm_riscv_cove_io_destroy_vm(struct kvm *kvm)
{
	struct cove_io_tdi_sbi_params *params;
	struct sbiret sbi_ret;
	u64 tdi_id;

	if (!kvm->cmode)
		return;

	riscv_iommu_cove_io_mrif_unbind_vm(kvm);

	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!params)
		return;

	params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);

	for (tdi_id = 1; tdi_id <= KVM_COVE_IO_MAX_TDIS; tdi_id++) {
		memset(params, 0, sizeof(*params));
		params->vmid_ptr = (struct kvm_vmid *)__pa(&kvm->arch.vmid);
		params->tdi_id = tdi_id;

		params->op = KVM_COVE_IO_TDI_GET_STATE;
		sbi_ret = sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
				    __pa(params), 0, 0, 0, 0, 0);
		if (sbi_ret.error)
			continue;

		if (params->state == KVM_COVE_IO_TDI_STATE_STARTED) {
			params->op = KVM_COVE_IO_TDI_STOP;
			sbi_ret = sbi_ecall(SBI_EXT_CVM,
					    SBI_EXT_CVM_COVE_IO_TDI_OP,
					    __pa(params), 0, 0, 0, 0, 0);
			if (sbi_ret.error)
				continue;
			params->state = KVM_COVE_IO_TDI_STATE_STOPPING;
		}

		if (params->state == KVM_COVE_IO_TDI_STATE_BOUND ||
		    params->state == KVM_COVE_IO_TDI_STATE_STOPPING) {
			params->op = KVM_COVE_IO_TDI_IRQ_UNBIND;
			params->irq_id = 0;
			params->irq_num = 0;
			sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
				  __pa(params), 0, 0, 0, 0, 0);

			params->op = KVM_COVE_IO_TDI_DMA_UNMAP;
			sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
				  __pa(params), 0, 0, 0, 0, 0);

			params->op = KVM_COVE_IO_TDI_RECLAIM_MMIO;
			sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
				  __pa(params), 0, 0, 0, 0, 0);

			params->op = KVM_COVE_IO_TDI_UNBIND;
			sbi_ecall(SBI_EXT_CVM, SBI_EXT_CVM_COVE_IO_TDI_OP,
				  __pa(params), 0, 0, 0, 0, 0);
		}
	}

	kfree(params);
}

int kvm_riscv_destroy_sw_node(struct kvm *kvm)
{
	struct swiotlb_node **link, *node;

	spin_lock(&sw_list);
	link = &sw_list_head;
	while (*link) {
		node = *link;
		if (node->vmid == &kvm->arch.vmid.vmid) {
			*link = node->next;
			kfree(node);
			continue;
		}
		link = &node->next;
	}
	spin_unlock(&sw_list);

	return 0;
}
