// SPDX-License-Identifier: GPL-2.0-only
// scsi.c -- one polled SCSI initiator for the shim and the loader service.
//
// #included by shim.c and ldrsvc.c, which run in different places (the shim
// with Open Firmware's BATs gone, the loader service under IBM's loader) but
// in the same memory model: virtual == physical, the CPU little-endian, and
// Bandit reversing its byte lanes, so that every register and every DMA
// structure is written as its little-endian PCI owner expects, with natural
// stores.  The includer defines SCSI_WORK, a 1 KB 32-byte-aligned DMA work
// area, and sets scsi_chip.
//
// Two chips:
//
//   a 53C825A (scsi_chip = its Bandit 1 device, 13 or 17): the Network
//     Server's soldered chip or a card in a Power Macintosh's first PCI slot,
//     through its own SCRIPTS processor and its memory BAR at SCSI_MEM_BAR;
//
//   MESH (scsi_chip = SCSI_CHIP_MESH): a Power Macintosh 7500/8500/9500's
//     internal bus, Grand Central's cell at $F3018000.  The non-data phases go
//     through its 16-byte FIFO, the data phases through DBDMA channel 10, as
//     Linux's and MkLinux's drivers do it.
//
// Either way a command is select with ATN, an IDENTIFY without disconnect
// privilege, the command, the data, status and message in: one command at a
// time, nothing disconnects.
#include "../patches/src/loader-svc.h"

#ifndef SCSI_WORK
#error "the includer defines SCSI_WORK"
#endif

static unsigned scsi_chip; // 0 = none

static inline void scsi_eieio(void) {
    __asm__ volatile("eieio" ::: "memory");
}

static void scsi_flush(uint32_t a, uint32_t n) {
    for (uint32_t p = a & ~31u; p < a + n; p += 32)
        __asm__ volatile("dcbf 0,%0" ::"r"(p) : "memory");
    __asm__ volatile("sync" ::: "memory");
}

// Time, from the time base: the bus clock / 4.  Every machine here has a
// 40-50 MHz bus; the slower rate is taken, so a time-out is never short.
#define TB_PER_MS 10000u
static uint32_t scsi_tb(void) {
    uint32_t t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}
static int scsi_expired(uint32_t t0, uint32_t ms) {
    return scsi_tb() - t0 > ms * TB_PER_MS;
}
static void scsi_delay_us(uint32_t us) {
    uint32_t t0 = scsi_tb();
    while (scsi_tb() - t0 < us * (TB_PER_MS / 1000u) + 1u)
        ;
}

// --- the 53C825A ---------------------------------------------------------------

#define NCR(o)       (SCSI_MEM_BAR + (o))
#define NCR_SCNTL0   0x00
#define NCR_SCID     0x04
#define NCR_DSTAT    0x0C
#define NCR_ISTAT    0x14
#define NCR_DSP      0x2C
#define NCR_DSPS     0x30
#define NCR_DMODE    0x38
#define NCR_DIEN     0x39
#define NCR_SIEN0    0x40
#define NCR_SIEN1    0x41
#define NCR_SIST0    0x42
#define NCR_SIST1    0x43
#define NCR_STIME0   0x48
#define NCR_RESPID0  0x4A
#define NCR_RESPID1  0x4B

// The work area: the script, its message, command and status bytes.
#define NCR_SCRIPT   (SCSI_WORK + 0x000)
#define NCR_MSGOUT   (SCSI_WORK + 0x100)
#define NCR_CDB      (SCSI_WORK + 0x110)
#define NCR_STATUS   (SCSI_WORK + 0x120)
#define NCR_MSGIN    (SCSI_WORK + 0x124)

static uint8_t ncr_rd(uint32_t o) {
    uint8_t v = *(volatile uint8_t *)NCR(o);
    scsi_eieio();
    return v;
}
static void ncr_wr(uint32_t o, uint8_t v) {
    *(volatile uint8_t *)NCR(o) = v;
    scsi_eieio();
}

