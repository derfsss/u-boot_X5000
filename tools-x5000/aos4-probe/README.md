# aos4-probe

A developer tool for checking the AmigaOS 4 boot handoff on the AmigaOne X5000. It is not part of
the firmware and is not needed to boot AmigaOS.

`aos4probe` is a stand-in for `Kickstart/loader`, the EXEC in a Kicklayout entry. The boot loader
(normally U-Boot's `boota`) loads it in place of the AmigaOS 4 kernel loader. It prints on the serial
port (115200 8N1, DUART1 port 0) everything the loader handed over. Use it to check what `boota`
passes to the kernel loader, or to compare another loader's handoff (BBoot, for example) with
`boota`'s by diffing the two outputs.

It prints:

- r1, LR and r3–r6 at entry (r3 should be `"AmigaOS4"`, r6 the command line);
- the CPU state: MSR, PIR, PVR, SVR, HID0, L1CSR0/1, L2CSR0, EPCR, IVPR, TCR, TSR, DEC (read
  twice, to show whether it is counting) and the time base;
- every valid TLB1 entry;
- the module list (an Exec MinList): each node's id, flags, address, size, alloc/dealloc fields,
  the first word of its data and its name;
- the board info at r5 as a hexdump, with the known fields decoded, and the X5000 tail if the
  0x12345678 magic is at +60;
- any device tree it finds: at r3, in a module named `dtb`, or at the board info's +68 pointer.

If the module list also holds the real loader (a MODULE named `loader`), the probe hands over to it
after printing. It first makes the list look as if the loader had been the EXEC, so the machine boots
on as normal. Without it, the probe stops.

If the command line contains `probe_smp`, or a module named `probe_smp` is loaded, it instead
releases core 1 through the `cpu-release-addr` of `cpu@1` in the device tree (the ePAPR spin-table
protocol, which U-Boot's mpc85xx code implements), and reports whether core 1 came up. See
`docs/booting-amigaos.md` for how `boota` leaves the secondary core.

Like `Kickstart/loader`, it is a flat binary that can be loaded at any address. It first copies
itself to 0x01000000 and runs there.

## Build

With any PowerPC cross toolchain, from the root of the U-Boot tree:

```
make -C tools-x5000/aos4-probe CROSS_COMPILE=powerpc-linux-gnu-
```

(`docker-x5000/Dockerfile` provides a toolchain; `CROSS_COMPILE` is already set in it.)

This produces `aos4probe` (about 6 KB).

## Use on the X5000

1. Copy `aos4probe` to `SYS:Kickstart/` on the boot partition.
2. Add a configuration to the end of `SYS:Kickstart/Kicklayout`, with an empty line before it:

   ```
   LABEL aos4 probe
   EXEC Kickstart/aos4probe
   MODULE Kickstart/loader
   MODULE Kickstart/kernel
   ...the rest of the normal configuration's MODULE lines...
   ```

3. Connect a serial terminal at 115200 baud, and pick the "aos4 probe" entry in the `boota` menu
   (it appears as its own entry). The probe prints its report, then AmigaOS boots as usual.
4. To test the core 1 release instead, also add `MODULE Kickstart/probe_smp` (any small file
   with that name). The probe then stops after the test; reset the machine.
