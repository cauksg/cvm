/* Freestanding end-to-end tests: adapted from bitmap/tests/bitmap; real QEMU executes these instructions. */
#include <stdint.h>
#include <stddef.h>

#ifndef SCENARIO
#define SCENARIO 0
#endif
#define READ_CSR(csr) ({ uint64_t x; __asm__ volatile("csrr %0, " #csr : "=r"(x)); x; })
#define WRITE_CSR(csr, x) __asm__ volatile("csrw " #csr ", %0" :: "r"((uint64_t)(x)) : "memory")
#define PA 0x80403000UL
#define VA 0x40000000UL
#define BIGVA 0x60003000UL
#define GIGAVA 0x100403000UL
#define DATA 0x1122334455667788UL
#define RWX 0xcfUL
#define U 0x10UL
#define ALIGN __attribute__((aligned(4096)))

volatile uint64_t probe_active, trap_cause, trap_tval, trap_value, trap_epc;
static uint8_t bitmap[96 * 1024] ALIGN;
static uint64_t root[512] ALIGN, l1[512] ALIGN, l0[512] ALIGN;
static uint64_t big[512] ALIGN;
static uint64_t groot[2048] __attribute__((aligned(16384)));
static uint64_t gl1[512] ALIGN, gl0[512] ALIGN;
static unsigned passed;
extern void run_mode(uint64_t, uint64_t, uint64_t, uint64_t);
extern void probe_load(void), probe_store(void), probe_amo(void), probe_lr(void);
extern void probe_sc(void), probe_fetch(void), probe_cbo(void), probe_vector(void);
extern void probe_vector_store(void), probe_mprv_load(void), probe_mprv_store(void);
extern void probe_hlv(void);

