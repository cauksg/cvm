.option norvc
.text
.globl probe_irq_read
probe_irq_read:
 csrr a0,0x251
 ecall
.globl probe_irq_clear
probe_irq_clear:
 csrc 0x251,a1
 ecall
.globl probe_irq_set
probe_irq_set:
 csrs 0x251,a1
 ecall
.globl probe_irq_topei
probe_irq_topei:
 csrrw a0,0x25c,zero
 ecall
.globl probe_vs_irq_read
probe_vs_irq_read:
 li t0,0x80
 csrw 0x150,t0
 csrr a0,0x151
 ecall
.globl probe_vs_irq_topei
probe_vs_irq_topei:
 csrrw a0,0x15c,zero
 ecall

.globl probe_hgeip
probe_hgeip:
 csrr a0,0xe12
 ecall
.globl probe_hip
probe_hip:
 csrr a0,0x644
 ecall
.globl probe_vstopi
probe_vstopi:
 csrr a0,0xeb0
 ecall
