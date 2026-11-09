/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-adb.c -- the PC keyboard controller OS/2 drives, on the Network
 * Server's ADB keyboard and mouse.
 *
 * OS/2's console driver (dd_cons: kbd_intr, the PS/2 mouse code) talks to an
 * 8042 at ports $60 (data) and $64 (status, commands), and takes IRQ 1 for
 * the keyboard and 12 for the mouse.  The Network Server has none of that:
 * its keyboard and mouse are ADB devices behind Cuda, a microcontroller on
 * Grand Central's 6522 VIA (Apple, Cuda Firmware ERS; Network Server Hardware
 * Developer Notes).  So the configuration trap (kernel-trap.c) hands ports
 * $60 and $64 to kbd_port() here, which keeps an 8042's state:
 *
 *   - Cuda auto-polls the ADB devices and presents each report as a packet on
 *     the VIA's shift register (TREQ, TIP and ByteAck on port B).  A status
 *     read collects any packet waiting.  A keyboard report (ADB address 2)
 *     becomes scan code set 1 bytes; a mouse report (address 3) a three-byte
 *     PS/2 packet.
 *   - Commands written to $60 and $64 are answered as an 8042 and a PS/2
 *     keyboard and mouse would: $FA for each byte, the self-test results, the
 *     IDs, the command byte.
 *   - IRQ 1 and IRQ 12 are both Grand Central's VIA source (kernel-ints.c),
 *     so a Cuda packet interrupts.  Bytes made here (answers, a key's second
 *     byte, the mouse's packets) interrupt through the same source: while
 *     the next byte's interrupt is on in the command byte, kbd_line keeps
 *     the VIA's timer 2 (which nothing else uses) running out, re-armed after
 *     each access, so each byte brings a fresh edge.  The clock cannot
 *     deliver them itself: while it runs, the kernel's mask holds every IRQ.
 *   - A real 8042 raises IRQ 1 only for a keyboard byte and IRQ 12 only for
 *     a mouse byte, and OS/2's handlers rely on it: kbd_intr reads $60
 *     whatever the status says, and the mouse's handler only ever runs on
 *     IRQ 12, and a driver polling for an answer with that answer's
 *     interrupt off must not lose it to an interrupt handler.  So when the
 *     VIA's source is taken, kbd_irq names the IRQ the next byte is for; in
 *     that interrupt a reader sees the next byte only if it is for that IRQ
 *     and its interrupt is on.  Any other reader (a thread, polling) sees
 *     whatever comes next.
 *
 * Written from Apple's description of the protocol; the NT HAL for this
 * machine (powermac-nt-hal, src/cuda.c) drives the same handshake.
 */
/* cflags: -Os */
#include "kernel-cave.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define VIA(n)     (GC_BASE + 0x16000u + (n) * 0x200u)
#define VIA_B      VIA(0)
#define VIA_DIRB   VIA(2)
#define VIA_T2CL   VIA(8)
#define VIA_T2CH   VIA(9)
#define VIA_SR     VIA(10)
#define VIA_ACR    VIA(11)
#define VIA_IFR    VIA(13)
#define VIA_IER    VIA(14)
#define IF_SR      0x04
#define IF_T2      0x20
#define TREQ       0x08      /* port B, in, active low: Cuda has a packet */
#define BYTEACK    0x10      /* port B, out */
#define TIP        0x20      /* port B, out, active low: transfer in progress */
#define ACR_SR_IN  0x0C
#define ACR_SR_OUT 0x1C

/* The kernel debugger's db_get_spregs and db_set_spregs, which nothing calls
 * (the kernel has no debugger stub on this boot), hold the code. */
#define CUDA       __attribute__((section(".patch.db_get_spregs"), noinline, used))
#define KBD        __attribute__((section(".patch.db_set_spregs+0xc"), noinline, used))
/* And IBM's mot_enable_l2_cache, which kernel-trap.c reduces to a blr. */
#define MOT        __attribute__((section(".patch.mot_enable_l2_cache+0x4"), noinline, used))
#define DATA       __attribute__((section(".patch.0xF016E660"), used))   /* CAVE_LEGACY_DATA */

/* Set by interrupt() around taking a vector and running its handlers. */
#define IN_INTR    (*(volatile u32 *)0xF0173DE8)

/* The 8042's state: a queue of bytes for $60 (bit 8 marks the mouse's), the
 * command byte, and what the next byte written to $60 is for. */
DATA static struct {
    u16 q[32];
    u8 head, tail, cmd, next;   /* next: 0 the keyboard, $60 the command byte, $D4 the mouse */
    u8 up;                      /* Cuda set up and auto-polling */
    u8 aux;                     /* the interrupt being taken is IRQ 12 */
    u8 pad[2];
} k;

/* ADB keyboard code -> scan code set 1 (bit 7: preceded by $E0; 0: none). */
__attribute__((section(".patch.0xF016E6A8"), used)) static const u8 adb_set1[128] = {
    0x1E, 0x1F, 0x20, 0x21, 0x23, 0x22, 0x2C, 0x2D, 0x2E, 0x2F, 0x56, 0x30, 0x10, 0x11, 0x12, 0x13,
    0x15, 0x14, 0x02, 0x03, 0x04, 0x05, 0x07, 0x06, 0x0D, 0x0A, 0x08, 0x0C, 0x09, 0x0B, 0x1B, 0x18,
    0x16, 0x1A, 0x17, 0x19, 0x1C, 0x26, 0x24, 0x28, 0x25, 0x27, 0x2B, 0x33, 0x35, 0x31, 0x32, 0x34,
    0x0F, 0x39, 0x29, 0x0E, 0x9C, 0x01, 0x1D, 0x38, 0x2A, 0x3A, 0x38, 0xCB, 0xCD, 0xD0, 0xC8, 0x00,
    0x00, 0x53, 0x00, 0x37, 0x00, 0x4E, 0x00, 0x45, 0x00, 0x00, 0x00, 0xB5, 0x9C, 0x00, 0x4A, 0x00,
    0x00, 0x0D, 0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x00, 0x48, 0x49, 0x00, 0x00, 0x00,
    0x3F, 0x40, 0x41, 0x3D, 0x42, 0x43, 0x00, 0x57, 0x00, 0xB7, 0x00, 0x46, 0x00, 0x44, 0x00, 0x58,
    0x00, 0x00, 0xD2, 0xC7, 0xC9, 0xD3, 0x3E, 0xCF, 0x3C, 0xD1, 0x3B, 0x36, 0xB8, 0x9D, 0x00, 0x00,
};

static inline u8 rd(u32 a)
{
    u8 v = *(volatile u8 *)a;
    __asm__ volatile("eieio" ::: "memory");
    return v;
}
static inline void wr(u32 a, u8 v)
{
    *(volatile u8 *)a = v;
    __asm__ volatile("eieio" ::: "memory");
}

CUDA static void put(u32 v)
{
    u8 t = (k.tail + 1) & 31;
    if (t != k.head) {
        k.q[k.tail] = (u16)v;
        k.tail = t;
    }
}

/* Free places in the queue: a mouse packet goes in whole or not at all. */
static inline u32 room(void)
{
    return (u32)(k.head - k.tail - 1) & 31;
}

/* The next byte would interrupt: there is one, and the command byte has its
 * interrupt on (bit 0 the keyboard's, bit 1 the mouse's). */
static inline int raised(void)
{
    return k.head != k.tail && (k.cmd & (k.q[k.head] & 0x100 ? 2 : 1));
}

/* Whether the reader at hand may have the next byte (see the top). */
static inline int visible(void)
{
    if (k.head == k.tail)
        return 0;
    if (!IN_INTR)
        return 1;
    return raised() && !(k.q[k.head] & 0x100) == !k.aux;
}

/* Wait for the shift register to latch a byte (Cuda clocks every transfer). */
CUDA int cuda_wait(void)
{
    for (u32 n = 0; n < 200000; n++)
        if (rd(VIA_IFR) & IF_SR)
            return 1;
    return 0;
}

/* Read the packet Cuda is presenting into p (at most 8 bytes): its length. */
CUDA u32 cuda_read(u8 *p)
{
    u32 n = 0;
    if (!cuda_wait())
        return 0;
    rd(VIA_SR);                                         /* the attention byte */
    wr(VIA_B, (rd(VIA_B) | BYTEACK) & ~TIP);
    while (n < 8 && cuda_wait()) {
        p[n++] = rd(VIA_SR);
        if (rd(VIA_B) & TREQ)                           /* that was the last */
            break;
        wr(VIA_B, rd(VIA_B) ^ BYTEACK);
    }
    wr(VIA_B, rd(VIA_B) | TIP | BYTEACK);
    if (cuda_wait())
        rd(VIA_SR);                                     /* the idle acknowledge */
    return n;
}

/* Set Cuda up and start auto-polling: a pseudo-command, [1][1][1]. */
CUDA void cuda_start(void)
{
    u8 p[8];
    wr(VIA_DIRB, (rd(VIA_DIRB) & ~TREQ) | BYTEACK | TIP);
    wr(VIA_B, rd(VIA_B) | TIP | BYTEACK);
    wr(VIA_IER, 0x7F);                                  /* every VIA interrupt off */
    wr(VIA_ACR, (rd(VIA_ACR) & ~0x1C) | ACR_SR_OUT);
    wr(VIA_SR, 0x01);
    wr(VIA_B, (rd(VIA_B) | BYTEACK) & ~TIP);
    for (u32 i = 0; i < 2 && cuda_wait(); i++) {
        wr(VIA_SR, 0x01);
        wr(VIA_B, rd(VIA_B) ^ BYTEACK);
    }
    cuda_wait();
    wr(VIA_ACR, (rd(VIA_ACR) & ~0x1C) | ACR_SR_IN);
    rd(VIA_SR);
    wr(VIA_B, rd(VIA_B) | TIP | BYTEACK);
    cuda_read(p);                                       /* its reply */
    wr(VIA_IER, 0x80 | IF_SR);                          /* the shift register's interrupt on */
    k.up = 1;
}

/* Collect a waiting ADB report into the queue. */
KBD void kbd_poll(void)
{
    u8 p[8];
    if (!k.up)
        cuda_start();
    if (rd(VIA_B) & TREQ)
        return;
    u32 n = cuda_read(p);
    if (n < 4 || p[0] != 0)                             /* not an ADB packet with data */
        return;
    if ((p[2] >> 4) == 2) {                             /* the keyboard: up to two keys */
        for (u32 i = 3; i < n && i < 5; i++) {
            u8 c = p[i], s = adb_set1[c & 0x7F];
            if (c == 0xFF || !s)
                continue;
            if (s & 0x80)
                put(0xE0);
            put((s & 0x7F) | (c & 0x80));
        }
    } else if ((p[2] >> 4) == 3 && n >= 5 && (k.cmd & 2) && room() >= 3) {
        /* The mouse, while its interrupt is on (a packet nobody takes would
         * hold up the keys behind it): one PS/2 packet. */
        int dy = (int)(signed char)(p[3] << 1) >> 1, dx = (int)(signed char)(p[4] << 1) >> 1;
        put(0x108 | !(p[3] & 0x80) | (dx < 0) << 4 | (-dy < 0) << 5);
        put(0x100 | (dx & 0xFF));
        put(0x100 | (-dy & 0xFF));
    }
}

/* The next byte would interrupt: timer 2 runs out shortly (it
 * starts, and its flag clears, as its high byte is written), and its
 * interrupt is on.  Otherwise it is off. */
MOT void kbd_line(void)
{
    if (raised()) {
        wr(VIA_T2CL, 0x20);
        wr(VIA_T2CH, 0);
        wr(VIA_IER, 0x80 | IF_T2);
    } else {
        wr(VIA_IER, IF_T2);
        rd(VIA_T2CL);
    }
}

/* The VIA's source taken (kernel-ints.c): the IRQ the next byte is for. */
MOT int kbd_irq(void)
{
    k.aux = raised() && (k.q[k.head] & 0x100);
    return k.aux ? 12 : 1;
}

/* An access to port $60 or $64. */
KBD static u32 port_access(u32 port, int store, u32 v)
{
    if (!store) {
        if (port == 0x64) {
            if (k.head == k.tail)
                kbd_poll();
            if (!visible())
                return 0x14;
            return 0x15 | (k.q[k.head] & 0x100) >> 3;
        }
        if (!visible())
            return 0;
        v = k.q[k.head] & 0xFF;
        k.head = (k.head + 1) & 31;
        return v;
    }
    v &= 0xFF;
    if (port == 0x64) {                                 /* a controller command */
        k.next = 0;
        if (v == 0x20)
            put(k.cmd);
        else if (v == 0x60 || v == 0xD4)
            k.next = (u8)v;
        else if (v == 0xAA)
            put(0x55);
        else if (v == 0xAB || v == 0xA9)
            put(0x00);
        return 0;
    }
    if (k.next == 0x60) {                               /* the command byte */
        k.cmd = (u8)v;
        k.next = 0;
        return 0;
    }
    u32 aux = k.next == 0xD4 ? 0x100 : 0;
    k.next = 0;
    put(aux | 0xFA);                                    /* the device acknowledges */
    if (v == 0xFF) {                                    /* reset: passed */
        put(aux | 0xAA);
        if (aux)
            put(0x100);
    } else if (v == 0xF2) {                             /* identify */
        put(aux);
        if (!aux) {
            put(0xAB);
            put(0x41);
        }
    } else if (v == 0xE9 && aux) {                      /* the mouse's status: */
        put(0x120);                                     /* stream mode, reporting, no button */
        put(0x102);                                     /* 4 counts/mm */
        put(0x164);                                     /* 100 reports/s */
    }
    return 0;
}

KBD u32 kbd_port(u32 port, int store, u32 v)
{
    v = port_access(port, store, v);
    kbd_line();
    return v;
}

/* From the clock: bytes still waiting get another edge. */
MOT void kbd_tick(void)
{
    if (k.head != k.tail)
        kbd_line();
}

/* The entry points the other patches call, at fixed addresses: kernel-trap.c's
 * port path (kbd_port, $F01031A0), kernel-ints.c's clock (kbd_tick,
 * $F01031A4) and its vector (kbd_irq, $F01031A8). */
__asm__(
    "  .section .patch.db_set_spregs, \"ax\"\n"
    "  b kbd_port\n"
    "  b kbd_tick\n"
    "  b kbd_irq\n");
