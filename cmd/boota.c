// SPDX-License-Identifier: GPL-2.0+
/*
 * boota - boot AmigaOS 4 on an AmigaOne (X5000 and A1222).
 *
 * U-Boot reads Kickstart/Kicklayout from the AmigaOS volumes itself (NGFS,
 * SFS and FFS drivers), shows the boot menu, loads the chosen kickstart and
 * enters the kernel loader; the work is done in common/aos4/. Tested on the
 * X5000 and the A1222. See docs/booting-amigaos.md.
 *
 * Environment:
 *   os4_bootdevice   "auto" (SATA, then USB) or interface names
 *   os4_commandline  kernel command line
 *   boota_timeout    menu countdown in seconds (default 5, 0 = wait)
 *   amigaboot_quiet  y: no menu, boot the default entry (the variable
 *                    keeps the name AmigaBoot used)
 *   bootable_only    y: only volumes flagged bootable in the RDB
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <usb.h>
#include <asm/global_data.h>
#include "../common/aos4/aos4.h"

DECLARE_GLOBAL_DATA_PTR;

#ifdef CONFIG_TARGET_TABOR
extern int tabor_env_dirty;		/* board/aeon/tabor/tabor.c */
#endif

static struct aos4_menu menu;

static int do_boota(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	bool dry_run = false, quiet;
	const char *cmdline;
	int choice = -1, ret;

	if (argc > 1 && !strcmp(argv[1], "-n")) {
		dry_run = true;
		argc--;
		argv++;
	}
	if (argc > 2)
		return CMD_RET_USAGE;
	quiet = env_get_yesno("amigaboot_quiet") == 1;
#ifdef CONFIG_TARGET_TABOR
	/*
	 * A1222: a MAC address read from the EEPROM at start-up is saved
	 * to the environment here, as the official firmware does.
	 */
	if (tabor_env_dirty) {
		tabor_env_dirty = 0;
		run_command("saveenv", 0);
	}
#endif
	aos4_console_save();

	/*
	 * Bring the devices up on the console as it is, so on a quiet boot
	 * (stdout=serial) their messages stay off the screen.
	 */
	run_command("usb start", 0);
	run_command("sata init", 0);
	/* without CONFIG_CONSOLE_MUX stdin stays as it is */
	if (IS_ENABLED(CONFIG_CONSOLE_MUX))
		env_set("stdin", "serial,usbkbd");
#ifdef CONFIG_USB_KEYBOARD
	/* drop key-repeat and queued keys left from before the menu */
	usb_kbd_refresh();
#endif

	if (!aos4_scan(&menu, env_get("os4_bootdevice"),
		       env_get_yesno("bootable_only") == 1)) {
		aos4_console_screen();
		printf("No bootable AmigaOS volume found.\n");
		return CMD_RET_FAILURE;
	}

	if (argc == 2) {
		char c = argv[1][0];

		choice = c >= '1' && c <= '9' ? c - '1' :
			 c >= 'a' && c <= 'z' ? 9 + c - 'a' :
			 c >= 'A' && c <= 'Z' ? 9 + c - 'A' : -1;
		if (choice < 0 || choice >= menu.count) {
			aos4_console_screen();
			aos4_menu_print(&menu);
			printf("No entry %s.\n", argv[1]);
			aos4_menu_free(&menu);
			return CMD_RET_FAILURE;
		}
	} else if (quiet) {
		choice = menu.def;
	} else {
		/*
		 * The menu and the kickstart loading are shown on the
		 * screen; aos4_boot() clears it once loading is done.
		 */
		aos4_console_screen();
		choice = aos4_menu_choose(&menu,
					  env_get_ulong("boota_timeout", 10, 5));
		printf("\n");
	}
	if (choice < 0) {
		aos4_menu_free(&menu);
		return CMD_RET_SUCCESS;
	}

	cmdline = env_get("os4_commandline");
	ret = aos4_boot(&menu.entries[choice], cmdline ? cmdline : "", quiet,
			dry_run);
	aos4_menu_free(&menu);
	if (ret)
		aos4_console_screen();
	return ret ? CMD_RET_FAILURE : CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	boota, 3, 0, do_boota,
	"boot AmigaOS 4 from a menu of the AmigaOS volumes",
	"[-n] [entry]\n"
	"    - show the boot menu built from the /Kickstart/Kicklayout files of\n"
	"      the AmigaOS partitions, or boot menu entry 'entry' (1-9, A-Z)\n"
	"      directly\n"
	"    -n: load the kickstart and print where it went, but do not start it\n"
	"  Menu keys: 1-9/A-Z pick an entry, UP/DOWN move, RETURN boots,\n"
	"  SPACE stops the countdown, Ctrl-C returns to the prompt.\n"
	"  Environment:\n"
	"    boota_timeout    menu countdown in seconds (default 5)\n"
	"    amigaboot_quiet  y: boot the default entry without the menu\n"
	"    bootable_only    y: list only partitions marked bootable\n"
	"    os4_bootdevice   where to look: auto (SATA, then USB), or e.g. usb,sata\n"
	"    os4_commandline  kernel command line"
);
