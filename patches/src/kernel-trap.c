/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-trap.c -- PReP's PC ports and direct-mapped configuration, emulated.
 *
 *
 * Two PReP address spaces cannot be mapped onto a Mac page by page:
 *   $80000000-$807FFFFF  ISA ports in the spread map: port p at
 *                        ((p & $FFE0) << 7) | (p & $1F), 32 ports a page
 *   $80800000-$80FFFFFF  direct-mapped PCI configuration: slot s at bit s
 * OS/2's drivers still reach them, through LIBDDPIO's ISA and configuration
 * classes (VPMISRV's PCI walk and VGA ports, the NVRAM, the SCSI driver's
 * configuration registers) and the kernel's fixed ISA window.  So every
 * mapping of them (kernel-iomap.S for tasks, BAT2 for the kernel) points
 * at $88000000 + the same offset: Bandit 1 memory nobody decodes, where an
 * access is a PCI master abort and the 604 takes a machine check.  mc_slih
 * hands its saved state here first.  The faulting load or store is decoded,
 * its PReP address found (the page table entry of its effective address
 * gives the trap page, the effective address the offset in it), and the
 * access done on the Network Server's own hardware:
 *
 *   ports $3B0-$3DF   VGA, on Bandit 1's PCI I/O space (the 54M30); not $3BC-$3BF,
 *                     the monochrome adapter's printer port, which nothing
 *                     there decodes (Bandit would machine-check).  Without
 *                     a 54M30, only the DAC: the on-board display's palette
 *   ports $CF8/$CFC   configuration mechanism #1, latched here: bus 0 device n
 *                     is direct-mapped slot n (the same IDSEL)
 *   direct config     slot 12 (the Sandalfoot's 53C810) is the 53C825A,
 *                     device 17 or a card's 13; slot 13 the 54M30, device 15.
 *                     A Power Macintosh without them gets stand-ins: a
 *                     53C810 for its MESH, Control for its on-board display
 *   $80FFF000         the stand-in 53C810's registers: MESH and DBDMA
 *   ports $74-$77     PReP NVRAM: reads zero (as kernel-legacy.c's and the
 *                     loader's), writes dropped
 *   ports $60/$64     the 8042, emulated on the ADB keyboard and mouse
 *                     (kernel-adb.c)
 *   everything else   no such device: reads all-ones, writes dropped
 *                     (the 16550s, IBM's planar registers)
 *
 * The 53C825A answers as a 53C810 (device $0001), as it does to the loader
 * (loader-svc.h): the hardware database binds the NCR driver to that ID.  The
 * 54M30 answers as the CL-GD5434 ($00A8, and $A8 in CR27, the die ID a Cirrus
 * driver reads), the only Cirrus part the database and cirrus.pmi know.  And
 * like a 53C810 it has no third BAR (the 53C825A's SCRIPTS RAM).
 *
 * Then the register is written back, the update form's base register too,
 * and SRR0 moves past the instruction.  The saved state: GPR n at +4n, SRR0
 * at +128, SRR1 at +132 (call_slih, ds_slih).
 */
#include "kernel-cave.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define TRAP_BASE  0x88000000u
#define BANDIT1_IO NS_IO(0xF2000000u)

/* Each piece where there is room: picinit's 8259 initialisation, which patch
 * 80 jumps over; after the `blr` each of IBM's L2-cache routines now starts
 * with (their callers -- machinit, pcreset, power management -- still call
 * them, and get nothing done); and a word of state in the cave
 * (kernel-cave.h). */
#define AT_DECODE  __attribute__((section(".patch.picinit+0x114"), noinline, used))
#define AT_IO      __attribute__((section(".patch.l2_cache_size+0x8"), noinline, used))
#define AT_CFG     __attribute__((section(".patch.flush_l2_cache+0x4"), noinline, used))
#define AT_DATA    __attribute__((section(".patch.0xF016E650"), used))

extern u32 *pte_lookup(u32 vsid, u32 page);

AT_DATA static u32 cf8;
__attribute__((section(".patch.0xF016E654"), used)) static u32 crtc;   /* the CRTC index last written */

static inline void eieio(void)
{
    __asm__ volatile("eieio" ::: "memory");
}

/* A device register of `size` bytes: stored, or loaded and returned.  In the
 * PCI slice's unused end (kernel-cave.h). */
__attribute__((section(".patch.0xF016DD40"), noinline, used))
static u32 rw(u32 p, u32 size, int store, u32 v)
{
    if (store) {
        if (size == 1) *(volatile u8 *)p = (u8)v;
        else if (size == 2) *(volatile u16 *)p = (u16)v;
        else *(volatile u32 *)p = v;
        v = 0;
    } else
        v = size == 1 ? *(volatile u8 *)p : size == 2 ? *(volatile u16 *)p : *(volatile u32 *)p;
    eieio();
    return v;
}

/* --- configuration ------------------------------------------------------------ */

/* Device 17 if the Network Server's first 53C825A is there (a Symbios vendor
 * ID), else 13 if a card is in the first PCI slot (slot_cfg, below), else 0:
 * no 53C825A at all.  In the cave's free space past this file's fault
 * entries (kernel-cave.h). */
__attribute__((section(".patch.0xF016E540"), noinline, used))
static u32 scsi_dev(void)
{
    for (u32 dev = 17; dev >= 13; dev -= 4) {
        *(volatile u32 *)BANDIT1_CFG_ADDR = 1u << dev;
        eieio();
        u32 id = *(volatile u32 *)BANDIT1_CFG_DATA;
        eieio();
        if ((id & 0xFFFF) == 0x1000)
            return dev;
    }
    return 0;
}

/* Stand-ins for the two PReP slots a Power Macintosh 7500/8500/9500 without
 * cards has no chip for.  Each is a configuration header in the cave's free
 * end (kernel-cave.h); a BAR write keeps only the address bits, so a size
 * probe reads the size back.
 *
 * Slot 12 without a 53C825A: a 53C810, so that OS/2's hardware database
 * binds the NCR driver as on any other machine, and the driver's MESH layer
 * (dd-scsi-mesh.c) drives the machine's own bus instead.  Its one BAR, 256
 * bytes of I/O at PCI $00FFF000 (PReP $80FFF000, the top of PReP's direct
 * configuration window, which no slot uses), is a trap-window page like the
 * rest of that window, and mesh_reg below turns it into Grand Central's MESH
 * and DBDMA channel 10.
 *
 * Slot 13 without a 54M30: the board's Control (its own ID), so that OS/2's
 * video server finds a display adapter (RDF.STZ, VIDEOPMI.LST, CONTROL.PMI).
 * Its memory BAR, 1 MB at Bandit $8E000000 (PCI $0E000000 and PReP
 * $CE000000 to OS/2), is the shadow frame buffer in RAM (kernel-iomap.S),
 * which kernel-video.c copies to the screen. */
__attribute__((section(".patch.0xF016E780"), used)) static u32 fake_810[16] = {
    0x00011000u,          /* Symbios 53C810 */
    0x02000007u,          /* I/O, memory, bus master; medium DEVSEL */
    0x01000002u,          /* SCSI controller, revision 2 */
    0, 0x00FFF001u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x0000010Du,          /* INTA, line 13 */
};
__attribute__((section(".patch.0xF016E7C0"), used)) static u32 fake_control[16] = {
    0x0003106Bu,          /* Apple Control */
    0x02000002u,          /* memory; medium DEVSEL */
    0x03000000u,          /* VGA-compatible display controller */
    0, 0x8E000000u,
};

/* A stand-in's configuration register: read, or (a BAR) written.  In
 * l2_cache_dpe's dead body. */
__attribute__((section(".patch.l2_cache_dpe+0x8"), noinline, used))
static u32 fake(u32 *img, u32 reg, u32 size, int store, u32 v)
{
    if (store) {
        if (reg == 0x10 && size == 4)
            img[4] = img[4] & 1 ? (v & ~0xFFu) | 1u : v & ~0xFFFFFu;   /* 256 bytes of I/O, 1 MB of memory */
        return 0;
    }
    u32 w = reg < 0x40 ? img[reg >> 2] >> 8 * (reg & 3) : 0;
    return size == 1 ? w & 0xFF : size == 2 ? w & 0xFFFF : w;
}

/* Is Bandit 1 device n empty? */
__attribute__((section(".patch.trace_setup+0x8"), noinline, used))
static u32 absent(u32 n)
{
    *(volatile u32 *)BANDIT1_CFG_ADDR = 1u << n;
    eieio();
    u32 id = *(volatile u32 *)BANDIT1_CFG_DATA;
    eieio();
    return id == 0xFFFFFFFFu;
}

/* slot_cfg's device: the device to address (17 becomes the 53C825A that is
 * there, scsi_dev), or a stand-in's header. */
__attribute__((section(".patch.trace_setup+0x8"), noinline, used))
static u32 stand_in(u32 dev)
{
    if (dev == 17 && !(dev = scsi_dev()))
        return (u32)fake_810;
    if (dev == 15 && absent(15))
        return (u32)fake_control;
    return dev;
}

/* The 53C825A as the 53C810 OS/2 binds its driver to, and the 54M30 as the
 * CL-GD5434, the one Cirrus part OS/2 knows (RDF.STZ's "A81013"): the device
 * ID in a read of register 0.  In l2_cache_present, which nothing calls. */
__attribute__((section(".patch.l2_cache_present"), noinline, used))
static u32 cfg_id(u32 dev, u32 reg, u32 size, u32 v)
{
    u32 id = dev == 15 ? 0x00A8 : 0x0001;
    if (size == 4)
        v = (v & 0xFFFF) | id << 16;
    else if (size == 2 && (reg & 2))
        v = id;
    return v;
}

/* The stand-in 53C810's registers (its trap-window page, $80FFF000): MESH's
 * sixteen byte registers where they are, on 16-byte centres, and in three of
 * the words between them DBDMA channel 10's control (+4), status (+8) and
 * command pointer (+$C).  The driver maps one page for its 256 bytes, and
 * MESH and the DMA channel are two pages of Grand Central. */
__attribute__((section(".patch.pic_resume+0x28"), noinline, used))  /* past kernel-ints.c's pic_resume */
static u32 mesh_reg(u32 o, u32 size, int store, u32 v)
{
    u32 k = o & 0xF;
    if (o > 0xFF || (k & 3) || (k && size != 4))
        return 0xFFFFFFFFu;
    return rw(k ? GC_BASE + 0x8A00 + (k == 4 ? 0 : k == 8 ? 4 : 0xC) : GC_BASE + 0x18000 + o, size, store, v);
}

/* A Bandit 1 configuration access; absent devices (and every bus but 0) read all-ones. */
AT_CFG static u32 cfg(u32 dev, u32 fn, u32 reg, u32 size, int store, u32 v)
{
    u32 ones = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    if (dev < 11 || dev > 31)
        return ones;
    if ((dev = stand_in(dev)) > 31)
        return fake((u32 *)dev, reg, size, store, v);
    /* A 53C810 has no SCRIPTS RAM: the 53C825A's BAR2 reads 0 and keeps its
     * address (the driver would take that RAM for its registers).  dev is
     * slot_cfg's: the 53C825A or the 54M30 (15). */
    if (dev != 15 && (reg & 0xFC) == 0x18)
        return 0;
    *(volatile u32 *)BANDIT1_CFG_ADDR = (1u << dev) | (fn << 8) | (reg & 0xFC);
    eieio();
    v = rw(BANDIT1_CFG_DATA + (reg & 3), size, store, v);
    if (store || (reg & 0xFC))
        return v & -(u32)!store;
    return cfg_id(dev, reg, size, v);
}

/* A PReP slot's configuration space (direct-mapped slot n, or mechanism #1's
 * device n -- the same IDSEL): slot 12 is the 53C825A, slot 13 the 54M30,
 * and nothing else answers.  The 53C825A is the Network Server's first
 * soldered chip, device 17, where there is one (a Symbios vendor ID there);
 * otherwise a 53C825A card in the first PCI slot, device 13 (a Power
 * Macintosh 7500/8500/9500's A1).  The 54M30 is device 15: soldered there on
 * the Network Server, a card in a Power Macintosh's C1.  The second 53C825A
 * stays hidden, as it does from the loader: a Sandalfoot has one SCSI
 * controller, and the driver on its IRQ (13: Grand Central source 22, or the
 * slot's 23) must find the first. */
__attribute__((section(".patch.enable_l2_cache+0x4"), noinline, used))  /* cfg's old room */
static u32 slot_cfg(u32 slot, u32 fn, u32 reg, u32 size, int store, u32 v)
{
    u32 dev = slot == 12 ? 17 : slot == 13 ? 15 : 0;   /* cfg moves 17 to a card's 13 */
    /* PCI memory: PReP's CPU address $C0000000 + m is PCI m; Bandit's
     * $80000000 + m is PCI $80000000 + m.  So a memory BAR, in OS/2's terms,
     * is the chip's less $80000000 (a size probe, all-ones, passes as is). */
    /* Any access that includes a BAR's top byte: VPMISRV writes the video
     * BAR's alone.  A whole BAR says what it is (bit 0); a piece of one is
     * taken as memory only on slot 13, the display adapter. */
    u32 hi = reg - 0x10 < 0x18 && (reg & 3) + size == 4 && (size == 4 || slot == 13)
                 ? 0x80u << (8 * size - 8) : 0;       /* the top bit of a BAR access, or 0 */
    if (store && !(v & hi) && (size < 4 || !(v & 1)))
        v += hi;
    u32 r = cfg(dev, fn, reg, size, store, v);
    if (!store && (r & ((hi << 1) - (hi >> 3))) == hi && (size < 4 || !(r & 1)))
        r -= hi;                                       /* top nibble 8: Bandit's window */
    return r;
}

/* --- PReP I/O ------------------------------------------------------------------- */

/* VGA's ports.  With the 54M30 (Bandit 1 device 15), Bandit 1's PCI I/O space,
 * where it decodes them, with CR27, the die's ID, as the 5434's ($A8): a
 * Cirrus driver checks it.  Without one, the board's Control has no VGA, and
 * only the palette means anything: the DAC's write index ($3C8) and data
 * ($3C9, red, green, blue, 6 bits each) become the RaDACal's colour table
 * (Grand Central + $1B000: its index at +0, the table at +$30), and the
 * input status ($3DA) alternates, for a driver that waits for a retrace.
 * Nothing else answers. */
#define RADACAL NS_IO(0xF301B000u)
__attribute__((section(".patch.0xF016E658"), used)) static unsigned char dac[4];   /* index, byte, $3DA */
__attribute__((section(".patch.trace_setup+0x8"), noinline, used))
static u32 vga(u32 port, u32 size, int store, u32 v)
{
    if (!absent(15)) {
        if ((port & ~0x20) == 0x3B4 && store)
            crtc = v & 0xFF;
        if ((port & ~0x20) == 0x3B5 && !store && size == 1 && crtc == 0x27)
            return 0xA8;
        return rw(BANDIT1_IO + port, size, store, v);
    }
    if (port == 0x3DA && !store)
        return dac[2] ^= 0x09;
    if (!store)
        return 0xFFFFFFFFu;
    if (port == 0x3C8) {
        dac[0] = (unsigned char)v;
        dac[1] = 0;
    } else if (port == 0x3C9) {
        if (!dac[1])
            rw(RADACAL, 1, 1, dac[0]);
        rw(RADACAL + 0x30, 1, 1, (v & 0x3F) << 2 | (v & 0x3F) >> 4);
        if (++dac[1] == 3) {
            dac[1] = 0;
            dac[0]++;
        }
    }
    return 0;
}

AT_IO static u32 prep_io(u32 a, u32 size, int store, u32 v)
{
    u32 off = a - 0x80000000u;
    u32 ones = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    if (off - 0x00FFF000u < 0x1000u)          /* the stand-in 53C810 (above) */
        return mesh_reg(off & 0xFFF, size, store, v) & ones;
    if (off >= 0x00800000u) {                 /* direct configuration */
        off -= 0x00800000u;
        u32 slot = 0;
        for (u32 b = 11; b <= 18; b++)
            if (off & (1u << b)) {
                slot = b;
                break;
            }
        return slot_cfg(slot, (off >> 8) & 7, off & 0xFF, size, store, v);
    }
    u32 port = ((off >> 7) & 0xFFE0) | (off & 0x1F);   /* the spread map */
    if (port - 0x3B0 < 0x30 && port - 0x3BC > 3)    /* VGA, not the MDA printer port $3BC */
        return vga(port, size, store, v) & ones;
    if ((port & ~4u) == 0x60)                  /* the 8042: ADB, through Cuda (kernel-adb.c) */
        return ((u32 (*)(u32, int, u32))KBD_PORT)(port, store, v);
    if ((port & ~3u) == 0x74)
        return 0;    /* PReP NVRAM: blank, as kernel-legacy.c's */
    if (port >= 0xCF8 && port <= 0xCFB && size == 4) {
        if (store)
            cf8 = v;
        return store ? 0 : cf8;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        if (!(cf8 & 0x80000000u) || (cf8 >> 16) & 0xFF)
            return store ? 0 : ones;
        return slot_cfg((cf8 >> 11) & 31, (cf8 >> 8) & 7, (cf8 & 0xFC) | (port & 3), size, store, v);
    }
    return store ? 0 : ones;
}

/* --- the faulting instruction ------------------------------------------------ */

#define AT_DO      __attribute__((section(".patch.disable_l2_cache+0x4"), noinline, used))

__attribute__((section(".patch.0xF016DFC0"), noinline, used))  /* past kernel-ints.c's code (kernel-cave.h) */
static u32 swap(u32 v, u32 size)
{
    return size == 2 ? (v & 0xFF) << 8 | (v >> 8 & 0xFF)
                     : (v & 0xFF) << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24;
}

/* Do access k (below) at effective address ea, if it is in the trap window. */
AT_DO int trap_do(u32 *st, u32 k, u32 rev, u32 ea, u32 rd, u32 ra)
{
    u32 size = k >= 8 ? 2 : (k >> 1) & 1 ? 1 : 4;
    u32 page;
    if (ea - 0xF0800000u < 0x00800000u)         /* the kernel's fixed ISA window, BAT2 */
        page = TRAP_BASE + ((ea - 0xF0800000u) & ~0xFFFu);
    else {
        u32 sr;
        __asm__("mfsrin %0, %1" : "=r"(sr) : "r"(ea));
        u32 *pte = pte_lookup(sr & 0x00FFFFFF, (ea >> 12) & 0xFFFF);
        if (!pte)
            return 0;
        page = pte[0] & ~0xFFFu;
    }
    if (page - TRAP_BASE >= 0x01000000u)
        return 0;
    u32 a = 0x80000000u + (page - TRAP_BASE) + (ea & 0xFFF);
    if ((k >> 2) & 1) {                         /* a store */
        u32 v = st[rd];
        v = size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v;
        prep_io(a, size, 1, rev ? swap(v, size) : v);
    } else {
        u32 v = prep_io(a, size, 0, 0);
        if (rev)
            v = swap(v, size);
        if ((k >> 1) == 5)                      /* lha, lhau, lhax, lhaux */
            v = (u32)(int)(short)v;
        st[rd] = v;
    }
    if (k & 1)                                  /* the update forms */
        st[ra] = ea;
    st[32] += 4;
    return 1;
}

/* 1 when the access was to the trap window, now done.  The effective address
 * comes from the instruction's registers, not the DAR: in little-endian mode
 * the DAR can hold the munged address (byte ^7, halfword ^6, word ^4, as the
 * some implementations report it), while a PReP bridge in little-endian mode undoes the
 * munge, so the bus -- and the port -- is the true effective address
 * (LIBDDPIO's NVRAM index port $74 is byte $14 of its page).  A DSI is
 * precise, so the registers still hold their values; after a machine check a
 * load whose rA is its rD has already overwritten it, which is why the
 * window faults by protection now and the machine check is only a fallback.
 *
 * The loads and stores, as k = D-form opcode - 32:
 *   0 lwz  1 lwzu  2 lbz  3 lbzu  4 stw  5 stwu  6 stb  7 stbu
 *   8 lhz  9 lhzu 10 lha 11 lhau 12 sth 13 sthu
 * The indexed forms are opcode 31 with xo = k << 5 | 23 (lwzx ... sthux), and
 * the byte-reversed ones xo = (k + 16) << 5 | 22 for k = 0, 4, 8, 12 (lwbrx,
 * stwbrx, lhbrx, sthbrx). */
AT_DECODE static int trap_insn(u32 *st)
{
    u32 insn = *(volatile u32 *)st[32];
    u32 op = insn >> 26, ra = (insn >> 16) & 31;
    u32 k, rev = 0, e = ra ? st[ra] : 0;
    if (op >= 32 && op <= 45) {
        k = op - 32;
        e += (u32)(int)(short)insn;
    } else if (op == 31) {
        u32 xo = (insn >> 1) & 0x3FF;
        k = xo >> 5;
        if ((xo & 0x1F) == 22 && k >= 16 && !(k & 3)) {
            k -= 16;
            rev = 1;
        } else if ((xo & 0x1F) != 23 || k > 13)
            return 0;
        e += st[(insn >> 11) & 31];
    } else
        return 0;
    return trap_do(st, k, rev, e, (insn >> 21) & 31, ra);
}

#define AT_TRAP __attribute__((section(".patch.0xF016E4A0"), noinline, used))

/* mc_slih's: a machine check (kernel accesses through a user segment, where
 * PP 00 still lets the supervisor through). */
AT_TRAP int trap_mc(u32 *st)
{
    return trap_insn(st);
}

/* ds_slih's: a protection DSI.  Every trap-window page is mapped PP 00
 * (pmap_enter, below) and so is BAT2, so a driver's access, or the kernel's
 * through its fixed window, faults before it reaches the bus, precisely. */
AT_TRAP int trap_ds(u32 *st)
{
    return trap_insn(st);
}

__asm__(
    "  .section .patch.0xF016E4A0, \"ax\"\n"
    /* ds_slih+0x38, in place of its `lwz 12, 252(28)` (r30 the state, r29
     * DSISR, r27 DAR): a protection fault in the trap window is done here
     * and ds_slih returns; anything else goes on as before. */
    "ds_hook:\n"
    "  mflr  0\n"
    "  stwu  1, -16(1)\n"
    "  stw   0, 20(1)\n"
    "  andis. 12, 29, 0x0800\n"
    "  beq   1f\n"
    "  mr    3, 30\n"
    "  bl    trap_ds\n"
    "  cmpwi 3, 0\n"
    "  beq   1f\n"
    "  addi  1, 1, 16\n"
    "  b     ds_slih+0xcc\n"
    "1:\n"
    "  lwz   0, 20(1)\n"
    "  addi  1, 1, 16\n"
    "  mtlr  0\n"
    "  lwz   12, 252(28)\n"
    "  blr\n"
    /* pmap_enter+0xd0, in place of `srwi 12, 22, 12` (r22 the physical
     * address, r29 the PP bits just chosen): no access to the trap window. */
    "pp_hook:\n"
    "  addis 11, 22, -0x8800\n"
    "  srwi. 11, 11, 24\n"
    "  bne   1f\n"
    "  li    29, 0\n"
    "1:\n"
    "  srwi  12, 22, 12\n"
    "  blr\n"
    "  .section .patch.ds_slih+0x38, \"ax\"\n  bl ds_hook\n"
    "  .section .patch.pmap_enter+0xd0, \"ax\"\n  bl pp_hook\n"
    /* BAT2 (604): PP 00 in both lower BATs, from `ori 13, 13, 34`. */
    "  .section .patch._start+0x19c, \"ax\"\n  ori 13, 13, 32\n");

/* mc_slih: before anything else (IBM's L2 check, the state dump, the panic),
 * offer the machine check to trap_mc; a handled one returns at once. */
__asm__(
    "  .section .patch.mc_slih+0x28, \"ax\"\n"
    "  mr    3, 29\n"
    "  bl    trap_mc\n"
    "  cmpwi 3, 0\n"
    "  bne   mc_slih+0x5c\n"
    /* IBM's L2 cache, which these drove through planar registers: nothing to do. */
    "  .section .patch.mot_enable_l2_cache, \"ax\"\n  blr\n"
    "  .section .patch.flush_l2_cache, \"ax\"\n  blr\n"
    "  .section .patch.disable_l2_cache, \"ax\"\n  blr\n"
    /* l2_cache_size (pmc_items_init's question): no L2 cache.  prep_io follows. */
    "  .section .patch.l2_cache_size, \"ax\"\n"
    "  li    3, 0\n"
    "  blr\n"
    /* trace_setup, the trace service's set-up, which only a trace tool
     * calls: refused (KERN_FAILURE), its body the room for kernel-video.c and
     * the stand-ins' routines above. */
    "  .section .patch.trace_setup, \"ax\"\n"
    "  li    3, 5\n"
    "  blr\n"
    /* l2_cache_dpe (IBM's L2 error status, in a planar register): none. */
    "  .section .patch.l2_cache_dpe, \"ax\"\n"
    "  li    3, 0\n"
    "  blr\n"
    /* BAT2, the kernel's fixed ISA window, onto the trap window too. */
    "  .section .patch._start+0x144, \"ax\"\n"
    "  lis   15, 0x8800\n");
