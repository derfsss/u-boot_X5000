// SPDX-License-Identifier: GPL-2.0+
/*
 * AmigaOne X5000 (Varisys Cyrus) SPL DDR diagnostics (CONFIG_CYRUS_DDR_DIAG).
 *
 * A developer tool for fault-finding DDR problems, kept deliberately. It is
 * built only by the Cyrus_P5020_SDCARD_DDRDIAG*_defconfig images and is not
 * part of the normal firmware. It adds a few seconds to every boot.
 *
 * Runs in the SPL, from L3 SRAM, so it works even when DRAM does not.
 * Every line starts with "DDRDIAG:" so a serial capture can be grepped.
 *
 *  - early:    SoC/reset status, RCW words and the derived clocks
 *  - computed: full SPD dump + decode per DIMM, computed DIMM and common
 *              timing parameters, and the chosen controller options
 *  - D_INIT:   how long the controller's memory fill took; on a timeout,
 *              keep watching for up to 10 s more
 *  - enabled:  every controller register
 *  - after:    the LAW table and each controller's physical range; then,
 *              per controller whose D_INIT completed (a controller stuck
 *              in D_INIT is never touched: any access hangs the CPU):
 *              fill-pattern check (was RAM initialised before use?), a
 *              bracketed first DRAM store, walking 1s/0s on one
 *              doubleword (names DQ bits), and pattern tests with
 *              per-byte-lane error masks. Memory above the SPL's 2 GiB
 *              mapping is reached through a temporary TLB window. Stops
 *              if U-Boot's load address is on a failed controller.
 *  - copy:     crc32 of the U-Boot copy, a second SD read of the same
 *              blocks compared word by word; stop if they differ
 *
 * Byte lane numbering: lane L is the byte at (address & 7) == L within a
 * 64-bit beat, i.e. MDQ[8L..8L+7]. MDQ0 is the most significant bit of
 * the word at address offset 0 (big-endian).
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <clock_legacy.h>
#include <cpu_func.h>
#include <fsl_ddr_sdram.h>
#include <fsl_ddr.h>
#include <hang.h>
#include <memalign.h>
#include <mmc.h>
#include <timestamp.h>
#include <u-boot/crc.h>
#include <asm/fsl_law.h>
#include <asm/immap_85xx.h>
#include <asm/io.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <linux/delay.h>
#include <linux/sizes.h>
#include "ddr_diag.h"

#define DIAG		"DDRDIAG: "
#define NCTRL		CONFIG_SYS_NUM_DDR_CTLRS
#define TEST_LEN	(16UL * SZ_1M)
#define WALK_ADDR	0x01000000UL
#define COPY_SCRATCH	0x32000000UL	/* second SD read of U-Boot */

struct diag_ctrl {
	int populated;
	int dinit_requested;
	u32 data_init;
	int dinit_ms;		/* time to clear, or -1 = never seen clear */
};

/* Initialised, so this lives in .data (the SPL does not rely on .bss). */
static struct diag_ctrl st[NCTRL] = {
	[0 ... NCTRL - 1] = { .dinit_ms = -2 },
};
static phys_size_t diag_mapped = 0x1;

unsigned long get_board_sys_clk(void);	/* spl.c */

static struct ccsr_ddr __iomem *ddr_regs(unsigned int i)
{
	return i == 0 ? (void *)CONFIG_SYS_FSL_DDR_ADDR :
			(void *)CONFIG_SYS_FSL_DDR2_ADDR;
}

#ifdef CONFIG_CYRUS_DDR_DIAG_SWEEP
/*
 * Boot-to-boot timing sweep. Re-initialising the DDR controller at run
 * time is not a supported flow on the P5020 (a second init hangs, and the
 * controller cannot reset the DRAM), so each power-on applies one setting
 * at the normal first init. A step counter in an
 * unused SD sector advances on every boot; it is written before DDR init,
 * so a setting that hangs the board still moves the sweep on. Reflashing
 * the image zeroes the sector and restarts the sweep.
 *
 * All-lanes sweep: every byte lane gets the same write-levelling start,
 * 0x02..0x14 (1/4 .. 5/2 clock) in quarter-clock steps. Write levelling
 * is per lane, and the diagnostics report pass/fail per lane, so one run
 * maps every lane's working window at once. ddr.c also applies
 * cyrus_ddr_sweep.cpo (read gating) when it is set, but this sweep leaves
 * it at -1.
 *
 * WRLVL_START encoding: n/8 clock, 0x15-0x1f reserved (P5020 RM Rev. 5,
 * DDRx_DDR_WRLVL_CNTL/_2/_3).
 */
struct cyrus_ddr_sweep cyrus_ddr_sweep = { -1, -1 };

