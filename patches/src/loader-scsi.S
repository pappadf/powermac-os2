/* SPDX-License-Identifier: GPL-2.0-only */
/* loader-scsi.S -- BL_AUTO's SCSI module on the Network Server's hardware.
 * The module runs at file offset + $ACE3C (cc000-cdfff in memory); see
 * loader-svc.h for the service it calls.
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
