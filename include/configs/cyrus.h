/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Based on corenet_ds.h
 */

#ifndef __CONFIG_H
#define __CONFIG_H

#include <linux/stringify.h>

#if !defined(CONFIG_ARCH_P5020) && !defined(CONFIG_ARCH_P5040)
#error Must call Cyrus CONFIG with a specific CPU enabled.
#endif

/* AmigaOne X5000: the SDCARD defconfigs already set CONFIG_SDCARD. */
#ifndef CONFIG_SDCARD
#define CONFIG_SDCARD
#endif
#define CONFIG_FSL_SATA_V2
#define CONFIG_PCIE3
#define CONFIG_PCIE4
#ifdef CONFIG_ARCH_P5020
#define CONFIG_SYS_FSL_RAID_ENGINE
#define CONFIG_SYS_DPAA_RMAN
#endif
#define CONFIG_SYS_DPAA_PME

/*
 * Corenet DS style board configuration file
 */
#define CONFIG_RAMBOOT_TEXT_BASE	CONFIG_SYS_TEXT_BASE
#ifdef CONFIG_SPL
/*
 * AmigaOne X5000: two-stage SPL/SDCARD boot. The full U-Boot is loaded
 * into and runs from DDR.
 */
#define CONFIG_RESET_VECTOR_ADDRESS	0x30000FFC
/*
 * NO_RESETVEC only for the DDR-resident main U-Boot: the SPL loads it to
 * CONFIG_SYS_MMC_U_BOOT_DST and jumps to CONFIG_SYS_MMC_U_BOOT_START
 * (= its .text - 0x1000 = its .bootpg), so it needs no CPU reset vector.
 * The SPL itself keeps a reset vector, placed above .text at 0xFFFFFFFC by
 * u-boot-spl.lds (via BOOT_PAGE_OFFSET/RESET_VECTOR_OFFSET below), so its
 * flat binary starts at CONFIG_SPL_TEXT_BASE = the pblimage -a load addr,
 * and the CPU resets straight into it. NO_RESETVEC on the SPL would put
 * .bootpg a page below .text and shift the whole image by 0x1000, so it
 * would not start.
 */
#ifndef CONFIG_SPL_BUILD
#define CONFIG_SYS_MPC85XX_NO_RESETVEC
#endif
#else
#define CONFIG_RESET_VECTOR_ADDRESS	0xfffffffc
#endif
#define CONFIG_SYS_FSL_PBL_PBI board/varisys/cyrus/pbi.cfg
#if defined(CONFIG_ARCH_P5020)
#define CONFIG_SYS_CLK_FREQ 133000000
#define CONFIG_SYS_FSL_PBL_RCW board/varisys/cyrus/rcw_p5020_v2.cfg
#elif defined(CONFIG_ARCH_P5040)
#define CONFIG_SYS_CLK_FREQ 100000000
#define CONFIG_SYS_FSL_PBL_RCW board/varisys/cyrus/rcw_p5040.cfg
#endif

#ifdef CONFIG_SPL
/*
 * AmigaOne X5000 SPL: the block-8 PBL wraps only the SPL; the full U-Boot
 * is a separate image read from CONFIG_SYS_MMC_U_BOOT_OFFS.
 *
 * SD card layout: SPL PBL at 0x1000, AmigaOS4 NVRAM block
 * (nonvolatile.library) at 0x89200, U-Boot environment at 0x150000, FMan
 * microcode at 0x160000, the main U-Boot in a 2 MiB slot at 0x200000.
 * See docs/sd-card-layout.md.
 */
#define CONFIG_SPL_PAD_TO		0x40000
#define CONFIG_SPL_MAX_SIZE		0x28000
#define RESET_VECTOR_OFFSET		0x27FFC
#define BOOT_PAGE_OFFSET		0x27000
#ifdef CONFIG_SPL_BUILD
#define CONFIG_SPL_SKIP_RELOCATE
#define CONFIG_SPL_COMMON_INIT_DDR
#define CONFIG_SYS_CCSR_DO_NOT_RELOCATE
#endif
#define CONFIG_SYS_MMC_U_BOOT_SIZE	(2 << 20)
#define CONFIG_SYS_MMC_U_BOOT_DST	0x30000000
#define CONFIG_SYS_MMC_U_BOOT_START	0x30000000
#define CONFIG_SYS_MMC_U_BOOT_OFFS	0x200000
#endif /* CONFIG_SPL */

