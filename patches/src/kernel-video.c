/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-video.c -- the on-board display: OS/2's drawing, put on Control.
 *
 * Without a display card OS/2 draws on the board's Control behind the Chaos
 * bridge (shim/control.c sets its mode).  Chaos passes the CPU's byte lanes
 * straight through, so from the CPU's little-endian mode each aligned 8
 * bytes of an image would reach the screen mirrored.  OS/2 draws instead
 * into a shadow frame buffer in RAM (the stand-in display adapter's memory,
 * kernel-trap.c and kernel-iomap.S), and on each clock tick this copies a
 * band of it to VRAM, 8 bytes at a time with the bytes put right: two words
 * loaded, each stored byte-reversed in the other's place.  Only what changed
 * goes out: a copy of what was last sent, beside the shadow, says what did.
 *
 * The kernel maps neither the shadow (above the 8 MB its BAT0 covers) nor
 * Control's VRAM, and data BAT 3 is unused: it maps each in turn, a scan
 * line at a time, cache-inhibited, at $FE000000 (above everything the kernel
 * maps; supervisor only).
 *
 *   shadow   SHADOW_PA ($3F00000), 1 MB, hidden from OS/2 by the residual data
 *            +0        the image OS/2 draws, pitch 640
 *            +$80000   what was last sent to VRAM
 *            +$FFFC0   the screen, as the shim set it (shim/control.c)
 *
 * In trace_setup's body, after kernel-trap.c's routines; the clock calls it
 * (kernel-ints.c's gc_tick_hook, VIDEO_TICK).
 */
/* cflags: -Os -fno-unroll-loops */
#include "kernel-cave.h"

typedef unsigned int u32;

#define SHADOW_PA   0x03F00000u
#define SENT        0x00080000u
#define SCREEN      0x000FFFC0u    /* magic, vram, pitch, width, height, depth, ~magic */
#define CTL_MAGIC   0x314C5443u
#define WIN         0xFE000000u    /* data BAT 3's window */
#define BAND        24             /* scan lines a tick: the screen in 20 ticks */

#define AT_TICK __attribute__((section(".patch.trace_setup+0x1f4"), noinline, used))   /* VIDEO_TICK */
__attribute__((section(".patch.0xF016E65C"), used)) static u32 next_line;

/* Data BAT 3 onto 1 MB at pa, cache-inhibited and guarded, supervisor only. */
static inline void dbat3(u32 pa)
{
    __asm__ volatile("isync; mtspr 542,%0; mtspr 543,%1; mtspr 542,%2; isync"
                     ::"r"(0), "r"(pa | 0x2A), "r"(WIN | 0x1E) : "memory");
}

AT_TICK void video_tick(void)
{
    u32 buf[32];                    /* 128 bytes of a line, between the two mappings */
    dbat3(SHADOW_PA);
    const volatile u32 *scr = (const volatile u32 *)(WIN + SCREEN);
    if (scr[0] != CTL_MAGIC || scr[6] != ~CTL_MAGIC)
        return;                     /* a display card: this is OS/2's memory */
    u32 vram = scr[1], pitch = scr[2], height = scr[4];
    u32 y = next_line;
    for (u32 n = 0; n < BAND; n++, y++) {
        if (y >= height)
            y = 0;
        for (u32 c = 0; c < 640; c += 128) {
            dbat3(SHADOW_PA);
            volatile u32 *img = (volatile u32 *)(WIN + y * 640 + c), *sent = (volatile u32 *)(WIN + SENT + y * 640 + c);
            u32 dirty = 0;
            for (u32 i = 0; i < 32; i += 2) {
                u32 a = img[i], b = img[i + 1];
                buf[i] = a;
                buf[i + 1] = b;
                if (a != sent[i] || b != sent[i + 1]) {
                    sent[i] = a;
                    sent[i + 1] = b;
                    dirty |= 1u << i;
                }
            }
            if (!dirty)
                continue;
            dbat3(vram & 0xFFF00000u);
            u32 v = WIN + (vram & 0xFFFFFu) + y * pitch + c;
            for (u32 i = 0; i < 32; i += 2)
                if (dirty & 1u << i)
                    __asm__ volatile("stwbrx %0,0,%2; stwbrx %1,0,%3"
                                     ::"r"(buf[i + 1]), "r"(buf[i]), "r"(v + 4 * i), "r"(v + 4 * i + 4) : "memory");
        }
    }
    next_line = y;
}
