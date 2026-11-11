/* SPDX-License-Identifier: GPL-2.0-only */
/* dd-scsi-mesh.c -- OS/2's SCSI driver on a Power Macintosh's MESH.
 *
 * A Power Macintosh 7500/8500/9500 without a SCSI card has no 53C825A for
 * the NCR driver: its internal bus is MESH, Grand Central's own cell.  The
 * configuration trap then presents a stand-in 53C810 (kernel-trap.c), so that
 * the hardware database binds this driver as everywhere else, and its BARs
 * lead to MESH and DBDMA channel 10 (kernel-trap.c's mesh_reg).  The driver's generic
 * layer, its per-target queues and its command blocks stay as they are.
 * Only the parts that touch the 53C810 change, and only when the chip
 * behind the stand-in is MESH:
 *
 *   ncr_chip_reset_chip,     the chip's and the bus's reset: MESH's (and a
 *   ncr_chip_reset_bus       SCSI bus reset), instead of the 53C810's
 *                            registers and its SCRIPTS program;
 *   ncr_chip_start_810,      starting SCRIPTS: nothing to start;
 *   ncr_chip_initiate_io
 *   ncr_execute_scb          a command block for a target (called with the
 *                            driver's locks held): handed to the worker;
 *   ncr_intr_thread          the interrupt thread: the worker instead.
 *
 * The worker runs one command at a time, start to finish, polled: select
 * with ATN, IDENTIFY without disconnect privilege, the command, the data by
 * DBDMA, status and message in (shim/scsi.c does the same before OS/2).
 * Then it finishes the command block exactly as the interrupt thread does
 * for a SCRIPTS completion: off the target's queue, the target idle again,
 * its next command started, and the generic state machine told -- event 16
 * (done), 192 (done with a SCSI status for it to look at: the status byte
 * is where ncr_get_tsb_info reads it) or, for a target that never answers,
 * 224 (failed), as after a selection time-out.  MESH's interrupt stays
 * masked, so the driver's interrupt routine never runs.
 *
 * The code is in the AHA-1542 layer (adscsi), which only an ISA adapter
 * selects, from ad_do_mbi on; its state in that layer's controller block.
 */
/* cflags: -Os */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

/* Everything in the one place (static helpers the compiler keeps out of line too). */
#pragma clang section text = ".patch.0x2016B48"
#define CODE __attribute__((noinline, used))

/* The driver's own routines and its imports (objpatch: symbols, PLT_ slots). */
extern u32 PLT_foo_vmRegRead8(u32 h, u32 off);
extern void PLT_foo_vmRegWrite8(u32 h, u32 off, u32 v);
extern u32 PLT_foo_vmRegRead32(u32 h, u32 off);
extern void PLT_foo_vmRegWrite32(u32 h, u32 off, u32 v);
extern void PLT_mutex_lock_solid(void *m);
extern void PLT_mutex_unlock_solid(void *m);
extern void PLT_condition_wait(void *c, void *m);
extern void PLT_condition_signal(void *c);
extern void *PLT_calloc(u32 n, u32 size);
extern u32 PLT_thread_self(void);
extern void PLT_cthread_wire(void);
extern void PLT_dd_set_thread_scheduling(u32 task, u32 thread, u32 pri);
extern void PLT_cthread_yield(void);
extern int PLT_Printf(const char *fmt, ...);
extern u32 wiring_alloc(u32 size);
extern u32 wiring_v2p(u32 va, u32 vmspace);
extern int ncr_queue_remove(void *q, void **out);
extern void ncr_set_dev_state(u32 ctlr, u32 t, u32 state);
extern void ncr_start_io(u32 ctlr, u32 t);
extern void ncr_fail_queued_io(u32 ctlr, u32 t, u32 event);
extern void scsi_statem(u32 ctlr, u32 t, u32 lun, u32 event);

/* The driver's data. */
#define CSTATE(c)     (0x02039568u + 280u * (c))  /* the generic per-controller state */
#define NCR_GLOBALS   (*(u8 **)0x020359C0u)        /* the NCR layer's: +0 mutex, +4 state, 32 per target */
#define TGT(t)        (NCR_GLOBALS + 32u * (t))    /* +28 mutex, +32 state, +40 queue, +52 status block */
#define INTR_START    ((u32 *)0x02039538u)         /* the interrupt thread's start: mutex, condition, thread */
#define GOT(off)      (*(u32 *)(0x0202392Cu + (off)))
#define TASK_SELF     (*(u32 *)GOT(0x34))          /* _mk_task_self_ */
#define VMSPACE_SELF  (*(u32 *)GOT(0x100))         /* _mk_vmspace_self_ */

