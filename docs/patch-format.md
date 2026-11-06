# Patch files and the code patcher

## `remaster.py`

Re-master the OS/2 Warp (PowerPC Edition) CD with patches.

```
remaster.py IN.iso OUT.iso PATCHFILE...
```

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

## `asm` and `asm-raw`: code patches (`objpatch.py`)

Patch an ELF file on the CD with newly assembled or compiled code.

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

## Writing a new patch

1. Name it `NN-what.patch`. Patches are applied in name order, and a later
   patch may depend on an earlier one; say so in its header comment.
2. Use `file /PATH` to select the file, then the operations.
3. For `asm`/`asm-raw`, leave the hash off the first time. The run then fails
   and prints the hash to write.
4. Rebuild with `make ISO=...`.
5. Kernel code that needs room takes a slice of the cave
   (`patches/src/kernel-cave.h`) or a dead routine's body. The build refuses
   any overlap.
