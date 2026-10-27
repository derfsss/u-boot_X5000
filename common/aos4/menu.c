// SPDX-License-Identifier: GPL-2.0+
/*
 * AmigaOS 4 boot menu for boota (AmigaOne X5000, A1222): one line per
 * configuration, chosen by its key, with a countdown to the default.
 * Also moves the console between serial and screen around the boot.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <console.h>
#include <env.h>
#include <stdio_dev.h>
#include <asm/global_data.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <time.h>
#include "aos4.h"

/* the USB keyboard driver sends its arrows as these codes (usb_kbd.c) */
#define KEY_UP_USB	0x10	/* ^P: Up */
#define KEY_DOWN_USB	0x0e	/* ^N: Down */

DECLARE_GLOBAL_DATA_PTR;

static char saved_stdout[32], saved_stderr[32];

/* Remember the console boota started with (serial only on a quiet boot). */
void aos4_console_save(void)
{
	const char *out = env_get("stdout"), *err = env_get("stderr");

	strlcpy(saved_stdout, out ? out : "serial", sizeof(saved_stdout));
	strlcpy(saved_stderr, err ? err : "serial", sizeof(saved_stderr));
}

/*
 * Put the console on the screen as well: the menu and errors. In a build
 * without CONFIG_CONSOLE_MUX a device list is refused, so the output
 * moves to the screen alone until aos4_console_restore().
 */
void aos4_console_screen(void)
{
	const char *screen = IS_ENABLED(CONFIG_CONSOLE_MUX) ? "serial,vga" :
			     "vga";

	gd->flags &= ~GD_FLG_SILENT;
	if (!stdio_get_by_name("vga"))
		return;
	env_set("stdout", screen);
	env_set("stderr", screen);
	/* a visible cursor is redrawn around every character: slow on VGA */
	printf("\x1b[?25l");
}

/* Back to the saved console, wiping the menu off the screen if it goes. */
void aos4_console_restore(void)
{
	if (!strstr(saved_stdout, "vga"))
		printf("\x1b[2J");
	env_set("stdout", saved_stdout);
	env_set("stderr", saved_stderr);
}

static char entry_key(int i)
{
	return i < 9 ? '1' + i : 'A' + i - 9;
}

static int key_entry(int c)
{
	if (c >= '1' && c <= '9')
		return c - '1';
	if (c >= 'a' && c <= 'z')
		c -= 'a' - 'A';
	if (c >= 'A' && c <= 'Z')
		return 9 + c - 'A';
	return -1;
}

void aos4_menu_print(struct aos4_menu *menu)
{
	int i;

	printf("\x1b[2J");
	printf("AmigaOS 4 boot menu\n\n");
	for (i = 0; i < menu->count; i++) {
		struct aos4_entry *e = &menu->entries[i];

		printf(" %c %c: %-32s %s %d:%d %s\n",
		       i == menu->def ? '>' : ' ', entry_key(i), e->cfg->label,
		       e->ifname, e->devnum, e->part, e->volume);
	}
	puts("\n  RETURN boots the marked entry, UP and DOWN move the mark,\n"
	       "  SPACE stops the countdown, its key boots any other entry,\n"
	       "  Ctrl-C leaves to the U-Boot prompt.\n\n");
}

/* Read one key, folding the serial arrow sequences ESC [ A / ESC [ B. */
static int read_key(void)
{
	int c = getchar();
	ulong t;

	if (c != 0x1b)
		return c;
	for (t = get_timer(0); !tstc(); )
		if (get_timer(t) > 50)
			return c;
	if (getchar() != '[')
		return 0;
	for (t = get_timer(0); !tstc(); )
		if (get_timer(t) > 50)
			return 0;
	c = getchar();
	return c == 'A' ? KEY_UP_USB : c == 'B' ? KEY_DOWN_USB : 0;
}

/*
 * Show the menu and return the chosen entry, or -1 to stay in U-Boot.
 * timeout is in seconds; 0 waits for a key.
 */
int aos4_menu_choose(struct aos4_menu *menu, int timeout)
{
	bool counting = timeout > 0;
	ulong start = get_timer(0);
	int shown = -1;

	aos4_menu_print(menu);
	for (;;) {
		int left = timeout - get_timer(start) / 1000;

		if (counting) {
			if (left <= 0)
				return menu->def;
			if (left != shown) {
				printf("\r  Booting the marked entry in %d s ",
				       left);
				shown = left;
			}
		}
		if (!tstc()) {
			udelay(10000);
			continue;
		}

		int c = read_key();

		if (c == '\r' || c == '\n')
			return menu->def;
		if (c == 0x03)
			return -1;
		if (c == ' ') {
			if (counting)
				printf("\r  Countdown stopped.               ");
			counting = false;
			continue;
		}
		if (c == KEY_UP_USB || c == KEY_DOWN_USB) {
			int n = menu->count;

			menu->def = (menu->def + (c == KEY_UP_USB ? n - 1 : 1)) % n;
			counting = false;
			aos4_menu_print(menu);
			continue;
		}
		c = key_entry(c);
		if (c >= 0 && c < menu->count)
			return c;
	}
}
