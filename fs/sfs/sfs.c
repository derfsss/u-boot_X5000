// SPDX-License-Identifier: GPL-2.0+
/*
 * sfs.c: Amiga SmartFileSystem (SFS) read-only filesystem driver.
 *
 * Plugs into U-Boot's generic filesystem framework (fstype/ls/load/size) so
 * the AmigaOne X5000 and A1222 ports can read files from SFS Amiga RDB
 * partitions.  Both SFS\0 and SFS\2 (48-bit file sizes) are handled.
 *
 * Ported from ub2lb (the UBoot second level bootloader, "Parthenope"), whose
 * working SFS reader this driver is modelled on.  The on-disk structures and
 * the lookup logic (root-block probe, path resolution, object-container walk,
 * extent B-tree lookup and extent-chain file read) come from that code.
 *
 * Original ub2lb SFS reader:
 *   Copyright (C) 2006 - 2010  Giuseppe Coviello <cjg@cruxppc.org>
 *   Copyright (C) 2010         ACube Systems Srl
 *
 * U-Boot port:
 *   Copyright (C) 2026 Richard Gibbs
 *
 * ub2lb's only block-IO primitive was loadsector(); here all block access goes
 * through fs_devread() (fs/fs_internal.c).  The SFS block size may differ from
 * the device's 512-byte (or larger) sector size, so an SFS block number is
 * converted to a (sector, byte_offset) pair before each read.
 */

#include <common.h>
#include <blk.h>
#include <errno.h>
#include <fs.h>
#include <fs_internal.h>
#include <malloc.h>
#include <part.h>
#include <sfs.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/string.h>

/* Device / partition stashed by sfs_probe(), used by all reads. */
static struct blk_desc *sfs_blk_desc;
static struct disk_partition sfs_part_info;

/* Geometry learned from the SFS root block. */
static u32 sfs_blocksize;		/* bytes per SFS block */

#define SFS_MAX_RUN	4096		/* blocks per read request */
static u32 sfs_extentbnoderoot;		/* SFS block of the extent B-tree root */
static bool sfs_v2;			/* SFS\2 rather than SFS\0 */
static u32 sfs_root_firstdirblock;	/* first object container of the root dir */

/* Decoded extent (one run of contiguous blocks). */
struct sfs_extent {
	u32 key;
	u32 next;
	u32 prev;
	u32 blocks;
};

/*
 * SFS\2 (root block version 4) widens a file's size to 48 bits: the size
 * field at +16 gains a high 16-bit word in front of the 32-bit low part,
 * so every later field of an object moves up by 2 bytes. Directories keep
 * firstdirblock at +16. Read object fields through these.
 */
#define SFS_OBJ_COPY	28		/* fixed part of either kind of object */

static u8 sfs_obj_bits(const struct SfsObject *o)
{
	return ((const u8 *)o)[sfs_v2 ? 26 : 24];
}

static u8 *sfs_obj_name(const struct SfsObject *o)
{
	return (u8 *)o + (sfs_v2 ? 27 : 25);
}

static u64 sfs_obj_size(const struct SfsObject *o)
{
	const u8 *p = (const u8 *)o + 16;

	if (sfs_v2)
		return (u64)get_unaligned_be16(p) << 32 |
		       get_unaligned_be32(p + 2);
	return get_unaligned_be32(p);
}

/* Directory stream returned to the fs layer; fs_dir_stream must be first. */
struct sfs_dir_stream {
	struct fs_dir_stream fs_dirs;
	u32 next_block;			/* next object container to load (0 = end) */
	struct SfsObject *cur;		/* current object in buf, NULL => reload */
	void *buf;			/* holds the current object container */
	struct fs_dirent dent;		/* entry handed back by sfs_readdir() */
};

/*
 * Read 'count' consecutive SFS blocks starting at SFS block number 'block'
 * into 'buf'.  Returns 1 on success, 0 on failure (matching fs_devread()).
 *
 * fs_devread() takes a sector number in device-block units relative to the
 * partition start plus a byte offset.  An SFS block is sfs_blocksize bytes,
 * which need not equal the device block size, so map the SFS block number to
 * the absolute byte position on the partition and split it into a device
 * sector and an intra-sector byte offset.
 */
