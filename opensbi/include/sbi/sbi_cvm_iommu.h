/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef __SBI_CVM_IOMMU_H__
#define __SBI_CVM_IOMMU_H__
#include <sbi/sbi_types.h>
/* Experimental fixed-topology platform ABI; not a CoVE standard SBI. */
#define CVM_IOMMU_PROBE 0
#define CVM_IOMMU_HOST_CONTEXT 1
#define CVM_IOMMU_SYNC 2
#define CVM_IOMMU_MSI 3
#define CVM_IOMMU_PCI_READ 4
#define CVM_IOMMU_PCI_WRITE 5
#define CVM_IOMMU_STATUS 6
#define CVM_IOMMU_CAPABILITIES 7
#define CVM_IOMMU_BIND_FILE 8
#define CVM_IOMMU_MAX_RIDS 64
#define CVM_IOMMU_REG_BASE 0x03010000UL
#define CVM_IOMMU_ECAM_BASE 0x30000000UL
#define CVM_IOMMU_POOL_BASE 0xbf000000UL
#define CVM_IOMMU_POOL_SIZE 0x01000000UL
int sbi_cvm_iommu_platform_init(const void *fdt);
bool sbi_cvm_iommu_active(void);
void *sbi_cvm_iommu_alloc(unsigned long size);
void sbi_cvm_iommu_guest_context(unsigned long owner);
int sbi_cvm_iommu_file_reserve(unsigned long file, unsigned long owner);
int sbi_cvm_iommu_file_release(unsigned long file, unsigned long owner);
int sbi_cvm_iommu_set_owner(unsigned long rid, unsigned long file, unsigned long owner);
bool sbi_cvm_iommu_irq_reserved(unsigned long owner, unsigned long file,
                                unsigned long iid);
bool sbi_cvm_iommu_host_buffer(unsigned long pa, unsigned long size);
int sbi_cvm_iommu_ecall(unsigned long op, unsigned long a,
                       unsigned long b, unsigned long c, unsigned long d,
                       unsigned long *value);
int sbi_cvm_iommu_protect(unsigned long pa, bool protect);
int sbi_cvm_iommu_claim(unsigned long rid);
int sbi_cvm_iommu_map(unsigned long rid, unsigned long gpa, unsigned long hpa);
int sbi_cvm_iommu_msi_install(unsigned long rid, unsigned long addr,
                             unsigned long target, unsigned long iid);
int sbi_cvm_iommu_start(unsigned long rid);
int sbi_cvm_iommu_stop(unsigned long rid);
int sbi_cvm_iommu_release(unsigned long rid);
#endif
