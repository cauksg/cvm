/* SPDX-License-Identifier: BSD-2-Clause */
/* Fixed QEMU virt prototype: one hart, 1 GiB RAM, one platform IOMMU.
 * All PCI DMA uses this IOMMU. Hardware tables live in an M-only pool.
 * Host FSC is an untrusted first stage behind the Monitor's identity G-stage.
 */
#include <libfdt.h>
#include <sbi/riscv_io.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_locks.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_cvm_iommu.h>
#include <sbi/sbi_cvm_bitmap.h>
#include <sbi/sbi_cvm_mmio.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_helper.h>

#define RAM_BASE 0x80000000UL
#define RAM_END 0xc0000000UL
#define PAGE 4096UL
#define V 1UL
#define RW 0xd7UL /* V,R,W,U,A,D; device cannot execute */
#define CQB 0x18
#define CQT 0x24
#define CQH 0x20
#define CQCSR 0x48
#define DDTP 0x10
#define MSI_ADDR 0x28000000UL
#define MSI_FILE 0x28001000UL
static unsigned long file_owners[8];
#define DMA_SLOTS 8 /* platform-wide, distinct RID and IID per interface */
#define CONTROL 0x03011000UL
#define CONTROL_MAGIC 0x43564d01UL

struct ti_dc { u64 tc, iohgatp, ta, fsc, msiptp, mask, pattern, reserved; };
struct ti_cmd { u64 lo, hi; };
struct ti_slot {
 unsigned long rid;
 unsigned long *root;
 unsigned long *msi;
 bool used, started, mmio_claimed;
 unsigned long bar, bar_size, msi_iid, file, owner;
};
static bool active, failed;
static unsigned long next_page, host_root;
static struct ti_dc *directory;
static struct ti_cmd *commands;
static unsigned int tail;
static struct ti_slot slots[DMA_SLOTS];
static unsigned long host_fsc[CVM_IOMMU_MAX_RIDS];
static bool host_valid[CVM_IOMMU_MAX_RIDS];
static spinlock_t ti_lock = SPIN_LOCK_INITIALIZER;

