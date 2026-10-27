// SPDX-License-Identifier: GPL-2.0+
/*
 * ngfs.c: Amiga "New Generation File System" (NGFS, AmigaOS 4) read-only
 * filesystem driver.
 *
 * Plugs into U-Boot's generic filesystem framework (fstype/ls/load/size) so
 * the AmigaOne X5000 and A1222 ports can read files from NGFS Amiga RDB
 * partitions.
 *
 * NGFS and SFS are unrelated filesystems.  This reader is written from the
 * on-disk format description in docs/ngfs-format.md and checked against real
 * NGFS volumes.  NGFS is a 64-bit filesystem: every disk address (block,
 * sector, extent) is a u64.
 *
 * On-disk model (v1):
 *   - Signature block (NGFS block 0): magic "NGF" + version byte, a sum-of-
 *     longwords == 0 checksum, and an ASCII "Amiga Partition:" key=value config
 *     that gives SectorsPerBlock (block_size = spb * 512) and Reserved (root
 *     block number, default 2).  Read as one 512-byte sector during probe.
 *   - Root super block (NGFS block 'Reserved'): magic 0x58204F52 and the same
 *     longword checksum, taken over the full block_size; carries the u64
 *     root_dir_addr.
 *   - Every file and directory is an "object".  A lightweight node record
 *     (own_block, type, name, data_size) appears packed in its parent
 *     directory's data; the object's full block (at own_block) also carries an
 *     extent map.
 *   - Directories: read the object's data via its extent map, then iterate it
 *     as packed node records (stride rec_size, default 0xB0).
 *   - Files: the object's extent-mapped data is the file content.
 *
 * v0 (NGF\0) directories are type-2/13 container blocks of u64 child
 * addresses.  That path is implemented behind the version check but has not
 * been tested on a real volume; only NGF\1 has been tested.
 *
 * Only the generic U-Boot fstype_info plumbing is shared with the sibling
 * fat/ext4/squashfs/sfs/affs drivers: block I/O via fs_devread()
 * (fs/fs_internal.c) and the opendir/readdir/closedir + fs_ls_generic pattern.
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
#include <ngfs.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/string.h>

/* Device / partition stashed by ngfs_probe(), used by all reads. */
static struct blk_desc *ngfs_blk_desc;
static struct disk_partition ngfs_part_info;

/* Geometry learned from the NGFS signature + root blocks. */
static u32 ngfs_block_size;		/* bytes per NGFS block = spb * 512 */
static u32 ngfs_spb;			/* sectors (512 B) per NGFS block */
static u64 ngfs_reserved_block;		/* root super block NGFS block number */
static u64 ngfs_root_dir_addr;		/* 64-bit block addr of the root-dir node */
static int ngfs_version;		/* 0 = NGF\0, 1 = NGF\1 */

/* One decoded directory entry, as collected by ngfs_enum_dir(). */
struct ngfs_entry {
	char name[NGFS_NAME_MAX + 1];
	u64 ref;			/* the object's own_block (full block addr) */
	u64 size;			/* data_size from the record (files) */
	int is_dir;
};

/* Directory stream handed back to the fs layer; fs_dir_stream must be first. */
struct ngfs_dir_stream {
	struct fs_dir_stream fs_dirs;
	struct ngfs_entry *ents;	/* all entries, collected at opendir() */
	u32 count;
	u32 index;
	struct fs_dirent dent;		/* entry handed back by ngfs_readdir() */
};

/* -------------------------------------------------------------------------
 * Low-level block I/O: NGFS 64-bit block -> device (sector, byte_offset).
 *
 * The on-disk mapping is
 *   disk_sector = ngfs_block * SectorsPerBlock + first_sector
 * and fs_devread() already biases by partition->start (= first_sector), so the
 * driver must not add first_sector itself.  fs_devread() takes a sector in
 * device-block (desc->blksz) units plus a byte offset, so the partition-
 * relative byte position (ngfs_block * block_size) is split into a device
 * sector and an intra-sector byte offset.  On real hardware desc->blksz == 512
 * and this reduces exactly to sector = ngfs_block * SectorsPerBlock, offset 0.
 * ------------------------------------------------------------------------- */
