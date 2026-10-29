/* A shared HPA remains Host-accessible but must never be redonated. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define CVM_HOST_PAGES 64UL
#define SBI_EDENIED -4
static unsigned long donated[1],pending_donation[1];
static struct owner { unsigned long vmidp; } owners[64],*page_own_table;
static unsigned int ownership_table_level;
static unsigned long own_max_num=64;
static bool sbi_cvm_iommu_host_buffer(unsigned long pa,unsigned long size) {
 return pa>=0x80000000 && pa<0x80040000 && size<=0x80040000-pa;
}
#include "donation.inc"
int main(void) {
 assert(!host_donation_range(0x80001000,1));
 assert(host_donation_range(0x80001000,1)==SBI_EDENIED);
 memset(pending_donation,0,sizeof(pending_donation));page_own_table=owners;
 owners[1].vmidp=0x1234;
 assert(sbi_cvm_iommu_host_buffer(0x80001000,PAGE_SIZE));
 assert(host_donation_range(0x80001000,1)==SBI_EDENIED);
 assert(!pending_donation[0]);
 owners[1].vmidp=0;
 assert(!host_donation_range(0x80001000,1));
 donated[0]=pending_donation[0];pending_donation[0]=0;
 assert(host_donation_range(0x80001000,1)==SBI_EDENIED);
 assert(host_donation_range(0x80001001,1)==SBI_EDENIED);
 assert(host_donation_range(0x80040000,1)==SBI_EDENIED);
 assert(host_donation_range(0x80000000,65)==SBI_EDENIED);
 puts("PASS: donation rejects public shared owners, pending aliases, committed replay, alignment and bounds");
 return 0;
}
