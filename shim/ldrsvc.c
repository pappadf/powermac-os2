// SPDX-License-Identifier: GPL-2.0-only
// ldrsvc.c -- the loader service: IBM's boot loader's PReP hardware, on the
// Network Server's own.
//
// BL_AUTO drives PC ports, PReP PCI configuration and an NVRAM that no Mac
// has.  remaster patch 90 turns the routines that do it into branches to the
// table below; this code does their jobs on the Mac's hardware.  It is linked
// at LDRSVC ($40000) and copied there by the shim just before the hand-off;
// the loader calls it with its MMU on, through the shim's BATs (0-256 MB, and
// $F0000000-$FFFFFFFF cache-inhibited for the I/O).  Like everything the 604
// runs little-endian here, a natural-size access reaches each register
// (the CPU munges addresses, Bandit reverses its lanes).
//
//   PC ports (out8/16/32, in8/16/32 by full PReP address, spread or not):
//     COM1 $3F8-$3FF   a 16550 emulated on Grand Central's ESCC channel A
//     VGA $3B0-$3DF    passed to Bandit 1's PCI I/O space, where the 54M30
//                      decodes the legacy ports
//     8042 $60/$64     no controller: status 0 (nothing to read, room to write)
//     everything else  reads all-ones, writes are dropped
//   PReP direct configuration ($80800000 + offset, through the port helpers or
//     the SCSI module's own routines): Bandit 1 configuration cycles.  PReP slot 12 (the
//     Sandalfoot's 53C810) is the 53C825A: the Network Server's first, device 17,
//     or a card in the first PCI slot, device 13; slot 13 the 54M30, device 15;
//     every other slot is empty.  The chip reports the
//     53C810's device ID ($0001), as PReP's would and as the NT HAL reports
//     it to NT's miniport: the loader accepts nothing else, and the 53C825A
//     is register-compatible for what it does.  Its command register keeps
//     memory decode on (the loader's way to the chip is the memory BAR).
//   NVRAM: reads as zeros.  (So did the PReP window's, apart from the boot
//     device the shim wrote, which BOOT.CFG names now.)
#include <stdint.h>

#include "../patches/src/loader-svc.h"

#define ESCC_A_CTRL 0xF3013020u
#define ESCC_A_DATA 0xF3013030u
#define BANDIT1_IO  0xF2000000u
#define BANDIT1_CFG_ADDR 0xF2800000u
#define BANDIT1_CFG_DATA 0xF2C00000u

__asm__(".section .text.entry, \"ax\"\n"
        "  b svc_out8\n"
        "  b svc_out16\n"
        "  b svc_out32\n"
        "  b svc_in8\n"
        "  b svc_in16\n"
        "  b svc_in32\n"
        "  b svc_cfg_rd32\n"
        "  b svc_cfg_wr16\n"
        "  b svc_cfg_wr32\n"
        "  b svc_nvram_rd\n"
        "  b svc_scsi_init\n"
        "  b svc_scsi_probe\n"
        "  b svc_scsi_rw\n"
        "  .org 0x40\n"
        "  .long 0\n"   // LDRSVC_SCSI_CHIP, written by the shim
        "  .long 0\n"   // LDRSVC_VGA: a 54M30 decodes the VGA ports
        "  .long 255\n"); // LDRSVC_BOOT_DISK

static inline void eieio(void) {
    __asm__ volatile("eieio" ::: "memory");
}
static inline uint32_t rd(uint32_t a, unsigned size) {
    uint32_t v = size == 1 ? *(volatile uint8_t *)a : size == 2 ? *(volatile uint16_t *)a : *(volatile uint32_t *)a;
    eieio();
    return v;
}
static inline void wr(uint32_t a, unsigned size, uint32_t v) {
    if (size == 1)
        *(volatile uint8_t *)a = (uint8_t)v;
    else if (size == 2)
        *(volatile uint16_t *)a = (uint16_t)v;
    else
        *(volatile uint32_t *)a = v;
    eieio();
}
#define LDRSVC_VGA (LDRSVC + 0x44)

