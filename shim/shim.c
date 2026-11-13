// SPDX-License-Identifier: GPL-2.0-only
//
// os2shim -- boot OS/2 Warp (PowerPC Edition) from Open Firmware on an Apple
// Network Server or a Power Macintosh 7500/8500/9500, without PReP firmware.
//
// IBM's boot image (the CD's PReP type-$41 partition) starts with a big-endian
// stub that a PReP firmware enters with r3 = residual data.  The stub checks
// the machine, programs two PReP ports, byte-reverses the first part of the
// little-endian body word by word, sets up the BATs and rfi's into it.  The
// body then expects TRUE little-endian memory -- which a PReP host bridge
// provides by reversing its byte lanes, and which no Macintosh has.
//
// On a Mac the CPU's little-endian mode is the only reversal there is (the
// 60x munges addresses).  Software that only ever uses the CPU cannot tell
// the two apart, so the shim does the stub's work itself, in the munged
// little-endian world stage0 has put the machine in:
//
//   1. stage0 (stage0.c, big-endian, Open Firmware still alive): the RAM size,
//      the stub's BATs, then little-endian mode for the CPU and the bridges,
//      and into this file with no firmware left;
//   2. the boot device and its partition, read with the shim's own SCSI
//      driver at the load address, logged to the serial console;
//   3. the residual data at $700000 -- big-endian fields, as IBM's big-endian
//      firmware hands it over (the body converts it);
//   4. the stub's memory effects: one big-endian store into its own image,
//      its stack frames below $100000, and the region it byte-reverses word
//      by word (r4 bytes from the entry);
//   5. PReP ports $850 (the spread I/O map) and $92 (little-endian);
//   6. the stub's registers, and rfi into the body (start.S).
//
// Everything here is ordinary little-endian C: the shim runs in exactly the
// memory model the loader will, so what it writes is what the loader reads.

#include <stddef.h>
#include <stdint.h>

#include "prep-residual.h"
#include "../patches/src/loader-svc.h"

// The boot device: the first hard disk on the SCSI chip (scsi_chip, below)
// whose partition table has a PReP boot partition (type $41), else the first
// such CD.  A fresh install finds the CD (ID 3), the blank disk having no
// partition table; once OS/2 is installed its hard disk comes first, CD or no
// CD in the drive.
#define LOAD_ADDR    0x00800000u // the boot partition, byte 0
#define LOAD_MAX     0x000C0000u // what is claimed there
// IBM's stub, wherever the partition header puts its entry: the little-endian
// body it enters is $6A8 past it, and the words below are its own (identical
// code on the CD and on an installed disk; only its embedded sizes differ).
#define BODY_REL     0x6A8u
#define RESIDUAL_ADDR 0x00700000u

void shim_handoff(uint32_t entry, uint32_t swapped, uint32_t residual, uint32_t residual_len) __attribute__((noreturn));

uint8_t shim_stack[16384] __attribute__((aligned(16))); // start.S sets r1 to its end

_Static_assert(sizeof(RESIDUAL) == 27148, "the PReP residual layout the loader reads");

// --- after Open Firmware: the Network Server's own I/O (through BAT3) ----------

// Grand Central's ESCC channel A (Open Firmware's ttya, already programmed)
// and Bandit 1's configuration ports, physical addresses that BAT3 maps
// one-to-one, cache-inhibited (start.S).
#define ESCC_A_CTRL      0xF3013020u
#define ESCC_A_DATA      0xF3013030u
#define BANDIT1_CFG_ADDR 0xF2800000u
#define BANDIT1_CFG_DATA 0xF2C00000u

static void eieio(void) {
    __asm__ volatile("eieio" ::: "memory");
}

static void escc_puts(const char *s) {
    for (; *s; s++) {
        for (int spin = 0; spin < 100000 && !(*(volatile uint8_t *)ESCC_A_CTRL & 0x04); spin++)
            eieio();
        *(volatile uint8_t *)ESCC_A_DATA = (uint8_t)*s;
        eieio();
    }
}

