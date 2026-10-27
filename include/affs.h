/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * affs.h: Amiga FastFileSystem / OldFileSystem (FFS/OFS, "affs"),
 * read-only, on-disk constants and U-Boot fs op prototypes.
 *
 * Written for U-Boot's generic filesystem framework (fstype/ls/load/size),
 * used by the AmigaOne X5000 and A1222 ports.  The on-disk layout was taken
 * from the public AmigaDOS documentation and cross-checked against the Linux
 * kernel affs driver (fs/affs/, GPL-2.0) and the amitools implementation,
 * then checked against a real FFS image.
 *
 * All multi-byte on-disk fields are stored big-endian 32-bit longs.  They are
 * read through get_unaligned_be32()/be16() so the driver does not depend on
 * host endianness (on the big-endian PowerPC hosts the conversion is a
 * no-op).
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#ifndef _AFFS_H_
#define _AFFS_H_

#include <linux/types.h>

struct blk_desc;
struct disk_partition;
struct fs_dir_stream;
struct fs_dirent;

/*
 * Boot-block DOS type: 'D','O','S', flag.  Bit0 of flag selects FFS (raw data
 * blocks) over OFS (24-byte data-block header); flag>=2 selects the
 * "international" upper-casing used for hashing; flag 6/7 selects long names.
 */
#define AFFS_DOSTYPE_PREFIX	0x444f5300	/* 'D''O''S''\0' (mask low byte) */
#define AFFS_DOS_OFS		0		/* DOS\0  OldFileSystem            */
#define AFFS_DOS_FFS		1		/* DOS\1  FastFileSystem           */
#define AFFS_DOS_OFS_INTL	2		/* DOS\2  OFS, international        */
#define AFFS_DOS_FFS_INTL	3		/* DOS\3  FFS, international        */
#define AFFS_DOS_OFS_DC		4		/* DOS\4  OFS, dir cache           */
#define AFFS_DOS_FFS_DC		5		/* DOS\5  FFS, dir cache           */
#define AFFS_DOS_OFS_LN		6		/* DOS\6  OFS, long names          */
#define AFFS_DOS_FFS_LN		7		/* DOS\7  FFS, long names (AOS4)   */

#define AFFS_FLAG_FFS		1		/* bit0: FFS (else OFS)            */

/* Primary block type (offset 0). */
#define AFFS_T_HEADER		2		/* root / dir / file header        */
#define AFFS_T_LIST		16		/* file extension block            */
#define AFFS_T_DATA		8		/* OFS data block                  */

/* Secondary block type (offset BSIZE-4). */
#define AFFS_ST_ROOT		1
#define AFFS_ST_USERDIR		2
#define AFFS_ST_SOFTLINK	3
#define AFFS_ST_LINKDIR		4
#define AFFS_ST_FILE		(-3)
#define AFFS_ST_LINKFILE	(-4)

/*
 * Field byte-offsets within a BSIZE block.  The header fields are absolute
 * from the start of the block; the trailer fields are expressed relative to
 * the block size, which is how AmigaDOS lays them out (they are written from
 * the end of the block, so they are independent of BSIZE).
 */
#define AFFS_OFF_TYPE		0		/* primary type                    */
#define AFFS_OFF_OWNKEY		4		/* own block number                */
#define AFFS_OFF_HIGHSEQ	8		/* #data-block ptrs used in block  */
#define AFFS_OFF_HTSIZE		12		/* hashtable size (dirs/root)      */
#define AFFS_OFF_FIRSTDATA	16		/* first data block (file header)  */
#define AFFS_OFF_HASHTABLE	24		/* hashtable / start of dir hash   */

#define AFFS_OFFR_DATATAB	204		/* BSIZE-204: data table[0] (top)  */
#define AFFS_OFFR_BYTESIZE	188		/* BSIZE-188: file size in bytes   */
#define AFFS_OFFR_NAME_LN	184		/* BSIZE-184: long-name BSTR field */
#define AFFS_OFFR_NAME		80		/* BSIZE-80:  short-name BSTR      */
#define AFFS_OFFR_HASHCHAIN	16		/* BSIZE-16:  next entry same hash */
#define AFFS_OFFR_PARENT	12		/* BSIZE-12:  parent header block  */
#define AFFS_OFFR_EXTENSION	8		/* BSIZE-8:   next extension block */
#define AFFS_OFFR_SECTYPE	4		/* BSIZE-4:   secondary type       */

#define AFFS_NAME_LN_FIELD	112		/* size of the long-name BSTR area */
#define AFFS_NAME_MAX_SHORT	30		/* short-name max chars            */
#define AFFS_NAME_MAX_LONG	110		/* long-name max chars             */

/* Per-block administrative longs not available for hash/data pointers. */
#define AFFS_RESERVED_LONGS	56		/* ht/data entries = BSIZE/4 - 56  */

/* OFS data-block header length stripped from each payload. */
#define AFFS_OFS_DATA_HDR	24

/* U-Boot generic filesystem operations (mirrors include/sfs.h). */
int affs_probe(struct blk_desc *fs_dev_desc,
	       struct disk_partition *fs_partition);
int affs_opendir(const char *filename, struct fs_dir_stream **dirsp);
int affs_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
void affs_closedir(struct fs_dir_stream *dirs);
int affs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	      loff_t *actread);
int affs_size(const char *filename, loff_t *size);
int affs_exists(const char *filename);
void affs_close(void);

#endif /* _AFFS_H_ */
