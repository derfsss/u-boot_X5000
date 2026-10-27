// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on corenet_ds ddr code
 */

#include <common.h>
#include <env.h>		/* AmigaOne X5000: env_get_f() for ddr_wrlvlN */
#include <i2c.h>
#include <hwconfig.h>
#include <init.h>
#include <log.h>
#include <asm/global_data.h>
#include <asm/mmu.h>
#include <cpu_func.h>		/* AmigaOne X5000: flush_cache() */
#include <fsl_ddr_sdram.h>
#include <fsl_ddr_dimm_params.h>
#include <asm/fsl_law.h>
#include "ddr_diag.h"		/* AmigaOne X5000: DDR diagnostics */

DECLARE_GLOBAL_DATA_PTR;

struct board_specific_parameters {
	u32 n_ranks;
	u32 datarate_mhz_high;
	u32 clk_adjust;
	u32 wrlvl_start;
	u32 cpo;
	u32 write_data_delay;
	u32 force_2t;
};

/*
 * This table contains all valid speeds we want to override with board
 * specific parameters. datarate_mhz_high values need to be in ascending order
 * for each n_ranks group.
 */
static const struct board_specific_parameters udimm0[] = {
	/*
	 * memory controller 0
	 *   num|  hi|  clk| wrlvl | cpo  |wrdata|2T
	 * ranks| mhz|adjst| start |      |delay |
	 */
	{4,   850,    4,     6,   0xff,    2,  0},
	{4,   950,    5,     7,   0xff,    2,  0},
	{4,  1050,    5,     8,   0xff,    2,  0},
	{4,  1250,    5,    10,   0xff,    2,  0},
	{4,  1350,    5,    11,   0xff,    2,  0},
	{4,  1666,    5,    12,   0xff,    2,  0},
	{2,   850,    5,     6,   0xff,    2,  0},
	{2,  1050,    5,     7,   0xff,    2,  0},
	{2,  1250,    4,     6,   0xff,    2,  0},
	{2,  1350,    5,     7,   0xff,    2,  0},
	{2,  1666,    5,     8,   0xff,    2,  0},
	{1,  1250,    4,     6,   0xff,    2,  0},
	{1,  1335,    4,     7,   0xff,    2,  0},
	{1,  1666,    4,     8,   0xff,    2,  0},
	{}
};

/*
 * The two slots have slightly different timing. The center values are good
 * for both slots. We use identical speed tables for them. In future use, if
 * DIMMs have fewer center values that require two separated tables, copy the
 * udimm0 table to udimm1 and make changes to clk_adjust and wrlvl_start.
 */
static const struct board_specific_parameters *udimms[] = {
	udimm0,
	udimm0,
};

static const struct board_specific_parameters rdimm0[] = {
	/*
	 * memory controller 0
	 *   num|  hi|  clk| wrlvl | cpo  |wrdata|2T
	 * ranks| mhz|adjst| start |      |delay |
	 */
	{4,   850,    4,     6,   0xff,    2,  0},
	{4,   950,    5,     7,   0xff,    2,  0},
	{4,  1050,    5,     8,   0xff,    2,  0},
	{4,  1250,    5,    10,   0xff,    2,  0},
	{4,  1350,    5,    11,   0xff,    2,  0},
	{4,  1666,    5,    12,   0xff,    2,  0},
	{2,   850,    4,     6,   0xff,    2,  0},
	{2,  1050,    4,     7,   0xff,    2,  0},
	{2,  1666,    4,     8,   0xff,    2,  0},
	{1,   850,    4,     5,   0xff,    2,  0},
	{1,   950,    4,     7,   0xff,    2,  0},
	{1,  1666,    4,     8,   0xff,    2,  0},
	{}
};

/*
 * The two slots have slightly different timing. See comments above.
 */
static const struct board_specific_parameters *rdimms[] = {
	rdimm0,
	rdimm0,
};

/*
 * AmigaOne X5000: per-lane DDR write-levelling start (DQS_ADJUST for the
 * first write-
 * levelling sample), in 1/8 clock units, 0x01-0x14 (P5020 RM,
 * DDRx_DDR_WRLVL_CNTL/_2/_3). Byte lane 0 uses WRLVL_CNTL[WRLVL_START];
 * lanes 1-8 use one byte each of WRLVL_CNTL_2 (lanes 1-4, MSB first) and
 * WRLVL_CNTL_3 (lanes 5-8; 8 is the ECC lane).
 *
 * A board whose byte lanes do not arrive where the computed start expects
 * (for example after a hardware repair, where some lanes may need about
 * 1.5 clocks instead of 7/8) can be tuned per controller from the
 * environment, without a firmware change:
 *
 *   setenv ddr_wrlvl1 07,07,07,07,0b,0d,0d,0d; saveenv
 *
 * lists lanes 0, 1, 2 ... in hex; lanes not given keep the computed start.
 * ddr_wrlvl0 is controller 0, ddr_wrlvl1 controller 1. The SPL reads it
 * before DDR is up (env_get_f), so a wrong value is fixed by clearing it
 * from the U-Boot prompt or reflashing the environment.
 */
