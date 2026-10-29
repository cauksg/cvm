/* SPDX-License-Identifier: BSD-2-Clause */
/* Legacy ABI gateway: snapshot controls before mutation, never trust donated
 * list metadata after return. Fixed platform bounds are deliberate. */
#define CVM_HOST_PAGES (0x40000000UL / PAGE_SIZE)
#define CVM_HOST_CHUNKS 256
#define CVM_HOST_BATCH 32
#define CVM_HOST_LOAD_PAGES (0x20000000UL / PAGE_SIZE)
struct cvm_host_chunk {
 bool used;
 unsigned long descriptor, array;
 struct cvm_mem_chunk_infor info;
 unsigned long pages[512];
};
static struct cvm_host_chunk *host_chunks;
static unsigned long donated[CVM_HOST_PAGES / 64];
static unsigned long pending_donation[CVM_HOST_PAGES / 64];
static unsigned long host_chunk_ptrs[CVM_HOST_BATCH], host_roots[32];
static unsigned long *host_load_pages;
static unsigned int pending_chunks[CVM_HOST_BATCH];
static bool host_memory_ready;
/* Never reuse an accepted identity during this boot (also bounds hardware VMIDs). */
static unsigned long host_last_identity;

static int host_copy(void *dst, unsigned long pa, unsigned long size,
                     unsigned long alignment)
{
 if ((pa & (alignment - 1)) || !sbi_cvm_iommu_host_buffer(pa, size))
  return SBI_EDENIED;
 sbi_memcpy(dst, (void *)pa, size);
 return 0;
}
static int host_donation_range(unsigned long pa, unsigned long pages)
{
 if (!pages || (pa & (PAGE_SIZE - 1)) || pages > CVM_HOST_PAGES ||
     !sbi_cvm_iommu_host_buffer(pa, pages * PAGE_SIZE)) return SBI_EDENIED;
 for (unsigned long i = 0; i < pages; i++) {
  unsigned long index = (pa - 0x80000000UL) / PAGE_SIZE + i;
  unsigned long mask = 1UL << (index % 64);
  /* A public shared HPA is still owned by its accepted Guest leaf. Host
   * accessibility alone must not let a refill repurpose it as private
   * allocator or page-table memory while that leaf remains installed.
   * Trusted initialization permits only the flat ownership table. */
  if (page_own_table && (ownership_table_level || index >= own_max_num ||
      page_own_table[index].vmidp)) return SBI_EDENIED;
  if ((donated[index / 64] | pending_donation[index / 64]) & mask)
   return SBI_EDENIED;
  pending_donation[index / 64] |= mask;
 }
 return 0;
}
static int host_prepare_chunks(struct cvm_list_params *list, bool refill)
{
 unsigned long addresses[CVM_HOST_BATCH];
 int ret;
 if (!list->ele_num || list->ele_num > CVM_HOST_BATCH ||
     (ret = host_copy(addresses, list->addr, list->ele_num * 8, 8)))
  return SBI_EDENIED;
 for (unsigned long i = 0; i < list->ele_num; i++) {
  unsigned int n;
  for (n = 0; n < CVM_HOST_CHUNKS; n++) {
   bool reserved = false;
   for (unsigned long j = 0; j < i; j++) if (pending_chunks[j] == n) reserved = true;
   if (!host_chunks[n].used && !reserved) break;
  }
  if (n == CVM_HOST_CHUNKS) return SBI_ENOSPC;
  pending_chunks[i] = n;
  struct cvm_host_chunk *s = &host_chunks[n];
  if ((ret = host_copy(&s->info, addresses[i], sizeof(s->info), PAGE_SIZE))) return ret;
  s->descriptor = addresses[i]; s->array = (unsigned long)s->info.paddr_list;
  if (s->info.type != (refill ? 1U : 0U) ||
      (ret = host_copy(s->pages, s->array, PAGE_SIZE, PAGE_SIZE))) return SBI_EDENIED;
  if ((ret = host_donation_range(s->descriptor, 1)) ||
      (ret = host_donation_range(s->array, 1))) return ret;
  for (unsigned int j = 0; j < 512; j++)
   if ((ret = host_donation_range(s->pages[j], 1))) return ret;
  s->info.paddr_list = s->pages;
  host_chunk_ptrs[i] = (unsigned long)&s->info;
 }
 list->addr = (unsigned long)host_chunk_ptrs;
 return 0;
}
static void host_commit_donation(unsigned long count)
{
 for (unsigned long word = 0; word < CVM_HOST_PAGES / 64; word++) {
  unsigned long bits = pending_donation[word];
  while (bits) {
   unsigned int bit = 0;
   while (!(bits & (1UL << bit))) bit++;
   set_bitmap(0x80000000UL + (word * 64 + bit) * PAGE_SIZE);
   bits &= bits - 1;
  }
  donated[word] |= pending_donation[word];
 }
 for (unsigned long i = 0; i < count; i++) host_chunks[pending_chunks[i]].used = true;
}
static void host_release_chunk(struct cvm_mem_chunk_infor *info)
{
 for (unsigned int n = 0; n < CVM_HOST_CHUNKS; n++) {
  struct cvm_host_chunk *s = &host_chunks[n];
  if (!s->used || &s->info != info) continue;
  for (unsigned int i = 0; i < 514; i++) {
   unsigned long pa = i < 512 ? s->pages[i] : i == 512 ? s->array : s->descriptor;
   unsigned long index = (pa - 0x80000000UL) / PAGE_SIZE;
   reset_bitmap(pa);
   donated[index / 64] &= ~(1UL << (index % 64));
  }
  sbi_memset(s, 0, sizeof(*s));
  return;
 }
 sbi_panic("CVM: missing trusted donation record\n");
}
static int host_init_memory(struct sbi_trap_regs *regs)
{
 struct cvm_list_params chunks, roots, bmp, own;
 int ret;
 if (host_memory_ready || sbi_cvm_bitmap_active()) return SBI_EALREADY;
 if (!host_chunks) host_chunks = sbi_cvm_iommu_alloc(sizeof(*host_chunks) * CVM_HOST_CHUNKS);
 if (!host_load_pages) host_load_pages = sbi_cvm_iommu_alloc(CVM_HOST_LOAD_PAGES * 8);
 if (!host_chunks || !host_load_pages) return SBI_ENOMEM;
 if ((ret = host_copy(&chunks, regs->a0, sizeof(chunks), 8)) ||
     (ret = host_copy(&roots, regs->a1, sizeof(roots), 8)) ||
     (ret = host_copy(&bmp, regs->a2, sizeof(bmp), 8)) ||
     (ret = host_copy(&own, regs->a3, sizeof(own), 8))) return ret;
 sbi_memset(pending_donation, 0, sizeof(pending_donation));
 if (bmp.level || bmp.ele_num != CVM_HOST_PAGES / 64 || bmp.page_num != 8 ||
     own.level || own.ele_num != CVM_HOST_PAGES || own.page_num != 512 ||
     !roots.ele_num || roots.ele_num > 32) return SBI_EINVAL;
 if ((ret = host_donation_range(bmp.addr, bmp.page_num)) ||
     (ret = host_donation_range(own.addr, own.page_num)) ||
     (ret = host_copy(host_roots, roots.addr, roots.ele_num * 8, 8))) return ret;
 for (unsigned long i = 0; i < roots.ele_num; i++)
  if ((host_roots[i] & (4 * PAGE_SIZE - 1)) ||
      (ret = host_donation_range(host_roots[i], 4))) return SBI_EDENIED;
 if ((ret = host_prepare_chunks(&chunks, false))) return ret;
 roots.addr = (unsigned long)host_roots;
 /* No reusable failure after commit. A hardware revoke failure retains pages. */
 if (init_bitmap(&bmp) || init_page_own_table(&own)) sbi_panic("CVM memory init\n");
 host_commit_donation(chunks.ele_num);
 if (init_cvm_memorypool(&chunks) || init_root_pt_list(&roots) ||
     sbi_cvm_bitmap_init((unsigned long)bitmap)) sbi_panic("CVM memory commit\n");
 host_memory_ready = true;
 return 0;
}
int sbi_cvm_host_ecall(unsigned long fid, struct sbi_trap_regs *regs)
{
 struct iie_cvm_sbi_params p;
 struct cvm_node *node;
 int ret;
 if (check_in_cmode()) return SBI_EDENIED;
 if (fid == SBI_EXT_CVM_INIT_PAGE_LIST) return host_init_memory(regs);
 if (fid == SBI_EXT_CVM_REFILL_MEMORY_POOL) {
  struct cvm_list_params chunks;
  if ((ret = host_copy(&chunks, regs->a0, sizeof(chunks), 8))) return ret;
  if (!host_memory_ready) return SBI_EDENIED;
  sbi_memset(pending_donation, 0, sizeof(pending_donation));
  if ((ret = host_prepare_chunks(&chunks, true))) return ret;
  host_commit_donation(chunks.ele_num);
  return refill_memory_pool(&chunks);
 }
 if (fid == SBI_EXT_CVM_LOAD_FILE || fid == SBI_EXT_CVM_RETRY_LOAD) {
  struct iie_cvm_sbi_params_load load;
  if ((ret = host_copy(&load, regs->a0, sizeof(load), 8))) return ret;
  node = cvm_registered(load.vmid_ptr);
  if (!node || node->cvm.state != INITIALIZING || !node->cvm.root_pt) return SBI_EDENIED;
  if (!load.count || load.count > CVM_HOST_LOAD_PAGES ||
      (load.des_gpa & (PAGE_SIZE - 1)) || load.des_gpa < 0x80000000UL ||
      load.des_gpa >= 0xc0000000UL || load.count > (0xc0000000UL-load.des_gpa)/PAGE_SIZE)
   return SBI_EINVAL;
  if ((ret = host_copy(host_load_pages, (unsigned long)load.src_hpa_array,
                       load.count * 8, 8))) return ret;
  for (unsigned long i = 0; i < load.count; i++)
   if ((host_load_pages[i] & (PAGE_SIZE - 1)) ||
       !sbi_cvm_iommu_host_buffer(host_load_pages[i], PAGE_SIZE)) return SBI_EDENIED;
  load.src_hpa_array = host_load_pages;
  return fid == SBI_EXT_CVM_LOAD_FILE ? load_file(&load) : retry_load_after_refill(&load);
 }
 if (fid == SBI_EXT_CVM_INIT_SWIOTLB) {
  struct iie_cvm_sbi_params_swiotlb sw;
  if ((ret = host_copy(&sw, regs->a0, sizeof(sw), 8))) return ret;
  if (!cvm_registered((void *)regs->a1)) return SBI_EDENIED;
  return init_swiotlb_params(&sw, (void *)regs->a1);
 }
 if (fid == SBI_EXT_CVM_CREATE_MEMORY_REGION || fid == SBI_EXT_CVM_INIT_MEM_POOL ||
     fid == SBI_EXT_CVM_HASH_IMAGE || fid == SBI_EXT_CVM_ATTEST || fid == SBI_EXT_CVM_TEST)
  return SBI_ENOTSUPP;
 if (fid != SBI_EXT_CVM_CREATE && fid != SBI_EXT_CVM_ALLOC_ROOT_PT &&
     fid != SBI_EXT_RECYCLE_MEMORY && fid != SBI_EXT_CVM_DESTROY) return SBI_ENOTSUPP;
 if ((ret = host_copy(&p, regs->a0, sizeof(p), 8))) return ret;
 if (!host_memory_ready) return SBI_EDENIED;
 node = cvm_registered(p.vmid_ptr);
 if (fid == SBI_EXT_CVM_CREATE) {
  struct kvm_vmid id;
  if (node) return SBI_EALREADY;
  if ((ret = host_copy(&id, (unsigned long)p.vmid_ptr, sizeof(id), 8))) return ret;
  if (!id.cvm_id || id.cvm_id >= (1UL << 14) || id.vmid >= (1UL << 14))
   return SBI_EINVAL;
  if (id.cvm_id <= host_last_identity) return SBI_EDENIED;
  unsigned int live = 0;
  for (struct cvm_node *n = iie_cvm_list_head ? iie_cvm_list_head->next : NULL; n; n = n->next) live++;
  if (live >= 5) return SBI_ENOSPC;
  host_create_id = id;
  ret = sbi_cvm_create(&p);
  if (!ret) host_last_identity = id.cvm_id;
  return ret;
 }
 if (!node) return SBI_EDENIED;
 if (fid == SBI_EXT_CVM_ALLOC_ROOT_PT) {
  if (node->cvm.state != INITIALIZING) return SBI_EDENIED;
  return init_cvm_vcpu_rootptAndChunk(&p);
 }
 if (fid == SBI_EXT_RECYCLE_MEMORY) {
  struct cvm_list_params output;
  unsigned long values[CVM_HOST_CHUNKS];
  if ((ret = host_copy(&output, regs->a1, sizeof(output), 8))) return ret;
  if (!output.ele_num || output.ele_num > 1024 ||
      !sbi_cvm_iommu_host_buffer(output.addr, output.ele_num * 8) || (output.addr & 7))
   return SBI_EDENIED;
  unsigned long count = 0;
  if (!node->cvm.root_pt || node->cvm.state == RECLAIMED) return SBI_EDENIED;
  for (struct cvm_mem_chunk_node *c = node->cvm.used_chunk_list_head->next; c; c = c->next)
   if (c->chunk_infor->type) count++;
  if (count > output.ele_num) return SBI_ENOSPC;
  unsigned long pa = output.addr;
  if ((pa < regs->a0 + sizeof(p) && regs->a0 < pa + count * 8) ||
      (pa < regs->a1 + sizeof(output) && regs->a1 < pa + count * 8)) return SBI_EDENIED;
  output.addr = (unsigned long)values;
  if ((ret = recycle_memory(&p, &output))) return ret;
  sbi_memcpy((void *)pa, values, output.ele_num * 8);
  output.addr = pa;
  sbi_memcpy((void *)regs->a1, &output, sizeof(output));
  return 0;
 }
 if (node->cvm.root_pt) return SBI_EDENIED;
 return sbi_cvm_destroy(&p);
}