static int sfs_read_blocks(u32 block, u32 count, void *buf)
{
	u64 byte_pos;
	lbaint_t sector;
	int byte_offset;

	if (!sfs_blk_desc)
		return 0;

	byte_pos = (u64)block * sfs_blocksize;
	sector = (lbaint_t)(byte_pos / sfs_blk_desc->blksz);
	byte_offset = (int)(byte_pos - (u64)sector * sfs_blk_desc->blksz);

	return fs_devread(sfs_blk_desc, &sfs_part_info, sector, byte_offset,
			  (int)(count * sfs_blocksize), (char *)buf);
}

/*
 * Recompute an SFS block's checksum.  The on-disk algorithm sums the block as
 * an array of big-endian 32-bit words (with the checksum field treated as 0);
 * a live, written block yields a non-zero result, a blank/zero block yields 0.
 * This mirrors ub2lb, where a zero result is treated as "invalid".
 */
static u32 sfs_calcchecksum(void *block, u32 blocksize)
{
	u8 *data = (u8 *)block;
	u32 checksum = 1;

	SBH(block)->checksum = 0;

	while (blocksize >= 4) {
		checksum += get_unaligned_be32(data);
		data += 4;
		blocksize -= 4;
	}

	return -checksum;
}

/*
 * Advance to the next object in an object container.  Objects are
 * variable-length: skip past the name and comment strings (two NUL
 * terminators) then round up to a 16-bit boundary.
 */
static struct SfsObject *sfs_nextobject(struct SfsObject *o)
{
	int i = 2;
	u8 *p = sfs_obj_name(o);

	while (i > 0) {
		if (*p == '\0')
			i--;
		p++;
	}

	if ((p - (u8 *)o) & 0x01)
		p++;

	return (struct SfsObject *)p;
}

/*
 * Walk a directory's object-container chain (firstdirblock -> next -> ...) and
 * return an allocated copy of the fixed part of the object whose name matches
 * 'name' (case-insensitively) and whose type satisfies 'type_mask' (0 = any).
 * Deleted objects are skipped.  Caller frees the result.
 */
static struct SfsObject *sfs_findobject(u32 firstdirblock, const char *name,
					u8 type_mask)
{
	struct SfsObject *o, *res;
	u32 next = firstdirblock;
	void *buf;

	buf = malloc(sfs_blocksize);
	if (!buf)
		return NULL;

	while (next != 0) {
		if (!sfs_read_blocks(next, 1, buf))
			break;
		if (!sfs_calcchecksum(buf, sfs_blocksize))
			break;
		if (get_unaligned_be32(&SBH(buf)->id) != SOC_ID)
			break;

		o = SOC(buf)->object;
		while (get_unaligned_be32(&o->objectnode) > 0) {
			u8 bits = sfs_obj_bits(o);

			if (!(bits & OTYPE_DELETED) &&
			    strcasecmp((char *)sfs_obj_name(o), name) == 0 &&
			    (!type_mask || (bits & type_mask))) {
				res = malloc(SFS_OBJ_COPY);
				if (res)
					memcpy(res, o, SFS_OBJ_COPY);
				free(buf);
				return res;
			}
			o = sfs_nextobject(o);
		}
		next = get_unaligned_be32(&SOC(buf)->next);
	}

	free(buf);
	return NULL;
}

/*
 * Resolve a directory path to the block number of its first object container.
 * Every path component must be a directory.  "/" / "" resolve to the root.
 * Returns 0 on success.
 */
static int sfs_resolve_dir(const char *path, u32 *out)
{
	u32 cur = sfs_root_firstdirblock;
	char *dup, *p, *slash;
	struct SfsObject *o;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;

	p = dup;
	while (*p == '/')
		p++;

	while (*p) {
		slash = strchr(p, '/');
		if (slash)
			*slash = '\0';

		if (*p) {
			o = sfs_findobject(cur, p, OTYPE_DIR);
			if (!o) {
				free(dup);
				return -ENOENT;
			}
			cur = get_unaligned_be32(&o->object.dir.firstdirblock);
			free(o);
		}

		if (!slash)
			break;
		p = slash + 1;
	}

	free(dup);
	*out = cur;
	return 0;
}

