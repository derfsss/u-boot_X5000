// SPDX-License-Identifier: GPL-2.0+
/*
 * Based on corenet_ds.c
 */

#include <common.h>
#include <command.h>
#include <env.h>
#include <fdt_support.h>
#include <image.h>
#include <init.h>
#include <netdev.h>
#include <linux/compiler.h>
#include <asm/mmu.h>
#include <asm/processor.h>
#include <asm/cache.h>
#include <asm/immap_85xx.h>
#include <asm/fsl_law.h>
#include <asm/fsl_serdes.h>
#include <asm/fsl_portals.h>
#include <asm/fsl_liodn.h>
#include <fm_eth.h>
#include <pci.h>
/* AmigaOne X5000: for video_splash_blank() in misc_init_r(). */
#include <video_fb.h>
#include <linux/string.h>

#include "cyrus.h"
#include "../common/eeprom.h"

#define GPIO_OPENDRAIN 0x30000000
#define GPIO_DIR       0x3c000004
#define GPIO_INITIAL   0x30000000
#define GPIO_VGA_SWITCH 0x00001000

int checkboard(void)
{
	printf("Board: CYRUS\n");

	return 0;
}

int board_early_init_f(void)
{
	ccsr_gur_t *gur = (void *)(CONFIG_SYS_MPC85xx_GUTS_ADDR);
	ccsr_gpio_t *pgpio = (void *)(CONFIG_SYS_MPC85xx_GPIO_ADDR);

	/*
	 * Only use DDR1_MCK0/3 and DDR2_MCK0/3
	 * disable DDR1_MCK1/2/4/5 and DDR2_MCK1/2/4/5 to reduce
	 * the noise introduced by these unterminated and unused clock pairs.
	 */
	setbits_be32(&gur->ddrclkdr, 0x001B001B);

	/* Set GPIO reset lines to open-drain, tristate */
	setbits_be32(&pgpio->gpdat, GPIO_INITIAL);
	setbits_be32(&pgpio->gpodr, GPIO_OPENDRAIN);

	/* Set GPIO Direction */
	setbits_be32(&pgpio->gpdir, GPIO_DIR);

	return 0;
}

int board_early_init_r(void)
{
	fsl_lbc_t *lbc = LBC_BASE_ADDR;

	out_be32(&lbc->lbcr, 0);
	/* 1 clock LALE cycle */
	out_be32(&lbc->lcrr, 0x80000000 | CONFIG_SYS_LBC_LCRR);

	set_liodns();

#ifdef CONFIG_SYS_DPAA_QBMAN
	setup_qbman_portals();
#endif
	print_lbc_regs();
	return 0;
}

int misc_init_r(void)
{
#ifdef CONFIG_CFB_CONSOLE
	/*
	 * AmigaOne X5000: quiet boot (stdout without "vga"). Replace the
	 * console-coloured screen left by video init with plain black as
	 * early as possible; the autoboot countdown then draws its progress
	 * bar on it.
	 */
	const char *so = env_get("stdout");

	if (so && !strstr(so, "vga"))
		video_splash_blank();
#endif
	return 0;
}

int ft_board_setup(void *blob, struct bd_info *bd)
{
	phys_addr_t base;
	phys_size_t size;

	ft_cpu_setup(blob, bd);

	base = env_get_bootm_low();
	size = env_get_bootm_size();

	fdt_fixup_memory(blob, (u64)base, (u64)size);

#ifdef CONFIG_PCI
	pci_of_setup(blob, bd);
#endif

	fdt_fixup_liodn(blob);
	fsl_fdt_fixup_dr_usb(blob, bd);

#ifdef CONFIG_SYS_DPAA_FMAN
	fdt_fixup_fman_ethernet(blob);
#endif

	return 0;
}

int mac_read_from_eeprom(void)
{
	int ret;

	init_eeprom(CONFIG_SYS_EEPROM_BUS_NUM,
		CONFIG_SYS_I2C_EEPROM_ADDR,
		CONFIG_SYS_I2C_EEPROM_ADDR_LEN);

	/*
	 * AmigaOne X5000: the NXID MAC table is unprogrammed (mac_count == 0);
	 * the per-unit MAC addresses are factory-stored at fixed I2C locations
	 * (CONFIG_SYS_I2C_MAC1/MAC2_*). Read the NXID table first (for the
	 * banner and any unit that does have it programmed), then fill
	 * ethaddr/eth1addr from the fixed locations. Upstream Cyrus defines the
	 * fixed-location config and mac_read_from_fixed_id() but never calls
	 * it, which leaves ethaddr/eth1addr unset ("No ethernet found").
	 */
	ret = mac_read_from_eeprom_common();
	mac_read_from_fixed_id();

	return ret;
}
