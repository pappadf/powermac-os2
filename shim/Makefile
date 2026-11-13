# SPDX-License-Identifier: GPL-2.0-only
# os2shim -- OS/2 Warp (PowerPC Edition) from Open Firmware on an Apple Network Server
CLANG  ?= clang
LLD    ?= ld.lld
OBJCOPY ?= llvm-objcopy
TARGET = --target=powerpcle-unknown-linux-gnu -mcpu=604
CFLAGS = $(TARGET) -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -fno-pie -msoft-float \
         -fno-asynchronous-unwind-tables -fno-stack-protector -fno-jump-tables -mno-altivec -I. \
         -mllvm -combiner-store-merging=false  # LE mode faults on every misaligned access
# stage0, the big-endian front end Open Firmware loads (stage0.c)
TARGET_BE = --target=powerpc-unknown-linux-gnu -mcpu=604
CFLAGS_BE = $(subst $(TARGET),$(TARGET_BE),$(CFLAGS))
B ?= build

all: $(B)/os2shim.img

$(B):
	mkdir -p $(B)
$(B)/start.o: start.S | $(B)
	$(CLANG) $(TARGET) -x assembler-with-cpp -c $< -o $@
# The loader service: linked where the loader calls it, embedded in the shim.
$(B)/ldrsvc.o: ldrsvc.c scsi.c ../patches/src/loader-svc.h | $(B)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(B)/ldrsvc.elf: $(B)/ldrsvc.o ldrsvc.ld
	$(LLD) -T ldrsvc.ld -static -nostdlib -o $@ $(B)/ldrsvc.o
$(B)/ldrsvc.bin: $(B)/ldrsvc.elf
	$(OBJCOPY) -O binary $< $@
$(B)/ldrsvc_blob.o: ldrsvc_blob.S $(B)/ldrsvc.bin
	$(CLANG) $(TARGET) -c -DBLOB=\"$(B)/ldrsvc.bin\" $< -o $@
$(B)/shim.o: shim.c scsi.c control.c prep-residual.h ../patches/src/loader-svc.h | $(B)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(B)/shim.elf: $(B)/start.o $(B)/shim.o $(B)/ldrsvc_blob.o shim.ld
	$(LLD) -T shim.ld -static -nostdlib -o $@ $(B)/start.o $(B)/shim.o $(B)/ldrsvc_blob.o
$(B)/shim.bin: $(B)/shim.elf
	$(OBJCOPY) -O binary $< $@
# stage0 carries the little-endian shim as its payload and enters it after
# switching the machine to little-endian mode itself.
$(B)/stage0.o: stage0.c | $(B)
	$(CLANG) $(CFLAGS_BE) -c $< -o $@
$(B)/stage0s.o: stage0.S $(B)/shim.bin | $(B)
	$(CLANG) $(TARGET_BE) -x assembler-with-cpp -c -DPAYLOAD=\"$(B)/shim.bin\" $< -o $@
$(B)/stage0.elf: $(B)/stage0s.o $(B)/stage0.o stage0.ld
	$(LLD) -T stage0.ld -static -nostdlib -o $@ $(B)/stage0s.o $(B)/stage0.o
$(B)/stage0.bin: $(B)/stage0.elf
	$(OBJCOPY) -O binary $< $@
# A big-endian XCOFF, for Open Firmware's own xcoff-loader in its factory
# (big-endian) mode: every Power Macintosh ROM and both Network Server ROMs.
$(B)/SHIM.XCF: $(B)/stage0.bin mkxcoff.py
	python3 mkxcoff.py $< $@ 0x00600000 be
# A FAT12 1.44 MB floppy: OS2BOOT.OF (sectors of SHIM.XCF filled in) and SHIM.XCF.
$(B)/os2shim.img: $(B)/SHIM.XCF os2boot.of mkfloppy.sh
	sh mkfloppy.sh $(B)/SHIM.XCF os2boot.of $@

clean:
	rm -rf $(B)
