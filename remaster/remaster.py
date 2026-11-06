#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Re-master the OS/2 Warp (PowerPC Edition) CD with patches.

    remaster.py IN.iso OUT.iso PATCHFILE...

The original image is never rebuilt from scratch.  It is copied and edited in
place, so the PReP type-0x41 boot partition (LBA 1, 1439 sectors), the
volume descriptors and every untouched file stay byte-identical.  That boot
partition is what the shim and IBM's PReP firmware load.

- A file that still fits the sectors it already owns is rewritten in place.
- A file that grows past them, or a new file, goes into sectors appended at
  the end of the image.
- A directory that runs out of room for a new record is moved to the end in
  the same way.  Its parent's record, its own "." record, its children's ".."
  records and both path tables are updated.

Patch files are line-oriented.  `#` starts a comment, and arguments are
shell-quoted (Python shlex).  Each `file` line selects the file that the
lines after it edit.

    file /BOOT.SCI
    comment-line 'PN_FILE_ARGV="libldpar.so"'   # first column becomes '#'
    replace-text 'old' 'new'                   # exactly once, any length; \r \n \t \\ escapes
    replace-hex  4e800020 60000000             # exactly once
    patch-at 0x1F3C 4082000c 60000000          # offset, expected, new
    patch-sym ncr_intr_thread+0x2bc 4082000c 60000000   # ELF symbol
    content local/path                         # replace the whole file
    asm local/code.S 1a2b3c4d5e6f              # assembled code at symbols (objpatch.py)
    grow-segment 0 0x6ED00                     # take a PT_LOAD's zero padding in (objpatch.py)
    asm-raw 0xD44B4 local/code.S 1a2b3c4d5e6f  # the same for raw code loaded at that address

    add /MK/NEWDRV.DYN local/path              # a new file

