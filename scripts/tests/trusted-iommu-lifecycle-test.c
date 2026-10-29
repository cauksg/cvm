/* Test actual firmware code with mocked MMIO queue completion, never linked in firmware. */
#include <assert.h>
#include <sys/mman.h>
#include "sbi_cvm_iommu.c"
int sbi_cvm_bitmap_mmio_init(const u64 *b,unsigned long a,unsigned long s) { return 0; }
void sbi_cvm_bitmap_mmio_sync(void) {}
void sbi_cvm_bitmap_mmio_set_trusted(bool v) {}
int sbi_cvm_mmio_bitmap_register(const struct sbi_cvm_mmio_bitmap_ops *ops)
{ assert(ops->claim && ops->quiesce && ops->unclaim); return 0; }
int cove_io_trusted_msi(unsigned long r,unsigned long a,unsigned long t,unsigned long i)
{ return SBI_EDENIED; }
int cove_io_bind_imsic(unsigned long v,unsigned long f,unsigned long g,unsigned long h)
{ return SBI_EDENIED; }
int main(void)
{
 unsigned long value=0;
 unsigned long pa=0x81000000, gpa=0x82000000;
 unsigned long *entry;
 assert(mmap((void *)CVM_IOMMU_POOL_BASE,CVM_IOMMU_POOL_SIZE,PROT_READ|PROT_WRITE,
             MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==(void *)CVM_IOMMU_POOL_BASE);
 assert(!sbi_cvm_iommu_platform_init((void *)1));
 assert(active && !failed);
 assert(!sbi_cvm_iommu_file_reserve(1,1));
 assert(!sbi_cvm_iommu_file_reserve(2,2));
 assert(sbi_cvm_iommu_file_reserve(1,2)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_file_reserve(3,1));
 assert(!sbi_cvm_iommu_file_release(3,1));
 assert(sbi_cvm_iommu_file_release(1,2)==SBI_EDENIED);
 assert(file_owners[1]==1 && file_owners[2]==2);
 assert(sbi_cvm_iommu_host_buffer(pa,8192));
 assert(!sbi_cvm_iommu_host_buffer(0x80000000,8));
 assert(!sbi_cvm_iommu_host_buffer(CVM_IOMMU_POOL_BASE-8,16));
 assert(!sbi_cvm_iommu_host_buffer(~0UL-7,16));
 assert(!sbi_cvm_iommu_host_buffer(0x40000000,8));
 assert(!leaf(host_root,CVM_IOMMU_POOL_BASE,false));
 entry=leaf(host_root,0x80000000,false);
 assert(!entry || !*entry);
 assert(!sbi_cvm_iommu_ecall(CVM_IOMMU_HOST_CONTEXT,16,0,1,0,&value));
 assert(directory[16].tc==1 && directory[16].iohgatp>>60==8);
 assert(!sbi_cvm_iommu_claim(16));
 assert(!sbi_cvm_iommu_set_owner(16,1,1));
 assert(sbi_cvm_iommu_set_owner(16,2,2)==SBI_EDENIED);
 assert(sbi_cvm_iommu_file_release(1,1)==SBI_EDENIED);
 assert(!directory[16].tc);
 assert(sbi_cvm_iommu_claim(16)==SBI_EALREADY);
 /* Eight independent RID slots; full capacity fails without mutation. */
 for (unsigned long rid=24; rid<=31; rid++) {
  if (rid == 31) assert(sbi_cvm_iommu_claim(rid)==SBI_ENOSPC);
  else assert(!sbi_cvm_iommu_claim(rid));
 }
 for (unsigned long rid=25; rid<=30; rid++) assert(!sbi_cvm_iommu_release(rid));
 assert(slot(16) && slot(24));
 assert(sbi_cvm_iommu_claim(0)==SBI_EINVAL);
 assert(sbi_cvm_iommu_claim(64)==SBI_EINVAL);
 assert(sbi_cvm_iommu_release(64)==SBI_EINVAL);
 assert(sbi_cvm_iommu_map(16,gpa,pa)==SBI_EDENIED);
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_HOST_CONTEXT,16,0,1,0,&value)==SBI_EDENIED);
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_HOST_CONTEXT,16,0,0,0,&value)==SBI_EDENIED);
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_PCI_WRITE,CVM_IOMMU_ECAM_BASE,16*PAGE+0x10,4,0,&value)==SBI_EDENIED);
 writel(0x11e81234,(void *)(CVM_IOMMU_ECAM_BASE+16*PAGE));
 test_control[0x40/8]=0x40000000;test_control[0x48/8]=0x100000;
 assert(mmio_claim((2ULL<<56)|16,0x40001000,0x100000)==SBI_EDENIED);
 assert(!mmio_claim((2ULL<<56)|16,0x40000000,0x100000));
 assert(slot(16)->mmio_claimed);
 assert(sbi_cvm_iommu_release(16)==SBI_EDENIED && slot(16));
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_PCI_WRITE,CVM_IOMMU_ECAM_BASE,16*PAGE+0x78,2,0x8000,&value)==SBI_EDENIED);
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_PCI_WRITE,CVM_IOMMU_ECAM_BASE,16*PAGE+0x32,2,1,&value)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_start(16));
 assert(!sbi_cvm_iommu_protect(pa,true));
 assert(!sbi_cvm_iommu_host_buffer(pa,8));
 assert(!sbi_cvm_iommu_host_buffer(pa-8,16));
 assert(!*leaf(host_root,pa,false));
 assert(!sbi_cvm_iommu_map(16,gpa,pa));
 entry=leaf((unsigned long)slot(16)->root,gpa,false);
 assert(*entry==pte(pa,RW));
 assert(sbi_cvm_iommu_map(16,gpa,pa+PAGE)==SBI_EDENIED);
 assert(sbi_cvm_iommu_map(16,gpa,CVM_IOMMU_POOL_BASE)==SBI_EINVAL);
 assert(sbi_cvm_iommu_msi_install(16,MSI_ADDR,MSI_FILE+PAGE,2)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_msi_install(16,MSI_ADDR,MSI_FILE,2));
 assert(sbi_cvm_iommu_irq_reserved(1,1,2));
 assert(!sbi_cvm_iommu_irq_reserved(1,2,2));
 assert(!sbi_cvm_iommu_irq_reserved(2,1,2));
 assert(!sbi_cvm_iommu_msi_install(16,MSI_ADDR,0,0));
 assert(sbi_cvm_iommu_irq_reserved(1,1,2));
 assert(!sbi_cvm_iommu_msi_install(16,MSI_ADDR,MSI_FILE,2));
 assert(!sbi_cvm_iommu_file_reserve(3,1));
 assert(sbi_cvm_iommu_set_owner(16,3,1)==SBI_EDENIED);
 assert(slot(16)->file==1);
 assert(!sbi_cvm_iommu_file_release(3,1));
 assert(sbi_cvm_iommu_msi_install(16,MSI_ADDR,MSI_FILE,3)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_ecall(CVM_IOMMU_PCI_WRITE,CVM_IOMMU_ECAM_BASE,16*PAGE+0x44,4,0xdead0000,&value));
 assert(readl((void *)(CVM_IOMMU_ECAM_BASE+16*PAGE+0x44))==MSI_ADDR);
 assert(!sbi_cvm_iommu_ecall(CVM_IOMMU_PCI_WRITE,CVM_IOMMU_ECAM_BASE,16*PAGE+0x4c,2,255,&value));
 assert(readw((void *)(CVM_IOMMU_ECAM_BASE+16*PAGE+0x4c))==2);
 assert(slot(16)->msi[0]==pte(MSI_FILE,7));
 /* Returning RAM to Host clears stale CVM-device aliases before the fence. */
 unsigned long fences=test_fences;
 assert(!sbi_cvm_iommu_protect(pa,false));
 assert(!*entry && *leaf(host_root,pa,false)==pte(pa,RW));
 assert(test_fences>fences);
 assert(!sbi_cvm_iommu_map(16,gpa,pa));
 assert(!sbi_cvm_iommu_set_owner(24,2,2));
 assert(sbi_cvm_iommu_msi_install(24,MSI_ADDR,MSI_FILE,3)==SBI_EDENIED);
 /* Device 24 has its own tables and IID. STOP(16) cannot revoke it. */
 test_control[0x40/8]=0x40200000;
 assert(!mmio_claim((2ULL<<56)|24,0x40200000,0x100000));
 assert(!sbi_cvm_iommu_start(24));
 assert(!sbi_cvm_iommu_map(24,gpa,pa));
 unsigned long first_msi = slot(16)->msi[0], before_fences = test_fences;
 assert(!sbi_cvm_iommu_msi_install(24,MSI_ADDR,MSI_FILE+PAGE,2));
 assert(slot(16)->msi[0]==first_msi && slot(24)->msi[0] && test_fences>before_fences);
 assert(sbi_cvm_iommu_msi_install(24,MSI_ADDR,MSI_FILE+PAGE,3)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_stop(16));
 assert(sbi_cvm_iommu_irq_reserved(1,1,2));
 assert(directory[24].tc && slot(24)->msi[0]);
 assert(*leaf((unsigned long)slot(24)->root,gpa,false)==pte(pa,RW));
 assert(!directory[16].tc && !slot(16)->msi[0]);
 assert(sbi_cvm_iommu_map(16,gpa+PAGE,pa)==SBI_EDENIED);
 assert(!mmio_quiesce((2ULL<<56)|16,0x40000000,0x100000));
 mmio_unclaim((2ULL<<56)|16,0x40000000,0x100000);
 assert(!sbi_cvm_iommu_release(16));
 assert(!sbi_cvm_iommu_irq_reserved(1,1,2));
 assert(!slot(16) && !host_valid[16] && !directory[16].tc);
 assert(!sbi_cvm_iommu_file_release(1,1));
 assert(!sbi_cvm_iommu_file_reserve(1,3));
 assert(sbi_cvm_iommu_file_release(1,1)==SBI_EDENIED);
 assert(!sbi_cvm_iommu_file_release(1,3));
 assert(file_owners[2]==2 && directory[24].tc && slot(24)->msi[0]);
 assert(!sbi_cvm_iommu_stop(24));
 assert(!mmio_quiesce((2ULL<<56)|24,0x40200000,0x100000));
 mmio_unclaim((2ULL<<56)|24,0x40200000,0x100000);
 assert(!sbi_cvm_iommu_release(24));
 assert(!sbi_cvm_iommu_claim(24));
 assert(!*leaf((unsigned long)slot(24)->root,gpa,false));
 assert(sbi_cvm_iommu_start(24)==SBI_EDENIED);
 test_control[0x40/8]=0x40000000;
 assert(!mmio_claim((2ULL<<56)|24,0x40000000,0x100000));
 assert(!sbi_cvm_iommu_start(24));
 test_command_failure=true;
 assert(sbi_cvm_iommu_stop(24)==SBI_EIO);
 assert(failed && !readq((void *)(CVM_IOMMU_REG_BASE+DDTP)));
 assert(sbi_cvm_iommu_release(24)==SBI_EIO && slot(24));
 assert(sbi_cvm_iommu_ecall(CVM_IOMMU_HOST_CONTEXT,24,0,1,0,&value)==SBI_EIO);
 assert(!sbi_cvm_iommu_ecall(CVM_IOMMU_PROBE,0,0,0,0,&value));
 puts("PASS: trusted IOMMU exclusive claim, Host override denial, authorized mapping, stale alias revoke, eight slots, distinct IID, per-RID STOP/reuse and failed-drain retention");
 return 0;
}
