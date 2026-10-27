# Graphics: the Radeon console

The X5000 has no on-board display. U-Boot drives a PCIe Radeon graphics card for its screen
console, the boot screen and the `boota` menu. The code is `board/varisys/cyrus/video.c`.

## How it works

A Radeon's video BIOS (VBIOS) is x86 code that the card expects a PC BIOS to run at power-on
("POST") to initialise it. The X5000 is a PowerPC machine, so U-Boot runs that code in its x86
emulator (`drivers/bios_emulator`, `CONFIG_BIOSEMU`), the same approach earlier AmigaOne firmware
used.

At start-up `video_hw_init()`:

1. finds the card: the first AMD/ATI PCI function of the display class (this skips the HDMI
   audio and USB functions that share the card's device number);
2. opens legacy VGA and full 16-bit I/O forwarding on every PCI-to-PCI bridge between the root
   port and the card, so the VBIOS's port accesses reach it (Vega cards have two bridges on the
   card itself). If the path cannot be worked out, the POST is skipped and the machine boots
   without a screen rather than risk a machine check;
3. POSTs the VBIOS in the emulator and asks it, through the VESA BIOS calls, for a linear
   framebuffer mode;
4. hands the framebuffer to U-Boot's `cfb_console`. The framebuffer's PCI bus address is
   translated to the CPU's view of the PCIe1 window before use.

Modes are tried in this order, and the first one the VBIOS provides as a linear framebuffer with
no line padding is used:

| Mode | Resolution | Depth |
|---|---|---|
| 0x117 | 1024x768 | 16 bpp |
| 0x118 | 1024x768 | 32 bpp |
| 0x114 | 800x600 | 16 bpp |
| 0x115 | 800x600 | 32 bpp |
| 0x111 | 640x480 | 16 bpp |
| 0x112 | 640x480 | 32 bpp |

16 bpp comes first: it halves the data written across PCIe, which makes scrolling faster, and the
boot screen's progress bar is drawn only in 16 bpp (RGB 5:6:5). Set the variable `vesamode` (hex,
e.g. `setenv vesamode 114`) to force one mode.

## Which cards work

Any AMD/ATI card whose option ROM contains a legacy x86 VBIOS image should work; the card is found
by its PCI class, not by a list of device IDs, so TeraScale, GCN and later cards are all
candidates. Tested:

- Radeon RX 560 (Polaris): POST takes about 1.3 s; 1024x768 16 bpp is used.
- Radeon RX Vega 64.

## Limits

- **PCIe1 slot only.** The emulator's x86 I/O port window is fixed at build time to the PCIe1
  controller (`VIDEO_IO_OFFSET` in `include/configs/cyrus.h`). A card on another controller is
  still tried, with a warning, but its POST will probably fail.
- **UEFI-only cards hang.** A card whose option ROM holds only a UEFI (GOP) image still passes the
  ROM signature check, and the emulator then runs code that is not x86 real-mode code and hangs.
  Choosing the legacy image from the ROM's image list is not implemented yet. The cards supplied
  with X5000 systems normally carry a legacy image.
- **No 8 bpp modes** (they would need a palette hook), and no modes whose lines are padded.
- Each mode tried costs a full POST, because the emulator only sets a mode as part of a POST.
  Normally the first mode works and there is a single POST.
- There is no boot logo; the boot screen is plain black. (`bmp` can still display a BMP file
  from the prompt.)

## The screen at start-up

By default `stdout=serial`, so U-Boot's start-up messages go to the serial port only and the screen
stays black, with a progress bar along the bottom for the autoboot countdown. Pressing a key
during the countdown brings the console to the screen. The `boota` menu and the kickstart loading
are always shown on the screen. Set `stdout` to `serial,vga` (and `saveenv`) to see all start-up
messages on the screen.

## The `videoboot` command

For trying a card:

```
videoboot          POST the VBIOS only, and report success or failure
videoboot scan     POST once per candidate mode and print what each one gives
videoboot <mode>   POST, set that VESA mode (hex, e.g. 0x117) and print its geometry
```

Unset `vesamode` to go back to automatic mode selection.
