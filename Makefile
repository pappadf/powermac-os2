# powermac-os2 -- OS/2 Warp (PowerPC Edition) on the Apple Network Server.
#
#   make ISO=/path/to/os2-ppc-cd1.iso    the patched CD and the boot floppy
#   make shim                            the boot floppy only
#   make check-iso ISO=...               is this the CD the patches expect?
#
# Outputs go to build/.

ISO     ?=
OUT     ?= build/os2-ppc-mac.iso
PYTHON  ?= python3

# The December 1995 OS/2 Warp (PowerPC Edition) CD 1 ("OS" disc).  Every patch
# also checks the bytes it replaces, so another build of the CD fails loudly.
CD1_SHA256 = dd704876b696428e045c8c076ecebe1f83a5ac07389e5ef973d763f8950bc820

PATCHES = $(sort $(wildcard patches/*.patch))

.PHONY: all cd shim check-iso clean

all: cd shim

cd: $(OUT)

$(OUT): $(PATCHES) $(wildcard patches/src/*) $(wildcard remaster/*.py) | check-iso
	@mkdir -p $(dir $@)
	$(PYTHON) remaster/remaster.py "$(ISO)" $@ $(PATCHES)

check-iso:
	@test -n "$(ISO)" || { echo "make: give the original CD image: make ISO=/path/to/os2-ppc-cd1.iso"; exit 1; }
	@test -f "$(ISO)" || { echo "make: $(ISO): no such file"; exit 1; }
	@$(PYTHON) -c 'import hashlib,sys; h=hashlib.sha256(); f=open(sys.argv[1],"rb"); [h.update(b) for b in iter(lambda: f.read(1<<20), b"")]; sys.exit(0 if h.hexdigest()==sys.argv[2] else "make: %s is not the December 1995 CD 1 image (sha256 %s)" % (sys.argv[1], h.hexdigest()))' "$(ISO)" $(CD1_SHA256)

shim:
	$(MAKE) -C shim
	@mkdir -p build
	cp shim/build/os2shim.img build/os2shim.img

clean:
	rm -rf build
	$(MAKE) -C shim clean
