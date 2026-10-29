/* Extracted production NIC registration and DMA routing, with two devices. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uintptr_t phys_addr_t;
typedef uint8_t u8;
#define PAGE_SIZE 4096UL
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(n) (((n) + PAGE_SIZE - 1) & PAGE_MASK)
#define READ_ONCE(x) (x)
#define BIT_ULL(n) (1ULL << (n))
#define GFP_KERNEL 0
#define kzalloc(n,g) calloc(1,n)
#define kfree free
#define virt_to_phys(p) ((phys_addr_t)(p))
#define dev_err_ratelimited(...) ((void)0)
#define dev_info(...) ((void)0)
#define SBI_EXT_COVG 0
#define SBI_EXT_COVG_SIM_GET_INTERFACE_ID 1
#define SBI_EXT_COVG_GET_INTERFACE_REPORT 2
#define SBI_EXT_COVG_SIM_PREPARE_DMA 3
#define SBI_COVG_INTERFACE_RUNNING 2
struct device { void *state; const void *dma_ops; };
struct pci_dev { unsigned vendor,device; struct device dev; unsigned long bar; };
struct sbiret { long error,value; };
struct sbi_cove_io_interface_report { unsigned long mmio_gpa,mmio_size,flags,state,dma_gpa,dma_size; };
static bool enabled=true, allocation_failure;
static const int nic_dma_ops;
static unsigned long mapped_tdi;
static unsigned char page[PAGE_SIZE];
static void set_dma_ops(struct device *d,const void *ops) { d->dma_ops=ops; }
static const void *get_dma_ops(struct device *d) { return d->dma_ops; }
static void *devres_find(struct device *d,void (*release)(struct device *,void *),void *match,void *data) { return d->state; }
static void *devres_alloc(void (*release)(struct device *,void *),size_t n,int flags) { return allocation_failure ? NULL : calloc(1,n); }
static void devres_add(struct device *d,void *p) { d->state=p; }
static bool dev_is_dma_coherent(struct device *d) { return true; }
static bool dma_capable(struct device *d,phys_addr_t p,size_t n,bool check) { return p >= 0x80000000 && n <= 0x10000000 && p < 0x90000000 && n <= 0x90000000-p; }
static phys_addr_t phys_to_dma(struct device *d,phys_addr_t p) { return p; }
static void *phys_to_virt(phys_addr_t p) { return page; }
static unsigned long pci_resource_start(struct pci_dev *d,int bar) { return d->bar; }
static unsigned long pci_resource_len(struct pci_dev *d,int bar) { return 0x20000; }
static struct sbiret sbi_ecall(unsigned long ext,unsigned long op,unsigned long id,
 unsigned long addr,unsigned long size,unsigned long a,unsigned long b,unsigned long c)
{
 if (op==SBI_EXT_COVG_SIM_GET_INTERFACE_ID)
  return (struct sbiret){0,id==0 ? 4 : id==4 ? 5 : 0};
 if (op==SBI_EXT_COVG_GET_INTERFACE_REPORT) {
  struct sbi_cove_io_interface_report *r=(void *)addr;
  *r=(struct sbi_cove_io_interface_report){.mmio_gpa=id==4 ? 0x40000000 : 0x40020000,
   .mmio_size=0x20000,.flags=BIT_ULL(1)|BIT_ULL(4),.state=2,
   .dma_gpa=0x80000000,.dma_size=0x10000000};
  return (struct sbiret){0,0};
 }
 assert(op==SBI_EXT_COVG_SIM_PREPARE_DMA);
 if ((id!=4 && id!=5) || !size || addr<0x80000000 || addr>=0x90000000 || size>0x90000000-addr)
  return (struct sbiret){-EPERM,0};
 mapped_tdi=id;
 return (struct sbiret){0,0};
}
#include "nic.inc"
static void release_device(struct device *d)
{
 nic_release(d,d->state);free(d->state);d->state=NULL;
}
int main(void)
{
 struct pci_dev a={.vendor=0x8086,.device=0x10d3,.bar=0x40000000};
 struct pci_dev b={.vendor=0x8086,.device=0x10d3,.bar=0x40020000};
 struct pci_dev c={.vendor=0x8086,.device=0x10d3,.bar=0x40040000};
 assert(!cove_io_nic_enable(&a));assert(!cove_io_nic_enable(&b));
 assert(nic_state(&a.dev)->tdi==4 && nic_state(&b.dev)->tdi==5);
 assert(cove_io_nic_enable(&a)==-EINVAL);
 assert(cove_io_nic_enable(&c)==-ENODEV && !c.dev.state);
 assert(!nic_prepare(&a.dev,0x80001000,4096) && mapped_tdi==4);
 assert(!nic_prepare(&b.dev,0x80002000,4096) && mapped_tdi==5);
 assert(nic_prepare(&c.dev,0x80002000,4096)==-EINVAL);
 assert(nic_prepare(&a.dev,0x40000000,4096)==-EINVAL);
 assert(nic_prepare(&a.dev,0x80000000,0)==-EINVAL);
 release_device(&a.dev);
 assert(!a.dev.dma_ops && !nic_state(&a.dev));
 assert(b.dev.dma_ops==&nic_dma_ops && nic_state(&b.dev)->tdi==5);
 assert(!nic_prepare(&b.dev,0x80002000,4096) && mapped_tdi==5);
 allocation_failure=true;assert(cove_io_nic_enable(&a)==-ENOMEM && !a.dev.state && !a.dev.dma_ops);
 allocation_failure=false;assert(!cove_io_nic_enable(&a));
 release_device(&b.dev);release_device(&a.dev);
 puts("PASS: two NICs route private DMA through distinct TDIs; unmatched device, duplicate, allocation failure and independent removal checked");
}