static unsigned long pte(unsigned long pa, unsigned long flags)
{ return ((pa >> 12) << 10) | flags; }
static unsigned long *table(unsigned long entry)
{ return (unsigned long *)((entry >> 10) << 12); }
static unsigned long alloc_pages(unsigned long n, unsigned long align)
{
 unsigned long pa = (next_page + align - 1) & ~(align - 1);
 if (pa < CVM_IOMMU_POOL_BASE || pa > CVM_IOMMU_POOL_BASE + CVM_IOMMU_POOL_SIZE || n > (CVM_IOMMU_POOL_BASE + CVM_IOMMU_POOL_SIZE - pa) / PAGE)
  return 0;
 next_page = pa + n * PAGE;
 sbi_memset((void *)pa, 0, n * PAGE);
 return pa;
}
void *sbi_cvm_iommu_alloc(unsigned long size)
{
 unsigned long pa;
 if (!active || !size || size > CVM_IOMMU_POOL_SIZE) return NULL;
 spin_lock(&ti_lock);
 pa = alloc_pages((size + PAGE - 1) / PAGE, PAGE);
 spin_unlock(&ti_lock);
 return (void *)pa;
}
/* Sv39x4. Never use superpages: bitmap changes revoke individual 4K pages. */
static unsigned long *leaf(unsigned long root, unsigned long addr, bool create)
{
 unsigned long *p = (void *)root;
 if (addr >= (1UL << 41)) return NULL;
 for (int level = 2; level > 0; level--) {
  unsigned int idx = (addr >> (12 + 9 * level)) & (level == 2 ? 2047 : 511);
  if (!(p[idx] & V)) {
   unsigned long pa;
   if (!create || !(pa = alloc_pages(1, PAGE))) return NULL;
   p[idx] = pte(pa, V);
  }
  if (p[idx] & 0xe) return NULL;
  p = table(p[idx]);
 }
 return &p[(addr >> 12) & 511];
}
static int command(u64 lo, u64 hi)
{
 unsigned int next = (tail + 1) & 255;
 for (unsigned int n = 0; n < 1000000; n++) {
  if (readl((void *)(CVM_IOMMU_REG_BASE + CQH)) != next) goto ready;
 }
 return SBI_ETIMEDOUT;
ready:
 commands[tail].lo = lo; commands[tail].hi = hi;
 mb();
 writel(next, (void *)(CVM_IOMMU_REG_BASE + CQT)); tail = next;
 return 0;
}
/* Invalidate contexts and BOTH translation stages before draining PR/PW. */
static int sync_locked(void)
{
 int ret;
 if (failed) return SBI_EIO;
 mb();
 ret = command(3, 0); /* IODIR.INVAL.DDT, DV=0 */
 if (!ret) ret = command(1, 0); /* IOTINVAL.VMA, GV=0 */
 if (!ret) ret = command(1 | (1UL << 7), 0); /* GVMA, GV=0, AV=0 */
 if (!ret) ret = command(2 | (3UL << 12), 0); /* IOFENCE.C PR/PW */
 if (!ret) {
  for (unsigned int n = 0; n < 1000000; n++) {
   u32 csr = readl((void *)(CVM_IOMMU_REG_BASE + CQCSR));
   if (csr & 0x700) { ret = SBI_EIO; break; }
   if (readl((void *)(CVM_IOMMU_REG_BASE + CQH)) == tail) return 0;
   ret = SBI_ETIMEDOUT;
  }
 }
 /* Never return a successful revoke or publish another target after failure. */
 failed = true;
 writeq(0, (void *)(CVM_IOMMU_REG_BASE + DDTP));
 return ret;
}
static struct ti_slot *slot(unsigned long rid)
{
 for (unsigned int i = 0; i < DMA_SLOTS; i++)
  if (slots[i].used && slots[i].rid == rid) return &slots[i];
 return NULL;
}
static void publish(unsigned long rid, unsigned long root, unsigned long fsc,
                    unsigned long msi, bool valid)
{
 struct ti_dc *dc = &directory[rid];
 dc->tc = 0; mb();
 dc->iohgatp = (8UL << 60) | (root >> 12) | ((rid + 1) << 44);
 dc->ta = (rid + 1) << 12;
 dc->fsc = fsc;
 dc->msiptp = msi ? (1UL << 60) | (msi >> 12) : 0;
 dc->mask = msi ? 7 : 0; dc->pattern = msi ? (MSI_ADDR >> 12) : 0;
 dc->reserved = 0; mb();
 dc->tc = valid ? V : 0; /* no ATS, PRI, PDTV or hardware AD writes */
}
static void host_context(unsigned long rid)
{
 struct ti_slot *s = slot(rid);
 if (!s) publish(rid, host_root, host_fsc[rid], 0, host_valid[rid]);
}
bool sbi_cvm_iommu_active(void) { return active; }

/* The trusted Host G-stage reflects current RAM ownership. Exclude firmware,
 * table pool, MMIO and every private page, including cross-page buffers. */
bool sbi_cvm_iommu_host_buffer(unsigned long pa, unsigned long size)
{
 bool ok = true;
 if (!active) return true;
 if (!size || pa < RAM_BASE || pa >= CVM_IOMMU_POOL_BASE ||
     size > CVM_IOMMU_POOL_BASE - pa) return false;
 spin_lock(&ti_lock);
 for (unsigned long page = pa & ~(PAGE - 1); page < pa + size; page += PAGE) {
  unsigned long *entry = leaf(host_root, page, false);
  if (!entry || *entry != pte(page, RW)) { ok = false; break; }
 }
 spin_unlock(&ti_lock);
 return ok;
}

/* Only the two endpoints permitted by the fixed QEMU topology. */
static unsigned long msi_cap(unsigned long rid)
{
 unsigned long base = CVM_IOMMU_ECAM_BASE + rid * PAGE;
 u32 id = readl((void *)base);
 if (id == 0x11e81234) return 0x40;
 if (id == 0x10d38086) return 0xd0;
 return 0;
}
static void program_msi(struct ti_slot *s)
{
 unsigned long cap = msi_cap(s->rid);
 unsigned long base = CVM_IOMMU_ECAM_BASE + s->rid * PAGE + cap;
 if (!cap || !s->msi_iid) return;
 writel(MSI_ADDR, (void *)(base + 4));
 writel(0, (void *)(base + 8));
 writew(s->msi_iid, (void *)(base + 12));
}

