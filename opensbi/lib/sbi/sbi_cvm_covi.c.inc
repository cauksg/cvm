/* SPDX-License-Identifier: BSD-2-Clause */
/* Single physical hart COVI subset: dedicated files retained until destroy. */
struct cvm_aia_params {
 unsigned long base;
 u32 group_bits, group_shift, hart_bits, guest_bits, guests;
};
static unsigned char covi_files[8]; /* host, converting, fenced, bound */
static bool covi_global_pending;
static void covi_forget_file(unsigned long file)
{
 if (file && file < 8) covi_files[file] = 0;
}
int sbi_cvm_covi_fence(unsigned long fid)
{
 if (!sbi_cvm_iommu_active() || check_in_cmode()) return SBI_ENOTSUPP;
 if (fid == 3) {
  if (covi_global_pending) return SBI_EALREADY_STARTED;
  covi_global_pending = true;
  return 0;
 }
 if (fid != 4 || !covi_global_pending) return SBI_EINVAL;
 /* Boot policy refuses multiple physical harts. Flush completes this epoch. */
 __sbi_hfence_gvma_all();
 __sbi_hfence_vvma_all();
 asm volatile("fence rw,rw" ::: "memory");
 for (unsigned int i = 1; i < 8; i++)
  if (covi_files[i] == 1) covi_files[i] = 2;
 covi_global_pending = false;
 return 0;
}
static void covi_write_pending(struct cvm_imsic_context *ctx, unsigned long iid)
{
 unsigned long h = csr_read(CSR_HSTATUS), sel = csr_read(CSR_VSISELECT);
 csr_write(CSR_HSTATUS, (h & ~HSTATUS_VGEIN) | (ctx->file << 12));
 csr_write(CSR_VSISELECT, 0x80 + 2 * (iid / 64));
 csr_set(CSR_VSIREG, 1UL << (iid % 64));
 csr_write(CSR_VSISELECT, sel);
 csr_write(CSR_HSTATUS, h);
}
int sbi_cvm_covi_ecall(unsigned long fid, unsigned long a,
                         unsigned long b, unsigned long c)
{
 struct cvm_node *n;
 struct cvm_vcpu_node *v;
 int ret;
 if (!sbi_cvm_iommu_active() || check_in_cmode()) return SBI_ENOTSUPP;
 if (fid == 2 || fid == 3) {
  unsigned long f;
  if (a <= 0x28000000UL || a >= 0x28008000UL || (a & (PAGE_SIZE - 1)))
   return SBI_EINVAL;
  f = (a - 0x28000000UL) / PAGE_SIZE;
  if (fid == 2) {
   if (covi_files[f] || covi_global_pending) return SBI_EDENIED;
   ret = sbi_cvm_iommu_file_reserve(f, ~0UL);
   if (ret) return ret;
   covi_files[f] = 1;
   return 0;
  }
  if (covi_files[f] != 2) return SBI_EDENIED;
  ret = sbi_cvm_iommu_file_release(f, ~0UL);
  if (!ret) covi_files[f] = 0;
  return ret;
 }
 n = get_cvm(a);
 if (!n) return SBI_EINVAL;
 if (fid == 0) {
  struct cvm_aia_params p;
  if (n->cvm.state != INITIALIZING || n->cvm.aia_initialized ||
      c != sizeof(p) || (b & 7) || !sbi_cvm_iommu_host_buffer(b, c))
   return SBI_EINVAL;
  sbi_memcpy(&p, (void *)b, sizeof(p));
  sbi_printf("COVI AIA: tvm=%lu base=%lx groups=%u shift=%u harts=%u guests=%u nested=%u\n", a, p.base, p.group_bits, p.group_shift, p.hart_bits, p.guest_bits, p.guests);
  if (p.base != 0x08000000UL || p.group_bits || p.guest_bits || p.guests ||
      p.hart_bits > 2 || p.group_shift < 24 || p.group_shift > 63)
   return SBI_ENOTSUPP;
  n->cvm.aia_base = p.base;
  n->cvm.aia_hart_bits = p.hart_bits;
  n->cvm.aia_initialized = true;
  return 0;
 }
 if (b >= CVM_MAX_VCPUS || !(v = get_cvm_vcpu_node(a, b))) return SBI_EINVAL;
 if (fid == 1) {
  if (n->cvm.state != INITIALIZING || !n->cvm.aia_initialized ||
      v->vcpu.imsic.configured || c < n->cvm.aia_base ||
      c >= n->cvm.aia_base + (PAGE_SIZE << n->cvm.aia_hart_bits) ||
      (c & (PAGE_SIZE - 1))) return SBI_EINVAL;
  for (struct cvm_vcpu_node *p = n->cvm.cvm_vcpu_list_head->next; p; p = p->next)
   if (p->vcpu.imsic.configured && p->vcpu.imsic.gpa == c) return SBI_EDENIED;
  v->vcpu.imsic.gpa = c;
  v->vcpu.imsic.configured = true;
  return 0;
 }
 if (fid == 4) {
  unsigned long f = 0, pa;
  pt_entry_t *entry;
  if (n->cvm.state != READY || !v->vcpu.imsic.configured ||
      !c || (c & 1) || (c & (c - 1)) || c > (1UL << 7) ||
      v->vcpu.imsic.file) return SBI_EINVAL;
  while (!(c & (1UL << f))) f++;
  if (covi_files[f] != 2) return SBI_EDENIED;
  pa = 0x28000000UL + f * PAGE_SIZE;
  ret = find_pte(&n->cvm, v->vcpu.imsic.gpa, &entry);
  if (ret) return ret;
  if (*entry) return SBI_EDENIED;
  ret = sbi_cvm_iommu_file_reserve(f, a);
  if (ret) return ret;
  *entry = ((pa >> PAGE_SHIFT) << PAGE_PFN_SHIFT) |
            PTE_U | PTE_R | PTE_W | PTE_A | PTE_D | PTE_V;
  v->vcpu.imsic.file = f;
  v->vcpu.imsic.hart = current_hartid();
  covi_files[f] = 3;
  if (!b) n->cvm.imsic_file = f;
  for (unsigned int i = 0; i < 4; i++)
   for (unsigned int bit = 0; bit < 64; bit++)
    if (v->vcpu.imsic.pending[i] & (1UL << bit))
     covi_write_pending(&v->vcpu.imsic, 64 * i + bit);
  sbi_memset(v->vcpu.imsic.pending, 0, sizeof(v->vcpu.imsic.pending));
  __sbi_hfence_gvma_all();
  sbi_printf("CVM IMSIC owner: cvm=%lu file=%lu gpa=%lx target=%lx vcpu=%lu\n",
             a, f, v->vcpu.imsic.gpa, pa, b);
  return 0;
 }
 if (fid == 7) {
  if (!c || c >= CVM_IMSIC_IDS ||
      !(v->vcpu.imsic.allowed[c / 64] & (1UL << (c % 64)))) return SBI_EDENIED;
  if (sbi_cvm_iommu_irq_reserved(a, v->vcpu.imsic.file, c))
   return SBI_EDENIED;
  /* Guest consent to emulated IRQs never authorizes the Host to forge an
   * assigned device's IID. Those arrive only through its trusted RID route. */
  for (unsigned int i = 0; i < COVE_IO_MAX_TDIS; i++) {
   struct cove_io_tdi_entry *tdi = &cove_io_tdis[i];
   if (!tdi->valid || !cove_io_tdi_owned_by(tdi, n->cvm.vmid) ||
       !cove_io_uses_trusted_iommu(tdi->device_id)) continue;
   for (unsigned int j = 0; j < tdi->irq_range_count; j++)
    if (tdi->irq_ranges[j].vcpu_id == b && tdi->irq_ranges[j].irq_iid == c)
     return SBI_EDENIED;
  }
  if (v->vcpu.imsic.file) covi_write_pending(&v->vcpu.imsic, c);
  else v->vcpu.imsic.pending[c / 64] |= 1UL << (c % 64);
  return 0;
 }
 return SBI_ENOTSUPP;
}

