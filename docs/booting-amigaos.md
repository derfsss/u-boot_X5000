# Booting AmigaOS 4 with `boota`

U-Boot boots AmigaOS 4.1 itself. The `boota` command finds the AmigaOS volumes on the attached
disks, shows a boot menu, loads the chosen kickstart and starts the kernel. No A-Eon software is
involved: the original X5000 firmware started A-Eon's AmigaBoot loader to do this, and `boota`
replaces it. It is the default boot command (`bootcmd=boota`).

The code is in `cmd/boota.c` and `common/aos4/`. It is shared with the A1222 port.

## What `boota` does

1. Starts USB (`usb start`) and SATA (`sata init`).
2. Scans the devices named by `os4_bootdevice` (`auto`: SATA, then USB) for Amiga RDB partitions
   with a filesystem U-Boot can read: NGFS (`NGF\0`, `NGF\1`), SFS (`SFS\0`, `SFS\2`) and
   FFS/OFS (`DOS\x`). See [ngfs-format.md](ngfs-format.md) for NGFS.
3. Reads `Kickstart/Kicklayout` from each such partition. Every `LABEL` in it becomes a menu
   entry: the partition's RDB drive name (e.g. `DH0`) and the label.
4. Shows the menu, or with `amigaboot_quiet=y` picks the default entry straight away.
5. Loads the entry's `EXEC` (the kernel loader, normally `Kickstart/loader`) and its `MODULE`
   files into memory, builds the module list, board information and device tree described below,
   and enters the kernel loader.

The default entry is the first configuration on the partition with the highest RDB boot priority.

## The boot menu

The menu is shown on the serial console and on the screen.

| Key | Action |
|---|---|
| `1`-`9`, `A`-`Z` | boot that entry |
| UP / DOWN | move the selection |
| RETURN | boot the selected entry |
| SPACE | stop the countdown |
| Ctrl-C | leave the menu for the U-Boot prompt |

When the countdown (`boota_timeout`) runs out, the selected entry boots. While the kickstart
loads, a progress bar shows each file; then the screen is cleared and the kernel starts. If a
`MODULE` cannot be read, `boota` asks whether to boot without it (C) or abort (A). A missing kernel
loader always aborts.

From the prompt:

```
boota          show the menu
boota 2        boot entry 2 without the menu (1-9, A-Z)
boota -n       load the kickstart but do not start it; print the module list,
               the board information, the device tree and a CRC32 per file
```

## Settings

| Variable | Default | Meaning |
|---|---|---|
| `bootcmd` | `boota` | What runs when the autoboot countdown ends. |
| `bootdelay` | `3` | Seconds of autoboot countdown before `bootcmd` runs. A key press stops it. |
| `boota_timeout` | not set (5) | Seconds the menu waits before booting the selected entry. `0` waits for a key. |
| `amigaboot_quiet` | `n` (debug build), `y` (production build) | `y`: no menu, boot the default entry. (The name is the one the original firmware used, so existing environments keep working.) |
| `bootable_only` | not set (`n`) | `y`: list only partitions marked bootable in the RDB. |
| `os4_bootdevice` | `auto` | Where to look: `auto` (SATA, then USB), or a list of interface names such as `usb,sata`. |
| `os4_commandline` | `serial munge debuglevel=1` (debug), `serial munge debuglevel=0` (production) | Kernel command line. `debuglevel=0` gives a quieter serial log. |

The debug and production builds differ only in these two defaults (see [building.md](building.md)).
Change a setting with `setenv name value` and keep it with `saveenv`.

## The kernel handoff

This section is for developers. It describes what the AmigaOS kernel loader receives. The same
interface is used by A-Eon's AmigaBoot and by BBoot; it was measured on an X5000/20 with
`tools-x5000/aos4-probe`, a stand-in loader that prints what it is given.

### Memory

The kickstart goes in a 64 MiB area that ends 1 MiB below U-Boot's global data (just under U-Boot
in RAM). In order, from the bottom of the area:

1. the module list header;
2. each file in Kicklayout order (the EXEC first), each starting on a 32-byte boundary, followed
   directly by its 36-byte module node (no alignment) and its name string;
3. the board information (4-byte aligned), then the device tree (8-byte aligned, 8 KiB reserved);
4. the strings `"AmigaOS4"` and the command line.

The kernel loader copies itself to 0x01000000-0x010078A4 and runs there, so that range must be free
when it starts; the kickstart area is far above it.

### Entry

The EXEC is a flat, position-independent binary entered at its first byte:

| Register | Value |
|---|---|
| r3 | pointer to the string `"AmigaOS4"` |
| r4 | module list |
| r5 | board information |
| r6 | command line (`os4_commandline`) |
| r1 | U-Boot's stack |

The CPU is left as U-Boot runs it: MMU on with U-Boot's TLB1 map (RAM 0-2 GiB coherent, CCSR, the
PCIe windows, U-Boot itself, the DPAA windows), U-Boot's exception vectors, L1 and L2 caches on.
The decrementer interrupt is switched off (TCR[DIE] cleared) before the call, so U-Boot's timer
tick cannot fire into the kernel loader.

### Module list