static int control_command(unsigned long rid, unsigned long cmd)
{
 writeq(rid, (void *)(CONTROL + 8));
 writeq(cmd, (void *)(CONTROL + 0x10));
 return readq((void *)(CONTROL + 0x18)) ? SBI_EIO : 0;
}
static int mmio_claim(u64 device_id, u64 hpa, u64 size)
{
 unsigned long rid = device_id & 0xffff;
 struct ti_slot *s;
 int ret = SBI_EDENIED;
 if (device_id != ((2ULL << 56) | rid)) return SBI_EINVAL;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s || s->started || s->mmio_claimed || failed) goto out;
 writeq(rid, (void *)(CONTROL + 8));
 /* Real descriptors are supplied by the fixed hardware model, not KVM. */
 if (readq((void *)(CONTROL + 0x40)) != hpa ||
     readq((void *)(CONTROL + 0x48)) != size) {
  sbi_printf("COVE-IO BAR mismatch: rid=%lx requested=%lx+%lx hardware=%lx+%lx\n",
    rid, (unsigned long)hpa, (unsigned long)size,
    (unsigned long)readq((void *)(CONTROL + 0x40)),
    (unsigned long)readq((void *)(CONTROL + 0x48)));
  goto out;
 }
 writeq(s->file ? s->file : 1, (void *)(CONTROL + 0x38));
 ret = control_command(rid, 1);
 if (!ret) {
  writeq(s->owner, (void *)(CONTROL + 0x128));
  s->mmio_claimed = true; s->bar = hpa; s->bar_size = size; }
out:
 spin_unlock(&ti_lock);
 return ret;
}
static int mmio_quiesce(u64 device_id, u64 hpa, u64 size)
{
 unsigned long rid = device_id & 0xffff;
 struct ti_slot *s;
 int ret = sbi_cvm_iommu_stop(rid);
 if (ret) return ret;
 spin_lock(&ti_lock);
 s = slot(rid);
 ret = !s || !s->mmio_claimed || s->bar != hpa || s->bar_size != size ?
       SBI_EDENIED : control_command(rid, 3);
 spin_unlock(&ti_lock);
 return ret;
}
static void mmio_unclaim(u64 device_id, u64 hpa, u64 size)
{
 unsigned long rid = device_id & 0xffff;
 struct ti_slot *s;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s || !s->mmio_claimed || s->bar != hpa || s->bar_size != size ||
     control_command(rid, 2)) sbi_panic("COVE-IO: unsafe MMIO release\n");
 s->mmio_claimed = false; s->bar = 0; s->bar_size = 0;
 spin_unlock(&ti_lock);
}
static const struct sbi_cvm_mmio_bitmap_ops mmio_ops = {
 .init = sbi_cvm_bitmap_mmio_init, .claim = mmio_claim,
 .sync = sbi_cvm_bitmap_mmio_sync,
 .set_trusted = sbi_cvm_bitmap_mmio_set_trusted,
 .quiesce = mmio_quiesce, .unclaim = mmio_unclaim,
};

static void clear_device_irq(unsigned long file, unsigned long iid)
{
 unsigned long old_hstatus, old_isel;
 if (!iid || !file || file >= 8) return;
 old_hstatus = csr_read(CSR_HSTATUS);
 old_isel = csr_read(CSR_VSISELECT);
 csr_write(CSR_HSTATUS, (old_hstatus & ~HSTATUS_VGEIN) | (file << 12));
 csr_write(CSR_VSISELECT, 0x80 + (iid / 64) * 2);
 csr_clear(CSR_VSIREG, 1UL << (iid % 64));
 csr_write(CSR_VSISELECT, old_isel);
 csr_write(CSR_HSTATUS, old_hstatus);
}

