/* SPDX-License-Identifier: GPL-2.0-only */
/* os2shim stage0: the big-endian front end, one flat section at 0x600000 (in
 * the firmware's free RAM, clear of the shim at 0x50000 and of Open Firmware
 * itself, which a factory-default firmware keeps at 4-5 MB). */
ENTRY(_start)
SECTIONS
{
    . = 0x00600000;
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
