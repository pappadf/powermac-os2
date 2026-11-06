// SPDX-License-Identifier: MIT
//
// os2shim -- boot OS/2 Warp (PowerPC Edition) from Open Firmware on an Apple
// Network Server, without PReP firmware and without a lane-reversing bridge.
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
// little-endian world Open Firmware has already put the machine in:
//
//   1. with Open Firmware still alive (client interface): find RAM, claim the
//      work areas, read the boot partition off the CD at its load address,
//      log to the console;
//   2. leave Open Firmware: the stub's BATs (start.S);
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

#include "residual.h"
#include "../patches/src/loader-svc.h"

// The boot device: the first SCSI ID on the first fast/wide bus whose
// partition table has a PReP boot partition (type $41), as PReP firmware's
// boot list would find it.  A fresh install finds the CD (ID 3); once OS/2
// is installed, the hard disk the installer partitioned comes first, which
// is what its "remove the CD-ROM and restart" means.
#define SCSI_BUS     "/bandit/53c825@11/sd@"
#define LOAD_ADDR    0x00800000u // the boot partition, byte 0
#define LOAD_MAX     0x000C0000u // what is claimed there
// IBM's stub, wherever the partition header puts its entry: the little-endian
// body it enters is $6A8 past it, and the words below are its own (identical
// code on the CD and on an installed disk; only its embedded sizes differ).
#define BODY_REL     0x6A8u
#define RESIDUAL_ADDR 0x00700000u

uint32_t shim_cif; // the client interface handler, from r5 (start.S)
void shim_take_mmu(void);
void shim_handoff(uint32_t entry, uint32_t swapped, uint32_t residual, uint32_t residual_len) __attribute__((noreturn));

uint8_t shim_stack[16384] __attribute__((aligned(16))); // start.S sets r1 to its end

_Static_assert(sizeof(RESIDUAL) == 27148, "the PReP residual layout the loader reads");

// --- Open Firmware client interface ------------------------------------------

typedef int (*cif_fn)(void *);
static int cif(const char *service, int nargs, int nrets, uint32_t *args, uint32_t *rets) {
    uint32_t a[16];
    a[0] = (uint32_t)service;
    a[1] = (uint32_t)nargs;
    a[2] = (uint32_t)nrets;
    for (int i = 0; i < nargs; i++)
        a[3 + i] = args[i];
    for (int i = 0; i < nrets; i++)
        a[3 + nargs + i] = 0;
    int r = ((cif_fn)shim_cif)(a);
    for (int i = 0; i < nrets; i++)
        rets[i] = a[3 + nargs + i];
    return r;
}

