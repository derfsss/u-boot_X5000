# NGFS on-disk format

NGFS ("New Generation File System") is an AmigaOS 4 filesystem. This document describes the parts
of its on-disk format that a read-only reader needs, as U-Boot's `fs/ngfs/` driver reads them (`include/ngfs.h` has the same offsets as constants). It covers `NGF\1` volumes,
which is what current AmigaOS 4.1 installations use, and the older `NGF\0` layout as far as it is
known.

NGFS and SFS are unrelated filesystems; the driver shares no code with `fs/sfs/`.

All values are big-endian. Offsets are in bytes from the start of the block.

## Summary

- A 64-bit filesystem: every block address is a u64.
- Block 0 is a signature block with a text configuration (block size, root block).
- The root block holds the address of the root directory.
- Every file and directory is an object. Its full block holds an extent map that locates its data.
- A directory's data is a packed list of node records, one per entry.
- Only the signature block and the root block carry checksums.

## Identifying a volume

The RDB partition's DOS type is `NGF\0` (0x4E474600) or `NGF\1` (0x4E474601); test it with
`(DosType & 0xFFFFFF00) == 0x4E474600`. The low byte is the format version (0 or 1). It is repeated
in the signature block.

## Block addressing

```
sector = ngfs_block * SectorsPerBlock + first_sector
```

`first_sector` is the partition's first 512-byte sector (`LowCyl * Surfaces * BlocksPerTrack` from
the partition's DosEnvec). One NGFS block is `SectorsPerBlock * 512` bytes; 2048 bytes
(SectorsPerBlock = 4) is typical. Before the signature block has been read the block size is not
known, so block 0 is read as a single sector at `first_sector`.

## Signature block (block 0)

Read as one 512-byte sector.

| Offset | Size | Contents |
|---|---|---|
| 0x00 | 3 | `NGF` |
| 0x03 | 1 | version: 0 or 1 |
| 0x04 | 4 | checksum balancing word |
| 0x08 | text | configuration text, starting `Amiga Partition:` |

**Checksum:** the sum of all 128 big-endian u32 words of the sector is 0 (mod 2^32).

The configuration text is a list of `key = value` lines. The keys a reader needs:

| Key | Meaning |
|---|---|
| `SectorsPerBlock` | 512-byte sectors per NGFS block. Block size = SectorsPerBlock * 512. |
| `Reserved` | the NGFS block number of the root block; 2 if absent |

Other keys (such as `Variant`) exist; `Variant` does not select the root block layout, the version
byte does.

## Root block (block `Reserved`, normally 2)

Read as one full NGFS block. The layout depends on the version:

| Field | v0 (`NGF\0`) | v1 (`NGF\1`) |
|---|---|---|
| geometry structure (0x78 bytes) | 0x158 | 0x190 |
| u64 root directory address (geometry + 0x38) | 0x190 | 0x1C8 |
| u32 root magic `0x58204F52` (geometry + 0x78) | 0x1D0 | 0x208 |

**Checksum:** the sum of all u32 words over the whole block (not just its first sector) is 0.

Fields of the geometry structure (offsets from its start):

| Offset | Size | Contents |
|---|---|---|
| +0x08 | u64 | total sectors of the partition |
| +0x18 | u64 | first sector of the partition |
| +0x20 | u64 | last sector of the partition |
| +0x38 | u64 | root directory: block address of its node record |
| +0x66 | u16 | sectors per block |
| +0x70 | u32 | block size in bytes |

The geometry structure also holds other block addresses (allocation bitmap, journal and similar)
that a reader does not need.

## Objects (v1)

Every file and directory is an object. An object appears in two forms:

- a **node record**: a short record in its parent directory's data, carrying the name, type, size
  and the address of the object's full block;
- the **full object block** at `own_block`: the same record fields followed by the extent map.

Node record fields (the same in both forms):

| Offset | Size | Contents |
|---|---|---|
| 0x00 | u64 | `own_block`: block address of this object's full block |
| 0x08 | u64 | parent: block address of the containing directory's data |
| 0x10 | u16 | type |
| 0x14 | u32 | record checksum (not needed for reading) |
| 0x20 | u32 | record magic `0x2017D583` |
| 0x24 | u16 | record size: the distance to the next record in a directory (0xB0 for directory entries) |
| 0x26 | u16 | name length |
| 0x68 | u64 | data size in bytes: the file size, or the size of a directory's entry list |
| 0x8C | bytes | name (ASCII, not terminated) |

**Type:** 0x12 is a directory and 0x01 a file; these are the only types seen on real volumes. The
AmigaOS handler tests `(type & 0xFF) == 0x12` for a directory and `(type & ~0x10) == 1` for a file.
U-Boot's driver tests `(type & ~0x10) == 2` for a directory, which gives the same result for both
known types.

**Names** are stored with their case; lookups compare them case-insensitively (ASCII upper-case
fold).

The root directory address in the root block points at a node record; its `own_block` gives the
root directory's full object block.

## Extent map

The full object block maps the object's data to disk blocks:

| Offset | Size | Contents |
|---|---|---|
| 0x18 | u64 | continuation: next extent block, for objects with more extents than fit here |
| 0x194 | u16 | capacity: the size of both arrays below |
| 0x196 | u16 | number of extents in use |
| 0x1B0 | u64 x capacity | `addr[]`: first disk block of each extent |
| 0x1B0 + capacity * 8 | u32 x capacity | `len[]`: length of each extent, in blocks |

Note that `len[]` starts after the whole `addr[]` array (sized by the capacity, not the number in
use), and that its values are lengths, not file-block boundaries. Extent *i* covers the file
blocks from `cum(i)` to `cum(i) + len[i] - 1`, where `cum(i)` is the sum of the lengths before it:

```
disk_block = addr[i] + (file_block - cum(i))
```

A zero length ends the list. With 2048-byte blocks the capacity is 134, so almost every object fits
in its own block.

Example: an object with capacity 134, two extents, `addr[] = {A, B}`, `len[] = {2, 2}` and a data
size of 3 blocks is stored in blocks A, A+1 and B.

To read an object: read its full block, gather the extents, and read `data size` bytes from the
extents in order.

## Directories (v1)

A directory's data, read through its extent map, is a packed list of node records. Each record is
checked by its magic (0x2017D583) and the next one starts `record size` bytes later. Each record
gives one entry's name, type, size and `own_block`; descend into a subdirectory, or read a file, by
reading the full object block at `own_block`.

## Directories (v0)

`NGF\0` directories are container blocks holding arrays of u64 child block addresses (zero entries
are skipped):

| Container type (u16 at 0x10) | Count (u16) | Array of u64 addresses | Next container (u64) |
|---|---|---|---|
| 2: root container | 0x15C | 0x178 | none |
| 13: sub-container | 0x30 | 0x38 | 0x18 |

Each child address is an object block with the record magic 0x2017D583 at 0x20, the type at 0x10,
the name length (u16) at 0x40 and the name at 0x42.

## Checksums

Only the signature block (over its 512-byte sector) and the root block (over the whole block) are
checksummed. Object, directory and extent blocks carry no block checksum and are identified by
their magic and structure.

## What U-Boot's driver supports

- Reading `NGF\1` volumes: directory listing, file size, file reads. Tested on X5000 AmigaOS
  installations.
- `NGF\0` directories, as described above. Not yet tested on a real `NGF\0` volume.
- The extent continuation chain (offset 0x18) is not followed, so a file with more extents than
  fit in its object block (134 with 2048-byte blocks) is read only as far as those extents go.
- No writing.
