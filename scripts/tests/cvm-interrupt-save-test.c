/* Reproduce sched-out after consuming the virtual timer's pending mask. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

enum {
	CSR_VSSTATUS, CSR_VSIE, CSR_VSTVEC, CSR_VSSCRATCH, CSR_VSEPC,
	CSR_VSCAUSE, CSR_VSTVAL, CSR_HVIP, CSR_VSATP, NR_CSRS,
};
struct kvm_vcpu_csr {
	unsigned long vsstatus, vsie, vstvec, vsscratch, vsepc;
	unsigned long vscause, vstval, hvip, vsatp;
};
struct kvm { bool cmode; };
struct kvm_vcpu {
	struct kvm *kvm;
	int cpu;
	struct {
		struct kvm_vcpu_csr guest_csr;
		unsigned long irqs_pending[1], irqs_pending_mask[1];
		int guest_context, host_context, isa;
	} arch;
};
static unsigned long hardware[NR_CSRS];
static bool nacl;
#define READ_ONCE(v) (v)
static unsigned long exchange(unsigned long *p, unsigned long v)
{
	unsigned long old = *p;
	*p = v;
	return old;
}
#define xchg_acquire(p, v) exchange(p, v)
static void kvm_riscv_vcpu_aia_flush_interrupts(struct kvm_vcpu *v) { (void)v; }
static void kvm_riscv_vcpu_aia_put(struct kvm_vcpu *v) { (void)v; }
static void kvm_riscv_vcpu_timer_save(struct kvm_vcpu *v) { (void)v; }
static void kvm_riscv_vcpu_guest_fp_save(int *ctx, int isa) { (void)ctx; (void)isa; }
static void kvm_riscv_vcpu_host_fp_restore(int *ctx) { (void)ctx; }
static void kvm_riscv_vcpu_guest_vector_save(int *ctx, int isa) { (void)ctx; (void)isa; }
static void kvm_riscv_vcpu_host_vector_restore(int *ctx) { (void)ctx; }
static bool kvm_riscv_nacl_available(void) { return nacl; }
static void *nacl_shmem(void) { return hardware; }
static unsigned long csr_read(int n) { return hardware[n]; }
static unsigned long nacl_csr_read(void *p, int n) { return ((unsigned long *)p)[n]; }
#include "flush.inc"
#include "put.inc"

int main(void)
{
	struct kvm kvm = { .cmode = true };
	struct kvm_vcpu vcpu = { .kvm = &kvm };
	struct kvm_vcpu_csr saved;
	const unsigned long vstip = 1UL << 6;

	/* Monitor last saved HVIP=0. The hrtimer then marks VSTIP pending. */
	vcpu.arch.irqs_pending[0] = vstip;
	vcpu.arch.irqs_pending_mask[0] = vstip;
	kvm_riscv_vcpu_flush_interrupts(&vcpu);
	assert(vcpu.arch.guest_csr.hvip == vstip);
	assert(!vcpu.arch.irqs_pending_mask[0]);
	assert(!hardware[CSR_HVIP]); /* Monitor has not loaded it yet. */
	vcpu.arch.guest_csr.vsepc = 0x1234; /* Emulated trap may change VS CSRs too. */
	saved = vcpu.arch.guest_csr;
	kvm_arch_vcpu_put(&vcpu); /* Preemption in the entry recheck path. */
	assert(!memcmp(&saved, &vcpu.arch.guest_csr, sizeof(saved)));
	assert(vcpu.arch.guest_csr.hvip == vstip);
	assert(vcpu.arch.irqs_pending[0] == vstip);

	/* Normal VMs still save the live CSR bank, with and without NaCl. */
	kvm.cmode = false;
	for (int mode = 0; mode < 2; mode++) {
		nacl = mode;
		for (int i = 0; i < NR_CSRS; i++)
			hardware[i] = 0x100UL * (mode + 1) + i;
		kvm_arch_vcpu_put(&vcpu);
		assert(vcpu.arch.guest_csr.hvip == hardware[CSR_HVIP]);
		assert(vcpu.arch.guest_csr.vsepc == hardware[CSR_VSEPC]);
		assert(vcpu.arch.guest_csr.vsatp == hardware[CSR_VSATP]);
	}
	puts("PASS: CVM sched-out preserves flushed VSTIP and emulated VS state; normal/NaCl saves retained");
	return 0;
}
