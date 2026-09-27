# Build elf2aif on Linux/macOS (host), or cross-compile it for RISC OS with GCCSDK.
CC ?= cc
CFLAGS ?= -O2
GCCSDK_SRC ?= $(HOME)/riscos/gccsdk
DEFS = -DPACKAGE_STRING='"elf2aif 1.1 (large image fix)"' -I$(GCCSDK_SRC)/gcc4/riscos/elf2aif/src

elf2aif: elf2aif.c
	$(CC) $(CFLAGS) $(DEFS) -o $@ $<

# RISC OS: make riscos CROSS=arm-riscos-gnueabihf-
riscos: elf2aif.c
	$(CROSS)gcc $(CFLAGS) -static $(DEFS) -o elf2aif-elf $<
	./elf2aif -e elf2aif-elf elf2aif,ff8

clean:
	rm -f elf2aif elf2aif-elf elf2aif,ff8