static void ncr_init(void) {
    ncr_wr(NCR_ISTAT, 0x40); // software reset
    scsi_delay_us(100);
    ncr_wr(NCR_ISTAT, 0x00);
    ncr_wr(NCR_SCNTL0, 0xC0);  // full arbitration
    ncr_wr(NCR_SCID, 0x47);    // ID 7, respond to reselection
    ncr_wr(NCR_RESPID0, 0x80);
    ncr_wr(NCR_RESPID1, 0x00);
    ncr_wr(NCR_STIME0, 0x0C);  // selection time-out about 100 ms
    ncr_wr(NCR_DMODE, 0x00);
    ncr_wr(NCR_DIEN, 0x7D);    // every DMA condition halts and interrupts
    ncr_wr(NCR_SIEN0, 0x8F);   // phase mismatch, gross error, unexpected disconnect, reset, parity
    ncr_wr(NCR_SIEN1, 0x04);   // selection time-out
}

static int ncr_cmd(int id, const uint8_t *cdb, unsigned cdblen, uint32_t buf, uint32_t len, int out) {
    uint32_t *s = (uint32_t *)NCR_SCRIPT;
    const uint32_t at = NCR_SCRIPT;
    s[0] = 0x41000000u | ((uint32_t)id << 16); // SELECT ATN id; alternate (reselected): INT 2
    s[1] = at + 4 * 20;
    s[2] = 0x0E000001u; // MOVE 1, msgout, WHEN MSG_OUT
    s[3] = NCR_MSGOUT;
    s[4] = 0x0A000000u | cdblen; // MOVE cdblen, cdb, WHEN CMD
    s[5] = NCR_CDB;
    s[6] = len ? 0x830B0000u : 0x80080000u; // JUMP status, WHEN STATUS (always if no data)
    s[7] = at + 4 * 10;
    s[8] = (out ? 0x08000000u : 0x09000000u) | len; // MOVE len, buf, WHEN DATA_OUT / DATA_IN
    s[9] = buf;
    s[10] = 0x0B000001u; // MOVE 1, status, WHEN STATUS
    s[11] = NCR_STATUS;
    s[12] = 0x0F000001u; // MOVE 1, msgin, WHEN MSG_IN
    s[13] = NCR_MSGIN;
    s[14] = 0x60000040u; // CLEAR ACK
    s[15] = 0;
    s[16] = 0x48000000u; // WAIT DISCONNECT
    s[17] = 0;
    s[18] = 0x98080000u; // INT 1: done
    s[19] = 1;
    s[20] = 0x98080000u; // INT 2: reselected (never: no disconnect privilege)
    s[21] = 2;
    *(volatile uint8_t *)NCR_MSGOUT = 0x80; // IDENTIFY, LUN 0, no disconnect
    for (unsigned i = 0; i < cdblen; i++)
        ((volatile uint8_t *)NCR_CDB)[i] = cdb[i];
    *(volatile uint8_t *)NCR_STATUS = 0xFF;
    scsi_flush(SCSI_WORK, 0x200);
    if (len)
        scsi_flush(buf, len);

    *(volatile uint32_t *)NCR(NCR_DSP) = at;
    scsi_eieio();
    uint32_t t0 = scsi_tb();
    while (!scsi_expired(t0, 20000)) {
        uint8_t istat = ncr_rd(NCR_ISTAT);
        if (!(istat & 0x03))
            continue;
        uint8_t dstat = 0, sist0 = 0, sist1 = 0;
        if (istat & 0x01)
            dstat = ncr_rd(NCR_DSTAT);
        if (istat & 0x02) {
            sist0 = ncr_rd(NCR_SIST0);
            sist1 = ncr_rd(NCR_SIST1);
        }
        if (len)
            scsi_flush(buf, len);
        scsi_flush(SCSI_WORK, 0x200);
        if (sist1 & 0x04)
            return -1; // selection time-out: no device
        if (sist0)
            return -2; // phase mismatch, unexpected disconnect, gross error, reset, parity
        if ((dstat & 0x04) && *(volatile uint32_t *)NCR(NCR_DSPS) == 1)
            return *(volatile uint8_t *)NCR_STATUS;
        return -2;
    }
    ncr_init(); // a script that never finished: stop it
    return -2;
}

// --- MESH ------------------------------------------------------------------------

