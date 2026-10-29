/* Four actual e1000e endpoints: two owners, two device IIDs per file. */
#define TEST_NIC
#define test_main legacy_test_main
#include "trusted-io-control.c"
#undef test_main

static volatile uint32_t *config(unsigned index)
{
 return (volatile uint32_t *)(0x30010000UL + index * 0x8000);
}
static void select_rid(unsigned index) { CTRL[1] = 16 + index * 8; }
static uint64_t bar(unsigned index) { return 0x40000000UL + index * 0x100000; }
static void command(unsigned index,unsigned cmd)
{
 select_rid(index);CTRL[2]=cmd;check("quad-controller-command",CTRL[3]==0);
}
void test_main(void)
{
 CSR_WRITE(pmpaddr0,0x3fffffffffffUL);CSR_WRITE(pmpcfg0,0x1f);
 CSR_WRITE(satp,0);CSR_WRITE(vsatp,0);CSR_WRITE(hgatp,0);
 CSR_WRITE(0xbc3,(uint64_t)mmio_bitmap|4);
 for (unsigned i=0;i<4;i++) {
  volatile uint32_t *cfg=config(i);
  check("quad-e1000e-present",cfg[0]==0x10d38086);
  cfg[4]=bar(i);cfg[5]=bar(i)+0x20000;
  cfg[6]=(0x20+i*0x20)|1;cfg[7]=bar(i)+0x40000;cfg[1]=7;
 }
 /* Default subpage PIO allocations cannot become four protected owners. */
 select_rid(0);CTRL[2]=1;
 check("quad-shared-PIO-page-claim-rejected",CTRL[3]!=0);
 for (unsigned i=0;i<4;i++) config(i)[6]=((i+1)*4096)|1;
 for (unsigned owner=1;owner<=2;owner++) {
  CTRL[0x38/8]=owner;CTRL[0x30/8]=owner;
 }
 for (unsigned i=0;i<4;i++) {
  unsigned owner=i/2+1;
  command(i,1);CTRL[0x128/8]=owner;
  check("quad-RID-owner",CTRL[0x128/8]==owner);
  CTRL[0x128/8]=3-owner;
  check("quad-RID-retarget-rejected",CTRL[0x128/8]==owner);
  CTRL[0x38/8]=owner;CTRL[0x28/8]=i%2+2;
  check("quad-independent-IID",CTRL[0x28/8]==i%2+2);
  config(i)[1]=7;
 }
 for (unsigned owner=1;owner<=2;owner++) {
  CTRL[0x20/8]=owner;CSR_WRITE(0xbc3,CSR_READ(0xbc3)|1);
  for (unsigned i=0;i<4;i++) {
   probe(probe_load32,bar(i),0,1);
   check("quad-VS-owner-BAR-policy",trap_cause==(owner==i/2+1 ? 10 : 5));
   probe(probe_load32,0x03000000UL+(i+1)*4096,0,1);
   check("quad-VS-owner-PIO-policy",trap_cause==(owner==i/2+1 ? 10 : 5));
   probe(probe_load32,bar(i),0,0);check("quad-HS-BAR-denied",trap_cause==5);
  }
 }
 CTRL[0x20/8]=0;
 for (unsigned file=1;file<=2;file++) {
  CSR_WRITE(hstatus,(uint64_t)file<<12);
  CSR_WRITE(0x250,0xc0);CSR_WRITE(0x251,12);
  CSR_WRITE(0x250,0x70);CSR_WRITE(0x251,1);
  CSR_WRITE(0x250,0x80);CSR_WRITE(0x251,0);
  probe(probe_store32,0x28000000UL+file*4096,2,0);
  check("quad-Host-device-MSI-injection-denied",pending()==0);
  CSR_WRITE(0x251,12);
  CTRL[0x20/8]=3-file;
  probe(probe_vs_irq_topei,0,0,1);
  check("quad-peer-cannot-consume-other-file",trap_value==0 && pending()==12);
  CTRL[0x20/8]=0;
 }
 /* Quiesce/release one card cannot tear down its peer or the other CVM. */
 command(0,3);command(0,2);
 for (unsigned i=1;i<4;i++) {
  select_rid(i);check("quad-peer-IID-retained",CTRL[0x28/8]==i%2+2);
  check("quad-peer-owner-retained",CTRL[0x128/8]==i/2+1);
  CTRL[0x20/8]=i/2+1;
  probe(probe_load32,bar(i),0,1);check("quad-peer-BAR-survives-release",trap_cause==10);
  CTRL[0x20/8]=0;
 }
 for (unsigned i=1;i<4;i++) {command(i,3);command(i,2);}
 for (unsigned file=1;file<=2;file++) {CTRL[0x38/8]=file;CTRL[0x30/8]=0;}
 puts_("TRUSTED_IO_CONTROL_TEST_OK\n");finish(0);
}
