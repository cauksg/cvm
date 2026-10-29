#ifndef __SBI_CVM_CPU_H__
#define __SBI_CVM_CPU_H__

#include <sbi/sbi_types.h>
#include <stdint.h>

#ifndef MAX_HARTS
#define MAX_HARTS 32
#endif

struct cpu_context
{
	unsigned long zero;
	unsigned long ra;
	unsigned long sp;
	unsigned long gp;
	unsigned long tp;
	unsigned long t0;
	unsigned long t1;
	unsigned long t2;
	unsigned long s0;
	unsigned long s1;
	unsigned long a0;
	unsigned long a1;
	unsigned long a2;
	unsigned long a3;
	unsigned long a4;
	unsigned long a5;
	unsigned long a6;
	unsigned long a7;
	unsigned long s2;
	unsigned long s3;
	unsigned long s4;
	unsigned long s5;
	unsigned long s6;
	unsigned long s7;
	unsigned long s8;
	unsigned long s9;
	unsigned long s10;
	unsigned long s11;
	unsigned long t3;
	unsigned long t4;
	unsigned long t5;
	unsigned long t6;
	unsigned long sepc;
	unsigned long sstatus;
	unsigned long hstatus;
};

struct vcpu_csr {
	unsigned long vsstatus;
	unsigned long vsie;
	unsigned long vstvec;
	unsigned long vsscratch;
	unsigned long vsepc;
	unsigned long vscause;
	unsigned long vstval;
	unsigned long hvip;
	unsigned long vsatp;
	unsigned long scounteren;
	unsigned long senvcfg;
	unsigned long vstimecmp;
};

struct cpu_trap {
	unsigned long sepc;
	unsigned long scause;
	unsigned long stval;
	unsigned long htval;
	unsigned long htinst;
};

/* The Host sees a zero-based PC token, never the private resume address. */
enum cvm_resume_kind {
	CVM_RESUME_RETRY,
	CVM_RESUME_SBI,
	CVM_RESUME_MMIO,
	CVM_RESUME_WFI,
	CVM_RESUME_NOTIFY,
};

struct cvm_fp_context {
	uint64_t f[32];
	unsigned long fcsr;
};

#define CVM_MAX_VCPUS 4
#define CVM_IMSIC_IDS 256
struct cvm_imsic_context {
 unsigned long gpa, file, hart;
 unsigned long pending[4], enabled[4], allowed[4];
 unsigned long delivery, threshold;
 bool configured, unbinding;
};

struct cvm_vcpu {
 struct cvm_imsic_context imsic;
 bool hart_started;
	int *vcpu_id;
	/* Trusted identity never follows later Host writes to the shared ID. */
	int trusted_vcpu_id;
	bool state_finalized;
	bool state_failed;
	enum cvm_resume_kind resume_kind;
	unsigned int resume_len;
	uint32_t resume_in_mask;
	uint32_t resume_out_mask;
	unsigned int mmio_width;
	bool mmio_signed;
	unsigned long pending_share_gpa;
	struct vcpu_csr host_vs_csr;
	struct cvm_fp_context host_fp, guest_fp;
	unsigned long host_henvcfg;
	unsigned long host_hcounteren;
	unsigned long host_hstateen0;
	unsigned long host_hvictl, host_hviprio1, host_hviprio2;
	unsigned long host_hvien, host_htimedelta;
	unsigned long host_viselect, guest_viselect;
	/* SSCRATCH, STVEC, and SCOUNTEREN of Host */
	unsigned long host_sscratch;
	unsigned long host_stvec;
	unsigned long host_scounteren;
	unsigned long host_senvcfg;
	unsigned long host_sstateen0;
	unsigned long host_mepc;
	unsigned long host_mie;
	unsigned long host_mstatus;
	unsigned long host_mideleg;
	unsigned long host_medeleg;
	bool io_trap_guard_active;
	unsigned long host_hedeleg;
	unsigned long host_hideleg;
	unsigned long host_hgatp;
	unsigned long guest_mie;
	unsigned long guest_mstatus;
	unsigned long guest_mideleg;
	unsigned long guest_medeleg;
	unsigned long guest_hgatp;
	unsigned long cxt_tag;
	struct cpu_context host_context;
	struct cpu_context guest_context;
	struct vcpu_csr guest_csr;
	struct cpu_trap* kvm_vcpu_trap;
	struct cpu_context* kvm_vcpu_context;
	struct sbi_cvm *cvm;
	//TODO: delete it because root_pt is for each cvm rather than each vcpu
	unsigned long root_pt;
	struct vcpu_csr* kvm_vcpu_csr;
	unsigned long exit_reason;
};

int sbi_cvm_state_resume(struct cvm_vcpu *vcpu,
			 const struct cpu_context *response);
void sbi_cvm_state_export(struct cvm_vcpu *vcpu,
			  struct cpu_context *context, struct vcpu_csr *csr);
int sbi_cvm_state_sbi_args(unsigned long ext, unsigned long fid);
int sbi_cvm_state_mmio(struct cvm_vcpu *vcpu, struct cpu_trap *trap,
		       unsigned long instruction);
void sbi_cvm_fp_save(struct cvm_fp_context *context);
void sbi_cvm_fp_restore(const struct cvm_fp_context *context);

typedef struct
{
	uint32_t cmode;
	uint32_t cvmid;
	uint32_t vcpuid;
}cpu_state_t;

extern cpu_state_t cpu_s[MAX_HARTS];

struct cvm_vcpu_node {
	struct cvm_vcpu vcpu;
	struct cvm_vcpu_node *next;
};

#endif
