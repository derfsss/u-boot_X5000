# U-Boot commands on the X5000

This page describes the U-Boot commands of the X5000 firmware. The first two sections cover what
this port adds to mainline U-Boot and what it changes. The last section lists the other commands
that are built in, which work as in mainline U-Boot.

To get to the U-Boot prompt, press a key during the autoboot countdown (the progress bar after
power-on), or press Ctrl-C in the boot menu. At the prompt, `help` lists every command and
`help name` shows the usage of one. Commands can be typed on the serial console (115200 8N1) or
on a USB keyboard.

## Commands added by this port

| Command | What it does |
|---|---|
| [`boota`](#boota-boot-amigaos-4) | Shows the AmigaOS boot menu and boots AmigaOS 4.1. The default boot command. |
| [`fwupdate`](#fwupdate-update-the-firmware) | Writes a new firmware image to the firmware SD card from a disk or a USB stick. |
| [`videoboot`](#videoboot-start-the-graphics-card) | Starts the Radeon graphics card by hand, and lists the screen modes it offers. |
| [`shutdown`](#shutdown-and-restart) | Powers the machine off. |
| [`restart`](#shutdown-and-restart) | Restarts the machine. |

### `boota`: boot AmigaOS 4

```
boota              show the boot menu
boota 2            boot menu entry 2 without showing the menu (1-9, A-Z)
boota -n           load the default entry, print where everything went, and stop
boota -n 2         the same for entry 2
```

`boota` is U-Boot's own AmigaOS 4 loader. It runs when the autoboot countdown ends
(`bootcmd=boota`), and does the job of the AmigaBoot loader that the original firmware started.
It reads the AmigaOS partitions directly, so no other software is needed to boot.

**How it finds what to boot.**

1. It starts USB (`usb start`) and SATA (`sata init`), and makes the USB keyboard an input.
2. It looks at the disks named by `os4_bootdevice`. The default, `auto`, means the SATA disks
   first, then USB storage. A list such as `usb,sata` sets another order.
3. On each disk with an Amiga RDB partition table, it takes the partitions whose filesystem U-Boot
   can read: NGFS (`NGF\0`, `NGF\1`), SFS (`SFS\0`, `SFS\2`) and FFS or OFS (`DOS\x`). With `bootable_only=y`, it skips partitions that are not marked bootable in the RDB.
4. From each of those partitions it reads `Kickstart/Kicklayout`. Partitions without that file
   are skipped.

**Kicklayout.** This is the standard AmigaOS 4 kickstart list. It holds one or more
configurations:

```
; Lines starting with ';' are comments.
LABEL AmigaOS 4.1 Final Edition
EXEC Kickstart/loader
MODULE Kickstart/kernel
MODULE Kickstart/...
```

- `LABEL` starts a configuration and gives its name in the menu.
- `EXEC` names the kernel loader; there must be exactly one.
- `MODULE` lines name the other kickstart files, loaded in the order given.
- An empty line ends a configuration.
- File names are relative to the root of the partition.
- Keywords may be written in any case.

`boota` reports mistakes with their line number, such as a second `EXEC` or an unknown keyword, and
skips a configuration that has no `EXEC`. It reads at most 8 configurations from one file and
128 modules per configuration, and the menu holds at most 35 entries.

**The menu.** Each configuration becomes one entry, showing its key, the label, the disk
(e.g. `sata 0:2`) and the partition's drive name (e.g. `DH0`). The entry marked `>` is the
default: the first configuration on the partition with the highest RDB boot priority.

| Key | Action |
|---|---|
| `1`-`9`, `A`-`Z` | Boot that entry. |
| UP / DOWN | Move the mark. |
| RETURN | Boot the marked entry. |
| SPACE | Stop the countdown; the menu then waits. |
| Ctrl-C | Leave the menu for the U-Boot prompt. |

The menu counts down from `boota_timeout` seconds (5 if not set) and then boots the marked entry.
With `boota_timeout=0` it waits for a key. The menu is shown on the serial console and on the
screen. With `amigaboot_quiet=y` (the production build's default) there is no menu: the default
entry boots straight away.

**Loading and starting.** `boota` loads the `EXEC` file and each `MODULE` into memory, showing
a progress bar for each file and the total time at the end.

- A `MODULE` that is missing or cannot be read stops the loading with the question
  `Press C to boot without it, A to abort`. With `amigaboot_quiet=y` it boots without it, without
  asking.
- A missing `EXEC`, or a kickstart too large for memory, always stops the boot.

`boota` then builds what the kernel expects: a module list, the board information and a small
device tree, which describes the memory and how to start the other CPU cores. It switches off
U-Boot's timer interrupt, clears the screen and jumps to the kernel loader, passing
`os4_commandline` as the kernel command line. If the kernel loader ever returns, `boota` prints
`The kernel loader returned.` and U-Boot carries on at the prompt.

**Dry run.** `boota -n` does all of the above but does not start the kernel. Instead it prints:

- each loaded file with its address, size and CRC32;
- the module list and the memory area used;
- the entry point, and the board information: memory, CPU and bus clocks, baud rate;
- the device tree's address and size, the number of CPUs and the spin-table address.

This shows what a boot would use, without booting. The layout of what `boota` passes to the kernel
is in [booting-amigaos.md](booting-amigaos.md#the-kernel-handoff).

**Settings.**

| Variable | Default | Meaning |
|---|---|---|
| `os4_bootdevice` | `auto` | Disks to scan: `auto` (SATA, then USB), or interface names, e.g. `usb,sata`. |
| `bootable_only` | not set | `y`: only partitions marked bootable in the RDB. |
| `boota_timeout` | not set (5) | Menu countdown in seconds; `0` waits for a key. |
| `amigaboot_quiet` | `n` (debug build), `y` (production build) | `y`: no menu, boot the default entry. |
| `os4_commandline` | `serial munge debuglevel=1` (debug), `serial munge debuglevel=0` (production) | The AmigaOS kernel command line. |

Change one with `setenv name value`, and keep it with `saveenv`.

### `fwupdate`: update the firmware

```
fwupdate [-d] DRIVE:file [crc32]
fwupdate [-d] interface dev[:part] file [crc32]
```

`fwupdate` writes a new firmware file to the firmware SD card while it is in the machine.

Examples:

```
fwupdate DH3:x5000-openuboot-spl.img
fwupdate DH3:x5000-openuboot-spl.img 11a19204
fwupdate usb 0:1 x5000-openuboot-spl.img
fwupdate usb 0:1 u-boot.bin
fwupdate -d usb 0:1 x5000-openuboot-spl.img
```

**Where the file comes from.**

- `DRIVE:file` reads from an AmigaOS partition, by its RDB drive name (e.g. `DH3`), on the SATA
  disks, or on USB storage once USB has been started (`usb start`, which the firmware runs at
  start-up). NGFS, SFS and FFS partitions work.
- `interface dev[:part] file` reads from any disk U-Boot knows, e.g. `usb 0:1` for the first
  partition of the first USB stick. Use a FAT32 stick: exFAT works, but hangs on some sticks. Plug
  the stick in before switching on.

**What it writes.** The kind of file is chosen by its size:

- A file of up to 2 MiB is a **U-Boot image**: `u-boot.bin` from a build, or the 2 MiB U-Boot slot
  of an SD image. It is written to the card's U-Boot slot at 0x200000; the first stage and the
  settings are left alone. This needs a card that already has this firmware's layout.
- A larger file is a **whole SD card image**. It is written from the start of the card, except for
  the card's two settings blocks: the AmigaOS settings block at 0x89200 and the U-Boot environment
  at 0x150000. Saved settings such as DDR tuning and boot options are kept. With `-d`, the
  image's own default settings are written too.

**Checks before writing.** Nothing is written until all of these pass:

1. The file is read whole into memory (at most 64 MiB) and its CRC32 is computed. If you give a
   `crc32` (hex), a file with any other CRC32 is refused. This catches a damaged download.
2. The file must look like firmware: a U-Boot image must contain a U-Boot version string, and a
   whole image must also have the pre-boot loader header at 0x1000.
3. For a whole image, the reset configuration in the image is compared with the card's. A
   difference means the image is probably for the other model (/20 or /40), and `fwupdate` warns
   that the machine would then not start from the card.
4. For a U-Boot image, the card must already hold a U-Boot in the 2 MiB slot. A card with an
   older layout is refused (see
   [booting-amigaos.md](booting-amigaos.md#upgrading-from-an-image-with-the-old-layout)).

`fwupdate` then shows the file's size and CRC32, the firmware version now running, the version in
the file, and what will be written. It asks `Are you sure you want to update the firmware? (y/N)`;
any key but `y` cancels.

**Writing.** After confirmation, Ctrl-C is disabled and:

1. the current contents of the areas to be written are saved in memory;
2. the new contents are written, then read back and compared;
3. if the comparison fails, the saved contents are written back and checked in the same way.

At the end, `fwupdate` reports whether the update succeeded or the old firmware was restored, and
restarts after a key press. In the unlikely case that even the old firmware cannot be written
back, it says so and does not restart, so you can try again before switching off.

Keep a working spare firmware card, and do not switch off while `fwupdate` is writing. The same
description is in [booting-amigaos.md](booting-amigaos.md#fwupdate-updating-the-firmware).

### `videoboot`: start the graphics card

```
videoboot          run the card's video BIOS only (a test)
videoboot scan     try each screen mode and list what the card reports
videoboot 117      run the video BIOS and set VESA mode 0x117 (hex)
```

At power-on the firmware finds the first AMD Radeon card on PCI and runs its video BIOS in U-Boot's
x86 emulator ("POST"), which starts the card and sets a screen mode for the U-Boot console.
`videoboot` does the same by hand, for trying out a card.

- Without an argument it only runs the video BIOS and reports whether that worked.
- `videoboot scan` runs it once for each mode the firmware can use: 1024x768, 800x600 and 640x480,
  each at 16 and 32 bits per pixel (VESA 0x117, 0x118, 0x114, 0x115, 0x111, 0x112). It prints what
  the card reports for each, or that the card refused it.
- With a mode number, it sets that mode and prints the screen geometry and the framebuffer
  address.

The card must be in the slot on the PCIe1 controller; `videoboot` warns if it is not. To make
the console use one particular mode at every start-up, set `vesamode` (hex, e.g.
`setenv vesamode 114`) and `saveenv`; unset it to go back to automatic selection. See
[graphics.md](graphics.md).

### `shutdown` and `restart`

```
shutdown           power the machine off
restart            restart the machine
```

`shutdown` sends the power-off command (`#s`) to the X5000's supervisor microcontroller (the MCU
that controls the power supply), over the serial line that connects them. The machine switches off
at once. If it is still running three seconds later, `shutdown` reports that the MCU did not act.

`restart` resets the processor, which starts the firmware from the beginning; it is the same as
U-Boot's `reset`. The MCU has no command to power the machine back on, so there is no
power-cycle command.

## Standard commands that behave differently

### Filesystem commands and the Amiga partitions

The generic filesystem commands (`ls`, `load`, `size`, `fstype`, `fstypes`) work on AmigaOS
partitions:

```
ls sata 0:2 /Kickstart              list a directory on the second RDB partition of SATA disk 0
load sata 0:2 1000000 /Kickstart/Kicklayout
size usb 0:1 /x5000-openuboot-spl.img
fstype sata 0:2                     prints ngfs, sfs, affs, fat, exfat, ext4 ...
fstypes                             lists the filesystems this build can read
```

- The RDB partitions are numbered from 1 in the order of the RDB. `part list sata 0` lists them,
  with their drive names and DOS types.
- Mainline U-Boot finds an Amiga partition table but cannot use its partitions with these
  commands. This port makes them usable, and adds read-only drivers for NGFS (`ngfs`), SFS
  (`sfs`, including SFS\2) and FFS/OFS (`affs`), plus mainline's read-only exFAT driver (`exfat`).
- FAT and ext4 work as in mainline U-Boot.
- These builds can read filesystems but not write them, so `save` and `ln` report an error.
- A damaged RDB checksum now ends the partition list, where mainline U-Boot could loop forever.

### `boot` and `bootd`

These run `bootcmd`, which is `boota`, so they show the AmigaOS boot menu.

### The autoboot countdown

With the default `stdout=serial`, the screen stays black during start-up. During the autoboot
countdown it shows a progress bar that fills over `bootdelay` seconds; the serial console shows
the usual `Hit any key to stop autoboot` line.

A key press during the countdown stops it, as in mainline U-Boot. This port also switches the
console back to the screen (`stdout` and `stderr` become `serial,vga`), clears it, and prints the
firmware version and a pointer to `help`, so the prompt can be used from a screen and keyboard
alone. Set `stdout=serial,vga` and `saveenv` to see all start-up messages on the screen.

### `usb` and the USB keyboard

- The cursor keys of a USB keyboard send the single-byte codes Ctrl-P, Ctrl-N, Ctrl-F and Ctrl-B
  instead of escape sequences. They move the mark in the boot menu, and at the prompt UP and
  DOWN recall earlier commands, as they do on the serial console.
- `usb reset`: on the X5000, starting the USB controller a second time leaves it unusable until
  the next restart. `boota` therefore never resets USB; it only clears the keyboard's state before
  showing the menu. Avoid `usb reset` at the prompt; use `restart` instead.

## Other commands

These commands are built into the X5000 firmware and work as in mainline U-Boot. `help name`
shows the usage of each.

| Command | Description |
|---|---|
| **Environment** | |
| `printenv` | Show environment variables. |
| `setenv` | Set or delete an environment variable. |
| `saveenv` | Save the environment to the firmware SD card. |
| `env` | Environment commands (default, delete, export, import, print, ...). |
| `grepenv` | Search the environment. |
| `setexpr` | Set a variable to the result of an expression. |
| `run` | Run the commands stored in an environment variable. |
| **Booting other images** | |
| `bootm` | Boot a U-Boot image from memory. |
| `go` | Start a program at an address. |
| `iminfo` | Show the header of a U-Boot image in memory. |
| `imxtract` | Extract one part of a multi-file image. |
| `fdt` | Inspect or change a device tree in memory. |
| `usbboot` | Load a U-Boot image from a USB disk. |
| **Disks and files** | |
| `sata` | SATA disks: scan, list, read and write blocks. |
| `usb` | USB: start, stop, reset, list devices and storage, read and write blocks. |
| `mmc`, `mmcinfo` | The SD card: list, select, read and write blocks; show card details. |
| `part` | List partitions and their details. |
| `fatinfo`, `fatls`, `fatload`, `fatsize` | FAT-only versions of `fstype`, `ls`, `load` and `size`. |
| `ext2ls`, `ext2load` | ext2-only versions of `ls` and `load`. |
| `save` | Write memory to a file (no filesystem in this build can be written). |
| `ln` | Create a symbolic link (no filesystem in this build can be written). |
| **Network** | |
| `dhcp`, `bootp` | Get an address by DHCP or BOOTP and load a file by TFTP. |
| `tftpboot` | Load a file by TFTP. |
| `ping` | Check that a host answers. |
| **Serial download** | |
| `loadb`, `loadx`, `loady` | Load a binary file over the serial line (Kermit, XMODEM, YMODEM). |
| `loads` | Load an S-Record file over the serial line. |
| **Memory** | |
| `md` | Display memory. |
| `mm` | Modify memory, one address after another. |
| `nm` | Modify memory at one address. |
| `mw` | Fill memory with a value. |
| `cp` | Copy memory. |
| `cmp` | Compare memory. |
| `crc32` | Compute the CRC32 of a memory area. |
| `base` | Show or set the base address for memory commands. |
| `loop` | Read a memory range in an endless loop (for hardware debugging). |
| **System information** | |
| `version` | Show the firmware version and the compiler used. |
| `bdinfo` | Show board information: memory, clocks, addresses. |
| `coninfo` | List the console devices. |
| `cpu` | Show the status of the CPU cores, or release one at an address. |
| `errata` | List the CPU errata workarounds in use. |
| `reginfo` | Show the CPU's memory-map registers. |
| `addrmap` | Show the virtual-to-physical address mappings. |
| `pci` | List PCI devices and read or write their configuration space. |
| `i2c` | Read and write devices on the I2C buses. |
| `irqinfo`, `interrupts` | Show the interrupt sources; switch interrupts on or off. |
| `date` | Show or set the real-time clock. |
| `mac` | Show or program the board's ID EEPROM (serial number, MAC addresses). |
| `bmp` | Show information about a BMP image in memory, or display it. |
| **Scripting** | |
| `echo` | Print its arguments. |
| `test`, `itest` | Compare strings, numbers or files, for `if` in scripts. |
| `true`, `false` | Succeed or fail, for scripts. |
| `sleep` | Wait a number of seconds. |
| `source` | Run a script from memory. |
| `exit` | Leave a script. |
| `showvar` | Show the shell's local variables. |
| `panic` | Stop with a message (for testing). |
| **Other** | |
| `help`, `?` | List the commands, or show the usage of one. |
| `reset` | Restart the machine (the same as `restart`). |
