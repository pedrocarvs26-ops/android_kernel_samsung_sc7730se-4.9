/*
 * SMP support for the Spreadtrum SC7730SE / SC8830 ("scx35") SoC family.
 *
 * These SoCs ship without PSCI or ARM Trusted Firmware.  Secondary cores
 * are powered down by the PMU_APB power controller and held in reset by
 * AP_AHB.  When a core is released from reset it fetches its boot address
 * from the AP_AHB CPU_JUMP registers, so the kernel just programs the
 * physical address of secondary_startup() there and powers the core up.
 *
 * Based on arch/arm/mach-sc/platsmp.c from the Spreadtrum/Samsung 3.10
 * vendor kernel, rewritten for the mainline 4.9 SMP API and made fully
 * device-tree driven (no static iomap, no __CPUINIT, no holding-pen
 * assembly trampoline).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "sprd-smp: " fmt

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/proc-fns.h>
#include <asm/smp.h>
#include <asm/smp_plat.h>
#include <asm/smp_scu.h>

#include "core.h"

/* AP_AHB block, 0x20d00000 on SC7730SE */
#define SPRD_AHB_CA7_RST_SET		0x0008
#define SPRD_AHB_HOLDING_PEN		0x004c
#define SPRD_AHB_CPU_JUMP		0x0050

/* PMU_APB block, 0x402b0000 on SC7730SE */
#define SPRD_PMU_PD_CA7_C1_CFG		0x0008
#define SPRD_PMU_PD_CA7_C2_CFG		0x000c
#define SPRD_PMU_PD_CA7_C3_CFG		0x0010
#define SPRD_PMU_PWR_STATUS0_DBG	0x00bc

#define PD_CA7_FORCE_SHUTDOWN		BIT(25)
#define PD_CA7_AUTO_SHUTDOWN_EN		BIT(24)
#define PD_CA7_PWR_ON_DLY(x)		(((x) << 16) & GENMASK(23, 16))
#define PD_CA7_PWR_ON_SEQ_DLY(x)	(((x) << 8) & GENMASK(15, 8))
#define PD_CA7_ISO_ON_DLY(x)		((x) & GENMASK(7, 0))

#define SPRD_PWR_STATE_MASK		0xf
#define SPRD_PWR_STATE_OFF		0x7

#define SPRD_MAX_CPUS			4
#define SPRD_PWR_RETRY			20

static void __iomem *sprd_ahb_base;
static void __iomem *sprd_pmu_base;
static DEFINE_SPINLOCK(sprd_boot_lock);

static u32 sprd_pd_cfg_offset(unsigned int cpu)
{
	switch (cpu) {
	case 1:
		return SPRD_PMU_PD_CA7_C1_CFG;
	case 2:
		return SPRD_PMU_PD_CA7_C2_CFG;
	case 3:
		return SPRD_PMU_PD_CA7_C3_CFG;
	default:
		return 0;
	}
}

static void sprd_set_cpu_reset(unsigned int cpu, bool assert)
{
	u32 val = readl(sprd_ahb_base + SPRD_AHB_CA7_RST_SET);

	if (assert)
		val |= BIT(cpu);
	else
		val &= ~BIT(cpu);

	writel(val, sprd_ahb_base + SPRD_AHB_CA7_RST_SET);
}

static int sprd_cpu_power_up(unsigned int cpu)
{
	void __iomem *pd = sprd_pmu_base + sprd_pd_cfg_offset(cpu);
	int i;
	u32 val;

	/* hold the core in reset while its power domain comes up */
	sprd_set_cpu_reset(cpu, true);

	/* power up / isolation timings, same values as the vendor kernel */
	writel(PD_CA7_PWR_ON_DLY(4) | PD_CA7_PWR_ON_SEQ_DLY(4) |
	       PD_CA7_ISO_ON_DLY(4), pd);

	val = (readl(pd) | PD_CA7_AUTO_SHUTDOWN_EN) & ~PD_CA7_FORCE_SHUTDOWN;
	writel(val, pd);
	udelay(1000);

	for (i = 0; i < SPRD_PWR_RETRY; i++) {
		if (!(readl(pd) & PD_CA7_FORCE_SHUTDOWN) &&
		    (readl(sprd_ahb_base + SPRD_AHB_CA7_RST_SET) & BIT(cpu)))
			break;

		pr_debug("retrying power-up of CPU%u\n", cpu);
		sprd_set_cpu_reset(cpu, true);
		val = (readl(pd) | PD_CA7_AUTO_SHUTDOWN_EN) &
			~PD_CA7_FORCE_SHUTDOWN;
		writel(val, pd);
		udelay(500);
	}

	if (i == SPRD_PWR_RETRY) {
		pr_err("failed to power up CPU%u\n", cpu);
		return -ETIMEDOUT;
	}

	/* release from reset: the core jumps to the programmed address */
	sprd_set_cpu_reset(cpu, false);

	return 0;
}

static void __init sprd_smp_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;

	np = of_find_compatible_node(NULL, NULL, "sprd,scx35-ap-ahb");
	if (np) {
		sprd_ahb_base = of_iomap(np, 0);
		of_node_put(np);
	}

	np = of_find_compatible_node(NULL, NULL, "sprd,scx35-pmu-apb");
	if (np) {
		sprd_pmu_base = of_iomap(np, 0);
		of_node_put(np);
	}

	if (!sprd_ahb_base || !sprd_pmu_base) {
		pr_err("AP_AHB/PMU_APB not found in DT, SMP disabled\n");
		return;
	}

#ifdef CONFIG_HAVE_ARM_SCU
	np = of_find_compatible_node(NULL, NULL, "arm,cortex-a7-scu");
	if (!np)
		np = of_find_compatible_node(NULL, NULL, "arm,cortex-a9-scu");
	if (np) {
		void __iomem *scu_base = of_iomap(np, 0);

		of_node_put(np);
		if (scu_base) {
			scu_enable(scu_base);
			iounmap(scu_base);
		}
	}
#endif
}

static int sprd_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long entry = virt_to_phys(secondary_startup);
	int ret;

	if (!sprd_ahb_base || !sprd_pmu_base)
		return -ENODEV;

	if (cpu == 0 || cpu >= SPRD_MAX_CPUS)
		return -EINVAL;

	spin_lock(&sprd_boot_lock);

	/* where the core should jump to once it leaves reset */
	writel(entry, sprd_ahb_base + SPRD_AHB_CPU_JUMP + (cpu << 2));

	/* let the boot ROM know this core is allowed to leave the pen */
	writel(readl(sprd_ahb_base + SPRD_AHB_HOLDING_PEN) | BIT(cpu),
	       sprd_ahb_base + SPRD_AHB_HOLDING_PEN);

	ret = sprd_cpu_power_up(cpu);

	spin_unlock(&sprd_boot_lock);

	if (ret)
		pr_err("failed to boot CPU%u (%d)\n", cpu, ret);

	return ret;
}

#ifdef CONFIG_HOTPLUG_CPU
static int sprd_cpu_power_down(unsigned int cpu)
{
	void __iomem *pd = sprd_pmu_base + sprd_pd_cfg_offset(cpu);
	int i;
	u32 val;

	val = (readl(pd) | PD_CA7_FORCE_SHUTDOWN) & ~PD_CA7_AUTO_SHUTDOWN_EN;
	writel(val, pd);

	for (i = 0; i < SPRD_PWR_RETRY; i++) {
		val = readl(sprd_pmu_base + SPRD_PMU_PWR_STATUS0_DBG);
		val = (val >> (4 * (cpu + 1))) & SPRD_PWR_STATE_MASK;
		if (val == SPRD_PWR_STATE_OFF)
			return 0;

		udelay(60);
	}

	return -ETIMEDOUT;
}

static void sprd_cpu_die(unsigned int cpu)
{
	v7_exit_coherency_flush(louis);

	/* the boot CPU powers us down through sprd_cpu_kill() */
	while (1)
		cpu_do_idle();
}

static int sprd_cpu_kill(unsigned int cpu)
{
	int ret;

	if (!sprd_pmu_base || !sprd_pd_cfg_offset(cpu))
		return 0;

	ret = sprd_cpu_power_down(cpu);
	if (ret) {
		pr_err("failed to power down CPU%u (%d)\n", cpu, ret);
		return 0;
	}

	/* clear the holding pen bit so the core stays parked */
	writel(readl(sprd_ahb_base + SPRD_AHB_HOLDING_PEN) & ~BIT(cpu),
	       sprd_ahb_base + SPRD_AHB_HOLDING_PEN);

	return 1;
}
#endif /* CONFIG_HOTPLUG_CPU */

const struct smp_operations sprd_smp_ops __initconst = {
	.smp_prepare_cpus	= sprd_smp_prepare_cpus,
	.smp_boot_secondary	= sprd_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_die		= sprd_cpu_die,
	.cpu_kill		= sprd_cpu_kill,
#endif
};
