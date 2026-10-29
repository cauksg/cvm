/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
.option push
.option arch, +d
SYM_FUNC_START(cvm_cpu_state_probe)
	addi sp, sp, -80
	sd ra, 0(sp)
	sd s2, 8(sp)
	sd s3, 16(sp)
	csrr t0, sscratch
	sd t0, 24(sp)
	csrr t0, stvec
	sd t0, 32(sp)
	csrr t0, satp
	sd t0, 40(sp)
	fsd f0, 48(sp)
	fsd f31, 56(sp)
	frcsr t0
	sd t0, 64(sp)
	li s2, 16
	li s3, 0
1:
	li t3, 0x13579bdf2468ace0
	li t4, 0x6a5a4a3a2a1a0908
	li t0, 0x1122334455667788
	fmv.d.x f0, t0
	li t0, 0x8877665544332211
	fmv.d.x f31, t0
	li t0, 0x61
	fscsr t0
	li t0, 0x1928374655aaeeff
	csrw sscratch, t0
	/* BASE GET_SPEC_VERSION has no scalar arguments. */
	li a6, 0
	li a7, 0x10
	ecall
	li t0, 0x13579bdf2468ace0
	beq t3, t0, 2f
	ori s3, s3, 1
2:
	li t0, 0x6a5a4a3a2a1a0908
	beq t4, t0, 3f
	ori s3, s3, 2
3:
	fmv.x.d t1, f0
	li t0, 0x1122334455667788
	beq t1, t0, 4f
	ori s3, s3, 4
4:
	fmv.x.d t1, f31
	li t0, 0x8877665544332211
	beq t1, t0, 5f
	ori s3, s3, 8
5:
	frcsr t1
	li t0, 0x61
	beq t1, t0, 6f
	ori s3, s3, 16
6:
	csrr t1, sscratch
	li t0, 0x1928374655aaeeff
	beq t1, t0, 7f
	ori s3, s3, 32
7:
	csrr t1, stvec
	ld t0, 32(sp)
	beq t1, t0, 8f
	ori s3, s3, 64
8:
	csrr t1, satp
	ld t0, 40(sp)
	beq t1, t0, 9f
	ori s3, s3, 128
9:
	addi s2, s2, -1
	bnez s2, 1b
	mv a0, s3
	ld t0, 24(sp)
	csrw sscratch, t0
	fld f0, 48(sp)
	fld f31, 56(sp)
	ld t0, 64(sp)
	fscsr t0
	ld ra, 0(sp)
	ld s2, 8(sp)
	ld s3, 16(sp)
	addi sp, sp, 80
	ret
SYM_FUNC_END(cvm_cpu_state_probe)
.option pop
