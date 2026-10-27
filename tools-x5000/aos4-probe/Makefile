# SPDX-License-Identifier: GPL-2.0+
# Build with any PowerPC cross toolchain, from the top of the tree:
#   make -C tools-x5000/aos4-probe CROSS_COMPILE=powerpc-linux-gnu-
# (docker-x5000/Dockerfile provides one; CROSS_COMPILE is set there.)

CROSS_COMPILE ?= powerpc-linux-gnu-
CC	= $(CROSS_COMPILE)gcc
OBJCOPY	= $(CROSS_COMPILE)objcopy

CFLAGS	= -mcpu=e500mc -m32 -Os -ffreestanding -fno-builtin -fno-pic \
	  -msdata=none -mno-altivec -fno-stack-protector -Wall -Wextra -Wa,-mregnames
LDFLAGS	= -nostdlib -static -T probe.lds -Wl,--build-id=none

all: aos4probe

aos4probe.elf: start.S probe.c probe.lds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.S probe.c -lgcc

aos4probe: aos4probe.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -f aos4probe aos4probe.elf

.PHONY: all clean
