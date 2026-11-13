// SPDX-License-Identifier: GPL-2.0-only
// control.c -- a Power Macintosh 7500/8500/9500's own display, for OS/2.
//
// #included by shim.c.  Without a PCI display card OS/2 draws on the board's
// Control (343S1154) behind the Chaos bridge.  The shim sets the mode, since
// nothing in OS/2 knows the chip: 640x480 in 8-bit colour, the 13" monitor's
// raster the ROM's own driver programs (as Linux's controlfb.c does, which
// also gives the per-depth values).
//
// OS/2 does not draw into Control's memory itself.  Chaos, unlike Bandit,
// passes the CPU's byte lanes straight through, so in the CPU's little-endian
// mode each aligned 8 bytes of an image would reach VRAM mirrored.  So OS/2
// draws into a shadow frame buffer in RAM, the megabyte at SHADOW_PA, past
// the end of the RAM the residual data gives it, and the kernel copies what changes to
// VRAM with the bytes put right (kernel-video.c).  The shim leaves it a
// description of the screen at the shadow's end (struct ctl_screen).
//
// Byte order, from the CPU in little-endian mode: Chaos does not reverse, so
// a 32-bit little-endian register at physical p is stwbrx / lwbrx at p ^ 4
// and a VRAM byte at physical p is a byte store at p ^ 7.  The RaDACal is in
// Grand Central, on Bandit, which does reverse: plain byte stores.

#define CHAOS_CFG_ADDR 0xF0800000u
#define CHAOS_CFG_DATA 0xF0C00000u
#define CONTROL_DEV    11
#define VCI_WINDOW     0x90000000u // OF's VCI memory window: the VRAM aperture
#define VCI_REGS       0x94000000u // and Control's registers, where OF puts them
#define RADACAL        0xF301B000u // Grand Central + $1B000
#define VIA_CUDA       0xF3016000u

#define SHADOW_PA      0x03F00000u // 1 MB: the shadow, its copy, and ctl_screen (the top of 64 MB)
#define SHADOW_SIZE    0x00100000u
struct ctl_screen {                // at SHADOW_PA + SHADOW_SIZE - 64
    uint32_t magic;                // CTL_MAGIC once the mode is set
    uint32_t vram;                 // physical address of pixel 0
    uint32_t pitch, width, height, depth;
    uint32_t check;                // ~CTL_MAGIC
};
#define CTL_MAGIC      0x314C5443u // "CTL1"
#define CTL_SCREEN     ((volatile struct ctl_screen *)(SHADOW_PA + SHADOW_SIZE - 64))

static void le_wr(uint32_t pa, uint32_t v) {
    __asm__ volatile("stwbrx %0,0,%1; eieio" ::"r"(v), "r"(pa ^ 4) : "memory");
}
static uint32_t le_rd(uint32_t pa) {
    uint32_t v;
    __asm__ volatile("lwbrx %0,0,%1; eieio" : "=r"(v) : "r"(pa ^ 4) : "memory");
    return v;
}
static void vram_wr8(uint32_t pa, uint8_t v) {
    *(volatile uint8_t *)(pa ^ 7) = v;
    eieio();
}
static uint8_t vram_rd8(uint32_t pa) {
    uint8_t v = *(volatile uint8_t *)(pa ^ 7);
    eieio();
    return v;
}
static void gc_wr8(uint32_t pa, uint8_t v) {
    *(volatile uint8_t *)pa = v;
    eieio();
}
static uint8_t gc_rd8(uint32_t pa) {
    uint8_t v = *(volatile uint8_t *)pa;
    eieio();
    return v;
}

static uint32_t chaos_cfg_rd(uint32_t reg) {
    le_wr(CHAOS_CFG_ADDR, (1u << CONTROL_DEV) | reg);
    return le_rd(CHAOS_CFG_DATA);
}
static void chaos_cfg_wr(uint32_t reg, uint32_t v) {
    le_wr(CHAOS_CFG_ADDR, (1u << CONTROL_DEV) | reg);
    le_wr(CHAOS_CFG_DATA, v);
}

