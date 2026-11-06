/* SPDX-License-Identifier: GPL-2.0-only */
/* The loader service's image (ldrsvc.c, linked at LDRSVC), for the shim to copy. */
    .section .rodata
    .balign 4
    .globl ldrsvc_blob
    .globl ldrsvc_blob_end
ldrsvc_blob:
    .incbin BLOB
ldrsvc_blob_end:
