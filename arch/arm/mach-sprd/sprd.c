/*
 * Spreadtrum SC7730SE (SC8830 / "scx35" family) machine support.
 *
 * Device-tree only machine descriptor for the quad Cortex-A7 SC7730SE
 * used by the Samsung Galaxy Tab E 9.6 (SM-T560 / SM-T561).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>

#include <asm/mach/arch.h>

#include "core.h"

static const char * const sprd_scx35_compat[] __initconst = {
	"sprd,sc7730se",
	"sprd,sc7730",
	"sprd,sc8830",
	"sprd,scx35",
	NULL,
};

DT_MACHINE_START(SPRD_SCX35_DT, "Spreadtrum SC7730SE (Device Tree)")
	.dt_compat	= sprd_scx35_compat,
	.smp		= smp_ops(sprd_smp_ops),
MACHINE_END
