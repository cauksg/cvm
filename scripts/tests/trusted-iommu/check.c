#include <stdint.h>
struct result { long error; unsigned long value; };
volatile unsigned long trap_cause;
extern unsigned long probe_load(unsigned long);
extern void probe_store(unsigned long, unsigned long);
static unsigned long root[512] __attribute__((aligned(4096)));
static unsigned long l1[512] __attribute__((aligned(4096)));
static unsigned long l0[512] __attribute__((aligned(4096)));
static struct result call(unsigned long op,unsigned long x,unsigned long y,unsigned long z,unsigned long w)
{
 register unsigned long a0 __asm__("a0")=op,a1 __asm__("a1")=x;
 register unsigned long a2 __asm__("a2")=y,a3 __asm__("a3")=z,a4 __asm__("a4")=w;
 register unsigned long a6 __asm__("a6")=0x13,a7 __asm__("a7")=0x20000217;
 __asm__ volatile("ecall":"+r"(a0),"+r"(a1):"r"(a2),"r"(a3),"r"(a4),"r"(a6),"r"(a7):"memory");
 return (struct result){a0,a1};
}
static void puts_(const char *s) {while(*s) *(volatile uint8_t*)0x10000000=*s++;}
static __attribute__((noreturn)) void finish(int code)
{
 register unsigned long a0 __asm__("a0")=0,a1 __asm__("a1")=code;
 register unsigned long a6 __asm__("a6")=0,a7 __asm__("a7")=0x53525354;
 __asm__ volatile("ecall"::"r"(a0),"r"(a1),"r"(a6),"r"(a7):"memory");
 for(;;){}
}
static void check(const char *name,int ok)
{puts_(ok?"PASS ":"FAIL ");puts_(name);puts_("\n");if(!ok)finish(1);}
static uint32_t cfg(unsigned rid,unsigned off)
{return call(4,0x30000000,(rid<<12)|off,4,0).value;}
static int cfgw(unsigned rid,unsigned off,uint32_t v)
{return call(5,0x30000000,(rid<<12)|off,4,v).error;}
static long tdi_request(unsigned long pointer)
{
 register unsigned long a0 __asm__("a0")=pointer;
 register unsigned long a6 __asm__("a6")=0x12,a7 __asm__("a7")=0x20000217;
 __asm__ volatile("ecall":"+r"(a0):"r"(a6),"r"(a7):"a1","memory");
 return (long)a0;
}
static long cpu_request(unsigned long fid, unsigned long pointer,
                        unsigned long context, unsigned long csr,
                        unsigned long trap)
{
 register unsigned long a0 __asm__("a0")=pointer, a1 __asm__("a1")=context;
 register unsigned long a2 __asm__("a2")=csr, a3 __asm__("a3")=trap;
 register unsigned long a6 __asm__("a6")=fid, a7 __asm__("a7")=0x20000217;
 __asm__ volatile("ecall":"+r"(a0),"+r"(a1):"r"(a2),"r"(a3),"r"(a6),"r"(a7):"memory");
 return (long)a0;
}
static long ext_request(unsigned long ext, unsigned long fid,
                        unsigned long x, unsigned long y, unsigned long z)
{
 register unsigned long a0 __asm__("a0")=x,a1 __asm__("a1")=y,a2 __asm__("a2")=z;
 register unsigned long a6 __asm__("a6")=fid,a7 __asm__("a7")=ext;
 __asm__ volatile("ecall":"+r"(a0),"+r"(a1):"r"(a2),"r"(a6),"r"(a7):"memory");
 return (long)a0;
}
#define covi(fid,x,y,z) ext_request(0x434f5649,fid,x,y,z)
#define covh(fid) ext_request(0x434f5648,fid,0,0,0)
static void covi_lifecycle(unsigned long *request)
{
 static int vcpuid;
 static struct {unsigned long base;uint32_t group,shift,hart,guest,nested;} p;
 unsigned long tvm=7;
 request[1]=(unsigned long)&vcpuid;
 for (vcpuid=0;vcpuid<2;vcpuid++)
  check("COVI-create-secondary-vcpu",!cpu_request(3,(unsigned long)request,0,0,0));
 vcpuid=4;
 check("COVI-vcpu-limit-denied",cpu_request(3,(unsigned long)request,0,0,0)==-4);
 check("COVI-init-private-buffer-denied",covi(0,tvm,0xbf000000,sizeof(p))==-3);
 p.base=0x08000000;p.shift=24;p.hart=1;p.nested=1;
 check("COVI-nested-files-unsupported",covi(0,tvm,(unsigned long)&p,sizeof(p))==-2);
 p.nested=0;
 check("COVI-init-valid",!covi(0,tvm,(unsigned long)&p,sizeof(p)));
 check("COVI-init-replay-denied",covi(0,tvm,(unsigned long)&p,sizeof(p))==-3);
 check("COVI-vcpu0-GPA",!covi(1,tvm,0,0x08000000));
 check("COVI-duplicate-GPA-denied",covi(1,tvm,1,0x08000000)==-4);
 check("COVI-unaligned-GPA-denied",covi(1,tvm,1,0x08001001)==-3);
 check("COVI-vcpu1-GPA",!covi(1,tvm,1,0x08001000));
 check("COVI-unknown-vcpu-denied",covi(1,tvm,2,0x08001000)==-3);
 check("COVI-inject-denied-by-default",covi(7,tvm,1,1)==-4);
 check("COVI-invalid-IID-denied",covi(7,tvm,0,256)==-4);
 check("COVI-unfinalized-bind-denied",covi(4,tvm,0,2)==-3);
 check("COVI-migration-not-supported",covi(8,tvm,0,2)==-2);
 check("COVI-file0-not-convertible",covi(2,0x28000000,0,0)==-3);
 check("COVI-convert-file7",!covi(2,0x28007000,0,0));
 trap_cause=0;probe_store(0x28007000,1);
 check("COVI-converted-file-host-write-denied",trap_cause==7);
 check("COVI-convert-replay-denied",covi(2,0x28007000,0,0)==-4);
 check("COVI-reclaim-before-fence-denied",covi(3,0x28007000,0,0)==-4);
 check("COVI-local-without-global-denied",covh(4)==-3);
 check("COVI-global-fence",!covh(3));
 check("COVI-global-replay-denied",covh(3)!=0);
 check("COVI-local-fence",!covh(4));
 check("COVI-reclaim-fenced-file",!covi(3,0x28007000,0,0));
 check("COVI-reclaim-replay-denied",covi(3,0x28007000,0,0)==-4);
 request[1]=0;
}
static void dma(unsigned long src,unsigned long dst)
{
 volatile uint64_t *edu=(void*)0x40000000;
 edu[0x80/8]=src;edu[0x88/8]=dst;edu[0x90/8]=8;edu[0x98/8]=dst==0x40000?1:3;
 for(unsigned n=0;n<100000000 && (edu[0x98/8]&1);n++){}
 check("EDU-DMA-completed",!(edu[0x98/8]&1));
}
static unsigned long pte(unsigned long pa){return (pa>>12<<10)|0xd7;}
struct list_params { unsigned long vaddr, addr, count, pages, level; };
struct chunk_info { unsigned long vaddr, token, array; unsigned int type; unsigned char free; unsigned long id; };
static void memory_lifecycle(void)
{
 static struct list_params chunks, roots, bmp, own, refill, output;
 static unsigned long chunk_array[2], root_array[1], refill_array[1], sources[512], returned[256];
 static unsigned long id[3] = {1,0,7}, clone[3] = {1,0,7}, request[8];
 struct chunk_info *a=(void*)0x84500000, *b=(void*)0x84502000, *c=(void*)0x84504000;
 unsigned long *ap=(void*)0x84501000, *bp=(void*)0x84503000, *cp=(void*)0x84505000;
 for(unsigned i=0;i<512;i++) { ap[i]=0x85000000+i*4096; bp[i]=0x85200000+i*4096; cp[i]=0x85400000+i*4096; sources[i]=0x81000000; }
 *a=(struct chunk_info){0,0x1111,(unsigned long)ap,0,0,0};
 *b=(struct chunk_info){0,0x2222,(unsigned long)bp,0,0,0};
 *c=(struct chunk_info){0,0x3333,(unsigned long)cp,1,0,0};
 chunk_array[0]=(unsigned long)a; chunk_array[1]=(unsigned long)b; root_array[0]=0x84400000;
 chunks=(struct list_params){0,(unsigned long)chunk_array,2,1,0};
 roots=(struct list_params){0,(unsigned long)root_array,1,1,0};
 bmp=(struct list_params){0,0x84000000,4096,8,0}; own=(struct list_params){0,0x84200000,262144,512,0};
 unsigned long saved=bp[1]; bp[1]=ap[0];
 check("donation-cross-chunk-alias-denied",cpu_request(9,(unsigned long)&chunks,(unsigned long)&roots,(unsigned long)&bmp,(unsigned long)&own)==-4);
 bp[1]=saved;
 check("donation-valid-commit",!cpu_request(9,(unsigned long)&chunks,(unsigned long)&roots,(unsigned long)&bmp,(unsigned long)&own));
 check("donation-init-replay-denied",cpu_request(9,(unsigned long)&chunks,(unsigned long)&roots,(unsigned long)&bmp,(unsigned long)&own)==-6);
 trap_cause=0;probe_store((unsigned long)bp,0xbf000000);check("donated-page-list-host-mutation-denied",trap_cause==7);
 trap_cause=0;probe_load(0x85200000);check("unused-donated-page-private",trap_cause==5);
 request[0]=(unsigned long)id;
 check("create-valid-object",!cpu_request(0,(unsigned long)request,0,0,0));
 check("create-replay-denied",cpu_request(0,(unsigned long)request,0,0,0)==-6);
 request[0]=(unsigned long)clone;
 check("duplicate-live-identity-denied",cpu_request(0,(unsigned long)request,0,0,0)==-4);
 request[0]=(unsigned long)id;
 request[0]=(unsigned long)clone;
 check("root-forged-identity-denied",cpu_request(12,(unsigned long)request,0,0,0)==-4);
 request[0]=(unsigned long)id;
 check("root-valid-allocate",!cpu_request(12,(unsigned long)request,0,0,0));
 check("root-repeat-denied",cpu_request(12,(unsigned long)request,0,0,0)==-6);
 covi_lifecycle(request);
 check("destroy-before-recycle-denied",cpu_request(13,(unsigned long)request,0,0,0)==-4);
 refill_array[0]=(unsigned long)c;refill=(struct list_params){0,(unsigned long)refill_array,1,1,0};
 check("refill-valid-commit",!cpu_request(15,(unsigned long)&refill,0,0,0));
 check("refill-replay-denied",cpu_request(15,(unsigned long)&refill,0,0,0)==-4);
 static unsigned long load[4]; load[0]=(unsigned long)id;load[1]=(unsigned long)sources;load[2]=0x90000000;load[3]=512;
 check("load-private-source-list-denied",(load[1]=0xbf000000,cpu_request(7,(unsigned long)load,0,0,0))==-4);
 load[1]=(unsigned long)sources;
 check("load-private-pages",!cpu_request(7,(unsigned long)load,0,0,0));
 /* A hostile proposal may overlap loaded private code, but is never accepted.
  * Recycle must scrub it once through the private chunk, not as shared RAM. */
 static unsigned long sw[3]={0x90000000,4096,0};
 check("shared-window-private-overlap-proposed",!cpu_request(14,(unsigned long)sw,(unsigned long)id,0,0));
 check("shared-window-overlap-replay-denied",cpu_request(14,(unsigned long)sw,(unsigned long)id,0,0)==-4);
 sw[0]=0x90000001;
 check("shared-window-unaligned-denied",cpu_request(14,(unsigned long)sw,(unsigned long)id,0,0)==-3);
 sw[0]=0xbffff000;sw[1]=8192;
 check("shared-window-out-of-range-denied",cpu_request(14,(unsigned long)sw,(unsigned long)id,0,0)==-3);

 output=(struct list_params){0,(unsigned long)returned,0,1,0};
 check("recycle-zero-capacity-denied",cpu_request(17,(unsigned long)request,(unsigned long)&output,0,0)==-4);
 output.count=256;
 check("recycle-valid",!cpu_request(17,(unsigned long)request,(unsigned long)&output,0,0));
 check("recycle-exact-output",output.count==1 && returned[0]==0x3333);
 check("recycled-page-secret-scrubbed",probe_load(0x85400000)==0 && probe_load(0x85401000)==0);
 check("recycle-replay-denied",cpu_request(17,(unsigned long)request,(unsigned long)&output,0,0)==-4);
 check("load-after-recycle-denied",cpu_request(7,(unsigned long)load,0,0,0)==-4);
 check("destroy-recycled-object",!cpu_request(13,(unsigned long)request,0,0,0));
 check("destroy-replay-denied",cpu_request(13,(unsigned long)request,0,0,0)==-4);
 /* Host identities are monotonic and never reused within this boot. */
 request[0]=(unsigned long)clone;
 check("recreate-old-identity-denied",cpu_request(0,(unsigned long)request,0,0,0)==-4);
 clone[2]=8;
 /* Reuse returned pages under a fresh identity as well as a fresh pointer. */
 request[0]=(unsigned long)clone;
 check("recreate-new-identity-handle",!cpu_request(0,(unsigned long)request,0,0,0));
 check("recreate-root-reuses-scrubbed-chunk",!cpu_request(12,(unsigned long)request,0,0,0));
 check("redonate-recycled-refill",!cpu_request(15,(unsigned long)&refill,0,0,0));
 load[0]=(unsigned long)clone;
 check("recreated-private-load",!cpu_request(7,(unsigned long)load,0,0,0));
 output.count=256;
 check("recreated-recycle",!cpu_request(17,(unsigned long)request,(unsigned long)&output,0,0));
 check("recreated-exact-refill-output",output.count==1 && returned[0]==0x3333);
 check("recreated-destroy",!cpu_request(13,(unsigned long)request,0,0,0));

}
void test_main(void)
{
 puts_("TRUSTED_IOMMU_TEST_BEGIN\n");
 check("Monitor-IOMMU-ready",!call(0,0,0,0,0).error);
 trap_cause=0;probe_load(0x03010010);check("Host-DDTP-read-denied",trap_cause==5);
 trap_cause=0;probe_store(0x03010010,1);check("Host-DDTP-Bare-write-denied",trap_cause==7);
 trap_cause=0;probe_store(0xbf000000,~0UL);check("Host-trusted-DDT-write-denied",trap_cause==7);
 trap_cause=0;probe_store(0x30010010,0x50000000);check("Host-direct-ECAM-write-denied",trap_cause==7);
 trap_cause=0;probe_load(0x03011000);check("Host-security-controller-read-denied",trap_cause==5);
 trap_cause=0;probe_store(0x03011010,2);check("Host-security-controller-release-denied",trap_cause==7);
 check("TDI-null-pointer-denied",tdi_request(0)==-4);
 check("TDI-MMIO-pointer-denied",tdi_request(0x03011000)==-4);
 check("TDI-firmware-pointer-denied",tdi_request(0x80000000)==-4);
 check("TDI-trusted-pool-pointer-denied",tdi_request(0xbf000000)==-4);
 check("TDI-overflow-pointer-denied",tdi_request(~0UL-7)==-4);
 check("TDI-cross-pool-pointer-denied",tdi_request(0xbefffff8)==-4);
 static uint64_t request[16];
 request[1]=16;
 check("TDI-shared-buffer-accepted",tdi_request((unsigned long)request)==0 && request[15]);
 request[0]=0xbf000000;
 check("TDI-nested-private-pointer-denied",tdi_request((unsigned long)request)==-4);
 static uint64_t cpu_params[8], context[35], csrs[12], cpu_trap[5];
 static int vcpu_id;
 unsigned long shared = (unsigned long)cpu_params;
 for (unsigned long fid=3; fid<=6; fid++) {
  if (fid==5) continue;
  check("CPU-null-request-denied",cpu_request(fid,0,0,0,0)==-4);
  check("CPU-MMIO-request-denied",cpu_request(fid,0x03011000,0,0,0)==-4);
  check("CPU-private-request-denied",cpu_request(fid,0xbf000000,0,0,0)==-4);
  cpu_params[1]=0xbf000000;
  check("CPU-private-nested-ID-denied",cpu_request(fid,shared,0,0,0)==-4);
 }
 cpu_params[1]=(unsigned long)&vcpu_id;
 check("RUN-private-output-denied",cpu_request(6,shared,0xbf000000,(unsigned long)csrs,(unsigned long)cpu_trap)==-4);
 check("FINALIZE-private-context-denied",cpu_request(4,shared,0xbf000000,(unsigned long)csrs,(unsigned long)cpu_trap)==-4);
 check("RUN-overlapping-outputs-denied",cpu_request(6,shared,(unsigned long)context,(unsigned long)context,(unsigned long)cpu_trap)==-4);
 check("RUN-unregistered-identity-denied",cpu_request(6,shared,(unsigned long)context,(unsigned long)csrs,(unsigned long)cpu_trap)==-4);
 check("FINALIZE-unregistered-identity-denied",cpu_request(4,shared,(unsigned long)context,(unsigned long)csrs,(unsigned long)cpu_trap)==-4);
 static const unsigned long management[] = {0,7,9,12,13,14,15,16,17};
 for (unsigned i=0; i<sizeof(management)/sizeof(management[0]); i++) {
  unsigned long fid=management[i];
  check("management-null-request-denied",cpu_request(fid,0,0,0,0)==-4);
  check("management-private-request-denied",cpu_request(fid,0xbf000000,0,0,0)==-4);
  check("management-MMIO-request-denied",cpu_request(fid,0x03011000,0,0,0)==-4);
  check("management-overflow-request-denied",cpu_request(fid,~0UL-7,0,0,0)==-4);
 }
 static const unsigned long unsupported[] = {1,2,5,8,10,11,0xffff,0x123456};
 for (unsigned i=0;i<sizeof(unsupported)/sizeof(unsupported[0]);i++)
  check("unsupported-management-fail-closed",cpu_request(unsupported[i],shared,0,0,0)==-2);
 check("raw-register-SBI-not-supported",call(99,0x03010010,1,0,0).error);
 check("invalid-PCI-access-denied",call(5,0x30000000,0x100000,4,0).error);
 check("device-id-boundary-denied",call(1,64,0,1,0).error);
 check("Host-EDU-id",cfg(16,0)==0x11e81234);
 check("Host-EDU-BAR-config",!cfgw(16,0x10,0x40000000));
 check("Host-EDU-command",!cfgw(16,4,6));
 check("extended-config-write-denied",cfgw(16,0x100,~0U));
 volatile unsigned long *source=(void*)0x81000000,*dest=(void*)0x81001000;
 *source=0x1122334455667788UL;*dest=0;
 check("Host-Bare-still-trusted-Gstage",!call(1,16,0,1,0).error);
 dma((unsigned long)source,0x40000);dma(0x40000,(unsigned long)dest);
 check("Host-owned-RAM-DMA-roundtrip",*dest==*source);
 /* A hostile first-stage PTE cannot bypass trusted G-stage. */
 /* Allocator: DDT at pool+0, CQ +0x1000, aligned G-root +0x4000. */
 unsigned long target=0xbf004000;
 root[0]=(unsigned long)l1>>12<<10|1;
 l1[0]=(unsigned long)l0>>12<<10|1;
 l0[0]=pte((unsigned long)source);
 l0[1]=pte((unsigned long)dest);
 check("Host-nested-FSC-request",!call(1,16,(8UL<<60)|((unsigned long)root>>12),1,0).error);
 *dest=0;dma(0,0x40000);dma(0x40000,0x1000);
 check("Host-nested-FSC-DMA-roundtrip",*dest==*source);
 l0[0]=pte(target);
 check("Host-malicious-FSC-request",!call(1,16,(8UL<<60)|((unsigned long)root>>12),1,0).error);
 /* Device buffer contains the known source word before the attack. */
 trap_cause=0;probe_load(target);check("trusted-pool-remains-CPU-protected",trap_cause==5);
 /* Try overwriting the live Host G-root RAM entry, not an unused DDT. */
 dma(0x40000,16);
 /* A denied read must not leak the known nonzero trusted root entry. */
 dma(16,0x40000);*dest=0;dma(0x40000,0x1000);
 /* An unmapped transaction reads zero; the live root entry is nonzero. */
 check("trusted-table-DMA-read-denied",*dest==0);
 /* Reload the known buffer through an ordinary context for the next probe. */
 check("Host-context-post-read-recover",!call(1,16,0,1,0).error);
 dma((unsigned long)source,0x40000);
 /* Recover ordinary Host context and show a post-attack healthy DMA. */
 check("Host-context-recover",!call(1,16,0,1,0).error);
 *dest=0;dma(0x40000,(unsigned long)dest);
 check("Host-malicious-FSC-cannot-corrupt-live-Groot",*dest==*source);
 /* First-stage table walks themselves must also use the trusted G-stage. */
 check("Host-private-FSC-request",!call(1,16,(8UL<<60)|(target>>12),1,0).error);
 *dest=0;dma(0x40000,(unsigned long)dest);
 check("private-first-stage-table-walk-denied",*dest==0);
 check("Host-context-final-recover",!call(1,16,0,1,0).error);
 check("unclaimed-MSI-rejected",call(3,16,0x28000000,0x28001000,32).error);
 check("trusted-IOMMU-healthy",call(6,0,0,0,0).value==1);
 memory_lifecycle();
 puts_("TRUSTED_IOMMU_TEST_OK\n");finish(0);
}
