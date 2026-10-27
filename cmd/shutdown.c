// SPDX-License-Identifier: GPL-2.0+
/*
 * shutdown / restart - power control for the AmigaOne X5000 (Varisys Cyrus).
 *
 * The Cyrus board's supervisor MCU is wired to the SoC's DUART1 port B
 * (CONFIG_SYS_NS16550_COM2, CCSR+0x11C600) at 38400 8N1, speaking a
 * documented "#cmd\n" -> "$reply\n" ASCII protocol (Cyrus TRM 1.1.1 sec
 * 8.1). "#s" requests power-off: the MCU drops PS_ON and the machine
 * turns off -- same effect as 's' on the MCU's external P18 debug shell.
 *
 * The MCU's UART command set has no power-on/restart command (power-on
 * exists only on the external P18 management header), so `restart`
 * performs a SoC reset, which reboots the firmware from scratch.
 *
 * Note the DUART clock: the SoC's DUARTs run at half the platform bus
 * clock (see CONFIG_SYS_NS16550_CLK in cyrus.h); a divisor computed
 * from the full bus clock gives half the intended baud rate.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <command.h>
#include <clock_legacy.h>
#include <ns16550.h>
#include <linux/delay.h>

static int do_shutdown(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct ns16550 *mcu = (struct ns16550 *)CONFIG_SYS_NS16550_COM2;
	const char *cmd = "#s\n";
	int divisor;

	printf("Powering off via board MCU...\n");
	/* Let the message drain before power drops. */
	mdelay(50);

	divisor = CONFIG_SYS_NS16550_CLK / 16 / 38400;
	ns16550_init(mcu, divisor);
	while (*cmd)
		ns16550_putc(mcu, *cmd++);

	/* Power should drop almost immediately; wait for it. */
	mdelay(3000);

	/* Only reached if the MCU didn't act (unexpected). */
	printf("shutdown: MCU did not power off the board\n");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(
	shutdown, 1, 0, do_shutdown,
	"power off the machine (via the board MCU)",
	"\n    - sends the power-off command to the Cyrus supervisor MCU"
);

static int do_restart(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	printf("Restarting...\n");
	mdelay(50);
	return do_reset(NULL, 0, 0, NULL);
}

U_BOOT_CMD(
	restart, 1, 0, do_restart,
	"restart the machine (SoC reset, boots firmware from scratch)",
	"\n    - equivalent to 'reset'; named for symmetry with 'shutdown'"
);