/* High Level Configuration Options */
#define CONFIG_SYS_BOOK3E_HV		/* Category E.HV supported */

#define CONFIG_SYS_MMC_MAX_DEVICE     1

#define CONFIG_SYS_FSL_CPC		/* Corenet Platform Cache */
#define CONFIG_SYS_NUM_CPC		CONFIG_SYS_NUM_DDR_CTLRS
#define CONFIG_PCIE1			/* PCIE controller 1 */
#define CONFIG_PCIE2			/* PCIE controller 2 */
#define CONFIG_FSL_PCI_INIT		/* Use common FSL init code */
#define CONFIG_SYS_PCI_64BIT		/* enable 64-bit PCI resources */

#if defined(CONFIG_SDCARD)
#define CONFIG_FSL_FIXED_MMC_LOCATION
#endif

/*
 * These can be toggled for performance analysis, otherwise use default.
 */
#define CONFIG_SYS_CACHE_STASHING
#define CONFIG_BACKSIDE_L2_CACHE
#define CONFIG_SYS_INIT_L2CSR0		L2CSR0_L2E
#define CONFIG_BTB			/* toggle branch predition */
#define	CONFIG_DDR_ECC
#ifdef CONFIG_DDR_ECC
#define CONFIG_ECC_INIT_VIA_DDRCONTROLLER
#define CONFIG_MEM_INIT_VALUE		0xdeadbeef
#endif

#define CONFIG_ENABLE_36BIT_PHYS

/* test POST memory test */
#undef CONFIG_POST

/*
 *  Config the L3 Cache as L3 SRAM
 */
#ifdef CONFIG_SPL_BUILD
/* AmigaOne X5000: the SPL itself runs from CPC L3-SRAM. */
#define CONFIG_SYS_INIT_L3_ADDR		0xFFFC0000
#define CONFIG_SYS_L3_SIZE		(256 << 10)
#elif defined(CONFIG_SPL)
/*
 * AmigaOne X5000 main U-Boot (loaded by the SPL, runs from DRAM).
 * CONFIG_SYS_INIT_L3_ADDR has two consumers that would want different
 * addresses here:
 *   1. The static "boot page" TLB1 entry in board/varisys/cyrus/tlb.c,
 *      which must cover the DRAM .bootpg (CONFIG_SYS_MMC_U_BOOT_DST).
 *      tlb.c uses that address directly for this build.
 *   2. disable_cpc_sram() in arch/powerpc/cpu/mpc85xx/cpu_init.c, called
 *      from cpu_init_r() because CONFIG_RAMBOOT_PBL is set. It calls
 *      find_law(CONFIG_SYS_INIT_L3_ADDR) to find and remove the CPC-as-SRAM
 *      LAW that board/varisys/cyrus/pbi.cfg sets up (1 MiB at physical
 *      0xFFF00000), so it needs that SRAM address.
 * If this macro held the DRAM address, find_law() would match the DDR LAW
 * instead and disable_cpc_sram() would remove DRAM's own window while
 * U-Boot runs from it; the boot stops silently after the "L2: ... enabled"
 * line. So the macro keeps the CPC-SRAM address.
 */
#define CONFIG_SYS_INIT_L3_ADDR		0xFFF00000
#define CONFIG_SYS_L3_SIZE		(1024 << 10)
#else
#define CONFIG_SYS_INIT_L3_ADDR		CONFIG_RAMBOOT_TEXT_BASE
#define CONFIG_SYS_L3_SIZE		(1024 << 10)
#endif
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_INIT_L3_ADDR_PHYS	(0xf00000000ull | CONFIG_SYS_INIT_L3_ADDR)
#else
#define CONFIG_SYS_INIT_L3_ADDR_PHYS	CONFIG_SYS_INIT_L3_ADDR
#endif
#define CONFIG_SYS_INIT_L3_END (CONFIG_SYS_INIT_L3_ADDR + CONFIG_SYS_L3_SIZE)
/* AmigaOne X5000: SPL global data, environment copy, malloc and stack. */
#define CONFIG_SPL_GD_ADDR		(CONFIG_SYS_INIT_L3_ADDR + 32 * 1024)
#define SPL_ENV_ADDR			(CONFIG_SPL_GD_ADDR + 4 * 1024)
#define CONFIG_SPL_RELOC_MALLOC_ADDR	(CONFIG_SPL_GD_ADDR + 12 * 1024)
#define CONFIG_SPL_RELOC_MALLOC_SIZE	(30 << 10)
#define CONFIG_SPL_RELOC_STACK		(CONFIG_SPL_GD_ADDR + 64 * 1024)

