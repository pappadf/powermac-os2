/* SPDX-License-Identifier: GPL-2.0-only */
/* kernel-iomap.S -- PReP's linear I/O windows, mapped onto the Network Server's.
 *
 * OS/2's drivers ask for device registers by PReP physical address (LIBDDPIO's
 * classes; see docs/how-it-works.md): PCI I/O
 * port p at $80000000 | p, PCI memory m at $C0000000 | m.  Every mapping of
 * such an address ends in io_pemm_request, the pager behind the I/O-space
 * memory object (physical = offset): user tasks map that object, and so does
 * io_map_class for kernel extensions (its vm_map; only its fixed ISA window,
 * $80000000-$807FFFFF, is BAT2's instead).  So the pager's physical-address
 * callback alone remaps, the offsets staying PReP addresses:
 *
 *   $A0000000-$A0FFFFFF  PCI I/O $20000000-...: PReP's place for I/O BARs,
 *                        which Bandit's 16-bit I/O space cannot reach.  The
 *                        first 53C825A's I/O BAR is $20000000 and its
 *                        registers repeat in its memory BAR at $8F000000
 *                        (loader-svc.h), so -> $8F000000 + the offset.
 *   $C0000000-$CFFFFFFF  PCI memory: Bandit 1 forwards $80000000-$8FFFFFFF
 *                        one-to-one, so -> $80000000-$8FFFFFFF.  Except
 *                        $CE000000-$CE0FFFFF, the stand-in display adapter's
 *                        memory (kernel-trap.c): -> the shadow frame buffer
 *                        in RAM at $03F00000 (kernel-video.c).
 *
 * Both are linear, page by page.  The spread ISA ports and direct-mapped
 * configuration ($80000000-$80FFFFFF) are not: their pages go to the trap
 * window at $88000000 + the same offset, where kernel-trap.c emulates them.
 */
#include "kernel-cave.h"

	.section .patch.0xF016E400, "ax"        /* CAVE_IOMAP */

/* io_pemm_request's physical-address callback (`mr 3, 4; blr`, which it hands
 * vm_object_page_map_physical): r3 = the physical address of object offset r4.
 * The offset itself stays the PReP address: it is where the faulting thread
 * looks for the page, and a page put at the remapped offset instead leaves
 * the fault waiting on a busy page forever. */
io_phys:
	mr    3, 4
	srwi  11, 3, 24
	cmpwi 11, 0x80
	beq   2f
	srwi  11, 3, 28
	cmpwi 11, 0xA
	beq   1f
	cmpwi 11, 0xC
	bnelr
	addis 12, 3, 0x3200             /* the stand-in display's memory, $CE000000-$CE0FFFFF? */
	srwi. 12, 12, 20
	beq   3f
	addis 3, 3, -0x4000             /* $Cxxxxxxx -> $8xxxxxxx */
	blr
3:	addis 3, 3, 0x35F0              /* $CE000000 + o -> the shadow frame buffer, $03F00000 + o */
	blr
1:	addis 3, 3, -0x1100             /* $A0000000 + o -> $8F000000 + o (SCSI_MEM_BAR) */
	blr
2:	addis 3, 3, 0x0800              /* $80xxxxxx -> $88xxxxxx, the trap window (kernel-trap.c) */
	blr

	.section .patch.io_get_io_memory+0x34, "ax"
	b     io_phys