static void puts_(const char *s) {
  while (*s) *(volatile uint8_t *)0x10000000 = (unsigned char)*s++;
}
static void hex(uint64_t n) {
  const char *digits = "0123456789abcdef";
  puts_("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    *(volatile uint8_t *)0x10000000 = digits[(n >> shift) & 15];
}
static __attribute__((noreturn)) void finish(unsigned status) {
  *(volatile uint32_t *)0x100000 = status ? (status << 16) | 0x3333 : 0x5555;
  for (;;) {}
}
void fatal_trap(uint64_t cause, uint64_t epc, uint64_t tval) {
  puts_("UNEXPECTED cause="); hex(cause); puts_(" epc="); hex(epc);
  puts_(" tval="); hex(tval); puts_("\n"); finish(255);
}
static void require(const char *name, int ok) {
  if (!ok) { puts_("FAIL "); puts_(name); puts_("\n"); finish(1); }
  puts_("PASS "); puts_(name); puts_("\n"); passed++;
}
static void check(const char *name, void (*fn)(void), uint64_t addr, uint64_t value,
                  unsigned mode, unsigned cause, uint64_t tval) {
  run_mode((uint64_t)fn, addr, value, mode);
  if (trap_cause != cause || (cause != 9 && cause != 8 && cause != 11 && trap_tval != tval)) {
    puts_("FAIL "); puts_(name); puts_(" cause="); hex(trap_cause);
    puts_(" tval="); hex(trap_tval); puts_(" epc="); hex(trap_epc);
    puts_(" expected="); hex(cause); puts_("/"); hex(tval); puts_("\n"); finish(1);
  }
  puts_("PASS "); puts_(name); puts_("\n"); passed++;
}
static void bclear(void);
static void bit(uint64_t pa, int protected) {
  unsigned ppn = pa >> 12;
  if (protected) bitmap[ppn/8] |= 1u << (ppn%8);
  else bitmap[ppn/8] &= ~(1u << (ppn%8));
  __asm__ volatile("fence rw,rw" ::: "memory");
  bclear(); /* QEMU caches permissions: publish every bitmap update. */
}
static uint64_t leaf(uint64_t pa, uint64_t flags) { return (pa >> 12 << 10) | flags; }
static uint64_t table(void *p) { return leaf((uint64_t)p, 1); }
static void flush(void) { __asm__ volatile("sfence.vma" ::: "memory"); }
static void bclear(void) {
  uint64_t x = READ_CSR(0xbc2);
  WRITE_CSR(0xbc2, x | 2);
}
static void cmode(unsigned enabled) {
  WRITE_CSR(0xbc2, (READ_CSR(0xbc2) & ~1UL) | enabled);
}
static void setup(void) {
  WRITE_CSR(pmpaddr0, 0x3fffffffffffUL);
  WRITE_CSR(pmpcfg0, 0x1f); /* S/U may access RAM and UART. */
  WRITE_CSR(menvcfg, READ_CSR(menvcfg) | (1UL << 7)); /* permit cbo.zero */
  WRITE_CSR(mstatus, READ_CSR(mstatus) | (3UL << 9)); /* VS enabled */
  *(volatile uint64_t *)PA = DATA;
  *(volatile uint64_t *)(PA-4096) = DATA;
  *(volatile uint64_t *)(PA+4096) = DATA;
  root[2] = leaf(0x80000000, RWX); /* identity mapping for supervisor probes */
  root[1] = table(l1);
  l1[0] = table(l0);
  l0[0] = leaf(PA, RWX);
  l0[1] = leaf(PA, RWX); /* alias */
  l0[2] = leaf(PA-4096, RWX);
  l0[3] = leaf(PA+4096, RWX);
  l1[256] = leaf(0x80400000, RWX); /* 2 MiB leaf */
  root[4] = leaf(0x80000000, RWX); /* 1 GiB alias */
  root[3] = table(big);
  big[0] = table(gl0); /* separate 4 KiB table for NAPOT */
  for (unsigned i=0; i<16; i++) gl0[i] = leaf(0x80408000, RWX) | (1UL << 63);
  WRITE_CSR(satp, (8UL << 60) | ((uint64_t)root >> 12));
  flush();
}

void test_main(void) {
  puts_("MBMC_TEST_BEGIN scenario="); hex(SCENARIO); puts_("\n");
  setup();
  if (SCENARIO == 6) {
    extern void probe_csr(void);
    check("disabled-extension-CSR-illegal", probe_csr, 0, 0, 3, 2, 0xbc202573);
    puts_("MBMC_TEST_OK\n"); finish(0);
  }
  require("RAM-reset-disabled", READ_CSR(0xbc2) == 0);
  require("MMIO-reset-disabled", READ_CSR(0xbc3) == 0);
  uint64_t base = (uint64_t)bitmap;
  if (SCENARIO == 1) base = 0;
  if (SCENARIO == 2) base = UINT64_MAX & ~63UL;
  if (SCENARIO == 3) base = 0xbfff0000;
  if (SCENARIO >= 1 && SCENARIO <= 3) {
    WRITE_CSR(satp, 0); flush();
    WRITE_CSR(0xbc2, base | 4);
    check("bad-metadata-load", probe_mprv_load, PA, 0, 3, 5, PA);
    check("bad-metadata-store", probe_mprv_store, PA, 1, 3, 7, PA);
    check("bad-metadata-fetch", probe_load, PA, 0, 1, 1, (uint64_t)probe_load);
    require("bad-metadata-did-not-write", *(volatile uint64_t *)PA == DATA);
    puts_("MBMC_TEST_OK\n"); finish(0);
  }
  if (SCENARIO == 5) {
    volatile uint32_t *config = (void *)0x30010000UL;
    volatile uint64_t *edu = (void *)0x40000000UL;
    WRITE_CSR(satp, 0); flush();
    config[4] = 0x40000000;
    config[1] = 6;
    WRITE_CSR(0xbc2, base | 4);
    bit(PA, 1); bit(PA+4096, 1);
    *(volatile uint64_t *)(PA+4096) = 0;
    check("CPU-source-protected", probe_load, PA, 0, 1, 5, PA);
    check("CPU-destination-protected", probe_load, PA+4096, 0, 1, 5, PA+4096);
    edu[0x80/8] = PA;
    edu[0x88/8] = 0x40000;
    edu[0x90/8] = 8;
    edu[0x98/8] = 1; /* RAM -> device buffer */
    unsigned n;
    for (n=0; n<50000000 && (edu[0x98/8] & 1); n++) {}
    require("DMA-read-completed", n < 50000000);
    edu[0x80/8] = 0x40000;
    edu[0x88/8] = PA+4096;
    edu[0x90/8] = 8;
    edu[0x98/8] = 3; /* device buffer -> RAM */
    for (n=0; n<50000000 && (edu[0x98/8] & 1); n++) {}
    require("DMA-write-completed", n < 50000000);
    require("DMA-independent-of-CPU-bitmap", *(volatile uint64_t *)(PA+4096) == DATA);
    check("CPU-still-denied-after-DMA", probe_load, PA+4096, 0, 1, 5, PA+4096);
    puts_("MBMC_TEST_OK\n"); finish(0);
  }
  if (SCENARIO == 4) {
    extern void probe_load32(void), probe_store32(void), probe_csr(void);
    extern void probe_mmio_csr_write(void);
    static uint8_t mmio_bitmap[32768] ALIGN;
    volatile uint32_t *config = (void *)0x30010000UL; /* EDU slot 2 */
    WRITE_CSR(satp, 0); flush();
    require("EDU-present", config[0] == 0x11e81234);
    config[4] = 0x40000000;
    config[1] = 6; /* memory decode and bus master */
    WRITE_CSR(0xbc3, (uint64_t)mmio_bitmap | 4);
    check("MMIO-warm-allowed", probe_load32, VA, 0, 1, 9, 0);
    require("MMIO-device-value", trap_value == 0x010000ed);
    mmio_bitmap[0] = 1;
    __asm__ volatile("fence rw,rw" ::: "memory");
    WRITE_CSR(0xbc3, READ_CSR(0xbc3) | 2);
    check("MMIO-warm-revoked", probe_load32, VA, 0, 1, 5, VA);
    check("MMIO-store-denied", probe_store32, VA+4, 7, 1, 7, VA+4);
    check("MMIO-MPRV-denied", probe_mprv_load, VA, 0, 3, 5, VA);
    require("MMIO-M-allowed", *(volatile uint32_t *)VA == 0x010000ed);
    WRITE_CSR(0xbc3, READ_CSR(0xbc3) | 1);
    check("MMIO-trusted-allowed", probe_load32, VA, 0, 1, 9, 0);
    WRITE_CSR(0xbc3, READ_CSR(0xbc3) & ~1UL);
    check("MMIO-trusted-revoked", probe_load32, VA, 0, 1, 5, VA);
    check("Host-cannot-read-CSR", probe_csr, 0, 0, 1, 2, 0xbc202573);
    check("Host-cannot-enable-bypass", probe_mmio_csr_write, 1, 0, 1, 2, 0xbc351073);
    uint64_t saved_mmio = READ_CSR(0xbc3);
    WRITE_CSR(0xbc3, 0);
    require("MMIO-BMA-BME-locked", READ_CSR(0xbc3) == saved_mmio);
    require("MMIO-BCLEAR-zero", !(READ_CSR(0xbc3) & 2));
    mmio_bitmap[0] = 0;
    WRITE_CSR(0xbc3, READ_CSR(0xbc3) | 2);
    check("MMIO-release-allowed", probe_load32, VA, 0, 1, 9, 0);
    puts_("MBMC_TEST_OK\n"); finish(0);
  }
  WRITE_CSR(0xbc2, base); bit(PA, 1);
  check("BME-off", probe_load, VA, 0, 1, 9, 0);
  require("BME-off-value", trap_value == DATA);
  WRITE_CSR(0xbc2, base | 4);
  check("4K-load-denied", probe_load, VA, 0, 1, 5, VA);
  check("4K-store-denied", probe_store, VA, 1, 1, 7, VA);
  require("denied-store-no-effect", *(volatile uint64_t *)PA == DATA);
  check("alias-denied", probe_load, VA+4096, 0, 1, 5, VA+4096);
  check("2M-subpage-denied", probe_load, BIGVA, 0, 1, 5, BIGVA);
  check("2M-neighbor-allowed", probe_load, BIGVA-4096, 0, 1, 9, 0);
  require("2M-neighbor-value", trap_value == DATA);
  check("1G-subpage-denied", probe_load, GIGAVA, 0, 1, 5, GIGAVA);
  check("1G-neighbor-allowed", probe_load, GIGAVA-4096, 0, 1, 9, 0);
  check("NAPOT-subpage-denied", probe_load, 0xc0003000, 0, 1, 5, 0xc0003000);
  check("NAPOT-neighbor-allowed", probe_load, 0xc0002000, 0, 1, 9, 0);
  check("AMO-denied", probe_amo, VA, 1, 1, 7, VA);
  check("LR-denied", probe_lr, VA, 0, 1, 5, VA);
  check("SC-denied", probe_sc, VA, 1, 1, 7, VA);
  check("CBO-denied", probe_cbo, VA, 0, 1, 7, VA);
  check("vector-load-denied", probe_vector, VA, 0, 1, 5, VA);
  check("vector-store-denied", probe_vector_store, VA, 1, 1, 7, VA);
  cmode(1);
  check("CMODE-allowed", probe_load, VA, 0, 1, 9, 0);
  require("CMODE-value", trap_value == DATA);
  cmode(0);
  check("CMODE-revoked", probe_load, VA, 0, 1, 5, VA);
  bit(PA, 0);
  check("unmark-allowed", probe_load, VA, 0, 1, 9, 0);
  bit(PA, 1); /* BCLEAR invalidates cached data permissions without SFENCE */
  check("remark-revoked", probe_load, VA, 0, 1, 5, VA);
  require("trusted-M-read", *(volatile uint64_t *)PA == DATA);
  check("MPRV-S-denied", probe_mprv_load, VA, 0, 3, 5, VA);

  bit(PA, 0);
  *(volatile uint32_t *)PA = 0x02a00513; /* li a0,42 */
  *(volatile uint32_t *)(PA+4) = 0x00000073; /* ecall */
  __asm__ volatile("fence.i" ::: "memory");
  check("fetch-allowed", probe_fetch, VA, 0, 1, 9, 0);
  require("fetch-value", trap_value == 42);
  bit(PA, 1);
  check("fetch-revoked", probe_fetch, VA, 0, 1, 1, VA);
  cmode(1);
  check("trusted-fetch-allowed", probe_fetch, VA, 0, 1, 9, 0);
  cmode(0); /* No bitmap update: this specifically checks the TB domain key. */
  check("trusted-fetch-revoked", probe_fetch, VA, 0, 1, 1, VA);
  l0[0] |= U; flush(); bit(PA, 0);
  check("U-fetch-allowed", (void (*)(void))VA, 0, 0, 0, 8, 0);
  bit(PA, 1);
  check("U-fetch-denied", (void (*)(void))VA, 0, 0, 0, 1, VA);
  l0[0] &= ~U; flush();
  *(volatile uint64_t *)PA = DATA;
  bit(PA, 0);

  /* A distinct U-mode code page accesses the separately labelled U data page. */
  *(volatile uint32_t *)(PA+4096) = 0x00053503; /* ld a0,0(a0) */
  *(volatile uint32_t *)(PA+4100) = 0x00000073;
  l0[0] |= U; l0[3] |= U; flush();
  __asm__ volatile("fence.i" ::: "memory");
  check("U-load-allowed", (void (*)(void))(VA+12288), VA, 0, 0, 8, 0);
  require("U-load-value", trap_value == DATA);
  bit(PA, 1);
  check("U-load-denied", (void (*)(void))(VA+12288), VA, 0, 0, 5, VA);
  *(volatile uint32_t *)(PA+4096) = 0x00b53023; /* sd a1,0(a0) */
  __asm__ volatile("fence.i" ::: "memory");
  check("U-store-denied", (void (*)(void))(VA+12288), VA, 1, 0, 7, VA);
  require("U-denied-store-no-effect", *(volatile uint64_t *)PA == DATA);
  l0[0] &= ~U; l0[3] &= ~U; flush();
  l0[0] &= ~1UL; flush();
  check("invalid-PTE-before-bitmap", probe_load, VA, 0, 1, 13, VA);
  l0[0] = leaf(PA, 0xc9); flush(); /* valid execute-only leaf */
  check("PTE-read-permission-before-bitmap", probe_load, VA, 0, 1, 13, VA);
  l0[0] = leaf(PA, RWX); flush(); bit(PA, 0);

  /* The page containing l0 is protected; report the original operation. */
  bit((uint64_t)l0, 1); bclear();
  check("PTW-load-fault", probe_load, VA, 0, 1, 5, VA);
  check("PTW-store-fault", probe_store, VA, 1, 1, 7, VA);
  check("PTW-fetch-fault", probe_fetch, VA, 0, 1, 1, VA);
  bit((uint64_t)l0, 0); bclear();
  require("BCLEAR-read-zero", !(READ_CSR(0xbc2) & 2));
  uint64_t saved = READ_CSR(0xbc2);
  WRITE_CSR(0xbc2, (base+4096) | 2); /* must not change BME/BMA */
  require("BME-BMA-locked", READ_CSR(0xbc2) == saved);

  /* Cross-page preflight must refuse the second page before writing either. */
  l0[0] = leaf(PA-4096, RWX); l0[1] = leaf(PA, RWX); flush();
  *(volatile uint64_t *)(PA-8) = DATA;
  bit(PA, 1);
  check("cross-page-load", probe_load, VA+4092, 0, 1, 5, VA+4096);
  check("cross-page-store", probe_store, VA+4092, 0, 1, 7, VA+4096);
  require("cross-store-no-partial-write", *(volatile uint64_t *)(PA-8) == DATA);
  l0[0] = leaf(PA, RWX); flush();

  /* G-stage maps GPA 0x40003000 to a different final PA, using a 2 MiB leaf. */
  groot[1] = table(gl1);
  gl1[0] = leaf(0x80400000, RWX | U);
  WRITE_CSR(vsatp, 0);
  WRITE_CSR(hgatp, (8UL << 60) | ((uint64_t)groot >> 12));
  WRITE_CSR(hstatus, READ_CSR(hstatus) | (1UL << 8)); /* SPVP=S */
  __asm__ volatile("hfence.gvma" ::: "memory");
  check("G-stage-final-PA-denied", probe_hlv, 0x40003000, 0, 1, 5, 0x40003000);
  bit(PA, 0);
  check("G-stage-allowed", probe_hlv, 0x40003000, 0, 1, 9, 0);
  require("G-stage-value", trap_value == DATA);
  bit(PA, 1);
  check("G-stage-cache-revoked", probe_hlv, 0x40003000, 0, 1, 5, 0x40003000);
  bit(PA, 0);
  check("G-stage-cache-warm", probe_hlv, 0x40003000, 0, 1, 9, 0);
  bit((uint64_t)gl1, 1); bclear();
  check("BCLEAR-G-stage-PTW", probe_hlv, 0x40003000, 0, 1, 5, 0x40003000);
  bit((uint64_t)gl1, 0); bclear();
  bit(PA, 1);
  groot[1] = leaf(0x80000000, RWX | U);
  __asm__ volatile("hfence.gvma" ::: "memory");
  check("G-stage-1G-subpage", probe_hlv, 0x40403000, 0, 1, 5, 0x40403000);
  groot[1] = table(gl1); gl1[0] = table(gl0);
  for (unsigned i=0; i<16; i++) gl0[i] = leaf(0x80408000, RWX | U) | (1UL << 63);
  __asm__ volatile("hfence.gvma" ::: "memory");
  check("G-stage-NAPOT-subpage", probe_hlv, 0x40003000, 0, 1, 5, 0x40003000);
  WRITE_CSR(hgatp, 0); __asm__ volatile("hfence.gvma" ::: "memory");

  WRITE_CSR(satp, 0); flush(); bit(PA, 1);
  check("Bare-S-load-denied", probe_load, PA, 0, 1, 5, PA);
  check("Bare-S-store-denied", probe_store, PA, 1, 1, 7, PA);
  /* Deny S-mode access to the bitmap using PMP, allow internal M-mode reads. */
  WRITE_CSR(pmpcfg0, 0);
  uint64_t metadata = (uint64_t)bitmap + (PA >> 15);
  WRITE_CSR(pmpaddr0, (metadata >> 12 << 10) | 511);
  WRITE_CSR(pmpaddr1, 0x3fffffffffffUL);
  WRITE_CSR(pmpcfg0, 0x1f18);
  check("metadata-M-mode-read", probe_load, PA, 0, 1, 5, PA);
  bit(PA, 0);
  check("metadata-PMP-S-deny-M-allow", probe_load, PA, 0, 1, 9, 0);
  check("S-cannot-read-metadata", probe_load, metadata & ~7UL, 0, 1, 5, metadata & ~7UL);
  /* Lock the bitmap byte's page against M-mode reads as well. */
  WRITE_CSR(pmpcfg0, 0);
  WRITE_CSR(pmpaddr0, (metadata >> 12 << 10) | 511);
  WRITE_CSR(pmpcfg0, 0x1f98);
  check("metadata-locked-PMP-denied", probe_mprv_load, PA, 0, 3, 5, PA);
  puts_("MBMC_TEST_OK checks="); hex(passed); puts_("\n"); finish(0);
}