#define MESH_BASE    0xF3018000u
#define MESH_DMA     0xF3008A00u // DBDMA channel 10
#define MR_COUNT_LO  0x0
#define MR_COUNT_HI  0x1
#define MR_FIFO      0x2
#define MR_SEQUENCE  0x3
#define MR_BUS0      0x4
#define MR_BUS1      0x5
#define MR_FIFO_CNT  0x6
#define MR_EXCEPTION 0x7
#define MR_ERROR     0x8
#define MR_INTR_MASK 0x9
#define MR_INTERRUPT 0xA
#define MR_SOURCE_ID 0xB
#define MR_DEST_ID   0xC
#define MR_SYNC      0xD
#define MR_SEL_TO    0xF

#define SEQ_DMA       0x80
#define SEQ_ATN       0x20
#define SEQ_ARBITRATE 0x1
#define SEQ_SELECT    0x2
#define SEQ_COMMAND   0x3
#define SEQ_STATUS    0x4
#define SEQ_DATAOUT   0x5
#define SEQ_DATAIN    0x6
#define SEQ_MSGOUT    0x7
#define SEQ_MSGIN     0x8
#define SEQ_BUSFREE   0x9
#define SEQ_DISRESEL  0xD
#define SEQ_RESETMESH 0xE
#define SEQ_FLUSHFIFO 0xF

#define BS0_REQ      0x20
#define BS1_RST      0x80
#define INT_CMDDONE  0x01
#define EXC_PHASEMM  0x02
#define EXC_SELTO    0x01

// DBDMA: the channel's registers, and its descriptors' commands.
#define DMA_CONTROL  0x00
#define DMA_STATUS   0x04
#define DMA_CMDPTR   0x0C
#define DMA_RUN      0x8000u
#define DMA_ACTIVE   0x0400u
#define DB_OUTPUT_LAST 0x1000u
#define DB_INPUT_LAST  0x3000u
#define DB_STOP        0x7000u

#define MESH_DESC    (SCSI_WORK + 0x000) // two descriptors: the transfer, STOP
#define MESH_CHUNK   0xF000u             // bytes per data sequence (the count is 16 bits)

static uint8_t mr_rd(unsigned r) {
    uint8_t v = *(volatile uint8_t *)(MESH_BASE + 16u * r);
    scsi_eieio();
    return v;
}
static void mr_wr(unsigned r, uint8_t v) {
    *(volatile uint8_t *)(MESH_BASE + 16u * r) = v;
    scsi_eieio();
}
static void mr_count(uint32_t n) {
    mr_wr(MR_COUNT_LO, (uint8_t)n);
    mr_wr(MR_COUNT_HI, (uint8_t)(n >> 8));
}
static void dma_wr(uint32_t o, uint32_t v) {
    *(volatile uint32_t *)(MESH_DMA + o) = v;
    scsi_eieio();
}
static uint32_t dma_rd(uint32_t o) {
    uint32_t v = *(volatile uint32_t *)(MESH_DMA + o);
    scsi_eieio();
    return v;
}
static void dma_stop(void) {
    dma_wr(DMA_CONTROL, 0xF000u << 16); // RUN, PAUSE, FLUSH and WAKE all clear
    uint32_t t0 = scsi_tb();
    while ((dma_rd(DMA_STATUS) & DMA_ACTIVE) && !scsi_expired(t0, 10))
        ;
}

// The next completion: interrupt | exception << 8 | error << 16, all three
// then cleared; 0 when nothing completed in `ms`.
static uint32_t mesh_wait(uint32_t ms) {
    uint32_t t0 = scsi_tb();
    for (;;) {
        uint8_t i = mr_rd(MR_INTERRUPT);
        if (i & 7) {
            uint32_t r = i | (uint32_t)mr_rd(MR_EXCEPTION) << 8 | (uint32_t)mr_rd(MR_ERROR) << 16;
            mr_wr(MR_EXCEPTION, 0xFF);
            mr_wr(MR_ERROR, 0xFF);
            mr_wr(MR_INTERRUPT, 0x07);
            return r;
        }
        if (scsi_expired(t0, ms))
            return 0;
    }
}
static uint32_t mesh_seq(uint8_t seq, uint32_t ms) {
    mr_wr(MR_SEQUENCE, seq);
    return mesh_wait(ms);
}
// A completion with nothing else: the command did what it was asked.
static int mesh_clean(uint32_t r) {
    return r == INT_CMDDONE;
}

