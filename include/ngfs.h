/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * ngfs.h: Amiga "New Generation File System" (NGFS, AmigaOS 4), read-only,
 * on-disk constants and U-Boot fs op prototypes.
 *
 * Used by the AmigaOne X5000 and A1222 ports through U-Boot's generic
 * filesystem framework (fstype/ls/load/size).
 *
 * NGFS and SFS are unrelated filesystems, and this driver is not derived from
 * fs/sfs/.  It is written from the on-disk format description in
 * docs/ngfs-format.md and checked against real NGFS volumes.  Only the generic
 * fstype_info plumbing (fs_devread() block I/O and the fstypes[] entry shape)
 * follows the sibling drivers.
 *
 * NGFS uses 64-bit addressing throughout: every block, sector and extent disk
 * address is a u64.  All multi-byte on-disk fields are big-endian and are read
 * through get_unaligned_be64()/be32()/be16(), so the driver does not depend on
 * host endianness (on the big-endian PowerPC hosts the conversion is a no-op).
 *
 * Two on-disk variants are supported, selected by the version byte at
 * signature block +0x03:
 *   - NGF\1 (v1): directories are packed node-record streams read through an
 *     object's extent map.  This is the variant that has been tested.
 *   - NGF\0 (v0): type-2/13 container blocks of u64 child addresses.  Not yet
 *     tested on a real volume; implemented behind the version check.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#ifndef _NGFS_H_
#define _NGFS_H_

#include <linux/types.h>

struct blk_desc;
struct disk_partition;
struct fs_dir_stream;
struct fs_dirent;

/* -------------------------------------------------------------------------
 * Probe + signature block (NGFS block 0)
 *
 * The RDB partition DosType is matched with mask 0xFFFFFF00, so
 * 'NGF\0', 'NGF\1' ... all match.  The signature block carries the same magic
 * in its first longword; the on-disk version byte is at +0x03.
 * ------------------------------------------------------------------------- */

/* (DosType / sig u32@+0 & 0xFFFFFF00) == 0x4E474600 ("NGF"). */
#define NGFS_SIG_MAGIC			0x4E474600	/* 'N''G''F' (low byte = version) */
#define NGFS_SIG_MAGIC_MASK		0xFFFFFF00

/* On-disk version byte: signature u32@+0 & 0xFF (0 = v0, 1 = v1). */
#define NGFS_SIG_VERSION_OFF		0x03	/* low byte of the magic longword */

/*
 * Signature-block checksum: the sum of every big-endian u32 longword over the
 * sector is 0 (accumulator init 0).  The word at +0x04 is the balancing word.
 */
#define NGFS_SIG_CHECKSUM_OFF		0x04	/* balancing word (info only) */

/*
 * ASCII "key = value" config starts at +0x08 and begins "Amiga Partition:".
 * Parsed keys (the reader scans for these key tokens):
 *   "SectorsPerBlock" -> spb ; block_size = spb * 512
 *   "Reserved"        -> root super-block NGFS block number (default 2)
 */
#define NGFS_SIG_TEXT_OFF		0x08
#define NGFS_SIG_TEXT_MAGIC		"Amiga Partition:"
#define NGFS_KEY_SPB			"SectorsPerBlock"
#define NGFS_KEY_RESERVED		"Reserved"

/* The probe reads NGFS block 0 as a single 512-byte sector (spb not yet known). */
#define NGFS_PROBE_SECTOR_BYTES		512
/* Bytes per sector implied by the block->sector mapping. */
#define NGFS_SECTOR_BYTES		512

/* -------------------------------------------------------------------------
 * Root / super block (NGFS block 'Reserved', default 2): one of only two
 * checksum-validated blocks (signature and root).
 * ------------------------------------------------------------------------- */

/* Root magic 'R''O'' ''X' read as a big-endian u32 = 0x58204F52. */
#define NGFS_ROOT_MAGIC			0x58204F52

/* Default primary root NGFS block when "Reserved" is absent. */
#define NGFS_ROOT_DEFAULT_BLOCK		2

/*
 * The root super block is checksummed over the full block_size: the sum of
 * its u32 longwords is 0 over the whole block (for example 2048 bytes; the
 * +0x10 longword balances it, and a 512-byte sum is non-zero).  ngfs_probe()
 * passes ngfs_block_size directly, so no length constant is needed.
 */

/*
 * Version-selected root field offsets.  The root magic sits at geom+0x78, and
 * root_dir_addr is the u64 64-bit block address of the root directory's node
 * record.  The v0 offsets have not been tested on a real volume.
 */
#define NGFS_ROOT_GEOM_OFF_V1		0x190	/* geom struct (v1) */
#define NGFS_ROOT_MAGIC_OFF_V1		0x208	/* u32 magic (v1) = geom+0x78 */
#define NGFS_ROOT_ROOTDIR_OFF_V1	0x1C8	/* u64 root_dir_addr (v1) */

#define NGFS_ROOT_GEOM_OFF_V0		0x158	/* geom struct (v0) */
#define NGFS_ROOT_MAGIC_OFF_V0		0x1D0	/* u32 magic (v0) */
#define NGFS_ROOT_ROOTDIR_OFF_V0	0x190	/* u64 root_dir_addr (v0) */

/* Sanity bounds for block_size derived from spb * 512. */
#define NGFS_MIN_BLOCK_SIZE		512
#define NGFS_MAX_BLOCK_SIZE		65536