static uint32_t ones(unsigned size) {
    return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

// --- PCI configuration ---------------------------------------------------------

// The PReP slot a direct-config offset names (its lowest IDSEL bit, A11-A18).
static unsigned cfg_slot(uint32_t off) {
    for (unsigned b = 11; b <= 18; b++)
        if (off & (1u << b))
            return b;
    return 0;
}

// The 53C825A's device: the Network Server's first soldered chip (17) where
// there is one, otherwise a card in the first PCI slot (13), as the shim found.
static unsigned scsi_dev(void) {
    wr(BANDIT1_CFG_ADDR, 4, 1u << 17);
    return (rd(BANDIT1_CFG_DATA, 4) & 0xFFFF) == 0x1000 ? 17 : 13;
}

// A PReP direct-config offset: the Bandit data-port address for it, or 0.
static uint32_t cfg_select(uint32_t off) {
    unsigned slot = cfg_slot(off);
    unsigned dev = slot == 12 ? scsi_dev() : slot == 13 ? 15 : 0;
    if (!dev)
        return 0;
    uint32_t fn = (off >> 8) & 7, reg = off & 0xFF;
    wr(BANDIT1_CFG_ADDR, 4, (1u << dev) | (fn << 8) | (reg & 0xFC));
    return BANDIT1_CFG_DATA + (reg & 3);
}

static uint32_t cfg_read(uint32_t off, unsigned size) {
    uint32_t p = cfg_select(off);
    if (!p)
        return ones(size);
    uint32_t v = rd(p, size), reg = off & 0xFF;
    // The first 53C825A as the 53C810 the loader knows (device ID at +2).
    if (cfg_slot(off) == 12 && (reg & 0xFC) == 0) {
        if (size == 4)
            v = (v & 0xFFFF) | 0x00010000u;
        else if (size == 2 && (reg & 2))
            v = 0x0001;
    }
    return v;
}

static void cfg_write(uint32_t off, unsigned size, uint32_t v) {
    uint32_t reg = off & 0xFF;
    if ((reg & 0xFC) == 0x04 && (reg & 3) == 0)
        v |= 0x0002; // keep memory decode on: the loader reaches the chip through its memory BAR
    uint32_t p = cfg_select(off);
    if (p)
        wr(p, size, v);
}

static uint32_t io_in(uint32_t addr, unsigned size);
static void io_out(uint32_t addr, unsigned size, uint32_t v);

// The SCSI module's configuration routines, with the full PReP address.
uint32_t svc_cfg_rd32(uint32_t addr) {
    return io_in(addr, 4);
}
void svc_cfg_wr16(uint32_t addr, uint32_t v) {
    io_out(addr, 2, v & 0xFFFF);
}
void svc_cfg_wr32(uint32_t addr, uint32_t v) {
    io_out(addr, 4, v);
}
uint32_t svc_nvram_rd(uint32_t index) {
    (void)index;
    return 0;
}

// --- COM1 on the ESCC --------------------------------------------------------------

static uint8_t lcr = 0x03, dll = 0x0C, dlm, ier, mcr, scr;

static void escc_put(uint8_t c) {
    for (int spin = 0; spin < 100000 && !(rd(ESCC_A_CTRL, 1) & 0x04); spin++)
        ;
    wr(ESCC_A_DATA, 1, c);
}

static uint32_t com1_in(unsigned reg) {
    switch (reg) {
    case 0:
        if (lcr & 0x80)
            return dll;
        return (rd(ESCC_A_CTRL, 1) & 0x01) ? rd(ESCC_A_DATA, 1) : 0;
    case 1:
        return (lcr & 0x80) ? dlm : ier;
    case 2:
        return 0x01; // IIR: nothing pending
    case 3:
        return lcr;
    case 4:
        return mcr;
    case 5: // LSR: the transmitter is always ready (com1_out waits), data when the ESCC has some
        return 0x60 | (rd(ESCC_A_CTRL, 1) & 0x01);
    case 6:
        return 0xB0; // MSR: CTS, DSR, DCD
    default:
        return scr;
    }
}

static void com1_out(unsigned reg, uint8_t v) {
    switch (reg) {
    case 0:
        if (lcr & 0x80)
            dll = v;
        else
            escc_put(v);
        break;
    case 1:
        if (lcr & 0x80)
            dlm = v;
        else
            ier = v;
        break;
    case 3:
        lcr = v;
        break;
    case 4:
        mcr = v;
        break;
    case 7:
        scr = v;
        break;
    default:
        break;
    }
}

// --- PC ports ----------------------------------------------------------------------

enum { NONE, PORT, CONFIG };

// What a PReP I/O address is: a PC port (in the spread map, port $850 bit 0,
// or not) or a direct-config offset.
static int classify(uint32_t addr, uint32_t *x) {
    if (addr < 0x80000000u || addr >= 0x81000000u)
        return NONE;
    uint32_t off = addr - 0x80000000u;
    if (off >= 0x00800000u) {
        *x = off - 0x00800000u;
        return CONFIG;
    }
    // Spread: ((port & $FFE0) << 7) | (port & $1F), so bits 5-11 are clear.
    if (off >= 0x1000 && !(off & 0xFE0))
        *x = ((off >> 7) & 0xFFE0) | (off & 0x1F);
    else
        *x = off;
    return PORT;
}

static uint32_t port_in(uint32_t port, unsigned size) {
    if (port >= 0x3F8 && port <= 0x3FF)
        return com1_in(port - 0x3F8);
    if (port >= 0x3B0 && port <= 0x3DF && (port & ~3u) != 0x3BC && *(volatile uint32_t *)LDRSVC_VGA)
        return rd(BANDIT1_IO + port, size);
    if (port == 0x60 || port == 0x64)
        return 0;
    return ones(size);
}

static void port_out(uint32_t port, unsigned size, uint32_t v) {
    if (port >= 0x3F8 && port <= 0x3FF)
        com1_out(port - 0x3F8, (uint8_t)v);
    else if (port >= 0x3B0 && port <= 0x3DF && (port & ~3u) != 0x3BC && *(volatile uint32_t *)LDRSVC_VGA)
        wr(BANDIT1_IO + port, size, v);
}

static uint32_t io_in(uint32_t addr, unsigned size) {
    uint32_t x;
    switch (classify(addr, &x)) {
    case PORT:
        return port_in(x, size);
    case CONFIG:
        return cfg_read(x, size);
    default:
        return ones(size);
    }
}

static void io_out(uint32_t addr, unsigned size, uint32_t v) {
    uint32_t x;
    switch (classify(addr, &x)) {
    case PORT:
        port_out(x, size, v);
        break;
    case CONFIG:
        cfg_write(x, size, v);
        break;
    default:
        break;
    }
}

void svc_out8(uint32_t a, uint32_t v) {
    io_out(a, 1, v & 0xFF);
}
void svc_out16(uint32_t a, uint32_t v) {
    io_out(a, 2, v & 0xFFFF);
}
void svc_out32(uint32_t a, uint32_t v) {
    io_out(a, 4, v);
}
uint32_t svc_in8(uint32_t a) {
    return io_in(a, 1);
}
uint32_t svc_in16(uint32_t a) {
    return io_in(a, 2);
}
uint32_t svc_in32(uint32_t a) {
    return io_in(a, 4);
}

// --- the SCSI module's disk routines ----------------------------------------------
//
// The loader keeps a 16-byte record per unit (0 the hard disk, 1 the CD):
// +0 the device type, +1 its SCSI ID, +4 1, +8 its last block, +12 its block
// size.  A transfer request is five words: the unit, the operation (8 read, 9
// write), the first block, the buffer and the block count.  The module's own
// routines fill and serve them through its SCRIPTS driver for the 53C810;
// these do the same through scsi.c, on whichever chip the shim found.

static uint8_t scsi_area[1024] __attribute__((aligned(32)));
#define SCSI_WORK ((uint32_t)scsi_area)
#include "scsi.c"

struct unit {
    uint8_t type, id, pad[2];
    uint32_t one, last, bs;
};

uint32_t svc_scsi_init(void) {
    scsi_chip = *(volatile uint32_t *)LDRSVC_SCSI_CHIP;
    scsi_init();
    return scsi_chip != 0;
}

// One target: ready (a CD may still be spinning up), the device type the unit
// wants (0 disk, 5 CD-ROM), and its capacity.
static int probe_id(struct unit *u, int id, unsigned type) {
    static const uint8_t tur[6] = {0x00, 0, 0, 0, 0, 0};
    static const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    int st = -2;
    for (int tries = 0; tries < 20 && st != 0; tries++) {
        st = scsi_do(id, tur, 6, 0, 0, 0);
        if (st == -1)
            return 0;
    }
    if (st != 0)
        return 0;
    if (scsi_do(id, inq, 6, SCSI_SENSE, 36, 0) != 0)
        return 0;
    if ((*(volatile uint8_t *)SCSI_SENSE & 0x1F) != type)
        return 0;
    uint32_t last, bs;
    if (scsi_capacity(id, &last, &bs) || !bs)
        return 0;
    u->type = (uint8_t)type;
    u->id = (uint8_t)id;
    u->pad[0] = 0;
    u->last = last;
    u->bs = bs;
    return 1;
}

// The unit's device: at `id`, or (id 255) the highest ID that has one.  The
// disk's loader (FAT_BL.HD) asks for the hard disk at ID 6, a Sandalfoot's;
// it is the disk the shim booted from, wherever that is (a Power
// Macintosh's internal bay is ID 0).
uint32_t svc_scsi_probe(uint32_t unit, uint32_t id, uint32_t type, struct unit *units) {
    struct unit *u = &units[unit];
    uint32_t boot = *(volatile uint32_t *)LDRSVC_BOOT_DISK;
    if (type == 0 && boot < 7)
        id = boot;
    u->id = (uint8_t)id;
    if (id != 255)
        return (uint32_t)probe_id(u, (int)id, type);
    for (int i = 6; i >= 0; i--)
        if (probe_id(u, i, type))
            return 1;
    return 0;
}

// BOOT.CFG names the boot disk "devices/scsi0?" (remaster patch 50): the "?"
// becomes the ID of the disk the shim booted from, as the disk's loader reads
// the file in.
static void name_boot_disk(uint8_t *p, uint32_t n) {
    static const char key[] = "devices/scsi0?";
    uint32_t boot = *(volatile uint32_t *)LDRSVC_BOOT_DISK;
    if (boot > 6)
        return;
    for (uint32_t i = 0; i + sizeof key - 1 <= n; i++) {
        uint32_t k = 0;
        while (k < sizeof key - 1 && p[i + k] == (uint8_t)key[k])
            k++;
        if (k == sizeof key - 1)
            p[i + k - 1] = (uint8_t)('0' + boot);
    }
}

uint32_t svc_scsi_rw(const uint32_t *req, struct unit *units) {
    const struct unit *u = &units[req[0]];
    uint32_t lba = req[2], buf = req[3], n = req[4];
    if (lba > u->last || !u->bs)
        return 0; // nothing past its last block
    if (scsi_rw10(u->id, lba, n, u->bs, buf, req[1] == 9))
        return 0;
    if (req[1] == 8 && u->type == 0)
        name_boot_disk((uint8_t *)buf, n * u->bs);
    return 1;
}