// The shim's DBAT2 is the stub's $A0000000, which nothing uses before the
// loader: for the setting up, it is the VCI window instead.
static void map_vci(int on) {
    uint32_t u = on ? 0x90001FFFu : 0xA0001FFFu, l = on ? 0x9000002Au : 0xA0000032u;
    __asm__ volatile("isync; mtspr 540,%0; mtspr 541,%1; mtspr 540,%2; isync; sync" ::"r"(0), "r"(l), "r"(u)
                     : "memory");
}

// --- Cuda: the pixel clock is an I2C device behind it -----------------------
//
// One pseudo-command packet (the Cuda Firmware ERS; kernel-adb.c drives the
// same handshake): the bytes out through the VIA's shift register with TIP
// and ByteAck, then the reply in, discarded.
#define VIA_B      (VIA_CUDA + 0x0000u)
#define VIA_DIRB   (VIA_CUDA + 0x0400u)
#define VIA_SR     (VIA_CUDA + 0x1400u)
#define VIA_ACR    (VIA_CUDA + 0x1600u)
#define VIA_IFR    (VIA_CUDA + 0x1A00u)
#define VIA_IER    (VIA_CUDA + 0x1C00u)
#define TREQ       0x08
#define BYTEACK    0x10
#define TIP        0x20

static int cuda_wait(void) {
    for (uint32_t n = 0; n < 400000; n++)
        if (gc_rd8(VIA_IFR) & 0x04)
            return 1;
    return 0;
}

// A packet Cuda presents, read and dropped (kernel-adb.c's cuda_read).
static void cuda_read(void) {
    if (!cuda_wait())
        return;
    (void)gc_rd8(VIA_SR); // the attention byte
    gc_wr8(VIA_B, (gc_rd8(VIA_B) | BYTEACK) & ~TIP);
    for (int n = 0; n < 16 && cuda_wait(); n++) {
        (void)gc_rd8(VIA_SR);
        if (gc_rd8(VIA_B) & TREQ)
            break;
        gc_wr8(VIA_B, gc_rd8(VIA_B) ^ BYTEACK);
    }
    gc_wr8(VIA_B, gc_rd8(VIA_B) | TIP | BYTEACK);
    if (cuda_wait())
        (void)gc_rd8(VIA_SR); // the idle acknowledge
}

static void cuda_packet(const uint8_t *b, unsigned n) {
    gc_wr8(VIA_DIRB, (gc_rd8(VIA_DIRB) & ~TREQ) | BYTEACK | TIP);
    gc_wr8(VIA_B, gc_rd8(VIA_B) | TIP | BYTEACK);
    gc_wr8(VIA_IER, 0x7F);
    // Cuda may be presenting an auto-poll packet (TREQ low): take it first.
    for (int tries = 0; tries < 4 && !(gc_rd8(VIA_B) & TREQ); tries++) {
        gc_wr8(VIA_ACR, (gc_rd8(VIA_ACR) & ~0x1C) | 0x0C); // shift in
        cuda_read();
    }
    gc_wr8(VIA_ACR, (gc_rd8(VIA_ACR) & ~0x1C) | 0x1C); // shift out
    gc_wr8(VIA_SR, b[0]);
    gc_wr8(VIA_B, (gc_rd8(VIA_B) | BYTEACK) & ~TIP);
    for (unsigned i = 1; i < n && cuda_wait(); i++) {
        gc_wr8(VIA_SR, b[i]);
        gc_wr8(VIA_B, gc_rd8(VIA_B) ^ BYTEACK);
    }
    cuda_wait();
    gc_wr8(VIA_ACR, (gc_rd8(VIA_ACR) & ~0x1C) | 0x0C); // shift in
    (void)gc_rd8(VIA_SR);
    gc_wr8(VIA_B, gc_rd8(VIA_B) | TIP | BYTEACK);
    cuda_read(); // the reply
}

// --- the mode ------------------------------------------------------------------

#define CTL(r) (regs + 0x10u * (r))
enum { C_PIPED = 8, C_CTRL = 18, C_START = 19, C_PITCH = 20, C_VRAM_ATTR = 22, C_MODE = 23, C_RFRCNT = 24, C_INTR_ENA = 25 };

static void radacal(uint8_t reg, uint8_t v) {
    gc_wr8(RADACAL + 0x00, reg);
    gc_wr8(RADACAL + 0x20, v);
}