/* A command block (ncr_scb_init, ncr_build_*, ncr_execute_scb). */
struct scb {
    u32 link[2];
    u32 flags;           /* bit 0: a scatter list */
    u32 target, lun;
    u8 msg[8];
    u32 msg_pa, msg_len;
    u8 cdb[12];
    u32 cdb_pa, cdb_len;
    u32 *sg;             /* {physical address, byte count} pairs */
    u32 sg_n;
    u32 len, pa;         /* the transfer without a scatter list */
    u32 resid;
};

/* This layer's state, in the AHA-1542 layer's controller block (.bss). */
struct mesh {
    u32 mode;            /* 0 not yet known, 1 MESH, 2 a real 53C810 */
    u32 ctlr;
    u32 regs;            /* the register handle: MESH, and DBDMA channel 10 at +4, +8, +$C */
    void *mtx, *cond;
    u32 pending;         /* targets with a command block waiting */
    struct scb *scb[8];
    u32 *desc;           /* DBDMA descriptors: a wired page */
    u32 desc_pa;
    u32 seen;            /* the chip has been reset (and announced) */
};
#define M ((struct mesh *)0x02038000u)

/* MESH (Linux's mesh.h) and DBDMA. */
#define MR_COUNT_LO  0x00
#define MR_COUNT_HI  0x10
#define MR_FIFO      0x20
#define MR_SEQUENCE  0x30
#define MR_BUS0      0x40
#define MR_BUS1      0x50
#define MR_FIFO_CNT  0x60
#define MR_EXCEPTION 0x70
#define MR_ERROR     0x80
#define MR_INTR_MASK 0x90
#define MR_INTERRUPT 0xA0
#define MR_SOURCE_ID 0xB0
#define MR_DEST_ID   0xC0
#define MR_SYNC      0xD0
#define MR_MESH_ID   0xE0
#define MR_SEL_TO    0xF0
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
#define BS0_REQ       0x20
#define BS1_RST       0x80
#define INT_CMDDONE   0x01
#define EXC_PHASEMM   0x02
#define EXC_SELTO     0x01
/* DBDMA channel 10, between MESH's registers (kernel-trap.c's mesh_reg). */
#define DMA_CONTROL   0x04
#define DMA_STATUS    0x08
#define DMA_CMDPTR    0x0C
#define DMA_RUN       0x8000u
#define DMA_ACTIVE    0x0400u
#define DB_OUTPUT_MORE 0x0000u
#define DB_INPUT_MORE  0x2000u
#define DB_STOP        0x7000u
#define DB_LAST        0x1000u   /* OUTPUT_LAST / INPUT_LAST */
#define MESH_CHUNK     0xF000u   /* bytes per descriptor and sequence: the counts are 16 bits */
#define MAX_DESC       250       /* a page of descriptors, less the STOP */

__attribute__((section(".patch.0x2018600"), used))
static const char found_msg[] = "dd_scsi: the internal bus, MESH (ID $%x)\n";

static inline u32 rd(u32 r) { return PLT_foo_vmRegRead8(M->regs, r); }
static inline void wr(u32 r, u32 v) { PLT_foo_vmRegWrite8(M->regs, r, v); }

/* Time from the time base (bus clock / 4; 10 ticks a microsecond is the
 * slowest of these machines', so a wait is never short). */
static inline u32 tb(void)
{
    u32 t;
    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}
CODE static void delay_us(u32 us)
{
    u32 t0 = tb();
    while (tb() - t0 < us * 10u)
        ;
}

CODE static void dma_stop(void)
{
    PLT_foo_vmRegWrite32(M->regs, DMA_CONTROL, 0xF0000000u);   /* RUN PAUSE FLUSH WAKE clear */
    u32 t0 = tb();
    while ((PLT_foo_vmRegRead32(M->regs, DMA_STATUS) & DMA_ACTIVE) && tb() - t0 < 100000u)
        ;
}

/* The next completion -- interrupt | exception << 8 | error << 16, all three
 * cleared -- or 0 after `ms`. */
