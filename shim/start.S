/* SPDX-License-Identifier: GPL-2.0-only
 * os2shim -- start.S: entry from stage0, and the hand-off.
 *
 * Entered at the image base by stage0's rfi (stage0.S), already in
 * little-endian mode with the BATs loaded and the firmware left behind:
 * %r3 = RAM size, %r4 = 'LE01'.  Both are passed straight to shim_main.
 */
#define LI32(r, v) lis r, (v)@h; ori r, r, (v)@l

    .section .text.start, "ax"
    .globl _start
_start:
    lis     %r1, (shim_stack + 16384)@ha
    addi    %r1, %r1, (shim_stack + 16384)@l
    li      %r0, 0
    stwu    %r0, -16(%r1)
    bl      shim_main                   /* never returns */
1:  b       1b

/* The hand-off: the machine state IBM's big-endian entry stub leaves when it
 * enters the little-endian body (recorded on a PReP-mode run: registers,
 * segment registers, HID0), then `rfi` to the body with MSR = ILE | ME | IR |
 * DR | LE.  %r3 = entry, %r4 = the swapped length, %r14 = residual, %r15 = its
 * length, %r16 = the machine type -- passed in from C. */
    .globl shim_handoff
shim_handoff:                           /* (entry, swapped_len, residual, residual_len) */
    mr      %r11, %r3
    mr      %r12, %r4
    mr      %r14, %r5
    mr      %r15, %r6
    li      %r0, 0
    mtsr    0, %r0
    mtsr    1, %r0
    mtsr    2, %r0
    mtsr    3, %r0
    mtsr    4, %r0
    mtsr    5, %r0
    mtsr    6, %r0
    mtsr    7, %r0
    mtsr    8, %r0
    mtsr    9, %r0
    mtsr    10, %r0
    mtsr    11, %r0
    mtsr    12, %r0
    mtsr    13, %r0
    mtsr    14, %r0
    mtsr    15, %r0
    isync
    mfspr   %r3, 1008                    /* HID0: flash-invalidate the I-cache */
    ori     %r4, %r3, 0x800
    mtspr   1008, %r4
    isync
    mtspr   1008, %r3
    isync
    sync
    mfmsr   %r3                          /* ILE on: a 60x rfi loads MSR[16-31] only */
    oris    %r3, %r3, 1
    mtmsr   %r3
    isync
    mtsrr0  %r11
    li      %r3, 0x1031
    mtsrr1  %r3
    addi    %r3, %r11, -92                /* LR as the stub leaves it: entry - $5C */
    mtlr    %r3
    LI32(%r3, 0x82822240)
    mtcr    %r3
    li      %r3, 0
    mtxer   %r3
    mr      %r3, %r11
    mr      %r4, %r12
    li      %r5, 0x46A8
    li      %r6, 0x554C
    li      %r7, -20
    li      %r8, 0
    li      %r9, 0
    li      %r10, 0x1031
    li      %r13, 0
    li      %r16, 5                      /* Sandalfoot */
    li      %r24, 2
    addi    %r25, %r11, -64               /* entry - $40 */
    LI32(%r26, 0x80004012)               /* port $92 in the spread map */
    LI32(%r1, 0x000FFFF8)
    LI32(%r0, 0x00100130)
    rfi
