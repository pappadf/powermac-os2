// SPDX-License-Identifier: GPL-2.0-only
//
// os2shim -- stage0.c: the big-endian front end.
//
// SHIM.XCF used to need a firmware already running little-endian, which on
// these machines means `setenv little-endian? true` and `reset-all`: a
// persistent NVRAM setting Apple never supported, which has left real Macs
// needing an NVRAM reset (at least one could not be recovered).  This stage
// instead runs as an ordinary big-endian client of Open Firmware, with the
// firmware exactly as the machine left the factory, and switches the CPU and
// the PCI bridges to little-endian itself (stage0.S) -- the approach of
// maciNTosh's loader on later Macs.  The switch lasts until the next reset.
//
// The firmware is asked only what the shim needs from it: the RAM size, and
// where the Bandit bridges are.  After stage0_switch nothing calls it again.

#include <stdint.h>

uint32_t stage0_cif;
uint8_t stage0_stack[8192] __attribute__((aligned(16)));

extern const uint8_t stage0_payload[], stage0_payload_end[];
int stage0_cif_call(uint32_t *args);
void stage0_switch(uint32_t ram, const uint32_t *bandits, uint32_t n, const uint8_t *payload, uint32_t len)
    __attribute__((noreturn));

#define SHIM_BASE 0x00050000u
#define STAGE0_BASE 0x00600000u // stage0.ld: the shim's copy must not reach it

// --- the client interface (big-endian here: the firmware's own mode) ---------

static uint32_t cif(const char *svc, int nargs, int nrets, const uint32_t *in, uint32_t *out) {
    uint32_t a[16];
    a[0] = (uint32_t)svc;
    a[1] = (uint32_t)nargs;
    a[2] = (uint32_t)nrets;
    for (int i = 0; i < nargs; i++)
        a[3 + i] = in[i];
    for (int i = 0; i < nrets; i++)
        a[3 + nargs + i] = 0;
    int r = stage0_cif_call(a);
    for (int i = 0; i < nrets; i++)
        out[i] = a[3 + nargs + i];
    return (uint32_t)r;
}

static uint32_t slen(const char *s) {
    uint32_t n = 0;
    while (s[n])
        n++;
    return n;
}

static uint32_t of_stdout;

static void puts_(const char *s) {
    if (!of_stdout)
        return;
    uint32_t a[3] = {of_stdout, (uint32_t)s, slen(s)}, r;
    cif("write", 3, 1, a, &r);
}

static void puthex(uint32_t v) {
    char b[11] = "0x";
    for (int i = 0; i < 8; i++)
        b[2 + i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
    b[10] = 0;
    puts_(b);
}

static uint32_t finddevice(const char *path) {
    uint32_t a[1] = {(uint32_t)path}, r = 0;
    return cif("finddevice", 1, 1, a, &r) ? 0xFFFFFFFFu : r;
}

static uint32_t walk(const char *svc, uint32_t ph) {
    uint32_t a[1] = {ph}, r = 0;
    return cif(svc, 1, 1, a, &r) ? 0 : r;
}

static int32_t getprop(uint32_t ph, const char *name, void *buf, uint32_t len) {
    uint32_t a[4] = {ph, (uint32_t)name, (uint32_t)buf, len}, r = 0;
    return cif("getprop", 4, 1, a, &r) ? -1 : (int32_t)r;
}

static void fail(const char *why) __attribute__((noreturn));
static void fail(const char *why) {
    puts_("os2shim: ");
    puts_(why);
    puts_("\r\n");
    cif("exit", 0, 0, 0, 0);
    for (;;)
        ;
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

// --- main ----------------------------------------------------------------------

void stage0_main(void) __attribute__((noreturn));
void stage0_main(void) {
    uint32_t chosen = finddevice("/chosen");
    if (chosen != 0xFFFFFFFFu)
        getprop(chosen, "stdout", &of_stdout, 4);
    puts_("\r\nos2shim: OS/2 Warp (PowerPC Edition) from Open Firmware\r\n");

    // RAM: /memory's reg, every (base, size) pair summed (the Network
    // Server's POST re-bases its banks contiguously from 0).
    uint32_t reg[32];
    uint32_t ram = 0;
    int32_t rl = getprop(finddevice("/memory"), "reg", reg, sizeof reg);
    for (int i = 0; i + 1 < rl / 4; i += 2)
        ram += reg[i + 1];
    if (ram < (16u << 20))
        fail("cannot size memory");
    puts_("os2shim: RAM ");
    puthex(ram);
    puts_("\r\n");

    // The Bandit bridges: the root's children named "bandit" (Chaos, the
    // 7500/8500's video bridge, has no endian mode and is left alone).
    // Their first reg word is the bridge's base: $F2000000, and $F4000000
    // on machines with a second.
    uint32_t bandits[4];
    uint32_t nb = 0;
    for (uint32_t ph = walk("child", finddevice("/")); ph && ph != 0xFFFFFFFFu && nb < 4; ph = walk("peer", ph)) {
        char name[32];
        int32_t n = getprop(ph, "name", name, sizeof name - 1);
        if (n <= 0)
            continue;
        name[n] = 0;
        uint32_t r[2];
        if (streq(name, "bandit") && getprop(ph, "reg", r, sizeof r) >= 4)
            bandits[nb++] = r[0];
    }
    if (nb == 0 || bandits[0] != 0xF2000000u)
        fail("no Bandit at $F2000000: not a machine this shim knows");
    for (uint32_t i = 0; i < nb; i++) {
        puts_("os2shim: Bandit at ");
        puthex(bandits[i]);
        puts_("\r\n");
    }

    uint32_t len = (uint32_t)(stage0_payload_end - stage0_payload);
    if (SHIM_BASE + len > STAGE0_BASE)
        fail("the shim is too big for its place");

    puts_("os2shim: switching to little-endian mode (until the next reset)\r\n");
    stage0_switch(ram, bandits, nb, stage0_payload, len);
}
