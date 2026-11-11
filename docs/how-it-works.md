# How it works

OS/2 Warp (PowerPC Edition) was built for IBM's PReP machines: a 604 or 603
on a PCI bus with an ISA bridge, PC devices behind it, and PReP firmware that
hands the boot loader a residual-data description of the machine. This
project makes it run on an Apple Network Server 500/700. That machine has a
604, two Bandit PCI bridges, a Grand Central I/O controller, two 53C825A SCSI
chips, a Cirrus 54M30 video chip and Open Firmware (1.1.22 in production).

A Power Macintosh 7500/8500/9500 is the same board family: Bandit, Grand
Central, Cuda and the ESCC at the same addresses, and Open Firmware 1.0.5.
What it lacks is OS/2-usable SCSI and video, so it gets the Network Server's
two chips as PCI cards: a 53C825A card in slot A1 (Bandit 1 device 13, Grand
Central source 23, exactly the Network Server's slot 1) and a 54M30 card in
C1 (device 15, where the Network Server solders its own). Everything below
finds the 53C825A at device 17 if the Network Server's is there, else at 13.

The machine is not changed. Each piece of PReP hardware OS/2 uses is answered
on the OS/2 side instead.

## The boot path

1. **Open Firmware** loads `os2boot.of` from the floppy, which loads
   `SHIM.XCF` (`shim/`) through the ROM's own `xcoff-loader`.
2. **The shim:**
   - leaves Open Firmware, and drives the 53C825A itself (Open Firmware on a
     Power Macintosh has no driver for a SCSI card without FCode);
   - finds the first SCSI device with a PReP boot partition (the CD at ID 3,
     or an installed disk);
   - reads IBM's boot image;
   - does what PReP firmware and IBM's boot stub would have done: residual
     data for an IBM "Sandalfoot", the stub's memory effects, and the
     hand-off registers;
   - assigns the 53C825A's and the 54M30's BARs, and switches on their
     decoders;
   - installs a **loader service** at `$40000`;
   - jumps to the loader.
3. **IBM's boot loader**, either `BL_AUTO` on the CD or `/OS2/FAT_BL.HD` on
   an installed disk, reads `BOOT.SCI`/`BOOT.CFG` and loads the Microkernel
   and its modules. Patch 90 turns its PC-port, PCI-configuration and NVRAM
   routines into branches to the loader service. It also points its SCSI
   code at the chip's memory BAR, and removes PReP's DMA offset.
4. **The IBM Microkernel** and the OS/2 personality start. Patches 60–98
   replace the PReP hardware the kernel uses.

## What replaces what

| PReP hardware | Replaced by | Where |
|---|---|---|
| 16550 COM1 console | The ESCC's channel A (Open Firmware's ttya) | patch 60 (`MTTY.DYN`), the loader service |
| PCI configuration (`$CF8`/`$CFC`, direct-mapped `$80800000`) | Bandit 1's configuration cycles | patch 70 (the kernel's own routines), patch 97 (everything else), the loader service |
| Two 8259s and the interrupt-acknowledge register | Grand Central's interrupt controller | patch 80 |
| MC146818 clock, NVRAM, 8254, power management, IBM's L2 cache | Answered in the kernel: the clock from the time base, the NVRAM blank, the rest stubbed out | patch 85 |
| PReP's PCI I/O window (`$A0000000`) and memory window (`$C0000000`) | Bandit 1's memory window | patch 95 |
| Spread ISA ports and direct configuration, from any task | A trap that emulates them | patch 97 |
| 8042 keyboard and mouse controller | Cuda and ADB | patch 98 |
| A 53C810, on a Power Macintosh without a SCSI card | MESH, behind a stand-in 53C810 | the shim, the loader service, patches 96 and 97 |
| A PCI display adapter, on a Power Macintosh without a card | Control, through a shadow frame buffer | the shim, patches 57, 95 and 97 |
| PReP's DMA offset (bus address = `$80000000` + physical) | None: Bandit's bus addresses are physical | patches 85, 90 and 97 |

