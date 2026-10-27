/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * AmigaOne X5000 (Varisys Cyrus) SPL DDR diagnostics, see ddr_diag.c.
 * A developer tool; when CONFIG_CYRUS_DDR_DIAG is off the calls below are
 * empty inlines.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#ifndef __CYRUS_DDR_DIAG_H
#define __CYRUS_DDR_DIAG_H

/*
 * Boot-to-boot timing sweep (CONFIG_CYRUS_DDR_DIAG_SWEEP): the setting
 * this boot applies at the normal DDR init; -1 = use the computed value.
 */
struct cyrus_ddr_sweep {
	int cpo;	/* TIMING_CFG_2[CPO] */
	int wlall;	/* write-levelling start, every byte lane */
};

#if defined(CONFIG_CYRUS_DDR_DIAG_SWEEP) && defined(CONFIG_SPL_BUILD)
extern struct cyrus_ddr_sweep cyrus_ddr_sweep;
#endif

#if defined(CONFIG_CYRUS_DDR_DIAG) && defined(CONFIG_SPL_BUILD)
void cyrus_ddr_diag_early(void);
void cyrus_ddr_diag_after_init(phys_size_t mapped);
#else
static inline void cyrus_ddr_diag_early(void) {}
static inline void cyrus_ddr_diag_after_init(phys_size_t mapped) {}
#endif

#endif