/* Read 'count' consecutive NGFS blocks starting at 'block' into buf. */
static int ngfs_read_blocks(u64 block, u32 count, void *buf)
{
	u64 byte_pos;
	lbaint_t sector;
	int byte_offset;

	if (!ngfs_blk_desc || !ngfs_block_size)
		return 0;

	byte_pos = block * (u64)ngfs_block_size;
	sector = (lbaint_t)(byte_pos / ngfs_blk_desc->blksz);
	byte_offset = (int)(byte_pos - (u64)sector * ngfs_blk_desc->blksz);

	return fs_devread(ngfs_blk_desc, &ngfs_part_info, sector, byte_offset,
			  (int)(ngfs_block_size * count), (char *)buf);
}

static int ngfs_read_block(u64 block, void *buf)
{
	return ngfs_read_blocks(block, 1, buf);
}

/*
 * NGFS sig/root checksum: the block is valid when the sum of its big-endian u32
 * longwords is 0 (accumulator init 0).  The sig block is summed over its single
 * 512-byte probe sector; the root super block is summed over the full
 * block_size.
 */
static int ngfs_checksum_ok(const void *block, u32 len)
{
	const u8 *p = block;
	u32 sum = 0;
	u32 i;

	for (i = 0; i + 4 <= len; i += 4)
		sum += get_unaligned_be32(p + i);

	return sum == 0;
}

/* ASCII upper-case fold for case-insensitive name comparison. */
static int ngfs_toupper(int c)
{
	c &= 0xff;
	if (c >= 'a' && c <= 'z')
		return c - ('a' - 'A');
	return c;
}

static int ngfs_name_eq(const char *a, const char *b)
{
	while (*a && *b) {
		if (ngfs_toupper((u8)*a) != ngfs_toupper((u8)*b))
			return 0;
		a++;
		b++;
	}
	return *a == '\0' && *b == '\0';
}

/* -------------------------------------------------------------------------
 * Signature-block config text parsing
 * ------------------------------------------------------------------------- */

/*
 * Scan the ASCII config (NUL/end-bounded) for a "<key> ... = <number>" line and
 * return the parsed unsigned value in *out.  Returns 1 if found.  The key text
 * is matched at a line start so "SectorsPerBlock" is not confused with
 * "SectorSize".
 */
static int ngfs_cfg_u64(const char *text, u32 textlen, const char *key, u64 *out)
{
	u32 keylen = (u32)strlen(key);
	u32 i = 0;

	while (i < textlen && text[i] != '\0') {
		u32 ls = i;		/* line start */
		u32 le = ls;		/* line end */

		while (le < textlen && text[le] != '\0' && text[le] != '\n' &&
		       text[le] != '\r')
			le++;

		/* Trim leading blanks in the line. */
		while (ls < le && (text[ls] == ' ' || text[ls] == '\t'))
			ls++;

		if (ls + keylen <= le &&
		    memcmp(text + ls, key, keylen) == 0) {
			u32 p = ls + keylen;
			int seen_eq = 0;
			u64 v = 0;
			int seen_digit = 0;

			while (p < le && text[p] != '=')
				p++;
			if (p < le && text[p] == '=') {
				seen_eq = 1;
				p++;
			}
			while (p < le && (text[p] == ' ' || text[p] == '\t'))
				p++;
			while (p < le && text[p] >= '0' && text[p] <= '9') {
				v = v * 10 + (u64)(text[p] - '0');
				seen_digit = 1;
				p++;
			}
			if (seen_eq && seen_digit) {
				*out = v;
				return 1;
			}
		}

		/* Advance to the next line. */
		i = le;
		while (i < textlen && (text[i] == '\n' || text[i] == '\r'))
			i++;
		if (i == ls)		/* no progress guard */
			i = le + 1;
	}
	return 0;
}

/* -------------------------------------------------------------------------
 * Extent map (v1 and v0 share the same object extent layout)
 * ------------------------------------------------------------------------- */

/*
 * Gather the inline extents from an object's full block.  addr[] (u64, stride
 * 8) is at +0x1B0 and len[] (u32, stride 4) follows it at
 * +0x1B0 + capacity * 8.  Returns the number gathered.
 */
