"""Patch an ELF file on the CD with newly assembled or compiled code.

    asm SOURCE [SHA]          (a remaster.py operation, after a `file` line)

SOURCE is a GNU-syntax PowerPC assembler file (.S, run through the C
preprocessor) or a C file (.c).  It is built little-endian for the 604, as all
of the CD's code is.  Every allocated section in it must be named for where its
bytes go in the file being patched:

    .section .patch.com_put,"ax"        at the symbol com_put
    .section .patch.main+0x28c,"ax"     at an offset from a symbol
    .section .patch.0x2004d00,"ax"      at an address

Each section is linked at that address.  Every symbol of the target ELF is
defined for the link, and so is each import's PLT stub as `PLT_<name>`.  So
`bl PLT_foo_vmRegRead8` or `lis 3, UART_ADDR@ha` resolve as they would in
the original build.  A section placed at a symbol must end before the next
symbol starts (it replaces that function, or that object, and nothing past
it).  A section placed at an address or an offset must fit in its section of
the file.

SHA is the first 12 hex digits of the SHA-256 of all the bytes the patch
replaces, taken in section order.  With it, a patch fails loudly on any other
build of the file, as every remaster.py edit does.  Without it, the patch is
refused and the error gives the value to write.

The tools are clang and ld.lld (Ubuntu's `clang`, `lld`).
"""
import hashlib
import os
import re
import struct
import subprocess
import tempfile

TARGET = ['--target=powerpcle-unknown-linux-gnu', '-mcpu=604']
CFLAGS = ['-O2', '-ffreestanding', '-fno-builtin', '-fno-pic', '-fno-pie', '-msoft-float', '-mno-altivec',
          '-fno-asynchronous-unwind-tables', '-fno-stack-protector', '-fno-jump-tables',
          '-mllvm', '-combiner-store-merging=false']  # a misaligned access faults in little-endian mode


class ObjPatchError(Exception):
    pass


