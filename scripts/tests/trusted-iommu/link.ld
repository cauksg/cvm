OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS {
 . = 0x80200000;
 .text : { KEEP(*(.text.start)) *(.text .text.*) }
 .rodata : { *(.rodata .rodata.*) }
 .data : { *(.data .data.*) *(.sdata .sdata.*) }
 . = ALIGN(4096);
 _bss_start = .;
 .bss : { *(.bss .bss.*) *(.sbss .sbss.*) *(COMMON) }
 . = ALIGN(16); _bss_end = .;
 . += 16384; _stack_top = .;
}