static int ngfs_gather_extents(const void *full, struct ngfs_extent *ext,
			       int maxext)
{
	u32 cnt = get_unaligned_be16((const u8 *)full + NGFS_EXT_COUNT_OFF);
	u32 cap = get_unaligned_be16((const u8 *)full + NGFS_EXT_CAP_OFF);
	u32 fit, keybase, n, i;

	/* Each extent consumes 8 bytes of addr[] + 4 bytes of len[]. */
	fit = (ngfs_block_size > NGFS_EXT_ADDR_OFF) ?
	      (ngfs_block_size - NGFS_EXT_ADDR_OFF) / (8 + 4) : 0;

	n = cnt;
	if (n > fit)
		n = fit;
	if (n > (u32)maxext)
		n = (u32)maxext;

	/*
	 * len[] follows the whole addr[] array, whose dimension is the capacity
	 * u16 at +0x194, not the active count at +0x196.  Using the count here
	 * would read from the unused middle of addr[] and mis-map every block of
	 * a multi-extent object (the root directory included).  cap >= cnt on a
	 * valid object; an out-of-range slot reads as a 0 terminator length.
	 */
	keybase = NGFS_EXT_ADDR_OFF + cap * 8;

	for (i = 0; i < n; i++) {
		u32 koff = keybase + i * 4;

		ext[i].addr = get_unaligned_be64((const u8 *)full +
						 NGFS_EXT_ADDR_OFF + (u64)i * 8);
		if (koff + 4 <= ngfs_block_size)
			ext[i].len = get_unaligned_be32((const u8 *)full + koff);
		else
			ext[i].len = 0;	/* out-of-range slot: terminator length */
	}
	return (int)n;
}

/*
 * Map file block 'fblk' to a disk block.  len[i] is the length (block count) of
 * extent i; extent i covers file blocks [cum_i, cum_i + len[i]) where cum_i is
 * the sum of the prior lengths, mapped contiguously from addr[i].  For example,
 * with 2048-byte blocks, len[] = {2, 2}, addr[] = {1000, 5000} and data_size
 * 0x1600 (3 blocks): fblk 0 -> 1000, 1 -> 1001, 2 -> 5000.  Returns 1 and fills
 * *disk, or 0 if the block is not mapped.
 */
static int ngfs_map_fileblock(const struct ngfs_extent *ext, int nextent,
			      u64 total_blocks, u64 fblk, u64 *disk)
{
	u64 cum = 0;
	int i;

	for (i = 0; i < nextent; i++) {
		u64 len = ext[i].len;

		if (len == 0)		/* zero-length = terminator / unused slot */
			break;
		if (fblk >= cum && fblk < cum + len) {
			*disk = ext[i].addr + (fblk - cum);
			return 1;
		}
		cum += len;
	}
	/* Defensive: a single under-counted extent maps contiguously. */
	if (nextent == 1 && fblk < total_blocks) {
		*disk = ext[0].addr + fblk;
		return 1;
	}
	return 0;
}

/*
 * Read an object's full block, given a reference address that is either the
 * object's node record (own_block field points to the full block) or the full
 * block itself (own_block field points to itself).  Returns 0 and an allocated
 * block in *out (caller frees).
 */
static int ngfs_read_full_object(u64 ref, void **out)
{
	void *buf;
	u64 own;

	buf = malloc(ngfs_block_size);
	if (!buf)
		return -ENOMEM;

	if (!ngfs_read_block(ref, buf)) {
		free(buf);
		return -EIO;
	}

	own = get_unaligned_be64((u8 *)buf + NGFS_OBJ_OWN_OFF);
	if (own != ref) {
		if (!ngfs_read_block(own, buf)) {
			free(buf);
			return -EIO;
		}
	}

	*out = buf;
	return 0;
}

/*
 * Read a byte range [offset, offset+want) of an object's extent-mapped data
 * into 'dest'.  'full' is the object's full block.  Returns 0 and the number of
 * bytes copied in *actread.
 */
