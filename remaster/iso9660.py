# SPDX-License-Identifier: GPL-2.0-only
"""Minimal ISO 9660 reader for the OS/2 PowerPC CDs (primary volume, no Joliet/RR)."""
import struct

SECTOR = 2048


class Entry:
    __slots__ = ('path', 'lba', 'size', 'is_dir', 'rec_off', 'rec_len')

    def __init__(self, path, lba, size, is_dir, rec_off, rec_len):
        self.path, self.lba, self.size, self.is_dir = path, lba, size, is_dir
        self.rec_off, self.rec_len = rec_off, rec_len  # byte offset of the directory record in the image

    def __repr__(self):
        return f'{self.path} lba={self.lba} size={self.size}{" dir" if self.is_dir else ""}'


def _records(img, lba, size):
    """Yield (byte offset, record bytes) for each directory record of a directory extent."""
    base = lba * SECTOR
    for sec in range(0, size, SECTOR):
        off = 0
        while off < SECTOR:
            n = img[base + sec + off]
            if n == 0:
                break  # records never span sectors: the rest of this one is padding
            yield base + sec + off, img[base + sec + off: base + sec + off + n]
            off += n


def walk(img):
    """All entries of the primary volume, directories first-order."""
    pvd = img[16 * SECTOR: 17 * SECTOR]
    assert pvd[1:6] == b'CD001' and pvd[0] == 1, 'no primary volume descriptor'
    root = pvd[156:156 + 34]
    out = []
    stack = [('', struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0])]
    while stack:
        prefix, lba, size = stack.pop()
        for off, r in _records(img, lba, size):
            nlen = r[32]
            name = r[33:33 + nlen]
            if name in (b'\x00', b'\x01'):
                continue
            name = name.decode('ascii', 'replace').split(';')[0].rstrip('.')
            elba, esize = struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0]
            is_dir = bool(r[25] & 2)
            path = f'{prefix}/{name}'
            out.append(Entry(path, elba, esize, is_dir, off, len(r)))
            if is_dir:
                stack.append((path, elba, esize))
    return out


def find(entries, path):
    p = path.upper().rstrip('/')
    if not p.startswith('/'):
        p = '/' + p
    for e in entries:
        if e.path.upper() == p:
            return e
    raise KeyError(path)


def containing(entries, byte_off):
    for e in entries:
        if not e.is_dir and e.lba * SECTOR <= byte_off < e.lba * SECTOR + e.size:
            return e, byte_off - e.lba * SECTOR
    return None, None
