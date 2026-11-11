# The shim: OS/2 from Open Firmware

`SHIM.XCF` is a small little-endian PowerPC program, an XCOFF that Open
Firmware's own `xcoff-loader` loads from the boot floppy: the Network
Server's production ROM (1.1.22) or its 2.26NT ROM, or a Power Macintosh
7500/8500/9500's Open Firmware 1.0.5. It hands IBM's
boot image the state that IBM's PReP firmware and boot stub would have given
it. It needs no PReP firmware and no lane-reversing host bridge, only the
Mac's own little-endian semantics.

It also installs the **loader service** (`ldrsvc.c`) at `$40000`. Patch 90
turns the boot loader's PC-port, PCI-configuration and NVRAM routines into
branches to it, and it does those jobs on the Mac's hardware:

- COM1 is a 16550 emulated on the ESCC.
- The VGA ports go to Bandit 1's I/O space (only when a 54M30 decodes them).
- PCI configuration uses Bandit configuration cycles.
- The NVRAM reads as blank.
- The loader's disk (its SCSI module's chip set-up, target probe and block
  transfers) goes through `scsi.c`, the shim's own polled driver: the
  53C825A by its SCRIPTS processor, or a Power Macintosh's MESH with DBDMA.
  The hard disk is the one the shim booted from, at any SCSI ID.

The shim reads the boot partition with the same `scsi.c`. On a Power
Macintosh without a display card it also sets up the on-board Control
(`control.c`): 640x480 in 8-bit colour, and a shadow frame buffer in RAM
that it keeps from OS/2 (see `docs/how-it-works.md`).

`make` here builds `build/SHIM.XCF` and `build/os2shim.img`. The top-level
`make shim` does the same and copies the floppy to `../build/`. See the
top-level README for booting.

## What it does

| Step | Where | What |
|---|---|---|
| 1 | `os2boot.of` | Reads `SHIM.XCF` off the floppy with `read-blocks` (its sectors are filled in by `mkfloppy.sh`), claims and maps its pages through `/chosen`'s `memory` and `mmu` instances, moves it to load-base, and calls the ROM's `xcoff-loader` `init-program` (which lays it out at 0x50000), then `go`. This is the NT boot floppy's mechanism on the production ROM |
| 2 | `shim.c` | Through the client interface: `/chosen` `stdout`; RAM from `/memory` `reg`; `claim` 0x700000, 0x800000 and the 0xFF000 page |
| 3 | `start.S` | Leaves Open Firmware: interrupts off, then four BATs: 0–256 MB identity, `$80000000` (Bandit 1's PCI memory, where the SCSI chip's registers are) and `$A0000000` cache-inhibited, and `$F0000000`–`$FFFFFFFF` for the Mac's own I/O (Bandit's ports, Grand Central) |
| 3a | `shim.c` | **The boot device, without Open Firmware.** The SCSI chip is the Network Server's first soldered 53C825A (Bandit 1 device 17) where there is one, otherwise a 53C825A card in the first PCI slot (device 13: a Power Macintosh's A1), otherwise a Power Macintosh's internal bus, MESH. The shim sets a 53C825A's BARs, and drives either chip itself with `scsi.c` (select, IDENTIFY, the command, the data, status, message in; a SCRIPTS program on the 53C825A, the FIFO and DBDMA on MESH): a Power Macintosh's Open Firmware has no driver for a card without FCode. Like PReP firmware working through its boot list, it reads sector 0 of SCSI IDs 0–6 for the first partition table with a type-0x41 PReP boot partition, hard disks before CDs. A fresh install finds the CD (ID 3); an installed system's disk comes first. It reads that partition's header (entry offset and image length at +0x200) and the image, to 0x800000 |
| 4 | `shim.c` | **The stub's work**, reproduced exactly (see below): residual data at 0x700000, its one big-endian store into the image, the per-word byte reversal of its little-endian trampoline, its stack frames (with the image-dependent words computed for the image loaded), cache flushes,  |
| 4a | `shim.c` | **What PReP firmware leaves for the loader, on this machine's hardware:** the 53C825A's memory BAR at `$8F000000` and I/O BAR at `$20000000`, with decoding and bus mastering on (step 3a); the 54M30's (device 15: soldered on the Network Server, a card in a Power Macintosh's C1) display memory at `$82000000`, with its I/O and memory decoders on (the disk's loader programs the VGA DAC); and the loader service copied to `$40000` |
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
- **Image base.** 0x600000 and 0x500000 were not claimable for the image.
  0x50000, where the NT veneer runs, is.
- **XCOFF headers are little-endian.** A firmware running with
  `little-endian? true` reads them with little-endian loads; the a.out entry
  is a function descriptor that the loader jumps through.
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
- `shim.c`: client-interface calls, the 53C825A reader, the residual data,
  and the stub's memory effects.
- `shim.ld`, `mkxcoff.py`: a flat image at 0x50000, wrapped in an XCOFF
  with one `.text` holding the whole image, an empty `.bss`, and the entry
  descriptor after the image.
- `ldrsvc.c`, `ldrsvc.ld`, `ldrsvc_blob.S`: the loader service, linked at
  `$40000` and embedded in the shim.
- `os2boot.of`, `mkfloppy.sh`: the floppy.
- `prep-residual.h`: the layout of PReP's residual data, as much as the shim
  fills, written for this project from the PReP specification's layout, with
  every offset the loader reads checked at compile time.