static int ngfs_read_object_range(const void *full, void *dest, u64 offset,
				  u64 len, u64 *actread)
{
	struct ngfs_extent *ext;
	void *datbuf;
	u64 data_size, want, total_blocks, filepos, copied = 0;
	int nextent, ret = 0;

	*actread = 0;

	data_size = get_unaligned_be64((const u8 *)full + NGFS_OBJ_DATASIZE_OFF);
	if (offset >= data_size)
		return 0;

	want = data_size - offset;
	if (len && len < want)
		want = len;

	ext = malloc(sizeof(*ext) * NGFS_MAX_EXTENTS);
	datbuf = malloc(ngfs_block_size);
	if (!ext || !datbuf) {
		ret = -ENOMEM;
		goto out;
	}

	nextent = ngfs_gather_extents(full, ext, NGFS_MAX_EXTENTS);
	if (nextent <= 0) {
		ret = -EIO;
		goto out;
	}

	total_blocks = (data_size + ngfs_block_size - 1) / ngfs_block_size;
	filepos = offset;

	while (copied < want) {
		u64 fblk = filepos / ngfs_block_size;
		u32 blk_off = (u32)(filepos - fblk * ngfs_block_size);
		u32 chunk = ngfs_block_size - blk_off;
		u64 disk;

		if ((u64)chunk > want - copied)
			chunk = (u32)(want - copied);

		if (!ngfs_map_fileblock(ext, nextent, total_blocks, fblk,
					&disk)) {
			ret = -EIO;
			goto out;
		}

		/*
		 * Whole blocks: read a run of disk-contiguous blocks straight
		 * into dest with one request.  Reading one block per request
		 * is several times slower for large files such as a kickstart.
		 */
		if (blk_off == 0 && want - copied >= ngfs_block_size) {
			u32 n = 1, max = NGFS_MAX_RUN;
			u64 next;

			if ((want - copied) / ngfs_block_size < max)
				max = (want - copied) / ngfs_block_size;
			while (n < max &&
			       ngfs_map_fileblock(ext, nextent, total_blocks,
						  fblk + n, &next) &&
			       next == disk + n)
				n++;
			if (!ngfs_read_blocks(disk, n, (u8 *)dest + copied)) {
				ret = -EIO;
				goto out;
			}
			copied += (u64)n * ngfs_block_size;
			filepos += (u64)n * ngfs_block_size;
			continue;
		}

		if (!ngfs_read_block(disk, datbuf)) {
			ret = -EIO;
			goto out;
		}

		memcpy((u8 *)dest + copied, (u8 *)datbuf + blk_off, chunk);
		copied += chunk;
		filepos += chunk;
	}

	*actread = copied;
out:
	free(ext);
	free(datbuf);
	return ret;
}

/*
 * Assemble an object's whole extent-mapped data (used for directory data).
 * Returns 0 and an allocated buffer (caller frees) of *size bytes; *data may be
 * NULL with *size 0 for an empty object.
 */
static int ngfs_load_object_data(u64 ref, u8 **data, u64 *size)
{
	void *full;
	u8 *buf;
	u64 data_size, got;
	int ret;

	*data = NULL;
	*size = 0;

	ret = ngfs_read_full_object(ref, &full);
	if (ret)
		return ret;

	data_size = get_unaligned_be64((u8 *)full + NGFS_OBJ_DATASIZE_OFF);
	if (data_size == 0) {
		free(full);
		return 0;
	}
	if (data_size > NGFS_MAX_DIR_DATA)
		data_size = NGFS_MAX_DIR_DATA;

	buf = malloc((size_t)data_size);
	if (!buf) {
		free(full);
		return -ENOMEM;
	}

	ret = ngfs_read_object_range(full, buf, 0, data_size, &got);
	free(full);
	if (ret) {
		free(buf);
		return ret;
	}

	*data = buf;
	*size = got;
	return 0;
}

/* -------------------------------------------------------------------------
 * Directory enumeration (collects all entries into a malloc'd array)
 * ------------------------------------------------------------------------- */

static int ngfs_entries_push(struct ngfs_entry **arr, u32 *count, u32 *cap,
			     const struct ngfs_entry *e)
{
	if (*count >= *cap) {
		u32 ncap = *cap ? *cap * 2 : 32;
		struct ngfs_entry *na;

		if (ncap > NGFS_MAX_DIR_ENTRIES)
			ncap = NGFS_MAX_DIR_ENTRIES;
		if (*count >= ncap)
			return 0;	/* silently cap pathological dirs */
		na = realloc(*arr, ncap * sizeof(**arr));
		if (!na)
			return -ENOMEM;
		*arr = na;
		*cap = ncap;
	}
	(*arr)[(*count)++] = *e;
	return 0;
}

