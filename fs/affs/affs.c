// SPDX-License-Identifier: GPL-2.0+
/*
 * affs.c: Amiga FastFileSystem / OldFileSystem (FFS/OFS, "affs") read-only
 * filesystem driver.
 *
 * Plugs into U-Boot's generic filesystem framework (fstype/ls/load/size) so
 * the AmigaOne X5000 and A1222 ports can read files from FFS/OFS Amiga RDB
 * partitions.
 *
 * Written for U-Boot.  The on-disk layout comes from the public AmigaDOS
 * documentation and was cross-checked against the Linux kernel affs driver
 * (fs/affs/amigaffs.c, file.c, GPL-2.0, used only as a format reference) and
 * the amitools implementation, then checked against a real FFS image.
 *
 * Block I/O goes through fs_devread() (fs/fs_internal.c).  The affs block size
 * (BSIZE) may differ from the device's 512-byte sector size, so an affs block
 * number (relative to the partition) is converted to a (sector, byte_offset)
 * pair before each read, mirroring the sibling SFS driver (fs/sfs/sfs.c).
 *
 * Modes:
 *   - FFS (DOS\1,3,5,7): data blocks are raw BSIZE bytes.
 *   - OFS (DOS\0,2,4,6): data blocks carry a 24-byte header that is stripped.
 *   - International (DOS>=2): hashing upper-cases 0xE0..0xFE (except 0xF7) too.
 *   - Long names (DOS\6/\7, the AmigaOS 4 default): the file/dir name is a
 *     BSTR in the combined name+comment field at BSIZE-184 instead of BSIZE-80.
 *   - DirCache (DOS\4/\5): the cache blocks are ignored; the hashtable is
 *     walked as usual, so reads still work.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

#include <common.h>
#include <blk.h>
#include <errno.h>
#include <fs.h>
#include <fs_internal.h>
#include <malloc.h>
#include <part.h>
#include <affs.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/string.h>

/* Device / partition stashed by affs_probe(), used by all reads. */
static struct blk_desc *affs_blk_desc;
static struct disk_partition affs_part_info;

/* Geometry / mode learned by affs_probe(). */
static u32 affs_bsize;		/* bytes per affs block (BSIZE)              */
static u32 affs_ht_size;	/* hashtable / data-table entries (BSIZE/4-56) */
static u32 affs_root_block;	/* root header block (relative to partition) */
static int affs_ffs;		/* 1 = FFS raw data, 0 = OFS (strip 24)      */
static int affs_intl;		/* 1 = international upper-casing for hashing */
static int affs_longname;	/* 1 = DOS\6/\7 long-name layout             */

/* Directory stream handed back to the fs layer; fs_dir_stream must be first. */
struct affs_dir_stream {
	struct fs_dir_stream fs_dirs;
	u32 dir_block;			/* directory header (holds the hashtable) */
	u32 slot;			/* next hashtable slot to scan            */
	u32 chain;			/* next entry block in the current chain  */
	void *hdrbuf;			/* directory header block (the hashtable) */
	void *entbuf;			/* scratch for entry blocks               */
	struct fs_dirent dent;		/* entry handed back by affs_readdir()    */
};

/* Read one BSIZE affs block (relative to the partition).  1 = ok, 0 = fail. */
static int affs_read_block(u32 block, void *buf)
{
	u64 byte_pos;
	lbaint_t sector;
	int byte_offset;

	if (!affs_blk_desc || !affs_bsize)
		return 0;

	byte_pos = (u64)block * affs_bsize;
	sector = (lbaint_t)(byte_pos / affs_blk_desc->blksz);
	byte_offset = (int)(byte_pos - (u64)sector * affs_blk_desc->blksz);

	return fs_devread(affs_blk_desc, &affs_part_info, sector, byte_offset,
			  (int)affs_bsize, (char *)buf);
}

/* Signed read of a trailer field expressed relative to BSIZE. */
static s32 affs_be32_rel(const void *blk, u32 rel)
{
	return (s32)get_unaligned_be32((const u8 *)blk + affs_bsize - rel);
}

/* International-aware upper-casing used by the BCPL name hash. */
static int affs_toupper(int c)
{
	c &= 0xff;
	if (c >= 'a' && c <= 'z')
		return c - ('a' - 'A');
	if (affs_intl && c >= 0xe0 && c <= 0xfe && c != 0xf7)
		return c - ('a' - 'A');
	return c;
}