#define SWEEP_SECTOR	(0x7FE000 / 512)
#define SWEEP_MAGIC	0x44535733		/* "DSW3" */

static const u8 sweep_wlall[] = {
	0x02, 0x04, 0x06, 0x08, 0x0a, 0x0c, 0x0e, 0x10, 0x12, 0x14,
};
#define SWEEP_STEPS	ARRAY_SIZE(sweep_wlall)

static void diag_boot_sweep(void)
{
	ALLOC_CACHE_ALIGN_BUFFER(u32, buf, 512 / 4);
	struct mmc *mmc = find_mmc_device(0);
	u32 step;

	if (!mmc || mmc->block_dev.block_read(&mmc->block_dev, SWEEP_SECTOR,
					      1, buf) != 1) {
		printf(DIAG "BOOT SWEEP: cannot read SD sector %u; computed "
		       "settings\n", SWEEP_SECTOR);
		return;
	}
	step = buf[0] == SWEEP_MAGIC ? buf[1] : 0;

	memset(buf, 0, 512);
	buf[0] = SWEEP_MAGIC;
	buf[1] = step < SWEEP_STEPS ? step + 1 : step;
	if (mmc->block_dev.block_write(&mmc->block_dev, SWEEP_SECTOR, 1,
				       buf) != 1)
		printf(DIAG "BOOT SWEEP: counter write FAILED; this step will "
		       "repeat\n");

	if (step >= SWEEP_STEPS) {
		printf(DIAG "BOOT SWEEP complete (%u steps): computed settings; "
		       "reflash the image to restart\n", (u32)SWEEP_STEPS);
		return;
	}
	cyrus_ddr_sweep.wlall = sweep_wlall[step];
	printf(DIAG "BOOT SWEEP step %u of %u: all lanes wl=%02x\n", step + 1,
	       (u32)SWEEP_STEPS, cyrus_ddr_sweep.wlall);
}
#endif

/* ------------------------------------------------------------------ */
/* early                                                               */
/* ------------------------------------------------------------------ */

void cyrus_ddr_diag_early(void)
{
	ccsr_gur_t *gur = (void *)CONFIG_SYS_MPC85xx_GUTS_ADDR;
	u32 rcw0 = in_be32(&gur->rcwsr[0]);
	int i;

	printf(DIAG "SPL DDR diagnostics, built " U_BOOT_DATE " " U_BOOT_TIME
	       "%s\n", IS_ENABLED(CONFIG_CYRUS_DDR_DIAG_RELAXED) ?
	       " -- RELAXED timings (2T, CL10)" : "");
	printf(DIAG "svr=%08x pvr=%08x porsr1=%08x rstrqsr1=%08x rstrqmr1=%08x\n",
	       mfspr(SPRN_SVR), mfspr(SPRN_PVR), in_be32(&gur->porsr1),
	       in_be32(&gur->rstrqsr1), in_be32(&gur->rstrqmr1));
	for (i = 0; i < 16; i += 4)
		printf(DIAG "rcwsr[%2d..%2d]=%08x %08x %08x %08x\n", i, i + 3,
		       in_be32(&gur->rcwsr[i]), in_be32(&gur->rcwsr[i + 1]),
		       in_be32(&gur->rcwsr[i + 2]),
		       in_be32(&gur->rcwsr[i + 3]));
	printf(DIAG "clocks: sysclk=%lu MHz platform=%lu MHz (SYS_PLL %u:1) "
	       "DDR=%lu MT/s (MEM_PLL %u:1)\n",
	       get_board_sys_clk() / 1000000, get_bus_freq(0) / 1000000,
	       (rcw0 >> 25) & 0x1f, get_ddr_freq(0) / 1000000,
	       (rcw0 >> 17) & 0x1f);
	printf(DIAG "the SPL does not touch PCIe; SerDes is set up by the "
	       "RCW before any software runs\n");
#ifdef CONFIG_CYRUS_DDR_DIAG_SWEEP
	diag_boot_sweep();
#endif
}

/* ------------------------------------------------------------------ */
/* SPD and computed parameters                                         */
/* ------------------------------------------------------------------ */

static u16 spd_crc16(const u8 *p, int n)
{
	int crc = 0, i;

	while (--n >= 0) {
		crc ^= (int)*p++ << 8;
		for (i = 0; i < 8; i++)
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
	}
	return crc & 0xffff;
}

/* A DDR3 SPD time in ps: medium-timebase count plus a signed fine offset */
static int spd_ps(int mtb_ps, int mtb, s8 fine)
{
	return mtb * mtb_ps + fine;
}

