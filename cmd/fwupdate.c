// SPDX-License-Identifier: GPL-2.0+
/*
 * fwupdate - update the AmigaOne X5000's firmware SD card from a file on an
 * AmigaOS volume or another disk. X5000 only: the offsets and checks are
 * the X5000's SD card layout. See docs/booting-amigaos.md in the X5000
 * port.
 *
 *   fwupdate [-d] DH3:path/file [crc32]
 *   fwupdate [-d] interface dev[:part] file [crc32]
 *
 * The file is either a main U-Boot (u-boot.bin, or the 2 MiB U-Boot slot
 * of an SD image), written to the U-Boot slot, or a complete SD card image,
 * written from block 0. A full image keeps the card's saved U-Boot
 * environment and the AmigaOS NVRAM block, so per-board settings survive,
 * unless -d asks for the image's settings instead.
 *
 * Nothing is written before the file has been checked and the user has
 * confirmed. The old contents are kept in RAM, the new ones are read back
 * and compared, and on a mismatch the old contents are written back.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <blk.h>
#include <command.h>
#include <console.h>
#include <fs.h>
#include <mmc.h>
#include <part.h>
#include <version.h>
#include <linux/ctype.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <u-boot/crc.h>

#define SD_BLK		512
#define SLOT_OFFS	CONFIG_SYS_MMC_U_BOOT_OFFS	/* main U-Boot */
#define SLOT_SIZE	CONFIG_SYS_MMC_U_BOOT_SIZE
#define PBL_OFFS	0x1000				/* RCW + SPL */
#define PBL_MAGIC	0xaa55aa55
#define RCW_LEN		64
#define OS_NVRAM_OFFS	0x89200		/* read by AmigaOS nonvolatile.library */
#define OS_NVRAM_SIZE	0x2000
#define IMAGE_MAX	SZ_64M
#define OLD_SLOT_OFFS	0x8C000		/* U-Boot slot of older releases */
#define OLD_SLOT_SIZE	(768 << 10)

/* scratch RAM: the new file, the card's old contents, the read-back */
#define NEW_ADDR	0x10000000UL
#define OLD_ADDR	0x04000000UL
#define CHECK_ADDR	0x14000000UL

struct region {
	ulong offs;
	ulong size;
};

/* Find RDB partition 'name' (e.g. DH3) on the SATA or USB disks. */
static struct blk_desc *find_drive(const char *name, int *part_out)
{
	static const char * const ifaces[] = { "sata", "usb" };
	int i, dev, part;

	for (i = 0; i < ARRAY_SIZE(ifaces); i++) {
		for (dev = 0; dev < 8; dev++) {
			struct blk_desc *desc = blk_get_dev(ifaces[i], dev);

			if (!desc)
				break;
			if (desc->type == DEV_TYPE_UNKNOWN ||
			    desc->part_type != PART_TYPE_AMIGA)
				continue;
			for (part = 1; part <= AMIGA_ENTRY_NUMBERS; part++) {
				struct disk_partition info;

				if (part_get_info(desc, part, &info))
					break;
				if (!strcasecmp((char *)info.name, name)) {
					*part_out = part;
					return desc;
				}
			}
		}
	}
	return NULL;
}

/* The "U-Boot 20..." version string inside an image, or NULL. */
static const char *find_version(const u8 *p, ulong len)
{
	ulong i;

	for (i = 0; i + 16 < len; i++)
		if (p[i] == 'U' && !memcmp(p + i, "U-Boot 20", 9))
			return (const char *)p + i;
	return NULL;
}

static void print_version(const char *what, const char *v)
{
	int n = 0;

	while (v && n < 72 && isprint((unsigned char)v[n]))
		n++;
	printf("  %-9s %.*s\n", what, n, v ? v : "(none found)");
}

static int sd_rw(struct blk_desc *sd, bool write, ulong offs, ulong size,
		 ulong addr)
{
	lbaint_t blk = offs / SD_BLK, cnt = DIV_ROUND_UP(size, SD_BLK);
	ulong n;

	n = write ? blk_dwrite(sd, blk, cnt, (void *)addr) :
		    blk_dread(sd, blk, cnt, (void *)addr);
	return n == cnt ? 0 : -EIO;
}

