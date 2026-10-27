/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * sfs.h: Amiga SmartFileSystem (SFS), read-only, on-disk structures.
 *
 * Ported to U-Boot's generic filesystem framework from ub2lb (the UBoot
 * second level bootloader / "Parthenope") SFS reader.
 *
 * Copyright (C) 2006 - 2010  Giuseppe Coviello <cjg@cruxppc.org>
 * Copyright (C) 2010         ACube Systems Srl
 * Copyright (C) 2026 Richard Gibbs
 *
 * All multi-byte on-disk fields are stored big-endian.  They are declared
 * here with their natural C types and must be read through the
 * be*_to_cpu()/get_unaligned_be*() helpers so the driver does not depend on
 * host endianness (on the big-endian PowerPC hosts the conversion is a
 * no-op).
 */

#ifndef _SFS_H_
#define _SFS_H_

#include <linux/types.h>

struct blk_desc;
struct disk_partition;
struct fs_dir_stream;
struct fs_dirent;

/* Common header at the start of every administrative SFS block. */
struct SfsBlockHeader {
	u32 id;
	u32 checksum;
	u32 ownblock;
};
#define SBH(b) ((struct SfsBlockHeader *)(b))

/* The Root Block lives at the very first block of an SFS partition. */
struct SfsRootBlock {
	struct SfsBlockHeader bheader;

	u16 version;
	u16 sequencenumber;

	u32 datecreated;
	u8 bits;
	u8 pad1;
	u16 pad2;

	u32 reserved1[2];

	u32 firstbyteh;
	u32 firstbyte;

	u32 lastbyteh;
	u32 lastbyte;

	u32 totalblocks;
	u32 blocksize;

	u32 reserved2[2];
	u32 reserved3[8];

	u32 bitmapbase;
	u32 adminspacecontainer;
	u32 rootobjectcontainer;
	u32 extentbnoderoot;

	u32 reserved4[4];
};
#define SRB(b) ((struct SfsRootBlock *)(b))
#define SRB_ID 0x53465300	/* 'S''F''S''\0' */
#define SRB_ID2 0x53465302	/* 'S''F''S''\2': 48-bit file sizes */

/* Object bits. */
#define OTYPE_HIDDEN  (1)
#define OTYPE_DELETED (32)
#define OTYPE_LINK    (64)
#define OTYPE_DIR     (128)

/*
 * A directory entry.  Variable length: a name string and a comment string
 * (both NUL terminated) follow the fixed part.  Objects within an object
 * container are packed and 16-bit aligned, so the multi-byte fields of any
 * object other than the first may be misaligned -- always use the unaligned
 * accessors when reading them.
 */
struct SfsObject {
	u16 owneruid;
	u16 ownergid;
	u32 objectnode;
	u32 protection;

	union {
		struct {
			u32 data;
			u32 size;
		} file;

		struct {
			u32 hashtable;
			u32 firstdirblock;
		} dir;
	} object;

	u32 datemodified;
	u8 bits;

	u8 name[0];
};
#define SO(b) ((struct SfsObject *)(b))

/* An Object Container holds a chain of variable-length objects. */
struct SfsObjectContainer {
	struct SfsBlockHeader bheader;
	u32 parent;
	u32 next;
	u32 previous;
	struct SfsObject object[1];
};
#define SOC(b) ((struct SfsObjectContainer *)(b))
#define SOC_ID (('O' << 24) | ('B' << 16) | ('J' << 8) | 'C')

/* B-tree node (internal nodes: key + child block in 'data'). */
struct BNode {
	u32 key;
	u32 data;
};

struct BTreeContainer {
	u16 nodecount;
	u8 isleaf;
	u8 nodesize;

	struct BNode bnode[0];
};

struct SfsBNodeContainer {
	struct SfsBlockHeader bheader;
	struct BTreeContainer btc;
};
#define SBNC(b) ((struct SfsBNodeContainer *)(b))

/* Leaf node of the extent B-tree: describes a run of contiguous blocks. */
struct SfsExtentBNode {
	u32 key;
	u32 next;
	u32 prev;
	u16 blocks;
};

/* U-Boot generic filesystem operations (mirrors include/squashfs.h). */
int sfs_probe(struct blk_desc *fs_dev_desc,
	      struct disk_partition *fs_partition);
int sfs_opendir(const char *filename, struct fs_dir_stream **dirsp);
int sfs_readdir(struct fs_dir_stream *dirs, struct fs_dirent **dentp);
void sfs_closedir(struct fs_dir_stream *dirs);
int sfs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	     loff_t *actread);
int sfs_size(const char *filename, loff_t *size);
int sfs_exists(const char *filename);
void sfs_close(void);

#endif /* _SFS_H_ */
