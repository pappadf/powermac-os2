/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-pci.c -- the Microkernel's PCI configuration access, through Bandit.
 *
 * The kernel's accessors take a configuration-mechanism-#1 address,
 * $80000000 | bus << 16 | device << 11 | function << 8 | register, and
 * drove ports $CF8/$CFC through outl/inl.  No Mac has those.  Bandit 1 (the
 * Network Server's bus 0) does type-0 cycles through two ports of its own:
 * write (1 << device) | (function << 8) | (register & $FC) to the address
 * port at +$800000, then access the data port at +$C00000 + (register & 3) at
 * the natural size (Apple, Network Server Hardware Developer Notes; the NT
 * HAL's pci.c).  Devices below 11 do not exist on a Bandit, and an absent
 * device reads all-ones.  OS/2's device numbers are Bandit's IDSELs: the
 * 53C825As at 17 and 18, the 54M30 at 15, Grand Central at 16.  Bus 0 is all
 * that is reached here; Bandit 2's bus, like every other bus, reads as empty.
 *
 * The ports are reached through BAT1 (kernel-bat1.S).  The work is done in
 * the text cave; each original function becomes a tail call.
 */
#include "kernel-cave.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

#define CAVE __attribute__((section(".patch.0xF016DB00"), noinline, used))  /* CAVE_PCI */
#define AT(sym) __attribute__((section(".patch." #sym), used))

static inline void eieio(void)
{
    __asm__ volatile("eieio" ::: "memory");
}

/* Select a register; its data port's address, or 0 when there is no device there. */
CAVE static u32 cfg_select(u32 a)
{
    u32 bus = (a >> 16) & 0xFF, dev = (a >> 11) & 31, fn = (a >> 8) & 7, reg = a & 0xFF;
    if (bus != 0 || dev < 11)
        return 0;
    *(volatile u32 *)BANDIT1_CFG_ADDR = (1u << dev) | (fn << 8) | (reg & 0xFC);
    eieio();
    return BANDIT1_CFG_DATA + (reg & 3);
}

CAVE static u32 cfg_read(u32 a, u32 size)
{
    u32 p = cfg_select(a), v;
    if (!p)
        return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    if (size == 1)
        v = *(volatile u8 *)p;
    else if (size == 2)
        v = *(volatile u16 *)p;
    else
        v = *(volatile u32 *)p;
    eieio();
    return v;
}

CAVE static u32 cfg_write(u32 a, u32 size, u32 v)
{
    u32 p = cfg_select(a);
    if (p) {
        if (size == 1)
            *(volatile u8 *)p = (u8)v;
        else if (size == 2)
            *(volatile u16 *)p = (u16)v;
        else
            *(volatile u32 *)p = v;
        eieio();
    }
    return v;
}

/* The first function 0 on bus 0 with this vendor and device ID: its address, or 0. */
CAVE static u32 cfg_find(u32 vendor, u32 device)
{
    for (u32 dev = 11; dev < 32; dev++) {
        u32 a = 0x80000000u | dev << 11, id = cfg_read(a, 4);
        if ((id & 0xFFFF) == vendor && id >> 16 == device)
            return a;
    }
    return 0;
}

CAVE static u32 cfg_modify16(u32 a, u32 mask, u32 value)
{
    u32 v = (cfg_read(a, 2) & ~mask) | (value & mask);
    return cfg_write(a, 2, v & 0xFFFF);
}

AT(pci_readl_unlocked) u32 k_pci_readl(u32 a) { return cfg_read(a, 4); }
AT(pci_reads_unlocked) u32 k_pci_reads(u32 a) { return cfg_read(a, 2); }
AT(pci_readb_unlocked) u32 k_pci_readb(u32 a) { return cfg_read(a, 1); }
AT(pci_writel_unlocked) u32 k_pci_writel(u32 a, u32 v) { return cfg_write(a, 4, v); }
AT(pci_writes_unlocked) u32 k_pci_writes(u32 a, u32 v) { return cfg_write(a, 2, v & 0xFFFF); }
AT(pci_writeb_unlocked) u32 k_pci_writeb(u32 a, u32 v) { return cfg_write(a, 1, v & 0xFF); }
AT(pci_modifys_unlocked) u32 k_pci_modifys(u32 a, u32 mask, u32 v) { return cfg_modify16(a, mask, v); }
AT(pci_find_config_entry) u32 k_pci_find(u32 vendor, u32 device) { return cfg_find(vendor, device); }