/* File reservations survive descheduling. Only destruction releases them. */
static void clear_file(unsigned long file, bool enable)
{
 unsigned long status = csr_read(CSR_HSTATUS), isel = csr_read(CSR_VSISELECT);
 csr_write(CSR_HSTATUS, (status & ~HSTATUS_VGEIN) | (file << 12));
 for (unsigned int i = 0; i < 4; i++) {
  csr_write(CSR_VSISELECT, 0x80 + i * 2); csr_write(CSR_VSIREG, 0);
  csr_write(CSR_VSISELECT, 0xc0 + i * 2); csr_write(CSR_VSIREG, 0);
 }
 csr_write(CSR_VSISELECT, 0x70); csr_write(CSR_VSIREG, enable ? 1 : 0);
 csr_write(CSR_VSISELECT, 0x72); csr_write(CSR_VSIREG, 0);
 csr_write(CSR_VSISELECT, isel); csr_write(CSR_HSTATUS, status);
}
int sbi_cvm_iommu_file_reserve(unsigned long file, unsigned long owner)
{
 if (!active || !file || file >= 8 || !owner) return SBI_EINVAL;
 if (file_owners[file] && file_owners[file] != ~0UL)
  return file_owners[file] == owner ? 0 : SBI_EDENIED;
 clear_file(file, true);
 writeq(file, (void *)(CONTROL + 0x38));
 writeq(owner, (void *)(CONTROL + 0x30));
 if (readq((void *)(CONTROL + 0x30)) != owner) return SBI_EDENIED;
 file_owners[file] = owner;
 return 0;
}
int sbi_cvm_iommu_file_release(unsigned long file, unsigned long owner)
{
 if (!file) return 0;
 if (file >= 8 || !owner || file_owners[file] != owner) return SBI_EDENIED;
 for (unsigned int i = 0; i < DMA_SLOTS; i++)
  if (slots[i].used && slots[i].owner == owner && slots[i].file == file) return SBI_EDENIED;
 clear_file(file, false);
 writeq(file, (void *)(CONTROL + 0x38));
 writeq(0, (void *)(CONTROL + 0x30));
 file_owners[file] = 0;
 return 0;
}
int sbi_cvm_iommu_set_owner(unsigned long rid, unsigned long file, unsigned long owner)
{
 struct ti_slot *s = slot(rid);
 if (!s || !owner || file >= 8 || (s->owner && s->owner != owner)) return SBI_EDENIED;
 if (file && file_owners[file] != owner) return SBI_EDENIED;
 /* Retargeting a live device requires a drained lifecycle. Keeping the
  * first MSI file pinned prevents stale pending interrupts on the old CPU. */
 if (s->msi_iid && s->file != file) return SBI_EDENIED;
 s->owner = owner; s->file = file;
 return 0;
}
void sbi_cvm_iommu_guest_context(unsigned long owner)
{
 if (active) writeq(owner, (void *)(CONTROL + 0x20));
}

/* IRQ authorization ranges are Host-controlled and may be removed while a
 * stopped/disabled device still owns its first MSI identity. Consult the
 * trusted device lifetime reservation before emulated injection. */
bool sbi_cvm_iommu_irq_reserved(unsigned long owner, unsigned long file,
                                unsigned long iid)
{
 for (unsigned int i = 0; i < DMA_SLOTS; i++)
  if (slots[i].used && slots[i].owner == owner && slots[i].file == file &&
      slots[i].msi_iid == iid) return true;
 return false;
}

