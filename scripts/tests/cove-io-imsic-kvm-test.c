/* Run actual KVM lifecycle decisions against a minimal hardware-file model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint64_t phys_addr_t;
#define __iomem
#define IMSIC_MMIO_PAGE_SZ 4096
#define KVM_DEV_RISCV_AIA_MODE_EMUL 0
#define KVM_DEV_RISCV_AIA_MODE_AUTO 1
#define KVM_DEV_RISCV_AIA_MODE_HWACCEL 2
#define KVM_EXIT_FAIL_ENTRY_NO_VSFILE 1
#define KVM_EXIT_FAIL_ENTRY 9
#define HSTATUS_VGEIN (0x3fUL << 12)
#define HSTATUS_VGEIN_SHIFT 12
#define BITS_PER_LONG 64
#define BIT(n) (1UL << (n))
#define CSR_HSTATUS 0
#define CSR_VSISELECT 1
#define CSR_VSIREG 2
#define CSR_HGEIE 3
#define IMSIC_EIP0 0x80
struct imsic_mrif { unsigned long pending; };
struct imsic {
 int vsfile_lock, direct_users, vsfile_cpu, vsfile_hgei;
 bool direct_transition, trusted_file;
 void *vsfile_va;
 phys_addr_t vsfile_pa;
 u32 nr_hw_eix;
};
struct kvm_run {
 int exit_reason;
 struct { u64 hardware_entry_failure_reason; int cpu; } fail_entry;
};
struct kvm_vcpu_aia { struct imsic *imsic_state; u64 imsic_addr; };
struct kvm { bool initialized, cmode; struct { struct { int mode; } aia; struct { unsigned long cvm_id; } vmid; } arch; struct kvm_vcpu *vcpu; };
struct kvm_vcpu {
 struct kvm *kvm;
 struct kvm_run *run;
 int cpu;
 unsigned long vcpu_id;
 struct { struct kvm_vcpu_aia aia_context; struct { unsigned long hstatus; } guest_context; } arch;
};
struct imsic_direct_clear { int hgei; u32 iid; };
static int lock, allocations, frees, copies, maps, alloc_result=1, bind_error;
static int reclaim_error, quarantines;
static unsigned long csrs[4], pending[32];
static struct kvm_vcpu *kvm_get_vcpu_by_id(struct kvm *k, u64 id) { return id ? NULL : k->vcpu; }
#define kvm_riscv_aia_initialized(k) ((k)->initialized)
#define atomic_read(p) (*(p))
#define atomic_inc(p) (++*(p))
#define write_lock_irqsave(p,f) do { (void)(p); (f)=0; assert(!lock); lock=1; } while (0)
#define write_unlock_irqrestore(p,f) do { (void)(p); (void)(f); assert(lock); lock=0; } while (0)
static unsigned long csr_read(int n) { return csrs[n]; }
static void csr_write(int n, unsigned long val) { csrs[n]=val; }
static void csr_clear(int n, unsigned long mask)
{ if (n==CSR_VSIREG) pending[csrs[CSR_VSISELECT]-IMSIC_EIP0]&=~mask; else csrs[n]&=~mask; }
static void kvm_riscv_mmu_iounmap(struct kvm *k,u64 a,int size) { (void)k;(void)a;(void)size; maps--; }
static int kvm_riscv_mmu_ioremap(struct kvm *k,u64 a,u64 b,int size,bool r,bool w)
{ (void)k;(void)a;(void)b;(void)size;(void)r;(void)w; maps++; return 0; }
static void imsic_vsfile_read(int i,int c,u32 n,bool clear,struct imsic_mrif *s)
{ (void)i;(void)c;(void)n;(void)clear;s->pending=7;copies++; }
static void imsic_swfile_update(struct kvm_vcpu *v,struct imsic_mrif *s) { (void)v;(void)s; }
static void kvm_riscv_aia_free_hgei(int c,int i) { (void)c;(void)i;frees++; }
static void kvm_riscv_aia_quarantine_hgei(int c,int i) { (void)c;(void)i;quarantines++; }
static int kvm_riscv_aia_alloc_hgei(int c,struct kvm_vcpu *v,void **va,phys_addr_t *pa)
{ (void)c;(void)v; allocations++; *va=(void *)(uintptr_t)0x1000; *pa=0x28001000; return alloc_result; }
static void imsic_vsfile_local_clear(int i,u32 n) { (void)i;(void)n; }
static void imsic_swfile_read(struct kvm_vcpu *v,bool c,struct imsic_mrif *s) { (void)v;(void)c;s->pending=7; }
static void imsic_vsfile_local_update(int i,u32 n,struct imsic_mrif *s) { (void)i;(void)n;(void)s; }
static void imsic_direct_queue_refresh(struct kvm_vcpu *v) { (void)v; }
#define SBI_EXT_COVI 3
#define SBI_EXT_COVH 4
#define SBI_EXT_CVM 1
#define SBI_EXT_CVM_TRUSTED_IOMMU 2
#define __pa(p) ((unsigned long)(p))
struct sbiret { long error, value; };
static struct sbiret sbi_ecall(unsigned long e,unsigned long f,unsigned long op,
 unsigned long a,unsigned long b,unsigned long c,unsigned long d,unsigned long x)
{ (void)e;(void)f;(void)op;(void)a;(void)b;(void)c;(void)d;(void)x; return (struct sbiret){e == SBI_EXT_COVI && f == 4 ? bind_error : e == SBI_EXT_COVI && f == 3 ? reclaim_error : 0,0}; }
static int sbi_err_map_linux_errno(long err) { return (int)err; }
#include "imsic-kvm.inc"
int main(void)
{
 struct imsic imsic={.vsfile_cpu=0,.vsfile_hgei=1,.vsfile_pa=0x28001000,.direct_users=1};
 struct kvm_run run={0};
 struct kvm k={.initialized=true,.arch.aia.mode=KVM_DEV_RISCV_AIA_MODE_AUTO};
 struct kvm_vcpu v={.kvm=&k,.run=&run,.cpu=0,.arch.aia_context.imsic_state=&imsic};
 k.vcpu=&v;
 kvm_riscv_vcpu_aia_imsic_release(&v);
 assert(imsic.vsfile_cpu==0 && !frees && !copies);
 assert(kvm_riscv_vcpu_aia_imsic_update(&v)==1 && !allocations);
 v.cpu=1; assert(kvm_riscv_vcpu_aia_imsic_update(&v)==0);
 assert(run.exit_reason==KVM_EXIT_FAIL_ENTRY && !allocations && !frees);
 imsic.direct_users=0; imsic.trusted_file=true;
 kvm_riscv_vcpu_aia_imsic_release(&v);
 assert(imsic.vsfile_cpu==0 && !frees && !copies);
 imsic.trusted_file=false;
 kvm_riscv_vcpu_aia_imsic_release(&v);
 assert(imsic.vsfile_cpu==-1 && frees==1 && copies==1 && !imsic.direct_transition);
 imsic.direct_transition=true;
 assert(kvm_riscv_aia_imsic_direct_enable(&k,0)==-EAGAIN && !imsic.direct_users);
 imsic.direct_transition=false;
 assert(!kvm_riscv_aia_imsic_direct_enable(&k,0) && imsic.direct_users==1);
 alloc_result=-ENOSPC;
 assert(kvm_riscv_vcpu_aia_imsic_update(&v)==0);
 assert(run.exit_reason==KVM_EXIT_FAIL_ENTRY && imsic.vsfile_cpu==-1 && !imsic.direct_transition);
 imsic.direct_users=0; k.arch.aia.mode=KVM_DEV_RISCV_AIA_MODE_EMUL;
 assert(kvm_riscv_aia_imsic_direct_enable(&k,0)==-EOPNOTSUPP);
 assert(kvm_riscv_aia_imsic_direct_enable(&k,7)==-ENODEV);
 csrs[CSR_HSTATUS]=0x123;csrs[CSR_VSISELECT]=0xabc;
 pending[0]=BIT(2)|BIT(7);pending[2]=BIT(1)|BIT(9);
 struct imsic_direct_clear clear={.hgei=1,.iid=2};
 imsic_direct_clear_local(&clear);
 assert(pending[0]==BIT(7) && pending[2]==(BIT(1)|BIT(9)));
 clear.iid=65; imsic_direct_clear_local(&clear);
 assert(pending[2]==BIT(9));
 assert(csrs[CSR_HSTATUS]==0x123 && csrs[CSR_VSISELECT]==0xabc);
 k.arch.aia.mode=KVM_DEV_RISCV_AIA_MODE_HWACCEL;k.cmode=true;
 v.cpu=0;imsic.vsfile_cpu=-1;imsic.trusted_file=false;alloc_result=2;bind_error=-EPERM;
 int before_maps=maps,before_frees=frees;
 assert(kvm_riscv_vcpu_aia_imsic_update(&v)==-EPERM);
 assert(maps==before_maps && frees==before_frees+1 && !imsic.trusted_file);
 before_frees=frees;reclaim_error=-EPERM;
 assert(kvm_riscv_vcpu_aia_imsic_update(&v)==-EPERM);
 assert(frees==before_frees && quarantines==1 && !imsic.direct_transition);
 reclaim_error=0;
 bind_error=0;
 assert(kvm_riscv_vcpu_aia_imsic_update(&v)==1);
 assert(imsic.trusted_file && imsic.vsfile_hgei==2);
 before_frees=frees;
 kvm_riscv_vcpu_aia_imsic_release(&v);
 assert(frees==before_frees && imsic.vsfile_hgei==2);
 puts("PASS: IMSIC hardware reservation, migration/resource fail-closed, transition exclusion and atomic IID cleanup");
}
