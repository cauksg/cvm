.option norvc
.section .text.start,"ax"
.globl _start
_start:
  la sp, _stack_top
  la t0, trap_vector
  csrw mtvec, t0
  csrw mie, zero
  csrw medeleg, zero
  li t0, 0x6600
  csrs mstatus, t0
  la t0, _bss_start
  la t1, _bss_end
1: bgeu t0, t1, 2f
  sd zero, 0(t0)
  addi t0, t0, 8
  j 1b
2: call test_main
  li a0, 254
  li t0, 0x100000
  li t1, 0x13333
  sw t1, 0(t0)
  j .

.section .text,"ax"
.globl run_mode
/* run_mode(entry, argument, value, mode): probes use caller-saved registers. */
run_mode:
  addi sp, sp, -16
  sd ra, 0(sp)
  csrw mscratch, sp
  la t0, probe_active
  li t1, 1
  sd t1, 0(t0)
  csrw mepc, a0
  csrr t0, mstatus
  li t1, 0x21800
  not t1, t1
  and t0, t0, t1
  slli t1, a3, 11
  or t0, t0, t1
  csrw mstatus, t0
  mv a0, a1
  mv a1, a2
  mret
probe_return:
  ld ra, 0(sp)
  addi sp, sp, 16
  ret

.balign 4
trap_vector:
  /* MPRV survives an M-mode trap: clear it before touching handler data. */
  li t0, 0x20000
  csrc mstatus, t0
  la t0, probe_active
  ld t1, 0(t0)
  beqz t1, unexpected
  sd zero, 0(t0)
  csrr sp, mscratch
  la t0, trap_value
  sd a0, 0(t0)
  la t0, trap_cause
  csrr t1, mcause
  sd t1, 0(t0)
  la t0, trap_tval
  csrr t1, mtval
  sd t1, 0(t0)
  la t0, trap_epc
  csrr t1, mepc
  sd t1, 0(t0)
  li t0, 0x1800
  csrs mstatus, t0
  la t0, probe_return
  csrw mepc, t0
  mret
unexpected:
  csrr a0, mcause
  csrr a1, mepc
  csrr a2, mtval
  tail fatal_trap

.macro probe name, instruction
.globl \name
\name:
  \instruction
  ecall
.endm

probe probe_load, "ld a0, 0(a0)"
probe probe_store, "sd a1, 0(a0)"
probe probe_amo, "amoadd.d a0, a1, (a0)"
probe probe_lr, "lr.d a0, (a0)"
probe probe_sc, "sc.d a0, a1, (a0)"
probe probe_cbo, "cbo.zero (a0)"
.globl probe_fetch
probe_fetch:
  jr a0
.globl probe_vector
probe_vector:
  vsetivli t0, 1, e64, m1, ta, ma
  vle64.v v0, (a0)
  vmv.x.s a0, v0
  ecall
.globl probe_vector_store
probe_vector_store:
  vsetivli t0, 1, e64, m1, ta, ma
  vmv.v.x v0, a1
  vse64.v v0, (a0)
  ecall
.globl probe_mprv_load
probe_mprv_load:
  li t0, 0x1800
  csrc mstatus, t0
  li t0, 0x20800
  csrs mstatus, t0
  ld a0, 0(a0)
  li t0, 0x20000
  csrc mstatus, t0
  ecall
.globl probe_mprv_store
probe_mprv_store:
  li t0, 0x1800
  csrc mstatus, t0
  li t0, 0x20800
  csrs mstatus, t0
  sd a1, 0(a0)
  li t0, 0x20000
  csrc mstatus, t0
  ecall
.globl probe_hlv
probe_hlv:
  hlv.d a0, (a0)
  ecall

probe probe_load32, "lw a0, 0(a0)"
probe probe_store32, "sw a1, 0(a0)"
probe probe_csr, "csrr a0, 0xbc2"
probe probe_mmio_csr_write, "csrw 0xbc3, a0"