#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_DCSRBAR		0xf0000000
#define CONFIG_SYS_DCSRBAR_PHYS		0xf00000000ull
#endif

/*
 * DDR Setup
 */
#define CONFIG_VERY_BIG_RAM
#define CONFIG_SYS_DDR_SDRAM_BASE	0x00000000
#define CONFIG_SYS_SDRAM_BASE		CONFIG_SYS_DDR_SDRAM_BASE

#define CONFIG_DIMM_SLOTS_PER_CTLR	1
#define CONFIG_CHIP_SELECTS_PER_CTRL	(4 * CONFIG_DIMM_SLOTS_PER_CTLR)

#define CONFIG_DDR_SPD

#define CONFIG_SYS_SPD_BUS_NUM	1
#define SPD_EEPROM_ADDRESS1	0x51
#define SPD_EEPROM_ADDRESS2	0x52
#define CONFIG_SYS_SDRAM_SIZE	4096	/* for fixed parameter use */

/*
 * Local Bus Definitions
 */

#define CONFIG_SYS_LBC0_BASE		0xe0000000 /* Start of LBC Registers */
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_LBC0_BASE_PHYS	0xfe0000000ull
#else
#define CONFIG_SYS_LBC0_BASE_PHYS	CONFIG_SYS_LBC0_BASE
#endif

#define CONFIG_SYS_LBC1_BASE		0xe1000000 /* Start of LBC Registers */
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_LBC1_BASE_PHYS	0xfe1000000ull
#else
#define CONFIG_SYS_LBC1_BASE_PHYS	CONFIG_SYS_LBC1_BASE
#endif

/* Set the local bus clock 1/16 of platform clock */
#define CONFIG_SYS_LBC_LCRR		(LCRR_CLKDIV_16 | LCRR_EADC_1)

#define CONFIG_SYS_BR0_PRELIM \
(BR_PHYS_ADDR(CONFIG_SYS_LBC0_BASE_PHYS) | BR_PS_16 | BR_V)
#define CONFIG_SYS_BR1_PRELIM \
(BR_PHYS_ADDR(CONFIG_SYS_LBC1_BASE_PHYS) | BR_PS_16 | BR_V)

#define CONFIG_SYS_OR0_PRELIM	0xfff00010
#define CONFIG_SYS_OR1_PRELIM	0xfff00010

/* AmigaOne X5000: the SPL's own image starts at CONFIG_SPL_TEXT_BASE. */
#ifdef CONFIG_SPL_BUILD
#define CONFIG_SYS_MONITOR_BASE		CONFIG_SPL_TEXT_BASE	/* start of monitor */
#else
#define CONFIG_SYS_MONITOR_BASE		CONFIG_SYS_TEXT_BASE	/* start of monitor */
#endif

#if defined(CONFIG_RAMBOOT_PBL)
#define CONFIG_SYS_RAMBOOT
#endif

#define CONFIG_HWCONFIG

/* define to use L1 as initial stack */
#define CONFIG_L1_INIT_RAM
#define CONFIG_SYS_INIT_RAM_LOCK
#define CONFIG_SYS_INIT_RAM_ADDR	0xfdd00000	/* Initial L1 address */
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS_HIGH 0xf
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS_LOW CONFIG_SYS_INIT_RAM_ADDR
/* The assembler doesn't like typecast */
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS \
	((CONFIG_SYS_INIT_RAM_ADDR_PHYS_HIGH * 1ull << 32) | \
	  CONFIG_SYS_INIT_RAM_ADDR_PHYS_LOW)
