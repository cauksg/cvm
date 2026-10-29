/* Exercises the actual monitor mapping routine with a small page-table model. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sbi/sbi_cvm_mmio.h>
typedef uint64_t u64;
typedef uint64_t pt_entry_t;
#define PAGE_SIZE 4096ULL
#define PAGE_SHIFT 12
#define PAGE_PFN_SHIFT 10
#define PTE_V 1
#define PTE_R 2
#define PTE_W 4
#define PTE_X 8
#define PTE_U 16
#define PTE_A 64
#define PTE_D 128
#define CVM_ERROR -2
#define COVE_IO_TDI_F_DIRECT_MMIO (1U << 4)
#define COVE_IO_TDI_F_MMIO_BITMAP (1U << 5)
#define COVE_IO_TDI_STATE_ERROR 5
#define SBI_TLB_FLUSH_ALL (~0UL)
#define SBI_TLB_HFENCE_GVMA 4
#define current_hartid() 0
#define spin_lock(p) ((void)(p))
#define spin_unlock(p) ((void)(p))
#define sbi_printf(...) ((void)0)
struct sbi_cvm { int page_table_lock; };
struct cvm_node { struct sbi_cvm cvm; };
struct sbi_tlb_info { int type; };
#define SBI_TLB_INFO_INIT(t, a, b, c, d, e, f) ((t)->type = (e))
struct cove_io_tdi_entry {
 u64 tdi_id, vmid, mmio_hpa, mmio_gpa, mmio_size;
 u64 flags, mmio_map_generation;
 int state;
 bool mmio_installed, mmio_revoke_pending;
};
unsigned int sbi_hart_count(void) { return 1; }
void __sbi_sfence_vma_all(void) {}
void __sbi_hfence_gvma_all(void) {}
void __sbi_hfence_vvma_all(void) {}
static const u64 *bitmap;
static int stop_error;
static int hw_init(const u64 *map, unsigned long base, unsigned long size) {
 assert(base == 0x40000000 && size == 0x40000000); bitmap = map; return 0;
}
static int hw_claim(u64 device, u64 hpa, u64 size) {
 assert(device == 17 && hpa == 0x50000000 && size == 0x100000); return 0;
}
static void hw_sync(void) {}
static void hw_mode(bool trusted) { (void)trusted; }
static int hw_stop(u64 device, u64 hpa, u64 size) {
 assert(bitmap[(hpa-0x40000000)/4096/64] && device == 17 && size == 0x100000);
 return stop_error;
}
static void hw_unclaim(u64 device, u64 hpa, u64 size) {
 assert(!bitmap[(hpa-0x40000000)/4096/64] && device == 17 && size == 0x100000);
}
/* Host-selected delegation must not enter HS without restoring bitmap checks. */
#define CSR_HEDELEG 0
#define CSR_HIDELEG 1
#define CSR_MEDELEG 2
#define CSR_MIDELEG 3
#define CSR_MIE 4
#define MIP_VSSIP (1UL << 2)
#define MIP_VSTIP (1UL << 6)
#define MIP_VSEIP (1UL << 10)
#define MIP_SGEIP (1UL << 12)
static unsigned long csrs[5];
static unsigned long csr_read(unsigned int csr) { return csrs[csr]; }
static void csr_write(unsigned int csr, unsigned long value) {
 /* Model one unsupported hedeleg bit: it must be sent to M instead of HS. */
 csrs[csr] = csr == CSR_HEDELEG ? value & ~(1UL << 1) : value;
 if (csr == CSR_MIDELEG) csrs[csr] |= MIP_SGEIP;
}
static void csr_clear(unsigned int csr, unsigned long mask) {
 csr_write(csr, csr_read(csr) & ~mask);
}
static unsigned long csr_swap(unsigned int csr, unsigned long value) {
 unsigned long old = csr_read(csr); csr_write(csr, value); return old;
}
#define sbi_panic(...) assert(!"unexpected panic")
struct cvm_vcpu { unsigned long guest_medeleg, host_hedeleg, host_hideleg; bool io_trap_guard_active; };
static bool direct_context;
static bool cove_io_has_direct_mmio(struct cvm_vcpu *vcpu) { (void)vcpu; return direct_context; }
static bool ram_hw_active;
static bool sbi_cvm_bitmap_active(void) { return ram_hw_active; }
static bool trusted_context;
static bool sbi_cvm_iommu_active(void) { return trusted_context; }
#include "cove-io-mmio-context.inc"
static void test_delegation(void) {
 struct cvm_vcpu vcpu = { .guest_medeleg = 0xff };
 unsigned long virtual_irqs = MIP_VSSIP | MIP_VSTIP | MIP_VSEIP;
 csrs[CSR_MEDELEG] = vcpu.guest_medeleg;
 csrs[CSR_MIDELEG] = virtual_irqs | (1UL << 5) | (1UL << 12);
 csrs[CSR_MIE] = MIP_SGEIP;
 /* Host deliberately leaves HEDELEG/HIDELEG zero. */
 cove_io_enter_mmio_context(&vcpu);
 assert(csrs[CSR_MEDELEG] == (0xff & ~(1UL << 1)));
 assert(csrs[CSR_MIDELEG] == (virtual_irqs | MIP_SGEIP));
 assert(!(csrs[CSR_MIE] & MIP_SGEIP));
 assert(csrs[CSR_HIDELEG] == virtual_irqs);
 assert(!vcpu.host_hedeleg && !vcpu.host_hideleg);
 cove_io_exit_mmio_context(&vcpu);
 assert(!csrs[CSR_HEDELEG] && !csrs[CSR_HIDELEG]);
 assert(!vcpu.io_trap_guard_active);
}
static struct cvm_node node;
static pt_entry_t entries[256];
static int fail_page = -1, fence_error, fences;
static struct cvm_node *get_cvm(u64 id) { return id == 1 ? &node : NULL; }
static int slot(u64 gpa) {
 assert(gpa >= 0x40000000 && gpa < 0x40100000);
 return (gpa - 0x40000000) / PAGE_SIZE;
}
static int find_pte(struct sbi_cvm *cvm, u64 gpa, pt_entry_t **pte) {
 int i = slot(gpa); (void)cvm;
 if (i == fail_page || entries[i] & PTE_V) return CVM_ERROR;
 *pte = &entries[i]; return 0;
}
static int find_sw_pte(struct sbi_cvm *cvm, u64 gpa, pt_entry_t **pte) {
 (void)cvm; *pte = &entries[slot(gpa)]; return !(**pte & PTE_V);
}
static int sbi_tlb_request(unsigned long mask, unsigned long base,
                          struct sbi_tlb_info *tlb) {
 assert(mask == 0 && base == ~0UL && tlb->type == SBI_TLB_HFENCE_GVMA);
 fences++; return fence_error;
}
#include "cove-io-direct-mmio.inc"
#include "cove-io-reclaim-mmio.inc"
int main(void) {
 struct cove_io_tdi_entry tdi = {
  .tdi_id=7, .vmid=1, .mmio_gpa=0x40000000,
  .mmio_hpa=0x50000000, .mmio_size=0x100000,
  .flags=COVE_IO_TDI_F_DIRECT_MMIO | COVE_IO_TDI_F_MMIO_BITMAP,
 };
 const struct sbi_cvm_mmio_bitmap_ops ops = {
  .init=hw_init, .claim=hw_claim, .sync=hw_sync, .set_trusted=hw_mode,
  .quiesce=hw_stop, .unclaim=hw_unclaim,
 };
 assert(!cove_io_update_mmio(&tdi, false));
 assert(!entries[0] && !fences);
 assert(cove_io_update_mmio(&tdi, true) == CVM_ERROR);
 ram_hw_active = true;
 test_delegation(); /* RAM isolation needs the same guard before MMIO attaches. */
 ram_hw_active = false;
 direct_context = true;
 test_delegation(); /* Direct IRQs need the guard with both bitmaps disabled. */
 direct_context = false;
 assert(!sbi_cvm_mmio_bitmap_register(&ops));
 assert(!sbi_cvm_mmio_reserve(&tdi, 17, tdi.mmio_hpa, tdi.mmio_size, true));
 test_delegation();
 fail_page = 128;
 assert(cove_io_update_mmio(&tdi, true) == CVM_ERROR);
 for (int i=0; i<256; i++) assert(!entries[i]);
 assert(!tdi.mmio_installed && !fences);
 fail_page = -1;
 entries[128] = PTE_V | PTE_R;
 assert(cove_io_update_mmio(&tdi, true) == CVM_ERROR);
 assert(entries[128] == (PTE_V | PTE_R) && !entries[0]);
 entries[128] = 0;
 assert(!cove_io_update_mmio(&tdi, true));
 assert(tdi.mmio_installed && fences == 1);
 for (int i=0; i<256; i++) {
  assert(entries[i] == (((0x50000000ULL+i*PAGE_SIZE) >> 12 << 10) |
   PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D | COVE_IO_PTE_IO));
  assert(!(entries[i] & PTE_X));
 }
 assert(!cove_io_update_mmio(&tdi, true) && fences == 1);
 assert(!cove_io_update_mmio(&tdi, false));
 assert(!tdi.mmio_installed && fences == 2);
 assert(bitmap[(0x50000000-0x40000000)/4096/64]); /* STOP retains Host denial. */
 for (int i=0; i<256; i++) assert(!entries[i]);
 fence_error = -5;
 assert(cove_io_update_mmio(&tdi, true) == -5);
 assert(tdi.mmio_installed); /* Failed revoke fence remains pending for retry. */
 assert(tdi.mmio_revoke_pending);
 assert(cove_io_update_mmio(&tdi, true) == CVM_ERROR);
 for (int i=0; i<256; i++) assert(!entries[i]);
 assert(cove_io_reclaim_mmio(&tdi) == -5);
 assert(tdi.mmio_hpa && bitmap[(0x50000000-0x40000000)/4096/64]);
 fence_error = 0;
 stop_error = -7;
 assert(cove_io_reclaim_mmio(&tdi) == -7);
 assert(tdi.state == COVE_IO_TDI_STATE_ERROR && !tdi.mmio_revoke_pending);
 assert(!tdi.mmio_installed && tdi.mmio_hpa && tdi.mmio_size);
 assert(bitmap[(0x50000000-0x40000000)/4096/64]);
 stop_error = 0;
 assert(!cove_io_reclaim_mmio(&tdi));
 assert(!tdi.mmio_hpa && !tdi.mmio_gpa && !tdi.mmio_size && !tdi.flags);
 assert(!bitmap[(0x50000000-0x40000000)/4096/64]);
 puts("PASS: monitor direct MMIO permissions, bitmap lifecycle, conflict, revoke and fence rollback");
}
