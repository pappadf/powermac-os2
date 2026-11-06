/* mtty-escc.S -- MTTY.DYN's serial console on the Network Server's own port.
 *
 * MTTY.DYN, the Micro-Console server, drives a PC 16550 at ISA port $3F8.
 * This replaces its UART layer with one for channel A of the Z85C30 ESCC in
 * Grand Central (the port Open Firmware calls ttya): control at physical
 * $F3013020, data at $F3013030 (Apple, Network Server Hardware Developer
 * Notes; the NT HAL's ans.h).  Open Firmware has already programmed the
 * channel (ttya is its console), so com_init and set_baud have nothing
 * to do.
 *
 * mtty maps the UART through LIBDDPIO with map_io(UART_ADDR, 8, &pio_com),
 * the ISA I/O class.  The ESCC is plain physical memory, which the memory
 * class reaches: its translation is `addr | $C0000000`, and $F3013020 already
 * has those bits.  So main calls map_physical_mem instead, and UART_ADDR
 * holds the channel's physical address.  Every register access stays
 * foo_vmRegRead8/foo_vmRegWrite8(handle, UART_ADDR + reg - handle->base),
 * as mtty's own code does.
 *
 * The .DYN runs at its link address in every task, so absolute addresses
 * are fine here (no GOT).
 */
#define ESCC_A_CTRL 0xF3013020
#define DATA        0x10          /* data register: control + $10 */
#define RR0_RX      0x01          /* RR0: a character is waiting */
#define RR0_TX      0x04          /* RR0: the transmit buffer is empty */

/* Load mtty's PIO handle (*pio_com_ptr) into r3, and UART_ADDR + reg - handle->base into r4. */
.macro uart_reg reg
	lis   9, 0x2013600@ha
	lwz   3, 0x2013600@l(9)      /* the handle: the word mtty's GOT+$FC names */
	lis   9, UART_ADDR@ha
	lwz   4, UART_ADDR@l(9)
	lwz   9, 12(3)               /* handle->base */
	addi  4, 4, \reg
	subf  4, 9, 4
.endm

.macro prologue
	mflr  0
	stwu  1, -32(1)
	stw   0, 36(1)
	stw   31, 28(1)
.endm
.macro epilogue
	lwz   31, 28(1)
	lwz   0, 36(1)
	addi  1, 1, 32
	mtlr  0
	blr
.endm

/* Map the channel's registers as physical memory (main, at the map_io(UART_ADDR, 8, &pio_com) call). */
	.section .patch.main+0x288, "ax"
	li    4, 0x20
	bl    map_physical_mem

/* The default port: channel A's control register. */
	.section .patch.UART_ADDR, "aw"
	.long ESCC_A_CTRL

/* int com_init(void): Open Firmware set the channel up. */
	.section .patch.com_init, "ax"
	li    3, 0
	blr

/* int set_baud(void): likewise. */
	.section .patch.set_baud, "ax"
	li    3, 0
	blr

/* int com_put(int c): '\n' goes out as "\r\n", as the 16550 code did. */
	.section .patch.com_put, "ax"
	prologue
	mr    31, 3
	cmpwi 31, 10
	bne   1f
	li    3, 13
	bl    com_put
1:	uart_reg 0
	bl    PLT_foo_vmRegRead8
	andi. 3, 3, RR0_TX
	beq   1b
	uart_reg DATA
	andi. 5, 31, 0xFF
	bl    PLT_foo_vmRegWrite8
	li    3, 0
	epilogue

/* int com_get(void): wait for a character and return it. */
	.section .patch.com_get, "ax"
	prologue
1:	uart_reg 0
	bl    PLT_foo_vmRegRead8
	andi. 3, 3, RR0_RX
	beq   1b
	uart_reg DATA
	bl    PLT_foo_vmRegRead8
	epilogue

/* int com_check_input(void): 1 when a character is waiting. */
	.section .patch.com_check_input, "ax"
	prologue
	uart_reg 0
	bl    PLT_foo_vmRegRead8
	andi. 3, 3, RR0_RX
	beq   1f
	li    3, 1
1:	epilogue

/* void wait_for_xon(void): read until an XON ($11). */
	.section .patch.wait_for_xon, "ax"
	prologue
1:	uart_reg 0
	bl    PLT_foo_vmRegRead8
	andi. 3, 3, RR0_RX
	beq   1b
	uart_reg DATA
	bl    PLT_foo_vmRegRead8
	cmpwi 3, 0x11
	bne   1b
	epilogue
