/* SPDX-License-Identifier: GPL-2.0-only */
/* loader-scsi.S -- BL_AUTO's SCSI module on the Network Server's hardware.
 * The module runs at file offset + $ACE3C (cc000-cdfff in memory); see
 * loader-svc.h for the service it calls.  Its disk routines go to the
 * service's own driver (the end of this file); the edits before them keep
 * its PCI configuration routines working, and its 53C810 code, which no
 * longer runs, pointed at the right place.
 */
#include "loader-svc.h"

/* PCI configuration: read32, write16, write32 of a register (r3) of the device
 * whose PReP direct-config address the module keeps at $CF84C (slot 12's,
 * $80881000).  Each adds that base and hands the full address on. */
.macro cfg_entry svc
	lis   12, 0xCF84C@ha
	lwz   12, 0xCF84C@l(12)
	add   3, 3, 12
	ba    \svc
.endm
	.section .patch.0xcc0e0-0xcc0f0, "ax"
	cfg_entry LDRSVC_CFG_RD32
	.section .patch.0xcc158-0xcc168, "ax"
	cfg_entry LDRSVC_CFG_WR16
	.section .patch.0xcc26c-0xcc27c, "ax"
	cfg_entry LDRSVC_CFG_WR32

/* The chip's registers: its memory BAR, which the shim sets and the loader's
 * BAT for $80000000 maps, instead of PReP PCI I/O port $20000000 at $A0000000. */
	.section .patch.0xcd9c8-0xcd9cc, "ax"
	lis   12, SCSI_MEM_BAR@h

/* DMA: a PReP bus master sees memory at $80000000 + physical, and the module
 * flips bit 31 to make bus addresses (and back).  Bandit's are physical. */
	.section .patch.0xcc704-0xcc708, "ax"
	mr    11, 11
	.section .patch.0xcc73c-0xcc740, "ax"
	mr    11, 11
	.section .patch.0xcc774-0xcc778, "ax"
	mr    11, 11
	.section .patch.0xcc7ac-0xcc7b0, "ax"
	mr    11, 11
	.section .patch.0xcc7cc-0xcc7d0, "ax"
	mr    7, 12
	.section .patch.0xcc804-0xcc808, "ax"
	mr    7, 12
	.section .patch.0xcd170-0xcd174, "ax"
	mr    3, 24
	.section .patch.0xcd210-0xcd214, "ax"
	mr    3, 24
	.section .patch.0xcd378-0xcd37c, "ax"
	mr    3, 12

/* The disk itself -- the chip's set-up, a unit's probe, a transfer -- goes to
 * the loader service's polled driver (shim/scsi.c), which drives the 53C825A
 * as the module would, and also a Power Macintosh's MESH, for which the
 * module has no code at all.  The module's SCRIPTS engine is never started.
 *   - the chip's set-up: the service, then the module's own initialisation
 *     of its two unit records (or its "no SCSI adapter" message);
 *   - probe (unit, ID or 255 for any, device type) and transfer (request):
 *     the service, which fills and reads the unit records at UNITS. */
#define UNITS 0xCF4A0
	.section .patch.0xcd918-0xcd928, "ax"
	bla   LDRSVC_SCSI_INIT
	cmpwi 3, 0
	bne   . + 0xd0          /* the unit records, as before */
	b     . + 0xd4          /* "no SCSI adapter" */
	.section .patch.0xcd3e8-0xcd3f4, "ax"
	lis   6, UNITS@ha
	addi  6, 6, UNITS@l
	ba    LDRSVC_SCSI_PROBE
	.section .patch.0xcd79c-0xcd7a8, "ax"
	lis   4, UNITS@ha
	addi  4, 4, UNITS@l
	ba    LDRSVC_SCSI_RW