static void mesh_init(void) {
    dma_stop();
    mr_wr(MR_EXCEPTION, 0xFF);
    mr_wr(MR_ERROR, 0xFF);
    mr_wr(MR_INTR_MASK, 0x00); // polled: nothing reaches Grand Central
    mesh_seq(SEQ_RESETMESH, 10);
    mr_wr(MR_SOURCE_ID, 7);
    mr_wr(MR_SEL_TO, 25); // 250 ms
    mr_wr(MR_SYNC, 2);    // asynchronous
    mr_wr(MR_INTERRUPT, 0xFF);
    mr_wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
}

// Something went wrong mid-command: reset the bus (a target left in any phase
// lets go of it) and the cell, and give the targets their reset recovery.
static int mesh_fail(void) {
    dma_stop();
    mr_wr(MR_BUS1, BS1_RST);
    scsi_delay_us(30);
    mr_wr(MR_BUS1, 0);
    mesh_init();
    scsi_delay_us(250000);
    return -2;
}

static int mesh_wait_req(uint32_t ms) {
    uint32_t t0 = scsi_tb();
    while (!(mr_rd(MR_BUS0) & BS0_REQ))
        if (scsi_expired(t0, ms))
            return 0;
    return 1;
}

static int mesh_cmd(int id, const uint8_t *cdb, unsigned cdblen, uint32_t buf, uint32_t len, int out) {
    // Arbitrate with our own ID in dest_id (MESH puts both IDs on the bus
    // while it arbitrates) and reselection off (the chip can hang in
    // ARBITRATE on a reselection race): both from Linux's mesh.c.
    mr_wr(MR_INTERRUPT, 0x07);
    mr_wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
    mr_wr(MR_DEST_ID, 7);
    mr_wr(MR_SEQUENCE, SEQ_DISRESEL);
    if (!mesh_clean(mesh_seq(SEQ_ARBITRATE, 10)))
        return mesh_fail();
    mr_wr(MR_DEST_ID, (uint8_t)id);
    uint32_t r = mesh_seq(SEQ_SELECT | SEQ_ATN, 1000);
    if (r & (EXC_SELTO << 8)) {
        mr_wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
        return -1; // nothing at this ID
    }
    if (!mesh_clean(r))
        return mesh_fail();

    // IDENTIFY, LUN 0, no disconnect: the message's last (only) byte, so ATN
    // drops for it -- MSGOUT without SEQ_ATN, once the target asks.
    if (!mesh_wait_req(1000))
        return mesh_fail();
    mr_count(1);
    mr_wr(MR_SEQUENCE, SEQ_MSGOUT);
    mr_wr(MR_FIFO, 0x80);
    if (!mesh_clean(mesh_wait(1000)))
        return mesh_fail();

    mr_count(cdblen);
    mr_wr(MR_SEQUENCE, SEQ_COMMAND);
    for (unsigned i = 0; i < cdblen; i++)
        mr_wr(MR_FIFO, cdb[i]);
    if (!mesh_clean(mesh_wait(1000)))
        return mesh_fail();

    // What the target does next: the data, or (an error) straight to status.
    if (!mesh_wait_req(20000))
        return mesh_fail();
    uint8_t phase = mr_rd(MR_BUS0) & 7;
    if (len && phase == (out ? 0 : 1)) {
        uint32_t *d = (uint32_t *)MESH_DESC;
        scsi_flush(buf, len);
        for (uint32_t done = 0; done < len;) {
            uint32_t n = len - done > MESH_CHUNK ? MESH_CHUNK : len - done;
            d[0] = (out ? DB_OUTPUT_LAST : DB_INPUT_LAST) << 16 | n;
            d[1] = buf + done;
            d[2] = 0;
            d[3] = 0;
            d[4] = DB_STOP << 16;
            d[5] = d[6] = d[7] = 0;
            scsi_flush(MESH_DESC, 32);
            dma_stop();
            dma_wr(DMA_CMDPTR, MESH_DESC);
            dma_wr(DMA_CONTROL, DMA_RUN << 16 | DMA_RUN);
            mr_count(n);
            r = mesh_seq((out ? SEQ_DATAOUT : SEQ_DATAIN) | SEQ_DMA, 20000);
            dma_stop();
            if (r & (EXC_PHASEMM << 8))
                break; // the target moved on early: a short transfer
            if (!mesh_clean(r))
                return mesh_fail();
            done += n;
        }
        scsi_flush(buf, len);
    }

    mr_count(1);
    if (!mesh_clean(mesh_seq(SEQ_STATUS, 20000)) || !mr_rd(MR_FIFO_CNT))
        return mesh_fail();
    uint8_t status = mr_rd(MR_FIFO);
    mr_count(1);
    if (!mesh_clean(mesh_seq(SEQ_MSGIN, 1000)))
        return mesh_fail();
    while (mr_rd(MR_FIFO_CNT))
        (void)mr_rd(MR_FIFO); // COMMAND COMPLETE
    if (!mesh_clean(mesh_seq(SEQ_BUSFREE, 1000)))
        return mesh_fail();
    return status;
}

