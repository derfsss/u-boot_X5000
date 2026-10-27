/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Native AmigaOS 4 boot for the boota command: Kicklayout parsing, boot
 * volume scan, boot menu, kickstart loading and the kernel handoff. Used
 * by the AmigaOne X5000 and A1222 ports; tested on both.
 *
 * The kernel interface (module list, board info with its extended tail,
 * the device tree) is described in docs/booting-amigaos.md; the
 * Kicklayout format is the one documented in every Kickstart/Kicklayout
 * file. See also BBoot (GPL, Zoltan Balaton), which makes the same kernel
 * call.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#ifndef __AOS4_H
#define __AOS4_H

#include <linux/types.h>

#define AOS4_MAX_MODULES	128
#define AOS4_MAX_CONFIGS	8
#define AOS4_MAX_ENTRIES	35	/* menu keys 1-9, A-Z */

/* One LABEL of a Kicklayout. The strings point into the file's text. */
struct aos4_config {
	char *label;
	char *exec;
	int nmods;
	char *mods[AOS4_MAX_MODULES];
};

/* One menu line: a configuration on a volume. */
struct aos4_entry {
	char ifname[8];
	int devnum;
	int part;
	char volume[32];	/* RDB drive name, e.g. DH0 */
	s32 boot_pri;
	struct aos4_config *cfg;
};

struct aos4_menu {
	int count;
	int def;		/* default entry */
	struct aos4_entry entries[AOS4_MAX_ENTRIES];
	int nbufs;		/* per-volume allocations, freed by aos4_menu_free() */
	void *bufs[2 * AOS4_MAX_ENTRIES];
};

/* kicklayout.c */
int aos4_parse_kicklayout(char *text, struct aos4_config *cfgs, int max);

/* scan.c */
int aos4_scan(struct aos4_menu *menu, const char *bootdevice,
	      bool bootable_only);
void aos4_menu_free(struct aos4_menu *menu);

/* menu.c */
int aos4_menu_choose(struct aos4_menu *menu, int timeout);
void aos4_menu_print(struct aos4_menu *menu);
void aos4_console_save(void);
void aos4_console_screen(void);
void aos4_console_restore(void);

/* load.c */
int aos4_boot(struct aos4_entry *e, const char *cmdline, bool quiet,
	      bool dry_run);

#endif
