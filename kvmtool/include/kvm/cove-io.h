#ifndef KVM__COVE_IO_H
#define KVM__COVE_IO_H

#include <stdbool.h>
#include <linux/kvm.h>
#include <linux/types.h>

struct kvm;

#define COVE_IO_MAX_TDIS		KVM_COVE_IO_MAX_TDIS
#define COVE_IO_DEFAULT_TDI_ID		1
#define COVE_IO_PCI_TDI_ID		2
#define COVE_IO_FIRST_DEVICE_TDI_ID	3
#define COVE_IO_DEVICE_TEST_WRONG	KVM_COVE_IO_DEVICE_INVALID
#define COVE_IO_IOMMU_GROUP_NONE	KVM_COVE_IO_IOMMU_GROUP_INVALID
#define COVE_IO_DEVICE_TEST_UNKNOWN_TYPE	0xfe
#define COVE_IO_IRQ_IID_ANY		KVM_COVE_IO_IRQ_IID_ANY
#define COVE_IO_SIM_TEST_MMIO_GPA	0x2ff00000ULL
#define COVE_IO_SIM_TEST_MMIO_SIZE	0x1000ULL

static inline u64 cove_io__device_id(u64 type, u64 payload)
{
	return (type << KVM_COVE_IO_DEVICE_TYPE_SHIFT) |
	       (payload & ~KVM_COVE_IO_DEVICE_TYPE_MASK);
}

static inline u64 cove_io__virtio_mmio_device_id(u64 mmio_addr)
{
	return cove_io__device_id(KVM_COVE_IO_DEVICE_TYPE_VIRTIO_MMIO,
				  mmio_addr);
}

static inline u16 cove_io__pci_requester_id(u64 bus, u64 dev, u64 fn)
{
	return ((bus & 0xff) << 8) | ((dev & 0x1f) << 3) | (fn & 0x7);
}

static inline u64 cove_io__pci_device_id(u64 domain, u64 bus, u64 dev, u64 fn)
{
	u64 payload = ((domain & 0xffff) << 16) |
		      cove_io__pci_requester_id(bus, dev, fn);

	return cove_io__device_id(KVM_COVE_IO_DEVICE_TYPE_PCI_RID, payload);
}

static inline u64 cove_io__pci_device_id_from_rid(u64 segment, u64 rid)
{
	u64 payload = ((segment & 0xffff) << 16) | (rid & 0xffff);

	return cove_io__device_id(KVM_COVE_IO_DEVICE_TYPE_PCI_RID, payload);
}

static inline u16 cove_io__pci_device_id_segment(u64 device_id)
{
	return (device_id >> 16) & 0xffff;
}

static inline u16 cove_io__pci_device_id_requester_id(u64 device_id)
{
	return device_id & 0xffff;
}

bool cove_io__test_skip(const char *name);
bool cove_io__test_probe_enabled(void);
bool cove_io__test_bad_swiotlb_device(const char *name);
bool cove_io__test_bad_swiotlb_rid(const char *name);
bool cove_io__test_bad_swiotlb_iommu_group(const char *name);
bool cove_io__test_bad_tdi_rid(const char *name);
bool cove_io__test_bad_iommu_group(const char *name);
unsigned int cove_io__test_pause_before_run_secs(void);
unsigned int cove_io__test_mrif_retarget_loops(void);
bool cove_io__direct_dma_enabled(void);
bool cove_io__lifecycle_test_enabled(void);

void cove_io__tdi_op(struct kvm *kvm, struct kvm_cove_io_tdi *tdi);
bool cove_io__probe_tdi_op(struct kvm *kvm, struct kvm_cove_io_tdi *tdi);
void cove_io__expect_probe(struct kvm *kvm, struct kvm_cove_io_tdi tdi,
			   bool expect_allowed, const char *name);

u64 cove_io__start_tdi(struct kvm *kvm, u64 tdi_id, u64 mmio_gpa,
		       u64 mmio_size, u64 device_id, u64 dma_gpa,
		       u64 dma_size, u64 iommu_group, u64 irq_id,
		       u64 irq_num);
void cove_io__bind_irq(struct kvm *kvm, u64 tdi_id, u64 irq_id,
		       u64 irq_num);
void cove_io__bind_irq_target(struct kvm *kvm, u64 tdi_id, u64 irq_id,
			      u64 irq_num, u64 vcpu_id);
void cove_io__bind_irq_target_iid(struct kvm *kvm, u64 tdi_id, u64 irq_id,
				  u64 irq_num, u64 vcpu_id, u64 irq_iid);
void cove_io__bind_irq_target_iid_device(struct kvm *kvm, u64 tdi_id,
					 u64 irq_id, u64 irq_num,
					 u64 vcpu_id, u64 irq_iid,
					 u64 device_id);
void cove_io__unbind_irq(struct kvm *kvm, u64 tdi_id);
void cove_io__unbind_irq_range(struct kvm *kvm, u64 tdi_id, u64 irq_id,
			       u64 irq_num);
bool cove_io__try_reclaim_mmio(struct kvm *kvm, u64 tdi_id);
bool cove_io__try_unmap_dma(struct kvm *kvm, u64 tdi_id);
void cove_io__teardown_tdi(struct kvm *kvm, u64 tdi_id);
void cove_io__teardown_all(struct kvm *kvm);

#endif /* KVM__COVE_IO_H */
