# SPDX-License-Identifier: GPL-2.0+

# AmigaOne X5000: the SPL (spl.o, optional DDR diagnostics) and the main
# U-Boot build different board files; video.o is the Radeon VBIOS POST.
ifdef CONFIG_SPL_BUILD
obj-y	+= spl.o
obj-$(CONFIG_CYRUS_DDR_DIAG) += ddr_diag.o
else
obj-y	+= $(BOARD).o
obj-y	+= eth.o
obj-$(CONFIG_PCI) += pci.o
obj-$(CONFIG_BIOSEMU) += video.o
endif
obj-y	+= ddr.o
obj-y	+= law.o
obj-y	+= tlb.o