/* Write regions of 'src' and check them by reading them back. */
static int write_checked(struct blk_desc *sd, const struct region *r, int n,
			 ulong src)
{
	int i;

	for (i = 0; i < n; i++) {
		if (sd_rw(sd, true, r[i].offs, r[i].size, src + r[i].offs) ||
		    sd_rw(sd, false, r[i].offs, r[i].size,
			  CHECK_ADDR + r[i].offs) ||
		    memcmp((void *)(src + r[i].offs),
			   (void *)(CHECK_ADDR + r[i].offs), r[i].size))
			return -EIO;
	}
	return 0;
}

static bool confirm(void)
{
	int c;

	printf("Are you sure you want to update the firmware? (y/N) ");
	c = getchar();
	printf("%c\n", isprint(c) ? c : ' ');
	return c == 'y' || c == 'Y';
}

static void wait_and_reboot(void)
{
	printf("\nPress any key to reboot.\n");
	getchar();
	do_reset(NULL, 0, 0, NULL);
}

static int do_fwupdate(struct cmd_tbl *cmdtp, int flag, int argc,
		       char *const argv[])
{
	struct region regions[3];
	struct blk_desc *disk, *sd;
	struct mmc *mmc;
	char drive[32], path[256], where[300];
	const char *colon, *ver;
	loff_t size, got;
	ulong crc, want = 0;
	bool full, defaults = false;
	int part, nreg, i;

	/* -d: write the image's settings too, instead of keeping the card's */
	if (argc > 1 && !strcmp(argv[1], "-d")) {
		defaults = true;
		argc--;
		argv++;
	}
	/* 1. find and check the file; nothing is written yet */
	run_command("sata init", 0);
	if (argc >= 2 && argc <= 3 && (colon = strchr(argv[1], ':')) &&
	    colon != argv[1] && colon - argv[1] < sizeof(drive)) {
		/* DRIVE:file, an AmigaOS partition by its RDB name */
		strlcpy(drive, argv[1], colon - argv[1] + 1);
		snprintf(path, sizeof(path), "/%s", colon + 1);
		disk = find_drive(drive, &part);
		if (!disk) {
			printf("No partition named %s on the SATA or USB disks.\n",
			       drive);
			return CMD_RET_FAILURE;
		}
		snprintf(where, sizeof(where), "%s:%s", drive, colon + 1);
	} else if (argc >= 4 && argc <= 5) {
		/* interface dev[:part] file, e.g. a USB stick: usb 0:1 */
		struct disk_partition info;

		if (!strcmp(argv[1], "usb"))
			run_command("usb start", 0);
		part = blk_get_device_part_str(argv[1], argv[2], &disk, &info,
					       1);
		if (part < 0)
			return CMD_RET_FAILURE;
		snprintf(path, sizeof(path), "/%s",
			 argv[3] + (argv[3][0] == '/'));
		snprintf(where, sizeof(where), "%s %s %s", argv[1], argv[2],
			 path);
		argc -= 2;
		argv += 2;
	} else {
		return CMD_RET_USAGE;
	}
	if (argc == 3)
		want = simple_strtoul(argv[2], NULL, 16);

	if (fs_set_blk_dev_with_part(disk, part) || fs_size(path, &size)) {
		printf("%s: file not found.\n", where);
		return CMD_RET_FAILURE;
	}
	if (!size || size > IMAGE_MAX) {
		printf("%s: %lld bytes is not a firmware file.\n", where, size);
		return CMD_RET_FAILURE;
	}
	memset((void *)NEW_ADDR, 0, IMAGE_MAX);
	if (fs_set_blk_dev_with_part(disk, part) ||
	    fs_read(path, NEW_ADDR, 0, size, &got) || got != size) {
		printf("%s: read error.\n", where);
		return CMD_RET_FAILURE;
	}
	crc = crc32(0, (void *)NEW_ADDR, size);

	mmc = find_mmc_device(0);
	if (!mmc || mmc_init(mmc)) {
		printf("The firmware SD card is not available.\n");
		return CMD_RET_FAILURE;
	}
	sd = mmc_get_blk_desc(mmc);

	full = size > SLOT_SIZE;
	if (full) {
		u32 magic = *(u32 *)(NEW_ADDR + PBL_OFFS);

		/*
		 * An image's own SPL knows where its U-Boot is: this layout's
		 * slot, or the 768 KiB one at 0x8C000 used by older
		 * releases.
		 */
		ver = NULL;
		if (size > CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE)
			ver = find_version((u8 *)NEW_ADDR + OLD_SLOT_OFFS,
					   OLD_SLOT_SIZE);
		if (!ver && size >= SLOT_OFFS + SLOT_SIZE)
			ver = find_version((u8 *)NEW_ADDR + SLOT_OFFS,
					   SLOT_SIZE);
		if (magic != PBL_MAGIC || !ver) {
			printf("%s is not an X5000 SD card image.\n", where);
			return CMD_RET_FAILURE;
		}
		nreg = 0;
		if (defaults) {
			/* the whole image, settings included */
			regions[nreg++] = (struct region){
				0, ALIGN(size, SD_BLK) };
		} else {
			/* all of it but the card's two settings blocks */
			regions[nreg++] = (struct region){ 0, OS_NVRAM_OFFS };
			regions[nreg++] = (struct region){
				OS_NVRAM_OFFS + OS_NVRAM_SIZE,
				CONFIG_ENV_OFFSET -
				(OS_NVRAM_OFFS + OS_NVRAM_SIZE) };
			regions[nreg++] = (struct region){
				CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE,
				ALIGN(size, SD_BLK) - (CONFIG_ENV_OFFSET +
						       CONFIG_ENV_SIZE) };
		}
	} else {
		ver = find_version((u8 *)NEW_ADDR, size);
		if (!ver) {
			printf("%s is not a U-Boot image.\n", where);
			return CMD_RET_FAILURE;
		}
		nreg = 1;
		regions[0] = (struct region){ SLOT_OFFS, SLOT_SIZE };
		/* the slot is written whole: the new image, zero-padded */
		memmove((void *)(NEW_ADDR + SLOT_OFFS), (void *)NEW_ADDR,
			SLOT_SIZE);
		memset((void *)NEW_ADDR, 0, SLOT_OFFS);
		ver += SLOT_OFFS;	/* it moved with the image */
	}

	/* 2. what will happen, the warnings, and the question */
	printf("\nFirmware update from %s\n", where);
	printf("  file      %lld bytes, CRC32 %08lx\n", size, crc);
	print_version("current", version_string);
	print_version("new", ver);
	if (full && defaults)
		printf("  writes    the whole firmware SD card image, settings included\n");
	else if (full)
		printf("  writes    the whole firmware SD card image (keeps the saved\n"
		       "            U-Boot settings and the AmigaOS NVRAM block)\n");
	else
		printf("  writes    the U-Boot slot of the firmware SD card\n"
		       "            (SD %#x, %u KiB)\n", SLOT_OFFS, SLOT_SIZE >> 10);
	if (defaults && !full)
		printf("  (-d has no effect: a U-Boot image holds no settings)\n");

	if (argc == 3 && crc != want) {
		printf("\nThe file's CRC32 is %08lx, not %08lx as given: not updating.\n",
		       crc, want);
		return CMD_RET_FAILURE;
	}
	if (full) {
		/* read the card's RCW to compare the board model */
		if (sd_rw(sd, false, PBL_OFFS, SD_BLK, OLD_ADDR)) {
			printf("Cannot read the firmware SD card.\n");
			return CMD_RET_FAILURE;
		}
		if (memcmp((void *)OLD_ADDR, (void *)(NEW_ADDR + PBL_OFFS),
			   RCW_LEN))
			puts("\nWARNING: this image's reset configuration differs from the\n"
			       "card's. It may be built for the other X5000 model (/20 or /40)\n"
			       "and then this machine will not start from the card.\n");
	} else if (sd_rw(sd, false, SLOT_OFFS, SLOT_SIZE, OLD_ADDR) ||
		   !find_version((u8 *)OLD_ADDR, SLOT_SIZE)) {
		/*
		 * The card's SPL must be one that loads U-Boot from this
		 * slot; a card with the older layout loads it elsewhere.
		 */
		puts("\nThe card holds no U-Boot in this firmware's U-Boot slot, so its\n"
		     "firmware uses an older SD layout. Update it with a whole SD card\n"
		     "image first; U-Boot images can be used after that.\n");
		return CMD_RET_FAILURE;
	}

	/* puts: printf() truncates at CONFIG_SYS_PBSIZE */
	puts("\n*** Before you go on ***\n"
	       "- Have a working backup firmware SD card at hand. If the update\n"
	       "  fails, the machine can only be started again from a good card.\n"
	       "- Do not turn the power off or reset the machine while the update\n"
	       "  runs. It takes a few seconds; wait for the 'press any key' line.\n");
	if (full && defaults)
		puts("- -d replaces this card's saved settings (U-Boot environment,\n"
		       "  e.g. DDR tuning and boot options, and the AmigaOS NVRAM block)\n"
		       "  with the image's defaults.\n");
	printf("\n");
	if (!confirm()) {
		printf("Update cancelled; nothing was written.\n");
		return CMD_RET_SUCCESS;
	}

	/* 3. keep the old contents, write, read back, compare */
	disable_ctrlc(1);
	printf("\nUpdating. DO NOT TURN THE POWER OFF.\n");
	for (i = 0; i < nreg; i++) {
		if (sd_rw(sd, false, regions[i].offs, regions[i].size,
			  OLD_ADDR + regions[i].offs)) {
			disable_ctrlc(0);
			printf("Cannot read the firmware SD card; nothing was written.\n");
			return CMD_RET_FAILURE;
		}
	}
	printf("  saved the current firmware in memory\n");

	if (!write_checked(sd, regions, nreg, NEW_ADDR)) {
		disable_ctrlc(0);
		printf("  written and verified\n\n"
		       "Firmware update complete. The new firmware runs after the reboot.\n");
		wait_and_reboot();
		return CMD_RET_SUCCESS;
	}

	printf("  VERIFY FAILED: the card does not hold what was written.\n"
	       "  Writing the previous firmware back...\n");
	if (!write_checked(sd, regions, nreg, OLD_ADDR)) {
		disable_ctrlc(0);
		printf("  previous firmware restored and verified.\n\n"
		       "The update failed, but the card holds the firmware it had before.\n");
		wait_and_reboot();
		return CMD_RET_FAILURE;
	}

	disable_ctrlc(0);
	puts("\n*** THE FIRMWARE SD CARD COULD NOT BE RESTORED ***\n"
	       "Do NOT turn the power off or reboot: the card will probably not\n"
	       "start the machine. Try 'fwupdate' again from here, or prepare\n"
	       "your backup SD card before switching off.\n");
	return CMD_RET_FAILURE;
}

