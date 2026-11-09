#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""mkxcoff.py flat.bin out.xcf [base] -- wrap a flat little-endian PowerPC image, linked at
`base` with its entry at byte 0, in the XCOFF that Open Firmware's own /packages/xcoff-loader
takes: every Power Macintosh ROM and the Network Server's production ROM have one, where only
the Network Server's 2.26NT ROM has a pe-loader.

One .text holding the whole image (its .bss included, as zeros) and an empty .bss.  The headers
are little-endian: a firmware running with little-endian? true reads them with little-endian
loads (powermac-nt-hal measured `bad f_magic` for big-endian ones).  The a.out entry is a
function descriptor {code, toc}, which the loader jumps through; it follows the image."""
import struct, sys

flat = open(sys.argv[1], 'rb').read()
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x00050000
img = flat.ljust((len(flat) + 7) & ~7, b'\0')
desc = base + len(img)
img += struct.pack('<II', base, 0)
img = img.ljust((len(img) + 0x1FF) & ~0x1FF, b'\0')
end = base + len(img)
E = '<'
# file header: U802TOCMAGIC, 2 sections, no symbols, a 72-byte a.out header,
# F_RELFLG | F_EXEC | F_LNNO | F_AR32W
fh = struct.pack(E + 'HHIIIHH', 0x01DF, 2, 0, 0, 0, 72, 0x0207)
ah = struct.pack(E + 'HHIIIIIIIHHHHHHHH2sBBIII', 0x010B, 1, len(img), 0, 0, desc, base, end,
                 0, 1, 1, 0, 0, 0, 2, 2, 2, b'1L', 0, 0, 0, 0, 0).ljust(72, b'\0')
def scn(name, vaddr, size, ptr, flags):
    return struct.pack(E + '8sIIIIIIHHI', name, vaddr, vaddr, size, ptr, 0, 0, 0, 0, flags)
hdr = 20 + 72 + 2 * 40
sh = scn(b'.text', base, len(img), hdr, 0x0020) + scn(b'.bss', end, 0, 0, 0x0080)
open(sys.argv[2], 'wb').write(fh + ah + sh + img)
print(f'{sys.argv[2]}: XCOFF, .text {base:#x}+{len(img):#x}, entry descriptor {desc:#x}')
