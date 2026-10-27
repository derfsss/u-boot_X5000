#!/bin/bash
# SPDX-License-Identifier: GPL-2.0+
# Copyright (C) 2026 Richard Gibbs
#
# Build the AmigaOne X5000 (Varisys Cyrus) two-stage SPL firmware and
# assemble a complete SD-card image. It needs a PowerPC cross toolchain
# (CROSS_COMPILE, default powerpc-linux-gnu-), make, python3 and the usual
# U-Boot host build tools; it can run natively or in any container that has
# them (docker-x5000/Dockerfile is one). See docs/building.md.
#
# Directories:
#   WORK  inputs and outputs go in $WORK/build/. Default: the U-Boot tree
#         this script is in, or the project directory above it when the
#         tree is at <project>/sources/u-boot-v2021.04.
#   OBJ   optional object cache. If the directory exists, the source tree
#         is mirrored into $OBJ/src (only files changed since the last run
#         are copied; FULL_SYNC=1 copies everything, e.g. after deleting or
#         renaming files) and built there, one object directory per
#         defconfig. Useful when the tree is on a slow bind mount. Default
#         /obj.
set -e

# Model: MODEL=20 (X5000/20, P5020, the default) or MODEL=40 (X5000/40,
# P5040). It picks the defconfig, the FMan microcode and the non-SPL
# regression build.
MODEL=${MODEL:-20}
case "$MODEL" in
20)	CPU=P5020; UCODE=build/fsl_fman_ucode_p5020_r2.0_106_1_18.bin ;;
40)	CPU=P5040; UCODE=build/fsl_fman_ucode_p5040_r2.1_106_1_18.bin ;;
*)	echo "MODEL must be 20 or 40 (got '$MODEL')"; exit 1 ;;
esac

# Build: BUILD=debug (the default: boot menu shown, detailed kernel serial
# log) or BUILD=production (no boot menu, debuglevel=0); see
# CONFIG_BOOTA_PRODUCTION in cmd/Kconfig.
BUILD=${BUILD:-debug}
case "$BUILD" in
debug)		CFG_SUFFIX= ; IMG_SUFFIX= ;;
production)	CFG_SUFFIX=_PRODUCTION; IMG_SUFFIX=-production ;;
*)		echo "BUILD must be debug or production (got '$BUILD')"; exit 1 ;;
esac
case "$MODEL" in
20)	IMG_DEFAULT=build/x5000-openuboot-spl$IMG_SUFFIX.img ;;
40)	IMG_DEFAULT=build/x5000-40-openuboot-spl$IMG_SUFFIX.img ;;
esac

# Optional overrides, e.g. for the DDR diagnostics build:
#   DEFCONFIG=Cyrus_P5020_SDCARD_DDRDIAG_defconfig IMG=build/x.img
DEFCONFIG=${DEFCONFIG:-Cyrus_${CPU}_SDCARD${CFG_SUFFIX}_defconfig}
IMG=${IMG:-$IMG_DEFAULT}
# Optional: ENV_EXTRA=path/to/file (relative to $WORK) holding extra
# name=value lines, e.g. a board's "ddr_wrlvl1=07,07,07,07,0b,0d,0d,0d".
# The image then carries a saved U-Boot environment at SD 0x150000: the
# firmware's built-in defaults plus these lines. (A saved environment
# replaces the built-in one, so the defaults must be in it too.)
ENV_EXTRA=${ENV_EXTRA:-}
REGRESSION=${REGRESSION:-1}	# 0 skips the extra non-SPL build
export CROSS_COMPILE=${CROSS_COMPILE:-powerpc-linux-gnu-}
export ARCH=powerpc
JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
TMP=${TMPDIR:-/tmp}/x5000-build
mkdir -p "$TMP"
fsize() { wc -c < "$1" | tr -d ' '; }

TREE=$(cd "$(dirname "$0")/.." && pwd)
if [ -z "$WORK" ]; then
	if [ -d "$TREE/sources/u-boot-v2021.04" ]; then
		WORK=$TREE			# project-level copy of this script
	elif [ "$(basename "$(dirname "$TREE")")" = sources ]; then
		WORK=$(cd "$TREE/../.." && pwd)	# tree inside a project directory
	else
		WORK=$TREE			# the tree on its own
	fi
fi
if [ -d "$WORK/sources/u-boot-v2021.04" ]; then
	SRC=$WORK/sources/u-boot-v2021.04
