/* SPDX-License-Identifier: GPL-2.0-only */
/* os2shim: one flat section at the image base; the pe-loader lays it out there. */
ENTRY(_start)
SECTIONS
{
    . = 0x00050000;
    .text : {
        KEEP(*(.text.start))
        *(.text .text.*)
        *(.rodata .rodata.* .sdata2 .sdata2.*)
        *(.data .data.* .sdata .sdata.*)
        . = ALIGN(16);
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(512);
    }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