static int cvm_covi_guest(struct sbi_trap_regs *r, bool *handled)
{
 struct cvm_vcpu_node *self = get_cvm_vcpu_node(get_cvm_id(), get_cvm_vcpu_id());
 struct cvm_vcpu_node *target;
 int ret = 0;
 unsigned long value = 0;
 if (!self) return SBI_EINVAL;
 if (r->a7 == SBI_EXT_COVG && (r->a6 == 4 || r->a6 == 5)) {
  bool allow = r->a6 == 4;
  if (r->a0 == ~0UL) {
   for (unsigned int i = 0; i < 4; i++) self->vcpu.imsic.allowed[i] = allow ? ~0UL : 0;
   self->vcpu.imsic.allowed[0] &= ~1UL;
  } else if (!r->a0 || r->a0 >= CVM_IMSIC_IDS) ret = SBI_EINVAL;
  else if (allow) self->vcpu.imsic.allowed[r->a0 / 64] |= 1UL << (r->a0 % 64);
  else self->vcpu.imsic.allowed[r->a0 / 64] &= ~(1UL << (r->a0 % 64));
  if (!ret) {
   *handled = true;
   return cvm_vcpu_exit(r);
  }
 } else if (r->a7 == SBI_EXT_HSM) {
  switch (r->a6) {
  case SBI_EXT_HSM_HART_START:
   target = r->a0 < CVM_MAX_VCPUS ? get_cvm_vcpu_node(get_cvm_id(), r->a0) : NULL;
   if (!target || (r->a1 & 1) || !cvm_boot_page(self->vcpu.cvm, r->a1, PTE_X)) {
    ret = SBI_EINVAL; break;
   }
   if (target->vcpu.hart_started) { ret = SBI_EALREADY; break; }
   sbi_memset(&target->vcpu.guest_context, 0, sizeof(target->vcpu.guest_context));
   sbi_memset(&target->vcpu.guest_csr, 0, sizeof(target->vcpu.guest_csr));
   sbi_memset(&target->vcpu.guest_fp, 0, sizeof(target->vcpu.guest_fp));
   target->vcpu.guest_viselect = 0;
   target->vcpu.guest_context.sepc = r->a1;
   target->vcpu.guest_context.a0 = r->a0;
   target->vcpu.guest_context.a1 = r->a2;
   target->vcpu.guest_context.sstatus = MSTATUS_SPP | MSTATUS_SPIE;
   target->vcpu.guest_context.hstatus = HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_VTW;
   target->vcpu.guest_csr.scounteren = 7;
   target->vcpu.guest_csr.vstimecmp = ~0UL;
   target->vcpu.hart_started = true;
   target->vcpu.state_finalized = true;
   target->vcpu.resume_len = 0;
   target->vcpu.resume_in_mask = target->vcpu.resume_out_mask = 0;
   *handled = true;
   sbi_printf("CVM trusted HSM START: cvm=%lu vcpu=%lu\n", (unsigned long)get_cvm_id(), r->a0);
   return cvm_vcpu_exit(r);
  case SBI_EXT_HSM_HART_STOP:
   self->vcpu.hart_started = false;
   *handled = true;
   return cvm_vcpu_exit(r);
  case SBI_EXT_HSM_HART_GET_STATUS:
   target = r->a0 < CVM_MAX_VCPUS ? get_cvm_vcpu_node(get_cvm_id(), r->a0) : NULL;
   if (!target) ret = SBI_EINVAL;
   else value = target->vcpu.hart_started ? 0 : 1;
   break;
  default: ret = SBI_ENOTSUPP;
  }
 } else if (r->a7 == SBI_EXT_RFENCE) {
  /* On this one-hart platform every resume flushes Guest translations too. */
  if (r->a6 > 2) ret = SBI_ENOTSUPP;
  else {
   asm volatile("fence.i" ::: "memory");
   __sbi_hfence_vvma_all();
  }
 } else return 0;
 *handled = true;
 r->a0 = ret; r->a1 = value; r->mepc += 4;
 return 0;
}
