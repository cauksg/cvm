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
	switch (funcid) {
	case SBI_EXT_CVM_CREATE:
		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_CREATE.\n", __func__);
		ret = sbi_cvm_create((void *)regs->a0);
		break;
	case SBI_EXT_CVM_CREATE_MEMORY_REGION:
   		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_CREATE_MEMORY_REGION.\n", __func__);
		ret = sbi_cvm_create_memory_region((void *)regs->a0);
		break;
	case SBI_EXT_CVM_MEASURED_PAGES:
    	sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_MEASURED_PAGES.\n", __func__);
		ret = sbi_cvm_create_measured_pages((void *)regs->a0);
		break;
	case SBI_EXT_CVM_CREATE_VCPU:
		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_CREATE_VCPU.\n", __func__);
		ret = sbi_cvm_create_vcpu((void *)regs->a0);
		break;
	case SBI_EXT_CVM_RUN_VCPU:
		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_RUN_VCPU.\n", __func__);
		ret = sbi_cvm_run_vcpu((void *)regs->a0);
		break;
	case SBI_EXT_CVM_FINALIZE:
		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_FINALIZE.\n", __func__);
		ret = sbi_cvm_create_finalize((void *)regs->a0);
		break;
	case SBI_EXT_CVM_INIT_MEM_POOL:
		sbi_printf("[IIE CVM Monitor@%s] SBI_EXT_CVM_INIT_MEM_POOL.\n", __func__);
		ret = sbi_cvm_init_mem_pool((void *)regs->a0);
		break;
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