static void diag_spd(unsigned int c, const u8 *b)
{
	static const char *const mtype[] = {
		"?", "RDIMM", "UDIMM", "SO-DIMM", "Micro-DIMM", "Mini-RDIMM",
		"Mini-UDIMM", "Mini-CDIMM", "72b-SO-UDIMM", "72b-SO-RDIMM",
		"72b-SO-CDIMM", "LRDIMM", "16b-SO-DIMM", "32b-SO-DIMM",
	};
	char part[19];
	int mtb_ps, i, n, cov;
	u16 stored, calc;

	for (i = 0; i < 256; i += 16)
		printf(DIAG "c%u spd %02x: %02x %02x %02x %02x %02x %02x %02x "
		       "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n", c, i,
		       b[i], b[i + 1], b[i + 2], b[i + 3], b[i + 4], b[i + 5],
		       b[i + 6], b[i + 7], b[i + 8], b[i + 9], b[i + 10],
		       b[i + 11], b[i + 12], b[i + 13], b[i + 14], b[i + 15]);

	if (b[2] != 0x0b) {
		printf(DIAG "c%u SPD memory type 0x%02x is not DDR3; "
		       "not decoded\n", c, b[2]);
		return;
	}

	cov = (b[0] & 0x80) ? 117 : 126;
	stored = b[126] | (b[127] << 8);
	calc = spd_crc16(b, cov);
	printf(DIAG "c%u SPD CRC over bytes 0-%d: stored %04x computed %04x "
	       "-> %s\n", c, cov - 1, stored, calc,
	       stored == calc ? "OK" : "BAD (SPD corrupt or misread)");

	for (i = 0; i < 18; i++)
		part[i] = (b[128 + i] >= 0x20 && b[128 + i] < 0x7f) ?
			  b[128 + i] : '.';
	part[18] = 0;
	printf(DIAG "c%u SPD: DDR3 %s, part '%s' rev %02x%02x, SPD rev %x.%x, "
	       "%d bytes used\n", c,
	       (b[3] & 0xf) < ARRAY_SIZE(mtype) ? mtype[b[3] & 0xf] : "?",
	       part, b[146], b[147], b[1] >> 4, b[1] & 0xf,
	       (b[0] & 0xf) == 1 ? 128 : (b[0] & 0xf) == 2 ? 176 :
	       (b[0] & 0xf) == 3 ? 256 : 0);
	printf(DIAG "c%u SPD: module mfr %02x%02x, DRAM mfr %02x%02x, "
	       "made 20%02x week %02x at location %02x, serial %02x%02x%02x%02x\n",
	       c, b[117], b[118], b[148], b[149], b[120], b[121], b[119],
	       b[122], b[123], b[124], b[125]);

	n = 0;
	printf(DIAG "c%u SPD: %u rank(s) of x%u devices, %u Mb/die, %u banks, "
	       "%u row / %u col bits, %u-bit bus, %s, 1.5V %s%s%s\n", c,
	       ((b[7] >> 3) & 7) + 1, 4 << (b[7] & 7),
	       256 << (b[4] & 0xf), 8 << ((b[4] >> 4) & 7),
	       ((b[5] >> 3) & 7) + 12, (b[5] & 7) + 9,
	       8 << (b[8] & 7), (b[8] >> 3) & 3 ? "ECC" : "no ECC",
	       b[6] & 1 ? "NOT supported" : "ok",
	       b[6] & 2 ? ", 1.35V ok" : "", b[6] & 4 ? ", 1.25V ok" : "");

	mtb_ps = b[11] ? b[10] * 1000 / b[11] : 0;
	printf(DIAG "c%u SPD: MTB %d ps, FTB %d/%d ps, CL supported:", c,
	       mtb_ps, b[9] >> 4, b[9] & 0xf);
	for (i = 0; i < 15; i++)
		if ((b[14] | (b[15] << 8)) & (1 << i)) {
			printf(" %d", i + 4);
			n++;
		}
	printf("%s\n", n ? "" : " none");
	printf(DIAG "c%u SPD timing ps: tCKmin %d tAAmin %d tRCDmin %d "
	       "tRPmin %d tRASmin %d tRCmin %d tRFCmin %d\n", c,
	       spd_ps(mtb_ps, b[12], (s8)b[34]),
	       spd_ps(mtb_ps, b[16], (s8)b[35]),
	       spd_ps(mtb_ps, b[18], (s8)b[36]),
	       spd_ps(mtb_ps, b[20], (s8)b[37]),
	       spd_ps(mtb_ps, ((b[21] & 0xf) << 8) | b[22], 0),
	       spd_ps(mtb_ps, ((b[21] >> 4) << 8) | b[23], (s8)b[38]),
	       spd_ps(mtb_ps, b[24] | (b[25] << 8), 0));
	printf(DIAG "c%u SPD timing ps: tWRmin %d tRRDmin %d tWTRmin %d "
	       "tRTPmin %d tFAWmin %d; options %02x thermal %02x sensor %02x "
	       "height %02x raw-card %02x mirror %02x\n", c,
	       spd_ps(mtb_ps, b[17], 0), spd_ps(mtb_ps, b[19], 0),
	       spd_ps(mtb_ps, b[26], 0), spd_ps(mtb_ps, b[27], 0),
	       spd_ps(mtb_ps, ((b[28] & 0xf) << 8) | b[29], 0),
	       b[30], b[31], b[32], b[60], b[62], b[63]);
}