#else
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS	CONFIG_SYS_INIT_RAM_ADDR /* Initial L1 address */
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS_HIGH 0
#define CONFIG_SYS_INIT_RAM_ADDR_PHYS_LOW CONFIG_SYS_INIT_RAM_ADDR_PHYS
#endif
#define CONFIG_SYS_INIT_RAM_SIZE		0x00004000	/* Size of used area in RAM */

#define CONFIG_SYS_GBL_DATA_OFFSET	(CONFIG_SYS_INIT_RAM_SIZE - GENERATED_GBL_DATA_SIZE)
#define CONFIG_SYS_INIT_SP_OFFSET	CONFIG_SYS_GBL_DATA_OFFSET

#if defined(CONFIG_SPL) && !defined(CONFIG_SPL_BUILD)
/*
 * AmigaOne X5000: the 2 MiB SD slot holds the 4 KiB boot page and then the
 * image.
 */
#define CONFIG_SYS_MONITOR_LEN		(CONFIG_SYS_MMC_U_BOOT_SIZE - 0x1000)
#else
#define CONFIG_SYS_MONITOR_LEN		(768 * 1024)
#endif
#define CONFIG_SYS_MALLOC_LEN		(1024 * 1024)	/* Reserved for malloc */

/* Serial Port - controlled on board with jumper J8
 * open - index 2
 * shorted - index 1
 */
#define CONFIG_SYS_NS16550_SERIAL
#define CONFIG_SYS_NS16550_REG_SIZE	1
#define CONFIG_SYS_NS16550_CLK		(get_bus_freq(0)/2)

#define CONFIG_SYS_BAUDRATE_TABLE	\
{300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200}

#define CONFIG_SYS_NS16550_COM1	(CONFIG_SYS_CCSRBAR+0x11C500)
#define CONFIG_SYS_NS16550_COM2	(CONFIG_SYS_CCSRBAR+0x11C600)
#define CONFIG_SYS_NS16550_COM3	(CONFIG_SYS_CCSRBAR+0x11D500)
#define CONFIG_SYS_NS16550_COM4	(CONFIG_SYS_CCSRBAR+0x11D600)

/* I2C */
#define CONFIG_SYS_I2C
#define CONFIG_SYS_I2C_FSL
#define CONFIG_I2C_MULTI_BUS
#define CONFIG_I2C_CMD_TREE
#define CONFIG_SYS_FSL_I2C_SPEED		400000	/* I2C speed and slave address */
#define CONFIG_SYS_FSL_I2C_SLAVE		0x7F
#define CONFIG_SYS_FSL_I2C_OFFSET		0x118000
#define CONFIG_SYS_FSL_I2C2_SPEED		400000	/* I2C speed and slave address */
#define CONFIG_SYS_FSL_I2C2_SLAVE		0x7F
#define CONFIG_SYS_FSL_I2C2_OFFSET		0x118100
#define CONFIG_SYS_FSL_I2C3_SPEED		400000	/* I2C speed and slave address */
#define CONFIG_SYS_FSL_I2C3_SLAVE		0x7F
#define CONFIG_SYS_FSL_I2C3_OFFSET		0x119000
#define CONFIG_SYS_FSL_I2C4_SPEED		400000	/* I2C speed and slave address */
#define CONFIG_SYS_FSL_I2C4_SLAVE		0x7F
#define CONFIG_SYS_FSL_I2C4_OFFSET		0x119100

#define CONFIG_ID_EEPROM
#define CONFIG_SYS_I2C_EEPROM_NXID
#define CONFIG_SYS_EEPROM_BUS_NUM	0
#define CONFIG_SYS_I2C_EEPROM_ADDR_LEN	1
#define CONFIG_SYS_I2C_EEPROM_ADDR	0x57

#define CONFIG_SYS_I2C_GENERIC_MAC
#define CONFIG_SYS_I2C_MAC1_BUS 3
#define CONFIG_SYS_I2C_MAC1_CHIP_ADDR 0x57
#define CONFIG_SYS_I2C_MAC1_DATA_ADDR 0xf2
#define CONFIG_SYS_I2C_MAC2_BUS 0
#define CONFIG_SYS_I2C_MAC2_CHIP_ADDR 0x50
#define CONFIG_SYS_I2C_MAC2_DATA_ADDR 0xfa