/*
 * Resolve a full path to its (final-component) object.  The parent
 * directories are walked first, then the last component is looked up with any
 * type.  Returns an allocated object copy (caller frees) or NULL.
 */
static struct SfsObject *sfs_find(const char *path)
{
	struct SfsObject *o;
	char *dup, *file, *slash;
	u32 cur;

	dup = strdup(path);
	if (!dup)
		return NULL;

	slash = strrchr(dup, '/');
	if (slash) {
		*slash = '\0';
		file = slash + 1;
		if (sfs_resolve_dir(dup, &cur)) {
			free(dup);
			return NULL;
		}
	} else {
		file = dup;
		cur = sfs_root_firstdirblock;
	}

	if (*file == '\0') {
		free(dup);
		return NULL;
	}

	o = sfs_findobject(cur, file, 0);
	free(dup);
	return o;
}

/*
 * Look up an extent in the extent B-tree.  Descends from the root, at each
 * level picking the entry with the largest key <= 'key'; on a leaf that entry
 * is the extent.  Returns 0 and fills 'out' on success.
 */
static int sfs_get_extent(u32 key, struct sfs_extent *out)
{
	struct BTreeContainer *btc;
	struct SfsExtentBNode *e;
	struct BNode *bnode, *tmp;
	u32 next = sfs_extentbnoderoot;
	int ret = -ENOENT;
	void *buf;
	int i;

	buf = malloc(sfs_blocksize);
	if (!buf)
		return -ENOMEM;

	while (sfs_read_blocks(next, 1, buf)) {
		if (!sfs_calcchecksum(buf, sfs_blocksize))
			break;

		btc = &SBNC(buf)->btc;
		bnode = NULL;

		for (i = get_unaligned_be16(&btc->nodecount); i > 0; i--) {
			tmp = (struct BNode *)((u8 *)btc->bnode +
					       (i - 1) * btc->nodesize);
			if (get_unaligned_be32(&tmp->key) <= key) {
				bnode = tmp;
				break;
			}
		}

		if (!bnode)
			break;

		if (btc->isleaf) {
			e = (struct SfsExtentBNode *)bnode;
			out->key = get_unaligned_be32(&e->key);
			out->next = get_unaligned_be32(&e->next);
			out->prev = get_unaligned_be32(&e->prev);
			/* SFS\2 leaves are 16 bytes, with a 32-bit length */
			out->blocks = btc->nodesize >= 16 ?
				      get_unaligned_be32(&e->blocks) :
				      get_unaligned_be16(&e->blocks);
			ret = 0;
			break;
		}

		next = get_unaligned_be32(&bnode->data);
	}

	free(buf);
	return ret;
}

/*
 * Identify an SFS partition from its root block (SFS\0 or SFS\2) and load the
 * block size, extent B-tree root and root directory used by every other
 * operation.  Returns 0 if the partition is SFS, -1 otherwise.
 */
