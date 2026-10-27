/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * aos4-probe entry. It is a Kicklayout EXEC: a flat binary that the loader
 * copies anywhere and enters at byte 0 with
 *   r3 = "AmigaOS4", r4 = module list, r5 = board info, r6 = command line.
 * Like Kickstart/loader, it first moves itself to its link address.
 *
 * Copyright (C) 2026 Richard Gibbs
 */

	.section .text.start, "ax"
	.globl	_start
_start:
	mr	r24, r3
	mr	r25, r4
	mr	r26, r5
	mr	r27, r6
	mr	r28, r1
	mflr	r29
	bl	1f
1:	mflr	r20
	addi	r20, r20, -(1b - _start)	/* where we were loaded */
	lis	r21, _start@ha
	addi	r21, r21, _start@l		/* where we are linked */
	cmpw	r20, r21
	beq	4f

	lis	r22, __image_end@ha
	addi	r22, r22, __image_end@l
	subf	r23, r21, r22
	srwi	r23, r23, 2
	mtctr	r23
	cmplw	r20, r21
	blt	3f

	/* source above destination: copy forwards */
	mr	r8, r20
	mr	r9, r21
2:	lwz	r0, 0(r8)
	stw	r0, 0(r9)
	dcbst	0, r9
	sync
	icbi	0, r9
	addi	r8, r8, 4
	addi	r9, r9, 4
	bdnz	2b
	b	5f

	/* source below destination: copy backwards, in case they overlap */
3:	slwi	r0, r23, 2
	add	r8, r20, r0
	add	r9, r21, r0
6:	lwzu	r0, -4(r8)
	stwu	r0, -4(r9)
	dcbst	0, r9
	sync
	icbi	0, r9
	bdnz	6b

5:	sync
	isync
4:	lis	r11, linked@ha
	addi	r11, r11, linked@l
	mtctr	r11
	bctr

linked:
	lis	r8, __bss_start@ha
	addi	r8, r8, __bss_start@l
	lis	r9, __bss_end@ha
	addi	r9, r9, __bss_end@l
	li	r0, 0
7:	cmplw	r8, r9
	bge	8f
	stw	r0, 0(r8)
	addi	r8, r8, 4
	b	7b

8:	lis	r1, stack0_top@ha
	addi	r1, r1, stack0_top@l
	stwu	r0, -16(r1)
	mr	r3, r24
	mr	r4, r25
	mr	r5, r26
	mr	r6, r27
	mr	r7, r28
	mr	r8, r29
	mr	r9, r20
	bl	probe_main
9:	b	9b

/*
 * chain_to(entry, r3, r4, r5, r6, sp, lr): enter the real loader with the
 * registers and stack this probe was entered with. Does not return.
 */
	.globl	chain_to
chain_to:
	mtctr	r3
	mr	r1, r8
	mtlr	r9
	mr	r3, r4
	mr	r4, r5
	mr	r5, r6
	mr	r6, r7
	bctr

/*
 * Secondary core, released through a spin table. U-Boot's release code
 * has mapped the 64 MiB around this address 1:1 and put the spin
 * table's r3 value in r3.
 */
	.globl	core1_entry
core1_entry:
	lis	r1, stack1_top@ha
	addi	r1, r1, stack1_top@l
	li	r0, 0
	stwu	r0, -16(r1)
	bl	core1_main
10:	b	10b
