/* SPDX-License-Identifier: GPL-2.0-only
 * os2shim -- start.S: entry from Open Firmware's pe-loader, and the hand-off.
 *
 * Entered at ImageBase + 0 by `go`, in Open Firmware's little-endian mode,
 * with %r5 = the client interface handler (IEEE 1275 PowerPC binding).
 */
#define LI32(r, v) lis r, (v)@h; ori r, r, (v)@l

    .section .text.start, "ax"
    .globl _start
_start:
    lis     %r6, shim_cif@ha
    stw     %r5, shim_cif@l(%r6)          /* keep the client interface */
    lis     %r1, (shim_stack + 16384)@ha
    addi    %r1, %r1, (shim_stack + 16384)@l
    li      %r0, 0
    stwu    %r0, -16(%r1)
    bl      shim_main                   /* never returns */
1:  b       1b

/* Leave Open Firmware: interrupts off, then four BATs: IBM's stub's
 * 0-256 MB identity (cacheable), $80000000 and $A0000000 identity,
 * cache-inhibited and guarded, as the stub leaves them, and in place of its
 * $C0000000 (PReP PCI memory, which the loader never touches) the Network
 * Server's own I/O at $F0000000: Bandit 1's ports and Grand Central, for the
 * shim and for the loader service (ldrsvc.c).  The shim's own image, stack and work areas
 * are below 256 MB and Open Firmware maps them virtual == physical, so
 * execution continues through the switch. */
    .text
    .globl shim_take_mmu
shim_take_mmu:
    mfmsr   %r3
    rlwinm  %r3, %r3, 0, 17, 15           /* EE off */
    mtmsr   %r3
    isync
    li      %r0, 0
    mtspr   528, %r0                     /* invalidate every upper half first */
    mtspr   530, %r0
    mtspr   532, %r0
    mtspr   534, %r0
    mtspr   536, %r0
    mtspr   538, %r0
    mtspr   540, %r0
    mtspr   542, %r0
    isync
    li      %r3, 0x52
    mtspr   529, %r3                     /* IBAT0L: BRPN 0, WIMG 0010, PP 10 */
    li      %r3, 0x1FFF
    mtspr   528, %r3                     /* IBAT0U: BEPI 0, 256 MB, Vs Vp */
    li      %r3, 0x52
    mtspr   537, %r3                     /* DBAT0 = IBAT0 */
    li      %r3, 0x1FFF
    mtspr   536, %r3
    LI32(%r3, 0x80000032)
    mtspr   539, %r3                     /* DBAT1L: $80000000, WIMG 0110 */
    LI32(%r3, 0x80001FFF)
    mtspr   538, %r3
    LI32(%r3, 0xA0000032)
    mtspr   541, %r3                     /* DBAT2: $A0000000 */
    LI32(%r3, 0xA0001FFF)
    mtspr   540, %r3
    LI32(%r3, 0xF0000032)
    mtspr   543, %r3                     /* DBAT3: $F0000000, the Mac's own I/O */
    LI32(%r3, 0xF0001FFF)
    mtspr   542, %r3
    isync
    sync
    blr

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