void fsl_ddr_board_diag_computed(const fsl_ddr_info_t *pinfo)
{
	unsigned int c, last = pinfo->first_ctrl + pinfo->num_ctrls - 1;

	for (c = pinfo->first_ctrl; c <= last && c < NCTRL; c++) {
		const u8 *spd = (const u8 *)&pinfo->spd_installed_dimms[c][0];
		const dimm_params_t *d = &pinfo->dimm_params[c][0];
		const common_timing_params_t *t =
			&pinfo->common_timing_params[c];
		const memctl_options_t *o = &pinfo->memctl_opts[c];
		const fsl_ddr_cfg_regs_t *r = &pinfo->fsl_ddr_config_reg[c];

		printf(DIAG "---- controller %u (SPD at I2C 0x%02x) ----\n", c,
		       c == 0 ? SPD_EEPROM_ADDRESS1 : SPD_EEPROM_ADDRESS2);
		if (!spd[0] && !spd[2]) {
			printf(DIAG "c%u no SPD data (empty slot or I2C read "
			       "failed)\n", c);
			continue;
		}
		diag_spd(c, spd);

		if (d->n_ranks)
			printf(DIAG "c%u dimm: '%s' %u rank(s) %u MiB "
			       "(%u MiB/rank) width %u (primary %u ecc %u) "
			       "x%u devices rows %u cols %u banks %u "
			       "registered %u mirrored %u\n", c, d->mpart,
			       d->n_ranks, (u32)(d->capacity >> 20),
			       (u32)(d->rank_density >> 20), d->data_width,
			       d->primary_sdram_width, d->ec_sdram_width,
			       d->device_width, d->n_row_addr, d->n_col_addr,
			       d->n_banks_per_sdram_device, d->registered_dimm,
			       d->mirrored_dimm);
		printf(DIAG "c%u common: dimms %u tCKmin %u ps lowest CL %u "
		       "derated CL %u AL %u ecc-capable %u total %u MiB "
		       "base 0x%x\n", c, t->ndimms_present, t->tckmin_x_ps,
		       t->lowest_common_spd_caslat,
		       t->highest_common_derated_caslat, t->additive_latency,
		       t->all_dimms_ecc_capable, (u32)(t->total_mem >> 20),
		       (u32)t->base_address);
		printf(DIAG "c%u opts: interleave %u/%u ecc %u D_INIT %u 2T %u "
		       "CL-override %u(%u) clk_adjust %u wrlvl %u start %u "
		       "sample %u cpo %u wrdata-delay %u half-drive %u zq %u "
		       "cdr1 %08x\n", c, o->memctl_interleaving,
		       o->memctl_interleaving_mode, o->ecc_mode,
		       o->ecc_init_using_memctl, o->twot_en,
		       o->cas_latency_override, o->cas_latency_override_value,
		       o->clk_adjust, o->wrlvl_en, o->wrlvl_start,
		       o->wrlvl_sample, o->cpo_override, o->write_data_delay,
		       o->half_strength_driver_enable, o->zq_en, o->ddr_cdr1);

		st[c].populated = t->ndimms_present;
		st[c].dinit_requested =
			!!(r->ddr_sdram_cfg_2 & SDRAM_CFG2_D_INIT);
		st[c].data_init = r->ddr_data_init;
		printf(DIAG "c%u memory fill before use (D_INIT): %s, "
		       "pattern %08x\n", c,
		       st[c].dinit_requested ? "requested" : "NOT requested",
		       st[c].data_init);
	}
}

/* ------------------------------------------------------------------ */
/* D_INIT                                                              */
/* ------------------------------------------------------------------ */