/* v1: parse the assembled packed node-record stream into entries. */
static int ngfs_enum_v1(u64 dir_ref, struct ngfs_entry **arr, u32 *count)
{
	u8 *data;
	u64 size, off = 0;
	u32 cap = 0;
	int ret;

	*arr = NULL;
	*count = 0;

	ret = ngfs_load_object_data(dir_ref, &data, &size);
	if (ret)
		return ret;
	if (!data)
		return 0;	/* empty directory */

	while (off + NGFS_REC_MIN <= size) {
		u8 *rec = data + off;
		u32 magic = get_unaligned_be32(rec + NGFS_OBJ_MAGIC_OFF);
		u32 rec_size = get_unaligned_be16(rec + NGFS_OBJ_RECSIZE_OFF);
		u32 stride;

		if (rec_size < NGFS_REC_MIN || rec_size > ngfs_block_size)
			stride = NGFS_DIR_REC_STRIDE;
		else
			stride = rec_size;

		if (magic == NGFS_OBJ_MAGIC) {
			struct ngfs_entry e;
			u32 nlen = get_unaligned_be16(rec + NGFS_OBJ_NAMELEN_OFF);
			u16 type = get_unaligned_be16(rec + NGFS_OBJ_TYPE_OFF);
			u32 maxn;

			/* Bound the name to the record and the data buffer. */
			maxn = (stride > NGFS_OBJ_NAME_OFF) ?
			       stride - NGFS_OBJ_NAME_OFF : 0;
			if (off + NGFS_OBJ_NAME_OFF + nlen > size)
				nlen = (u32)(size - off - NGFS_OBJ_NAME_OFF);
			if (nlen > maxn)
				nlen = maxn;
			if (nlen > NGFS_NAME_MAX)
				nlen = NGFS_NAME_MAX;

			memcpy(e.name, rec + NGFS_OBJ_NAME_OFF, nlen);
			e.name[nlen] = '\0';
			e.ref = get_unaligned_be64(rec + NGFS_OBJ_OWN_OFF);
			e.size = get_unaligned_be64(rec + NGFS_OBJ_DATASIZE_OFF);
			e.is_dir = NGFS_IS_DIR_TYPE(type);

			if (nlen > 0) {
				ret = ngfs_entries_push(arr, count, &cap, &e);
				if (ret)
					goto out;
			}
		}

		off += stride;
	}

out:
	free(data);
	if (ret) {
		free(*arr);
		*arr = NULL;
		*count = 0;
	}
	return ret;
}

/*
 * v0: walk type-2/13 container blocks of u64 child addresses.  Not yet
 * tested on a real volume.
 */
static int ngfs_enum_v0(u64 cont_addr, struct ngfs_entry **arr, u32 *count)
{
	void *cbuf, *obuf;
	u32 cap = 0, chain = 0;
	u64 cont = cont_addr;
	int ret = 0;

	*arr = NULL;
	*count = 0;

	cbuf = malloc(ngfs_block_size);
	obuf = malloc(ngfs_block_size);
	if (!cbuf || !obuf) {
		ret = -ENOMEM;
		goto out;
	}

	while (cont != 0 && chain++ < NGFS_MAX_CONT_CHAIN) {
		u16 type;
		u32 cnt, array_off, maxn, i;
		u64 next = 0;

		if (!ngfs_read_block(cont, cbuf)) {
			ret = -EIO;
			goto out;
		}

		type = get_unaligned_be16((u8 *)cbuf + NGFS_V0_TYPE_OFF);
		if (type == NGFS_V0_TYPE2) {
			cnt = get_unaligned_be16((u8 *)cbuf +
						 NGFS_V0_T2_COUNT_OFF);
			array_off = NGFS_V0_T2_ARRAY_OFF;
		} else if (type == NGFS_V0_TYPE13) {
			cnt = get_unaligned_be16((u8 *)cbuf +
						 NGFS_V0_T13_COUNT_OFF);
			array_off = NGFS_V0_T13_ARRAY_OFF;
			next = get_unaligned_be64((u8 *)cbuf +
						  NGFS_V0_T13_CHAIN_OFF);
		} else {
			break;		/* not a container */
		}

		if (array_off >= ngfs_block_size)
			break;
		maxn = (ngfs_block_size - array_off) / NGFS_CHILD_ENTRY_SIZE;
		if (cnt > maxn)
			cnt = maxn;

		for (i = 0; i < cnt; i++) {
			u64 child = get_unaligned_be64((u8 *)cbuf + array_off +
						       (u64)i *
						       NGFS_CHILD_ENTRY_SIZE);
			struct ngfs_entry e;
			u32 nlen;
			u16 otype;

			if (child == 0)
				continue;
			if (!ngfs_read_block(child, obuf)) {
				ret = -EIO;
				goto out;
			}

			nlen = get_unaligned_be16((u8 *)obuf +
						  NGFS_V0_NAMELEN_OFF);
			if (nlen > NGFS_NAME_MAX)
				nlen = NGFS_NAME_MAX;
			if (NGFS_V0_NAME_OFF + nlen > ngfs_block_size)
				nlen = ngfs_block_size - NGFS_V0_NAME_OFF;

			memcpy(e.name, (u8 *)obuf + NGFS_V0_NAME_OFF, nlen);
			e.name[nlen] = '\0';
			otype = get_unaligned_be16((u8 *)obuf +
						   NGFS_OBJ_TYPE_OFF);
			e.ref = child;
			e.size = get_unaligned_be64((u8 *)obuf +
						    NGFS_OBJ_DATASIZE_OFF);
			e.is_dir = NGFS_IS_DIR_TYPE(otype);

			if (nlen > 0) {
				ret = ngfs_entries_push(arr, count, &cap, &e);
				if (ret)
					goto out;
			}
		}

		cont = next;
	}

out:
	free(cbuf);
	free(obuf);
	if (ret) {
		free(*arr);
		*arr = NULL;
		*count = 0;
	}
	return ret;
}