### The trap (patch 97)

OS/2's user-mode drivers reach PC ports through LIBDDPIO. LIBDDPIO maps
PReP's "spread" ISA map (port p at `$80000000 + ((p & $FFE0) << 7) | (p & $1F)`)
and the direct-mapped configuration space. No page mapping onto the Mac's
hardware can express these. So the trap works like this:

- **Redirect.** Every mapping of them is pointed at a window of Bandit
  memory that nothing decodes (`$88000000` + the offset).
- **Fault.** Those pages are mapped no-access (`pmap_enter`), so any access
  takes a protection fault.
- **Emulate.** `ds_slih` hands the fault to `trap_ds`. That decodes the load
  or store, does it on the Network Server's hardware, writes back the
  register and steps past the instruction.

The kernel's own fixed ISA window (BAT2) gets the same treatment.

Behind the trap:
- **Ports `$3B0`–`$3DF`** (VGA) go to Bandit 1's I/O space, except
  `$3BC`–`$3BF`.
- **PCI configuration** goes to Bandit 1:
  - PReP slot 12 is the 53C825A (device 17, or a card's 13), presented as
    a 53C810 (which has no third BAR).
  - Slot 13 is the 54M30, presented as a CL-GD5434 (device ID and CR27).
  - Memory BARs are translated between PReP's numbering (CPU `$C0000000` +
    m is PCI m) and Bandit's (CPU and PCI `$80000000` + m).
- **Ports `$60`/`$64`** go to the 8042 emulation (patch 98).
- **The NVRAM ports** read blank.
- **Anything else** reads all-ones.

### Interrupts (patch 80)

OS/2 keeps its 8259-style IRQ numbers, and a table maps them onto Grand
Central sources:
- IRQ 13 is the 53C825A: source 22 for the Network Server's first chip,
  source 23 for a card in the first slot. Only one is ever there.
- IRQ 1 and 12 are the VIA (Cuda), for the emulated keyboard and mouse.

Grand Central runs in clear mode 1, so edge sources latch. Level IRQs follow
the source's level. The clock re-arms an edge source whose line stayed
asserted.

### The I/O windows (patch 95)

Every mapping of PReP's linear windows passes through one place: the I/O
pager's physical-address callback. Only the physical address is remapped
there. The PReP address stays the offset that the faulting thread looks
pages up by.