static void shim_fail(const char *why) __attribute__((noreturn));
static void shim_fail(const char *why) {
    escc_puts("os2shim: ");
    escc_puts(why);
    escc_puts("\r\n");
    for (;;)
        ;
}

// A Bandit 1 type-0 configuration cycle: (1 << device) | (function << 8) |
// register to the address port, then the data port (NT HAL pci.c).
static void cfg_select(unsigned dev, uint32_t reg) {
    *(volatile uint32_t *)BANDIT1_CFG_ADDR = (1u << dev) | (reg & 0xFC);
    eieio();
}
static uint32_t cfg_read32(unsigned dev, uint32_t reg) {
    cfg_select(dev, reg);
    uint32_t v = *(volatile uint32_t *)BANDIT1_CFG_DATA;
    eieio();
    return v;
}
static void cfg_write32(unsigned dev, uint32_t reg, uint32_t v) {
    cfg_select(dev, reg);
    *(volatile uint32_t *)BANDIT1_CFG_DATA = v;
    eieio();
}

// The loader service (ldrsvc.c, built and linked at LDRSVC), embedded by
// ldrsvc_blob.S.
extern const uint8_t ldrsvc_blob[], ldrsvc_blob_end[];

// --- the residual data --------------------------------------------------------

static void zero(void *p, size_t n) {
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}
static void copy(void *d, const void *s, size_t n) {
    uint8_t *db = d;
    const uint8_t *sb = s;
    while (n--)
        *db++ = *sb++;
}

// --- the SCSI chip, without Open Firmware -------------------------------------
//
// The boot partition is read by the shim itself, after Open Firmware is left:
// Open Firmware has a driver only for the machine's own chips, and none for a
// PC SCSI card, which carries no FCode.  scsi.c is one small polled driver for
// a 53C825A (the Network Server's, or a card in a Power Macintosh's first PCI
// slot) and for a Power Macintosh's MESH; the loader service carries the same
// code.  Everything is physical (the shim runs virtual == physical through
// BAT0, and Bandit's bus addresses are physical).

// Its work area, and a bounce buffer for whole blocks: below the boot image's
// $800000 and above the residual data's 64 KB at $700000.
#define SCSI_WORK    0x00780000u
#define SCSI_BOUNCE  0x00900000u
#define SCSI_CHUNK   0x8000u      // bytes per READ(10)
#include "scsi.c"
#include "control.c"

// The 53C825A the loader and OS/2 drive: the Network Server's first soldered
// chip (Bandit 1 device 17) where there is one, otherwise a 53C825A card in
// the first PCI slot, device 13 (a Power Macintosh 7500/8500/9500's A1, a
// Network Server's slot 1).
static int is_53c825(unsigned dev) {
    return cfg_read32(dev, 0x00) == 0x00031000u;
}

static uint32_t scsi_block[8]; // the block size at each ID, once read

// Bytes [off, off + len) of the device at `id` into dst: whole blocks through
// the bounce buffer.  0 on success.
static int scsi_read(int id, uint32_t off, uint32_t len, void *dst) {
    uint32_t bs = scsi_block[id];
    if (!bs) {
        uint32_t last;
        if (scsi_capacity(id, &last, &bs) || (bs != 512 && bs != 2048))
            return -1;
        scsi_block[id] = bs;
    }
    uint8_t *d = dst;
    while (len) {
        uint32_t lba = off / bs, skip = off % bs;
        uint32_t want = skip + len;
        if (want > SCSI_CHUNK)
            want = SCSI_CHUNK;
        uint32_t nblk = (want + bs - 1) / bs;
        if (scsi_rw10(id, lba, nblk, bs, SCSI_BOUNCE, 0))
            return -1;
        uint32_t n = nblk * bs - skip;
        if (n > len)
            n = len;
        copy(d, (const void *)(SCSI_BOUNCE + skip), n);
        d += n;
        off += n;
        len -= n;
    }
    return 0;
}

#define S32(x) (x) = __builtin_bswap32(x)
#define S16(x) (x) = __builtin_bswap16(x)

