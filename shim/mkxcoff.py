#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""mkxcoff.py flat.bin out.xcf [base] [be|le] -- wrap a flat PowerPC image, linked at `base`
with its entry at byte 0, in the XCOFF that Open Firmware's own /packages/xcoff-loader takes:
every Power Macintosh ROM and both Network Server ROMs have one, where only the Network
Server's 2.26NT ROM has a pe-loader.

One .text holding the whole image (its .bss included, as zeros) and an empty .bss.  The
headers are in the firmware's byte order: big-endian (`be`, the default) for a firmware in its
factory mode, which is what SHIM.XCF's stage0 needs; little-endian (`le`) only for a firmware
running with little-endian? true, which reads them with little-endian loads (powermac-nt-hal
measured `bad f_magic` for big-endian ones there).  The a.out entry is a function descriptor
{code, toc}, which the loader jumps through; it follows the image."""
import struct, sys

flat = open(sys.argv[1], 'rb').read()
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x00050000
E = '<' if len(sys.argv) > 4 and sys.argv[4] == 'le' else '>'
img = flat.ljust((len(flat) + 7) & ~7, b'\0')
desc = base + len(img)
img += struct.pack(E + 'II', base, 0)
# Whole pages, then one page each of .data and .bss: the big-endian loader
# claims .data at the end of .text whatever its size, and a claim of nothing
# fails (CLAIM failed); the little-endian one skipped it.  Both pages are zeros.
img = img.ljust((len(img) + 0xFFF) & ~0xFFF, b'\0')
PAGE = 0x1000
end = base + len(img)
# file header: U802TOCMAGIC, 3 sections, no symbols, a 72-byte a.out header,
# F_RELFLG | F_EXEC | F_LNNO | F_AR32W
fh = struct.pack(E + 'HHIIIHH', 0x01DF, 3, 0, 0, 0, 72, 0x0207)
# a.out: tsize, dsize, bsize, entry, text_start, data_start, toc, then the
# section numbers of the entry (1), .text (1), .data (2), toc (0), loader (0),
# .bss (3)
ah = struct.pack(E + 'HHIIIIIIIHHHHHHHH2sBBIII', 0x010B, 1, len(img), PAGE, PAGE, desc, base, end,
                 0, 1, 1, 2, 0, 0, 3, 2, 2, b'1L', 0, 0, 0, 0, 0).ljust(72, b'\0')
def scn(name, vaddr, size, ptr, flags):
    return struct.pack(E + '8sIIIIIIHHI', name, vaddr, vaddr, size, ptr, 0, 0, 0, 0, flags)
hdr = 20 + 72 + 3 * 40
sh = (scn(b'.text', base, len(img), hdr, 0x0020) + scn(b'.data', end, PAGE, hdr + len(img), 0x0040)
      + scn(b'.bss', end + PAGE, PAGE, 0, 0x0080))
img += bytes(PAGE)
open(sys.argv[2], 'wb').write(fh + ah + sh + img)
print(f'{sys.argv[2]}: XCOFF, .text {base:#x}+{len(img):#x}, entry descriptor {desc:#x}')
