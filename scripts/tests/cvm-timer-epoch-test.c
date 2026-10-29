/* Run production timer helpers: public scheduling must share Monitor time. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint64_t u64;
struct kvm_guest_timer { u64 time_delta; unsigned int nsec_mult,nsec_shift; };
struct kvm { bool cmode; struct { struct kvm_guest_timer timer; } arch; };
static u64 cycles = 90000000;
static u64 get_cycles64(void) { return cycles; }
static void riscv_cs_get_mult_shift(unsigned int *m,unsigned int *s) { *m=100;*s=0; }
#include "cycles.inc"
#include "init.inc"
int main(void) {
 struct kvm cvm={.cmode=true}, vm={.cmode=false};
 kvm_riscv_guest_timer_init(&cvm);kvm_riscv_guest_timer_init(&vm);
 assert(cvm.arch.timer.time_delta==0);
 assert(kvm_riscv_current_cycles(&cvm.arch.timer)==cycles);
 assert(kvm_riscv_current_cycles(&vm.arch.timer)==0);
 cycles+=12345;
 u64 deadline=cycles+100;
 assert(deadline-kvm_riscv_current_cycles(&cvm.arch.timer)==100);
 assert(kvm_riscv_current_cycles(&vm.arch.timer)==12345);
 puts("PASS: CVM Host scheduling matches Monitor physical-time epoch; normal VM relative epoch retained");
 return 0;
}