/*
 * AmigaDOS BCPL name hash: start with the length, fold in each upper-cased
 * character (hash = (hash * 13 + ch) & 0x7ff), then take it modulo the
 * hashtable size.
 */
static u32 affs_hashname(const char *name, int len)
{
	u32 hash = (u32)len;
	int i;

	for (i = 0; i < len; i++)
		hash = (hash * 13 + (u32)affs_toupper((u8)name[i])) & 0x7ff;

	return affs_ht_size ? (hash % affs_ht_size) : 0;
}

/*
 * Extract an entry's name from its header block into 'out' (NUL terminated).
 * Long-name volumes keep the name in the combined name+comment BSTR field at
 * BSIZE-184; short-name volumes keep it at BSIZE-80.  Returns the length.
 */
static int affs_entry_name(const void *blk, char *out, int outmax)
{
	const u8 *b = blk;
	u32 base;
	int len, maxlen;

	if (affs_longname) {
		base = affs_bsize - AFFS_OFFR_NAME_LN;
		maxlen = AFFS_NAME_MAX_LONG;
	} else {
		base = affs_bsize - AFFS_OFFR_NAME;
		maxlen = AFFS_NAME_MAX_SHORT;
	}

	len = b[base];
	if (len > maxlen)
		len = maxlen;
	if (len > outmax - 1)
		len = outmax - 1;

	memcpy(out, b + base + 1, len);
	out[len] = '\0';
	return len;
}

/* Case-insensitive (affs international) compare of two equal-length names. */
static int affs_name_eq(const char *a, const char *b, int len)
{
	int i;

	for (i = 0; i < len; i++)
		if (affs_toupper((u8)a[i]) != affs_toupper((u8)b[i]))
			return 0;
	return 1;
}

/*
 * Look up 'name' (length 'namelen') inside the directory whose header block is
 * 'dir_block'.  Hash the name, follow the hashtable bucket and its hash chain
 * (BSIZE-16), and return the matching entry's block number, or 0.
 */
static u32 affs_find_in_dir(u32 dir_block, const char *name, int namelen)
{
	void *dbuf, *cbuf;
	u32 cand = 0;
	u32 h;

	dbuf = malloc(affs_bsize);
	cbuf = malloc(affs_bsize);
	if (!dbuf || !cbuf)
		goto out;

	if (!affs_read_block(dir_block, dbuf))
		goto out;
	if (get_unaligned_be32((u8 *)dbuf + AFFS_OFF_TYPE) != AFFS_T_HEADER)
		goto out;

	h = affs_hashname(name, namelen);
	cand = get_unaligned_be32((u8 *)dbuf + AFFS_OFF_HASHTABLE + h * 4);

	while (cand) {
		char nbuf[256];
		int nlen;

		if (!affs_read_block(cand, cbuf)) {
			cand = 0;
			break;
		}
		if (get_unaligned_be32((u8 *)cbuf + AFFS_OFF_TYPE) !=
		    AFFS_T_HEADER)
			break;

		nlen = affs_entry_name(cbuf, nbuf, sizeof(nbuf));
		if (nlen == namelen && affs_name_eq(nbuf, name, namelen))
			break;

		cand = affs_be32_rel(cbuf, AFFS_OFFR_HASHCHAIN);
	}

out:
	free(dbuf);
	free(cbuf);
	return cand;
}

/* Is the header block 'blk' a directory (or the root)? */
static int affs_block_is_dir(u32 block)
{
	void *buf;
	int st, ret = 0;

	buf = malloc(affs_bsize);
	if (!buf)
		return 0;
	if (affs_read_block(block, buf)) {
		st = affs_be32_rel(buf, AFFS_OFFR_SECTYPE);
		ret = (st == AFFS_ST_USERDIR || st == AFFS_ST_ROOT);
	}
	free(buf);
	return ret;
}

/*
 * Resolve a directory path to its header block number.  Every component must
 * be a directory.  "" and "/" resolve to the root.  Returns 0 on success.
 */