#define CONFIG_RTC_MCP79411		1
#define CONFIG_SYS_RTC_BUS_NUM		3
#define CONFIG_SYS_I2C_RTC_ADDR		0x6f

/*
 * eSPI - Enhanced SPI
 */

/*
 * General PCI
 * Memory space is mapped 1-1, but I/O space must start from 0.
 */

/*
 * AmigaOne X5000: do not allocate the P2P bridges' own BARs. The PCIe1
 * memory region is 512MB
 * and a Radeon's framebuffer BAR is 256MB with matching alignment, so it only
 * fits if it lands at the bottom of the region. A bridge BAR allocated first
 * pushes the allocator past 0xe0000000, the framebuffer BAR then has to skip to
 * 0xf0000000, and the card's register/doorbell/ROM BARs are left unassigned -
 * holding their sizing masks, which alias the framebuffer BAR's range. Cards
 * with onboard bridges (every Vega 10 has two) hit this; a directly attached
 * card such as the RX560 does not. See docs/graphics.md.
 */
#define CONFIG_PCIAUTO_SKIP_BRIDGE_BARS

/* controller 1, direct to uli, tgtid 3, Base address 20000 */
#define CONFIG_SYS_PCIE1_MEM_VIRT	0x80000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE1_MEM_BUS	0xe0000000
#define CONFIG_SYS_PCIE1_MEM_PHYS	0xc00000000ull
#else
#define CONFIG_SYS_PCIE1_MEM_BUS	0x80000000
#define CONFIG_SYS_PCIE1_MEM_PHYS	0x80000000
#endif
#define CONFIG_SYS_PCIE1_MEM_SIZE	0x20000000	/* 512M */
#define CONFIG_SYS_PCIE1_IO_VIRT	0xf8000000
#define CONFIG_SYS_PCIE1_IO_BUS		0x00000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE1_IO_PHYS	0xff8000000ull
#else
#define CONFIG_SYS_PCIE1_IO_PHYS	0xf8000000
#endif
#define CONFIG_SYS_PCIE1_IO_SIZE	0x00010000	/* 64k */

/* controller 2, Slot 2, tgtid 2, Base address 201000 */
#define CONFIG_SYS_PCIE2_MEM_VIRT	0xa0000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE2_MEM_BUS	0xe0000000
#define CONFIG_SYS_PCIE2_MEM_PHYS	0xc20000000ull
#else
#define CONFIG_SYS_PCIE2_MEM_BUS	0xa0000000
#define CONFIG_SYS_PCIE2_MEM_PHYS	0xa0000000
#endif
#define CONFIG_SYS_PCIE2_MEM_SIZE	0x20000000	/* 512M */
#define CONFIG_SYS_PCIE2_IO_VIRT	0xf8010000
#define CONFIG_SYS_PCIE2_IO_BUS		0x00000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE2_IO_PHYS	0xff8010000ull
#else
#define CONFIG_SYS_PCIE2_IO_PHYS	0xf8010000
#endif
#define CONFIG_SYS_PCIE2_IO_SIZE	0x00010000	/* 64k */

/* controller 3, Slot 1, tgtid 1, Base address 202000 */
#define CONFIG_SYS_PCIE3_MEM_VIRT	0xc0000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE3_MEM_BUS	0xe0000000
#define CONFIG_SYS_PCIE3_MEM_PHYS	0xc40000000ull
#else
#define CONFIG_SYS_PCIE3_MEM_BUS	0xc0000000
#define CONFIG_SYS_PCIE3_MEM_PHYS	0xc0000000
#endif
#define CONFIG_SYS_PCIE3_MEM_SIZE	0x20000000	/* 512M */
#define CONFIG_SYS_PCIE3_IO_VIRT	0xf8020000
#define CONFIG_SYS_PCIE3_IO_BUS		0x00000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_PCIE3_IO_PHYS	0xff8020000ull
#else
#define CONFIG_SYS_PCIE3_IO_PHYS	0xf8020000
#endif
#define CONFIG_SYS_PCIE3_IO_SIZE	0x00010000	/* 64k */

