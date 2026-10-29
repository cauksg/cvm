/* SPDX-License-Identifier: BSD-2-Clause */
#include <sbi/sbi_cvm.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_ecall_interface.h>

/* Exact scalar arguments deliberately declassified by an emulation request.
 * Buffer-based SBI calls stay in the Monitor or return NOT_SUPPORTED.
 */
int sbi_cvm_state_sbi_args(unsigned long ext, unsigned long fid)
{
	switch (ext) {
	case SBI_EXT_BASE:
		return fid == SBI_EXT_BASE_PROBE_EXT ? 1 :
		       (fid <= SBI_EXT_BASE_GET_MIMPID ? 0 : -1);
	case SBI_EXT_TIME:
		return fid == 0 ? 1 : -1;
	case SBI_EXT_IPI:
	case SBI_EXT_SRST:
		return fid == 0 ? 2 : -1;
	case SBI_EXT_HSM:
		return fid == SBI_EXT_HSM_HART_START ? 3 :
		       (fid == SBI_EXT_HSM_HART_STOP ? 0 :
		       (fid == SBI_EXT_HSM_HART_GET_STATUS ? 1 : -1));
	case SBI_EXT_COVG:
		if (fid == 4 || fid == 5) return 1;
		if (fid == SBI_EXT_COVG_STOP_INTERFACE) return 2;
		if (fid == SBI_EXT_COVG_SIM_PREPARE_DMA) return 3;
		return -1;
	case 0: /* Legacy SET_TIMER, RV64 only. */
	case 1: /* Legacy console_putchar explicitly publishes one character. */
		return 1;
	case 2: /* Legacy console_getchar */
	case 8: /* Legacy shutdown */
		return 0;
	default:
		return -1;
	}
}

int sbi_cvm_state_resume(struct cvm_vcpu *vcpu,
			 const struct cpu_context *response)
{
	unsigned long *dst = (unsigned long *)&vcpu->guest_context;
	const unsigned long *src = (const unsigned long *)response;
	unsigned long value;

	/* The pending operation and register mask are Monitor-owned. A Host may
	 * choose an emulated result, but cannot choose PC, SP, VSATP or a new mask.
	 */
	if (response->sepc != vcpu->resume_len)
		return SBI_EDENIED;
	/* The Monitor already committed these notifications. Host scheduling
	 * cannot replace their successful result with forged return values. */
	if (vcpu->resume_kind == CVM_RESUME_NOTIFY) {
		vcpu->guest_context.a0 = 0;
		vcpu->guest_context.a1 = 0;
	}
	for (unsigned int reg = 1; reg < 32; reg++) {
		if (!(vcpu->resume_in_mask & (1U << reg))) continue;
		value = src[reg];
		if (vcpu->resume_kind == CVM_RESUME_MMIO && vcpu->mmio_width < 8) {
			unsigned int shift = 64 - vcpu->mmio_width * 8;
			value = vcpu->mmio_signed ?
				(unsigned long)((long)(value << shift) >> shift) :
				(value << shift) >> shift;
		}
		dst[reg] = value;
	}
	vcpu->guest_context.zero = 0;
	vcpu->guest_context.sepc += vcpu->resume_len;
	vcpu->resume_kind = CVM_RESUME_RETRY;
	vcpu->resume_len = 0;
	vcpu->resume_in_mask = vcpu->resume_out_mask = 0;
	return 0;
}

