/*
 * Spreadtrum SC7730SE / SC8830 (scx35 / scx30g) secondary CPU holding pen.
 *
 * Ported from arch/arm/mach-sc/headsmp.S (Linux 3.10 vendor kernel).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/linkage.h>
#include <linux/init.h>

/*
 * Secondary CPUs jump here in physical mode after leaving reset.
 * Hold each core in a software pen until the primary CPU writes its
 * MPIDR core index (0..3) into pen_release, so secondary_data.stack
 * in arch/arm/kernel/head.S is never accessed concurrently by two cores.
 */
ENTRY(sci_secondary_startup)
	mrc	p15, 0, r0, c0, c0, 5
	and	r0, r0, #15		/* MPIDR is 0xf00..0xf03 */
	adr	r4, 1f
	ldmia	r4, {r5, r6}
	sub	r4, r4, r5
	add	r6, r6, r4
pen:	ldr	r7, [r6]
	and	r7, r7, #15
	cmp	r7, r0
	bne	pen

	b	secondary_startup
ENDPROC(sci_secondary_startup)

	.align	2
1:	.long	.
	.long	pen_release
