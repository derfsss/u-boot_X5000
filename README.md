# U-Boot for the AmigaOne X5000

> [!WARNING]
> **Only install this firmware if you know what you are doing. You use it at your own risk.**
>
> This replaces your X5000's boot firmware (its "BIOS"). A wrong image, a faulty card, or a power
> cut during an update can leave the machine unable to start until a working firmware card is
> fitted.
>
> - **Back up first.** Before you install or update anything, make a raw image of your current
>   firmware SD card (the whole card) and keep the original card unchanged in a safe place. Have a
>   spare card, a known-good firmware image and an SD card writer to hand, so you can always go
>   back.
> - **Use the image for your model.** An X5000/20 image does not start an X5000/40, and the reverse.
>   The X5000/40 images have not been tested on an X5000/40.
> - **Never switch off or reset during an update.** If `fwupdate` is interrupted while it writes, the
>   card may no longer start the machine.
> - **Recovering.** The firmware lives only on the SD card in the firmware slot; this firmware
>   changes nothing else on the computer. If the machine does not start, switch off and fit your
>   original (or backup) card. A serial console (115200 8N1) shows how far a failed start got.
> - **Beta firmware.** This is a v0.9 beta, in development. It is not the official X5000 firmware,
>   and it is not made, supported or endorsed by A-Eon Technology or the makers of the X5000. Do not
>   ask them for help with it.
> - **No warranty.** It is provided "as is", without warranty of any kind; see sections 11 and 12 of
>   the [licence](LICENSE).