CODE static u32 wait_int(u32 ms)
{
    u32 t0 = tb(), spins = 0;
    for (;;) {
        u32 i = rd(MR_INTERRUPT);
        if (i & 7) {
            u32 r = i | rd(MR_EXCEPTION) << 8 | rd(MR_ERROR) << 16;
            wr(MR_EXCEPTION, 0xFF);
            wr(MR_ERROR, 0xFF);
            wr(MR_INTERRUPT, 0x07);
            return r;
        }
        if (tb() - t0 > ms * 10000u)
            return 0;
        if (++spins > 1000)            /* a long wait (a CD spinning up): let others run */
            PLT_cthread_yield();
    }
}
CODE static u32 seq(u32 s, u32 ms)
{
    wr(MR_SEQUENCE, s);
    return wait_int(ms);
}
static inline void count(u32 n)
{
    wr(MR_COUNT_LO, n & 0xFF);
    wr(MR_COUNT_HI, (n >> 8) & 0xFF);
}
CODE static int wait_req(u32 ms)
{
    u32 t0 = tb();
    while (!(rd(MR_BUS0) & BS0_REQ))
        if (tb() - t0 > ms * 10000u)
            return 0;
    return 1;
}

CODE static void mesh_init(void)
{
    dma_stop();
    wr(MR_EXCEPTION, 0xFF);
    wr(MR_ERROR, 0xFF);
    wr(MR_INTR_MASK, 0);               /* polled: MESH never interrupts */
    seq(SEQ_RESETMESH, 10);
    wr(MR_SOURCE_ID, 7);
    wr(MR_SEL_TO, 25);                 /* 250 ms */
    wr(MR_SYNC, 2);                    /* asynchronous */
    wr(MR_INTERRUPT, 0xFF);
    wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
}

/* A SCSI bus reset, the cell reset, and the targets' reset recovery.  The
 * driver's first reset of the chip is the first access to it. */
CODE static void mesh_reset_bus(void)
{
    if (!M->seen) {
        M->seen = 1;
        PLT_Printf(found_msg, rd(MR_MESH_ID));
    }
    dma_stop();
    wr(MR_BUS1, BS1_RST);
    delay_us(30);
    wr(MR_BUS1, 0);
    mesh_init();
    delay_us(250000);
}

/* Is the chip behind the stand-in 53C810 MESH?  The stand-in's I/O range,
 * the first (and only) one the controller has, is PReP $80FFF000; a real
 * 53C810's, or a 53C825A's, is not.  The range's record, which is also the
 * register handle: +$14 its PReP address.  Nothing here touches the chip:
 * the interrupt thread, the first to ask, starts before the controller's
 * registers are mapped. */
CODE static int is_mesh(u32 ctlr)
{
    if (!M->mode) {
        u32 r = *(u32 *)(CSTATE(ctlr) + 4);
        if (!r)
            return 0;                  /* no I/O range yet: not the chip's business */
        u32 h = *(u32 *)(r + 4);
        M->regs = h;
        M->ctlr = ctlr;
        M->mode = *(u32 *)(h + 0x14) == 0x80FFF000u ? 1 : 2;
        if (M->mode == 1) {
            /* One whole page, so that the table is physically contiguous. */
            u32 va = wiring_alloc(8192);
            va = (va + 4095) & ~4095u;
            M->desc = (u32 *)va;
            M->desc_pa = wiring_v2p(va, VMSPACE_SELF);
            M->mtx = PLT_calloc(1, 16);
            M->cond = PLT_calloc(1, 12);
        }
    }
    return M->mode == 1;
}

/* One command block, start to finish: the SCSI status byte, -1 for no
 * device, -2 for any other failure (the bus is reset). */
