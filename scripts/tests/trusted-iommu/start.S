.option norvc
.section .text.start,"ax"
.globl _start
_start:
 la sp, _stack_top
 la t0, _bss_start
 la t1, _bss_end
1: bgeu t0, t1, 2f
 sd zero, 0(t0)
 addi t0,t0,8
 j 1b
2:
 la t0, trap_vector
 csrw stvec,t0
 call test_main
 j .
.balign 4
trap_vector:
 csrr t0,scause
 la t1,trap_cause
 sd t0,0(t1)
 csrr t0,sepc
 addi t0,t0,4
 csrw sepc,t0
 sret
.globl probe_load
probe_load:
 ld a0,0(a0)
 ret
.globl probe_store
probe_store:
 sd a1,0(a0)
 ret