static size_t slen(const char *s) {
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint32_t of_stdout;
static void of_puts(const char *s) {
    if (!of_stdout)
        return;
    uint32_t a[3] = {of_stdout, (uint32_t)s, (uint32_t)slen(s)}, r;
    cif("write", 3, 1, a, &r);
}

static void of_puthex(uint32_t v) {
    char b[11] = "0x";
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    of_puts(b);
}

static uint32_t of_finddevice(const char *path) {
    uint32_t a[1] = {(uint32_t)path}, r = 0;
    return cif("finddevice", 1, 1, a, &r) ? 0xFFFFFFFFu : r;
}

// Property values are byte strings, and an integer in one is big-endian
// (IEEE 1275 encode-int) whatever mode the firmware runs in; only the
// client interface's argument array is native.
static uint32_t prop_int(const uint32_t *p) {
    return __builtin_bswap32(*p);
}

static int32_t of_getprop(uint32_t ph, const char *name, void *buf, uint32_t len) {
    uint32_t a[4] = {ph, (uint32_t)name, (uint32_t)buf, len}, r = 0;
    return cif("getprop", 4, 1, a, &r) ? -1 : (int32_t)r;
}

static uint32_t of_claim(uint32_t virt, uint32_t size) {
    uint32_t a[3] = {virt, size, 0}, r = 0;
    return cif("claim", 3, 1, a, &r) ? 0xFFFFFFFFu : r;
}

static void of_fail(const char *why) {
    of_puts("os2shim: ");
    of_puts(why);
    of_puts("\r\n");
    cif("exit", 0, 0, NULL, NULL);
    for (;;)
        ;
}

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

#define S32(x) (x) = __builtin_bswap32(x)
#define S16(x) (x) = __builtin_bswap16(x)

// An IBM Power Series 440 ("Sandalfoot", 6015): the loader's SCSI path, the
// model whose NCR 53C810 the Network Server's 53C825A stands in for.  Numbers
// big-endian, as IBM's big-endian firmware writes them.
static uint32_t build_residual(RESIDUAL *r, uint32_t ram, uint32_t pvr) {
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

void shim_main(void) __attribute__((noreturn));
void shim_main(void) {
    uint32_t chosen = of_finddevice("/chosen");
    if (chosen != 0xFFFFFFFFu) {
        uint32_t ih;
        if (of_getprop(chosen, "stdout", &ih, 4) == 4)
            of_stdout = prop_int(&ih);
    }
    of_puts("\r\nos2shim: OS/2 Warp (PowerPC Edition) from Open Firmware\r\n");

    // RAM: /memory's reg, every (base, size) pair summed (the Network Server's
    // POST re-bases its banks contiguously from 0).
    uint32_t reg[32];
    uint32_t ram = 0;
    int32_t rl = of_getprop(of_finddevice("/memory"), "reg", reg, sizeof reg);
    for (int i = 0; i + 1 < rl / 4; i += 2)
        ram += prop_int(&reg[i + 1]);
    if (ram < (16u << 20))
        of_fail("cannot size memory");
    of_puts("os2shim: RAM ");
    of_puthex(ram);
    of_puts("\r\n");

    // The work areas, virtual == physical.
    if (of_claim(RESIDUAL_ADDR, 0x10000) != RESIDUAL_ADDR || of_claim(LOAD_ADDR, 0xC0000) != LOAD_ADDR)
        of_fail("cannot claim $700000/$800000");
    of_claim(0x000FF000u, 0x1000); // the stub's stack page (may already be Open Firmware's: harmless)

    // Find the boot device and its PReP boot partition.
    static uint8_t mbr[512];
    char path[32];
    uint32_t ih = 0, part_lba = 0, part_len = 0;
    int boot_id = -1; // the SCSI ID booted from (reported below)
    for (int id = 0; id <= 6 && !part_len; id++) {
        size_t n = 0;
        for (const char *q = SCSI_BUS; *q; q++)
            path[n++] = *q;
        path[n++] = (char)('0' + id);
        path[n++] = ',';
        path[n++] = '0';
        path[n] = 0;
        uint32_t a1[1] = {(uint32_t)path}, got = 0, rc = 0;
        if (cif("open", 1, 1, a1, &ih) || !ih)
            continue;
        uint32_t a2[3] = {ih, 0, 0};
        cif("seek", 3, 1, a2, &rc);
        uint32_t a3[3] = {ih, (uint32_t)mbr, 512};
        cif("read", 3, 1, a3, &got);
        if (got == 512 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
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
        if (!part_len) {
            uint32_t a4[1] = {ih};
            cif("close", 1, 0, a4, NULL);
        }
    }
    if (!part_len)
        of_fail("no PReP boot partition (type $41) on " SCSI_BUS "0-6");
    (void)boot_id;
    of_puts("os2shim: booting ");
    of_puts(path);
    of_puts("\r\n");

    // Its header (the second sector: entry offset, image length), then the
    // image, as far as the partition or the image ends, whichever is first.
    uint32_t got = 0, rc = 0;
    uint32_t a2[3] = {ih, 0, part_lba * 512u};
    cif("seek", 3, 1, a2, &rc);
    uint32_t a3[3] = {ih, LOAD_ADDR, 1024};
    cif("read", 3, 1, a3, &got);
    const uint8_t *img = (const uint8_t *)LOAD_ADDR;
    uint32_t entry_off = le32_bytes(img + 0x200);
    uint32_t img_len = le32_bytes(img + 0x204);
    uint32_t want = ((entry_off + img_len + 511u) & ~511u);
    if (want > part_len * 512u)
        want = part_len * 512u;
    if (got != 1024 || entry_off < 0x400u || want > LOAD_MAX || entry_off + 0x1000u > want)
        of_fail("not an OS/2 PowerPC boot partition");
    cif("seek", 3, 1, a2, &rc);
    uint32_t a5[3] = {ih, LOAD_ADDR, want};
    cif("read", 3, 1, a5, &got);
    uint32_t a4[1] = {ih};
    cif("close", 1, 0, a4, NULL);
    if (got != want)
        of_fail("short read of the boot partition");
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
        of_fail("unexpected byte-reversal length");
    uint32_t body = LOAD_ADDR + entry_off + BODY_REL;
    of_puts("os2shim: boot image read, body ");
    of_puthex(body);
    of_puts(", ");
    of_puthex(swapped);
    of_puts(" bytes to byte-reverse\r\n");
    of_puts("os2shim: leaving Open Firmware\r\n");

    // From here on nothing of Open Firmware's is used.
    shim_take_mmu();
    escc_puts("os2shim: out of Open Firmware\r\n");

    uint32_t pvr;
    __asm__ volatile("mfpvr %0" : "=r"(pvr));
    uint32_t rlen = build_residual((RESIDUAL *)RESIDUAL_ADDR, ram, pvr);

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

    // The first 53C825A (Bandit 1 device 17), as the loader will find it: its
    // registers through its memory BAR at SCSI_MEM_BAR (the loader's base,
    // remaster patch 90), its I/O BAR at PReP's $20000000 (loader-svc.h),
    // I/O and memory decode and bus mastering on.
    cfg_write32(17, 0x14, SCSI_MEM_BAR);
    cfg_write32(17, 0x10, SCSI_IO_BAR);
    cfg_write32(17, 0x04, (cfg_read32(17, 0x04) & 0xFFFF) | 0x0007);

    // The 54M30 (device 15), where OS/2's hardware database expects a display
    // adapter's 16 MB: PCI $02000000 in PReP's numbering, which the kernel's
    // configuration trap and its $C0000000 -> $80000000 mapping make Bandit's
    // VIDEO_MEM_BAR (loader-svc.h).
    cfg_write32(15, 0x10, VIDEO_MEM_BAR);
    // And its decoders on: the disk's loader (FAT_BL.HD) programs the VGA
    // DAC through the legacy ports, and an undecoded I/O cycle on Bandit is
    // a machine check.
    cfg_write32(15, 0x04, (cfg_read32(15, 0x04) & 0xFFFF) | 0x0003);

    // The loader service at LDRSVC, where patch 90 sends the loader's PC-port,
    // configuration and NVRAM routines.
    copy((void *)LDRSVC, ldrsvc_blob, (size_t)(ldrsvc_blob_end - ldrsvc_blob));
    flush_range(LDRSVC, (uint32_t)(ldrsvc_blob_end - ldrsvc_blob));

    // Nothing of PReP's is set: no spread-I/O or little-endian bit (ports $850
    // and $92), and nothing in an NVRAM (the loader's reads as zeros; an
    // installed system's BOOT.CFG names its boot device).  The CPU's interrupt
    // line is Grand Central's, as on any Network Server (remaster patch 80).
    escc_puts("os2shim: entering the IBM Microkernel Bootloader\r\n");

    shim_handoff(body, swapped, RESIDUAL_ADDR, rlen);
}
