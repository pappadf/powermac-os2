/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-cave.h -- what the patches put in MK_TEST.PPC's text cave, and where.
 *
 * Segment 0 of the Microkernel ends at $F016DA7D (.rodata); .got starts at
 * $F0170000.  The 9.6 KB between is zero padding in the file at the same
 * offsets, so `grow-segment` can take it into the loaded, executable image
 * without moving anything.  The loader copies it (checked with markers).
 *
 * Not all of it is free at run time.  The kernel's first exception frames and
 * their stack sit just below the first page past segment 0's end:
 * $F016E000 originally, frames at -$80 and down.  Grown to end at $F0170000
 * (all the padding), that page is the GOT's, and the first exception frame
 * overwrote GOT[0], the `blrl` every PIC routine calls.  So the segment is
 * grown to $F016F000 (size $6ED00), and cave code stays below CAVE_END.  That
 * leaves the stack 2 KB, more than the 1.4 KB it had before reaching .rodata.
 *
 * Each patch owns a fixed slice; objpatch's hash check of the zeros it
 * replaces catches two patches claiming the same bytes.
 */
#define CAVE_BAT1   0xF016DA80   /* 0x080: BAT1 for the Network Server's I/O (kernel-bat1.S) */
#define CAVE_PCI    0xF016DB00   /* 0x300: PCI configuration through Bandit (kernel-pci.c); kernel-trap.c's rw() at +0x240 */
#define CAVE_LEGACY 0xF016E000   /* 0x5C0: the PC ports the kernel still uses (kernel-legacy.c) */
#define CAVE_IOMAP  0xF016E400   /* 0x0A0: PReP physical addresses -> the Mac's (kernel-iomap.S) */
#define CAVE_TRAP   0xF016E4A0   /* 0x120: the trap window's fault entries (kernel-trap.c) */
#define CAVE_LEGACY_DATA 0xF016E600 /* 0x200: its state (the clock index, CMOS RAM) */
/* kernel-adb.c's entry points (in db_set_spregs, which nothing calls). */
#define KBD_PORT    0xF01031A0u  /* u32 kbd_port(u32 port, int store, u32 v): ports $60/$64 */
#define KBD_TICK    0xF01031A4u  /* void kbd_tick(void): from the clock */
#define KBD_IRQ     0xF01031A8u  /* int kbd_irq(void): the IRQ (1 or 12) the VIA's source is taken as */
/* mot_enable_l2_cache, a blr (kernel-trap.c): its body holds kernel-adb.c's
 * kbd_line, kbd_irq and kbd_tick from +$4, and kernel-ints.c's gc_vector_kbd
 * from +$150. */
#define CAVE_END    0xF016E800   /* the rest, to $F016F000, is the boot stack */
#define CAVE_INTS   0xF016DE00   /* 0x800: interrupts through Grand Central (kernel-ints.c; its tables at +0x7C0) */

/* The Network Server's I/O as the kernel sees it through BAT1: virtual
 * $FC000000-$FDFFFFFF is physical $F2000000-$F3FFFFFF, cache-inhibited and
 * guarded.  That is Bandit 1 (its I/O window and configuration ports) and
 * Grand Central.  The kernel's own map stops at $FC000000 (kmem_init). */
#define NS_IO_VIRT  0xFC000000u
#define NS_IO_PHYS  0xF2000000u
#define NS_IO(phys) (NS_IO_VIRT + ((phys) - NS_IO_PHYS))

#define BANDIT1_CFG_ADDR NS_IO(0xF2800000u)
#define BANDIT1_CFG_DATA NS_IO(0xF2C00000u)
#define GC_BASE          NS_IO(0xF3000000u)