/* Enumerate a directory (given its reference address) into an entry array. */
static int ngfs_enum_dir(u64 dir_ref, struct ngfs_entry **arr, u32 *count)
{
	if (ngfs_version == 0)
		return ngfs_enum_v0(dir_ref, arr, count);
	return ngfs_enum_v1(dir_ref, arr, count);
}

/* -------------------------------------------------------------------------
 * Path resolution
 * ------------------------------------------------------------------------- */

/*
 * Find 'name' in the directory at reference 'dir_ref'.  On success returns 1
 * and fills *obj_ref (the entry's own_block) and *is_dir; 0 if not found,
 * negative errno on error.
 */
static int ngfs_lookup(u64 dir_ref, const char *name, u64 *obj_ref, int *is_dir)
{
	struct ngfs_entry *arr;
	u32 count, i;
	int ret;

	ret = ngfs_enum_dir(dir_ref, &arr, &count);
	if (ret)
		return ret;

	for (i = 0; i < count; i++) {
		if (ngfs_name_eq(arr[i].name, name)) {
			*obj_ref = arr[i].ref;
			*is_dir = arr[i].is_dir;
			free(arr);
			return 1;
		}
	}

	free(arr);
	return 0;
}

/*
 * Resolve a directory path to its reference address (root_dir_addr for "" /
 * "/", else the matched directory's own_block).  Returns 0 on success.
 */
static int ngfs_resolve_dir(const char *path, u64 *out_ref)
{
	u64 cur = ngfs_root_dir_addr;
	char *dup, *p, *slash;
	int ret = 0;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;

	p = dup;
	while (*p == '/')
		p++;

	while (*p) {
		u64 obj;
		int is_dir, found;

		slash = strchr(p, '/');
		if (slash)
			*slash = '\0';

		if (*p) {
			found = ngfs_lookup(cur, p, &obj, &is_dir);
			if (found < 0) {
				ret = found;
				goto out;
			}
			if (!found) {
				ret = -ENOENT;
				goto out;
			}
			if (!is_dir) {
				ret = -ENOTDIR;
				goto out;
			}
			cur = obj;
		}

		if (!slash)
			break;
		p = slash + 1;
	}

	*out_ref = cur;
out:
	free(dup);
	return ret;
}

/*
 * Resolve a full path to its final object (file or directory).  Returns 1 and
 * fills *obj_ref / *is_dir on success, 0 if not found, negative errno on error.
 */
static int ngfs_find(const char *path, u64 *obj_ref, int *is_dir)
{
	char *dup, *file, *slash;
	u64 dir_ref;
	int ret;

	dup = strdup(path);
	if (!dup)
		return -ENOMEM;

	slash = strrchr(dup, '/');
	if (slash) {
		*slash = '\0';
		file = slash + 1;
		ret = ngfs_resolve_dir(dup, &dir_ref);
		if (ret) {
			free(dup);
			return ret;
		}
	} else {
		file = dup;
		dir_ref = ngfs_root_dir_addr;
	}

	if (*file == '\0') {
		free(dup);
		return 0;	/* path was a directory, no final object */
	}

	ret = ngfs_lookup(dir_ref, file, obj_ref, is_dir);
	free(dup);
	return ret;
}

