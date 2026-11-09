/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-ints.c -- the Microkernel's interrupt controller: Grand Central.
 *
 * The kernel was written for two cascaded 8259s.  It keeps an 8259-style
 * mask (16 IRQs, a set bit masks), programs it with outb to ports $21/$A1,
 * reads each interrupt's vector from the PReP interrupt-acknowledge register
 * ($BFFFFFF0), and sends each 8259 an end-of-interrupt after the handlers.
 * No Mac has any of that.
 *
 * Grand Central has 32 sources and four registers (Apple, Network Server
 * Hardware Developer Notes; the NT HAL's ints.c):
 *   Events (+$20)  edge-latched source changes
 *   Mask   (+$24)  a set bit enables a source
 *   Clear  (+$28)  write 1 to clear an event.  Bit 31 selects the clear mode.
 *                  Mode 0 (MkLinux, the NT HAL): the CPU's line is
 *                  (Events | Levels) & Mask.  Mode 1 (the Mac OS NanoKernel):
 *                  the line is a latch, set by any change of an enabled
 *                  source and dropped by a Clear write with bit 31 set.
 *   Levels (+$2C)  the sources' live levels
 *
 * Mode 1 is used here, because OS/2 sets most of its IRQs edge-triggered, as
 * an 8259 leaves them: a user-level driver's interrupt (ULDD) only wakes its
 * thread, which services the device later.  In mode 0 the PCI chip's level,
 * still high, re-interrupts at once and that thread never runs.
 *
 * OS/2's own IRQ numbers stay (the hardware database assigns them: the
 * Sandalfoot's SCSI line is 13).  irq_gc maps each to a Grand Central source.
 * The mask follows the kernel's 8259-style one, through that map.  An
 * interrupt is the highest-priority pending source in 8259 order: an edge
 * IRQ's latched event (cleared as it is taken), or a level IRQ's live level
 * (vector_set_level; it drops when its device is serviced).  So the
 * end-of-interrupt writes go.
 *
 * Grand Central is reached through BAT1 (kernel-bat1.S, kernel-cave.h).
 */
#include "kernel-cave.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define CAVE __attribute__((section(".patch.0xF016DE00"), noinline, used))  /* CAVE_INTS */
#define CAVE_DATA __attribute__((section(".patch.0xF016E5C0"), used))  /* CAVE_INTS + 0x7C0: its tables */
#define AT(sym) __attribute__((section(".patch." #sym), used))

#define GC_EVENTS (GC_BASE + 0x20)
#define GC_MASK   (GC_BASE + 0x24)
#define GC_CLEAR  (GC_BASE + 0x28)
#define GC_LEVELS (GC_BASE + 0x2C)
#define NONE      0xFF
#define ACK       0x80000000u   /* a Clear write with this bit: mode 1, the latch dropped */

/* The kernel's 8259-style mask (a set bit masks the IRQ), as form_pic_mask reads it. */
#define PIC_MASK  (*(volatile u32 *)0xF01732F0)

/* OS/2 IRQ -> Grand Central source (Network Server Hardware Developer Notes, the interrupt map). */
CAVE_DATA static const u8 irq_gc[16] = {
    NONE,
    18,   /* IRQ 1: the 8042's keyboard, emulated on ADB (kernel-adb.c): Cuda's VIA */
    NONE, NONE, NONE, NONE, NONE, NONE,
    NONE, NONE, NONE, NONE,
    18,   /* IRQ 12: its mouse, the same */
    22,   /* IRQ 13: the first 53C825A (FW SCSI 0), the Sandalfoot's SCSI line */
    NONE,
    NONE, /* IRQ 15: nothing.  The second 53C825A (source 26) is hidden from
           * OS/2, as the loader and the configuration trap hide it; routed
           * here, its line (left asserted) woke the second NCR instance,
           * which has no chip, on every clock tick. */
};

/* The Grand Central sources of the IRQs set level-triggered (vector_set_level). */
__attribute__((section(".patch.0xF016E5E0"), used)) static u32 gc_level;
/* Edge sources whose line stayed asserted past an interrupt: re-armed by
 * the clock (gc_tick), so a device that raises a second condition without
 * dropping its line in between is not lost (an 8259 would lose it too, but
 * PReP's PCI lines are level-sensitive behind it). */
__attribute__((section(".patch.0xF016E5E4"), used)) static u32 gc_rearm;

/* 8259 priority: master IRQ 0-1, the slave's 8-15 in place of 2, then 3-7. */
CAVE_DATA static const u8 irq_order[15] = {0, 1, 8, 9, 10, 11, 12, 13, 14, 15, 3, 4, 5, 6, 7};

static inline u32 rd(u32 a)
{
    u32 v = *(volatile u32 *)a;
    __asm__ volatile("eieio" ::: "memory");
    return v;
}
static inline void wr(u32 a, u32 v)
{
    *(volatile u32 *)a = v;
    __asm__ volatile("eieio" ::: "memory");
}

/* Enable exactly the sources of the IRQs an 8259-style mask leaves on. */
CAVE void gc_apply_mask(u32 pic_mask)
{
    u32 m = 0;
    for (u32 irq = 0; irq < 16; irq++)
        if (!(pic_mask & (1u << irq)) && irq_gc[irq] != NONE)
            m |= 1u << irq_gc[irq];
    wr(GC_MASK, m);
}

/* Everything off, every event cleared, clear mode 1. */
CAVE void gc_init(void)
{
    wr(GC_MASK, 0);
    wr(GC_CLEAR, ACK | 0x7FFFFFFF);
}

static inline u32 gc_pending(void)
{
    return (rd(GC_EVENTS) | (rd(GC_LEVELS) & (gc_level | gc_rearm))) & rd(GC_MASK);
}

/* The interrupt to take: OS/2's IRQ number; or -1 (nothing, or a source's
 * deassertion).  Its event is cleared and the latch dropped (every Clear
 * write keeps bit 31, which also keeps mode 1); whatever still waits is
 * latched again by re-enabling it, so the line comes straight back. */
CAVE int gc_vector(void)
{
    u32 pending = gc_pending(), clear = ACK;
    int irq = -1;
    for (u32 i = 0; i < sizeof irq_order; i++) {
        u32 src = irq_gc[irq_order[i]];
        if (src != NONE && (pending & (1u << src))) {
            irq = (int)irq_order[i];
            clear |= 1u << src;
            gc_rearm &= ~(1u << src);
            break;
        }
    }
    wr(GC_CLEAR, clear);
    u32 late = gc_pending();
    if (late) {
        u32 m = rd(GC_MASK);
        wr(GC_MASK, m & ~late);
        wr(GC_MASK, m);
    }
    return irq;
}

/* Each clock tick: re-arm the enabled edge sources still asserted, and latch
 * them by re-enabling them. */
__attribute__((section(".patch.db_get_spregs+0x310"), noinline, used))  /* past kernel-adb.c's Cuda code */
void gc_tick(void)
{
    ((void (*)(void))KBD_TICK)();
    u32 m = rd(GC_MASK), stuck = rd(GC_LEVELS) & m & ~gc_level;
    if (stuck) {
        gc_rearm |= stuck;
        wr(GC_MASK, m & ~stuck);
        wr(GC_MASK, m);
    }
}

/* interrupt()'s vector: gc_vector's, except that the VIA's source, which
 * carries both IRQ 1 and 12 and so is always found as IRQ 1, is taken as the
 * IRQ the emulated 8042's next byte is for (kernel-adb.c).  In
 * mot_enable_l2_cache's body, past kernel-adb.c's code (kernel-cave.h). */
__attribute__((section(".patch.mot_enable_l2_cache+0x150"), noinline, used))
int gc_vector_kbd(void)
{
    int irq = gc_vector();
    return irq == 1 ? ((int (*)(void))KBD_IRQ)() : irq;
}

/* The 8259 routines that remain whole functions. */
AT(form_pic_mask) void k_form_pic_mask(void) { gc_apply_mask(PIC_MASK); }
AT(pic_resume) void k_pic_resume(void) { gc_apply_mask(PIC_MASK); }
AT(pic_suspend) u32 k_pic_suspend(void) { wr(GC_MASK, 0); return 0; }
AT(vector_set_edge) void k_vector_set_edge(u32 irq)
{
    if (irq < 16 && irq_gc[irq] != NONE)
        gc_level &= ~(1u << irq_gc[irq]);
}
AT(vector_set_level) void k_vector_set_level(u32 irq)
{
    if (irq < 16 && irq_gc[irq] != NONE)
        gc_level |= 1u << irq_gc[irq];
}

/* And the instruction-level patches. */
__asm__(
    /* interrupt(): the vector from gc_vector_kbd, not from the acknowledge register;
     * none (-1) leaves through the normal exit. */
    "  .section .patch.interrupt+0x28, \"ax\"\n"
    "  bl gc_vector_kbd\n"
    "  mr. 7, 3\n"
    "  blt interrupt+0x70\n"
    /* picinit: Grand Central instead of the 8259s' initialisation words.  Its
     * mapping of the interrupt-acknowledge page stays: nothing reads it any
     * more, and the page it takes from the bootstrap allocator is one the
     * rest of early boot is laid out around (skipping it left vseg_release
     * running before its zone existed). */
    "  .section .patch.picinit+0x10c, \"ax\"\n"
    "  bl gc_init\n"
    "  b picinit+0x310\n"   /* and past the block for IBM planars 1, 8 and 9 */
    /* hardclock, in place of its `li 11, 1`: gc_tick first. */
    "  .section .patch.hardclock+0x28, \"ax\"\n"
    "  bl gc_tick_hook\n"
    "  .section .patch.0xF016DDBC, \"ax\"\n"   /* past kernel-trap.c's rw() */
    "gc_tick_hook:\n"
    "  mflr 0\n"
    "  stwu 1, -16(1)\n"
    "  stw 0, 20(1)\n"
    "  bl gc_tick\n"
    "  lwz 0, 20(1)\n"
    "  addi 1, 1, 16\n"
    "  mtlr 0\n"
    "  li 11, 1\n"
    "  blr\n"
    /* vectors_init: its mask (r31) through Grand Central. */
    "  .section .patch.vectors_init+0xa8, \"ax\"\n"
    "  nop\n"
    "  nop\n"
    "  mr 3, 31\n"
    "  bl gc_apply_mask\n"
    /* No end-of-interrupt: each dispatcher's two outb calls go. */
    "  .section .patch.root_io_ext_socket_null_dispatch+0x1c, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_null_dispatch+0x28, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_single_edge_dispatch+0x64, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_single_edge_dispatch+0x70, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_multiple_edge_dispatch+0x64, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_multiple_edge_dispatch+0x70, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_single_level_dispatch+0xb0, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_single_level_dispatch+0xbc, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_multiple_level_dispatch+0xcc, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_multiple_level_dispatch+0xd8, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_clear+0x14, \"ax\"\n  nop\n"
    "  .section .patch.root_io_ext_socket_clear+0x20, \"ax\"\n  nop\n");