int sbi_cvm_iommu_platform_init(const void *fdt)
{
 int node = fdt_node_offset_by_compatible(fdt, -1, "riscv,iommu");
 u64 addr, size;
 struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
 /* Match the full firmware PMP envelope, including alignment padding. */
 unsigned long fw_end = (scratch->fw_start + scratch->fw_size + 0x1fffffUL) & ~0x1fffffUL;
 int ret;
 if (node < 0 || !fdt_getprop(fdt, node, "xs,cove-trusted", NULL)) return 0;
 if (sbi_hart_count() != 1 || fdt_get_node_addr_size(fdt, node, 0, &addr, &size) ||
     addr != CVM_IOMMU_REG_BASE || size != PAGE) return SBI_ENOTSUPP;
 /* Device reset is OFF before the Host executes. Static MMIO and ECAM have
  * no Host-controlled relocation/reset path. PMP protects even TVM bypass. */
 ret = sbi_domain_root_add_memrange(addr, PAGE, PAGE,
        SBI_DOMAIN_MEMREGION_M_READABLE | SBI_DOMAIN_MEMREGION_M_WRITABLE |
        SBI_DOMAIN_MEMREGION_MMIO);
 if (ret) return ret;
 ret = sbi_domain_root_add_memrange(CONTROL, PAGE, PAGE,
        SBI_DOMAIN_MEMREGION_M_READABLE | SBI_DOMAIN_MEMREGION_M_WRITABLE |
        SBI_DOMAIN_MEMREGION_MMIO);
 if (ret) return ret;
 if (readq((void *)CONTROL) != CONTROL_MAGIC) return SBI_ENOTSUPP;
 ret = sbi_domain_root_add_memrange(CVM_IOMMU_ECAM_BASE, 0x10000000UL, PAGE,
        SBI_DOMAIN_MEMREGION_M_READABLE | SBI_DOMAIN_MEMREGION_M_WRITABLE |
        SBI_DOMAIN_MEMREGION_MMIO);
 if (ret) return ret;
 ret = sbi_domain_root_add_memrange(CVM_IOMMU_POOL_BASE, CVM_IOMMU_POOL_SIZE, PAGE,
        SBI_DOMAIN_MEMREGION_M_READABLE | SBI_DOMAIN_MEMREGION_M_WRITABLE);
 if (ret) return ret;
 writeq(0, (void *)(addr + DDTP));
 next_page = CVM_IOMMU_POOL_BASE;
 directory = (void *)alloc_pages(1, PAGE);
 commands = (void *)alloc_pages(1, PAGE);
 host_root = alloc_pages(4, 4 * PAGE);
 if (!directory || !commands || !host_root) return SBI_ENOMEM;
 for (unsigned long pa = RAM_BASE; pa < CVM_IOMMU_POOL_BASE; pa += PAGE) {
  unsigned long *entry;
  if (pa < fw_end && pa + PAGE > scratch->fw_start) continue;
  entry = leaf(host_root, pa, true);
  if (!entry) return SBI_ENOMEM;
  *entry = pte(pa, RW);
 }
 /* Only the Host S-file. Guest files and all device/control MMIO are denied. */
 unsigned long *entry = leaf(host_root, MSI_ADDR, true);
 if (!entry) return SBI_ENOMEM;
 *entry = pte(MSI_ADDR, RW);
 writeq(((unsigned long)commands >> 2) | 7, (void *)(addr + CQB));
 writel(0, (void *)(addr + CQT));
 writel(1, (void *)(addr + CQCSR));
 if (!(readl((void *)(addr + CQCSR)) & (1UL << 16))) return SBI_EIO;
 writeq(((unsigned long)directory >> 2) | 2, (void *)(addr + DDTP));
 if ((readq((void *)(addr + DDTP)) & 15) != 2) return SBI_EIO;
 active = true;
 ret = sync_locked();
 if (ret) return ret;
 ret = sbi_cvm_mmio_bitmap_register(&mmio_ops);
 if (ret) return ret;
 sbi_printf("COVE-IO trusted IOMMU: OpenSBI owner, all PCI DMA blocked until context, pool=%lx\n", CVM_IOMMU_POOL_BASE);
 return 0;
}