/* -------------------------------------------------------------------------
 * Object / node record (v1)
 *
 * The same record shape describes the root-dir object and every directory
 * entry.  Lightweight node-record copies (the packed dir entries) carry the
 * identity and name; the full object block (at own_block) also carries the
 * extent map.
 * ------------------------------------------------------------------------- */

#define NGFS_OBJ_OWN_OFF		0x00	/* u64 block# of this object's full block */
#define NGFS_OBJ_PARENT_OFF		0x08	/* u64 back-ptr to containing dir data (info) */
#define NGFS_OBJ_TYPE_OFF		0x10	/* u16 type; dir iff (type & ~0x10)==2 */
#define NGFS_OBJ_MAGIC			0x2017D583
#define NGFS_OBJ_MAGIC_OFF		0x20	/* u32 record magic */
#define NGFS_OBJ_RECSIZE_OFF		0x24	/* u16 packed-record stride (dir entries 0xB0) */
#define NGFS_OBJ_NAMELEN_OFF		0x26	/* u16 name length */
#define NGFS_OBJ_DATASIZE_OFF		0x68	/* u64 file size / dir-data byte count */
#define NGFS_OBJ_NAME_OFF		0x8C	/* ASCII name bytes */

/* Default packed dir-entry stride when a record's own rec_size is implausible. */
#define NGFS_DIR_REC_STRIDE		0xB0	/* 176, the usual dir-entry stride */
/* Smallest record that still has a name field. */
#define NGFS_REC_MIN			NGFS_OBJ_NAME_OFF

#define NGFS_NAME_MAX			255

/* Directory test: object is a directory when (type & ~0x10) == 2. */
#define NGFS_TYPE_DIR_FLAG		0x10
#define NGFS_TYPE_DIR_BASE		2
#define NGFS_IS_DIR_TYPE(t) \
	((((u16)(t)) & (u16)(~NGFS_TYPE_DIR_FLAG)) == NGFS_TYPE_DIR_BASE)

/* -------------------------------------------------------------------------
 * Extent map
 *
 * Present only in an object's full block.  Parallel arrays: u64 disk-block
 * start addresses (stride 8), then u32 per-extent block lengths (stride 4;
 * len[i] is the number of blocks in extent i, consumed cumulatively, not a
 * boundary).  The len[] array begins after the whole addr[] array, whose
 * dimension is the capacity u16 at +0x194, not the active count at +0x196.
 * So the len[] base is +0x1B0 + capacity * 8.  ext_count (active extents) is
 * at +0x196.
 * ------------------------------------------------------------------------- */

#define NGFS_EXT_COUNT_OFF		0x196	/* u16 active extent count */
#define NGFS_EXT_CAP_OFF		0x194	/* u16 capacity (info only) */
#define NGFS_EXT_ADDR_OFF		0x1B0	/* u64 addr[cap]; u32 len[cap] follows */
#define NGFS_EXT_CONT_OFF		0x18	/* u64 continuation/extension block */

/* One decoded extent: a contiguous run of 'len' blocks starting at disk 'addr'. */
struct ngfs_extent {
	u64 addr;	/* first disk block of the run */
	u32 len;	/* length of the run in NGFS blocks (not a boundary) */
};

/* Caps (loop / allocation guards). */
#define NGFS_MAX_EXTENTS		1024
#define NGFS_MAX_RUN			4096	/* blocks per read request */
#define NGFS_MAX_CONT_CHAIN		64
#define NGFS_MAX_DIR_DATA		(16UL << 20)	/* cap on assembled dir data */
#define NGFS_MAX_DIR_ENTRIES		65536		/* cap on entries per directory */

/* -------------------------------------------------------------------------
 * v0 (NGF\0) container blocks (type 2 and 13).  Not yet tested on a real
 * volume.
 * ------------------------------------------------------------------------- */

#define NGFS_V0_TYPE_OFF		0x10	/* u16 block type */
#define NGFS_V0_TYPE2			2	/* root container */
#define NGFS_V0_TYPE13			13	/* sub container */
#define NGFS_V0_T2_COUNT_OFF		0x15C	/* u16 child count (type 2) */
#define NGFS_V0_T2_ARRAY_OFF		0x178	/* u64 child addr[] (type 2) */
#define NGFS_V0_T13_COUNT_OFF		0x30	/* u16 child count (type 13) */
#define NGFS_V0_T13_ARRAY_OFF		0x38	/* u64 child addr[] (type 13) */
#define NGFS_V0_T13_CHAIN_OFF		0x18	/* u64 next container (type 13) */
#define NGFS_V0_NAMELEN_OFF		0x40	/* u16 name_len on a child object */
#define NGFS_V0_NAME_OFF		0x42	/* ASCII name on a child object */
#define NGFS_CHILD_ENTRY_SIZE		8	/* u64 per child address slot */

/* -------------------------------------------------------------------------
 * U-Boot generic filesystem operations (mirrors include/sfs.h / affs.h).
 * ------------------------------------------------------------------------- */
int ngfs_probe(struct blk_desc *fs_dev_desc,
	       struct disk_partition *fs_partition);
int ngfs_opendir(const char *filename, struct fs_dir_stream **dirsp);
int ngfs_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
void ngfs_closedir(struct fs_dir_stream *dirs);
int ngfs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	      loff_t *actread);
int ngfs_size(const char *filename, loff_t *size);
int ngfs_exists(const char *filename);
void ngfs_close(void);

#endif /* _NGFS_H_ */
