#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_cvm.h>
#include <sbi/sbi_cvm_iommu.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_string.h>

static int cove_io_host_request(unsigned long address)
{
 struct cove_io_tdi_sbi_params request;
 int ret;
 if (sbi_cvm_iommu_active()) {
  if ((address & 7) ||
      !sbi_cvm_iommu_host_buffer(address, sizeof(request))) return SBI_EDENIED;
  sbi_memcpy(&request, (void *)address, sizeof(request));
  if (request.vmid_ptr &&
      (((unsigned long)request.vmid_ptr & (sizeof(unsigned long) - 1)) ||
       !sbi_cvm_iommu_host_buffer((unsigned long)request.vmid_ptr,
                                  sizeof(*request.vmid_ptr)))) return SBI_EDENIED;
  /* Dispatch a Monitor-local snapshot. Host DMA can mutate the shared input
   * while the request executes, but cannot change the authorized operation. */
  ret = cove_io_tdi_op(&request);
  sbi_memcpy((void *)address, &request, sizeof(request));
  return ret;
 }
 return cove_io_tdi_op((void *)address);
}

static int cvm_host_state(struct sbi_trap_regs *regs, unsigned long fid)
{
 struct iie_cvm_sbi_params request;
 unsigned long address = regs->a0, id_address;
 int vcpu_id;
 if (!sbi_cvm_iommu_active()) {
  if (fid == SBI_EXT_CVM_CREATE_VCPU) return sbi_cvm_create_vcpu((void *)address);
  if (fid == SBI_EXT_CVM_FINALIZE) return sbi_cvm_create_finalize((void *)address);
  return sbi_cvm_run_vcpu(regs, (void *)address, regs->a1, regs->a2, regs->a3);
 }
 if ((address & 7) || !sbi_cvm_iommu_host_buffer(address, sizeof(request)))
  return SBI_EDENIED;
 sbi_memcpy(&request, (void *)address, sizeof(request));
 id_address = (unsigned long)request.vcpu_id_ptr;
 if ((id_address & 3) || !sbi_cvm_iommu_host_buffer(id_address, sizeof(int)))
  return SBI_EDENIED;
 sbi_memcpy(&vcpu_id, (void *)id_address, sizeof(vcpu_id));
 if (vcpu_id < 0 || vcpu_id >= CVM_MAX_VCPUS) return SBI_EDENIED;
 request.vcpu_id_ptr = &vcpu_id;
 if (fid == SBI_EXT_CVM_CREATE_VCPU) return sbi_cvm_create_vcpu(&request);
 if ((regs->a1 & 7) || (regs->a2 & 7) || (regs->a3 & 7) ||
     !sbi_cvm_iommu_host_buffer(regs->a1, sizeof(struct cpu_context)) ||
     !sbi_cvm_iommu_host_buffer(regs->a2, sizeof(struct vcpu_csr)) ||
     !sbi_cvm_iommu_host_buffer(regs->a3, sizeof(struct cpu_trap)))
  return SBI_EDENIED;
 /* Outputs may not alias each other, the request, or its nested ID. */
 unsigned long start[] = {address, regs->a1, regs->a2, regs->a3, id_address};
 unsigned long size[] = {sizeof(request), sizeof(struct cpu_context),
                         sizeof(struct vcpu_csr), sizeof(struct cpu_trap), sizeof(int)};
 for (unsigned int i = 0; i < 5; i++)
  for (unsigned int j = i + 1; j < 5; j++)
   if (start[i] < start[j] + size[j] && start[j] < start[i] + size[i])
    return SBI_EDENIED;
 if (fid == SBI_EXT_CVM_FINALIZE) {
  struct cpu_context initial;
  sbi_memcpy(&initial, (void *)regs->a1, sizeof(initial));
  return sbi_cvm_finalize_vcpu(&request, &initial);
 }
 return sbi_cvm_run_vcpu(regs, &request, regs->a1, regs->a2, regs->a3);
}

static int sbi_ecall_cvm_handler(unsigned long extid, unsigned long funcid,
        struct sbi_trap_regs *regs, struct sbi_ecall_return *out)
{
    int ret = 0;
    if (sbi_cvm_iommu_active() && funcid != SBI_EXT_CVM_CREATE_VCPU &&
        funcid != SBI_EXT_CVM_RUN_VCPU && funcid != SBI_EXT_CVM_FINALIZE &&
        funcid != SBI_EXT_CVM_TRUSTED_IOMMU && funcid != SBI_EXT_CVM_COVE_IO_TDI_OP) {
        return sbi_cvm_host_ecall(funcid, regs);
    }