else
	SRC=$WORK
fi
MKSDIMAGE=$SRC/tools-x5000/mksdimage.sh
NXP_NOTICE=$SRC/Licenses/NXP-FMan-ucode-EULA.txt
mkdir -p "$WORK/build"
echo "tree: $SRC  outputs: $WORK/build  build: $BUILD ($DEFCONFIG)"

OBJ=${OBJ:-/obj}
if [ -d "$OBJ" ]; then
	O=$OBJ/$DEFCONFIG
	stamp=$OBJ/src/.sync-stamp
	next=$(mktemp)
	if [ "${FULL_SYNC:-0}" = 1 ] || [ ! -f "$stamp" ]; then
		rm -rf "$OBJ/src" && mkdir -p "$OBJ/src"
		(cd "$SRC" && tar c --exclude=./.git --exclude=./build .) | (cd "$OBJ/src" && tar x)
		echo "source mirrored to $OBJ/src (full copy)"
	else
		n=$(cd "$SRC" && find . -path ./.git -prune -o -path ./build -prune -o -type f -newer "$stamp" -print | tee "$TMP/changed" | wc -l)
		[ "$n" -gt 0 ] && (cd "$SRC" && tar c -T "$TMP/changed") | (cd "$OBJ/src" && tar x)
		echo "source mirrored to $OBJ/src ($n changed file(s))"
	fi
	mv "$next" "$stamp"
	# The mirror has no .git: hand the version suffix over the way
	# scripts/setlocalversion accepts it (.scmversion).
	(cd "$SRC" && git config --global --add safe.directory "$SRC" 2>/dev/null
	 echo "-g$(git rev-parse --short=8 HEAD)$(git diff --quiet HEAD -- 2>/dev/null || echo -dirty)") \
		> "$OBJ/src/.scmversion" 2>/dev/null || rm -f "$OBJ/src/.scmversion"
	SRC=$OBJ/src
else
	O=$TMP/o1
fi

# --- preflight: all non-repo inputs present? -----------------------------
missing=0
for f in "$WORK/$UCODE" "$NXP_NOTICE"; do
	[ -f "$f" ] || { echo "MISSING: $f"; missing=1; }
done
if [ "$missing" = 1 ]; then
	echo ""
	echo "Download the FMan microcode from github.com/nxp-qoriq/qoriq-fm-ucode"
	echo "into $WORK/build/ (see docs/building.md)."
	exit 1
fi

# SD card layout (include/configs/cyrus.h): the main U-Boot's slot
UBOOT_OFFS=0x200000
UBOOT_SLOT=0x200000

cd "$SRC"
[ "$O" = "$TMP/o1" ] && rm -rf "$O"; mkdir -p "$O"
make O="$O" "$DEFCONFIG" >/dev/null 2>&1
# Don't let a DEFCONFIG override build one model's firmware into the
# other model's image (wrong RCW, clocks and Ethernet ports).
grep -q "^CONFIG_ARCH_$CPU=y" "$O/.config" || {
	echo "$DEFCONFIG is not a $CPU (X5000/$MODEL) config"; exit 1; }
make O="$O" -j"$JOBS" >"$TMP/b1.log" 2>&1 && echo "SPL build OK" || { echo "SPL build FAIL (log: $TMP/b1.log)"; tail -20 "$TMP/b1.log"; exit 1; }
cp "$O/spl/u-boot-spl.pbl" "$O/u-boot.bin" "$WORK/build/"
[ "$(fsize "$WORK/build/u-boot.bin")" -le $((UBOOT_SLOT)) ] || {
	echo "u-boot.bin is larger than its $((UBOOT_SLOT / 1024)) KiB SD slot"; exit 1; }
echo "spl.pbl=$(fsize "$WORK/build/u-boot-spl.pbl")  u-boot.bin=$(fsize "$WORK/build/u-boot.bin")  spl.bin=$(fsize "$O/spl/u-boot-spl.bin")"
echo "SPL binary first 16 bytes (expect a uImage header 27 05 19 56..., not zeros):"
od -A n -N 16 -t x1 "$O/spl/u-boot-spl.bin"
echo "SPL binary last 4 bytes (resetvec, expect a branch 48/4b..):"
tail -c 4 "$O/spl/u-boot-spl.bin" | od -A n -t x1

make O="$O" u-boot-initial-env >/dev/null 2>&1