An Exec `MinList` (`head`, `tail` = 0, `tailpred`) of 36-byte nodes, all fields big-endian:

| Offset | Field |
|---|---|
| +0 / +4 | `succ` / `pred` |
| +8 | name: pointer to the file's base name, e.g. `"kernel"` |
| +12 | options, 0 |
| +16 | u16 id: 1 = EXEC (the first node), 2 = MODULE |
| +18 | u16 flags: 3 |
| +20 / +24 | alloc / dealloc: 0 here (the kernel does not use them) |
| +28 | address of the file data |
| +32 | size of the file in bytes |

There is no device-tree module; the device tree is passed through the board information.

### Board information (r5)

A 72-byte structure: the U-Boot `bd_t` layout followed by an X5000 tail.

| Offset | Value |
|---|---|
| +0 | memory start, 0 |
| +4 | memory size passed to the kernel (see "Second core" below; at most 2 GiB) |
| +44 | CPU clock, Hz |
| +48 | bus clock, Hz |
| +52 | serial baud rate |
| +60 | tail magic `0x12345678` |
| +64 | u16 tail version, 0 |
| +66 | u16 structure size, 72 |
| +68 | pointer to the device tree |

All other words are 0.

### Device tree

A small flattened device tree (version 17), placed after the board information:

```
/ {
    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        cpu@0 {
            device_type = "cpu";
            reg = <0>;
            enable-method = "spin-table";
            cpu-release-addr = /bits/ 64 <spin_table + 0x00>;
        };
        cpu@1 { ... reg = <1>; cpu-release-addr = /bits/ 64 <spin_table + 0x40>; };
    };
    memory {
        device_type = "memory";
        reg = <0 memstart 0 memsize>;
    };
};
```

There is one `cpu@N` node per core: two on the X5000/20, four on the X5000/40.

### Second core (SMP)

U-Boot parks the secondary cores in its spin table (the ePAPR spin-table protocol of the mpc85xx
code), with one 64-byte entry per core. `cpu-release-addr` of `cpu@N` is the address of entry N, so a
multicore kernel can start the other cores by writing an entry address there. The board's TLB setup
maps the reset page (0xFFFFF000) so the boot page for the secondary cores can be installed in the
two-stage build.

When a core is parked, the memory size given to the kernel ends below U-Boot (at U-Boot's
relocation address, rounded down to 1 MiB), so the spin loop is never overwritten. The current
single-core AmigaOS kernel boots normally with the other cores parked. Releasing core 1 through the
spin table has been tested with `aos4-probe` (its `probe_smp` mode).

## `fwupdate`: updating the firmware

`fwupdate` writes a new firmware file to the firmware SD card from inside U-Boot, so the card does
not have to come out of the machine. It is available on the X5000 only.

```
fwupdate [-d] DRIVE:file [crc32]
fwupdate [-d] interface dev[:part] file [crc32]
```

- `DRIVE:file` reads from an AmigaOS partition by its RDB drive name, e.g.
  `fwupdate DH3:x5000-openuboot-spl.img`.
- `interface dev[:part] file` reads from any U-Boot device, e.g.
  `fwupdate usb 0:1 x5000-openuboot-spl.img` for a FAT32 USB stick. Plug the stick in before
  switching on.
- The file is either a whole SD card image, or a U-Boot image for the 2 MiB U-Boot slot alone
  (`u-boot.bin`, or a zero-padded 2 MiB slot).
- `crc32` (hex) makes it refuse a file whose CRC32 differs, for example a damaged download.
- A whole image keeps the card's settings: the AmigaOS settings block at 0x89200 and the U-Boot
  environment at 0x150000. `-d` writes the image's default settings instead.

`fwupdate` shows the firmware on the card and the firmware in the file, warns if the image's reset
configuration (RCW) differs from the card's, reminds you not to switch off and to keep a spare
card, and asks for confirmation before writing anything. It then saves the current card contents
in memory, writes, reads the card back and compares. If the check fails it writes the old firmware
back. At the end it asks for a key press and restarts. `help fwupdate` gives the full text.

Keep a working spare SD card, and do not switch off while it is writing.

### Upgrading from an image with the old layout

Earlier images of this firmware kept the main U-Boot in a 768 KiB slot at 0x8C000. It now has a
2 MiB slot at 0x200000 (see [sd-card-layout.md](sd-card-layout.md)). A card with the old layout
must be moved once by writing a whole new image to it with an SD card writer: the old firmware
looks for U-Boot at 0x8C000 and its `fwupdate` refuses the new image, and the new `fwupdate`
refuses to write a U-Boot slot on a card that has no U-Boot at 0x200000. Later updates can use
`fwupdate`.

## Known issue: exFAT USB sticks

U-Boot can read exFAT (mainline U-Boot's driver, read-only), but on some USB sticks exFAT reads
stop after a few dozen USB transfers and the command hangs. The same stick formatted FAT32 works.
Use FAT32 for USB sticks, including for `fwupdate`. See [roadmap.md](roadmap.md).

## Other commands

- `shutdown` powers the machine off through the X5000's supervisor MCU.
- `restart` restarts the machine (the same as `reset`).

All the commands are described in [commands.md](commands.md).