void fsl_ddr_board_diag_dinit(unsigned int c, int budget, int remaining)
{
	struct ccsr_ddr __iomem *ddr = ddr_regs(c);
	int ms;

	if (c >= NCTRL)
		return;
	if (remaining > 0) {
		st[c].dinit_ms = (budget - remaining) * 10;
		printf(DIAG "c%u D_INIT clear after ~%d ms (budget %d ms)\n",
		       c, st[c].dinit_ms, budget * 10);
		return;
	}

	printf(DIAG "c%u D_INIT still set after the %d ms budget; watching "
	       "for 10 s more (sdram_cfg_2=%08x dsr1=%08x dsr2=%08x)\n", c,
	       budget * 10, in_be32(&ddr->sdram_cfg_2), in_be32(&ddr->ddr_dsr1),
	       in_be32(&ddr->ddr_dsr2));
	for (ms = 10; ms <= 10000; ms += 10) {
		udelay(10000);
		if (!(in_be32(&ddr->sdram_cfg_2) & SDRAM_CFG2_D_INIT)) {
			st[c].dinit_ms = budget * 10 + ms;
			printf(DIAG "c%u D_INIT cleared %d ms after the budget "
			       "(slow fill, total ~%d ms)\n", c, ms,
			       st[c].dinit_ms);
			return;
		}
		if (!(ms % 1000))
			printf(DIAG "c%u D_INIT still set at +%d s\n", c,
			       ms / 1000);
	}
	st[c].dinit_ms = -1;
	printf(DIAG "c%u D_INIT NEVER cleared: the controller's memory fill "
	       "is stuck; DRAM contents are undefined\n", c);
}

/* ------------------------------------------------------------------ */
/* controller registers                                                */
/* ------------------------------------------------------------------ */

#define R(x)	in_be32(&ddr->x)

static void diag_regs(unsigned int c, const char *when)
{
	struct ccsr_ddr __iomem *ddr = ddr_regs(c);

	printf(DIAG "c%u registers %s:\n", c, when);
	printf(DIAG "c%u sdram_cfg=%08x cfg_2=%08x interval=%08x clk_cntl=%08x "
	       "data_init=%08x\n", c, R(sdram_cfg), R(sdram_cfg_2),
	       R(sdram_interval), R(sdram_clk_cntl), R(sdram_data_init));
	printf(DIAG "c%u cs0 bnds=%08x cfg=%08x cs1 bnds=%08x cfg=%08x "
	       "cs2 cfg=%08x cs3 cfg=%08x\n", c, R(cs0_bnds), R(cs0_config),
	       R(cs1_bnds), R(cs1_config), R(cs2_config), R(cs3_config));
	printf(DIAG "c%u timing_cfg 0=%08x 1=%08x 2=%08x 3=%08x 4=%08x 5=%08x\n",
	       c, R(timing_cfg_0), R(timing_cfg_1), R(timing_cfg_2),
	       R(timing_cfg_3), R(timing_cfg_4), R(timing_cfg_5));
	printf(DIAG "c%u mode=%08x mode_2=%08x md_cntl=%08x zq=%08x "
	       "wrlvl=%08x %08x %08x\n", c, R(sdram_mode), R(sdram_mode_2),
	       R(sdram_md_cntl), R(ddr_zq_cntl), R(ddr_wrlvl_cntl),
	       R(ddr_wrlvl_cntl_2), R(ddr_wrlvl_cntl_3));
	printf(DIAG "c%u cdr1=%08x cdr2=%08x dsr1=%08x dsr2=%08x ip_rev=%08x "
	       "%08x debug[0..3]=%08x %08x %08x %08x\n", c, R(ddr_cdr1),
	       R(ddr_cdr2), R(ddr_dsr1), R(ddr_dsr2), R(ip_rev1), R(ip_rev2),
	       R(debug[0]), R(debug[1]), R(debug[2]), R(debug[3]));
	printf(DIAG "c%u err_detect=%08x err_disable=%08x attr=%08x "
	       "addr=%08x:%08x data=%08x:%08x sbe=%08x "
	       "(non-ECC DIMMs: data errors can NOT show here)\n", c,
	       R(err_detect), R(err_disable), R(capture_attributes),
	       R(capture_ext_address), R(capture_address),
	       R(capture_data_hi), R(capture_data_lo), R(err_sbe));
}

void fsl_ddr_board_diag_enabled(const fsl_ddr_info_t *pinfo)
{
	unsigned int c;

	for (c = 0; c < NCTRL; c++)
		if (st[c].populated)
			diag_regs(c, "after enable");
}

/* ------------------------------------------------------------------ */
/* DRAM tests                                                          */
/* ------------------------------------------------------------------ */

struct lane_acc {
	u32 errors;
	u8 mask[8];		/* OR of flipped bits per byte lane */
	u32 lane_errors[8];
};

static void acc_add(struct lane_acc *a, ulong addr, u32 diff)
{
	int b;

	for (b = 0; b < 4; b++) {
		u8 m = diff >> (24 - 8 * b);

		if (m) {
			a->mask[(addr & 4) + b] |= m;
			a->lane_errors[(addr & 4) + b]++;
		}
	}
	a->errors++;
}

