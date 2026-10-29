// SPDX-License-Identifier: GPL-2.0
/* Opt-in integration probe: secrets must survive repeated Host exits. */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/irqflags.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/io.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <asm/fpu.h>
#include <asm/sbi.h>

static bool enabled;
static int __init cpu_state_setup(char *value)
{
	return kstrtobool(value, &enabled);
}
early_param("cvm_cpu_state_test", cpu_state_setup);

extern unsigned long cvm_cpu_state_probe(void);

static atomic_t smp_probe_count = ATOMIC_INIT(0);
static atomic_t smp_probe_fail = ATOMIC_INIT(0);
static void cvm_smp_state_probe(void *unused)
{
	unsigned long result, flags;
	struct sbiret denied, allowed, revoked, restored;
	/* Different CPUs execute the same private-state probe while sharing
	 * one physical hart. Each ecall is a real Monitor/Host round trip. */
	kernel_fpu_begin();
	local_irq_save(flags);
	denied = sbi_ecall(SBI_EXT_COVG, 5, ~0UL, 0, 0, 0, 0, 0);
	allowed = sbi_ecall(SBI_EXT_COVG, 4, 254, 0, 0, 0, 0, 0);
	revoked = sbi_ecall(SBI_EXT_COVG, 5, 254, 0, 0, 0, 0, 0);
	restored = sbi_ecall(SBI_EXT_COVG, 4, ~0UL, 0, 0, 0, 0, 0);
	result = cvm_cpu_state_probe();
	if (denied.error || allowed.error || revoked.error || restored.error)
		result |= BIT(31);
	local_irq_restore(flags);
	kernel_fpu_end();
	if (result) atomic_inc(&smp_probe_fail);
	atomic_inc(&smp_probe_count);
	pr_info("CVM SMP CPU state: cpu=%u mask=%lx\n", smp_processor_id(), result);
}

static int __init cpu_state_test(void)
{
	unsigned long result, flags;
	struct sbiret probe;

	if (!enabled) return 0;
	probe = sbi_ecall(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT,
			 SBI_EXT_COVG, 0, 0, 0, 0, 0);
	if (probe.error || !probe.value) return -ENODEV;
	cpus_read_lock();
	on_each_cpu(cvm_smp_state_probe, NULL, 1);
	if (atomic_read(&smp_probe_fail) || atomic_read(&smp_probe_count) != num_online_cpus()) {
		cpus_read_unlock();
		pr_err("CVM SMP selftest: FAIL\n");
		return -EIO;
	}
	pr_info("CVM SMP selftest: PASS cpus=%d private-state/IPI\n", atomic_read(&smp_probe_count));
	cpus_read_unlock();
	kernel_fpu_begin();
	local_irq_save(flags);
	result = cvm_cpu_state_probe();
	local_irq_restore(flags);
	kernel_fpu_end();
	if (result) {
		pr_err("CVM CPU state selftest: FAIL mask=0x%lx\n", result);
		return -EIO;
	}
    pr_info("CVM CPU state selftest: PASS GPR/FP/FCSR/SSCRATCH/STVEC/SATP across 16 Host exits\n");
    /* Direct DMA never uses this legacy reserved pool. Exercise actual
     * Guest revocation/fault/reaccept on its exact advertised window. */
    {
        unsigned long base = 0x82d00000, size = 0x100000, value = 0;
        unsigned long *shared = phys_to_virt(base);
        struct sbiret ret;
        ret = sbi_ecall(SBI_EXT_COVG, 1089, base, size, 0, 0, 0, 0);
        if (ret.error == SBI_ERR_NOT_SUPPORTED) return 0; /* baseline */
        if (ret.error != SBI_ERR_ALREADY_AVAILABLE) goto share_fail;
        ret = sbi_ecall(SBI_EXT_COVG, 1089, base, PAGE_SIZE, 0, 0, 0, 0);
        if (!ret.error) goto share_fail;
        WRITE_ONCE(*shared, 0x123456789abcdef0UL);
        ret = sbi_ecall(SBI_EXT_COVG, 1090, base, size, 0, 0, 0, 0);
        if (ret.error) goto share_fail;
        if (!copy_from_kernel_nofault(&value, shared, sizeof(value))) goto share_fail;
        ret = sbi_ecall(SBI_EXT_COVG, 1090, base, size, 0, 0, 0, 0);
        if (!ret.error) goto share_fail;
        ret = sbi_ecall(SBI_EXT_COVG, 1089, base, size, 0, 0, 0, 0);
        if (ret.error) goto share_fail;
        if (copy_from_kernel_nofault(&value, shared, sizeof(value)) ||
            value != 0x123456789abcdef0UL) goto share_fail;
        pr_info("CVM shared window selftest: PASS exact consent, partial/replay denial, unshare fault and reaccept\n");
    }
    return 0;
share_fail:
    pr_err("CVM shared window selftest: FAIL\n");
    return -EIO;
}
late_initcall(cpu_state_test);