Open-source boot firmware for the **AmigaOne X5000** that boots **AmigaOS 4.1**. It is mainline
[U-Boot](https://www.denx.de/wiki/U-Boot) v2021.04 with support for the X5000 added: its own
AmigaOS loader and boot menu, the Amiga filesystems, a Radeon screen console and firmware updates
from U-Boot. It replaces the firmware the X5000 shipped with. All of it is source code under the
GPL.

**Status:** in development. It boots AmigaOS 4.1 on an X5000/20. Keep your original firmware card,
so you can always go back to it.

A port to the AmigaOne A1222 is a separate project:
[github.com/derfsss/u-boot_A1222](https://github.com/derfsss/u-boot_A1222).

## Machines

| Machine | CPU | Status |
|---|---|---|
| AmigaOne X5000/20 | NXP P5020, two e5500 cores | Tested; boots AmigaOS 4.1 |
| AmigaOne X5000/40 | NXP P5040, four e5500 cores | Built from the same source with the same features; not yet tested on an X5000/40 |

The X5000 mainboard is the Varisys "Cyrus", which is the board name used in the source.

## Features

- **Boots AmigaOS 4.1 by itself.** `boota` reads the AmigaOS partitions (NGFS, SFS including
  SFS\2, and FFS), lists every configuration in each partition's `Kickstart/Kicklayout` in a boot
  menu, loads the kickstart and starts AmigaOS. No A-Eon software is involved.
- **A quiet start.** The machine powers on to a black screen with a progress bar for the autoboot
  countdown, then the boot menu, then AmigaOS. Press a key during the countdown to get the U-Boot
  prompt instead.
- **Graphics** on a PCIe Radeon card in the PCIe1 slot (the card's video BIOS is run in U-Boot's
  x86 emulator).
- **USB** keyboard and storage, **SATA**, **Ethernet**.
- **Ready for multicore AmigaOS.** The other cores are parked in U-Boot's spin table and passed to
  the kernel in the way a multicore kernel expects.
- **Firmware updates from U-Boot** with `fwupdate`, from an AmigaOS partition or a FAT32 USB
  stick.
- **Power commands:** `shutdown` powers the machine off through its supervisor MCU; `restart`
  restarts it.

Known issue: exFAT USB sticks can be read, but reads hang on some sticks. Use FAT32.

Planned work is in [docs/roadmap.md](docs/roadmap.md).

## Installing

You need a spare microSD card for the X5000's firmware slot and an SD card writer.

1. Download the image for your model from the
   [Releases](https://github.com/derfsss/u-boot_X5000/releases) page, or build one (see
   [Building](#building) below). The production image boots straight into AmigaOS; the debug
   image shows the boot menu and a more detailed AmigaOS serial log.
2. Write the image raw to the whole card (for example with `dd`, or any raw image writer).
3. Put the original firmware card aside, and fit the new card.

Putting the original card back restores the machine as it was; this firmware changes nothing else
on the computer. A serial console (115200 8N1) shows the start-up messages if something goes
wrong. The card layout is in [docs/sd-card-layout.md](docs/sd-card-layout.md).

## Updating

Once a card has this firmware, update it from the U-Boot prompt:

```
fwupdate DH3:x5000-openuboot-spl.img          (a file on the AmigaOS partition DH3:)
fwupdate usb 0:1 x5000-openuboot-spl.img      (a file on a FAT32 USB stick)
```

`fwupdate` shows the firmware on the card and in the file, and asks before writing. It then reads
the card back to check it, puts the old firmware back if the check fails, and restarts after a key
press. Your settings are kept; `-d` writes the image's default settings instead. Give the file's
CRC32 as a last argument to refuse a damaged file.

If your card has an image with the old layout (U-Boot at 0x8C000), write the new image with an SD
card writer once; `fwupdate` works from then on. See
[docs/booting-amigaos.md](docs/booting-amigaos.md#upgrading-from-an-image-with-the-old-layout).

Keep a working spare card, and do not switch off while `fwupdate` is writing.

## Using the firmware

After power-on the autoboot countdown runs for 3 seconds, then `boota` shows the boot menu:

- `1`-`9` or `A`-`Z` boots an entry; UP/DOWN and RETURN boot the marked one.
- SPACE stops the menu's countdown.
- Ctrl-C leaves the menu for the U-Boot prompt.

Press any key during the autoboot countdown to get the U-Boot prompt instead. There, `help` lists
the commands and `boota` shows the menu again.

The commands this port adds (`boota`, `fwupdate`, `videoboot`, `shutdown`, `restart`), the
standard commands it changes, and the full command list are described in
[docs/commands.md](docs/commands.md).

## Settings

At the prompt, `printenv name` shows a setting; `setenv name value` followed by `saveenv` changes
it.

| Setting | Default | Meaning |
|---|---|---|
| `bootdelay` | `3` | Seconds of autoboot countdown. The progress bar fills over this time. |
| `bootcmd` | `boota` | What runs when the countdown ends. |
| `boota_timeout` | not set (5) | Seconds the boot menu waits before booting the marked entry; SPACE stops it, `0` waits. |
| `amigaboot_quiet` | `n` (debug build), `y` (production build) | `y` skips the menu and boots the default entry. |
| `bootable_only` | not set (`n`) | `y` lists only partitions marked bootable. |
| `os4_bootdevice` | `auto` | Where to look for AmigaOS: `auto` (SATA, then USB), or e.g. `usb,sata`. |
| `os4_commandline` | `serial munge debuglevel=1` (debug), `serial munge debuglevel=0` (production) | Options passed to the AmigaOS kernel. |
| `stdout` | `serial` | Start-up messages go to the serial port only and the screen stays black. `serial,vga` shows them on the screen too. |
| `vesamode` | not set | Force one VESA mode for the screen (hex, e.g. `114`); see [docs/graphics.md](docs/graphics.md). |
| `ddr_wrlvl0`, `ddr_wrlvl1` | not set | Per-byte-lane DDR write-levelling start for memory controller 0 or 1, e.g. `07,07,07,07,0b,0d,0d,0d`. Only for a board whose memory needs tuning. |

## Building

You need Linux (or WSL, or a container), a PowerPC cross compiler and the usual U-Boot build
tools. On Debian or Ubuntu:

```sh
sudo apt install gcc-powerpc-linux-gnu build-essential bison flex bc libssl-dev python3 \
    device-tree-compiler git curl
```

(Tested with GCC 10 and GCC 12. `docker-x5000/Dockerfile` is a ready-made container with the
compiler and build tools; download the microcode below on the host. See
[docs/building.md](docs/building.md).)

**1. Get the source and the Ethernet microcode.** The SD card image needs one file that is not in
this repository: NXP's FMan Ethernet microcode, from
[github.com/nxp-qoriq/qoriq-fm-ucode](https://github.com/nxp-qoriq/qoriq-fm-ucode). It goes in
`build/` at the top of the repository:

```sh
git clone https://github.com/derfsss/u-boot_X5000.git
cd u-boot_X5000
mkdir -p build
U=https://raw.githubusercontent.com/nxp-qoriq/qoriq-fm-ucode/master
curl -fL -o build/fsl_fman_ucode_p5020_r2.0_106_1_18.bin $U/fsl_fman_ucode_p5020_r2.0_106_1_18.bin   # X5000/20
curl -fL -o build/fsl_fman_ucode_p5040_r2.1_106_1_18.bin $U/fsl_fman_ucode_p5040_r2.1_106_1_18.bin   # X5000/40
```

**2. Build the SD card image.** `tools-x5000/build_spl_img.sh` compiles the firmware and
assembles the image in one step:

```sh
tools-x5000/build_spl_img.sh                            # X5000/20, debug build
BUILD=production tools-x5000/build_spl_img.sh           # X5000/20, production build
MODEL=40 tools-x5000/build_spl_img.sh                   # X5000/40, debug build
MODEL=40 BUILD=production tools-x5000/build_spl_img.sh  # X5000/40, production build
```

The image is written to `build/`:

| Model | Debug build | Production build |
|---|---|---|
| X5000/20 | `x5000-openuboot-spl.img` | `x5000-openuboot-spl-production.img` |
| X5000/40 | `x5000-40-openuboot-spl.img` | `x5000-40-openuboot-spl-production.img` |

The **debug** build shows the boot menu and asks the AmigaOS kernel for a serial log
(`debuglevel=1`); the **production** build boots the default entry without the menu, with
`debuglevel=0`. Install the image as described in [Installing](#installing).

**Building U-Boot alone.** For development, or for a `u-boot.bin` to install with `fwupdate`,
build it like any U-Boot tree:

```sh
export CROSS_COMPILE=powerpc-linux-gnu- ARCH=powerpc
make O=out Cyrus_P5020_SDCARD_defconfig && make O=out -j$(nproc)
```

This gives `out/u-boot.bin` (the main U-Boot) and `out/spl/u-boot-spl.pbl` (the first stage). The
defconfigs are `Cyrus_P5020_SDCARD_defconfig` and `Cyrus_P5020_SDCARD_PRODUCTION_defconfig` for the
X5000/20, and `Cyrus_P5040_SDCARD_defconfig` and `Cyrus_P5040_SDCARD_PRODUCTION_defconfig` for the
X5000/40.

The firmware has two stages: a small first stage runs from on-chip SRAM, sets up the memory and
loads the main U-Boot into it. The build options and a container recipe are in
[docs/building.md](docs/building.md).

Developer tools, not needed for a normal build:

- `Cyrus_P5020_SDCARD_DDRDIAG_defconfig` (and its `_RELAXED` and `_SWEEP` variants) build a first
  stage that prints DDR diagnostics on the serial port.
- `tools-x5000/aos4-probe` is a stand-in for the AmigaOS kernel loader that prints what a loader
  hands over to the kernel.

## Source layout

```
(root)                      mainline U-Boot v2021.04 with the X5000 changes
board/varisys/cyrus/        the X5000 board: DDR, TLBs, video, first stage
cmd/boota.c, common/aos4/   the AmigaOS loader and boot menu
cmd/fwupdate.c              firmware updates
cmd/shutdown.c              shutdown and restart
fs/ngfs/, fs/sfs/, fs/affs/ NGFS, SFS and FFS readers
docs/                       documentation for this port
tools-x5000/                SD image assembly and the aos4-probe tool
docker-x5000/               the build container
```

Changes to upstream U-Boot files are marked with comments starting `AmigaOne X5000:` or
`AmigaOne:`, so `grep -rn "AmigaOne" .` finds them.

## Documentation

| Document | Contents |
|---|---|
| [docs/commands.md](docs/commands.md) | The commands this port adds and changes, and all other commands |
| [docs/booting-amigaos.md](docs/booting-amigaos.md) | `boota`, the boot menu and settings, the kernel handoff, SMP, `fwupdate` |
| [docs/building.md](docs/building.md) | Building the firmware and the SD card image |
| [docs/sd-card-layout.md](docs/sd-card-layout.md) | What is where on the firmware card |
| [docs/graphics.md](docs/graphics.md) | The Radeon console: video BIOS POST, supported cards, limits |
| [docs/ngfs-format.md](docs/ngfs-format.md) | The NGFS on-disk format |
| [docs/roadmap.md](docs/roadmap.md) | Planned work |

## Licences and third-party components

U-Boot is free software under the GNU General Public License version 2 ([`LICENSE`](LICENSE));
most files also allow any later version. Each source file states its licence in an
`SPDX-License-Identifier` line, and [`Licenses/`](Licenses/) holds the full texts
([`Licenses/README`](Licenses/README) lists them). This port's additions are GPL-2.0-or-later.

The source contains no proprietary code. The firmware image contains one third-party binary:
NXP's FMan Ethernet **microcode**, included unmodified under NXP's binary licence. That licence
requires NXP's notice to go with every copy; it is in
[`Licenses/NXP-FMan-ucode-EULA.txt`](Licenses/NXP-FMan-ucode-EULA.txt), and the image build puts
a copy next to every image it makes.

`fs/exfat/` is mainline U-Boot's exFAT driver (libexfat, Copyright Andrew Nayenko,
GPL-2.0-or-later), backported read-only.

AmigaOS 4.1 is not part of this firmware; it boots the copy installed on your disks.

## Acknowledgments

- The U-Boot project, and Andy Fleming and Adrian Cox (Varisys), authors of the mainline
  `board/varisys/cyrus` port this builds on.
- ACube's Sam440ep/Sam460ex U-Boot (Andrea Vallinotto), a reference for the SFS driver and
  the boot menu input handling.
- BBoot by Zoltan Balaton ([codeberg.org/qmiga/bboot](https://codeberg.org/qmiga/bboot), GPL),
  an open AmigaOS 4 loader whose kernel call this firmware's loader also uses.

## Development

This project's additions were built with help from [ClaudeAI](https://claude.ai) (Anthropic).
