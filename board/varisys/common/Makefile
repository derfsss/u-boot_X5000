# SPDX-License-Identifier: GPL-2.0+
#
# (C) Copyright 2006
# Wolfgang Denk, DENX Software Engineering, wd@denx.de.

MINIMAL=

ifdef CONFIG_SPL_BUILD
ifdef CONFIG_SPL_INIT_MINIMAL
MINIMAL=y
endif
endif

ifdef MINIMAL
# necessary to create built-in.o
obj- := __dummy__.o
else
ifdef CONFIG_SPL_BUILD
# AmigaOne X5000: the SPL uses nothing from this dir, but built-in.o must
# still be created.
obj- := __dummy__.o
else
obj-$(CONFIG_ID_EEPROM)	+= sys_eeprom.o
endif
endif