/* -------------------------------------------------------------------------
 * Probe
 * ------------------------------------------------------------------------- */

/*
 * Identify an NGFS partition from its signature block and load the geometry
 * and root directory address used by every other operation.  Returns 0 if the
 * partition is NGFS, -1 otherwise.
 */
int ngfs_probe(struct blk_desc *fs_dev_desc, struct disk_partition *fs_partition)
{
	void *sig = NULL, *root = NULL;
	u32 sigmagic;
	u64 spb = 0, reserved = NGFS_ROOT_DEFAULT_BLOCK;
	u32 magic_off, rootdir_off;

	ngfs_blk_desc = fs_dev_desc;
	ngfs_part_info = *fs_partition;
	ngfs_block_size = 0;

	if (!fs_dev_desc || !fs_dev_desc->blksz)
		goto err;

	/*
	 * Do not reject on fs_partition->type here.  disk/part_amiga.c reports
	 * type "U-Boot" for every Amiga RDB partition (so they pass the type
	 * check in disk/part.c), so the type string is never "NGF\x".
	 * The signature-block magic below is the authoritative NGFS identifier.
	 */

	/*
	 * Signature block = NGFS block 0.  SectorsPerBlock is not known yet, so
	 * read exactly one 512-byte sector at partition-relative sector 0.
	 */
	sig = malloc(NGFS_PROBE_SECTOR_BYTES);
	if (!sig)
		goto err;
	if (!fs_devread(fs_dev_desc, &ngfs_part_info, 0, 0,
			NGFS_PROBE_SECTOR_BYTES, (char *)sig))
		goto err;

	sigmagic = get_unaligned_be32((u8 *)sig);
	if ((sigmagic & NGFS_SIG_MAGIC_MASK) != NGFS_SIG_MAGIC)
		goto err;
	ngfs_version = (int)(sigmagic & 0xff);

	if (!ngfs_checksum_ok(sig, NGFS_PROBE_SECTOR_BYTES))
		goto err;

	/* ASCII config must begin "Amiga Partition:". */
	if (memcmp((u8 *)sig + NGFS_SIG_TEXT_OFF, NGFS_SIG_TEXT_MAGIC,
		   strlen(NGFS_SIG_TEXT_MAGIC)) != 0)
		goto err;

	if (!ngfs_cfg_u64((const char *)sig + NGFS_SIG_TEXT_OFF,
			  NGFS_PROBE_SECTOR_BYTES - NGFS_SIG_TEXT_OFF,
			  NGFS_KEY_SPB, &spb))
		goto err;
	if (spb == 0)
		goto err;
	(void)ngfs_cfg_u64((const char *)sig + NGFS_SIG_TEXT_OFF,
			   NGFS_PROBE_SECTOR_BYTES - NGFS_SIG_TEXT_OFF,
			   NGFS_KEY_RESERVED, &reserved);

	ngfs_spb = (u32)spb;
	ngfs_block_size = (u32)spb * NGFS_SECTOR_BYTES;
	ngfs_reserved_block = reserved;

	if (ngfs_block_size < NGFS_MIN_BLOCK_SIZE ||
	    ngfs_block_size > NGFS_MAX_BLOCK_SIZE ||
	    (ngfs_block_size % fs_dev_desc->blksz) != 0)
		goto err;

	/* Root super block = NGFS block 'Reserved'. */
	root = malloc(ngfs_block_size);
	if (!root)
		goto err;
	if (!ngfs_read_block(reserved, root))
		goto err;

	if (ngfs_version == 0) {
		magic_off = NGFS_ROOT_MAGIC_OFF_V0;
		rootdir_off = NGFS_ROOT_ROOTDIR_OFF_V0;
	} else {
		magic_off = NGFS_ROOT_MAGIC_OFF_V1;
		rootdir_off = NGFS_ROOT_ROOTDIR_OFF_V1;
	}

	if (magic_off + 4 > ngfs_block_size ||
	    rootdir_off + 8 > ngfs_block_size)
		goto err;
	if (get_unaligned_be32((u8 *)root + magic_off) != NGFS_ROOT_MAGIC)
		goto err;
	/*
	 * Checksum the whole root block (block_size): the longword sum is zero
	 * over the full block (the +0x10 longword balances it; a 512-byte sum is
	 * nonzero).
	 */
	if (!ngfs_checksum_ok(root, ngfs_block_size))
		goto err;

	ngfs_root_dir_addr = get_unaligned_be64((u8 *)root + rootdir_off);

	free(sig);
	free(root);
	return 0;

err:
	free(sig);
	free(root);
	ngfs_blk_desc = NULL;
	ngfs_block_size = 0;
	return -1;
}