int sfs_probe(struct blk_desc *fs_dev_desc, struct disk_partition *fs_partition)
{
	u32 blocksize, rootcontainer;
	void *buf;

	sfs_blk_desc = fs_dev_desc;
	sfs_part_info = *fs_partition;

	/*
	 * The root block is the first block of the partition.  Its size is not
	 * known yet, so read one device sector (enough to cover the root block
	 * header fields) directly via fs_devread().
	 */
	buf = malloc(sfs_blk_desc->blksz);
	if (!buf)
		goto err;

	if (!fs_devread(sfs_blk_desc, &sfs_part_info, 0, 0,
			(int)sfs_blk_desc->blksz, (char *)buf))
		goto err_free;

	switch (get_unaligned_be32(&SBH(buf)->id)) {
	case SRB_ID:
		sfs_v2 = false;
		break;
	case SRB_ID2:
		sfs_v2 = true;
		break;
	default:
		goto err_free;
	}

	blocksize = get_unaligned_be32(&SRB(buf)->blocksize);
	/* Power-of-two, sane bounds, and a multiple of the device sector. */
	if (blocksize < 512 || blocksize > 65536 ||
	    (blocksize & (blocksize - 1)) ||
	    (blocksize % sfs_blk_desc->blksz) != 0)
		goto err_free;

	sfs_blocksize = blocksize;
	sfs_extentbnoderoot = get_unaligned_be32(&SRB(buf)->extentbnoderoot);
	rootcontainer = get_unaligned_be32(&SRB(buf)->rootobjectcontainer);
	free(buf);

	/* Load the root object container; its first object is the root dir. */
	buf = malloc(sfs_blocksize);
	if (!buf)
		goto err;

	if (!sfs_read_blocks(rootcontainer, 1, buf))
		goto err_free;
	if (get_unaligned_be32(&SBH(buf)->id) != SOC_ID)
		goto err_free;

	sfs_root_firstdirblock =
		get_unaligned_be32(&SOC(buf)->object[0].object.dir.firstdirblock);

	free(buf);
	return 0;

err_free:
	free(buf);
err:
	sfs_blk_desc = NULL;
	return -1;
}

int sfs_opendir(const char *filename, struct fs_dir_stream **dirsp)
{
	struct sfs_dir_stream *dirs;
	u32 firstdir;

	if (sfs_resolve_dir(filename, &firstdir))
		return -ENOENT;

	dirs = calloc(1, sizeof(*dirs));
	if (!dirs)
		return -ENOMEM;

	dirs->buf = malloc(sfs_blocksize);
	if (!dirs->buf) {
		free(dirs);
		return -ENOMEM;
	}

	dirs->next_block = firstdir;
	dirs->cur = NULL;

	*dirsp = (struct fs_dir_stream *)dirs;
	return 0;
}

int sfs_readdir(struct fs_dir_stream *fs_dirs, struct fs_dirent **dentp)
{
	struct sfs_dir_stream *dirs = (struct sfs_dir_stream *)fs_dirs;
	struct fs_dirent *dent = &dirs->dent;
	struct SfsObject *o;
	u8 bits;

	for (;;) {
		/* Need to (re)load an object container? */
		if (!dirs->cur ||
		    get_unaligned_be32(&dirs->cur->objectnode) == 0) {
			if (dirs->next_block == 0) {
				*dentp = NULL;
				return -ENOENT;
			}
			if (!sfs_read_blocks(dirs->next_block, 1, dirs->buf)) {
				*dentp = NULL;
				return -EIO;
			}
			if (!sfs_calcchecksum(dirs->buf, sfs_blocksize) ||
			    get_unaligned_be32(&SBH(dirs->buf)->id) != SOC_ID) {
				*dentp = NULL;
				return -ENOENT;
			}
			dirs->next_block =
				get_unaligned_be32(&SOC(dirs->buf)->next);
			dirs->cur = SOC(dirs->buf)->object;
			/* Empty container: move on to the next one. */
			if (get_unaligned_be32(&dirs->cur->objectnode) == 0)
				continue;
		}

		o = dirs->cur;
		dirs->cur = sfs_nextobject(o);

		/* Skip deleted objects. */
		bits = sfs_obj_bits(o);
		if (bits & OTYPE_DELETED)
			continue;

		if (bits & OTYPE_DIR) {
			dent->type = FS_DT_DIR;
			dent->size = 0;
		} else if (bits & OTYPE_LINK) {
			dent->type = FS_DT_LNK;
			dent->size = 0;
		} else {
			dent->type = FS_DT_REG;
			dent->size = sfs_obj_size(o);
		}

		strncpy(dent->name, (char *)sfs_obj_name(o),
			sizeof(dent->name) - 1);
		dent->name[sizeof(dent->name) - 1] = '\0';

		*dentp = dent;
		return 0;
	}
}

