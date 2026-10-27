# Building the X5000 firmware

This tree is mainline U-Boot v2021.04 (the last release that still has `board/varisys/cyrus`)
with the AmigaOne X5000 changes on top. It builds like any U-Boot tree: with a PowerPC cross
compiler, natively on Linux or in a container. A Dockerfile is included as one ready-made
toolchain, but nothing in the build depends on it.

## What you need

- A PowerPC cross toolchain and U-Boot's host tools. Any `powerpc-linux-gnu-` (or similar) GCC
  that builds U-Boot v2021.04 will do; it is tested with GCC 10 (Debian bullseye) and GCC 12
  (Debian bookworm). On Debian or Ubuntu:

  ```sh
  sudo apt install gcc-powerpc-linux-gnu build-essential bison flex bc libssl-dev python3 \
      device-tree-compiler git curl
  ```

  GCC 12 prints more warnings than GCC 10 (array bounds, executable stack notes from the linker);
  they come from upstream U-Boot v2021.04 and do not affect the firmware.
- NXP's FMan Ethernet microcode for your model, from
  [github.com/nxp-qoriq/qoriq-fm-ucode](https://github.com/nxp-qoriq/qoriq-fm-ucode), in `build/`
  at the top of the repository, for a complete SD card image:
  - X5000/20 (P5020): `fsl_fman_ucode_p5020_r2.0_106_1_18.bin`
  - X5000/40 (P5040): `fsl_fman_ucode_p5040_r2.1_106_1_18.bin`

  ```sh
  mkdir -p build
  U=https://raw.githubusercontent.com/nxp-qoriq/qoriq-fm-ucode/master
  curl -fL -o build/fsl_fman_ucode_p5020_r2.0_106_1_18.bin $U/fsl_fman_ucode_p5020_r2.0_106_1_18.bin
  curl -fL -o build/fsl_fman_ucode_p5040_r2.1_106_1_18.bin $U/fsl_fman_ucode_p5040_r2.1_106_1_18.bin
  ```

  It is the only input that is not in this repository. The microcode is NXP's, distributed under
  NXP's licence; its notice is in `Licenses/NXP-FMan-ucode-EULA.txt`, and the image build copies
  that notice next to every image it makes.

Nothing from A-Eon is needed: the image contains no AmigaBoot and no other A-Eon component.

### Optional: the toolchain container

`docker-x5000/Dockerfile` is Debian bullseye with the cross compiler and the host packages, and
sets `CROSS_COMPILE=powerpc-linux-gnu-` and `ARCH=powerpc`. Bullseye is no longer on the live
Debian mirrors, so the Dockerfile points apt at `archive.debian.org`. Build it under any name:

```sh
docker build -t x5000-uboot-builder docker-x5000/
```

## Defconfigs

| Defconfig | What it builds |
|---|---|
| `Cyrus_P5020_SDCARD_defconfig` | X5000/20 firmware, **debug build**: a first-stage loader (SPL) plus the main U-Boot, with the boot menu shown and `debuglevel=1` for the AmigaOS kernel serial log. |
| `Cyrus_P5020_SDCARD_PRODUCTION_defconfig` | X5000/20 firmware, **production build**: the same, but boots the default entry without showing the boot menu, with `debuglevel=0`. |
| `Cyrus_P5040_SDCARD_defconfig`, `Cyrus_P5040_SDCARD_PRODUCTION_defconfig` | The same two builds for the X5000/40. |
| `Cyrus_P5020_defconfig`, `Cyrus_P5040_defconfig` | The original single-stage mainline build (one `u-boot.pbl`). Kept as a build check; it is not the firmware this project ships. |
| `Cyrus_P5020_SDCARD_DDRDIAG_defconfig` and its `_RELAXED` and `_SWEEP` variants | Developer builds whose SPL prints DDR diagnostics on the serial port (SPD contents, computed timings, controller registers, a pattern test per byte lane). `_RELAXED` runs DDR with relaxed timings; `_SWEEP` tries one write-levelling setting per power-on. For diagnosing memory problems only. |

Debug and production differ only in the default environment (`CONFIG_BOOTA_PRODUCTION`):

| Setting | Debug | Production |
|---|---|---|
| `amigaboot_quiet` | `n` (boot menu shown) | `y` (default entry boots, no menu) |
| `os4_commandline` | `serial munge debuglevel=1` | `serial munge debuglevel=0` |

These are only the built-in defaults. An environment saved on the card (`saveenv`) keeps its own
values; `fwupdate` keeps the card's saved settings unless it is given `-d`.

The firmware is a two-stage build. The SPL, wrapped in the QorIQ pre-boot loader format (RCW and
PBI), runs from on-chip SRAM, sets up DDR and loads the main U-Boot from the SD card into DDR.
This keeps the first-stage image clear of the SD card area at 0x89200 that AmigaOS's
`nonvolatile.library` uses for its settings; a single-stage image is large enough to overwrite it.
See [sd-card-layout.md](sd-card-layout.md).

## Building U-Boot only

From the root of this tree, out of tree (`O=`) so the source stays clean:

```sh
export CROSS_COMPILE=powerpc-linux-gnu- ARCH=powerpc
make O=out Cyrus_P5020_SDCARD_defconfig
make O=out -j$(nproc)
```

The same in the container (the image sets `CROSS_COMPILE` itself):

```sh
docker run --rm -v "$PWD:/src" -w /src x5000-uboot-builder bash -lc \
  'make O=/tmp/obj Cyrus_P5020_SDCARD_defconfig && make O=/tmp/obj -j$(nproc) &&
   mkdir -p build && cp /tmp/obj/spl/u-boot-spl.pbl /tmp/obj/u-boot.bin build/'
```

(In PowerShell, write `${PWD}` and use a backtick instead of `\` at the end of the first line.)

This gives `spl/u-boot-spl.pbl` (the first stage) and `u-boot.bin` (the main U-Boot, at most
2 MiB). `u-boot.bin` on its own can be installed with `fwupdate` (see
[booting-amigaos.md](booting-amigaos.md#fwupdate-updating-the-firmware)); a new card needs a
whole image.

## Building a complete SD card image

`tools-x5000/build_spl_img.sh` builds the firmware and assembles the whole SD image with
`tools-x5000/mksdimage.sh`. Put the FMan microcode in `build/` at the top of the repository (the
folder is ignored by git); the image is written there too. Natively:

```sh
tools-x5000/build_spl_img.sh                          # X5000/20, debug
BUILD=production tools-x5000/build_spl_img.sh         # X5000/20, production
MODEL=40 BUILD=production tools-x5000/build_spl_img.sh
```

Or in the container:

```sh
docker run --rm -v "$PWD:/work" -e BUILD=production x5000-uboot-builder bash /work/tools-x5000/build_spl_img.sh
```

| Model and build | Output |
|---|---|
| `MODEL=20` (default), `BUILD=debug` (default) | `build/x5000-openuboot-spl.img` |
| `MODEL=20`, `BUILD=production` | `build/x5000-openuboot-spl-production.img` |
| `MODEL=40`, `BUILD=debug` | `build/x5000-40-openuboot-spl.img` |
| `MODEL=40`, `BUILD=production` | `build/x5000-40-openuboot-spl-production.img` |

Each run also leaves `build/u-boot-spl.pbl`, `build/u-boot.bin`, the AmigaOS settings block
`build/os4-env.bin` and a copy of `NXP-FMan-ucode-EULA.txt` in `build/`. The script stops with a
clear message if the microcode or the NXP notice is missing, if the defconfig does not match the
model, or if `u-boot.bin` is larger than its 2 MiB slot. At the end it prints the first bytes at
each component's offset and the CRC32 of the U-Boot slot, which you can pass to `fwupdate`.

Options, as environment variables:

| Variable | Meaning |
|---|---|
| `MODEL` | `20` (X5000/20, default) or `40` (X5000/40). |
| `BUILD` | `debug` (default) or `production`; picks the `_SDCARD` or `_SDCARD_PRODUCTION` defconfig and the image name. |
| `DEFCONFIG` | Build another defconfig for the same model instead, e.g. `Cyrus_P5020_SDCARD_DDRDIAG_defconfig`. |
| `IMG` | Output image path, relative to `WORK`. |
| `ENV_EXTRA` | A file (relative to `WORK`) of extra `name=value` lines. The image then carries a saved U-Boot environment at 0x150000: the built-in defaults with these lines added or replaced, for example a board's `ddr_wrlvl1=...` tuning. |
| `CROSS_COMPILE` | The toolchain prefix; default `powerpc-linux-gnu-`. |
| `JOBS` | Parallel make jobs; default the number of CPUs. |
| `WORK` | Where `build/` is. Default: the top of the repository. If the repository is checked out inside a folder named `sources` (as in `project/sources/u-boot-v2021.04`), `build/` goes in `project/` instead. |
| `OBJ` | An object cache directory; see below. Default `/obj`, used only if it exists. |
| `REGRESSION` | `0` skips the extra single-stage build that the script runs at the end as a check. |
| `TMPDIR` | Where build logs and temporary object directories go; default `/tmp`. |

### Faster builds

When the source tree is on a slow file system (for example a Windows folder bind-mounted into a
container), point `OBJ` at a fast directory, such as a named Docker volume:

```sh
docker run --rm -v x5000-obj:/obj -v "$PWD:/work" x5000-uboot-builder bash /work/tools-x5000/build_spl_img.sh
```

The script then mirrors the source tree into `$OBJ/src` (copying only files changed since the
last run) and keeps one incremental object directory per defconfig there. `FULL_SYNC=1` copies
everything again, which is needed after deleting or renaming source files. The version string
still carries the git commit of the source tree.

## Writing the image to a card

The image is written raw to the whole SD card, from sector 0 (there is no filesystem on the
firmware card). Use a spare card and keep your original firmware card unchanged, so you can go
back to it. With an SD card writer, use any raw image writer (for example `dd` on Linux:
`dd if=x5000-openuboot-spl.img of=/dev/sdX bs=1M conv=fsync`, where `sdX` is the card).

Once a card has this firmware, later versions can be installed from U-Boot with `fwupdate`.

## Configuration: Kconfig and driver model

- **Kconfig.** Each option this port adds is a Kconfig option, set in the defconfigs and visible
  in `make O=out menuconfig`:
  - `CONFIG_CMD_BOOTA` and `CONFIG_BOOTA_PRODUCTION`;
  - `CONFIG_CMD_FWUPDATE` and `CONFIG_CMD_SHUTDOWN`;
  - `CONFIG_FS_NGFS`, `CONFIG_FS_SFS`, `CONFIG_FS_AFFS` and `CONFIG_FS_EXFAT`;
  - `CONFIG_BIOSEMU`;
  - the `CONFIG_CYRUS_DDR_DIAG*` diagnostics.
- **Board header.** The board itself is still configured the older way, in
  `include/configs/cyrus.h`, as in the mainline Cyrus port: the memory map, the SD card offsets,
  the default environment, and `CONFIG_SYS_*` values that have no Kconfig option in v2021.04.
- **No driver model.** `CONFIG_DM` is off. Serial, PCI, USB, SATA, Ethernet, I2C, the SD card and
  the screen console use U-Boot's legacy (non-driver-model) drivers, and U-Boot runs without a
  control device tree of its own. The small device tree that `boota` hands to AmigaOS is built at
  boot time.

This is why the port stays on v2021.04. Later releases dropped the board from mainline, and
current U-Boot expects boards to use driver model and a device tree. Moving to a newer U-Boot means
converting the board to driver model first (see [roadmap.md](roadmap.md)).

## Developer tools

`tools-x5000/aos4-probe` is a developer tool, built separately with its own `Makefile`
(`make -C tools-x5000/aos4-probe`, with `CROSS_COMPILE` set as above): a stand-in for
`Kickstart/loader` that prints on the serial port what a loader hands to the AmigaOS kernel. See
its `README.md` and [booting-amigaos.md](booting-amigaos.md).