int sbi_cvm_state_mmio(struct cvm_vcpu *vcpu, struct cpu_trap *trap,
		       unsigned long instruction)
{
	bool store = trap->scause == CAUSE_STORE_GUEST_PAGE_FAULT;
	unsigned long insn = instruction;
	unsigned int reg, funct3, width, length;
	bool sign = false;

	if (trap->scause != CAUSE_LOAD_GUEST_PAGE_FAULT && !store)
		return SBI_EDENIED;
	if (trap->htinst & 1) {
		insn = trap->htinst | 3;
		length = (trap->htinst & 2) ? 4 : 2;
	} else {
		/* Nonzero special encodings describe implicit page-table accesses. */
		if (trap->htinst) return SBI_EDENIED;
		length = (insn & 3) == 3 ? 4 : 2;
	}
	if ((insn & 3) != 3) {
		unsigned int op = insn & 0xe003;
		if (!store && (op == INSN_MATCH_C_LW || op == INSN_MATCH_C_LD)) {
			reg = 8 + ((insn >> 2) & 7);
			funct3 = op == INSN_MATCH_C_LW ? 2 : 3;
		} else if (!store && (op == INSN_MATCH_C_LWSP || op == INSN_MATCH_C_LDSP)) {
			reg = (insn >> 7) & 31;
			if (!reg) return SBI_EDENIED;
			funct3 = op == INSN_MATCH_C_LWSP ? 2 : 3;
		} else if (store && (op == INSN_MATCH_C_SW || op == INSN_MATCH_C_SD)) {
			reg = 8 + ((insn >> 2) & 7);
			funct3 = op == INSN_MATCH_C_SW ? 2 : 3;
		} else if (store && (op == INSN_MATCH_C_SWSP || op == INSN_MATCH_C_SDSP)) {
			reg = (insn >> 2) & 31;
			funct3 = op == INSN_MATCH_C_SWSP ? 2 : 3;
		} else return SBI_EDENIED;
		insn = (funct3 << 12) | (reg << (store ? 20 : 7)) | (store ? 0x23 : 3);
	} else {
		if ((insn & 0x7f) != (store ? 0x23 : 3)) return SBI_EDENIED;
		funct3 = (insn >> 12) & 7;
		reg = (insn >> (store ? 20 : 7)) & 31;
	}
	if (funct3 > (store ? 3U : 6U) || (!store && funct3 == 7))
		return SBI_EDENIED;
	width = 1U << (funct3 & 3);
	sign = !store && funct3 < 3;
	unsigned long gpa = (trap->htval << 2) | (trap->stval & 3);
	if (gpa >= 0x80000000UL || gpa + width > 0x80000000UL ||
	    (gpa & (width - 1))) return SBI_EDENIED;
	vcpu->resume_kind = CVM_RESUME_MMIO;
	vcpu->resume_len = length;
	vcpu->resume_in_mask = store ? 0 : (1U << reg) & ~1U;
	vcpu->resume_out_mask = store ? (1U << reg) & ~1U : 0;
	vcpu->mmio_width = width;
	vcpu->mmio_signed = sign;
	/* Only the operand fields are public; address/immediate fields are zero. */
	trap->htinst = (insn & (store ? 0x01f0707fUL : 0x00007fffUL));
	if (length == 2) trap->htinst &= ~2UL;
	trap->stval &= 3;
	return 0;
}

void sbi_cvm_state_export(struct cvm_vcpu *vcpu,
			  struct cpu_context *context, struct vcpu_csr *csr)
{
	const unsigned long *src = (const unsigned long *)&vcpu->guest_context;
	unsigned long *dst = (unsigned long *)context;

	sbi_memset(context, 0, sizeof(*context));
	sbi_memset(csr, 0, sizeof(*csr));
	for (unsigned int reg = 1; reg < 32; reg++)
		if (vcpu->resume_out_mask & (1U << reg)) dst[reg] = src[reg];
	if (vcpu->resume_kind == CVM_RESUME_MMIO && vcpu->resume_out_mask &&
	    vcpu->mmio_width < 8) {
		for (unsigned int reg = 1; reg < 32; reg++)
			if (vcpu->resume_out_mask & (1U << reg))
				dst[reg] &= (1UL << (vcpu->mmio_width * 8)) - 1;
	}
	context->hstatus = HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_VTW;
	context->sstatus = MSTATUS_SPP | MSTATUS_SPIE;
	/* Scheduling hints only. No Host CSR is imported back as private state. */
	csr->hvip = vcpu->guest_csr.hvip & (MIP_VSSIP | MIP_VSTIP | MIP_VSEIP);
	/* Conservative wakeup mask, independent of the private VSIE value. */
	csr->vsie = (1UL << 1) | (1UL << 5) | (1UL << 9);
	csr->vstimecmp = vcpu->guest_csr.vstimecmp;
}