CODE static int mesh_fail(void)
{
    mesh_reset_bus();
    return -2;
}
CODE static int run(struct scb *s)
{
    wr(MR_INTERRUPT, 0x07);
    wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
    wr(MR_DEST_ID, 7);                 /* MESH puts both IDs on the bus while it arbitrates */
    wr(MR_SEQUENCE, SEQ_DISRESEL);     /* and can hang in ARBITRATE on a reselection race */
    if (seq(SEQ_ARBITRATE, 10) != INT_CMDDONE)
        return mesh_fail();
    wr(MR_DEST_ID, s->target);
    u32 r = seq(SEQ_SELECT | SEQ_ATN, 1000);
    if (r & (EXC_SELTO << 8)) {
        wr(MR_SEQUENCE, SEQ_FLUSHFIFO);
        return -1;
    }
    if (r != INT_CMDDONE)
        return mesh_fail();

    /* IDENTIFY, no disconnect privilege: the last byte of the message, so
     * ATN drops for it (MSGOUT without SEQ_ATN, once the target asks). */
    if (!wait_req(1000))
        return mesh_fail();
    count(1);
    wr(MR_SEQUENCE, SEQ_MSGOUT);
    wr(MR_FIFO, 0x80 | (s->lun & 7));
    if (wait_int(1000) != INT_CMDDONE)
        return mesh_fail();

    count(s->cdb_len);
    wr(MR_SEQUENCE, SEQ_COMMAND);
    for (u32 i = 0; i < s->cdb_len; i++)
        wr(MR_FIFO, s->cdb[i]);
    if (wait_int(1000) != INT_CMDDONE)
        return mesh_fail();

    /* The data, if the target asks for it: the whole transfer as one DBDMA
     * program, which MESH feeds one descriptor per sequence. */
    if (!wait_req(30000))
        return mesh_fail();
    u32 phase = rd(MR_BUS0) & 7, n = 0;
    if (phase <= 1) {
        u32 out = phase == 0, *d = M->desc;
        const u32 *sg = s->flags & 1 ? s->sg : &s->pa;
        u32 nsg = s->flags & 1 ? s->sg_n : 1;
        for (u32 i = 0; i < nsg; i++) {
            u32 pa = s->flags & 1 ? sg[2 * i] : s->pa;
            u32 len = (s->flags & 1 ? sg[2 * i + 1] : s->len) & 0xFFFFFF;   /* a table-indirect count */
            while (len) {              /* a descriptor counts 16 bits */
                if (n == MAX_DESC)
                    return mesh_fail();
                u32 c = len > MESH_CHUNK ? MESH_CHUNK : len;
                d[4 * n] = (out ? DB_OUTPUT_MORE : DB_INPUT_MORE) << 16 | c;
                d[4 * n + 1] = pa;
                d[4 * n + 2] = 0;
                d[4 * n + 3] = 0;
                pa += c;
                len -= c;
                n++;
            }
        }
        if (n) {
            d[4 * (n - 1)] |= DB_LAST << 16;
            d[4 * n] = DB_STOP << 16;
            d[4 * n + 1] = d[4 * n + 2] = d[4 * n + 3] = 0;
            __asm__ volatile("sync" ::: "memory");
            dma_stop();
            PLT_foo_vmRegWrite32(M->regs, DMA_CMDPTR, M->desc_pa);
            PLT_foo_vmRegWrite32(M->regs, DMA_CONTROL, DMA_RUN << 16 | DMA_RUN);
            for (u32 k = 0; k < n; k++) {   /* a sequence a descriptor: DMA never waits mid-way */
                count(d[4 * k] & 0xFFFF);
                r = seq((out ? SEQ_DATAOUT : SEQ_DATAIN) | SEQ_DMA, 30000);
                if (r & (EXC_PHASEMM << 8))
                    break;             /* the target moved on: a short transfer */
                if (r != INT_CMDDONE) {
                    dma_stop();
                    return mesh_fail();
                }
            }
            dma_stop();
        }
    }

    count(1);
    if (seq(SEQ_STATUS, 30000) != INT_CMDDONE || !rd(MR_FIFO_CNT))
        return mesh_fail();
    u32 status = rd(MR_FIFO);
    count(1);
    if (seq(SEQ_MSGIN, 1000) != INT_CMDDONE)
        return mesh_fail();
    while (rd(MR_FIFO_CNT))
        (void)rd(MR_FIFO);             /* COMMAND COMPLETE */
    if (seq(SEQ_BUSFREE, 1000) != INT_CMDDONE)
        return mesh_fail();
    return (int)status;
}

/* The command block of target t is done: as ncr_intr_thread finishes a
 * SCRIPTS completion (or, for no device, a selection time-out).  A command
 * that failed on the way (the bus was reset) is reported busy, for the
 * generic layer to try again. */
