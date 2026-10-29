/* Exercise actual IOMMU routines with deterministic hardware completion. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
#define IMSIC_MMIO_PAGE_SZ 4096
#define IMSIC_MMIO_PAGE_SHIFT 12
#define RISCV_IOMMU_MSIPTE_V 1
#define RISCV_IOMMU_MSIPTE_M 6
#define RISCV_IOMMU_MSIPTE_M_BASIC 3
#define RISCV_IOMMU_CAPABILITIES_MSI_FLAT 1
#define KVM_COVE_IO_VCPU_ANY UINT64_MAX
#define KVM_COVE_IO_IRQ_IID_ANY UINT64_MAX
#define FIELD_PREP(mask, v) ((u64)(v) << 1)
#define IS_ALIGNED(v, n) (!((v) & ((n)-1)))
#define WRITE_ONCE(dst, v) ((dst) = (v))
#define phys_to_ppn(pa) (((pa) >> 12) << 10)
#define dma_wmb() ((void)0)
#define dev_warn(...) ((void)0)
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
struct kvm { int refs, pins; };
struct riscv_iommu_msipte { u64 pte, mrif_info; };
struct kvm_riscv_aia_imsic_info { u64 vsfile_pa, imsic_addr; unsigned int nr_msis; };
struct riscv_iommu_device { u64 caps; bool cove_io_isolated_msi; };
struct riscv_iommu_info {
 struct riscv_iommu_msipte *cove_io_msipt;
 u64 cove_io_msi_entries, cove_io_msi_addr_mask;
 bool cove_io_imsic_active, cove_io_dma_blocked, cove_io_isolated_msi;
 struct kvm *cove_io_owner, *cove_io_imsic_kvm;
 u64 cove_io_imsic_vcpu_id, cove_io_imsic_irq_iid;
};
struct device { int unused; };
struct riscv_iommu_cove_io_imsic_refresh { struct kvm *kvm; u64 vcpu_id; };
static bool cove_io_imsic = true;
static int riscv_iommu_cove_io_msi_lock;
static struct device device;
static struct riscv_iommu_device iommu;
static struct riscv_iommu_info info;
static struct riscv_iommu_msipte table[4];
static struct kvm vm, other;
static int locked, sync_calls, fail_sync, clear_calls, target_queries;
static int target_error;
static u64 cleared_iid, cleared_vcpu;
static void mutex_lock(int *p) { (void)p; assert(!locked); locked=1; }
static void mutex_unlock(int *p) { (void)p; assert(locked); locked=0; }
static struct device *riscv_iommu_cove_io_find_pci_device(u64 id) { return id == 1 ? &device : NULL; }
#define dev_to_iommu(d) model_iommu(d)
static struct riscv_iommu_device *model_iommu(struct device *d) { (void)d; return &iommu; }
#define dev_iommu_priv_get(d) model_info(d)
static struct riscv_iommu_info *model_info(struct device *d) { (void)d; return &info; }
#define to_pci_dev(d) (d)
static void pci_dev_put(struct device *d) { (void)d; }
static void kvm_get_kvm(struct kvm *k) { k->refs++; }
static void kvm_put_kvm(struct kvm *k) { assert(k->refs); k->refs--; }
static void riscv_iommu_cove_io_init_msi_remap(struct riscv_iommu_device *i) { (void)i; }
static int kvm_riscv_aia_imsic_direct_enable(struct kvm *k, u64 v) { if (v>3) return -ENODEV; k->pins++; return 0; }
static void kvm_riscv_aia_imsic_direct_disable(struct kvm *k, u64 v) { (void)v; assert(k->pins); k->pins--; }
static int kvm_riscv_aia_imsic_direct_info(struct kvm *k, u64 v, u64 iid, struct kvm_riscv_aia_imsic_info *t)
{
 (void)k; (void)v; assert(locked); target_queries++;
 if (!iid || iid>2047) return -EINVAL;
 if (target_error) return target_error;
 *t=(struct kvm_riscv_aia_imsic_info){.imsic_addr=0x28000000,.vsfile_pa=0x28001000};
 return 0;
}
static int kvm_riscv_aia_imsic_direct_clear_pending(struct kvm *k, u64 v, unsigned int iid)
{
 (void)k; assert(locked); assert(!table[0].pte && !table[3].pte);
 assert(sync_calls); clear_calls++; cleared_iid=iid; cleared_vcpu=v; return 0;
}
static u64 riscv_iommu_cove_io_pext(u64 v, u64 mask)
{
 u64 n=0, out=1;
 for (; mask; mask&=mask-1,out<<=1) if (v & (mask & -mask)) n|=out;
 return n;
}
static int riscv_iommu_cove_io_alloc_device_msipt(struct riscv_iommu_device *i, struct riscv_iommu_info *p)
{ (void)i; p->cove_io_msipt=table; p->cove_io_msi_entries=4; return 0; }
static void riscv_iommu_cove_io_block_device_msipt(struct riscv_iommu_device *i, struct riscv_iommu_info *p)
{ (void)i; (void)p; memset(table,0,sizeof(table)); }
static int riscv_iommu_cove_io_update_msipt_context(struct riscv_iommu_device *i, struct device *d)
{ (void)i; (void)d; assert(locked); sync_calls++; return fail_sync==sync_calls ? -ETIMEDOUT : 0; }
#include "imsic-driver.inc"
static void reset(void)
{
 memset(&info,0,sizeof(info)); memset(table,0,sizeof(table));
 vm=(struct kvm){0}; other=(struct kvm){0};
 iommu=(struct riscv_iommu_device){.caps=1,.cove_io_isolated_msi=true};
 info.cove_io_owner=&vm; info.cove_io_msipt=table; info.cove_io_msi_entries=4;
 sync_calls=fail_sync=clear_calls=target_queries=target_error=locked=0;
}
static int unbind(void)
{ int ret; mutex_lock(&riscv_iommu_cove_io_msi_lock); ret=riscv_iommu_cove_io_deactivate_imsic(&device); mutex_unlock(&riscv_iommu_cove_io_msi_lock); return ret; }
int main(void)
{
 struct kvm_riscv_aia_imsic_info t={.imsic_addr=0x28001000,.vsfile_pa=0x28007000};
 struct riscv_iommu_cove_io_imsic_refresh refresh={.kvm=&vm,.vcpu_id=0};
 reset(); info.cove_io_msi_addr_mask=3;
 assert(riscv_iommu_cove_io_install_imsic_pte(&iommu,&info,&t)==1);
 assert(table[1].pte==(phys_to_ppn(t.vsfile_pa)|7)); assert(!table[1].mrif_info);
 assert(!table[0].pte && !table[2].pte && !table[3].pte);
 t.vsfile_pa++; assert(riscv_iommu_cove_io_install_imsic_pte(&iommu,&info,&t)==-EINVAL);
 reset(); assert(!riscv_iommu_cove_io_imsic_bind(1,&vm,0,2));
 assert(vm.refs==1 && vm.pins==1 && info.cove_io_imsic_active);
 assert(table[0].pte==(phys_to_ppn(0x28001000)|7));
 assert(!riscv_iommu_cove_io_imsic_bind(1,&vm,0,2)); assert(vm.refs==1 && vm.pins==1);
 assert(!riscv_iommu_cove_io_imsic_bind(1,&vm,1,7));
 assert(cleared_vcpu==0 && cleared_iid==2); assert(vm.refs==1 && vm.pins==1);
 assert(!unbind()); assert(!vm.refs && !vm.pins && !info.cove_io_imsic_active);
 for (int n=0;n<4;n++) assert(!table[n].pte);
 int before=target_queries; assert(!riscv_iommu_cove_io_imsic_refresh_cb(&device,&refresh));
 assert(target_queries==before && !table[0].pte);
 reset(); target_error=-EAGAIN; assert(!riscv_iommu_cove_io_imsic_bind(1,&vm,0,2));
 assert(!table[0].pte && vm.pins==1); target_error=0;
 assert(!riscv_iommu_cove_io_imsic_refresh_cb(&device,&refresh)); assert(table[0].pte);
 info.cove_io_dma_blocked=true; before=target_queries;
 assert(!riscv_iommu_cove_io_imsic_refresh_cb(&device,&refresh)); assert(before==target_queries);
 info.cove_io_dma_blocked=false; info.cove_io_owner=&other; before=target_queries;
 assert(!riscv_iommu_cove_io_imsic_refresh_cb(&device,&refresh)); assert(before==target_queries);
 info.cove_io_owner=&vm; fail_sync=sync_calls+1;
 assert(unbind()==-ETIMEDOUT); assert(vm.refs==1 && vm.pins==1 && info.cove_io_imsic_active);
 assert(info.cove_io_dma_blocked && !table[0].pte); fail_sync=0; assert(!unbind());
 reset(); fail_sync=2; assert(riscv_iommu_cove_io_imsic_bind(1,&vm,0,2)==-ETIMEDOUT);
 assert(vm.refs==1 && vm.pins==1 && info.cove_io_imsic_active && info.cove_io_dma_blocked);
 fail_sync=0; assert(!unbind()); assert(!vm.refs && !vm.pins);
 reset(); info.cove_io_owner=&other; assert(riscv_iommu_cove_io_imsic_bind(1,&vm,0,2)==-EPERM);
 assert(!vm.pins && !vm.refs);
 reset(); cove_io_imsic=false; assert(riscv_iommu_cove_io_imsic_bind(1,&vm,0,2)==-EOPNOTSUPP);
 cove_io_imsic=true;
 reset(); assert(riscv_iommu_cove_io_imsic_bind(1,&vm,4096,2)==-ENODEV);
 assert(!vm.pins && !vm.refs);
 puts("PASS: IMSIC BASIC target, exact refs, retarget drain, pending blocking, stale refresh and fence-failure retention");
}