void sfs_closedir(struct fs_dir_stream *fs_dirs)
{
	struct sfs_dir_stream *dirs = (struct sfs_dir_stream *)fs_dirs;

	if (!dirs)
		return;

	free(dirs->buf);
	free(dirs);
}

int sfs_size(const char *filename, loff_t *size)
{
	struct SfsObject *o;

	*size = 0;

	o = sfs_find(filename);
	if (!o)
		return -ENOENT;

	if (sfs_obj_bits(o) & OTYPE_DIR) {
		free(o);
		return -EISDIR;
	}

	*size = sfs_obj_size(o);
	free(o);
	return 0;
}

int sfs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	     loff_t *actread)
{
	struct sfs_extent ext;
	struct SfsObject *o;
	u64 filepos = 0;	/* byte offset (in file) of the current block */
	u64 copied = 0;
	u64 want, size;
	u32 next;
	void *blkbuf;
	u8 bits;
	u32 i;

	*actread = 0;

	o = sfs_find(filename);
	if (!o)
		return -ENOENT;

	bits = sfs_obj_bits(o);
	next = get_unaligned_be32(&o->object.file.data);
	size = sfs_obj_size(o);
	free(o);

	if (bits & OTYPE_DIR)
		return -EISDIR;

	if ((u64)offset >= size)
		return 0;

	want = size - (u64)offset;
	if (len && (u64)len < want)
		want = (u64)len;

	blkbuf = malloc(sfs_blocksize);
	if (!blkbuf)
		return -ENOMEM;

	/* Walk the extent chain, reading the blocks that overlap the request. */
	while (next != 0 && filepos < size && copied < want) {
		if (sfs_get_extent(next, &ext))
			break;

		for (i = 0; i < ext.blocks && filepos < size && copied < want;
		     i++) {
			u64 blk_actual = (size - filepos < sfs_blocksize) ?
					 (size - filepos) : sfs_blocksize;
			u64 blk_start = filepos;
			u64 blk_end = filepos + blk_actual;
			u64 copy_from = ((u64)offset > blk_start) ?
					(u64)offset : blk_start;
			u64 copy_end = ((u64)offset + want < blk_end) ?
				       (u64)offset + want : blk_end;
			u32 n = 0;

			/*
			 * Whole blocks inside the request: read the rest of
			 * the extent that fits straight into buf, with one
			 * request instead of one per block.
			 */
			while (blk_start >= (u64)offset && i + n < ext.blocks &&
			       n < SFS_MAX_RUN &&
			       blk_start + (u64)(n + 1) * sfs_blocksize <= size &&
			       blk_start + (u64)(n + 1) * sfs_blocksize <=
			       (u64)offset + want)
				n++;
			if (n) {
				if (!sfs_read_blocks(next + i, n, (u8 *)buf +
						     (blk_start - (u64)offset))) {
					free(blkbuf);
					return -EIO;
				}
				copied += (u64)n * sfs_blocksize;
				filepos += (u64)n * sfs_blocksize;
				i += n - 1;
				continue;
			}

			if (copy_end > copy_from) {
				if (!sfs_read_blocks(next + i, 1, blkbuf)) {
					free(blkbuf);
					return -EIO;
				}
				memcpy((u8 *)buf + (copy_from - (u64)offset),
				       (u8 *)blkbuf + (copy_from - blk_start),
				       (size_t)(copy_end - copy_from));
				copied += copy_end - copy_from;
			}

			filepos += sfs_blocksize;
		}

		next = ext.next;
	}

	free(blkbuf);
	*actread = copied;
	return 0;
}

int sfs_exists(const char *filename)
{
	struct SfsObject *o;
	u32 firstdir;

	o = sfs_find(filename);
	if (o) {
		free(o);
		return 1;
	}

	/* Maybe it is a directory path (e.g. ends in '/'). */
	if (sfs_resolve_dir(filename, &firstdir) == 0)
		return 1;

	return 0;
}

void sfs_close(void)
{
	sfs_blk_desc = NULL;
}