#define CYRUS_WRLVL_LANES	9

static int cyrus_wrlvl_from_env(unsigned int ctrl_num, u8 *lane)
{
	char name[] = "ddr_wrlvl0", buf[64], *p = buf, *end;
	u8 v[CYRUS_WRLVL_LANES];
	int n = 0, i;

	name[sizeof(name) - 2] += ctrl_num;
	if (env_get_f(name, buf, sizeof(buf)) <= 0)
		return 0;

	while (*p && n < CYRUS_WRLVL_LANES) {
		ulong val = simple_strtoul(p, &end, 16);

		if (end == p || val < 0x01 || val > 0x14) {
			printf("DDR: ignoring %s=%s (lane %d: want hex 01-14)\n",
			       name, buf, n);
			return 0;
		}
		v[n++] = val;
		for (p = end; *p == ',' || *p == ' '; p++)
			;
	}
	for (i = 0; i < n; i++)
		lane[i] = v[i];
	printf("DDR: controller %u write-levelling start from %s: %s\n",
	       ctrl_num, name, buf);
	return 1;
}

static void cyrus_wrlvl_lanes(memctl_options_t *popts, unsigned int ctrl_num)
{
	u8 lane[CYRUS_WRLVL_LANES];
	int i, set;

	for (i = 0; i < CYRUS_WRLVL_LANES; i++)
		lane[i] = popts->wrlvl_start;

	set = cyrus_wrlvl_from_env(ctrl_num, lane);

#if defined(CONFIG_CYRUS_DDR_DIAG_SWEEP) && defined(CONFIG_SPL_BUILD)
	/* Boot-to-boot sweep (ddr_diag.c): this boot's value, all lanes. */
	if (cyrus_ddr_sweep.wlall >= 0) {
		for (i = 0; i < CYRUS_WRLVL_LANES; i++)
			lane[i] = cyrus_ddr_sweep.wlall;
		set = 1;
	}
#endif
	if (!set)
		return;

	popts->wrlvl_start = lane[0];
	popts->wrlvl_ctl_2 = lane[1] << 24 | lane[2] << 16 |
			     lane[3] << 8 | lane[4];
	popts->wrlvl_ctl_3 = lane[5] << 24 | lane[6] << 16 |
			     lane[7] << 8 | lane[8];
}

void fsl_ddr_board_options(memctl_options_t *popts,
				dimm_params_t *pdimm,
				unsigned int ctrl_num)
{
	const struct board_specific_parameters *pbsp, *pbsp_highest = NULL;
	ulong ddr_freq;

	if (ctrl_num > 1) {
		printf("Wrong parameter for controller number %d", ctrl_num);
		return;
	}
	if (!pdimm->n_ranks)
		return;

	if (popts->registered_dimm_en)
		pbsp = rdimms[ctrl_num];
	else
		pbsp = udimms[ctrl_num];


	/* Get clk_adjust, cpo, write_data_delay,2T, according to the board ddr
	 * freqency and n_banks specified in board_specific_parameters table.
	 */
	ddr_freq = get_ddr_freq(0) / 1000000;
	while (pbsp->datarate_mhz_high) {
		if (pbsp->n_ranks == pdimm->n_ranks) {
			if (ddr_freq <= pbsp->datarate_mhz_high) {
				popts->cpo_override = pbsp->cpo;
				popts->write_data_delay =
					pbsp->write_data_delay;
				popts->clk_adjust = pbsp->clk_adjust;
				popts->wrlvl_start = pbsp->wrlvl_start;
				popts->twot_en = pbsp->force_2t;
				goto found;
			}
			pbsp_highest = pbsp;
		}
		pbsp++;
	}

	if (pbsp_highest) {
		printf("Error: board specific timing not found for data rate %lu MT/s!\nTrying to use the highest speed (%u) parameters\n",
			ddr_freq, pbsp_highest->datarate_mhz_high);
		popts->cpo_override = pbsp_highest->cpo;
		popts->write_data_delay = pbsp_highest->write_data_delay;
		popts->clk_adjust = pbsp_highest->clk_adjust;
		popts->wrlvl_start = pbsp_highest->wrlvl_start;
		popts->twot_en = pbsp_highest->force_2t;
	} else {
		panic("DIMM is not supported by this board");
	}
found:
	/*
	 * Factors to consider for half-strength driver enable:
	 *	- number of DIMMs installed
	 */
	popts->half_strength_driver_enable = 0;
	/*
	 * Write leveling override
	 */
	popts->wrlvl_override = 1;
	popts->wrlvl_sample = 0xf;

	/*
	 * Rtt and Rtt_WR override
	 */
	popts->rtt_override = 0;

	/* Enable ZQ calibration */
	popts->zq_en = 1;

	/* DHC_EN =1, ODT = 60 Ohm */
	popts->ddr_cdr1 = DDR_CDR1_DHC_EN;

	/*
	 * AmigaOne X5000: developer DDR-diagnostic options, then any
	 * per-lane write-levelling start from the environment.
	 */
	if (IS_ENABLED(CONFIG_CYRUS_DDR_DIAG_RELAXED)) {
		/* Diagnosis only: 2T command timing and CL 10 (15 ns at
		 * 1333 MT/s, above any DDR3-1333 tAAmin). */
		popts->twot_en = 1;
		popts->cas_latency_override = 1;
		popts->cas_latency_override_value = 10;
	}

#if defined(CONFIG_CYRUS_DDR_DIAG_SWEEP) && defined(CONFIG_SPL_BUILD)
	if (cyrus_ddr_sweep.cpo >= 0)
		popts->cpo_override = cyrus_ddr_sweep.cpo;
#endif
	cyrus_wrlvl_lanes(popts, ctrl_num);
}

