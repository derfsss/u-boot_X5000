#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0+
# Copyright (C) 2026 Richard Gibbs
#
# mksdimage.sh - assemble an AmigaOne X5000 SD-card firmware image by placing
# components at fixed byte offsets on a zero-filled base. The X5000 boots from
# the raw card (there is no filesystem on it); tools-x5000/build_spl_img.sh
# calls this with the layout below (see include/configs/cyrus.h and
# docs/sd-card-layout.md):
#
#   0x001000  u-boot-spl.pbl  RCW, PBI and the first-stage loader
#   0x089200  NVRAM           AmigaOS settings block (nonvolatile.library)
#   0x150000  U-Boot env      saved environment (optional)
#   0x160000  FMan microcode  NXP's ethernet microcode
#   0x200000  u-boot.bin      the main U-Boot (2 MiB slot)
#
# Usage:
#   mksdimage.sh -o OUT.img -s SIZE  COMPONENT:OFFSET [COMPONENT:OFFSET ...]
#
#   SIZE and OFFSET accept hex (0x...) or decimal. OFFSET must be 512-byte aligned.
#
set -euo pipefail

OUT=""; SIZE=""
while getopts "o:s:" opt; do
    case "$opt" in
        o) OUT="$OPTARG" ;;
        s) SIZE="$OPTARG" ;;
        *) echo "usage: $0 -o OUT.img -s SIZE COMPONENT:OFFSET ..." >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ -n "$OUT" ] && [ -n "$SIZE" ] && [ "$#" -ge 1 ] || {
    echo "usage: $0 -o OUT.img -s SIZE COMPONENT:OFFSET ..." >&2; exit 2; }

SIZE=$(( SIZE ))   # normalise hex/dec
: > "$OUT"
truncate -s "$SIZE" "$OUT"
echo "base: $OUT ($SIZE bytes, zero-filled)"

for spec in "$@"; do
    path="${spec%:*}"
    off="${spec##*:}"
    off=$(( off ))
    if [ $(( off % 512 )) -ne 0 ]; then
        echo "ERROR: offset $(printf '0x%x' "$off") for $path is not 512-aligned" >&2
        exit 1
    fi
    [ -f "$path" ] || { echo "ERROR: no such component: $path" >&2; exit 1; }
    sz=$(wc -c < "$path" | tr -d " ")
    end=$(( off + sz ))
    if [ "$end" -gt "$SIZE" ]; then
        echo "ERROR: $path ($sz bytes) @ $(printf '0x%x' "$off") overruns image size $(printf '0x%x' "$SIZE")" >&2
        exit 1
    fi
    printf "  place %-28s %10d bytes @ 0x%08x .. 0x%08x\n" "$(basename "$path")" "$sz" "$off" "$end"
    dd if="$path" of="$OUT" bs=512 seek=$(( off / 512 )) conv=notrunc status=none
done

echo "done: $OUT ($(wc -c < "$OUT" | tr -d " ") bytes)  sha256=$(sha256sum "$OUT" | cut -d' ' -f1)"