static void acc_print(const char *what, const struct lane_acc *a)
{
	printf(DIAG "%s: %u bad word(s); lane masks 0-7: %02x %02x %02x %02x "
	       "%02x %02x %02x %02x; lane counts %u %u %u %u %u %u %u %u\n",
	       what, a->errors, a->mask[0], a->mask[1], a->mask[2], a->mask[3],
	       a->mask[4], a->mask[5], a->mask[6], a->mask[7],
	       a->lane_errors[0], a->lane_errors[1], a->lane_errors[2],
	       a->lane_errors[3], a->lane_errors[4], a->lane_errors[5],
	       a->lane_errors[6], a->lane_errors[7]);
}

static void wr(ulong addr, u32 v)
{
	*(volatile u32 *)addr = v;
}

static u32 rd(ulong addr)
{
	return *(volatile u32 *)addr;
}

/* Write back and drop the lines so the next read comes from DRAM. */
static void to_dram(ulong start, ulong len)
{
	flush_dcache_range(start, start + len);
	asm volatile("sync");
}

static void diag_fill_check(unsigned int c, ulong vbase, ulong len)
{
	struct lane_acc a = { 0 };
	u32 expect = st[c].data_init, shown = 0, samples = 0;
	ulong off;

	if (!st[c].dinit_requested) {
		printf(DIAG "c%u fill check skipped: D_INIT not requested, RAM "
		       "is neither filled nor zeroed before use\n", c);
		return;
	}

	printf(DIAG "c%u fill check: 2 words per MiB over %u MiB at %08lx, "
	       "expecting %08x (first DRAM READ on c%u now)\n", c,
	       (u32)(len >> 20), vbase, expect, c);
	for (off = 0; off < len; off += SZ_1M) {
		ulong p[2] = { vbase + off, vbase + off + 0x80004 };
		int k;

		for (k = 0; k < 2; k++) {
			u32 v = rd(p[k]);

			samples++;
			if (v != expect) {
				acc_add(&a, p[k], v ^ expect);
				if (shown++ < 8)
					printf(DIAG "  fill @%08lx = %08x\n",
					       p[k], v);
			}
		}
	}
	printf(DIAG "c%u fill check: %u of %u samples differ from %08x\n", c,
	       a.errors, samples, expect);
	if (a.errors)
		acc_print("fill check", &a);
}

static void diag_walk(ulong addr)
{
	u32 bad_hi = 0, bad_lo = 0;
	int bit, inv;

	printf(DIAG "first DRAM STORE to %08lx ...\n", addr);
	wr(addr, 0x12345678);
	to_dram(addr, 32);
	printf(DIAG "first DRAM store ok, read back %08x\n", rd(addr));

	for (inv = 0; inv < 2; inv++) {
		for (bit = 0; bit < 64; bit++) {
			u32 hi = bit < 32 ? 0x80000000U >> bit : 0;
			u32 lo = bit < 32 ? 0 : 0x80000000U >> (bit - 32);

			if (inv) {
				hi = ~hi;
				lo = ~lo;
			}
			wr(addr, hi);
			wr(addr + 4, lo);
			to_dram(addr, 32);
			bad_hi |= rd(addr) ^ hi;
			bad_lo |= rd(addr + 4) ^ lo;
		}
	}
	if (!bad_hi && !bad_lo) {
		printf(DIAG "walking 1s/0s on %08lx: all 64 DQ bits ok\n",
		       addr);
		return;
	}
	printf(DIAG "walking 1s/0s on %08lx: failing DQ bits (MDQ n):", addr);
	for (bit = 0; bit < 64; bit++) {
		u32 m = bit < 32 ? bad_hi : bad_lo;

		if (m & (0x80000000U >> (bit & 31)))
			printf(" %d", bit);
	}
	printf("\n");
}

static u32 pattern(int k, ulong addr)
{
	switch (k) {
	case 0:
		return addr;
	case 1:
		return ~addr;
	case 2:
		return (addr & 4) ? 0xaaaaaaaa : 0x55555555;
	default:
		return (addr & 8) ? 0xffffffff : 0x00000000;
	}
}

static u32 diag_region(ulong base, int pass)
{
	static const char *const pname[] = {
		"addr", "~addr", "55/aa", "00/ff",
	};
	u32 total = 0;
	int k;

	for (k = 0; k < 4; k++) {
		struct lane_acc a = { 0 };
		char what[48];
		ulong addr, end = base + TEST_LEN;
		int shown = 0;

		for (addr = base; addr < end; addr += 4)
			wr(addr, pattern(k, addr));
		to_dram(base, TEST_LEN);
		for (addr = base; addr < end; addr += 4) {
			u32 v = rd(addr), e = pattern(k, addr);

			if (v != e) {
				acc_add(&a, addr, v ^ e);
				if (shown++ < 3)
					printf(DIAG "  @%08lx wrote %08x "
					       "read %08x\n", addr, e, v);
			}
		}
		snprintf(what, sizeof(what), "test pass %d %s %08lx+16M",
			 pass, pname[k], base);
		acc_print(what, &a);
		total += a.errors;
	}
	return total;
}