Relative local paths resolve against the patch file's own directory.
`patch-at` and `patch-sym` check the bytes they replace, so a patch can never
silently land on a different build of a file.  `patch-sym` resolves the
symbol through the file's ELF symbol table.  The Microkernel and its drivers
are unstripped ELF, and the file offset comes from the section that holds the
symbol.
"""
import os
import shlex
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import iso9660  # noqa: E402
import objpatch  # noqa: E402

SECTOR = iso9660.SECTOR


def sectors(n):
    return (n + SECTOR - 1) // SECTOR


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


class PatchError(Exception):
    pass


# ---------------------------------------------------------------------------
# ELF symbols (32-bit, either byte order)


def elf_symbol_offset(data, spec):
    name, _, add = spec.partition('+')
    add = int(add, 0) if add else 0
    if data[:4] != b'\x7fELF' or data[4] != 1:
        raise PatchError('patch-sym: not a 32-bit ELF file')
    e = '>' if data[5] == 2 else '<'
    shoff, = struct.unpack_from(e + 'I', data, 0x20)
    shentsize, shnum = struct.unpack_from(e + 'HH', data, 0x2E)
    secs = [struct.unpack_from(e + 'IIIIIIIIII', data, shoff + i * shentsize) for i in range(shnum)]
    for sh in secs:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[sh[6]]
        for k in range(sh[5] // 16):
            st_name, st_value, _, _, _, st_shndx = struct.unpack_from(e + 'IIIBBH', data, sh[4] + k * 16)
            s = data[strtab[4] + st_name: data.index(b'\0', strtab[4] + st_name)].decode()
            if s != name or not 0 < st_shndx < shnum:
                continue
            tgt = secs[st_shndx]  # (name, type, flags, addr, offset, size, ...)
            return tgt[4] + (st_value - tgt[3]) + add
    raise PatchError(f'patch-sym: no symbol {name}')


# ---------------------------------------------------------------------------
# File-content edits


def unhex(s):
    return bytes.fromhex(s.replace('_', ''))


def edit(data, op, args, base):
    if op == 'comment-line':
        (line,) = args
        needle = line.encode('latin1')
        hits = [i for i in find_all(data, needle) if i == 0 or data[i - 1] in b'\r\n']
        if len(hits) != 1:
            raise PatchError(f'comment-line: {len(hits)} lines start with {line!r}')
        i = hits[0]
        return data[:i] + b'#' + data[i + 1:]
    if op in ('replace-text', 'replace-hex'):
        old, new = args
        if op == 'replace-text':
            # Backslash escapes (\r, \n, \t, \\) are decoded, so a patch can
            # spell line ends: BOOT.SCI uses CRLF, the stanza files bare LF.
            old, new = (o.encode('latin1').decode('unicode_escape').encode('latin1') for o in (old, new))
        else:
            old, new = unhex(old), unhex(new)
        n = data.count(old)
        if n != 1:
            raise PatchError(f'{op}: {n} occurrences of {old!r}')
        return data.replace(old, new, 1)
    if op in ('patch-at', 'patch-sym'):
        where, old, new = args
        off = int(where, 0) if op == 'patch-at' else elf_symbol_offset(data, where)
        old, new = unhex(old), unhex(new)
        if len(old) != len(new):
            raise PatchError(f'{op}: expected and new bytes differ in length')
        if data[off:off + len(old)] != old:
            raise PatchError(f'{op} {where}: found {data[off:off + len(old)].hex()}, expected {old.hex()}')
        return data[:off] + new + data[off + len(new):]
    if op == 'asm':
        src, *sha = args
        data, report = objpatch.build(data, os.path.join(base, src), sha[0] if sha else None)
        print('    ' + ', '.join(report))
        return data
    if op == 'asm-raw':
        load, src, *sha = args
        data, report = objpatch.build(data, os.path.join(base, src), sha[0] if sha else None, int(load, 0))
        print('    ' + ', '.join(report))
        return data
    if op == 'grow-segment':
        index, size = args
        return objpatch.grow_segment(data, int(index, 0), int(size, 0))
    if op == 'content':
        (path,) = args
        return open(os.path.join(base, path), 'rb').read()
    raise PatchError(f'unknown operation {op}')


def find_all(data, needle):
    i = data.find(needle)
    while i >= 0:
        yield i
        i = data.find(needle, i + 1)


# ---------------------------------------------------------------------------
# The image


class Image:
    def __init__(self, data):
        self.img = bytearray(data)
        self.entries = iso9660.walk(self.img)
        pvd = 16 * SECTOR
        self.vol_blocks = struct.unpack_from('<I', self.img, pvd + 80)[0]
        # The pressing's run-out: zero sectors past the volume, kept so an
        # image with nothing appended stays the original's length.
        self.runout = max(0, len(self.img) - self.vol_blocks * SECTOR)
        if len(self.img) < self.vol_blocks * SECTOR:
            self.img.extend(b'\0' * (self.vol_blocks * SECTOR - len(self.img)))

    # --- space
    def append(self, payload):
        """Place `payload` in fresh sectors at the end; return its LBA."""
        lba = self.vol_blocks
        n = max(1, sectors(len(payload)))
        self.img[lba * SECTOR:] = b''
        self.img.extend(payload + b'\0' * (n * SECTOR - len(payload)))
        self.vol_blocks += n
        struct.pack_into('<I', self.img, 16 * SECTOR + 80, self.vol_blocks)
        struct.pack_into('>I', self.img, 16 * SECTOR + 84, self.vol_blocks)
        return lba

    # --- records
    def set_extent(self, rec_off, lba, size):
        self.img[rec_off + 2: rec_off + 10] = both32(lba)
        self.img[rec_off + 10: rec_off + 18] = both32(size)

    def read(self, e):
        return bytes(self.img[e.lba * SECTOR: e.lba * SECTOR + e.size])

    def write_file(self, e, payload):
        if sectors(len(payload)) <= max(1, sectors(e.size)):
            start = e.lba * SECTOR
            span = max(1, sectors(e.size)) * SECTOR
            self.img[start:start + span] = payload + b'\0' * (span - len(payload))
            lba = e.lba
        else:
            lba = self.append(payload)
        self.set_extent(e.rec_off, lba, len(payload))
        e.lba, e.size = lba, len(payload)

    # --- new files
    def add_file(self, path, payload):
        parent_path, _, name = path.rstrip('/').rpartition('/')
        parent = self.dir_entry(parent_path)
        ident = (name.upper() + ';1').encode('ascii')
        if any(x.path.upper() == path.upper() for x in self.entries):
            raise PatchError(f'add: {path} already exists')
        lba = self.append(payload)
        rec = self.make_record(ident, lba, len(payload), is_dir=False)
        self.insert_record(parent, rec)
        self.entries = iso9660.walk(self.img)

    def dir_entry(self, path):
        if path in ('', '/'):
            pvd = 16 * SECTOR
            root = iso9660.Entry('', struct.unpack_from('<I', self.img, pvd + 158)[0],
                                 struct.unpack_from('<I', self.img, pvd + 166)[0], True, pvd + 156, 34)
            return root
        e = iso9660.find(self.entries, path)
        if not e.is_dir:
            raise PatchError(f'{path} is not a directory')
        return e

    def make_record(self, ident, lba, size, is_dir):
        # Borrow the date of the primary volume's root record.
        date = bytes(self.img[16 * SECTOR + 156 + 18: 16 * SECTOR + 156 + 25])
        n = 33 + len(ident)
        n += n & 1
        r = bytearray(n)
        r[0] = n
        r[2:10] = both32(lba)
        r[10:18] = both32(size)
        r[18:25] = date
        r[25] = 2 if is_dir else 0
        r[28:32] = both16(1)
        r[32] = len(ident)
        r[33:33 + len(ident)] = ident
        return bytes(r)

    def insert_record(self, parent, rec):
        recs = [bytes(r) for _, r in iso9660._records(self.img, parent.lba, parent.size)]
        dots, rest = recs[:2], recs[2:]
        rest.append(rec)
        rest.sort(key=lambda r: r[33:33 + r[32]])  # ISO 9660 orders records by identifier
        # Lay the records out again; a record never spans a sector.
        out, cur = bytearray(), bytearray()
        for r in dots + rest:
            if len(cur) + len(r) > SECTOR:
                out += cur + b'\0' * (SECTOR - len(cur))
                cur = bytearray()
            cur += r
        out += cur + b'\0' * (SECTOR - len(cur))
        if len(out) <= max(1, sectors(parent.size)) * SECTOR:
            out += b'\0' * (sectors(parent.size) * SECTOR - len(out))
            self.img[parent.lba * SECTOR: parent.lba * SECTOR + len(out)] = out
            return
        self.move_dir(parent, bytes(out))

    def move_dir(self, d, payload):
        old = d.lba
        new = self.append(payload)
        size = len(payload)
        # Its own "." and, for the root, ".." too.
        base = new * SECTOR
        self.set_extent(base, new, size)
        if d.path == '':
            self.set_extent(base + self.img[base], new, size)
            self.set_extent(16 * SECTOR + 156, new, size)
        else:
            self.set_extent(d.rec_off, new, size)
        # Subdirectories' ".." records.
        for off, r in iso9660._records(self.img, new, size):
            name = r[33:33 + r[32]]
            if r[25] & 2 and name not in (b'\0', b'\1'):
                sub = struct.unpack_from('<I', r, 2)[0]
                self.set_extent(sub * SECTOR + self.img[sub * SECTOR], new, size)
        # Both path tables.
        pvd = 16 * SECTOR
        pt_size = struct.unpack_from('<I', self.img, pvd + 132)[0]
        for field, endian in ((140, '<'), (144, '<'), (148, '>'), (152, '>')):
            pt = struct.unpack_from(endian + 'I', self.img, pvd + field)[0]
            if pt == 0:
                continue
            off, end = pt * SECTOR, pt * SECTOR + pt_size
            while off < end:
                nlen = self.img[off]
                if struct.unpack_from(endian + 'I', self.img, off + 2)[0] == old:
                    struct.pack_into(endian + 'I', self.img, off + 2, new)
                off += 8 + nlen + (nlen & 1)
        self.entries = iso9660.walk(self.img)


# ---------------------------------------------------------------------------


def parse(path):
    base = os.path.dirname(os.path.abspath(path))
    ops = []
    for no, line in enumerate(open(path, encoding='utf-8'), 1):
        words = shlex.split(line, comments=True)
        if words:
            ops.append((f'{path}:{no}', words[0], words[1:], base))
    return ops


def run(src, dst, patchfiles):
    im = Image(open(src, 'rb').read())
    for pf in patchfiles:
        cur, data = None, None

        def flush():
            if cur is not None:
                im.write_file(cur, data)

        for where, op, args, base in parse(pf):
            try:
                if op == 'file':
                    flush()
                    cur = iso9660.find(im.entries, args[0])
                    data = im.read(cur)
                elif op == 'add':
                    flush()
                    cur = None
                    im.add_file(args[0], open(os.path.join(base, args[1]), 'rb').read())
                else:
                    if cur is None:
                        raise PatchError(f'{op} before any file line')
                    data = edit(data, op, args, base)
            except (PatchError, objpatch.ObjPatchError, KeyError, ValueError) as e:
                sys.exit(f'{where}: {e}')
            print(f'{where}: {op} {" ".join(args)}')
        flush()
    with open(dst, 'wb') as f:
        f.write(im.img[:im.vol_blocks * SECTOR])
        f.write(b'\0' * im.runout)
    print(f'wrote {dst}: {im.vol_blocks} sectors')


if __name__ == '__main__':
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    run(sys.argv[1], sys.argv[2], sys.argv[3:])
