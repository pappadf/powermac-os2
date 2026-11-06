# powermac-os2

Remaster IBM's OS/2 Warp (PowerPC Edition) CD so that it installs and boots
on Apple hardware: patches for the kernel, the boot loader and the drivers,
plus a small Open Firmware boot program.

OS/2 for the PowerPC was written for IBM's PReP machines. It expects PC
devices (8259 interrupt controllers, a 16550 serial port, an 8042 keyboard
controller, an MC146818 clock), PReP's address map and PReP firmware. An
Apple Network Server has none of these. This project leaves the machine
**unmodified** and changes OS/2 instead. It patches the IBM Microkernel, the
boot loaders and a few drivers on the CD to use the Mac's own hardware, which
is Bandit PCI, Grand Central, the ESCC serial port and Cuda/ADB.

## Status

Tested on an emulated Apple Network Server 500 (the
[granny-smith](https://github.com/pappadf/granny-smith) emulator, stock
machine model). Real hardware hasn't been tried yet. What works:

- Open Firmware loads the boot floppy, which starts IBM's boot loader from
  the CD or from an installed disk.
- The kernel and its drivers come up: SCSI on the first fast/wide bus, the
  serial console on the Mac's serial port, video on the on-board Cirrus 54M30,
  and the keyboard over ADB.
- The installer runs (Easy Installation, 2,045 files) onto a disk at SCSI
  ID 6.
- The installed system boots from that disk to the Workplace Shell and
  accepts keys.

Not working yet:
- **Mouse:** ADB mouse movement doesn't reach OS/2 yet.
- **Second SCSI bus:** only the first fast/wide bus is used. The second
  53C825A is hidden.
- **Persistence:** OS/2's NVRAM reads as blank, and the clock runs from the
  time base without being saved.
- **Other Macs:** only the Network Server 500/700 is handled. Other
  Bandit/Grand Central Power Macs (7500/8500/9500) are close relatives but
  are untested.

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

## Booting on a Network Server

**Once per machine**, put Open Firmware in little-endian mode. These are the
same settings Windows NT uses on this machine:

```
setenv little-endian? true
setenv real-mode? false
setenv real-base 3F00000
setenv load-base 3E00000
reset-all
```

**To install:**
1. Put the patched CD at SCSI ID 3 and a blank disk at **SCSI ID 6** on the
   first fast/wide bus. ID 6 matters: IBM's disk loader expects that ID.
2. Insert the floppy, and at the `0 >` prompt type:
   ```
   load fd:,\os2boot.of
   load-base loadsize eval
   ```
3. The boot loader talks on the serial port (ttya). Answer its prompts:
   - Enter for a manual boot
   - `cd3` for `PN_BOOT_DEV`
   - `cdrom` for `PN_BOOT_FS`
   - `boot.sci` for `PN_BOOT_CONFIG`
   - Enter through the configuration lines, and once more to start the
     kernel.
4. The installer appears on the screen. Easy Installation takes three
   Enters, then copies the system to the disk.

**To boot the installed system**, remove the CD and boot the same floppy.
It finds the disk's PReP boot partition, and the disk's own loader starts
OS/2 without any prompts.

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