int sbi_cvm_iommu_protect(unsigned long pa, bool protect)
{
 unsigned long *entry;
 int ret = 0;
 if (!active) return 0;
 if ((pa & (PAGE - 1)) || pa < RAM_BASE || pa >= CVM_IOMMU_POOL_BASE)
  return SBI_EINVAL;
 spin_lock(&ti_lock);
 entry = leaf(host_root, pa, false);
 if (!entry) ret = SBI_EDENIED; /* firmware and missing pages stay inaccessible */
 else {
  /* Reclaimed RAM must never retain an old private-device translation. */
  if (!protect) {
   for (unsigned int n = 0; n < DMA_SLOTS; n++) {
    struct ti_slot *s = &slots[n];
    if (!s->used) continue;
    for (unsigned int i = 0; i < 2048; i++) {
     if (!(s->root[i] & V)) continue;
     unsigned long *l1 = table(s->root[i]);
     for (unsigned int j = 0; j < 512; j++) {
      if (!(l1[j] & V)) continue;
      unsigned long *l0 = table(l1[j]);
      for (unsigned int k = 0; k < 512; k++)
       if ((l0[k] & V) && (unsigned long)table(l0[k]) == pa) l0[k] = 0;
     }
    }
   }
  }
  *entry = protect ? 0 : pte(pa, RW);
  ret = sync_locked();
 }
 spin_unlock(&ti_lock);
 return ret;
}
int sbi_cvm_iommu_claim(unsigned long rid)
{
 struct ti_slot *s = NULL;
 int ret = SBI_ENOSPC;
 if (!active) return 0;
 if (!rid || rid >= CVM_IOMMU_MAX_RIDS) return SBI_EINVAL;
 spin_lock(&ti_lock);
 if (slot(rid)) { ret = SBI_EALREADY; goto out; }
 for (unsigned int i = 0; i < DMA_SLOTS; i++)
  if (!slots[i].used) { s = &slots[i]; break; }
 if (!s) goto out;
 if (!s->root) s->root = (void *)alloc_pages(4, 4 * PAGE);
 if (!s->msi) s->msi = (void *)alloc_pages(1, PAGE);
 if (!s->root || !s->msi) { ret = SBI_ENOMEM; goto out; }
 /* Reuse the slot's existing table pages, clearing leaves, not leaking pages. */
 for (unsigned int i = 0; i < 2048; i++) {
  if (!s->root[i]) continue;
  unsigned long *l1 = table(s->root[i]);
  for (unsigned int j = 0; j < 512; j++)
   if (l1[j]) sbi_memset(table(l1[j]), 0, PAGE);
 }
 sbi_memset(s->msi, 0, PAGE);
 s->msi_iid = 0; s->mmio_claimed = false; s->bar = 0; s->bar_size = 0;
 s->used = true; s->started = false; s->rid = rid;
 s->file = 0; s->owner = 0;
 publish(rid, (unsigned long)s->root, 0, (unsigned long)s->msi, false);
 ret = sync_locked();
out:
 spin_unlock(&ti_lock);
 return ret;
}
int sbi_cvm_iommu_start(unsigned long rid)
{
 struct ti_slot *s;
 int ret = 0;
 if (!active) return 0;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s || !s->mmio_claimed) { ret = SBI_EDENIED; goto out; }
 if (s->mmio_claimed) {
  unsigned long cfg = CVM_IOMMU_ECAM_BASE + rid * PAGE;
  /* BAR decode and bus mastering are set by the Monitor before START. */
  writew((readw((void *)(cfg + 4)) | 6) & ~1U, (void *)(cfg + 4));
  program_msi(s);
 }
 s->started = true;
 publish(rid, (unsigned long)s->root, 0, (unsigned long)s->msi, true);
 ret = sync_locked();
out:
 spin_unlock(&ti_lock);
 return ret;
}
int sbi_cvm_iommu_map(unsigned long rid, unsigned long gpa, unsigned long hpa)
{
 struct ti_slot *s;
 unsigned long *entry;
 int ret = 0;
 if (!active) return 0;
 if ((gpa | hpa) & (PAGE - 1) || hpa < RAM_BASE || hpa >= CVM_IOMMU_POOL_BASE)
  return SBI_EINVAL;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s || !s->started) { ret = SBI_EDENIED; goto out; }
 entry = leaf((unsigned long)s->root, gpa, true);
 if (!entry) { ret = SBI_ENOMEM; goto out; }
 if (*entry && *entry != pte(hpa, RW)) { ret = SBI_EDENIED; goto out; }
 *entry = pte(hpa, RW);
 ret = sync_locked();