U_BOOT_CMD(
	fwupdate, 6, 0, do_fwupdate,
	"update the firmware SD card from a file on a disk or USB stick",
	"[-d] DRIVE:file [crc32]\n"
	"fwupdate [-d] interface dev[:part] file [crc32]\n"
	"    - DRIVE is the RDB drive name of an AmigaOS partition (e.g. DH3);\n"
	"      otherwise name the device, e.g. 'usb 0:1' for a FAT or exFAT\n"
	"      USB stick (plug it in before switching on).\n"
	"      'file' is a U-Boot image (u-boot.bin or a 2 MiB U-Boot slot),\n"
	"      written to the card's U-Boot slot, or a whole SD card image.\n"
	"      Give crc32 (hex) to refuse a file whose CRC32 differs.\n"
	"      A whole image keeps the card's settings (U-Boot environment,\n"
	"      AmigaOS NVRAM block); -d writes the image's settings instead.\n"
	"      Asks before writing; verifies, and restores the old firmware\n"
	"      if the verify fails, then waits for a key and reboots.\n"
	"  Examples:\n"
	"    fwupdate DH3:x5000-20.img 11a19204\n"
	"    fwupdate usb 0:1 u-boot.bin\n"
	"    fwupdate -d usb 0:1 x5000-20.img"
);