/* controller 4, Base address 203000 */
#define CONFIG_SYS_PCIE4_MEM_BUS	0xe0000000
#define CONFIG_SYS_PCIE4_MEM_PHYS	0xc60000000ull
#define CONFIG_SYS_PCIE4_MEM_SIZE	0x20000000	/* 512M */
#define CONFIG_SYS_PCIE4_IO_BUS		0x00000000
#define CONFIG_SYS_PCIE4_IO_PHYS	0xff8030000ull
#define CONFIG_SYS_PCIE4_IO_SIZE	0x00010000	/* 64k */

/* Qman/Bman */
#define CONFIG_SYS_BMAN_NUM_PORTALS	10
#define CONFIG_SYS_BMAN_MEM_BASE	0xf4000000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_BMAN_MEM_PHYS	0xff4000000ull
#else
#define CONFIG_SYS_BMAN_MEM_PHYS	CONFIG_SYS_BMAN_MEM_BASE
#endif
#define CONFIG_SYS_BMAN_MEM_SIZE	0x00200000
#define CONFIG_SYS_BMAN_SP_CENA_SIZE    0x4000
#define CONFIG_SYS_BMAN_SP_CINH_SIZE    0x1000
#define CONFIG_SYS_BMAN_CENA_BASE       CONFIG_SYS_BMAN_MEM_BASE
#define CONFIG_SYS_BMAN_CENA_SIZE       (CONFIG_SYS_BMAN_MEM_SIZE >> 1)
#define CONFIG_SYS_BMAN_CINH_BASE       (CONFIG_SYS_BMAN_MEM_BASE + \
					 CONFIG_SYS_BMAN_CENA_SIZE)
#define CONFIG_SYS_BMAN_CINH_SIZE       (CONFIG_SYS_BMAN_MEM_SIZE >> 1)
#define CONFIG_SYS_BMAN_SWP_ISDR_REG   0xE08
#define CONFIG_SYS_QMAN_NUM_PORTALS	10
#define CONFIG_SYS_QMAN_MEM_BASE	0xf4200000
#ifdef CONFIG_PHYS_64BIT
#define CONFIG_SYS_QMAN_MEM_PHYS	0xff4200000ull
#else
#define CONFIG_SYS_QMAN_MEM_PHYS	CONFIG_SYS_QMAN_MEM_BASE
#endif
#define CONFIG_SYS_QMAN_MEM_SIZE	0x00200000
#define CONFIG_SYS_QMAN_SP_CENA_SIZE    0x4000
#define CONFIG_SYS_QMAN_SP_CINH_SIZE    0x1000
#define CONFIG_SYS_QMAN_CENA_BASE       CONFIG_SYS_QMAN_MEM_BASE
#define CONFIG_SYS_QMAN_CENA_SIZE       (CONFIG_SYS_QMAN_MEM_SIZE >> 1)
#define CONFIG_SYS_QMAN_CINH_BASE       (CONFIG_SYS_QMAN_MEM_BASE + \
					  CONFIG_SYS_QMAN_CENA_SIZE)
#define CONFIG_SYS_QMAN_CINH_SIZE       (CONFIG_SYS_QMAN_MEM_SIZE >> 1)
#define CONFIG_SYS_QMAN_SWP_ISDR_REG   0xE08

#define CONFIG_SYS_DPAA_FMAN
/* Default address of microcode for the Linux Fman driver */
/*
 * PBL SD boot image should stored at 0x1000(8 blocks), the size of the image is
 * about 825KB (1650 blocks), Env is stored after the image, and the env size is
 * 0x2000 (16 blocks), 8 + 1650 + 16 = 1674, enlarge it to 1680.
 */
#ifdef CONFIG_SPL
/*
 * AmigaOne X5000: FMan microcode at SD 0x160000, between the U-Boot
 * environment (0x150000) and the main U-Boot slot (0x200000). See
 * docs/sd-card-layout.md.
 */
#define CONFIG_SYS_FMAN_FW_ADDR	0x160000
#else
#define CONFIG_SYS_FMAN_FW_ADDR	(512 * 1680)
#endif

#define CONFIG_SYS_QE_FMAN_FW_LENGTH	0x10000
#define CONFIG_SYS_FDT_PAD		(0x3000 + CONFIG_SYS_QE_FMAN_FW_LENGTH)