out:
 spin_unlock(&ti_lock);
 return ret;
}
int sbi_cvm_iommu_stop(unsigned long rid)
{
 struct ti_slot *s;
 int ret;
 if (!active) return 0;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s) { ret = 0; goto out; }
 s->started = false;
 publish(rid, (unsigned long)s->root, 0, (unsigned long)s->msi, false);
 ret = sync_locked();
 if (!ret) {
  sbi_memset(s->msi, 0, PAGE);
  clear_device_irq(s->file ? s->file : 1, s->msi_iid);
 }
out:
 spin_unlock(&ti_lock);
 return ret;
}
int sbi_cvm_iommu_release(unsigned long rid)
{
 struct ti_slot *s;
 int ret;
 if (!active) return 0;
 if (rid >= CVM_IOMMU_MAX_RIDS) return SBI_EINVAL;
 ret = sbi_cvm_iommu_stop(rid);
 if (ret) return ret;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (s && s->mmio_claimed) { spin_unlock(&ti_lock); return SBI_EDENIED; }
 if (s) s->used = false;
 /* Keep blocked; Host explicitly attaches a fresh context after cleanup. */
 host_valid[rid] = false;
 spin_unlock(&ti_lock);
 return 0;
}

/* MSI authorization is checked against trusted TDI/IRQ state by sbi_cvm.c. */
extern int cove_io_bind_imsic(unsigned long vmid, unsigned long file,
                              unsigned long gpa, unsigned long hart);
extern int cove_io_trusted_msi(unsigned long rid, unsigned long addr,
                             unsigned long target, unsigned long iid);
int sbi_cvm_iommu_ecall(unsigned long op, unsigned long a, unsigned long b,
                       unsigned long c, unsigned long d, unsigned long *value)
{
 int ret = 0;
 if (!active) return SBI_ENOTSUPP;
 *value = 0;
 if (op == CVM_IOMMU_PROBE) { *value = CVM_IOMMU_REG_BASE; return 0; }
 if (failed) return SBI_EIO;
 if (op == CVM_IOMMU_CAPABILITIES) { *value = readq((void *)CVM_IOMMU_REG_BASE); return 0; }
 if (op == CVM_IOMMU_BIND_FILE) return cove_io_bind_imsic(a, b, c, d);
 if (op == CVM_IOMMU_MSI) return cove_io_trusted_msi(a, b, c, d);
 spin_lock(&ti_lock);
 switch (op) {
 case CVM_IOMMU_HOST_CONTEXT:
  /* Host owns FSC only. G-stage always limits final RAM/table walk accesses.
   * A claimed RID never accepts Host replacement, including Bare/invalid. */
  if (a >= CVM_IOMMU_MAX_RIDS || slot(a) || c > 1 ||
      ((b >> 60) != 0 && (b >> 60) != 8 && (b >> 60) != 9 && (b >> 60) != 10) ||
      (b & 0x0ffff00000000000UL)) { ret = SBI_EDENIED; break; }
  host_fsc[a] = b; host_valid[a] = c;
  host_context(a); ret = sync_locked(); break;
 case CVM_IOMMU_SYNC:
  ret = sync_locked(); break;
 case CVM_IOMMU_PCI_READ:
 case CVM_IOMMU_PCI_WRITE: {
  unsigned long off = b & 0xfff, rid = b >> 12;
  unsigned long pa = CVM_IOMMU_ECAM_BASE + b;
  if (a != CVM_IOMMU_ECAM_BASE || b >= CVM_IOMMU_MAX_RIDS * PAGE ||
      (c != 1 && c != 2 && c != 4) || (off & (c - 1)) || off + c > PAGE) {
   ret = SBI_EINVAL; break;
  }
  if (op == CVM_IOMMU_PCI_READ) {
   *value = c == 1 ? readb((void *)pa) : c == 2 ? readw((void *)pa) : readl((void *)pa);
  } else {
   /* No bridge renumbering/reset, ATS/PASID/PRI or extended-capability writes.
    * Bound BARs cannot be relocated/probed. MSI address/data remain untrusted
    * requests but IOMMU restricts their destination to the authorized file. */
   if ((rid == 0 && off >= 0x10) || off >= 0x100 ||
       (slot(rid) && off < 0x34 && off + c > 0x10)) {
    ret = SBI_EDENIED; break;
   }
   for (unsigned int n = 0; n < DMA_SLOTS; n++) {
    if (slots[n].used && slots[n].mmio_claimed &&
        off < 0x34 && off + c > 0x10) { ret = SBI_EDENIED; break; }
   }
   if (ret) break;
   struct ti_slot *s = slot(rid);
   if (s && s->mmio_claimed) {
    unsigned long cap = msi_cap(rid);
    /* Host may disable the endpoint (availability is not guaranteed), but
     * cannot change decode, power state, reset it, or select an MSI target.
     * MSI writes are accepted as requests and replaced with the authorized
     * source address/IID. No arbitrary Host MSI data reaches the VS-file. */
    if (cap && off >= cap + 2 && off + c <= cap + 14) {
     if (off == cap + 2 && c == 2) {
      u32 old = readw((void *)pa);
      writew((old & ~1U) | (d & 1), (void *)pa);
     }
     program_msi(s);
     break;
    }
    if (off == 4 && c == 2) {
     writew((readw((void *)pa) & ~4U) | (d & 4), (void *)pa);
     break;
    }
    ret = SBI_EDENIED; break;
   }
   if (c == 1) writeb(d, (void *)pa);
   else if (c == 2) writew(d, (void *)pa);
   else writel(d, (void *)pa);
  }
  break;
 }
 case CVM_IOMMU_STATUS:
  *value = failed ? 0 : 1; break;
 default: ret = SBI_ENOTSUPP;
 }
 spin_unlock(&ti_lock);
 return ret;
}