class Elf:
    """The sections, symbols and PLT stubs of a 32-bit ELF file."""

    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 1:
            raise ObjPatchError('not a 32-bit ELF file')
        self.data = data
        e = self.e = '>' if data[5] == 2 else '<'
        shoff, = struct.unpack_from(e + 'I', data, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(e + 'HHH', data, 0x2E)
        raw = [struct.unpack_from(e + '10I', data, shoff + i * shentsize) for i in range(shnum)]

        def cstr(off):
            return data[off:data.index(b'\0', off)].decode('latin1')

        self.secs = [dict(name=cstr(raw[shstrndx][4] + s[0]), type=s[1], flags=s[2], addr=s[3], off=s[4],
                          size=s[5], link=s[6]) for s in raw]
        self.syms = {}  # name -> address
        addrs = set()
        for s in self.secs:
            if s['type'] not in (2, 11):  # SYMTAB, DYNSYM
                continue
            strtab = self.secs[s['link']]['off']
            for k in range(s['size'] // 16):
                nm, val, _, info, _, shndx = struct.unpack_from(e + 'IIIBBH', data, s['off'] + k * 16)
                if not nm or not 0 < shndx < len(self.secs) or (info & 0xF) in (3, 4):  # SECTION, FILE
                    continue
                name = cstr(strtab + nm)
                self.syms.setdefault(name, val)
                addrs.add(val)
        self.plt = {}
        for s in self.secs:
            if s['type'] == 4 and s['name'] == '.rela.plt':
                symsec = self.secs[s['link']]
                strtab = self.secs[symsec['link']]['off']
                for k in range(s['size'] // 12):
                    off, info, _ = struct.unpack_from(e + 'IIi', data, s['off'] + k * 12)
                    nm, = struct.unpack_from(e + 'I', data, symsec['off'] + (info >> 8) * 16)
                    self.plt[cstr(strtab + nm)] = off
        self.addrs = sorted(addrs | {s['addr'] + s['size'] for s in self.secs if s['flags'] & 2})
        phoff, = struct.unpack_from(e + 'I', data, 0x1C)
        phentsize, phnum = struct.unpack_from(e + 'HH', data, 0x2A)
        self.phoff, self.phentsize = phoff, phentsize
        self.segs = []  # PT_LOAD: dict(i, off, vaddr, filesz, memsz)
        for i in range(phnum):
            ptype, off, vaddr, _, filesz, memsz, _, _ = struct.unpack_from(e + '8I', data, phoff + i * phentsize)
            if ptype == 1:
                self.segs.append(dict(i=i, off=off, vaddr=vaddr, filesz=filesz, memsz=memsz))

    def section_at(self, addr):
        """The section, or failing that the PT_LOAD segment, whose file contents hold `addr`:
        a dict with addr, off and size.  A segment covers code put in its slack past its last
        section (grow_segment)."""
        for s in self.secs:
            if s['flags'] & 2 and s['type'] != 8 and s['addr'] <= addr < s['addr'] + s['size']:  # NOBITS
                return s
        for g in self.segs:
            if g['vaddr'] <= addr < g['vaddr'] + g['filesz']:
                return dict(addr=g['vaddr'], off=g['off'], size=g['filesz'])
        raise ObjPatchError(f'{addr:#x} is in no loaded section or segment with file contents')

    def resolve(self, spec):
        """(address, limit): where `.patch.<spec>` goes and the first address past its room."""
        name, plus, add = spec.partition('+')
        if re.fullmatch(r'0x[0-9a-fA-F]+', name):
            addr, by_symbol = int(name, 16), False
        elif name in self.syms:
            addr, by_symbol = self.syms[name], True
        else:
            raise ObjPatchError(f'no symbol {name}')
        addr += int(add, 0) if plus else 0
        sec = self.section_at(addr)
        limit = sec['addr'] + sec['size']
        if by_symbol or plus:
            # At a symbol, or inside one: no further than the next symbol.
            limit = min(limit, next((a for a in self.addrs if a > addr), limit))
        return addr, limit

    def offset(self, addr):
        s = self.section_at(addr)
        return s['off'] + addr - s['addr']


def grow_segment(data, index, size):
    """Grow PT_LOAD segment `index` (counting PT_LOADs from 0) to `size` bytes, file and memory,
    over bytes the file already has and that are all zero (alignment padding), short of the
    next segment's addresses and file contents.  Nothing moves."""
    elf = Elf(data)
    if not 0 <= index < len(elf.segs):
        raise ObjPatchError(f'grow-segment: no PT_LOAD {index}')
    g = elf.segs[index]
    if g['filesz'] != g['memsz']:
        raise ObjPatchError('grow-segment: the segment has a zero-fill part (memsz > filesz)')
    if size < g['filesz']:
        raise ObjPatchError('grow-segment: smaller than the segment already is')
    end_off, end_va = g['off'] + size, g['vaddr'] + size
    for h in elf.segs:
        if h is not g and (h['off'] < end_off and h['off'] + h['filesz'] > g['off']
                           or h['vaddr'] < end_va and h['vaddr'] + h['memsz'] > g['vaddr']):
            raise ObjPatchError(f'grow-segment: {size:#x} bytes would overlap PT_LOAD {elf.segs.index(h)}')
    if any(data[g['off'] + g['filesz']:end_off]):
        raise ObjPatchError('grow-segment: the bytes to take in are not all zero')
    out = bytearray(data)
    struct.pack_into(elf.e + 'II', out, elf.phoff + g['i'] * elf.phentsize + 16, size, size)
    return bytes(out)


class Raw:
    """A file of raw code loaded at `base` (no ELF headers, no symbols), as the
    resolve()/offset() interface build() needs.  Its sections must say how much
    room they have: `.patch.0xSTART-0xEND`."""

    def __init__(self, data, base):
        self.data, self.base, self.syms, self.plt = data, base, {}, {}

    def resolve(self, spec):
        m = re.fullmatch(r'(0x[0-9a-fA-F]+)-(0x[0-9a-fA-F]+)', spec)
        if not m:
            raise ObjPatchError(f'.patch.{spec}: a raw file needs .patch.0xSTART-0xEND')
        lo, hi = int(m.group(1), 16), int(m.group(2), 16)
        if not (self.base <= lo < hi <= self.base + len(self.data)):
            raise ObjPatchError(f'.patch.{spec}: outside the file ({self.base:#x}..{self.base + len(self.data):#x})')
        return lo, hi

    def offset(self, addr):
        return addr - self.base


def _sections(obj):
    """The allocated sections of a relocatable object: [(name, size)]."""
    o = Elf(obj)
    return [(s['name'], s['size']) for s in o.secs if s['flags'] & 2 and s['size']]


def build(data, src, expect=None, raw_base=None):
    """Assemble or compile `src` and write it into `data` (an ELF file, or with
    `raw_base`, raw code loaded at that address: remaster.py's `asm-raw`)."""
    elf = Elf(data) if raw_base is None else Raw(data, raw_base)
    ident = re.compile(r'[A-Za-z_][A-Za-z0-9_]*$')
    with tempfile.TemporaryDirectory() as tmp:
        obj, out, lds = (os.path.join(tmp, n) for n in ('patch.o', 'patch.elf', 'patch.ld'))
        cmd = ['clang'] + TARGET + ['-c', src, '-o', obj]
        if src.endswith('.c'):
            # A `/* cflags: ... */` comment in the source adds to the flags (say, -Os
            # for code that has to fit a small cave).
            m = re.search(r'/\* cflags: ([^*]*)\*/', open(src).read())
            cmd[1:1] = CFLAGS + (m.group(1).split() if m else [])
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise ObjPatchError(f'{" ".join(cmd)}\n{r.stderr}')
        secs = _sections(open(obj, 'rb').read())
        places = []
        for name, size in secs:
            if not name.startswith('.patch.'):
                raise ObjPatchError(f'{src}: section {name} is not a .patch.<where> section')
            addr, limit = elf.resolve(name[len('.patch.'):])
            if addr + size > limit:
                raise ObjPatchError(f'{name}: {size:#x} bytes at {addr:#x} overrun the room there ({limit - addr:#x})')
            places.append((name, addr, size))
        spans = sorted((a, a + n, nm) for nm, a, n in places)
        for (a0, e0, n0), (a1, e1, n1) in zip(spans, spans[1:]):
            if a1 < e0:
                raise ObjPatchError(f'{n0} ({a0:#x}..{e0:#x}) overlaps {n1} at {a1:#x}')
        with open(lds, 'w') as f:
            for n, v in sorted(elf.syms.items()):
                if ident.match(n):
                    f.write(f'{n} = {v:#x};\n')
            for n, v in sorted(elf.plt.items()):
                if ident.match(n):
                    f.write(f'PLT_{n} = {v:#x};\n')
            f.write('SECTIONS {\n')
            for name, addr, _ in sorted(places, key=lambda p: p[1]):  # lld lays files out in order
                f.write(f'  "{name}" {addr:#x} : {{ *("{name}") }}\n')
            f.write('  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }\n}\n')
        r = subprocess.run(['ld.lld', '-static', '-nostdlib', '--no-check-sections', '-T', lds, obj, '-o', out],
                           capture_output=True, text=True)
        if r.returncode:
            raise ObjPatchError(f'ld.lld: {r.stderr}')
        linked = Elf(open(out, 'rb').read())
    old = b''.join(data[elf.offset(a):elf.offset(a) + n] for _, a, n in places)
    sha = hashlib.sha256(old).hexdigest()[:12]
    if expect is None:
        raise ObjPatchError(f'asm {os.path.basename(src)}: give the replaced bytes\' hash, {sha}')
    if expect != sha:
        raise ObjPatchError(f'asm {os.path.basename(src)}: the bytes to replace hash to {sha}, not {expect}')
    patched = bytearray(data)
    report = []
    for name, addr, size in places:
        s = next(s for s in linked.secs if s['name'] == name)
        code = linked.data[s['off']:s['off'] + s['size']]
        off = elf.offset(addr)
        patched[off:off + size] = code
        report.append(f'{name[7:]} {addr:#x}+{size:#x}')
    return bytes(patched), report
