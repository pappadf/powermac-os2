/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-legacy.c -- the PC devices the Microkernel still talks to, on a Mac.
 *
 * Every PC port the kernel touches goes through six functions (inb, outb, inw,
 * outw, inl, outl), which turn a port into an address in PReP's spread I/O map
 * and reach it through the kernel's fixed window (BAT2, $F0800000 ->
 * $80000000).  On a Network Server that window is Bandit's PCI memory, where
 * an access nobody claims ends in a machine check.  So the six become calls
 * here:
 *
 *   $70/$71   an MC146818 clock: the time is the 604's time base since boot
 *             (11 MHz on the Network Server, the NT HAL's ANS_TIMEBASE_HZ)
 *             added to a fixed date, in BCD and 24-hour form, with the
 *             update-in-progress flag up in the last 244 us of each second,
 *             so code that waits for a second to turn over still does.  CMOS
 *             RAM ($0E-$3F) is kept, and starts zero.
 *   $74-$77   PReP NVRAM: reads zero, writes are dropped (as the loader's,
 *             loader-svc.h).
 *   $3B0-$3DF the VGA ports, passed to Bandit 1's PCI I/O space (BAT1)
 *   the rest  the 8259s (patch 80 replaced them), the 8237 DMA controller,
 *             the 8254 timer, the PMC, configuration mechanism #1 (patch 70):
 *             reads all-ones, writes dropped.
 *
 * Also here: IBM's L2-cache register (port $81C), which enable_l2_cache and
 * _start reach directly through the window (the Network Server's firmware
 * looks after its own cache), picinit's block for planars 1, 8 and 9 (PCI
 * interrupt routing in an IBM chipset's configuration space), and the PReP
 * DMA offset in dmabegin.
 */
/* cflags: -Os */
#include "kernel-cave.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define CAVE __attribute__((section(".patch.0xF016E000"), noinline, used))       /* CAVE_LEGACY */
#define CAVE_DATA __attribute__((section(".patch.0xF016E600"), used))            /* CAVE_LEGACY_DATA */
#define AT(sym) __attribute__((section(".patch." #sym), used))

#define TB_HZ 11000000u
#define BANDIT1_IO NS_IO(0xF2000000u)

CAVE_DATA static u8 rtc_index;
CAVE_DATA static u8 cmos[64];

static u32 bcd(u32 v)
{
    return (v / 10) << 4 | (v % 10);
}

/* Seconds since boot, and the time base's position within the second. */
CAVE static u32 tb_seconds(u32 *frac)
{
    u32 hi, lo, hi2;
    do {
        __asm__ volatile("mftbu %0" : "=r"(hi));
        __asm__ volatile("mftb %0" : "=r"(lo));
        __asm__ volatile("mftbu %0" : "=r"(hi2));
    } while (hi != hi2);
    /* (hi:lo) / TB_HZ by long division, without a 64-bit divide routine. */
    u32 q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        u32 bit = i >= 32 ? (hi >> (i - 32)) & 1 : (lo >> i) & 1;
        r = r << 1 | bit;
        q <<= 1;
        if (r >= TB_HZ) {
            r -= TB_HZ;
            q |= 1;
        }
    }
    *frac = r;
    return q;
}

CAVE static u32 rtc_read_reg(u32 reg)
{
    u32 frac, s = tb_seconds(&frac);
    /* 1 December 1995, 00:00:00, a Friday: the CD's own month. */
    u32 sec = s % 60, min = (s / 60) % 60, hour = (s / 3600) % 24, day = 1 + (s / 86400) % 28;
    switch (reg) {
    case 0x00: return bcd(sec);
    case 0x02: return bcd(min);
    case 0x04: return bcd(hour);
    case 0x06: return 1 + (5 + s / 86400) % 7;
    case 0x07: return bcd(day);
    case 0x08: return 0x12;
    case 0x09: return 0x95;
    case 0x0A: return 0x26 | (frac >= TB_HZ - 2700 ? 0x80 : 0);
    case 0x0B: return 0x02;   /* 24-hour, BCD, no interrupts */
    case 0x0C: return 0x00;
    case 0x0D: return 0x80;   /* the battery is good */
    default:
        return reg < 64 ? cmos[reg] : 0xFF;
    }
}

CAVE u32 k_port_in(u32 port, u32 size)
{
    port &= 0xFFFF;
    if (port == 0x71)
        return rtc_read_reg(rtc_index);
    if (port == 0x70)
        return rtc_index;
    if (port >= 0x74 && port <= 0x77)
        return 0;
    if (port - 0x3B0 < 0x30 && port - 0x3BC > 3) {
        u32 a = BANDIT1_IO + port, v;
        v = size == 1 ? *(volatile u8 *)a : size == 2 ? *(volatile u16 *)a : *(volatile u32 *)a;
        __asm__ volatile("eieio" ::: "memory");
        return v;
    }
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

CAVE void k_port_out(u32 port, u32 size, u32 v)
{
    port &= 0xFFFF;
    if (port == 0x70)
        rtc_index = v & 0x7F;
    else if (port == 0x71) {
        if (rtc_index >= 0x0E && rtc_index < 64)
            cmos[rtc_index] = (u8)v;
    } else if (port - 0x3B0 < 0x30 && port - 0x3BC > 3) {
        u32 a = BANDIT1_IO + port;
        if (size == 1)
            *(volatile u8 *)a = (u8)v;
        else if (size == 2)
            *(volatile u16 *)a = (u16)v;
        else
            *(volatile u32 *)a = v;
        __asm__ volatile("eieio" ::: "memory");
    }
}

/* The six entry points: tail calls, in the room the originals had. */
__asm__(
    "  .section .patch.inb, \"ax\"\n  li 4, 1\n  b k_port_in\n"
    "  .section .patch.inw, \"ax\"\n  li 4, 2\n  b k_port_in\n"
    "  .section .patch.inl, \"ax\"\n  li 4, 4\n  b k_port_in\n"
    "  .section .patch.outb, \"ax\"\n  mr 5, 4\n  li 4, 1\n  b k_port_out\n"
    "  .section .patch.outw, \"ax\"\n  mr 5, 4\n  li 4, 2\n  b k_port_out\n"
    "  .section .patch.outl, \"ax\"\n  mr 5, 4\n  li 4, 4\n  b k_port_out\n");

/* IBM's L2 cache register, and the rest. */
AT(enable_l2_cache) void k_enable_l2_cache(void) {}

__asm__(
    /* _start: the load and store of the L2 register ($F084001C) go. */
    "  .section .patch._start+0x298, \"ax\"\n  nop\n"
    "  .section .patch._start+0x2a8, \"ax\"\n  nop\n"
    /* dmabegin: a PCI bus master's address is the physical one. */
    "  .section .patch.dmabegin+0x540, \"ax\"\n  mr 12, 12\n"
    "  .section .patch.dmabegin+0x5b4, \"ax\"\n  mr 12, 12\n");