	// uint64_t temp;
	// sbi_printf("sbi_ecall_cvm_handler!\n");
	switch (funcid) {
	case SBI_EXT_CVM_CREATE:
		ret = sbi_cvm_create((void *)regs->a0);
		break;
	case SBI_EXT_CVM_CREATE_MEMORY_REGION:
		ret = sbi_cvm_create_memory_region((void *)regs->a0);
		break;
	// case SBI_EXT_CVM_MEASURED_PAGES:
	// 	ret = sbi_cvm_create_measured_pages((void *)regs->a0);
	// 	break;
	case SBI_EXT_CVM_CREATE_VCPU:
		ret = cvm_host_state(regs, funcid);
		break;
	case SBI_EXT_CVM_RUN_VCPU:
		ret = cvm_host_state(regs, funcid);
		out->skip_regs_update = !ret;
		break;
	case SBI_EXT_CVM_FINALIZE:
		ret = cvm_host_state(regs, funcid);
		if (!ret && sbi_cvm_iommu_active()) out->value = 1;
		break;
	case SBI_EXT_CVM_INIT_MEM_POOL:
		ret = sbi_cvm_init_mem_pool((void *)regs->a0);
		break;
	case SBI_EXT_CVM_LOAD_FILE:
		ret = load_file((void *)regs->a0);
		break;
	case SBI_EXT_CVM_INIT_PAGE_LIST:
		ret = convert_cvm_pages((void *)regs->a0, (void *)regs->a1, (void *)regs->a2, (void *)regs->a3);
		break;
	case SBI_EXT_CVM_HASH_IMAGE:
		ret = sbi_cvm_hash_image((void *)regs->a0);
		break;
	case SBI_EXT_CVM_ATTEST:
		ret = sbi_cvm_attest((void *)regs->a0);
		break;
	case SBI_EXT_CVM_DESTROY:
		ret = sbi_cvm_destroy((void *)regs->a0);
		break;
	case SBI_EXT_CVM_TEST:
		ret = sbi_cvm_test((void *)regs->a0);
		break;
	case SBI_EXT_CVM_ALLOC_ROOT_PT:
		ret = init_cvm_vcpu_rootptAndChunk((void *)regs->a0);
		break;
	case SBI_EXT_CVM_INIT_SWIOTLB:
		ret = init_swiotlb_params((void *)regs->a0, (void *)regs->a1);
		break;
	case SBI_EXT_CVM_REFILL_MEMORY_POOL:
		ret = refill_memory_pool((void *)regs->a0);
		break;
	case SBI_EXT_CVM_RETRY_LOAD:
		ret = retry_load_after_refill((void *)regs->a0);
		break;
	case SBI_EXT_RECYCLE_MEMORY:
		ret = recycle_memory((void *)regs->a0, (void *)regs->a1);
		break;
	case SBI_EXT_CVM_TRUSTED_IOMMU:
		return sbi_cvm_iommu_ecall(regs->a0, regs->a1, regs->a2,
				regs->a3, regs->a4, &out->value);
	case SBI_EXT_CVM_COVE_IO_TDI_OP:
		ret = cove_io_host_request(regs->a0);
		break;
	default:
		return SBI_ENOTSUPP;
	}
    return ret;
}

static int sbi_ecall_covh_handler(unsigned long extid, unsigned long funcid,
		struct sbi_trap_regs *regs, struct sbi_ecall_return *out)
{
	if (funcid == 3 || funcid == 4) return sbi_cvm_covi_fence(funcid);
	return cove_io_covh_ecall(funcid, regs, out);
}

static int sbi_ecall_covi_handler(unsigned long extid, unsigned long funcid,
		struct sbi_trap_regs *regs, struct sbi_ecall_return *out)
{
	return sbi_cvm_covi_ecall(funcid, regs->a0, regs->a1, regs->a2);
}

struct sbi_ecall_extension ecall_covi;
static int sbi_ecall_covi_register_extensions(void)
{
	return sbi_ecall_register_extension(&ecall_covi);
}
struct sbi_ecall_extension ecall_covi = {
	.extid_start = SBI_EXT_COVI, .extid_end = SBI_EXT_COVI,
	.register_extensions = sbi_ecall_covi_register_extensions,
	.handle = sbi_ecall_covi_handler,
};

struct sbi_ecall_extension ecall_cvm;
struct sbi_ecall_extension ecall_covh;
struct sbi_ecall_extension ecall_covi;

static int sbi_ecall_cvm_register_extensions(void)
{
    int ret = sbi_ecall_register_extension(&ecall_cvm);
    return ret ? ret : sbi_ecall_register_extension(&ecall_covi);
}

static int sbi_ecall_covh_register_extensions(void)
{
	return sbi_ecall_register_extension(&ecall_covh);
}

struct sbi_ecall_extension ecall_cvm = {
    // start = end
    .extid_start = SBI_EXT_CVM,
    .extid_end = SBI_EXT_CVM,
    // 每个 ecall 都需要注册，赋值注册函数的地址
    .register_extensions = sbi_ecall_cvm_register_extensions,
    // exception handler 地址
    .handle = sbi_ecall_cvm_handler,
};

struct sbi_ecall_extension ecall_covh = {
	.extid_start = SBI_EXT_COVH,
	.extid_end = SBI_EXT_COVH,
	.register_extensions = sbi_ecall_covh_register_extensions,
	.handle = sbi_ecall_covh_handler,
};