// An IBM Power Series 440 ("Sandalfoot", 6015): the loader's SCSI path, the
// model whose NCR 53C810 the Network Server's 53C825A stands in for.  Numbers
// big-endian, as IBM's big-endian firmware writes them.
static uint32_t build_residual(RESIDUAL *r, uint32_t ram, uint32_t pvr, int shadow) {
    zero(r, sizeof *r);
    r->ResidualLength = sizeof *r;
    VPD *v = &r->VitalProductData;
    copy(v->PrintableModel, "IBM PPS Model 6015", 19);
    copy(v->Serial, "IBM60150000001  ", 16);
    v->NvramSize = 8192;
    v->NumSIMMSlots = 8;
    v->EndianSwitchMethod = 2; // UsePort92
    v->SpreadIOMethod = 1;     // UsePort850
    v->ProcessorHz = 132000000u;
    v->ProcessorBusHz = 44000000u;
    v->TimeBaseDivisor = 4000; // a 604 counts the bus clock / 4
    v->WordWidth = 32;
    v->PageSize = 4096;
    v->CoherenceBlockSize = 32;
    v->GranuleSize = 32;
    v->CacheSize = 32;
    v->CacheAttrib = 2;
    v->I_CacheSize = 16;
    v->I_CacheAssoc = 4;
    v->I_CacheLineSize = 32;
    v->D_CacheSize = 16;
    v->D_CacheAssoc = 4;
    v->D_CacheLineSize = 32;
    v->TLBSize = 128;
    v->TLBAttrib = 2;
    v->I_TLBSize = 64;
    v->I_TLBAssoc = 2;
    v->D_TLBSize = 64;
    v->D_TLBAssoc = 2;
    r->MaxNumCpus = 1;
    r->ActualNumCpus = 1;
    r->Cpus[0].CpuType = pvr;
    // With the on-board display, its shadow frame buffer (control.c) is the
    // top megabyte of the first 64 MB, and OS/2 is told its RAM ends there:
    // the kernel sizes memory from TotalMemory and ignores holes in the
    // segment list (its page table goes at the top).  RAM past 64 MB is then
    // not used.
    if (shadow)
        ram = SHADOW_PA;
    r->TotalMemory = ram;
    r->GoodMemory = ram;
    uint32_t n = 0;
    r->Segs[n++] = (MEM_MAP){Free, 0, 0x800};
    r->Segs[n++] = (MEM_MAP){BootImage, 0x800, 0x100};
    r->Segs[n++] = (MEM_MAP){Free, 0x900, (ram >> 12) - 0x900};
    r->Segs[n++] = (MEM_MAP){SystemIO, 0x80000, 0x800};
    r->Segs[n++] = (MEM_MAP){PCIConfig, 0x80800, 0x800};
    r->Segs[n++] = (MEM_MAP){PCIAddr, 0xC0000, 0x3F000};
    r->Segs[n++] = (MEM_MAP){SystemROM, 0xFFF00, 0x100};
    r->ActualNumMemSegs = n;
    r->ActualNumMemories = 8;
    r->Memories[0].SIMMSize = ram >> 20;

    S32(r->ResidualLength);
    S16(r->EC);
    for (uint32_t *q = &v->FirmwareSupplier; q < (uint32_t *)&v->EndianSwitchMethod; q++)
        S32(*q);
    S16(v->EndianSwitchMethod);
    S16(v->SpreadIOMethod);
    for (uint32_t *q = &v->SmpIar; q <= &v->ExtendedVPD; q++)
        S32(*q);
    for (int i = 0; i < MAX_CPUS; i++) {
        S32(r->Cpus[i].CpuType);
        S16(r->Cpus[i].Reserved);
    }
    S16(r->MaxNumCpus);
    S16(r->ActualNumCpus);
    S32(r->TotalMemory);
    S32(r->GoodMemory);
    S32(r->ActualNumMemSegs);
    for (int i = 0; i < MAX_MEM_SEGS; i++) {
        S32(r->Segs[i].Usage);
        S32(r->Segs[i].BasePage);
        S32(r->Segs[i].PageCount);
    }
    S32(r->ActualNumMemories);
    for (int i = 0; i < MAX_MEMS; i++)
        S32(r->Memories[i].SIMMSize);
    S32(r->ActualNumDevices);
    return sizeof *r;
}