/*
 * Physical range a controller serves, from its chip-select bounds
 * (SA/EA in 16 MiB units). Returns 0 if no chip select is enabled.
 */
static int ctrl_range(unsigned int c, u64 *base, u64 *end, int *intlv)
{
	struct ccsr_ddr __iomem *ddr = ddr_regs(c);
	u32 bnds[4] = { R(cs0_bnds), R(cs1_bnds), R(cs2_bnds), R(cs3_bnds) };
	u32 cfg[4] = { R(cs0_config), R(cs1_config), R(cs2_config),
		       R(cs3_config) };
	int i, found = 0;

	*intlv = 0;
	for (i = 0; i < 4; i++) {
		u64 sa = (u64)((bnds[i] >> 16) & 0xffff) << 24;
		u64 ea = (u64)((bnds[i] & 0xffff) + 1) << 24;

		if (!(cfg[i] & 0x80000000))
			continue;
		if (cfg[i] & 0x20000000)
			*intlv = 1;
		if (!found || sa < *base)
			*base = sa;
		if (!found || ea > *end)
			*end = ea;
		found = 1;
	}
	return found;
}

/*
 * Controller memory above the 2 GiB the SPL maps is reached through a
 * temporary 1 GiB window over the PCIe entry (TLB1 entry 4, virtual
 * 0x80000000), which the SPL never uses; it is restored afterwards.
 */
#define WIN_VIRT	CONFIG_SYS_PCIE1_MEM_VIRT
#define WIN_ESEL	4

static void window_map(u64 phys)
{
	set_tlb(1, WIN_VIRT, phys, MAS3_SX | MAS3_SW | MAS3_SR, MAS2_M,
		0, WIN_ESEL, BOOKE_PAGESZ_1G, 1);
	asm volatile("isync");
}

static void window_restore(void)
{
	flush_dcache_range(WIN_VIRT, WIN_VIRT + SZ_1G);
	set_tlb(1, WIN_VIRT, CONFIG_SYS_PCIE1_MEM_PHYS, MAS3_SW | MAS3_SR,
		MAS2_I | MAS2_G, 0, WIN_ESEL, BOOKE_PAGESZ_1G, 1);
	asm volatile("isync");
}

static void diag_laws(void)
{
	ccsr_local_t *ccm = (void *)CONFIG_SYS_FSL_CORENET_CCM_ADDR;
	int i;

	for (i = 0; i < CONFIG_SYS_FSL_NUM_LAWS; i++) {
		u32 ar = in_be32(&ccm->law[i].lawar);
		u32 tgt = (ar >> 20) & 0xff;

		if (ar & LAW_EN)
			printf(DIAG "LAW%u %x:%08x size 2^%u target %02x%s\n",
			       i, in_be32(&ccm->law[i].lawbarh),
			       in_be32(&ccm->law[i].lawbarl), (ar & 0x3f) + 1,
			       tgt,
			       tgt == LAW_TRGT_IF_DDR_1 ? " = DDR ctrl 0" :
			       tgt == LAW_TRGT_IF_DDR_2 ? " = DDR ctrl 1" :
			       tgt == LAW_TRGT_IF_DDR_INTRLV ?
			       " = DDR interleaved" : "");
	}
}