/*
 * AmigaOne X5000: fixed DRAM address the SPL uses to hand its DDR size to
 * the main U-Boot image. It must lie inside the static "boot page" TLB1
 * entry (CONFIG_SYS_MMC_U_BOOT_DST, 4 MiB, set up early from tlb_table[]
 * by cpu_init_early_f; see tlb.c and the CONFIG_SYS_INIT_L3_ADDR note in
 * cyrus.h) so that dram_init() can read it before setup_ddr_tlbs() has
 * mapped the rest of DDR; an address outside that entry (low DRAM, say)
 * takes a DTLB miss. 0x303FFFF0 sits at the top of that 4 MiB window,
 * past the 2 MiB image slot and its bss.
 */
#define CYRUS_DDR_HANDOFF_ADDR		(CONFIG_SYS_MMC_U_BOOT_DST + 0x3FFFF0)
#define CYRUS_DDR_HANDOFF_MAGIC		0x44524831	/* "DRH1" */

int dram_init(void)
{
	phys_size_t dram_size;

#if defined(CONFIG_SPL) && !defined(CONFIG_SPL_BUILD)
	/*
	 * AmigaOne X5000: this is the main U-Boot image, loaded and started
	 * by the SPL, which has already initialised DDR; this code and its
	 * stack are running from that DRAM. Calling fsl_ddr_sdram() again
	 * would re-program the live DDR controller under the running CPU,
	 * which hangs in fsl_ddr_set_memctl_regs(). Instead reuse the size
	 * the SPL handed over at CYRUS_DDR_HANDOFF_ADDR (see the
	 * CONFIG_SPL_BUILD branch below). setup_ddr_tlbs() only programs
	 * this image's own TLB1 entries and never touches the DDR
	 * controller, so it still runs.
	 */
	u32 *handoff = (u32 *)CYRUS_DDR_HANDOFF_ADDR;

	if (handoff[0] == CYRUS_DDR_HANDOFF_MAGIC) {
		dram_size = (phys_size_t)handoff[1] << 20;
		dram_size = setup_ddr_tlbs(dram_size / 0x100000);
		dram_size *= 0x100000;
		gd->ram_size = dram_size;
		return 0;
	}
#endif

	puts("Initializing....");

	if (!fsl_use_spd())
		panic("Cyrus only supports using SPD for DRAM\n");

	puts("using SPD\n");
	dram_size = fsl_ddr_sdram();

	dram_size = setup_ddr_tlbs(dram_size / 0x100000);
	dram_size *= 0x100000;

#ifdef CONFIG_SPL_BUILD
	/*
	 * AmigaOne X5000: run the optional DDR diagnostics, then hand the DDR
	 * size to the main U-Boot image (see CYRUS_DDR_HANDOFF_ADDR).
	 */
	cyrus_ddr_diag_after_init(dram_size);
	{
		u32 *handoff = (u32 *)CYRUS_DDR_HANDOFF_ADDR;

		if (IS_ENABLED(CONFIG_CYRUS_DDR_DIAG))
			printf("DDRDIAG: handoff store to %p ...\n", handoff);
		handoff[0] = CYRUS_DDR_HANDOFF_MAGIC;
		handoff[1] = (u32)(dram_size >> 20);
		/*
		 * mmc_boot()'s flush_cache() only covers the u-boot.bin load
		 * range (CONFIG_SYS_MMC_U_BOOT_DST..+_SIZE), which ends well
		 * before this handoff address. Without an explicit flush here
		 * the write stays dirty in the SPL's D-cache; the main
		 * image's _start_e500 invalidates (does not write back) the
		 * D-cache as one of its first acts, so the handoff would
		 * never reach DRAM.
		 */
		flush_cache(CYRUS_DDR_HANDOFF_ADDR, 8);
		if (IS_ENABLED(CONFIG_CYRUS_DDR_DIAG))
			printf("DDRDIAG: handoff store ok, read back %08x\n",
			       handoff[0]);
	}
#endif

	debug("    DDR: ");
	gd->ram_size = dram_size;

	return 0;
}
