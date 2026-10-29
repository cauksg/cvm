/* Freestanding tests of the actual QEMU security-controller model. */
#include <stdint.h>
#define CSR_READ(c) ({ unsigned long v; __asm__ volatile("csrr %0," #c : "=r"(v)); v; })
#define CSR_WRITE(c,v) __asm__ volatile("csrw " #c ",%0" :: "r"((unsigned long)(v)) : "memory")
#define CTRL ((volatile uint64_t *)0x03011000)
#define CFG ((volatile uint32_t *)0x30010000)
volatile uint64_t probe_active, trap_cause, trap_tval, trap_value, trap_epc;
static uint8_t mmio_bitmap[32768] __attribute__((aligned(4096)));
extern void run_mode(uint64_t, uint64_t, uint64_t, uint64_t);
extern void probe_load32(void), probe_store32(void);
extern void probe_irq_read(void), probe_irq_clear(void), probe_irq_set(void);
extern void probe_hgeip(void), probe_hip(void), probe_vstopi(void);
extern void probe_irq_topei(void), probe_vs_irq_read(void), probe_vs_irq_topei(void);
static void puts_(const char *s) { while (*s) *(volatile uint8_t *)0x10000000 = *s++; }
static __attribute__((noreturn)) void finish(int code)
{
 *(volatile uint32_t *)0x100000 = code ? (code << 16) | 0x3333 : 0x5555;
 for (;;) {}
}
void fatal_trap(uint64_t cause, uint64_t epc, uint64_t tval)
{ puts_("FAIL unexpected M trap\n"); finish(255); }
static void check(const char *s, int ok)
{ puts_(ok ? "PASS " : "FAIL "); puts_(s); puts_("\n"); if (!ok) finish(1); }
static void probe(void (*fn)(void), uint64_t address, uint64_t value, int vs)
{
 /* Set MPV only for the impending mret; trap returns to M with MPV cleared. */
 CSR_WRITE(mstatus, (CSR_READ(mstatus) & ~(1UL << 39)) | ((uint64_t)vs << 39));
 run_mode((uint64_t)fn, address, value, 1);
}
static void claim(unsigned cmd)
{ CTRL[1] = 16; CTRL[2] = cmd; check("controller-command", CTRL[3] == 0); }
static unsigned long pending(void)
{ CSR_WRITE(0x250, 0x80); return CSR_READ(0x251); }
#ifdef TEST_NIC
static unsigned eeprom_word(void)
{
 *(volatile uint32_t *)0x40000014 = (10 << 2) | 1;
 return *(volatile uint32_t *)0x40000014 >> 16;
}
static void eeprom_write(unsigned value)
{ *(volatile uint32_t *)0x4000102c = (value << 16) | (10 << 2) | 1; }
#endif
void test_main(void)
{
 CSR_WRITE(pmpaddr0, 0x3fffffffffffUL); CSR_WRITE(pmpcfg0, 0x1f);
 CSR_WRITE(satp, 0); CSR_WRITE(vsatp, 0); CSR_WRITE(hgatp, 0);
 CSR_WRITE(hstatus, 1UL << 12);
 check("controller-present", CTRL[0] == 0x43564d01);
#ifdef TEST_NIC
 check("NIC-present", CFG[0] == 0x10d38086);
 CFG[4]=0x40000000; CFG[5]=0x40020000; CFG[6]=0x21; CFG[7]=0x40040000;
 unsigned bars=4;
#else
 check("EDU-present", CFG[0] == 0x11e81234);
 CFG[4]=0x40000000; unsigned bars=1;
#endif
 CFG[1]=7;
 CSR_WRITE(0xbc3, (uint64_t)mmio_bitmap | 4);
 probe(probe_load32,0x40000000,0,0); check("Host-warm-BAR-before-claim",trap_cause==9);
#ifdef TEST_NIC
 unsigned original_eeprom=eeprom_word();
 eeprom_write(original_eeprom ^ 0xffff);
 check("EEPROM-seeded-before-claim",eeprom_word()==(original_eeprom ^ 0xffff));
 /* Ordinary driver reset preserves NVM semantics. Ownership reset scrubs it. */
 *(volatile uint32_t *)0x40000000=0x04000000;
 check("ordinary-reset-preserves-EEPROM",eeprom_word()==(original_eeprom ^ 0xffff));
 probe(probe_load32,0x03000ff0,0,0); check("Host-warm-PIO-page-padding",trap_cause==9);
#endif
 claim(1);
 CTRL[0x128/8]=1;
 /* Claim resets decode. Firmware START enables it without changing BARs. */
 CFG[1]=6;
 check("actual-BAR0-descriptor", CTRL[0x40/8]==0x40000000);
#ifdef TEST_NIC
 check("claim-scrubs-Host-EEPROM",eeprom_word()==original_eeprom);
 probe(probe_load32,0x03000ff0,0,0); check("Host-PIO-page-padding-denied",trap_cause==5);
#endif
 for(unsigned i=0;i<bars;i++) {
  uint64_t address=CTRL[(0x40+i*16)/8];
  probe(probe_load32,address,0,0); check("Host-BAR-alias-read-denied",trap_cause==5);
  probe(probe_store32,address,0,0); check("Host-BAR-alias-write-denied",trap_cause==7);
 }
 probe(probe_load32,0x40000000,0,1); check("unowned-VS-BAR-denied",trap_cause==5);
 CTRL[0x30/8]=1; CTRL[0x20/8]=1; CSR_WRITE(0xbc3, CSR_READ(0xbc3)|1);
 probe(probe_load32,0x40000000,0,1); check("owner-VS-BAR-access",trap_cause==10);
 probe(probe_load32,0x40000000,0,0); check("HS-cannot-inherit-owner-bypass",trap_cause==5);
 CTRL[0x20/8]=0; CTRL[0x30/8]=0;
 probe(probe_load32,0x40000000,0,1); check("owner-access-revoked-on-exit",trap_cause==5);
 CTRL[0x28/8]=2; check("IID-claimed",CTRL[0x28/8]==2);
 CTRL[0x28/8]=7; check("IID-retarget-denied",CTRL[0x28/8]==2);
 CSR_WRITE(0x250,0xc0); CSR_WRITE(0x251,4);
 CSR_WRITE(0x250,0x70); CSR_WRITE(0x251,1);
 CSR_WRITE(0x250,0x80); CSR_WRITE(0x251,4);
 probe(probe_irq_read,0,0,0); check("Host-protected-pending-read-masked",trap_cause==9 && !(trap_value&4));
 probe(probe_irq_clear,0,4,0); check("Host-cannot-clear-device-pending",pending()&4);
 probe(probe_irq_topei,0,0,0); check("Host-cannot-consume-device-IRQ",trap_cause==9 && trap_value==0 && (pending()&4));
 CSR_WRITE(0x251,0);
 probe(probe_irq_set,0,4,0); check("Host-cannot-forge-device-pending",!(pending()&4));
 probe(probe_irq_set,0,128,0); check("Host-other-IID-remains-usable",pending()&128);
 probe(probe_store32,0x28001000,2,0); check("Host-MMIO-forgery-blocked",!(pending()&4));
 /* Finalized CVM owns delivery/threshold/EIP/EIE and observation too. */
 CTRL[0x30/8]=1;
 CSR_WRITE(0x250,0x80); CSR_WRITE(0x251,132);
 probe(probe_irq_read,0,0,0); check("whole-file-pending-read-masked",trap_value==0);
 probe(probe_irq_clear,0,128,0); check("whole-file-pending-clear-denied",pending()==132);
 CSR_WRITE(0x250,0x70);
 probe(probe_irq_clear,0,1,0); check("Host-delivery-disable-denied",CSR_READ(0x251)==1);
 CSR_WRITE(0x250,0x72); CSR_WRITE(0x251,0);
 probe(probe_irq_set,0,255,0); check("Host-threshold-poison-denied",CSR_READ(0x251)==0);
 CSR_WRITE(0x250,0xc0); CSR_WRITE(0x251,132);
 probe(probe_irq_clear,0,128,0); check("Host-enable-poison-denied",CSR_READ(0x251)==132);
 probe(probe_hgeip,0,0,0); check("Host-HGEIP-aggregate-wakeup-public",trap_value&2);
 probe(probe_hip,0,0,0); check("Host-HIP-file-signal-hidden",!(trap_value&1024));
 probe(probe_vstopi,0,0,0); check("Host-VSTOPI-hidden",trap_value==0);
 CTRL[0x20/8]=1; CSR_WRITE(0x250,0x80); CSR_WRITE(0x251,4);
 probe(probe_vs_irq_read,0,0,1); check("owner-VS-can-read-device-pending",trap_cause==10 && (trap_value&4));
 probe(probe_vs_irq_topei,0,0,1); check("owner-VS-can-consume-device-IRQ",trap_cause==10 && trap_value==0x20002);
 check("owner-ack-clears-pending",!(pending()&4));
 CTRL[0x20/8]=0;
 /* A and B own separate files on the same hart, even with Bare G-stage. */
 CTRL[0x38/8]=2; CTRL[0x30/8]=2;
 CSR_WRITE(hstatus,2UL<<12);
 CSR_WRITE(0x250,0xc0);CSR_WRITE(0x251,8);
 CSR_WRITE(0x250,0x70);CSR_WRITE(0x251,1);
 CSR_WRITE(0x250,0x80);CSR_WRITE(0x251,8);
 CTRL[0x20/8]=1;
 probe(probe_vs_irq_read,0,0,1);check("A-cannot-read-B-file",trap_value==0);
 probe(probe_vs_irq_topei,0,0,1);check("A-cannot-consume-B-IRQ",trap_value==0 && pending()==8);
 probe(probe_store32,0x28002000,7,1);check("A-cannot-write-B-file-MMIO",trap_cause==7 && pending()==8);
 CTRL[0x20/8]=2;
 probe(probe_vs_irq_read,0,0,1);check("B-reads-own-pending",trap_value==8);
 probe(probe_load32,0x40000000,0,1);check("B-cannot-access-A-BAR",trap_cause==5);
 probe(probe_vs_irq_topei,0,0,1);check("B-acknowledges-own-IRQ",trap_value==0x30003 && !pending());
 CTRL[0x20/8]=0;
 probe(probe_irq_set,0,8,0);check("Host-cannot-inject-B-file",!pending());
 CTRL[0x38/8]=1;CTRL[0x30/8]=0;
 CTRL[0x38/8]=2;check("release-A-preserves-B-owner",CTRL[0x30/8]==2);
 CTRL[0x30/8]=0;CTRL[0x38/8]=1;CTRL[0x30/8]=1;
 CSR_WRITE(hstatus,1UL<<12);
#ifndef TEST_NIC
 /* A quiesced endpoint must not expose the old inversion register value. */
 *(volatile uint32_t *)0x40000004=0x12345678;
#else
 eeprom_write(original_eeprom ^ 0x55aa);
 check("EEPROM-seeded-before-release",eeprom_word()==(original_eeprom ^ 0x55aa));
 *(volatile uint32_t *)0x400000c4=0x1234;
 check("ITR-seeded-before-release",*(volatile uint32_t *)0x400000c4==0x1234);
#endif
#ifdef TEST_MULTI
 volatile uint32_t *cfg2=(void *)0x30018000;
 check("second-EDU-present",cfg2[0]==0x11e81234);
 cfg2[4]=0x40400000;cfg2[1]=6;
 CTRL[1]=24;CTRL[2]=1;check("second-RID-claim",CTRL[3]==0);
 CTRL[0x128/8]=1;
 cfg2[1]=6;CTRL[0x28/8]=2;
 check("second-RID-duplicate-IID-denied",CTRL[0x28/8]==0);
 CTRL[0x28/8]=3;check("second-RID-distinct-IID",CTRL[0x28/8]==3);
 probe(probe_load32,0x40400000,0,0);check("Host-second-BAR-denied",trap_cause==5);
 CTRL[0x20/8]=1;
 probe(probe_load32,0x40400000,0,1);check("owner-second-BAR-access",trap_cause==10);
 CTRL[0x20/8]=0;
#endif
 claim(3); claim(2);
#ifdef TEST_MULTI
 probe(probe_load32,0x40400000,0,0);check("first-release-preserves-second-BAR",trap_cause==5);
 CTRL[1]=24;check("first-release-preserves-second-IID",CTRL[0x28/8]==3);
 CTRL[0x20/8]=1;
 probe(probe_load32,0x40400000,0,1);check("second-VS-access-after-first-release",trap_cause==10);
 CTRL[0x20/8]=0;CTRL[2]=3;check("second-quiesce",CTRL[3]==0);
 CTRL[2]=2;check("second-release",CTRL[3]==0);
 CTRL[1]=16;
#endif
 CTRL[0x30/8]=0;
 check("IID-released-after-scrub",CTRL[0x28/8]==0);
 CFG[1]=6;
 probe(probe_load32,0x40000000,0,0); check("Host-access-restored-after-release",trap_cause==9);
#ifndef TEST_NIC
 check("device-state-scrubbed",*(volatile uint32_t *)0x40000004==0);
#else
 check("release-scrubs-Guest-EEPROM",eeprom_word()==original_eeprom);
 check("release-scrubs-Guest-ITR",*(volatile uint32_t *)0x400000c4==0);
#endif
 puts_("TRUSTED_IO_CONTROL_TEST_OK\n"); finish(0);
}
