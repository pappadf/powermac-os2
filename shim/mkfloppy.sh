#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# mkfloppy.sh SHIM.XCF os2boot.of out.img -- a FAT12 1.44 MB floppy with SHIM.XCF
# and OS2BOOT.OF, the script told which sectors SHIM.XCF occupies (it is read
# with read-blocks, as the NT boot floppy reads its veneer).
set -e
exe=$1; tmpl=$2; out=$3
rm -f "$out"
mformat -C -f 1440 -v OS2SHIM -i "$out" ::
mcopy -i "$out" "$exe" ::SHIM.XCF
python3 - "$exe" "$tmpl" "$out" <<'PY'
import sys
exe, tmpl, out = sys.argv[1:]
img = open(out, 'rb').read(); data = open(exe, 'rb').read()
off = img.find(data)
assert off >= 0 and off % 512 == 0, 'SHIM.XCF not contiguous on the floppy'
sec, n = off // 512, (len(data) + 511) // 512
reads = []
addr = 0x1000000
while n > 0:
    k = min(n, 0x20)
    reads.append(f'   {addr:X} {sec:X} {k:X} " read-blocks" os2-fd $call-method drop')
    addr += k * 512; sec += k; n -= k
maplen = (len(data) + 0xFFF) // 0x1000 * 0x1000
# What xcoff-loader lays out at 0x600000: tsize + dsize + bsize (a.out header).
import struct
imglen = sum(struct.unpack('>III', data[24:36]))
s = open(tmpl).read()
s = s.replace('@READS@', '\n'.join(reads)).replace('@SIZE@', f'{len(data):X}').replace('@MAPLEN@', f'{maplen:X}').replace('@IMGLEN@', f'{imglen:X}')
s = s.replace('@SECTORS@ is filled in by mkfloppy.sh.', f'SHIM.XCF is sectors {off//512:X}-{off//512 + (len(data)+511)//512 - 1:X}.')
# CRLF: this Open Firmware's `\` comment runs to a carriage return, so with bare
# line feeds the first comment would swallow the whole file.
open(out + '.of', 'wb').write(s.replace('\r\n', '\n').replace('\n', '\r\n').encode())
PY
mcopy -i "$out" "$out.of" ::OS2BOOT.OF
rm -f "$out.of"
mdir -i "$out" ::