# The block at SD 0x89200 is AmigaOS's NVRAM: nonvolatile.library reads
# and writes it there, in the U-Boot environment format the official
# firmware used (8 KiB, big-endian CRC, no redundancy flag). This U-Boot
# does not read it (its own environment is at CONFIG_ENV_OFFSET). Fill it
# with this build's default environment, which is right for the model
# (e.g. ethprime).
"$O/tools/mkenvimage" -b -s 0x2000 -o "$WORK/build/os4-env.bin" "$O/u-boot-initial-env"
echo "AmigaOS environment block (SD 0x89200): X5000/$MODEL defaults"

ENV_PART=""
if [ -n "$ENV_EXTRA" ]; then
	[ -f "$WORK/$ENV_EXTRA" ] || { echo "MISSING: $ENV_EXTRA"; exit 1; }
	env_off=$(sed -n 's/^CONFIG_ENV_OFFSET=//p' "$O/.config")
	env_size=$(sed -n 's/^CONFIG_ENV_SIZE=//p' "$O/.config")
	grep -v '^[[:space:]]*#' "$WORK/$ENV_EXTRA" | grep '=' > "$TMP/env-extra.txt" || true
	# A line in the file replaces the default of the same name (e.g. a
	# board's own hwconfig), rather than adding a duplicate.
	awk -F= 'NR == FNR { own[$1] = 1; next } !($1 in own)' \
		"$TMP/env-extra.txt" "$O/u-boot-initial-env" > "$TMP/env.txt"
	cat "$TMP/env-extra.txt" >> "$TMP/env.txt"
	"$O/tools/mkenvimage" -b -s "$env_size" -o "$WORK/build/uboot-env.bin" "$TMP/env.txt"
	echo "environment with $(wc -l < "$TMP/env-extra.txt") extra line(s) from $ENV_EXTRA -> SD $env_off:"
	sed 's/^/  /' "$TMP/env-extra.txt"
	ENV_PART="build/uboot-env.bin:$env_off"
fi

cd "$WORK"
# SD 0x800000 and 0x900000, which hold AmigaBoot and its data image on
# A-Eon's cards, are left empty. The image has the same overall size as
# A-Eon's card image so it can replace it whole.
bash "$MKSDIMAGE" -o "$IMG" -s 0x2438600 \
  build/u-boot-spl.pbl:0x1000 \
  build/os4-env.bin:0x89200 \
  $UCODE:0x160000 \
  build/u-boot.bin:$UBOOT_OFFS $ENV_PART

# NXP's EULA lets the FMan microcode be redistributed only with NXP's
# notice beside every copy, so the image always gets the notice next to it.
cp "$NXP_NOTICE" "$(dirname "$IMG")/"
echo "NXP notice: $(dirname "$IMG")/NXP-FMan-ucode-EULA.txt"

echo "=== verify SD offsets ==="
printf "@0x1000  SPL PBL (aa55aa55) : "; od -A n -j $((0x1000)) -N 4 -t x1 "$IMG"
printf "@0x89200 AmigaOS NVRAM (CRC): "; od -A n -j $((0x89200)) -N 8 -t x1 "$IMG"
printf "@%s u-boot.bin       : " $UBOOT_OFFS; od -A n -j $((UBOOT_OFFS)) -N 4 -t x1 "$IMG"
# The DDR diagnostics SPL prints the crc32 of the slot it copied from here.
python3 -c 'import sys,zlib; o,n=int(sys.argv[2],0),int(sys.argv[3],0); d=open(sys.argv[1],"rb").read()[o:o+n]; print("crc32 of U-Boot slot %#x..%#x: %08x" % (o, o+n, zlib.crc32(d)))' "$IMG" $UBOOT_OFFS $UBOOT_SLOT 2>/dev/null || true

[ "$REGRESSION" = 0 ] && exit 0
echo "=== non-SPL regression ==="
cd "$SRC"
rm -rf "$TMP/o2" && mkdir -p "$TMP/o2"
make O="$TMP/o2" "Cyrus_${CPU}_defconfig" >/dev/null 2>&1 && make O="$TMP/o2" -j"$JOBS" >"$TMP/b2.log" 2>&1 && echo "non-SPL OK pbl=$(fsize "$TMP/o2/u-boot.pbl")" || { echo "non-SPL FAIL (log: $TMP/b2.log)"; tail -15 "$TMP/b2.log"; }
