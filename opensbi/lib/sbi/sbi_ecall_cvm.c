#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_cvm.h>
#include <sbi/sbi_console.h>

static int sbi_ecall_cvm_handler(unsigned long extid, unsigned long funcid,
        struct sbi_trap_regs *regs, struct sbi_ecall_return *out)
{
    int ret = 0;
	// uint64_t temp;
	sbi_printf("sbi_ecall_cvm_handler!\n");
	switch (funcid) {
	case SBI_EXT_CVM_CREATE:
		ret = sbi_cvm_create((void *)regs->a0);
		break;
	case SBI_EXT_CVM_CREATE_MEMORY_REGION:
		ret = sbi_cvm_create_memory_region((void *)regs->a0);
		break;
	case SBI_EXT_CVM_MEASURED_PAGES:
		ret = sbi_cvm_create_measured_pages((void *)regs->a0);
		break;
	case SBI_EXT_CVM_CREATE_VCPU:
		ret = sbi_cvm_create_vcpu((void *)regs->a0);
		break;
	case SBI_EXT_CVM_RUN_VCPU:
		ret = sbi_cvm_run_vcpu(regs, (void *)regs->a0, regs->a1, regs->a2);
		out->skip_regs_update = true;
		break;
	case SBI_EXT_CVM_FINALIZE:
		ret = sbi_cvm_create_finalize((void *)regs->a0);
		break;
	case SBI_EXT_CVM_INIT_MEM_POOL:
		ret = sbi_cvm_init_mem_pool((void *)regs->a0);
		break;
	case SBI_EXT_CVM_LOAD_FILE:
		ret = load_file((void *)regs->a0);
	case SBI_EXT_CVM_INIT_PAGE_LIST:
		ret = convert_cvm_pages((void *)regs->a0, regs->a1);
	default:
		break;
	}
    return ret;
}
struct sbi_ecall_extension ecall_cvm;

static int sbi_ecall_cvm_register_extensions(void)
{
    return sbi_ecall_register_extension(&ecall_cvm);
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

