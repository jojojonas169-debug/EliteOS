/* User programs are static ELF executables loaded at 4 MiB. */
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text : { *(.text.start) *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    . = ALIGN(4096);
    .data : { *(.data .data.*) }
    .bss : { *(COMMON) *(.bss .bss.*) }
    /DISCARD/ : { *(.eh_frame*) *(.note*) *(.comment) }
}
