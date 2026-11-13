/* SPDX-License-Identifier: GPL-2.0-only
 * os2shim -- stage0.S: the big-endian front end's entry, and the switch to
 * little-endian mode.
 *
 * Open Firmware runs big-endian, as it does on every Mac out of the box: the
 * floppy sets no NVRAM variable.  The firmware's xcoff-loader lays this image
 * out at its link address and `go` enters _start with %r5 = the client
 * interface handler (IEEE 1275 PowerPC binding).  stage0.c asks the firmware
 * what it needs; stage0_switch then leaves it for good and enters the
 * little-endian shim.  A power cycle or reset brings the machine back
 * big-endian: nothing persistent changes.
 */
#define LI32(r, v) lis r, (v)@h; ori r, r, (v)@l

    .section .text.start, "ax"
    .globl _start
_start:
    lis     %r6, stage0_cif@ha
    stw     %r5, stage0_cif@l(%r6)
    lis     %r1, (stage0_stack + 8192)@ha
    addi    %r1, %r1, (stage0_stack + 8192)@l
    li      %r0, 0
    stwu    %r0, -16(%r1)
    bl      stage0_main                 /* never returns */
1:  b       1b

    .text
/* stage0_switch(ram, bandits, nbandits, payload, payload_len)
 *
 * 1. Interrupts off, and the shim's BATs (it used to load them itself, from
 *    a firmware already little-endian): 0-256 MB identity, cacheable;
 *    $80000000 and $A0000000 identity, cache-inhibited and guarded; and
 *    $F0000000-$FFFFFFFF, the Mac's own I/O (Bandit's ports, Grand Central),
 *    cache-inhibited and guarded.  This image, its stack and the shim's
 *    address are all below 256 MB, so execution continues through it.
 * 2. The shim (a little-endian flat image) to SHIM_BASE, each doubleword's
 *    eight bytes reversed: what a little-endian load of the file through a
 *    lane-reversing bridge would have left, and what the 60x's
 *    little-endian address munge reads as the image's own bytes.
 * 3. Data cache written back, instruction cache invalidated.
 * 4. Each Bandit's byte lanes reversed, by the same bus cycles the ROM makes
 *    when it starts in little-endian mode: config address $50080000 (the
 *    bridge's own device 11, register $50, mode-select) stored as a word at
 *    address port + 4 and read back, then a byte read-modify-write of the
 *    data port's byte 7 clearing its bit 0.  (The ROM runs that sequence
 *    with the 604 already little-endian, so its munged accesses reach port
 *    + 4 and data + 7; issued big-endian, these are the same cycles.)
 * 5. ILE on (a 60x rfi loads MSR[16-31] only), then rfi into the shim with
 *    MSR = ME | IR | DR | LE, %r3 = RAM size.  Nothing touches the bridges
 *    between step 4 and the shim.
 */
#define SHIM_BASE 0x00050000

    .globl stage0_switch
stage0_switch:
    mr      %r20, %r3                    /* ram */
    mr      %r21, %r4                    /* bandit bases */
    mr      %r22, %r5                    /* count */
    mr      %r23, %r6                    /* payload */
    mr      %r24, %r7                    /* payload length */

    mfmsr   %r3
    rlwinm  %r3, %r3, 0, 17, 15          /* EE off */
    mtmsr   %r3
    isync
    li      %r0, 0
    mtspr   528, %r0                     /* every upper half invalid first */
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

    /* 2. the shim, each doubleword reversed */
    LI32(%r8, SHIM_BASE)
    addi    %r9, %r24, 7
    srwi.   %r9, %r9, 3                  /* doublewords */
    beq     3f
    mtctr   %r9
    mr      %r10, %r23
2:  lwbrx   %r11, 0, %r10                /* bytes 0-3, reversed */
    addi    %r12, %r10, 4
    lwbrx   %r12, 0, %r12                /* bytes 4-7, reversed */
    stw     %r12, 0(%r8)                 /* 7 6 5 4 */
    stw     %r11, 4(%r8)                 /* 3 2 1 0 */
    addi    %r10, %r10, 8
    addi    %r8, %r8, 8
    bdnz    2b
3:
    /* 3. caches */
    LI32(%r8, SHIM_BASE)
    add     %r9, %r8, %r24
4:  dcbst   0, %r8
    addi    %r8, %r8, 32
    cmplw   %r8, %r9
    blt     4b
    sync
    mfspr   %r3, 1008                    /* HID0: flash-invalidate the I-cache */
    ori     %r4, %r3, 0x800
    mtspr   1008, %r4
    isync
    mtspr   1008, %r3
    isync
    sync

    /* 4. the bridges */
    cmpwi   %r22, 0
    beq     6f
    mtctr   %r22
    LI32(%r9, 0x50080000)
5:  lwz     %r8, 0(%r21)                 /* bridge base ($F2000000, $F4000000) */
    addis   %r10, %r8, 0x80              /* + $800000: config address */
    addis   %r11, %r8, 0xC0              /* + $C00000: config data */
    stw     %r9, 4(%r10)
    sync
    lwz     %r12, 4(%r10)
    sync
    lbz     %r12, 7(%r11)
    sync
    andi.   %r12, %r12, 0xFE
    stb     %r12, 7(%r11)
    sync
    eieio
    addi    %r21, %r21, 4
    bdnz    5b
6:
    /* 5. into the shim */
    mfmsr   %r3
    oris    %r3, %r3, 1                  /* ILE */
    mtmsr   %r3
    isync
    LI32(%r3, SHIM_BASE)
    mtsrr0  %r3
    li      %r3, 0x1031                  /* ME | IR | DR | LE */
    mtsrr1  %r3
    mr      %r3, %r20
    LI32(%r4, 0x4C453031)                /* 'LE01': entered by stage0 */
    rfi

/* The client interface: one argument array, as the binding defines it. */
    .globl stage0_cif_call
stage0_cif_call:
    mflr    %r0
    stwu    %r1, -32(%r1)
    stw     %r0, 36(%r1)
    lis     %r4, stage0_cif@ha
    lwz     %r4, stage0_cif@l(%r4)
    mtctr   %r4
    bctrl
    lwz     %r0, 36(%r1)
    addi    %r1, %r1, 32
    mtlr    %r0
    blr

/* The shim, a little-endian flat image linked at SHIM_BASE (shim.ld). */
    .section .rodata.payload, "a"
    .balign 8
    .globl stage0_payload, stage0_payload_end
stage0_payload:
    .incbin PAYLOAD
    .balign 8
stage0_payload_end:
