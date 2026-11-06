#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""mkpe.py flat.bin out.exe [image_base] -- wrap a flat little-endian PowerPC image in the
bare PE shape Open Firmware's pe-loader takes (the NT veneer's): a COFF file header at
offset 0 (machine $01F0, no MS-DOS stub), a PE32 optional header, ONE section at RVA 0
whose raw data starts at file offset $200, and the entry point at RVA 0.  No imports,
no relocations: the image is linked at its base."""
import struct, sys
flat = open(sys.argv[1], 'rb').read()
base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x00050000
align = 0x200
size = (len(flat) + align - 1) // align * align
flat = flat.ljust(size, b'\0')
coff = struct.pack('<HHIIIHH', 0x01F0, 1, 0, 0, 0, 0xE0, 0x0102)
opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII',
                  0x010B, 3, 10, size, 0, 0, 0, 0, 0, base, align, align,
                  4, 0, 0, 0, 0, 0, 0, size, 0x200, 0, 0, 0,
                  0x100000, 0x1000, 0x100000, 0x1000, 0, 16)
opt += b'\0' * (16 * 8)
sect = struct.pack('<8sIIIIIIHHI', b'.text', size, 0, size, 0x200, 0, 0, 0, 0, 0x60000020)
hdr = (coff + opt + sect).ljust(0x200, b'\0')
open(sys.argv[2], 'wb').write(hdr + flat)
print(f'{sys.argv[2]}: {len(hdr) + len(flat)} bytes, base {base:#x}, image {size:#x}')
