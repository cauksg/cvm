
#ifndef __IIE_CVM_SBI_H__
#define __IIE_CVM_SBI_H__

// #include <sbi/sbi_types.h>
#include <linux/kvm_host.h>

#define SBI_EXT_CVM				0x20000217
#define SBI_EXT_CVM_CREATE 					0x0
#define SBI_EXT_CVM_CREATE_MEMORY_REGION	0x1			
#define SBI_EXT_CVM_MEASURED_PAGES			0x2	
#define SBI_EXT_CVM_CREATE_VCPU				0x3	
#define SBI_EXT_CVM_FINALIZE				0x4
#define SBI_EXT_CVM_INIT_MEM_POOL			0x5
#define SBI_EXT_CVM_RUN_VCPU				0x6


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
	pgd_t *pgd;
	phys_addr_t *pgd_phys;
	// struct kvm_vcpu *vcpu;
	// struct iie_cvm_vcpu_sbi_params cvm_vcpu_sbi_params;

};


int create_sbi_param(struct kvm *kvm, struct iie_cvm_sbi_params * cvm_sbi_params);

#endif