/* -------------------------------------------------------------------------
 * File operations
 * ------------------------------------------------------------------------- */

int ngfs_read(const char *filename, void *buf, loff_t offset, loff_t len,
	      loff_t *actread)
{
	void *full = NULL;
	u64 obj_ref, copied = 0;
	int is_dir, nfound, ret = 0;

	*actread = 0;

	nfound = ngfs_find(filename, &obj_ref, &is_dir);
	if (nfound < 0)
		return nfound;
	if (!nfound)
		return -ENOENT;
	if (is_dir)
		return -EISDIR;

	ret = ngfs_read_full_object(obj_ref, &full);
	if (ret)
		return ret;

	ret = ngfs_read_object_range(full, buf, (u64)offset, (u64)len, &copied);
	free(full);
	if (ret)
		return ret;

	*actread = (loff_t)copied;
	return 0;
}

int ngfs_size(const char *filename, loff_t *size)
{
	void *full = NULL;
	u64 obj_ref;
	int is_dir, nfound, ret;

	*size = 0;

	nfound = ngfs_find(filename, &obj_ref, &is_dir);
	if (nfound < 0)
		return nfound;
	if (!nfound)
		return -ENOENT;
	if (is_dir)
		return -EISDIR;

	ret = ngfs_read_full_object(obj_ref, &full);
	if (ret)
		return ret;

	*size = (loff_t)get_unaligned_be64((u8 *)full + NGFS_OBJ_DATASIZE_OFF);
	free(full);
	return 0;
}

int ngfs_exists(const char *filename)
{
	u64 obj_ref, dir_ref;
	int is_dir;

	if (ngfs_find(filename, &obj_ref, &is_dir) == 1)
		return 1;
	if (ngfs_resolve_dir(filename, &dir_ref) == 0)
		return 1;	/* a directory path (incl. "" / "/") */
	return 0;
}

/* -------------------------------------------------------------------------
 * Directory streaming
 * ------------------------------------------------------------------------- */

int ngfs_opendir(const char *filename, struct fs_dir_stream **dirsp)
{
	struct ngfs_dir_stream *dirs;
	u64 dir_ref;
	int ret;

	ret = ngfs_resolve_dir(filename, &dir_ref);
	if (ret)
		return ret;

	dirs = calloc(1, sizeof(*dirs));
	if (!dirs)
		return -ENOMEM;

	ret = ngfs_enum_dir(dir_ref, &dirs->ents, &dirs->count);
	if (ret) {
		free(dirs);
		return ret;
	}

	dirs->index = 0;
	*dirsp = (struct fs_dir_stream *)dirs;
	return 0;
}

int ngfs_readdir(struct fs_dir_stream *fs_dirs, struct fs_dirent **dentp)
{
	struct ngfs_dir_stream *dirs = (struct ngfs_dir_stream *)fs_dirs;
	struct fs_dirent *dent = &dirs->dent;
	struct ngfs_entry *e;

	if (dirs->index >= dirs->count) {
		*dentp = NULL;
		return -ENOENT;
	}

	e = &dirs->ents[dirs->index++];

	strncpy(dent->name, e->name, sizeof(dent->name) - 1);
	dent->name[sizeof(dent->name) - 1] = '\0';

	if (e->is_dir) {
		dent->type = FS_DT_DIR;
		dent->size = 0;
	} else {
		dent->type = FS_DT_REG;
		dent->size = (loff_t)e->size;
	}

	*dentp = dent;
	return 0;
}

void ngfs_closedir(struct fs_dir_stream *fs_dirs)
{
	struct ngfs_dir_stream *dirs = (struct ngfs_dir_stream *)fs_dirs;

	if (!dirs)
		return;

	free(dirs->ents);
	free(dirs);
}

void ngfs_close(void)
{
	ngfs_blk_desc = NULL;
	ngfs_block_size = 0;
}
