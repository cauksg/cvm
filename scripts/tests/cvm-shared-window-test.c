/* Execute real Monitor sharing and private-page decisions with model PTEs. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define PAGE_SIZE 4096UL
#define PAGE_SHIFT 12
#define PAGE_PFN_SHIFT 10
#define PTE_V 1
#define PTE_R 2
#define PTE_W 4
#define PTE_X 8
#define PTE_U 16
#define PTE_A 64
#define PTE_D 128
#define KEYID_MASK 0UL
#define CVM_ERROR -2
#define SBI_EDENIED -4
#define SBI_EALREADY -6
#define SBI_CVM_MMIO_END 0x80000000UL
#define SBI_EXT_COVG_SIM_ACCEPT_SHARED 1089
#define SBI_EXT_COVG_SIM_UNSHARE 1090
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#define sbi_printf(...) ((void)0)
#define sbi_panic(...) assert(!"unexpected ownership mismatch")
typedef unsigned long paddr_t;
typedef unsigned long pt_entry_t;
typedef uint64_t u64;
struct kvm_vmid { unsigned long vmid; };
struct cvm_shared_window { unsigned long base,size; bool accepted; };
struct sbi_cvm { struct kvm_vmid *vmid; unsigned int shared_count; struct cvm_shared_window shared[16]; unsigned long swiotlb_addr,swiotlb_size; int page_table_lock; };
typedef struct {unsigned long vmidp;} page_own_table_t;
static page_own_table_t owners[32], *page_own_table=owners;
static unsigned long bits[1], *bitmap=bits, bitmap_max_ele=1, own_max_num=32;
static unsigned int ownership_table_level;
static int spin_lock_page_own_table;
static bool sbi_cvm_iommu_active(void) { return true; }
static bool sbi_cvm_iommu_host_buffer(unsigned long pa,unsigned long size) { return pa>=0x80000000 && pa+size<=0x80020000 && !(bits[0] & (1UL<<((pa-0x80000000)/PAGE_SIZE))); }
static pt_entry_t entries[16];
static unsigned int fences;
static int find_sw_pte(struct sbi_cvm *c, unsigned long gpa, pt_entry_t **p) { assert(gpa>=0x90000000 && gpa<0x90010000); *p=&entries[(gpa-0x90000000)/PAGE_SIZE]; return 0; }
static int find_pte(struct sbi_cvm *c,unsigned long gpa,pt_entry_t **p) { find_sw_pte(c,gpa,p); return **p&PTE_V ? CVM_ERROR:0; }
static int set_page_own_table(unsigned long pa,unsigned long *owner) { unsigned long i=(pa-0x80000000)/PAGE_SIZE; if(owners[i].vmidp) return -1; owners[i].vmidp=(unsigned long)owner; return 0; }
static int reset_page_own_table(unsigned long pa,unsigned long *owner) { unsigned long i=(pa-0x80000000)/PAGE_SIZE; if(owners[i].vmidp!=(unsigned long)owner) return -1; owners[i].vmidp=0; return 0; }
static void __sbi_hfence_gvma_all(void) { fences++; }
static void __sbi_hfence_vvma_all(void) { fences++; }
#include "contains.inc"
#include "request.inc"
#include "map.inc"
#include "private.inc"
int main(void) {
 struct kvm_vmid id={7}; struct sbi_cvm cvm={.vmid=&id,.shared_count=2,.shared={{0x90000000,8192,false},{0x90004000,8192,false}}};
 assert(!cvm_shared_contains(&cvm,0x90003000,false));
 assert(cvm_shared_contains(&cvm,0x90000000,false));
 assert(!cvm_shared_contains(&cvm,0x90000000,true));
 assert(add_cvm_share_pages(&cvm,0x90000000,0x80001000,true,0)==SBI_EDENIED);
 assert(cvm_shared_request(&cvm,1089,0x90000000,4096,0,0)==SBI_EDENIED);
 assert(cvm_shared_request(&cvm,1089,0x90000000,8192,1,0)==SBI_EDENIED);
 assert(!cvm_shared_request(&cvm,1089,0x90000000,8192,0,0));
 assert(cvm_shared_request(&cvm,1089,0x90000000,8192,0,0)==SBI_EALREADY);
 assert(!add_cvm_share_pages(&cvm,0x90000000,0x80001000,true,0));
 assert(!(entries[0]&PTE_X));
 assert(!cove_io_private_page_owned_by(&cvm,0x80001000));
 assert(add_cvm_share_pages(&cvm,0x90001000,0x80001000,true,0)==SBI_EDENIED);
 assert(!entries[1]);
 assert(add_cvm_share_pages(&cvm,0x90003000,0x80002000,true,0)==SBI_EDENIED);
 assert(!cvm_shared_request(&cvm,1090,0x90000000,8192,0,0));
 assert(!entries[0] && !owners[1].vmidp && fences==2);
 assert(add_cvm_share_pages(&cvm,0x90000000,0x80001000,true,0)==SBI_EDENIED);
 assert(cvm_shared_request(&cvm,1090,0x90000000,8192,0,0)==SBI_EDENIED);
 assert(!cvm_shared_request(&cvm,1089,0x90000000,8192,0,0));
 entries[4]=PTE_V|PTE_R|PTE_X;
 assert(cvm_shared_request(&cvm,1089,0x90004000,8192,0,0)==SBI_EDENIED);
 assert(!cvm.shared[1].accepted);
 /* Recycle of a rejected proposal must preserve the private leaf/owner. */
 pt_entry_t private_leaf = entries[4];
 owners[3].vmidp=(unsigned long)&id.vmid;
 cvm_shared_revoke(&cvm,&cvm.shared[1]);
 assert(entries[4]==private_leaf && owners[3].vmidp==(unsigned long)&id.vmid);
 /* Accepted shared leaves are revoked independently of rejected windows. */
 assert(!add_cvm_share_pages(&cvm,0x90000000,0x80001000,true,0));
 cvm_shared_revoke(&cvm,&cvm.shared[0]);
 assert(!entries[0] && !owners[1].vmidp && !cvm.shared[0].accepted);
 bits[0]=1UL<<2;owners[2].vmidp=(unsigned long)&id.vmid;
 assert(cove_io_private_page_owned_by(&cvm,0x80002000));
 struct kvm_vmid other_id={8}; struct sbi_cvm other={.vmid=&other_id};
 assert(!cove_io_private_page_owned_by(&other,0x80002000));
 owners[2].vmidp=(unsigned long)&other_id.vmid;
 assert(!cove_io_private_page_owned_by(&cvm,0x80002000));
 assert(cove_io_private_page_owned_by(&other,0x80002000));
 owners[2].vmidp=(unsigned long)&id.vmid;
 assert(add_cvm_share_pages(&cvm,0x90001000,0x80002000,true,0)==SBI_EDENIED);
 assert(!cove_io_private_page_owned_by(&cvm,0x80020000));
 puts("PASS: exact consent, holes, replay, private refusal, NX, shared/private DMA distinction, unshare fence, denied remap and recycle of rejected private-overlap proposal");
 return 0;
}