static int affs_resolve_dir(const char *path, u32 *out)
{
	u32 cur = affs_root_block;
	char *dup, *p, *slash;

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
			u32 child = affs_find_in_dir(cur, p, strlen(p));

			if (!child) {
				free(dup);
				return -ENOENT;
			}
			if (!affs_block_is_dir(child)) {
				free(dup);
				return -ENOTDIR;
			}
			cur = child;
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
 * Resolve a full path to its final entry's header block number (any type).
 * The parent directories are walked first, then the last component is looked
 * up.  Returns 0 if not found.
 */
static u32 affs_find(const char *path)
{
	char *dup, *file, *slash;
	u32 dir, blk;

	dup = strdup(path);
	if (!dup)
		return 0;

	slash = strrchr(dup, '/');
	if (slash) {
		*slash = '\0';
		file = slash + 1;
		if (affs_resolve_dir(dup, &dir)) {
			free(dup);
			return 0;
		}
	} else {
		file = dup;
		dir = affs_root_block;
	}

	if (*file == '\0') {
		free(dup);
		return 0;
	}

	blk = affs_find_in_dir(dir, file, strlen(file));
	free(dup);
	return blk;
}

/*
 * Identify an FFS/OFS partition from its boot block DOS type (DOS\0..DOS\7),
 * then find the block size and root block.  Returns 0 if the partition is
 * FFS/OFS, -1 otherwise.
 */
int affs_probe(struct blk_desc *fs_dev_desc, struct disk_partition *fs_partition)
{
	static const u32 try_bs[] = { 512, 1024, 2048, 4096 };
	u64 part_bytes;
	u32 dostype, bootroot;
	int flag, i;
	u8 *boot;

	affs_blk_desc = fs_dev_desc;
	affs_part_info = *fs_partition;
	affs_bsize = 0;

	if (!fs_dev_desc || !fs_dev_desc->blksz)
		goto err;

	/* Read the boot block (partition-relative block 0) for the DOS type. */
	boot = malloc(fs_dev_desc->blksz);
	if (!boot)
		goto err;
	if (!fs_devread(fs_dev_desc, &affs_part_info, 0, 0,
			(int)fs_dev_desc->blksz, (char *)boot)) {
		free(boot);
		goto err;
	}

	dostype = get_unaligned_be32(boot);
	flag = dostype & 0xff;
	if ((dostype & 0xffffff00) != AFFS_DOSTYPE_PREFIX ||
	    flag > AFFS_DOS_FFS_LN) {
		free(boot);
		goto err;
	}

	affs_ffs = flag & AFFS_FLAG_FFS;
	affs_intl = (flag >= AFFS_DOS_OFS_INTL);
	affs_longname = (flag == AFFS_DOS_OFS_LN || flag == AFFS_DOS_FFS_LN);
	bootroot = get_unaligned_be32(boot + 8);	/* spec rootblock ptr */
	free(boot);

	part_bytes = (u64)affs_part_info.size * fs_dev_desc->blksz;

	/*
	 * The block size is not stored in the boot block, so probe the common
	 * values.  For each, the root block sits near the partition midpoint
	 * (reserved + nblocks - 1) / 2 with reserved == 2.  Validate candidates
	 * around that point (and the boot block's rootblock pointer) by checking
	 * the primary type, secondary type and hashtable size.
	 */
	for (i = 0; i < (int)ARRAY_SIZE(try_bs); i++) {
		u32 bs = try_bs[i];
		u64 nblocks;
		u32 ht, mid, c;
		int j;
		u32 cands[6];
		void *rb;

		if (bs < fs_dev_desc->blksz || (bs % fs_dev_desc->blksz))
			continue;
		if ((u64)bs > part_bytes)
			continue;

		nblocks = part_bytes / bs;
		if (nblocks < 4)
			continue;
		ht = bs / 4 - AFFS_RESERVED_LONGS;
		mid = (u32)((nblocks + 1) / 2);

		cands[0] = mid;
		cands[1] = mid - 1;
		cands[2] = mid + 1;
		cands[3] = mid - 2;
		cands[4] = mid + 2;
		cands[5] = bootroot;

		affs_bsize = bs;
		affs_ht_size = ht;

		rb = malloc(bs);
		if (!rb) {
			affs_bsize = 0;
			goto err;
		}

		for (j = 0; j < 6; j++) {
			c = cands[j];
			if (c == 0 || (u64)c >= nblocks)
				continue;
			if (!affs_read_block(c, rb))
				continue;
			if (get_unaligned_be32((u8 *)rb + AFFS_OFF_TYPE) !=
			    AFFS_T_HEADER)
				continue;
			if (affs_be32_rel(rb, AFFS_OFFR_SECTYPE) != AFFS_ST_ROOT)
				continue;
			if (get_unaligned_be32((u8 *)rb + AFFS_OFF_HTSIZE) != ht)
				continue;

			/* Found a valid root block for this block size. */
			affs_root_block = c;
			free(rb);
			return 0;
		}

		free(rb);
		affs_bsize = 0;
	}

err:
	affs_blk_desc = NULL;
	affs_bsize = 0;
	return -1;
}

int affs_size(const char *filename, loff_t *size)
{
	void *hbuf;
	u32 hdr;
	int st, ret = 0;

	*size = 0;

	hdr = affs_find(filename);
	if (!hdr)
		return -ENOENT;

	hbuf = malloc(affs_bsize);
	if (!hbuf)
		return -ENOMEM;
	if (!affs_read_block(hdr, hbuf)) {
		free(hbuf);
		return -EIO;
	}

	st = affs_be32_rel(hbuf, AFFS_OFFR_SECTYPE);
	if (st == AFFS_ST_USERDIR || st == AFFS_ST_ROOT)
		ret = -EISDIR;
	else
		*size = get_unaligned_be32((u8 *)hbuf + affs_bsize -
					   AFFS_OFFR_BYTESIZE);

	free(hbuf);
	return ret;
}

int affs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	      loff_t *actread)
{
	void *hbuf, *extbuf, *dbuf, *cur;
	u32 hdr, byte_size, payload, hdroff;
	u64 want, filepos = 0, copied = 0;
	int st, done = 0, ret = 0;

	*actread = 0;

	hdr = affs_find(filename);
	if (!hdr)
		return -ENOENT;

	hbuf = malloc(affs_bsize);
	extbuf = malloc(affs_bsize);
	dbuf = malloc(affs_bsize);
	if (!hbuf || !extbuf || !dbuf) {
		ret = -ENOMEM;
		goto out;
	}

	if (!affs_read_block(hdr, hbuf)) {
		ret = -EIO;
		goto out;
	}

	st = affs_be32_rel(hbuf, AFFS_OFFR_SECTYPE);
	if (st == AFFS_ST_USERDIR || st == AFFS_ST_ROOT) {
		ret = -EISDIR;
		goto out;
	}
	if (st != AFFS_ST_FILE) {
		/* Soft/hard links are not followed by this read-only driver. */
		ret = -EINVAL;
		goto out;
	}

	byte_size = get_unaligned_be32((u8 *)hbuf + affs_bsize -
				       AFFS_OFFR_BYTESIZE);

	if ((u64)offset >= byte_size)
		goto out;	/* nothing to read; copied == 0 */

	want = byte_size - (u64)offset;
	if (len && (u64)len < want)
		want = (u64)len;

	payload = affs_ffs ? affs_bsize : (affs_bsize - AFFS_OFS_DATA_HDR);
	hdroff = affs_ffs ? 0 : AFFS_OFS_DATA_HDR;

	/*
	 * Walk the data-block pointer table of the file header, then of each
	 * file-extension block in the chain.  In each block the table holds up
	 * to BSIZE/4-56 longs written backwards from BSIZE-204, so table[i]
	 * (data block i) lives at BSIZE-204 - i*4.  Read only the data blocks
	 * that overlap the requested [offset, offset+want) window.
	 */
	cur = hbuf;
	while (!done) {
		u32 count = get_unaligned_be32((u8 *)cur + AFFS_OFF_HIGHSEQ);
		u32 ext;
		u32 i;

		if (count > affs_ht_size)
			count = affs_ht_size;

		for (i = 0; i < count; i++) {
			u32 dblk = get_unaligned_be32((u8 *)cur + affs_bsize -
						      AFFS_OFFR_DATATAB - i * 4);
			u64 blk_actual, blk_start, blk_end, cfrom, cend;

			if (!dblk || filepos >= byte_size) {
				done = 1;
				break;
			}

			blk_actual = (byte_size - filepos < payload) ?
				     (byte_size - filepos) : payload;
			blk_start = filepos;
			blk_end = filepos + blk_actual;
			cfrom = ((u64)offset > blk_start) ?
				(u64)offset : blk_start;
			cend = ((u64)offset + want < blk_end) ?
			       (u64)offset + want : blk_end;

			if (cend > cfrom) {
				if (!affs_read_block(dblk, dbuf)) {
					ret = -EIO;
					goto out;
				}
				memcpy((u8 *)buf + (cfrom - (u64)offset),
				       (u8 *)dbuf + hdroff + (cfrom - blk_start),
				       (size_t)(cend - cfrom));
				copied += cend - cfrom;
			}

			filepos += blk_actual;
			if (copied >= want) {
				done = 1;
				break;
			}
		}

		if (done)
			break;

		ext = get_unaligned_be32((u8 *)cur + affs_bsize -
					 AFFS_OFFR_EXTENSION);
		if (!ext)
			break;
		if (!affs_read_block(ext, extbuf)) {
			ret = -EIO;
			goto out;
		}
		cur = extbuf;
	}

	*actread = copied;

out:
	free(hbuf);
	free(extbuf);
	free(dbuf);
	return ret;
}

