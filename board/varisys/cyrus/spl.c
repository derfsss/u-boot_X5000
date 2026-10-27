// SPDX-License-Identifier: GPL-2.0+
/*
 * AmigaOne X5000 (Varisys Cyrus) SPL: minimal eSDHC first stage, used by the
 * X5000/20 (P5020) and X5000/40 (P5040) SD-card builds.
 *
 * Runs from the CPC L3-SRAM, brings up DDR from the DIMMs' SPD, then loads
 * the full U-Boot from a raw SD offset (CONFIG_SYS_MMC_U_BOOT_OFFS) into DDR
 * and starts it. This two-stage split keeps the block-8 PBL small, so the
 * AmigaOS 4 NVRAM block at SD 0x89200 stays free (docs/sd-card-layout.md).
 * Based on board/freescale/t102xrdb/spl.c.
 *
 * Copyright 2014 Freescale Semiconductor, Inc.
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <clock_legacy.h>
#include <console.h>
#include <env_internal.h>
#include <init.h>
#include <malloc.h>
#include <ns16550.h>
#include <i2c.h>
#include <mmc.h>
#include <fsl_esdhc.h>
#include <u-boot/crc.h>
#include <asm/global_data.h>
#include <linux/delay.h>
#include "ddr_diag.h"

DECLARE_GLOBAL_DATA_PTR;

phys_size_t get_effective_memsize(void)
{
	/* Before DDR is up the SPL "RAM" is the CPC L3-SRAM. */
	return CONFIG_SYS_L3_SIZE;
}

unsigned long get_board_sys_clk(void)
{
	/* Cyrus has a fixed SYSCLK; DDR is derived from it in speed.c. */
	return CONFIG_SYS_CLK_FREQ;
}

void board_init_f(ulong bootflag)
{
	u32 plat_ratio, sys_clk, ccb_clk;
	ccsr_gur_t *gur = (void *)CONFIG_SYS_MPC85xx_GUTS_ADDR;

	/* Keep a copy of the global data in SRAM before relocation. */
	memcpy((void *)CONFIG_SPL_GD_ADDR, (void *)gd, sizeof(gd_t));
	gd = (gd_t *)(CONFIG_SPL_GD_ADDR);

	console_init_f();

	/* Bring up the console UART at the correct baud rate. */
	sys_clk = get_board_sys_clk();
	plat_ratio = (in_be32(&gur->rcwsr[0]) >> 25) & 0x1f;
	ccb_clk = sys_clk * plat_ratio / 2;

	ns16550_init((struct ns16550 *)CONFIG_SYS_NS16550_COM1,
		     ccb_clk / 16 / CONFIG_BAUDRATE);

	puts("\nSD boot...\n");

	relocate_code(CONFIG_SPL_RELOC_STACK, (gd_t *)CONFIG_SPL_GD_ADDR, 0x0);
}

void board_init_r(gd_t *gd, ulong dest_addr)
{
	struct bd_info *bd;

	bd = (struct bd_info *)(gd + sizeof(gd_t));
	memset(bd, 0, sizeof(struct bd_info));
	gd->bd = bd;

	arch_cpu_init();
	get_clocks();
	mem_malloc_init(CONFIG_SPL_RELOC_MALLOC_ADDR,
			CONFIG_SPL_RELOC_MALLOC_SIZE);
	gd->flags |= GD_FLG_FULL_MALLOC_INIT;

	mmc_initialize(bd);
	mmc_spl_load_image(CONFIG_ENV_OFFSET, CONFIG_ENV_SIZE,
			   (uchar *)SPL_ENV_ADDR);

	/*
	 * env_get_f() walks the name=value pairs from gd->env_addr, so point
	 * it at the data, past env_t's CRC header (at the header itself, the
	 * first variable is misread, and a zero CRC byte hides them all).
	 * Only a copy with a valid CRC is used; otherwise env_get_f() falls
	 * back to the built-in default environment. The DDR code reads
	 * hwconfig and ddr_wrlvlN from here before DDR is up.
	 */
	{
		env_t *env = (env_t *)SPL_ENV_ADDR;

		gd->env_addr = (ulong)env->data;
		gd->env_valid = crc32(0, env->data, ENV_SIZE) == env->crc ?
				ENV_VALID : ENV_INVALID;
	}

	i2c_init_all();

	cyrus_ddr_diag_early();
	dram_init();

	mmc_boot();
}