/* Called only after Monitor authorization, never a raw Host PTE write ABI. */
int sbi_cvm_iommu_msi_install(unsigned long rid, unsigned long addr,
                             unsigned long target, unsigned long iid)
{
 struct ti_slot *s;
 int ret = 0;
 if (!active) return 0;
 if (rid >= CVM_IOMMU_MAX_RIDS || addr != MSI_ADDR ||
     (target && (target < MSI_FILE || target >= MSI_ADDR + 8 * PAGE ||
                  (target & (PAGE - 1)) || !iid || iid > 255))) return SBI_EINVAL;
 spin_lock(&ti_lock);
 s = slot(rid);
 if (!s) { ret = target ? SBI_EDENIED : 0; goto out; }
 if (target && s->owner && (!s->file || file_owners[s->file] != s->owner ||
      target != MSI_ADDR + s->file * PAGE)) { ret = SBI_EDENIED; goto out; }
 if (target && !s->owner && target != MSI_FILE) { ret = SBI_EINVAL; goto out; }
 if (target && !s->mmio_claimed) { ret = SBI_EDENIED; goto out; }
 /* Pin the first authorized IID until the endpoint is completely released.
  * Retargeting requires a fresh lifecycle so stale pending bits cannot cross
  * owners. Temporary MSI disable keeps the identity protected. */
 if (target && s->msi_iid && s->msi_iid != iid) { ret = SBI_EDENIED; goto out; }
 /* Validate peer identity before revoking the caller's live MSI entry. */
 if (target) {
  for (unsigned int n = 0; n < DMA_SLOTS; n++)
   if (&slots[n] != s && slots[n].used && slots[n].msi_iid == iid &&
       slots[n].file == s->file) {
    ret = SBI_EDENIED; goto out;
   }
 }
 s->msi[0] = 0; mb();
 ret = sync_locked();
 if (ret) goto out;
 clear_device_irq(s->file ? s->file : 1, s->msi_iid);
 if (target) {
  if (s->mmio_claimed) {
   writeq(rid, (void *)(CONTROL + 8));
   writeq(s->file ? s->file : 1, (void *)(CONTROL + 0x38));
   writeq(iid, (void *)(CONTROL + 0x28));
   if (readq((void *)(CONTROL + 0x28)) != iid) { ret = SBI_EDENIED; goto out; }
  }
  s->msi_iid = iid;
  /* Discard any pending bit injected before this identity was reserved. */
  clear_device_irq(s->file ? s->file : 1, iid);
  program_msi(s);
  s->msi[0] = pte(target, 7); /* BASIC=3, V=1 */
 }
 ret = sync_locked();
out:
 spin_unlock(&ti_lock);
 return ret;
}
