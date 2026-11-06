/* SPDX-License-Identifier: GPL-2.0-only */
/* loader-svc.h -- the loader service the shim installs for IBM's boot loader.
 *
 * BL_AUTO, the IBM Microkernel Bootloader, drives PReP hardware: PC ports
 * through six helpers (out8/16/32, in8/16/32, by full PReP address), PCI
 * configuration through three routines of its SCSI module, its NVRAM through
 * one, and the SCSI chip's registers at $A0000000.  None of that exists on a
 * Network Server.  The shim (shim/ldrsvc.c) puts code at LDRSVC that
 * does the same jobs on the Mac's hardware, behind this table of branches,
 * and the loader patches turn each of those routines into a branch to its
 * entry: an absolute `ba`, since LDRSVC is below 32 MB.  The loader calls
 * them with its MMU on; the shim gives it a BAT for $F0000000-$FFFFFFFF.
 *
 * RAM at $40000 survives the whole loader phase (checked with marker blocks
 * written by the shim before the hand-off).
 */
#define LDRSVC            0x40000
#define LDRSVC_OUT8       (LDRSVC + 0x00)   /* (addr, v) */
#define LDRSVC_OUT16      (LDRSVC + 0x04)
#define LDRSVC_OUT32      (LDRSVC + 0x08)
#define LDRSVC_IN8        (LDRSVC + 0x0C)   /* (addr) -> v */
#define LDRSVC_IN16       (LDRSVC + 0x10)
#define LDRSVC_IN32       (LDRSVC + 0x14)
#define LDRSVC_CFG_RD32   (LDRSVC + 0x18)   /* (PReP direct-config address) -> v */
#define LDRSVC_CFG_WR16   (LDRSVC + 0x1C)   /* (address, v) */
#define LDRSVC_CFG_WR32   (LDRSVC + 0x20)   /* (address, v) */
#define LDRSVC_NVRAM_RD   (LDRSVC + 0x24)   /* (index) -> v */

/* The first 53C825A's two BARs.  Its registers appear in both, identically.
 * The I/O BAR keeps the value PReP firmware and the loader give it, PCI I/O
 * $20000000: Bandit drives only 16 bits of an I/O address, so nothing ever
 * reaches the chip there, and OS/2 can go on computing its PReP address
 * ($A0000000) from it.  Everything really goes to the memory BAR, which the
 * shim places at SCSI_MEM_BAR, inside Bandit 1's identity window: the
 * loader's base (patch 90), and where the kernel maps $A0000000 (patch 95).
 * It sits at the window's top, clear of the 54M30's 16 MB at $81000000 and of
 * the PCI memory OS/2 itself assigns; OS/2 reads it as $0F000000 (the
 * configuration trap's PReP view of PCI memory, kernel-trap.c), not as an
 * unassigned 0. */
#define SCSI_MEM_BAR      0x8F000000u
#define SCSI_IO_BAR       0x20000000u

/* The 54M30's display memory (BAR0, 16 MB): PCI $02000000 to OS/2, as its
 * hardware database places a display adapter (RDF.STZ, "A81013"). */
#define VIDEO_MEM_BAR     0x82000000u
