# The shim: OS/2 from Open Firmware

`SHIM.EXE` is a small little-endian PowerPC program. The Apple Network
Server's Open Firmware 2.26NT loads it from the boot floppy. It hands IBM's
boot image the state that IBM's PReP firmware and boot stub would have given
it. It needs no PReP firmware and no lane-reversing host bridge, only the
Mac's own little-endian semantics.

It also installs the **loader service** (`ldrsvc.c`) at `$40000`. Patch 90
turns the boot loader's PC-port, PCI-configuration and NVRAM routines into
branches to it, and it does those jobs on the Network Server's hardware:

- COM1 is a 16550 emulated on the ESCC.
- The VGA ports go to Bandit 1's I/O space.
- PCI configuration uses Bandit configuration cycles.
- The NVRAM reads as blank.

`make` here builds `build/SHIM.EXE` and `build/os2shim.img`. The top-level
`make shim` does the same and copies the floppy to `../build/`. See the
top-level README for booting.

## What it does

| Step | Where | What |
|---|---|---|
| 1 | `os2boot.of` | Reads `SHIM.EXE` off the floppy with `read-blocks` (its sectors are filled in by `mkfloppy.sh`), moves it to load-base, `init-program` (the ROM's PE loader lays it out at 0x50000), `go`. This is the NT boot floppy's own mechanism |
| 2 | `shim.c` | Through the client interface: `/chosen` `stdout`; RAM from `/memory` `reg`; `claim` 0x700000, 0x800000 and the 0xFF000 page. Then, like PReP firmware working through its boot list, it scans `/bandit/53c825@11/sd@0`–`6` for the first device whose partition table has a type-0x41 PReP boot partition. A fresh install finds the CD (ID 3); an installed system's disk comes first. It reads that partition's header (entry offset and image length at +0x200) and the image, to 0x800000 |
| 3 | `start.S` | Leaves Open Firmware: interrupts off, then four BATs: 0–256 MB identity, `$80000000` (Bandit 1's PCI memory, where the SCSI chip's registers are) and `$A0000000` cache-inhibited, and `$F0000000`–`$FFFFFFFF` for the Mac's own I/O (Bandit's ports, Grand Central) |
| 4 | `shim.c` | **The stub's work**, reproduced exactly (see below): residual data at 0x700000, its one big-endian store into the image, the per-word byte reversal of its little-endian trampoline, its stack frames (with the image-dependent words computed for the image loaded), cache flushes,  |
| 4a | `shim.c` | **What PReP firmware leaves for the loader, on this machine's hardware:** the first 53C825A's memory BAR at `$8F000000` and I/O BAR at `$20000000`, with decoding and bus mastering on; the 54M30's display memory at `$82000000`, with its I/O and memory decoders on (the disk's loader programs the VGA DAC); and the loader service copied to `$40000` |
| 5 | `start.S` | The stub's hand-off: segment registers zeroed, I-cache flash-invalidated, ILE on, the registers it leaves (r3 = entry, r4 = swapped length, r14 = residual, r15 = its length, r16 = 5 for Sandalfoot, and so on), `rfi` to 0x85F2A8 with MSR 0x11031 |

### Reproducing the stub exactly

Everything the stub does was established by **diffing a reference run**: IBM's
stub executing for real on a lane-reversing PReP bridge (an emulated one), stopped
where it enters its little-endian body. At that point:

- **Registers:** set as listed in step 5.
- **Memory:**
  1. **One big-endian store:** at entry + 0xC20, the word becomes the sum of
     the words at +0xC14 and +0xC1C. Here that's 0x554C.
  2. **A byte-reversed region:** that word minus 0x46A8 (0xEA4 bytes) of the
     body, from 0x85F2A8, is byte-reversed word by word.
  3. **Its stack:** 55 bytes at 0xFFFC9–0xFFFFF.

  Nothing else changes. The residual block is only flushed from cache.

The image this shim produces matches the reference hand-off byte for byte.
The disk's boot image carries the same stub code with different embedded
sizes, so every constant above is derived from the image: the entry from
its header, the byte-reversal length as the word at +0xC1C less the body's
offset (+0x6A8), and the stack's addresses relative to the entry.

### Why this works on a Mac

On PReP, the boot stub's output is read through a host bridge that reverses
memory byte lanes in little-endian mode. On a Mac it is read through the 60x's
address munging alone. The shim runs in that same munged little-endian mode,
so plain little-endian C stores leave memory exactly as the loader will read
it. Two encodings stay big-endian, as IBM's big-endian firmware writes them:

- the **residual data's numbers**, which the loader converts itself
- the stub's **one store**.

Open Firmware's own conventions apply where they apply: property values are
big-endian (`encode-int`), and the client interface's argument array is
native.

## Lessons learned

- **CRLF line endings in `os2boot.of`.** This firmware's `\` comment runs to a
  carriage return. With bare line feeds the first comment swallows the whole
  script, and `eval` silently does nothing.
- **Image base.** 0x600000 and 0x500000 were not claimable for the PE image.
  0x50000, where the NT veneer runs, is.
- **Property integers are big-endian** in little-endian mode too. A swapped
  `stdout` ihandle means a silent console; swapped `/memory` sizes mean a
  4-byte machine.
- **Misaligned accesses fault in little-endian mode** (a 604 alignment
  exception). Build with `-mllvm -combiner-store-merging=false`, or the
  compiler merges byte stores into misaligned words. Loads merge too: the
  partition table's unaligned fields are read through a volatile byte
  helper (`le32_bytes`).

## Files

- `start.S`: the entry, the BAT switch and the hand-off.
- `shim.c`: client-interface calls, the residual data, and the stub's memory
  effects.
- `shim.ld`, `mkpe.py`: a flat image at 0x50000, wrapped in the bare PE
  shape the ROM's pe-loader takes. That is a COFF header at offset 0 with
  machine 0x01F0, one section at RVA 0, and the entry at RVA 0, the same as
  the NT veneer.
- `ldrsvc.c`, `ldrsvc.ld`, `ldrsvc_blob.S`: the loader service, linked at
  `$40000` and embedded in the shim.
- `os2boot.of`, `mkfloppy.sh`: the floppy.
- `residual.h`, `pnp.h`: NetBSD's PReP headers (BSD licence), with
  `unsigned long` narrowed to `uint32_t`.