- **`$A0000000`** (the SCSI chip's PReP I/O BAR) maps to the chip's memory
  BAR at `$8F000000`.
- **`$C0000000` + m** (PCI memory) maps to `$80000000` + m.
- **The spread ISA and configuration ranges** map to the trap window.

### The keyboard (patch 98)

The trap passes ports `$60` and `$64` to `kbd_port`, which keeps an 8042's
state:

- **Reports in.** A status read collects any report Cuda has auto-polled
  from the ADB keyboard or mouse. A keyboard report becomes scan code set 1;
  a mouse report becomes a PS/2 packet.
- **Commands.** Controller and device commands are answered the way an 8042
  and a PS/2 keyboard and mouse would answer them.
- **Delivery.** While a byte waits whose channel has its interrupt enabled
  in the command byte, VIA timer 2 (unused otherwise) keeps the VIA's Grand
  Central source raised. When OS/2 takes that source, `kbd_irq` names it
  IRQ 12 if the byte at the head is the mouse's, else IRQ 1.
- **Visibility.** Inside an interrupt (the kernel's in-interrupt flag), the
  status and data ports show only a byte for the IRQ being taken. A driver
  polling for a reply outside an interrupt sees every byte. IBM's keyboard
  handler reads `$60` without checking the status, so this keeps it from
  eating mouse bytes, and keeps interrupt handlers from taking a reply a
  driver is polling for.
- **Mouse packets** are queued whole, and only while the mouse interrupt is
  enabled, so they can't hold up keystrokes during boot.

## A Power Macintosh without cards

A stock 7500/8500/9500 has neither chip. Its internal SCSI bus is MESH, a
cell in Grand Central, and its display is Control, behind the Chaos bridge.
OS/2 has drivers for neither, so each is put behind something OS/2 already
knows, and only the code that touches the chip changes.

**SCSI on MESH.**
- **Before the kernel.** `shim/scsi.c` is one polled driver for both chips,
  used by the shim and by the loader service. On MESH it sends the message,
  command and status bytes through MESH's FIFO and moves the data with DBDMA
  channel 10, as Linux and MkLinux do. Patch 90 sends the loader's three disk
  routines (set the chip up, probe a target, read or write blocks) to it, on
  every machine.
- **The device.** With no 53C825A, the trap presents a 53C810 in slot 12.
  The hardware database binds the NCR driver to it, as anywhere else. Its one
  BAR, 256 bytes of I/O at PReP `$80FFF000`, is a page of the trap window.
  There MESH's sixteen registers keep their offsets, and DBDMA channel 10's
  control, status and command pointer take three of the words between them.
- **The driver.** Patch 96 changes the NCR driver only where it touches the
  chip, and only when the chip is MESH (its range is at `$80FFF000`). The
  chip resets become MESH's and a SCSI bus reset. Starting a command block
  hands it to a worker thread, which takes the place of the interrupt
  thread. The worker runs each command start to finish, polled. It then
  finishes the block the way the interrupt thread finishes a SCRIPTS
  completion. MESH's interrupt stays masked.

**The display on Control.**
- **The mode.** The shim sets 640x480 in 8-bit colour: the ROM's 640x480
  raster, the pixel clock over Cuda's I²C, the VRAM banks probed as Linux
  probes them.
- **The byte order.** Chaos, unlike Bandit, doesn't reverse its byte lanes.
  From the CPU's little-endian mode, each aligned 8 bytes of an image would
  reach VRAM mirrored.
- **The shadow.** So OS/2 draws into a shadow frame buffer: 1 MB of RAM at
  `$3F00000`, the top megabyte of the 64 MB OS/2 needs. The residual data
  tells OS/2 its RAM ends there: its kernel sizes memory from the total and
  ignores holes in the segment list. The trap presents
  Control in slot 13, with its own ID, and with the shadow as its memory BAR
  (PReP `$CE000000`, patch 95).
- **The copy.** On every clock tick, `kernel-video.c` compares a band of
  scan lines with what it last sent. It copies what changed to VRAM, each
  8 bytes byte-reversed, through data BAT 3, which the kernel doesn't use.
- **The palette.** OS/2 sets it through the VGA DAC ports, which the trap
  turns into the RaDACal's colour table.
- **OS/2's side.** Patch 57 gives the hardware database a stanza for
  Control and VIDEOPMI.LST an entry for it. It adds `CONTROL.PMI`, which
  leaves the mode as the shim set it. No accelerated GRADD claims its OEM
  string, so the generic one draws.

## Where the code goes

The patches replace code in place where they can. When they need more room:

- **Kernel cave.** The kernel's text segment is grown over the zero padding
  between `.rodata` and `.got` (`grow-segment`).
  `patches/src/kernel-cave.h` assigns that cave to the patches slice by
  slice.
- **Dead code.** Routines that have no work on this machine (IBM's L2-cache
  routines, the kernel debugger's register dump) are reduced to a `blr` or a
  constant, and their bodies hold new code.

`objpatch.py` refuses a patch whose code overruns its room or overlaps
another section, or whose replaced bytes don't hash as expected.

## Byte order

The 604 runs little-endian with address munging, and Bandit reverses its byte
lanes. So a plain load or store of the natural size reaches every device
register. OS/2's code already assumes exactly this of a PReP bridge.
