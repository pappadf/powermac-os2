# powermac-os2

> [!CAUTION]
> **Experimental. Do not run this on a real Mac you care about.**
> Booting it writes `little-endian? true` to Open Firmware's NVRAM, which Apple never supported.
> Changing this setting has been [reported](https://discussions.apple.com/thread/1085726) to
> leave machines needing an NVRAM reset, and at least one could not be recovered. This project
> has only been tested in the [granny-smith](https://github.com/pappadf/granny-smith) emulator,
> and it comes with no warranty (see the [licence](LICENSE), GPL-2.0 sections 11–12).

Remaster IBM's OS/2 Warp (PowerPC Edition) CD so that it installs and boots
on Apple hardware, an Apple Network Server 500/700 or a Power Macintosh
7500/8500/9500: patches for the kernel, the boot loader and the drivers, plus
a small Open Firmware boot program.

OS/2 for the PowerPC was written for IBM's PReP machines. It expects PC
devices (8259 interrupt controllers, a 16550 serial port, an 8042 keyboard
controller, an MC146818 clock), PReP's address map and PReP firmware. These
Macs have none of these. This project leaves the machine
**unmodified** and changes OS/2 instead. It patches the IBM Microkernel, the
boot loaders and a few drivers on the CD to use the Mac's own hardware, which
is Bandit PCI, Grand Central, the ESCC serial port and Cuda/ADB.

## Status

Tested in the [granny-smith](https://github.com/pappadf/granny-smith)
emulator:
- an Apple Network Server 500 (stock machine model), on the production ROM
  (Open Firmware 1.1.22) and on the 2.26NT ROM;
- a Power Macintosh 8500 with two PCI cards: a Symbios 53C825A SCSI card in
  slot A1 and a Cirrus Logic 54M30 VGA card in slot C1;
- a Power Macintosh 8500 as it ships, without cards: OS/2 uses the internal
  SCSI bus (MESH) and the on-board display (Control).

Real hardware hasn't been tried yet. What works:

- Open Firmware loads the boot floppy, which starts IBM's boot loader from
  the CD or from an installed disk.
- The kernel and its drivers come up: SCSI on the 53C825A (the Network
  Server's first fast/wide bus, or the card) or, without a card, on a Power
  Macintosh's internal bus (MESH); the serial console on the Mac's serial
  port; video on the Cirrus 54M30 (on-board, or the card) or, without one,
  on a Power Macintosh's on-board Control, 640x480 in 256 colours; and the
  keyboard and mouse over ADB.
- The installer runs (Easy Installation, 2,045 files) onto a disk at any
  SCSI ID.
- The installed system boots from that disk to the Workplace Shell, with
  the keyboard and the mouse (pointer movement and buttons), with or
  without a CD in the drive. Patch 45 fixes a startup race in IBM's OS/2
  server that, on the 8500, hung every boot before the desktop.

Not working yet:
- **Second SCSI bus:** only the first fast/wide bus is used. The second
  53C825A is hidden.
- **Persistence:** OS/2's NVRAM reads as blank, and the clock runs from the
  time base without being saved.
- **Other Macs:** only the Network Server 500/700 and the 7500/8500/9500
  (Bandit and Grand Central) are handled, and of those only the 500 and the
  8500 have been run. On a 7500/8500/9500 the cards, if used, must be in A1
  and C1.
- **A Power Macintosh's other SCSI bus:** the 53C94 (the external connector,
  and on a stock machine the CD-ROM drive) has no driver. Without a SCSI
  card, the CD must be on the internal bus to install.
- **On-board video:** one mode, 640x480 in 256 colours, drawn by OS/2's
  generic display driver into memory that the kernel copies to the screen.
  That memory is the top megabyte of the first 64 MB, so OS/2 then uses
  63 MB of RAM, and none past 64 MB.

## What you need

- **The CD.** You need an image of the December 1995 OS/2 Warp (PowerPC
  Edition) CD 1 (the "OS" disc), sha256
  `dd704876b696428e045c8c076ecebe1f83a5ac07389e5ef973d763f8950bc820`. This
  repository contains no IBM code. Every patch checks the bytes it replaces,
  so a different build of the CD is refused rather than mis-patched.
- **Tools:**
  - `clang`, `ld.lld` and `llvm-objcopy` (any recent LLVM; the PowerPC
    target is built in)
  - `python3`
  - `make`
  - mtools (`mformat`, `mcopy`) for the boot floppy

On Debian or Ubuntu:

```
apt install clang lld llvm python3 make mtools
```

## Building

```
make ISO=/path/to/os2-ppc-cd1.iso
```

This writes two files:
- `build/os2-ppc-mac.iso` is the patched CD. It is the original image edited
  in place: the boot partition and every untouched file stay byte-identical.
- `build/os2shim.img` is a 1.44 MB FAT12 boot floppy for Open Firmware.

`make shim` builds the floppy only, and `make check-iso ISO=...` just checks
the input image.

## Booting

**The machine.** On a Network Server, nothing to add: the CD and the disk go
on the first fast/wide bus. A Power Macintosh 7500/8500/9500 runs OS/2 as it
is: the disk and the CD go on the internal SCSI bus (MESH), and OS/2's
screen is the on-board video. Or, with cards: a 53C825A SCSI card in slot
**A1** (the CD and the disk then go on its bus) and a Cirrus Logic 54M30 VGA
card in slot **C1** (OS/2's screen). Neither card needs FCode: the shim
drives the SCSI chip itself, and Open Firmware's console stays on the
serial port.

**Memory:** at least 64 MB. The settings below put Open Firmware at 63 MB
(`real-base`), and a machine with less RAM does not come back from the reset.

**The first time**, the floppy puts Open Firmware in little-endian mode by
itself. On a machine that isn't set up yet, `os2boot.of` sets the same
variables Windows NT uses on this machine and resets:

```
setenv little-endian? true
setenv real-mode? false
setenv real-base 3F00000
setenv load-base 3E00000
setenv boot-device fd:,\os2boot.of
```

It also points `boot-device` at itself, so after the reset, and at every
later power-on while the floppy is in, the firmware boots it without
typing.

**To install:**
1. Put the patched CD at SCSI ID 3 and a blank disk at another ID (0 is a
   Power Macintosh's internal bay) on the same bus: the 53C825A's, or
   without a card the internal bus.
2. Insert the floppy, and at the `0 >` prompt (on the serial port, ttya, or
   the Mac's own screen) type:
   ```
   load fd:,\os2boot.of
   load-base loadsize eval
   ```
   On a machine not yet in little-endian mode this resets once and then
   continues by itself.
3. IBM's boot loader picks the CD on its own (patch 99): its defaults name
   `cd3`, `cdrom` and `boot.sci`, so no serial input is needed. Its log goes
   to the serial port (ttya).
4. The installer appears on the screen. Easy Installation takes three
   Enters, then copies the system to the disk.

**To boot the installed system**, boot the same floppy. It looks for a
PReP boot partition on the hard disks before the CDs, so it finds the
installed disk even with the install CD still in its drive, and the disk's
own loader starts OS/2 without any prompts.

## How it works

See [docs/how-it-works.md](docs/how-it-works.md) for the replacement of each
PReP device. The patch file format and the code-patching tool are covered in
[docs/patch-format.md](docs/patch-format.md), and the shim in
[shim/README.md](shim/README.md).

## Layout

| Path | What |
|---|---|
| `Makefile` | builds the CD and the floppy |
| `remaster/` | `remaster.py`, which edits files inside the ISO in place; `objpatch.py`, which compiles C or assembly and links it at a target file's own symbols; `iso9660.py` |
| `patches/NN-*.patch` | the patches, applied in order |
| `patches/src/` | the code they assemble and compile into the kernel, the loaders and the drivers |
| `shim/` | the Open Firmware boot program and its floppy, plus the loader service it installs for IBM's boot loader |
| `tools/` | helpers that generate patch sources |
| `docs/` | how it works |

## License

GPL-2.0-only; see [LICENSE](LICENSE). Every source file carries an
`SPDX-License-Identifier: GPL-2.0-only` header.

The repository contains no IBM code. You supply your own CD image, and the
patched image you build is for your own use.