#ifdef CONFIG_PCI
#define CONFIG_PCI_INDIRECT_BRIDGE

#define CONFIG_PCI_SCAN_SHOW		/* show pci devices on startup */
#endif	/* CONFIG_PCI */

/*
 * AmigaOne X5000 video: POST the Radeon VBIOS in the x86 emulator
 * (CONFIG_BIOSEMU) to bring up a framebuffer for U-Boot's VGA console and
 * the boota boot menu. See docs/graphics.md.
 * VIDEO_IO_OFFSET is referenced unconditionally by the emulator (biosemui.h) and
 * must point emulated x86 I/O ports at the card's PCIe I/O window; the X5000's
 * Radeon is on PCIe1. CONFIG_FRAMEBUFFER_SET_VESA_MODE enables the VBE mode set.
 */
#ifdef CONFIG_BIOSEMU
#define CONFIG_FRAMEBUFFER_SET_VESA_MODE
#define VIDEO_IO_OFFSET		CONFIG_SYS_PCIE1_IO_VIRT
#endif

/* SATA */
#ifdef CONFIG_FSL_SATA_V2
#define CONFIG_SYS_SATA_MAX_DEVICE	2
#define CONFIG_SATA1
#define CONFIG_SYS_SATA1		CONFIG_SYS_MPC85xx_SATA1_ADDR
#define CONFIG_SYS_SATA1_FLAGS		FLAGS_DMA
#define CONFIG_SATA2
#define CONFIG_SYS_SATA2		CONFIG_SYS_MPC85xx_SATA2_ADDR
#define CONFIG_SYS_SATA2_FLAGS		FLAGS_DMA

#define CONFIG_LBA48
#endif

#ifdef CONFIG_FMAN_ENET
#define CONFIG_SYS_TBIPA_VALUE	8
/*
 * AmigaOne X5000: the first port of each model in board/varisys/cyrus/eth.c
 * (FIRST_PORT).
 */
#ifdef CONFIG_ARCH_P5040
#define CONFIG_ETHPRIME		"FM1@DTSEC5"
#else
#define CONFIG_ETHPRIME		"FM1@DTSEC4"
#endif
#endif

/*
 * Environment
 */
#define CONFIG_LOADS_ECHO		/* echo on for serial download */
#define CONFIG_SYS_LOADS_BAUD_CHANGE	/* allow baudrate change */

/*
 * USB
 */
#define CONFIG_HAS_FSL_DR_USB
#define CONFIG_HAS_FSL_MPH_USB

#if defined(CONFIG_HAS_FSL_DR_USB) || defined(CONFIG_HAS_FSL_MPH_USB)
/* AmigaOne X5000: CONFIG_USB_EHCI_FSL comes from Kconfig (set in the defconfigs) */
#define CONFIG_EHCI_HCD_INIT_AFTER_RESET
#define CONFIG_EHCI_IS_TDI
 /* _VIA_CONTROL_EP  */
#endif

#ifdef CONFIG_MMC
#define CONFIG_SYS_FSL_ESDHC_ADDR       CONFIG_SYS_MPC85xx_ESDHC_ADDR
#define CONFIG_SYS_FSL_ESDHC_BROKEN_TIMEOUT
#endif

/*
 * Miscellaneous configurable options
 */
#define CONFIG_SYS_LOAD_ADDR	0x2000000	/* default load address */

/*
 * For booting Linux, the board info and command line data
 * have to be in the first 64 MB of memory, since this is
 * the maximum mapped by the Linux kernel during initialization.
 */
#define CONFIG_SYS_BOOTMAPSZ	(64 << 20)	/* Initial Memory map for Linux*/
#define CONFIG_SYS_BOOTM_LEN	(64 << 20)	/* Increase max gunzip size */

#ifdef CONFIG_CMD_KGDB
#define CONFIG_KGDB_BAUDRATE	230400	/* speed to run kgdb serial port */
#endif

/*
 * Environment Configuration
 */
#define CONFIG_ROOTPATH		"/opt/nfsroot"
#define CONFIG_BOOTFILE		"uImage"
#define CONFIG_UBOOTPATH	u-boot.bin	/* U-Boot image on TFTP server */

