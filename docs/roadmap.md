# Roadmap

Planned and possible work on this firmware, roughly in order of effort. None of it is a promise.

## Small

- **exFAT on USB sticks.** exFAT reads on some USB sticks hang after a few dozen USB transfers,
  while the same stick formatted FAT32 works (see [booting-amigaos.md](booting-amigaos.md)). The
  exFAT library re-reads the same FAT sector in many tiny reads; the plan is a small sector cache
  in `fs/exfat/io.c`, and no USB keyboard polling while `fwupdate` reads from USB.
- **Test the X5000/40 image on an X5000/40.** It is built from the same source (with four
  `cpu@N` nodes in the kernel's device tree) but has only run on an X5000/20 so far.
- **Faster kickstart loading.** Loading a full kickstart takes about 3.1-3.5 s; AmigaBoot took
  about 1.6 s. Larger reads per file should close the gap.
- **Boot AmigaOS from a USB device.** This should already work: the Amiga filesystem drivers and
  RDB partition scanning do not depend on the device, and `boota` scans USB after SATA
  (`os4_bootdevice=auto`). It needs a test with a USB device holding a real AmigaOS installation
  (installed to the device, not copied file by file).
- **FFS on real disks.** FFS reads (`ls`, `load`, booting) have been tested only on disk images,
  not yet on a real X5000 disk. NGFS and SFS (including SFS\2) are tested on real disks.

## Medium

- **Network boot of the kickstart.** `boota` could load the Kicklayout files over TFTP instead
  of from disk. AmigaOS itself would still be on a local disk.
- **Boot screen artwork.** The boot screen is plain black with a progress bar
  (`video_splash_blank()` and `video_progress_bar()` in `drivers/video/cfb_console.c`). Artwork
  owned by this project could be added with BMP support (`CONFIG_CMD_BMP` is enabled); animation
  would be limited by the speed of the uncached PCIe framebuffer.

## Large

- **NVMe boot.** `boota` needs only U-Boot's NVMe driver (in mainline) and `nvme` in the
  `os4_bootdevice` list; the filesystem and Kicklayout code is device-independent. AmigaOS would
  also need an NVMe driver of its own.
- **Booting from CD/DVD or an ISO image.** This needs an ISO9660 filesystem driver (there is none in
  this tree; `disk/part_iso.c` handles only the partition and El Torito layer) and ATAPI support in
  the SATA driver (`fsl_sata.c` does not support ATAPI). Booting an ISO file from another
  filesystem would also need a file-backed block device, which U-Boot does not have.
- **Linux boot.** U-Boot's usual Linux boot paths (ext4/FAT, `bootm`) are present but not tested
  on this board.
- **A newer U-Boot.** The port uses U-Boot's legacy drivers, without driver model (see
  [building.md](building.md#configuration-kconfig-and-driver-model)). Moving to a current U-Boot
  release means converting the board to driver model and a control device tree, then porting
  this port's changes forward.

## Upstream

- Some general fixes, such as the EHCI queue lifetime handling and cache maintenance on
  hardware-coherent Freescale platforms, could be offered to mainline U-Boot, adapted to current
  mainline.