// --- the stub's memory effects -------------------------------------------------

// The stub's stack, $FFFC0-$FFFFF, as it is when the stub rfi's into the body
// (big-endian stores, read back little-endian by nobody but the body's frame
// walk; reproduced byte for byte).  As captured from the CD's stub; the words
// that depend on the image (its addresses, the byte-reversal length) are
// recomputed for the image actually loaded.
static const uint8_t stub_stack[64] = {
    0x00, 0xfc, 0x00, 0x10, 0x00, 0xfc, 0x00, 0x0f, 0x00, 0x0f, 0xff, 0xd8, 0x00, 0xfc, 0x00, 0x0d,
    0x00, 0x86, 0x01, 0x4c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0xff, 0xf8, 0x00, 0x85, 0xed, 0x7c,
    0x00, 0x85, 0xf2, 0xa8, 0x00, 0x0f, 0xff, 0xf8, 0x00, 0x0f, 0xff, 0xf8, 0x00, 0x85, 0xf2, 0xa8,
    0x00, 0x00, 0x0e, 0xa4, 0x00, 0x00, 0x46, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x85, 0xf2, 0x20,
};

// A little-endian word at any address, one byte at a time: the partition
// table's fields are not word-aligned, and the compiler merges plain byte
// loads into one lwz, which faults in little-endian mode.
static uint32_t le32_bytes(const volatile uint8_t *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void flush_range(uint32_t a, uint32_t n) {
    for (uint32_t p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("dcbst 0,%0" ::"r"(p) : "memory");
    __asm__ volatile("sync" ::: "memory");
    for (uint32_t p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("icbi 0,%0" ::"r"(p) : "memory");
    __asm__ volatile("sync; isync" ::: "memory");
}

// --- main ----------------------------------------------------------------------

void shim_main(uint32_t ram, uint32_t magic) __attribute__((noreturn));
void shim_main(uint32_t ram, uint32_t magic) {
    // stage0 (the big-endian front end) asked Open Firmware for the RAM size,
    // loaded the BATs, reversed Bandit's byte lanes and entered here in
    // little-endian mode; nothing of the firmware's is used from here on.
    if (magic != 0x4C453031u)
        shim_fail("not entered by stage0");
    if (ram < (16u << 20))
        shim_fail("cannot size memory");
    escc_puts("os2shim: little-endian, out of Open Firmware\r\n");

    // The SCSI chip: a 53C825A where there is one (scsi_chip = its device),
    // as the loader will find it -- its registers through its memory BAR at
    // SCSI_MEM_BAR (the loader's base, remaster patch 90), its I/O BAR at
    // PReP's $20000000 (loader-svc.h), I/O and memory decode and bus
    // mastering on.  Otherwise a Power Macintosh's own internal bus, MESH.
    scsi_chip = is_53c825(17) ? 17 : is_53c825(13) ? 13 : SCSI_CHIP_MESH;
    escc_puts(scsi_chip == 17   ? "os2shim: SCSI: the built-in 53C825A (device 17)\r\n"
              : scsi_chip == 13 ? "os2shim: SCSI: a 53C825A card in the first PCI slot (device 13)\r\n"
                                : "os2shim: SCSI: the internal bus (MESH)\r\n");
    if (scsi_chip != SCSI_CHIP_MESH) {
        cfg_write32(scsi_chip, 0x14, SCSI_MEM_BAR);
        cfg_write32(scsi_chip, 0x10, SCSI_IO_BAR);
        // Its SCRIPTS RAM (BAR2, 4 KB) beside them, out of the 54M30's way: a
        // Power Macintosh's Open Firmware leaves it at the bottom of the PCI
        // memory it assigned, $82000000, which is where the display memory
        // goes below (VIDEO_MEM_BAR).  OS/2 never sees this BAR (the
        // configuration trap presents a 53C810, which has none).
        cfg_write32(scsi_chip, 0x18, SCSI_MEM_BAR + 0x1000u);
        cfg_write32(scsi_chip, 0x04, (cfg_read32(scsi_chip, 0x04) & 0xFFFF) | 0x0007);
    }
    scsi_init();

    // Find the boot device and its PReP boot partition.
    static uint8_t mbr[512];
    uint32_t part_lba = 0, part_len = 0;
    int boot_id = -1;
    // Hard disks first (512-byte blocks), then CDs: an installed system boots
    // with the install CD still in its drive, and a blank disk (no partition
    // table) falls through to the CD.
    for (int pass = 0; pass < 2 && !part_len; pass++)
    for (int id = 0; id <= 6 && !part_len; id++) {
        if (scsi_read(id, 0, 512, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
            continue;
        if ((scsi_block[id] == 2048) != (pass == 1))
            continue;
        for (int e = 0; e < 4; e++) {
            const uint8_t *pe = mbr + 446 + 16 * e;
            if (pe[4] == 0x41) {
                part_lba = le32_bytes(pe + 8);
                part_len = le32_bytes(pe + 12);
                boot_id = id;
                break;
            }
        }
    }
    if (!part_len)
        shim_fail("no PReP boot partition (type $41) at SCSI ID 0-6");
    {
        char m[] = "os2shim: booting SCSI ID 0\r\n";
        m[25] = (char)('0' + boot_id);
        escc_puts(m);
    }

    // Its header (the second sector: entry offset, image length), then the
    // image, as far as the partition or the image ends, whichever is first.
    if (scsi_read(boot_id, part_lba * 512u, 1024, (void *)LOAD_ADDR))
        shim_fail("cannot read the boot partition");
    const uint8_t *img = (const uint8_t *)LOAD_ADDR;
    uint32_t entry_off = le32_bytes(img + 0x200);
    uint32_t img_len = le32_bytes(img + 0x204);
    uint32_t want = ((entry_off + img_len + 511u) & ~511u);
    if (want > part_len * 512u)
        want = part_len * 512u;
    if (entry_off < 0x400u || want > LOAD_MAX || entry_off + 0x1000u > want)
        shim_fail("not an OS/2 PowerPC boot partition");
    if (scsi_read(boot_id, part_lba * 512u, want, (void *)LOAD_ADDR))
        shim_fail("short read of the boot partition");
    // The stub's one store into its own image (at entry+$4D4): the word at
    // entry+$C20 becomes the sum of those at +$C14 and +$C1C, big-endian.
    // Then the length it byte-reverses is that word less +$C14's and the
    // body's offset ($46A8 on both media).
    uint8_t *im = (uint8_t *)LOAD_ADDR;
    uint32_t c14 = be32(im + entry_off + 0xC14), c1c = be32(im + entry_off + 0xC1C);
    uint32_t sum = c14 + c1c;
    im[entry_off + 0xC20] = (uint8_t)(sum >> 24);
    im[entry_off + 0xC21] = (uint8_t)(sum >> 16);
    im[entry_off + 0xC22] = (uint8_t)(sum >> 8);
    im[entry_off + 0xC23] = (uint8_t)sum;
    uint32_t swapped = sum - (c14 + BODY_REL);
    if (swapped == 0 || swapped > 0x10000u || (swapped & 3))
        shim_fail("unexpected byte-reversal length");
    uint32_t body = LOAD_ADDR + entry_off + BODY_REL;
    escc_puts("os2shim: boot image read\r\n");

    uint32_t pvr;
    __asm__ volatile("mfpvr %0" : "=r"(pvr));
    // Without a display card, the board's own display (control.c).
    // (Its shadow frame buffer is the top megabyte of the 64 MB OS/2 needs.)
    int onboard = cfg_read32(15, 0x00) != 0x00A01013u && ram >= SHADOW_PA + SHADOW_SIZE && control_setup();
    if (onboard)
        escc_puts("os2shim: display: the on-board Control, 640x480x8\r\n");
    uint32_t rlen = build_residual((RESIDUAL *)RESIDUAL_ADDR, ram, pvr, onboard);

    uint32_t *w = (uint32_t *)body;
    for (uint32_t i = 0; i < swapped / 4; i++)
        w[i] = __builtin_bswap32(w[i]);
    // The stub's frames, with the words that depend on the image recomputed
    // (big-endian, as the stub stores them).
    static uint8_t stack[sizeof stub_stack];
    copy(stack, stub_stack, sizeof stub_stack);
    const uint32_t entry = LOAD_ADDR + entry_off;
    const struct {
        unsigned word;
        uint32_t value;
    } fix[] = {{4, entry + c1c}, {7, entry + 0x17Cu}, {8, body}, {11, body}, {12, swapped}, {13, c14 + BODY_REL},
               {15, entry + 0x620u}};
    for (unsigned i = 0; i < sizeof fix / sizeof fix[0]; i++) {
        uint8_t *q = stack + 4 * fix[i].word;
        q[0] = (uint8_t)(fix[i].value >> 24);
        q[1] = (uint8_t)(fix[i].value >> 16);
        q[2] = (uint8_t)(fix[i].value >> 8);
        q[3] = (uint8_t)fix[i].value;
    }
    copy((void *)0x000FFFC0u, stack, sizeof stack);
    flush_range(LOAD_ADDR, want);
    flush_range(RESIDUAL_ADDR, rlen);

    if (cfg_read32(15, 0x00) == 0x00A01013u) { // a 54M30 card in the third slot
        // The 54M30 (device 15), where OS/2's hardware database expects a display
        // adapter's 16 MB: PCI $02000000 in PReP's numbering, which the kernel's
        // configuration trap and its $C0000000 -> $80000000 mapping make Bandit's
        // VIDEO_MEM_BAR (loader-svc.h).
        cfg_write32(15, 0x10, VIDEO_MEM_BAR);
        // And its decoders on: the disk's loader (FAT_BL.HD) programs the VGA
        // DAC through the legacy ports, and an undecoded I/O cycle on Bandit is
        // a machine check.
        cfg_write32(15, 0x04, (cfg_read32(15, 0x04) & 0xFFFF) | 0x0003);
        // No interrupt line, as on the Network Server, where the 54M30 has none:
        // a Power Macintosh's Open Firmware writes its slot's Grand Central source
        // (C1: $19) into the register even though the card has no interrupt pin.
        cfg_write32(15, 0x3C, cfg_read32(15, 0x3C) & ~0xFFu);
    }

    // The loader service at LDRSVC, where patch 90 sends the loader's PC-port,
    // configuration and NVRAM routines.
    copy((void *)LDRSVC, ldrsvc_blob, (size_t)(ldrsvc_blob_end - ldrsvc_blob));
    *(volatile uint32_t *)LDRSVC_SCSI_CHIP = scsi_chip;
    *(volatile uint32_t *)(LDRSVC + 0x44) = cfg_read32(15, 0x00) == 0x00A01013u; // LDRSVC_VGA: the 54M30's ports
    *(volatile uint32_t *)LDRSVC_BOOT_DISK = scsi_block[boot_id] == 512 ? (uint32_t)boot_id : 255u;
    flush_range(LDRSVC, (uint32_t)(ldrsvc_blob_end - ldrsvc_blob));

    // Nothing of PReP's is set: no spread-I/O or little-endian bit (ports $850
    // and $92), and nothing in an NVRAM (the loader's reads as zeros; an
    // installed system's BOOT.CFG names its boot device).  The CPU's interrupt
    // line is Grand Central's, as on any Network Server (remaster patch 80).
    escc_puts("os2shim: entering the IBM Microkernel Bootloader\r\n");

    shim_handoff(body, swapped, RESIDUAL_ADDR, rlen);
}
