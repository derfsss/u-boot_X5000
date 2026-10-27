# X5000 SD card layout

The X5000 boots from a microSD card inside the machine. The card has no filesystem: the firmware
is a raw image written from sector 0, and each part sits at a fixed byte offset.

## Layout

| Offset | Contents | Size |
|---|---|---|
| 0x000000 | zero (reserved) | 0x1000 |
| 0x001000 | first-stage loader: the SPL in QorIQ pre-boot loader format (RCW, PBI, SPL) | up to 0x88200 |
| 0x089200 | AmigaOS settings block, read by AmigaOS's `nonvolatile.library` | 8 KiB |
| 0x150000 | saved U-Boot environment (`saveenv`) | 8 KiB |
| 0x160000 | NXP FMan Ethernet microcode (P5020 r2.0 or P5040 r2.1) | about 30 KiB |
| 0x200000 | main U-Boot (`u-boot.bin`) | 2 MiB slot |

The whole image is 0x2438600 bytes (about 36 MiB), the same size as the original firmware image,
so it can replace a whole card image. Everything not listed above is zero.

The offsets are set in `include/configs/cyrus.h` (`CONFIG_SYS_MMC_U_BOOT_OFFS` and `_SIZE`) and
the SDCARD defconfigs (`CONFIG_ENV_OFFSET`).

## The parts

**Reset configuration and SPL (0x1000).** The P5020/P5040 boot ROM reads the RCW (reset
configuration word: clock ratios, SerDes lanes, boot source) and PBI commands from offset 0x1000,
then runs the SPL from on-chip SRAM. The SPL sets up DDR and copies the main U-Boot from 0x200000
into DDR at 0x30000000. The RCW is the one in the mainline Cyrus port, which matches the
configuration of the original X5000 firmware.

**AmigaOS settings block (0x89200).** AmigaOS's `nonvolatile.library` stores its settings at this
offset, where the original firmware kept its U-Boot environment. This firmware does not read the
block. A new image carries the build's default environment there, in the format AmigaOS expects
(8 KiB, big-endian CRC32 followed by `name=value` strings, no redundancy flag), made with U-Boot's
`mkenvimage`. `fwupdate` keeps the card's copy of this block unless told otherwise.

**U-Boot environment (0x150000).** Where `saveenv` writes. A new image normally has nothing here,
and U-Boot uses its built-in defaults until the first `saveenv`. An image built with `ENV_EXTRA`
(see [building.md](building.md)) carries a saved environment here.

**FMan microcode (0x160000).** The P5020/P5040 Ethernet (Frame Manager) needs NXP's microcode;
U-Boot loads it from this offset. It is NXP's binary, redistributed under NXP's licence
(`Licenses/NXP-FMan-ucode-EULA.txt`).

**Main U-Boot (0x200000).** `u-boot.bin`, at most 2 MiB. Older images of this firmware had a
768 KiB slot at 0x8C000 instead; see
[booting-amigaos.md](booting-amigaos.md#upgrading-from-an-image-with-the-old-layout).

## Assembling an image by hand

`tools-x5000/build_spl_img.sh` assembles the image for you (see [building.md](building.md)).
It calls `tools-x5000/mksdimage.sh`, which zero-fills an image of the given size and writes each
component at its offset (offsets must be 512-byte aligned):

```sh
mksdimage.sh -o x5000.img -s 0x2438600 \
    u-boot-spl.pbl:0x1000 \
    os4-env.bin:0x89200 \
    fsl_fman_ucode_p5020_r2.0_106_1_18.bin:0x160000 \
    u-boot.bin:0x200000
```

Add `uboot-env.bin:0x150000` to include a saved U-Boot environment (made with
`tools/mkenvimage -b -s 0x2000`).

## Writing a card

Write the image raw to the whole card, from sector 0, with any raw image writer. Use a spare
card and keep the original firmware card unchanged, so you can always go back to it. A serial
console (115200 8N1) shows the firmware's start-up messages, which helps if a card does not
boot.

After the first install, `fwupdate` can update the firmware from U-Boot without removing the card
(see [booting-amigaos.md](booting-amigaos.md#fwupdate-updating-the-firmware)).
