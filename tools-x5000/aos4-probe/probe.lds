/* SPDX-License-Identifier: GPL-2.0+ */
/* Copyright (C) 2026 Richard Gibbs */
OUTPUT_ARCH(powerpc)
ENTRY(_start)

SECTIONS
{
	/* Where Kickstart/loader runs too; a loader must keep it free. */
	. = 0x01000000;
	.text : {
		*(.text.start)
		*(.text*)
	}
	.rodata : { *(.rodata*) }
	.data : { *(.data*) *(.sdata*) }
	. = ALIGN(4);
	__image_end = .;

	.bss (NOLOAD) : {
		__bss_start = .;
		*(.sbss*) *(.bss*) *(COMMON)
		. = ALIGN(16);
		. += 0x4000;
		stack0_top = .;
		. += 0x1000;
		stack1_top = .;
		__bss_end = .;
	}

	/DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.got*) }
}