// --- the interface ------------------------------------------------------------------

static void scsi_init(void) {
    if (scsi_chip == SCSI_CHIP_MESH)
        mesh_init();
    else if (scsi_chip)
        ncr_init();
}

// One command: the SCSI status byte (0 good, 2 check condition), or -1 when
// nothing answers at that ID, -2 on any other failure.  `out` sends the data.
static int scsi_cmd(int id, const uint8_t *cdb, unsigned cdblen, uint32_t buf, uint32_t len, int out) {
    if (scsi_chip == SCSI_CHIP_MESH)
        return mesh_cmd(id, cdb, cdblen, buf, len, out);
    if (scsi_chip)
        return ncr_cmd(id, cdb, cdblen, buf, len, out);
    return -1;
}

#define SCSI_SENSE (SCSI_WORK + 0x200) // 32 bytes: sense data, capacity, inquiry

// A command with the retries a fresh target needs (unit attention after
// reset, a CD spinning up): the status, or a negative error.
static int scsi_do(int id, const uint8_t *cdb, unsigned cdblen, uint32_t buf, uint32_t len, int out) {
    int st = -2;
    for (int tries = 0; tries < 8; tries++) {
        st = scsi_cmd(id, cdb, cdblen, buf, len, out);
        if (st == 0 || st == -1)
            return st;
        if (st == 2) {
            static const uint8_t sense[6] = {0x03, 0, 0, 0, 18, 0};
            scsi_cmd(id, sense, 6, SCSI_SENSE, 18, 0);
            // Not ready, becoming ready (a CD spinning up): give it time.
            const volatile uint8_t *s = (const volatile uint8_t *)SCSI_SENSE;
            if ((s[2] & 0x0F) == 2)
                scsi_delay_us(500000);
        }
    }
    return st;
}

static uint32_t scsi_be32(const volatile uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

// READ CAPACITY: the last block and the block size.  0 on success.
static int scsi_capacity(int id, uint32_t *last, uint32_t *bs) {
    static const uint8_t cap[10] = {0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    if (scsi_do(id, cap, 10, SCSI_SENSE, 8, 0) != 0)
        return -1;
    const volatile uint8_t *c = (const volatile uint8_t *)SCSI_SENSE;
    *last = scsi_be32(c);
    *bs = scsi_be32(c + 4);
    return 0;
}

// READ(10) / WRITE(10) of whole blocks, straight to or from `buf`.  0 on success.
static int scsi_rw10(int id, uint32_t lba, uint32_t nblk, uint32_t bs, uint32_t buf, int out) {
    while (nblk) {
        uint32_t n = nblk;
        if (n * bs > 0x10000u)
            n = 0x10000u / bs;
        uint8_t c[10] = {out ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8),
                         (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n, 0};
        if (scsi_do(id, c, 10, buf, n * bs, out) != 0)
            return -1;
        lba += n;
        nblk -= n;
        buf += n * bs;
    }
    return 0;
}
