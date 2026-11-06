/* loader-core.S -- BL_AUTO's core on the Network Server's hardware.
 * The core runs at file offset + $D44B4 (d6000-e45ff); see loader-svc.h.
 */
#include "loader-svc.h"

/* The six port helpers: every PC port the core touches goes through them. */
	.section .patch.0xd6118-0xd611c, "ax"
	ba    LDRSVC_OUT8
	.section .patch.0xd6128-0xd612c, "ax"
	ba    LDRSVC_OUT16
	.section .patch.0xd6138-0xd613c, "ax"
	ba    LDRSVC_OUT32
	.section .patch.0xd6148-0xd614c, "ax"
	ba    LDRSVC_IN8
	.section .patch.0xd615c-0xd6160, "ax"
	ba    LDRSVC_IN16
	.section .patch.0xd6170-0xd6174, "ax"
	ba    LDRSVC_IN32

/* NVRAM read (index): through pointers to ports $74/$75/$77. */
	.section .patch.0xdd59c-0xdd5a0, "ax"
	ba    LDRSVC_NVRAM_RD