/* default location for tftp and bootm */
#define CONFIG_LOADADDR		1000000

#define __USB_PHY_TYPE	utmi

/*
 * AmigaOne X5000 additions to the default environment:
 *  - hwconfig "no_erratum_a005275": the EHCI driver does not apply the
 *    erratum A-005275 workaround (forcing full speed), so USB devices run
 *    at high speed.
 *  - stdin/stdout/stderr: serial console plus a USB keyboard; add "vga" to
 *    stdout to show the U-Boot console on the Radeon screen.
 *  - amigaboot_quiet, os4_bootdevice, os4_commandline: read by the boota
 *    command (see docs/booting-amigaos.md). The name amigaboot_quiet is
 *    kept so that environments saved by earlier firmware still work.
 *    A debug build shows the boot menu and passes debuglevel=1 to the
 *    kernel; a production build (CONFIG_BOOTA_PRODUCTION) boots the
 *    default entry without the menu, with debuglevel=0.
 */
#ifdef CONFIG_BOOTA_PRODUCTION
#define AOS4_ENV_QUIET		"y"
#define AOS4_ENV_CMDLINE	"serial munge debuglevel=0"
#else
#define AOS4_ENV_QUIET		"n"
#define AOS4_ENV_CMDLINE	"serial munge debuglevel=1"
#endif

#define	CONFIG_EXTRA_ENV_SETTINGS \
"hwconfig=fsl_ddr:ctlr_intlv=cacheline,"		\
"bank_intlv=cs0_cs1;"					\
"usb1:dr_mode=host,phy_type=" __stringify(__USB_PHY_TYPE) ";"\
"usb2:dr_mode=host,phy_type=" __stringify(__USB_PHY_TYPE) ";"\
"no_erratum_a005275\0"\
"netdev=eth0\0"						\
"uboot=" __stringify(CONFIG_UBOOTPATH) "\0"			\
"ubootaddr=" __stringify(CONFIG_SYS_TEXT_BASE) "\0"			\
"consoledev=ttyS0\0"					\
"ramdiskaddr=2000000\0"					\
"fdtaddr=1e00000\0"					\
"bdev=sda3\0"						\
"stdin=serial,usbkbd\0"					\
"stdout=serial\0"					\
"stderr=serial\0"					\
"amigaboot_quiet=" AOS4_ENV_QUIET "\0"			\
"os4_bootdevice=auto\0"					\
"os4_commandline=" AOS4_ENV_CMDLINE "\0"

#define CONFIG_HDBOOT					\
"setenv bootargs root=/dev/$bdev rw "		\
"console=$consoledev,$baudrate $othbootargs;"	\
"tftp $loadaddr $bootfile;"			\
"tftp $fdtaddr $fdtfile;"			\
"bootm $loadaddr - $fdtaddr"

#define CONFIG_NFSBOOTCOMMAND			\
"setenv bootargs root=/dev/nfs rw "	\
"nfsroot=$serverip:$rootpath "		\
"ip=$ipaddr:$serverip:$gatewayip:$netmask:$hostname:$netdev:off " \
"console=$consoledev,$baudrate $othbootargs;"	\
"tftp $loadaddr $bootfile;"		\
"tftp $fdtaddr $fdtfile;"		\
"bootm $loadaddr - $fdtaddr"

#define CONFIG_RAMBOOTCOMMAND				\
"setenv bootargs root=/dev/ram rw "		\
"console=$consoledev,$baudrate $othbootargs;"	\
"tftp $ramdiskaddr $ramdiskfile;"		\
"tftp $loadaddr $bootfile;"			\
"tftp $fdtaddr $fdtfile;"			\
"bootm $loadaddr $ramdiskaddr $fdtaddr"

/*
 * AmigaOne X5000: the default autoboot action starts AmigaOS 4 with boota
 * (in place of upstream's CONFIG_HDBOOT). This is only the default value
 * of "bootcmd"; change it with "setenv bootcmd ...".
 */
#define CONFIG_BOOTCOMMAND		"boota"

#include <asm/fsl_secure_boot.h>

#endif	/* __CONFIG_H */
