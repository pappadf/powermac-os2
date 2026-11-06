/* loader-fat.S -- the disk's boot loader (FAT_BL.HD) on the Network Server's
 * hardware.
 *
 * The installer writes /OS2/FAT_BL.HD into the hard disk's PReP boot
 * partition.  It is the same loader as BL_AUTO (patch 90, loader-core.S and
 * loader-scsi.S) in another build: its core sits 0x170 bytes further into the
 * file at the same load addresses, and its SCSI module is laid out
 * differently.  The edits are BL_AUTO's, at this file's offsets (base 0: the
 * section names are file offsets; every branch is absolute):
 *
 *   - the six PC-port helpers and the NVRAM read go to the loader service;
 *     the NVRAM write, which BL_AUTO never makes, is dropped;
 *   - the three PCI configuration routines, which take a register number,
 *     add the direct-configuration base (the variable at $CE65C, $80881000)
 *     and go to the service; the module's CF8/CFC port helpers go to the
 *     service's port routines as PReP addresses;
 *   - the SCSI chip's base is its memory BAR (SCSI_MEM_BAR), not PReP PCI
 *     I/O $A0000000;
 *   - DMA addresses lose PReP's $80000000.
 */
#include "loader-svc.h"

#define CFG_BASE_VAR 0xCE65C

/* The core's six port helpers. */
	.section .patch.0x1dd4-0x1dd8, "ax"
	ba    LDRSVC_OUT8
	.section .patch.0x1de4-0x1de8, "ax"
	ba    LDRSVC_OUT16
	.section .patch.0x1df4-0x1df8, "ax"
	ba    LDRSVC_OUT32
	.section .patch.0x1e04-0x1e08, "ax"
	ba    LDRSVC_IN8
	.section .patch.0x1e18-0x1e1c, "ax"
	ba    LDRSVC_IN16
	.section .patch.0x1e2c-0x1e30, "ax"
	ba    LDRSVC_IN32

/* NVRAM: the read (index) to the service, the write dropped. */
	.section .patch.0x936c-0x9370, "ax"
	ba    LDRSVC_NVRAM_RD
	.section .patch.0x94c4-0x94c8, "ax"
	blr

/* PCI configuration: register number -> the full direct-config address. */
.macro cfg_entry svc
	lis   12, CFG_BASE_VAR@ha
	lwz   12, CFG_BASE_VAR@l(12)
	add   3, 3, 12
	ba    \svc
.endm
	.section .patch.0x1a7c4-0x1a7d4, "ax"
	cfg_entry LDRSVC_CFG_RD32
	.section .patch.0x1a83c-0x1a84c, "ax"
	cfg_entry LDRSVC_CFG_WR16
	.section .patch.0x1a950-0x1a960, "ax"
	cfg_entry LDRSVC_CFG_WR32

/* The module's own 32-bit port helpers (port number in r3), for CF8/CFC. */
	.section .patch.0x1a758-0x1a760, "ax"
	oris  3, 3, 0x8000
	ba    LDRSVC_OUT32
	.section .patch.0x1a7a8-0x1a7b0, "ax"
	oris  3, 3, 0x8000
	ba    LDRSVC_IN32

/* The chip's registers: its memory BAR. */
	.section .patch.0x1bff4-0x1bff8, "ax"
	lis   12, SCSI_MEM_BAR@h

/* DMA: bus address = physical. */
	.section .patch.0x1adac-0x1adb0, "ax"
	mr    11, 11
	.section .patch.0x1ade4-0x1ade8, "ax"
	mr    11, 11
	.section .patch.0x1ae1c-0x1ae20, "ax"
	mr    11, 11
	.section .patch.0x1ae54-0x1ae58, "ax"
	mr    11, 11
	.section .patch.0x1ae74-0x1ae78, "ax"
	mr    7, 12
	.section .patch.0x1aeac-0x1aeb0, "ax"
	mr    7, 12
	.section .patch.0x1b7c8-0x1b7cc, "ax"
	mr    3, 24
	.section .patch.0x1b868-0x1b86c, "ax"
	mr    3, 24
	.section .patch.0x1b9c4-0x1b9c8, "ax"
	mr    3, 12
