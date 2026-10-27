// SPDX-License-Identifier: GPL-2.0+
/*
 * Find bootable AmigaOS 4 volumes for boota (AmigaOne X5000, A1222): RDB
 * partitions holding a Kickstart/Kicklayout, on the interfaces named by
 * os4_bootdevice.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <blk.h>
#include <fs.h>
#include <malloc.h>
#include <part.h>
#include <linux/string.h>
#include "aos4.h"

#define KICKLAYOUT	"/Kickstart/Kicklayout"
#define PBFF_BOOTABLE	0x1		/* RDB PART flag: volume is bootable */

static const char * const auto_ifaces[] = { "sata", "usb" };

/* NGFS, SFS and FFS/OFS volumes, the ones U-Boot can read */
static bool readable_dostype(u32 dostype)
{
	switch (dostype & 0xffffff00) {
	case 0x4e474600:	/* NGF\x */
		return IS_ENABLED(CONFIG_FS_NGFS);
	case 0x53465300:	/* SFS\x */
		return IS_ENABLED(CONFIG_FS_SFS);
	case 0x444f5300:	/* DOS\x */
		return IS_ENABLED(CONFIG_FS_AFFS);
	}
	return false;
}

/* Read the volume's Kicklayout and add one menu entry per configuration. */
static void scan_volume(struct aos4_menu *menu, struct blk_desc *desc,
			const char *ifname, int part, const char *volume,
			s32 pri)
{
	struct aos4_config *cfgs;
	loff_t size, got;
	char *text;
	int n, i;

	if (fs_set_blk_dev_with_part(desc, part) || fs_size(KICKLAYOUT, &size))
		return;
	if (menu->nbufs + 2 > ARRAY_SIZE(menu->bufs))
		return;

	text = malloc(size + 1);
	cfgs = malloc(AOS4_MAX_CONFIGS * sizeof(*cfgs));
	if (!text || !cfgs)
		goto fail;
	if (fs_set_blk_dev_with_part(desc, part) ||
	    fs_read(KICKLAYOUT, (ulong)text, 0, size, &got) || got != size) {
		printf("%s %d:%d (%s): cannot read %s\n", ifname, desc->devnum,
		       part, volume, KICKLAYOUT);
		goto fail;
	}
	text[size] = '\0';

	n = aos4_parse_kicklayout(text, cfgs, AOS4_MAX_CONFIGS);
	if (!n)
		goto fail;
	menu->bufs[menu->nbufs++] = text;
	menu->bufs[menu->nbufs++] = cfgs;

	for (i = 0; i < n && menu->count < AOS4_MAX_ENTRIES; i++) {
		struct aos4_entry *e = &menu->entries[menu->count];

		strlcpy(e->ifname, ifname, sizeof(e->ifname));
		e->devnum = desc->devnum;
		e->part = part;
		strlcpy(e->volume, volume, sizeof(e->volume));
		e->boot_pri = pri;
		e->cfg = &cfgs[i];
		/* default: first configuration of the highest-priority volume */
		if (!menu->count ||
		    (i == 0 && pri > menu->entries[menu->def].boot_pri))
			menu->def = menu->count;
		menu->count++;
	}
	return;

fail:
	free(text);
	free(cfgs);
}

static void scan_iface(struct aos4_menu *menu, const char *ifname,
		       bool bootable_only)
{
	int dev, part;

	for (dev = 0; dev < 8; dev++) {
		struct blk_desc *desc = blk_get_dev(ifname, dev);

		if (!desc)
			break;
		if (desc->type == DEV_TYPE_UNKNOWN ||
		    desc->part_type != PART_TYPE_AMIGA)
			continue;

		for (part = 1; part <= AMIGA_ENTRY_NUMBERS; part++) {
			struct disk_partition info;
			u32 flags, dostype;
			s32 pri;

			if (part_get_info(desc, part, &info) ||
			    part_amiga_boot_info(desc, part, &pri, &flags,
						 &dostype))
				break;
			if (bootable_only && !(flags & PBFF_BOOTABLE))
				continue;
			if (!readable_dostype(dostype))
				continue;
			scan_volume(menu, desc, ifname, part,
				    (const char *)info.name, pri);
		}
	}
}

/*
 * Build the boot menu. bootdevice is "auto" (SATA, then USB) or a list of
 * interface names, e.g. "usb sata".
 */
int aos4_scan(struct aos4_menu *menu, const char *bootdevice,
	      bool bootable_only)
{
	memset(menu, 0, sizeof(*menu));

	if (!bootdevice || !*bootdevice || !strcmp(bootdevice, "auto")) {
		for (int i = 0; i < ARRAY_SIZE(auto_ifaces); i++)
			scan_iface(menu, auto_ifaces[i], bootable_only);
	} else {
		char list[64], *s = list, *tok;

		strlcpy(list, bootdevice, sizeof(list));
		while ((tok = strsep(&s, " ,")))
			if (*tok)
				scan_iface(menu, tok, bootable_only);
	}
	return menu->count;
}

void aos4_menu_free(struct aos4_menu *menu)
{
	while (menu->nbufs)
		free(menu->bufs[--menu->nbufs]);
	menu->count = 0;
}
