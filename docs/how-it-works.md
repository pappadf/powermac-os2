# How it works

OS/2 Warp (PowerPC Edition) was built for IBM's PReP machines: a 604 or 603
on a PCI bus with an ISA bridge, PC devices behind it, and PReP firmware that
hands the boot loader a residual-data description of the machine. This
project makes it run on an Apple Network Server 500/700. That machine has a
604, two Bandit PCI bridges, a Grand Central I/O controller, two 53C825A SCSI
chips, a Cirrus 54M30 video chip and Open Firmware 2.26NT.

The machine is not changed. Each piece of PReP hardware OS/2 uses is answered
on the OS/2 side instead.

## The boot path

1. **Open Firmware** loads `os2boot.of` from the floppy, which loads
   `SHIM.EXE` (`shim/`).
2. **The shim:**
   - finds the first SCSI device with a PReP boot partition (the CD at ID 3,
     or an installed disk);
   - reads IBM's boot image;
   - does what PReP firmware and IBM's boot stub would have done: residual
     data for an IBM "Sandalfoot", the stub's memory effects, and the
     hand-off registers;
   - assigns the first 53C825A's and the 54M30's BARs, and switches on their
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
  - PReP slot 12 is the first 53C825A, presented as a 53C810 (which has no
    third BAR).
  - Slot 13 is the 54M30, presented as a CL-GD5434 (device ID and CR27).
  - Memory BARs are translated between PReP's numbering (CPU `$C0000000` +
    m is PCI m) and Bandit's (CPU and PCI `$80000000` + m).
- **Ports `$60`/`$64`** go to the 8042 emulation (patch 98).
- **The NVRAM ports** read blank.
- **Anything else** reads all-ones.

### Interrupts (patch 80)

OS/2 keeps its 8259-style IRQ numbers, and a table maps them onto Grand
Central sources:
- IRQ 13 is the first 53C825A.
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