int affs_exists(const char *filename)
{
	u32 dir;

	if (affs_find(filename))
		return 1;
	if (affs_resolve_dir(filename, &dir) == 0)
		return 1;
	return 0;
}

int affs_opendir(const char *filename, struct fs_dir_stream **dirsp)
{
	struct affs_dir_stream *dirs;
	u32 dir_block;

	if (affs_resolve_dir(filename, &dir_block))
		return -ENOENT;

	dirs = calloc(1, sizeof(*dirs));
	if (!dirs)
		return -ENOMEM;

	dirs->hdrbuf = malloc(affs_bsize);
	dirs->entbuf = malloc(affs_bsize);
	if (!dirs->hdrbuf || !dirs->entbuf) {
		free(dirs->hdrbuf);
		free(dirs->entbuf);
		free(dirs);
		return -ENOMEM;
	}

	if (!affs_read_block(dir_block, dirs->hdrbuf)) {
		free(dirs->hdrbuf);
		free(dirs->entbuf);
		free(dirs);
		return -EIO;
	}

	dirs->dir_block = dir_block;
	dirs->slot = 0;
	dirs->chain = 0;

	*dirsp = (struct fs_dir_stream *)dirs;
	return 0;
}

int affs_readdir(struct fs_dir_stream *fs_dirs, struct fs_dirent **dentp)
{
	struct affs_dir_stream *dirs = (struct affs_dir_stream *)fs_dirs;
	struct fs_dirent *dent = &dirs->dent;
	int st;

	/* Find the next entry: continue the current chain, else next slot. */
	while (dirs->chain == 0) {
		if (dirs->slot >= affs_ht_size) {
			*dentp = NULL;
			return -ENOENT;
		}
		dirs->chain = get_unaligned_be32((u8 *)dirs->hdrbuf +
						 AFFS_OFF_HASHTABLE +
						 dirs->slot * 4);
		dirs->slot++;
	}

	if (!affs_read_block(dirs->chain, dirs->entbuf)) {
		*dentp = NULL;
		return -EIO;
	}

	affs_entry_name(dirs->entbuf, dent->name, sizeof(dent->name));
	st = affs_be32_rel(dirs->entbuf, AFFS_OFFR_SECTYPE);

	if (st == AFFS_ST_USERDIR || st == AFFS_ST_ROOT) {
		dent->type = FS_DT_DIR;
		dent->size = 0;
	} else if (st == AFFS_ST_SOFTLINK || st == AFFS_ST_LINKDIR ||
		   st == AFFS_ST_LINKFILE) {
		dent->type = FS_DT_LNK;
		dent->size = 0;
	} else {
		dent->type = FS_DT_REG;
		dent->size = get_unaligned_be32((u8 *)dirs->entbuf +
						affs_bsize - AFFS_OFFR_BYTESIZE);
	}

	/* Advance to the next entry in this hash chain. */
	dirs->chain = affs_be32_rel(dirs->entbuf, AFFS_OFFR_HASHCHAIN);

	*dentp = dent;
	return 0;
}

void affs_closedir(struct fs_dir_stream *fs_dirs)
{
	struct affs_dir_stream *dirs = (struct affs_dir_stream *)fs_dirs;

	if (!dirs)
		return;

	free(dirs->hdrbuf);
	free(dirs->entbuf);
	free(dirs);
}

void affs_close(void)
{
	affs_blk_desc = NULL;
	affs_bsize = 0;
}
