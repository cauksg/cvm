#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096
#define TEE_NO_MEMORY -1
#define SBI_EINVAL -3
#define SBI_EALREADY -6
#define SBI_ENO_SHMEM -9
#define sbi_memset memset
typedef uintptr_t paddr_t;

struct kvm_vmid { unsigned long vmid; };
struct iie_cvm_sbi_params { struct kvm_vmid *vmid_ptr; };
struct list_head { struct list_head *next; };
struct cvm_mem_chunk_infor { paddr_t *paddr_list; };
struct cvm_mem_chunk_node {
	struct cvm_mem_chunk_node *next;
	struct cvm_mem_chunk_infor *chunk_infor;
};
struct sbi_cvm {
	paddr_t root_pt;
	struct cvm_mem_chunk_node *used_chunk_list_head;
	struct list_head *free_mem_list_head;
};
struct cvm_node { struct sbi_cvm cvm; };

static _Alignas(PAGE_SIZE) unsigned char root[4 * PAGE_SIZE];
static _Alignas(PAGE_SIZE) unsigned char pages[3 * PAGE_SIZE];
static struct cvm_node node;
static struct list_head root_list;
static struct list_head *free_root_pt_list_head = &root_list;
static bool available = true, chunk_available, exists = true;
static bool owners[4];
static int ownership_failure = -1;
static unsigned int root_returns, chunks;
static paddr_t addresses[3];
static struct cvm_mem_chunk_infor info = { .paddr_list = addresses };
static struct cvm_mem_chunk_node chunk = { .chunk_infor = &info };

static unsigned long cvm_identity(struct kvm_vmid *vmid)
{
	return vmid->vmid;
}
static struct cvm_node *get_cvm(unsigned long identity)
{
	assert(identity == 7);
	return exists ? &node : NULL;
}
static int get_free_page(struct list_head *head, struct sbi_cvm *cvm,
			 paddr_t *address)
{
	assert(head == &root_list && !cvm);
	if (!available)
		return TEE_NO_MEMORY;
	available = false;
	*address = (paddr_t)root;
	return 0;
}
static void put_free_page(struct list_head *head, paddr_t address,
			  struct sbi_cvm *cvm)
{
	assert(head == &root_list && address == (paddr_t)root && !cvm);
	assert(!available);
	available = true;
	root_returns++;
}
static int set_page_own_table(paddr_t address, unsigned long *owner)
{
	unsigned int index = (address - (paddr_t)root) / PAGE_SIZE;

	assert(index < 4 && *owner == 7 && !owners[index]);
	if ((int)index == ownership_failure)
		return SBI_EINVAL;
	owners[index] = true;
	return 0;
}
static void reset_page_own_table(paddr_t address, unsigned long *owner)
{
	unsigned int index = (address - (paddr_t)root) / PAGE_SIZE;

	assert(index < 4 && *owner == 7 && owners[index]);
	owners[index] = false;
}
static struct cvm_mem_chunk_node *get_chunk(void)
{
	return chunk_available ? &chunk : NULL;
}
static void chunk2list(struct sbi_cvm *cvm, struct cvm_mem_chunk_node *entry,
		       unsigned long count)
{
	assert(cvm == &node.cvm && entry == &chunk && count == 509);
	chunks++;
}

#include "cvm-allocation.inc"

int main(void)
{
	struct kvm_vmid vmid = { .vmid = 7 };
	struct iie_cvm_sbi_params request = { .vmid_ptr = &vmid };
	unsigned int attempt, index;

	assert(init_cvm_vcpu_rootptAndChunk(NULL) == SBI_EINVAL);
	exists = false;
	assert(init_cvm_vcpu_rootptAndChunk(&request) == SBI_EINVAL);
	exists = true;
	for (ownership_failure = 0; ownership_failure < 4; ownership_failure++) {
		assert(init_cvm_vcpu_rootptAndChunk(&request) == SBI_EINVAL);
		assert(available && !node.cvm.root_pt);
		for (index = 0; index < 4; index++)
			assert(!owners[index]);
	}
	ownership_failure = -1;
	for (attempt = 0; attempt < 64; attempt++) {
		assert(init_cvm_vcpu_rootptAndChunk(&request) == TEE_NO_MEMORY);
		assert(available && !node.cvm.root_pt && !chunks);
		for (index = 0; index < 4; index++)
			assert(!owners[index]);
	}
	assert(root_returns == 68);
	available = false;
	assert(init_cvm_vcpu_rootptAndChunk(&request) == SBI_ENO_SHMEM);
	available = true;
	chunk_available = true;
	for (index = 0; index < 3; index++)
		addresses[index] = (paddr_t)(pages + index * PAGE_SIZE);
	memset(root, 0xa5, sizeof(root));
	assert(!init_cvm_vcpu_rootptAndChunk(&request));
	assert(!available && node.cvm.root_pt == (paddr_t)root && chunks == 1);
	for (index = 0; index < sizeof(root); index++)
		assert(root[index] == 0);
	assert(init_cvm_vcpu_rootptAndChunk(&request) == SBI_EALREADY);
	assert(root_returns == 68 && chunks == 1);
	puts("PASS: real allocator ownership/chunk failure rollback, 64 retries, root capacity and scrub");
	return 0;
}
