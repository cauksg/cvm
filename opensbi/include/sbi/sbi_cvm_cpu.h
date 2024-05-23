#ifndef __SBI_CVM_CPU_H__
#define __SBI_CVM_CPU_H__

#include <sbi/sbi_types.h>
#include <stdint.h>

#define MAX_HARTS 32

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
};

struct cpu_trap {
	unsigned long sepc;
	unsigned long scause;
	unsigned long stval;
	unsigned long htval;
	unsigned long htinst;
};

struct cvm_vcpu {
	int *vcpu_id;
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
	unsigned long host_hgatp;
	unsigned long guest_mie;
	unsigned long guest_mstatus;
	unsigned long guest_mideleg;
	unsigned long guest_medeleg;
	unsigned long guest_hgatp;
	struct cpu_context host_context;
	struct cpu_context guest_context;
	struct vcpu_csr guest_csr;
	struct cpu_trap* kvm_vcpu_trap;
	struct cpu_context* kvm_vcpu_context;
	struct sbi_cvm *cvm;
	//TODO: delete it because root_pt is for each cvm rather than each vcpu
	unsigned long root_pt;
	struct vcpu_csr* kvm_vcpu_csr;
};

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