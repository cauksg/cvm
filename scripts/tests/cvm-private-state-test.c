/* Exercise the production policy, including poisoned Host return buffers. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sbi/sbi_cvm.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_ecall_interface.h>

int main(void)
{
	struct cvm_vcpu v = {0};
	struct cpu_context output, response, saved;
	struct vcpu_csr csr;
	struct cpu_trap trap;
	unsigned long *registers = (unsigned long *)&v.guest_context;

	for (unsigned int i = 0; i < 35; i++) registers[i] = 0x123400000000UL + i;
	v.guest_context.zero = 0;
	memset(&v.guest_csr, 0xac, sizeof(v.guest_csr));
	v.resume_kind = CVM_RESUME_SBI;
	v.resume_len = 4;
	v.resume_in_mask = (1U << 10) | (1U << 11);
	v.resume_out_mask = (1U << 10) | (1U << 16) | (1U << 17);
	sbi_cvm_state_export(&v, &output, &csr);
	for (unsigned int i = 0; i < 32; i++)
		assert(((unsigned long *)&output)[i] ==
		       ((v.resume_out_mask & (1U << i)) ? registers[i] : 0));
	assert(!output.sepc && !csr.vsatp && !csr.vstvec && !csr.vsscratch);
	assert(!csr.vsepc && !csr.vstval && !csr.vscause && !csr.vsstatus);
	saved = v.guest_context;
	memset(&response, 0xff, sizeof(response));
	response.sepc = 6;
	assert(sbi_cvm_state_resume(&v, &response) == SBI_EDENIED);
	assert(!memcmp(&saved, &v.guest_context, sizeof(saved)));
	response.sepc = 4;
	response.a0 = 0x42;
	response.a1 = 0x43;
	assert(!sbi_cvm_state_resume(&v, &response));
	saved.a0 = 0x42; saved.a1 = 0x43; saved.sepc += 4;
	assert(!memcmp(&saved, &v.guest_context, sizeof(saved)));
	assert(sbi_cvm_state_resume(&v, &response) == SBI_EDENIED);
	puts("PASS private GPR/CSR export, PC tamper rejection, bounded SBI import and replay rejection");

	for (unsigned int f = 0; f <= 6; f++) {
		trap = (struct cpu_trap){.scause = CAUSE_LOAD_GUEST_PAGE_FAULT,
			.htval = 0x10000000 >> 2, .stval = 0xffff000010000000UL};
		assert(!sbi_cvm_state_mmio(&v, &trap, (f << 12) | (9 << 7) | 3));
		memset(&response, 0xff, sizeof(response));
		response.sepc = 4;
		assert(!sbi_cvm_state_resume(&v, &response));
		assert(v.guest_context.s1 == (f >= 4 ? (1UL << (8U << (f & 3))) - 1 : ~0UL));
		assert(!trap.stval && !(trap.htinst & 0xffff8000UL));
	}
	trap = (struct cpu_trap){.scause = CAUSE_STORE_GUEST_PAGE_FAULT,
		.htval = 0x10000000 >> 2};
	v.guest_context.s1 = 0xabcdef0123456789UL;
	assert(!sbi_cvm_state_mmio(&v, &trap, (9 << 20) | 0x23));
	sbi_cvm_state_export(&v, &output, &csr);
	assert(output.s1 == 0x89 && !v.resume_in_mask);
	trap = (struct cpu_trap){.scause = CAUSE_LOAD_GUEST_PAGE_FAULT,
		.htval = 0x10000000 >> 2};
	assert(!sbi_cvm_state_mmio(&v, &trap, INSN_MATCH_C_LW | (3 << 2)));
	assert(v.resume_len == 2 && v.resume_in_mask == (1U << 11));
	assert((trap.htinst & 3) == 1);
	trap.htinst = 0x3000;
	assert(sbi_cvm_state_mmio(&v, &trap, 3) == SBI_EDENIED);
	trap.htinst = 0; trap.htval = 0x80000000UL >> 2;
	assert(sbi_cvm_state_mmio(&v, &trap, 3) == SBI_EDENIED);
	puts("PASS MMIO width/sign extension, store truncation, compressed decode and private RAM/implicit-walk denial");

	assert(sbi_cvm_state_sbi_args(SBI_EXT_BASE, SBI_EXT_BASE_PROBE_EXT) == 1);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_TIME, 0) == 1);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_SRST, 0) == 2);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_COVG, SBI_EXT_COVG_SIM_PREPARE_DMA) == 3);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_DBCN, 0) < 0);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_PMU, 0) < 0);
	assert(sbi_cvm_state_sbi_args(0x09000000, 0) < 0);
	puts("PASS exact SBI argument counts and buffer/vendor extension denial");
	v.resume_kind = CVM_RESUME_NOTIFY; v.resume_len = 4;
	v.resume_in_mask = 0;
	memset(&response, 0xff, sizeof(response)); response.sepc = 4;
	assert(!sbi_cvm_state_resume(&v, &response));
	assert(!v.guest_context.a0 && !v.guest_context.a1);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_COVG, 4) == 1);
	assert(sbi_cvm_state_sbi_args(SBI_EXT_COVG, 5) == 1);
	puts("PASS committed COVG/HSM notification ignores forged Host results");
	/* Different vCPUs must retain different secrets while the Host alternates
	 * their response buffers, not merely reproduce the same sentinel. */
	struct cvm_vcpu pair[2] = {0};
	for (unsigned int i = 0; i < 2; i++) {
		pair[i].guest_context.sp = 0x90000000UL + 0x10000 * i;
		pair[i].guest_context.t3 = 0xabc000UL + i;
		pair[i].guest_context.sepc = 0x80200000UL + 0x1000 * i;
		pair[i].guest_csr.vsatp = 0x8000000000080000UL + i;
		pair[i].guest_csr.vsscratch = 0x12340000UL + i;
		pair[i].guest_fp.f[0] = 0x1122334455667700UL + i;
		pair[i].imsic.file = i + 1;
		pair[i].imsic.allowed[0] = 1UL << (i + 2);
	}
	for (unsigned int turn = 0; turn < 64; turn++) {
		unsigned int i = turn % 2;
		struct cvm_vcpu other = pair[1 - i];
		pair[i].resume_kind = CVM_RESUME_SBI;
		pair[i].resume_len = 4;
		pair[i].resume_in_mask = (1U << 10) | (1U << 11);
		pair[i].resume_out_mask = (1U << 17);
		sbi_cvm_state_export(&pair[i], &output, &csr);
		assert(!output.sp && !output.t3 && !csr.vsatp && !csr.vsscratch);
		memset(&response, 0xff, sizeof(response));
		response.sepc = 4; response.a0 = turn; response.a1 = i;
		assert(!sbi_cvm_state_resume(&pair[i], &response));
		assert(!memcmp(&other, &pair[1 - i], sizeof(other)));
		assert(pair[i].guest_context.sp == 0x90000000UL + 0x10000 * i);
		assert(pair[i].guest_context.t3 == 0xabc000UL + i);
		assert(pair[i].guest_csr.vsatp == 0x8000000000080000UL + i);
		assert(pair[i].guest_csr.vsscratch == 0x12340000UL + i);
		assert(pair[i].guest_fp.f[0] == 0x1122334455667700UL + i);
		assert(pair[i].imsic.file == i + 1);
		assert(pair[i].imsic.allowed[0] == (1UL << (i + 2)));
	}
	assert(pair[0].guest_context.sepc == 0x80200000UL + 32 * 4);
	assert(pair[1].guest_context.sepc == 0x80201000UL + 32 * 4);
	puts("PASS distinct vCPU secrets and IMSIC policy survive 64 interleaved poisoned Host responses");
	return 0;
}
