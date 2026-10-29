/* The live chunk node is inside the first page to be scrubbed. */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096UL
typedef unsigned long paddr_t;
struct cvm_mem_chunk_infor { unsigned long chunk_infor_vaddr; unsigned long *paddr_list; unsigned int type; };
struct cvm_mem_chunk_node { struct cvm_mem_chunk_infor *chunk_infor; struct cvm_mem_chunk_node *next; };
struct cvm_list_params { unsigned long addr,ele_num; };
static struct cvm_mem_chunk_node *free_chunk;
static unsigned int resets,unprotects;
#define sbi_memset memset
static int reset_page_own_table(unsigned long pa,unsigned long *owner) { assert(pa && owner);resets++;return 0; }
static void reset_bitmap(unsigned long pa) { assert(pa);unprotects++; }
static bool sbi_cvm_iommu_active(void) { return false; }
static void host_release_chunk(struct cvm_mem_chunk_infor *p) { (void)p;abort(); }
static void put_chunk(struct cvm_mem_chunk_node *p) { free_chunk=p; }
#include "recycle.inc"
int main(void) {
 unsigned char *ram=aligned_alloc(PAGE_SIZE,PAGE_SIZE*512);assert(ram);
 unsigned long pages[512],owner=7,returned[1]={0};
 for(unsigned int i=0;i<512;i++)pages[i]=(unsigned long)ram+i*PAGE_SIZE;
 struct cvm_mem_chunk_infor info={.chunk_infor_vaddr=0x1234,.paddr_list=pages};
 struct cvm_mem_chunk_node *node=(void *)ram,tail={0},cursor={.next=node};
 struct cvm_list_params output={.addr=(unsigned long)returned};
 for(unsigned int type=0;type<2;type++) {
  memset(ram,0xa5,PAGE_SIZE*512);info.type=type;
  *node=(struct cvm_mem_chunk_node){.chunk_infor=&info,.next=&tail};cursor.next=node;
  assert(!recycle_one_chunk(&cursor,node,&owner,&output));
  assert(cursor.next==&tail);
  for(unsigned int i=PAGE_SIZE;i<PAGE_SIZE*512;i++)assert(!ram[i]);
  if(!type)assert(free_chunk==node && node->chunk_infor==&info);
  else { assert(output.ele_num==1 && returned[0]==0x1234); for(unsigned int i=0;i<PAGE_SIZE;i++)assert(!ram[i]); }
 }
 assert(resets==1024 && unprotects==513);free(ram);
 puts("PASS: chunk scrub snapshots embedded metadata, preserves next link, returns initial/refill resources without use after zero");
 return 0;
}
