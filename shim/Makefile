# SPDX-License-Identifier: GPL-2.0-only
# os2shim -- OS/2 Warp (PowerPC Edition) from Open Firmware on an Apple Network Server
CLANG  ?= clang
LLD    ?= ld.lld
OBJCOPY ?= llvm-objcopy
TARGET = --target=powerpcle-unknown-linux-gnu -mcpu=604
CFLAGS = $(TARGET) -O2 -Wall -Wextra -ffreestanding -fno-builtin -fno-pic -fno-pie -msoft-float \
         -fno-asynchronous-unwind-tables -fno-stack-protector -fno-jump-tables -mno-altivec -I. \
         -mllvm -combiner-store-merging=false  # LE mode faults on every misaligned access
B ?= build

all: $(B)/os2shim.img

$(B):
	mkdir -p $(B)
$(B)/start.o: start.S | $(B)
	$(CLANG) $(TARGET) -x assembler-with-cpp -c $< -o $@
# The loader service: linked where the loader calls it, embedded in the shim.
$(B)/ldrsvc.o: ldrsvc.c ../patches/src/loader-svc.h | $(B)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(B)/ldrsvc.elf: $(B)/ldrsvc.o ldrsvc.ld
	$(LLD) -T ldrsvc.ld -static -nostdlib -o $@ $(B)/ldrsvc.o
$(B)/ldrsvc.bin: $(B)/ldrsvc.elf
	$(OBJCOPY) -O binary $< $@
$(B)/ldrsvc_blob.o: ldrsvc_blob.S $(B)/ldrsvc.bin
	$(CLANG) $(TARGET) -c -DBLOB=\"$(B)/ldrsvc.bin\" $< -o $@
$(B)/shim.o: shim.c prep-residual.h ../patches/src/loader-svc.h | $(B)
	$(CLANG) $(CFLAGS) -c $< -o $@
$(B)/shim.elf: $(B)/start.o $(B)/shim.o $(B)/ldrsvc_blob.o shim.ld
	$(LLD) -T shim.ld -static -nostdlib -o $@ $(B)/start.o $(B)/shim.o $(B)/ldrsvc_blob.o
$(B)/shim.bin: $(B)/shim.elf
	$(OBJCOPY) -O binary $< $@
$(B)/SHIM.EXE: $(B)/shim.bin mkpe.py
	python3 mkpe.py $< $@ 0x00050000
# A FAT12 1.44 MB floppy: OS2BOOT.OF (sectors of SHIM.EXE filled in) and SHIM.EXE.
$(B)/os2shim.img: $(B)/SHIM.EXE os2boot.of mkfloppy.sh
	sh mkfloppy.sh $(B)/SHIM.EXE os2boot.of $@

clean:
	rm -rf $(B)