// 640x480, 8 bits a pixel: 1 when Control is there and set up.
static int control_setup(void) {
    map_vci(1);
    if (chaos_cfg_rd(0x00) != 0x0003106Bu) {
        map_vci(0);
        return 0;
    }
    // Open Firmware only assigns Control's BARs when it opens the display.
    uint32_t vram = chaos_cfg_rd(0x18) & ~0xFu, regs = chaos_cfg_rd(0x14) & ~0xFu;
    if (vram == 0 || vram == 0xFFFFFFF0u) {
        chaos_cfg_wr(0x18, VCI_WINDOW);
        vram = VCI_WINDOW;
    }
    if (regs == 0 || regs == 0xFFFFFFF0u) {
        chaos_cfg_wr(0x14, VCI_REGS);
        regs = VCI_REGS;
    }
    uint32_t fb = vram + 0x800000u; // the usable view of the aperture

    le_wr(CTL(C_CTRL), 0x400 | 0x3B); // display off
    static const uint8_t clk[3] = {14, 27, 2}; // 30.13 MHz (Linux's formula)
    for (int i = 0; i < 3; i++) {
        const uint8_t p[5] = {0x01, 0x22, 0x50, (uint8_t)(i + 1), clk[i]}; // PSEUDO, GET_SET_IIC
        cuda_packet(p, 5);
    }

    // The two 2 MB banks (controlfb.c's probe).
    le_wr(CTL(C_VRAM_ATTR), 0x31);
    vram_wr8(fb + 0x600000u, 0xB3);
    vram_wr8(fb + 0x600001u, 0x71);
    int bank2 = vram_rd8(fb + 0x600000u) == 0xB3 && vram_rd8(fb + 0x600001u) == 0x71;
    le_wr(CTL(C_VRAM_ATTR), 0x39);
    vram_wr8(fb, 0x5A);
    vram_wr8(fb + 1, 0xC7);
    int bank1 = vram_rd8(fb) == 0x5A && vram_rd8(fb + 1) == 0xC7;
    int big = bank1 && bank2;
    uint32_t attr = big ? 0x51 : bank1 ? 0x31 : 0x39;
    if (!bank1)
        fb += 0x600000u;

    radacal(0x20, big ? 0x20 : 0x10);
    radacal(0x21, bank1 ? 1 : 0);
    radacal(0x10, 0);
    radacal(0x11, 0);
    // vswin..hserr: the ROM's 640x480 raster, the pipe delay for 8 bits.
    static const uint16_t t[16] = {1045, 1042, 82, 43, 4, 1048, 1050, 0, 430, 393, 73, 31, 431, 16, 216, 400};
    for (int i = 0; i < 16; i++)
        le_wr(CTL(1 + i), i + 1 == C_PIPED ? 73u - (big ? 13u : 9u) : t[i]);
    le_wr(CTL(C_PITCH), 640);
    le_wr(CTL(C_MODE), big ? 3 : 2);
    le_wr(CTL(C_VRAM_ATTR), attr);
    le_wr(CTL(C_START), 0);
    le_wr(CTL(C_RFRCNT), 0x1E5);
    le_wr(CTL(C_INTR_ENA), 0);

    // A grey ramp until OS/2 loads its palette, and a black screen.
    gc_wr8(RADACAL + 0x00, 0);
    for (int i = 0; i < 256; i++)
        for (int c = 0; c < 3; c++)
            gc_wr8(RADACAL + 0x30, (uint8_t)i);
    uint32_t pix0 = fb + 16; // pixel 0 sits 16 bytes in (CTRLFB_OFF)
    for (uint32_t y = 0; y < 480; y++)
        for (uint32_t x = 0; x < 640; x += 4)
            le_wr(pix0 + y * 640 + x, 0);
    le_wr(CTL(C_CTRL), 0x3B); // display on
    map_vci(0);

    // The shadow (and the kernel's copy of what it last sent) black too, and
    // the screen described for the kernel.
    for (uint32_t a = SHADOW_PA; a < SHADOW_PA + SHADOW_SIZE; a += 4)
        *(volatile uint32_t *)a = 0;
    CTL_SCREEN->vram = pix0;
    CTL_SCREEN->pitch = 640;
    CTL_SCREEN->width = 640;
    CTL_SCREEN->height = 480;
    CTL_SCREEN->depth = 8;
    CTL_SCREEN->check = ~CTL_MAGIC;
    CTL_SCREEN->magic = CTL_MAGIC;
    return 1;
}