CODE static void finish(u32 t, int st)
{
    if (st == -2)
        st = 0x08;                     /* BUSY */
    u32 ctlr = M->ctlr;
    u8 *g = NCR_GLOBALS, *tg = TGT(t);
    struct scb *s;
    PLT_mutex_lock_solid(*(void **)g);
    PLT_mutex_lock_solid(*(void **)(tg + 28));
    if (*(u32 *)(g + 4) != 2 || *(u32 *)(tg + 32) != 4) {
        ncr_set_dev_state(ctlr, t, 1);
        PLT_mutex_unlock_solid(*(void **)(tg + 28));
        PLT_mutex_unlock_solid(*(void **)g);
        ncr_fail_queued_io(ctlr, t, 224);
        return;
    }
    ncr_queue_remove(tg + 40, (void **)&s);
    if (st < 0) {                      /* nothing at this ID */
        ncr_set_dev_state(ctlr, t, 2);
        PLT_mutex_unlock_solid(*(void **)(tg + 28));
        PLT_mutex_unlock_solid(*(void **)g);
        ncr_fail_queued_io(ctlr, t, 224);
        scsi_statem(ctlr, t, s->lun, 224);
        return;
    }
    **(u8 **)(tg + 52) = (u8)st;       /* the status byte ncr_get_tsb_info reads */
    ncr_set_dev_state(ctlr, t, 3);
    PLT_mutex_unlock_solid(*(void **)(tg + 28));
    PLT_mutex_unlock_solid(*(void **)g);
    ncr_start_io(ctlr, t);
    scsi_statem(ctlr, t, s->lun, st ? 192 : 16);
}

/* The worker, in place of the interrupt thread: started the same way. */
CODE static void worker(u32 ctlr)
{
    PLT_dd_set_thread_scheduling(TASK_SELF, PLT_thread_self(), 400);
    PLT_cthread_wire();
    u32 *st = INTR_START;
    PLT_mutex_lock_solid((void *)st[0]);
    st[2] = PLT_thread_self();
    PLT_condition_signal((void *)st[1]);
    PLT_mutex_unlock_solid((void *)st[0]);
    (void)ctlr;
    for (;;) {
        PLT_mutex_lock_solid(M->mtx);
        while (!M->pending)
            PLT_condition_wait(M->cond, M->mtx);
        u32 t = 0;
        while (!(M->pending & (1u << t)))
            t++;
        struct scb *s = M->scb[t];
        M->pending &= ~(1u << t);
        PLT_mutex_unlock_solid(M->mtx);
        finish(t, run(s));
    }
}

/* --- the hooks: each original's first instruction is `mflr 0` ------------ */

CODE void mesh_execute_scb(u32 ctlr, struct scb *s)
{
    PLT_mutex_lock_solid(M->mtx);
    M->scb[s->target & 7] = s;
    M->pending |= 1u << (s->target & 7);
    PLT_condition_signal(M->cond);
    PLT_mutex_unlock_solid(M->mtx);
    (void)ctlr;
}

#define HOOK(name, args, call, mesh_body)                                                  \
    extern u32 orig_##name args;                                                           \
    CODE u32 hook_##name args                                                              \
    {                                                                                      \
        if (!is_mesh(a))                                                                   \
            return orig_##name call;                                                       \
        mesh_body;                                                                         \
        return 0;                                                                          \
    }

HOOK(ncr_chip_reset_chip, (u32 a), (a), mesh_reset_bus())
HOOK(ncr_chip_reset_bus, (u32 a), (a), mesh_reset_bus())
HOOK(ncr_chip_start_810, (u32 a), (a), (void)0)
HOOK(ncr_chip_initiate_io, (u32 a, u32 b), (a, b), (void)0)
HOOK(ncr_execute_scb, (u32 a, u32 b), (a, b), mesh_execute_scb(a, (struct scb *)b))
HOOK(ncr_intr_thread, (u32 a), (a), worker(a))

/* Each original, entered past its first instruction (`mflr 0`), and the
 * branch at its entry to the hook. */
#define ORIG(name)                                                                         \
    "  .section .patch.0x2016B48, \"ax\"\n"                                            \
    "  .globl orig_" #name "\n"                                                            \
    "orig_" #name ":\n  mflr 0\n  b " #name "+4\n"                                         \
    "  .section .patch." #name ", \"ax\"\n  b hook_" #name "\n"
__asm__(ORIG(ncr_chip_reset_chip) ORIG(ncr_chip_reset_bus) ORIG(ncr_chip_start_810)
        ORIG(ncr_chip_initiate_io) ORIG(ncr_execute_scb) ORIG(ncr_intr_thread));