void cyrus_ddr_diag_after_init(phys_size_t total)
{
	u64 base[NCTRL], end[NCTRL];
	int ok[NCTRL], has[NCTRL], intlv = 0, any_bad = 0;
	u32 errors = 0;
	int c, pass, tested = 0;
	ulong dst = CONFIG_SYS_MMC_U_BOOT_DST;

	diag_mapped = min(total, (phys_size_t)CONFIG_MAX_MEM_MAPPED);
	printf(DIAG "---- DRAM checks (%u MiB total, %u MiB mapped by the "
	       "SPL) ----\n", (u32)(total >> 20), (u32)(diag_mapped >> 20));
	diag_laws();

	for (c = 0; c < NCTRL; c++) {
		int il = 0;

		has[c] = st[c].populated &&
			 ctrl_range(c, &base[c], &end[c], &il);
		ok[c] = has[c] && st[c].dinit_ms >= 0;
		intlv |= il;
		if (!has[c])
			continue;
		if (!ok[c])
			any_bad = 1;
		printf(DIAG "c%u serves phys %x_%08x..%x_%08x; D_INIT %s (%d ms)"
		       " -> %s\n", c, (u32)(base[c] >> 32), (u32)base[c],
		       (u32)((end[c] - 1) >> 32), (u32)(end[c] - 1),
		       st[c].dinit_ms >= 0 ? "cleared" :
		       st[c].dinit_ms == -1 ? "NEVER cleared" : "not seen",
		       st[c].dinit_ms, ok[c] ? "will be tested" :
		       "NOT accessed (a read would hang the CPU)");
	}

	if (intlv && any_bad) {
		printf(DIAG "controllers are interleaved and one failed: no "
		       "DRAM can be accessed safely\n");
	} else {
		for (c = 0; c < NCTRL; c++) {
			ulong vbase, len;

			if (!ok[c])
				continue;
			if (base[c] < diag_mapped) {
				vbase = base[c];
				len = min(end[c], (u64)diag_mapped) - base[c];
			} else {
				window_map(base[c]);
				vbase = WIN_VIRT;
				len = min(end[c] - base[c], (u64)SZ_1G);
				printf(DIAG "c%u reached through a window: virtual "
				       "%08lx = phys %x_%08x\n", c, vbase,
				       (u32)(base[c] >> 32), (u32)base[c]);
			}

			diag_fill_check(c, vbase, len);
			diag_walk(vbase + SZ_16M);
			for (pass = 1; pass <= 2; pass++) {
				errors += diag_region(vbase + SZ_16M, pass);
				errors += diag_region(vbase + len / 2, pass);
				errors += diag_region(vbase + len - TEST_LEN,
						      pass);
				if (vbase <= dst && dst + TEST_LEN <= vbase + len)
					errors += diag_region(dst, pass);
			}
			tested++;
			if (vbase == WIN_VIRT)
				window_restore();
		}
	}

	for (c = 0; c < NCTRL; c++)
		if (has[c])
			diag_regs(c, "after the tests");
	printf(DIAG "RESULT: %d controller(s) tested, pattern tests %s "
	       "(%u bad words)\n", tested, !tested ? "NOT RUN" :
	       errors ? "FAILED" : "passed", errors);

	/* U-Boot is copied to dst: its controller must be usable. */
	for (c = 0; c < NCTRL; c++)
		if (has[c] && !ok[c] &&
		    (intlv || (base[c] <= dst && dst < end[c]))) {
			printf(DIAG "U-Boot's load address %08lx is on c%u, whose "
			       "initialisation failed: stopping here. "
			       "Power off.\n", dst, c);
			hang();
		}
}

/* ------------------------------------------------------------------ */
/* U-Boot copy check                                                   */
/* ------------------------------------------------------------------ */

int spl_mmc_diag_after_copy(struct mmc *mmc, uint blk_start, uint blk_cnt,
			    void *dst, u32 len)
{
	struct lane_acc a = { 0 };
	u32 crc1, crc2, crc3, i, shown = 0;
	u32 *p = dst, *q = (u32 *)COPY_SCRATCH;
	uint ret;

	/* Every read below must come from DRAM, not the data cache. */
	to_dram((ulong)dst, len);
	crc1 = crc32(0, dst, len);
	printf(DIAG "U-Boot copy: crc32 %08x over %u bytes (compare: crc32 of "
	       "SD image bytes 0x%x..0x%x)\n", crc1, len, blk_start * 512,
	       blk_start * 512 + len);

	if (diag_mapped < COPY_SCRATCH + 0x1000000) {
		printf(DIAG "U-Boot copy: no room for a second read\n");
		return 0;
	}
	ret = mmc->block_dev.block_read(&mmc->block_dev, blk_start, blk_cnt,
					(void *)COPY_SCRATCH);
	if (ret != blk_cnt) {
		printf(DIAG "U-Boot copy: second SD read failed (%u of %u "
		       "blocks); cannot compare, starting U-Boot anyway\n",
		       ret, blk_cnt);
		return 0;
	}
	to_dram(COPY_SCRATCH, len);
	to_dram((ulong)dst, len);
	crc2 = crc32(0, (void *)COPY_SCRATCH, len);
	crc3 = crc32(0, dst, len);
	to_dram(COPY_SCRATCH, len);
	to_dram((ulong)dst, len);
	printf(DIAG "U-Boot copy: second SD read to %08lx ret=%u crc32 %08x; "
	       "first copy re-read crc32 %08x\n", COPY_SCRATCH, ret, crc2,
	       crc3);

	for (i = 0; i < len / 4; i++)
		if (p[i] != q[i]) {
			acc_add(&a, (ulong)&p[i], p[i] ^ q[i]);
			if (shown++ < 8)
				printf(DIAG "  +%06x first %08x second %08x\n",
				       i * 4, p[i], q[i]);
		}
	acc_print("U-Boot copy vs second read", &a);

	if (a.errors || crc1 != crc3) {
		printf(DIAG "U-Boot copy is NOT stable in DRAM: the same SD "
		       "blocks read back differently. Not starting U-Boot; "
		       "power off.\n");
		return 1;
	}
	printf(DIAG "U-Boot copy stable in DRAM; starting it\n");
	return 0;
}
