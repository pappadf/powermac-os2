/* SPDX-License-Identifier: GPL-2.0-only */
/* The loader service (ldrsvc.c): one block at LDRSVC, its branch table first. */
SECTIONS {
    . = 0x40000;
    .text : {
        *(.text.entry)
        *(.text .text.*)
        *(.rodata .rodata.*)
        *(.data .data.* .sdata .sdata.*)
        *(.bss .bss.* .sbss .sbss.* COMMON)
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
