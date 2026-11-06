/* kernel-bat1.S -- give the Microkernel a mapping of the Network Server's I/O.
 *
 * On a 604 the kernel's _start loads BAT0 (itself), BAT2 (PReP's ISA I/O,
 * $F0800000 -> $80000000) and BAT3, and zeroes BAT1.  BAT1 is taken here for
 * Bandit 1 and Grand Central: virtual $FC000000 -> physical $F2000000, 32 MB,
 * supervisor only, cache-inhibited and guarded, read/write.  So kernel code
 * reaches the Mac's registers with plain loads and stores, as the NT HAL does
 * through its one I/O block.  kmem_init then keeps the kernel's own map below
 * $FC000000, so nothing else is ever put there (the map ran to $FFFF6000).
 */
#include "kernel-cave.h"

/* _start, at the 604's `mtdbatu 1, 13` (r13 = 0), takes the detour. */
	.section .patch._start+0x138, "ax"
	b     bat1_setup

	.section .patch.0xF016DA80, "ax"        /* CAVE_BAT1 */
bat1_setup:
	mtdbatu 1, 13                       /* invalid first */
	isync
	lis   13, NS_IO_PHYS@h
	ori   13, 13, 0x2A                  /* WIMG = 0101 (I, G), PP = 2 */
	mtdbatl 1, 13
	lis   13, NS_IO_VIRT@h
	ori   13, 13, 0x3FE                 /* BL = 32 MB, Vs = 1, Vp = 0 */
	mtdbatu 1, 13
	isync
	b     _start+0x13c

/* kmem_init(start, end): the kernel map ends at $FC000000. */
	.section .patch.kmem_init+0x1c, "ax"
	lis   12, NS_IO_VIRT@h
