/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) SMP Support
 *
 * Adapted 1:1 from arch/arm/mach-sc/platsmp.c in the working Samsung
 * SM-T560 / SM-T561 3.10 kernel.
 *
 * Key hardware facts on SC7730SE (Cortex-A7 MPCore / TShark2 SCX30G):
 * 1. Cortex-A7 MPIDR[23:0] values are 0xf00, 0xf01, 0xf02, 0xf03.
 * 2. Cortex-A7 MPCore has NO SCU MMIO block at 0x12000000.
 * 3. On SCX30G (SC7730SE), REG_AP_AHB_HOLDING_PEN is at offset 0x0014 and
 *    REG_AP_AHB_JMP_ADDR_CA7_C0..C3 are at 0x0018..0x0024.
 *
 * Licensed under the GPL-2.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/irqchip/arm-gic.h>

#include <asm/cacheflush.h>
#include <asm/cp15.h>
#include <asm/smp_plat.h>

#include <soc/sprd/iomap.h>
#include "core.h"

extern void sci_secondary_startup(void);

static DEFINE_SPINLOCK(boot_lock);

#define CPU_UP_KEY_PART		0xbadf1a90
#define CPU_UP_SUCCESS_KEY	0x63507530

#define BITS_PD_CA7_PWR_ON_DLY(x)	(((x) & 0xFF) << 16)
#define BITS_PD_CA7_PWR_ON_SEQ_DLY(x)	(((x) & 0xFF) << 8)
#define BITS_PD_CA7_ISO_ON_DLY(x)	((x) & 0xFF)

static const int g_sprd_up_flag[4] = {
	(CPU_UP_SUCCESS_KEY + 0),
	(CPU_UP_SUCCESS_KEY + 1),
	(CPU_UP_SUCCESS_KEY + 2),
	(CPU_UP_SUCCESS_KEY + 3),
};

static void write_pen_release(int val)
{
	pen_release = val;
	smp_wmb();
	__cpuc_flush_dcache_area((void *)&pen_release, sizeof(pen_release));
	outer_clean_range(__pa(&pen_release), __pa(&pen_release + 1));
}

static int poweron_cpus(int cpu)
{
	unsigned long poweron;
	u32 val;
	int tries = 20;

	if (cpu == 1)
		poweron = REG_PMU_APB_PD_CA7_C1_CFG;
	else if (cpu == 2)
		poweron = REG_PMU_APB_PD_CA7_C2_CFG;
	else if (cpu == 3)
		poweron = REG_PMU_APB_PD_CA7_C3_CFG;
	else
		return -EINVAL;

	val = sci_glb_read(REG_AP_AHB_CA7_RST_SET, -1UL);
	val |= BIT(cpu);
	sci_glb_write(REG_AP_AHB_CA7_RST_SET, val, -1UL);

	val = BITS_PD_CA7_PWR_ON_DLY(4) |
	      BITS_PD_CA7_PWR_ON_SEQ_DLY(4) |
	      BITS_PD_CA7_ISO_ON_DLY(4);
	sci_glb_write(poweron, val, -1UL);

	val = (BIT_PD_CA7_C1_AUTO_SHUTDOWN_EN | __raw_readl((void __iomem *)poweron)) &
	      ~(BIT_PD_CA7_C1_FORCE_SHUTDOWN);
	sci_glb_write(poweron, val, -1UL);
	dmb();
	udelay(1000);

	while (((__raw_readl((void __iomem *)poweron) & BIT_PD_CA7_C1_FORCE_SHUTDOWN) ||
		!(__raw_readl((void __iomem *)REG_AP_AHB_CA7_RST_SET) & BIT(cpu))) &&
	       --tries > 0) {
		writel((__raw_readl((void __iomem *)REG_AP_AHB_CA7_RST_SET) | BIT(cpu)),
		       (void __iomem *)REG_AP_AHB_CA7_RST_SET);
		val = (BIT_PD_CA7_C1_AUTO_SHUTDOWN_EN | __raw_readl((void __iomem *)poweron)) &
		      ~(BIT_PD_CA7_C1_FORCE_SHUTDOWN);
		writel(val, (void __iomem *)poweron);
		dmb();
		udelay(500);
	}

	writel((__raw_readl((void __iomem *)REG_AP_AHB_CA7_RST_SET) & ~BIT(cpu)),
	       (void __iomem *)REG_AP_AHB_CA7_RST_SET);
	return 0;
}

static int boot_secondary_cpus(int cpu_id, u32 paddr)
{
	/* Extract core index 1..3 in case MPIDR (0xf01..0xf03) was passed */
	cpu_id &= 0xf;
	if (cpu_id < 1 || cpu_id > 3)
		return -EINVAL;

	/* Write secondary startup physical address to both SCX30G (0x18) and legacy (0x50) regs */
	writel(paddr, (void __iomem *)(REG_AP_AHB_JMP_ADDR_CA7_C0 + (cpu_id << 2)));
	writel(paddr, (void __iomem *)(REG_AP_AHB_JMP_ADDR_CA7_C0_LEGACY + (cpu_id << 2)));

	/* Set holding pen bit for this CPU on both SCX30G (0x14) and legacy (0x4C) regs */
	writel(readl((void __iomem *)REG_AP_AHB_HOLDING_PEN) | BIT(cpu_id),
	       (void __iomem *)REG_AP_AHB_HOLDING_PEN);
	writel(readl((void __iomem *)REG_AP_AHB_HOLDING_PEN_LEGACY) | BIT(cpu_id),
	       (void __iomem *)REG_AP_AHB_HOLDING_PEN_LEGACY);

	poweron_cpus(cpu_id);
	return 0;
}

static void sprd_secondary_init(unsigned int cpu)
{
	/* Signal CPU bringup completion to primary CPU */
	write_pen_release(g_sprd_up_flag[cpu]);

	/* Synchronize with the boot thread */
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

static int sprd_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;
	int ret;

	spin_lock(&boot_lock);

	/*
	 * Set synchronisation state between this boot processor
	 * and the secondary one (`0xbadf1a90 | cpu_logical_map(cpu)`).
	 */
	write_pen_release(cpu_logical_map(cpu) | (int)CPU_UP_KEY_PART);

	ret = boot_secondary_cpus(cpu, virt_to_phys(sci_secondary_startup));
	if (ret < 0)
		pr_warn("SPRD SMP: boot_secondary(%u) error %d\n", cpu, ret);

	dsb_sev();

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();
		if (pen_release == g_sprd_up_flag[cpu])
			break;

		udelay(10);
		dsb_sev();
	}

	spin_unlock(&boot_lock);

	if (pen_release != g_sprd_up_flag[cpu]) {
		pr_err("SPRD SMP: CPU%u failed to come online (pen_release=0x%08x, RST_SET=0x%08x)\n",
		       cpu, pen_release,
		       sci_glb_read(REG_AP_AHB_CA7_RST_SET, -1UL));
		return -ENOSYS;
	}

	return 0;
}

static void __init sprd_smp_init_cpus(void)
{
	unsigned int i, ncores;

	/* Read Cortex-A7 L2CTLR[25:24] (+1 = number of cores in cluster) */
	asm volatile("mrc p15, 1, %0, c9, c0, 2\n" : "=r" (ncores));
	ncores = ((ncores >> 24) & 0x3) + 1;

	if (ncores > nr_cpu_ids) {
		pr_warn("SMP: %u cores greater than maximum (%u), clipping\n",
			ncores, nr_cpu_ids);
		ncores = nr_cpu_ids;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);
}

static void __init sprd_smp_prepare_cpus(unsigned int max_cpus)
{
	int cpu_count = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		set_cpu_present(cpu, true);
		cpu_count++;
		if (cpu_count >= (int)max_cpus)
			break;
	}
}

#ifdef CONFIG_HOTPLUG_CPU
static int sprd_cpu_kill(unsigned int cpu)
{
	unsigned long pmu_cfg;
	u32 val;

	if (cpu == 0 || cpu > 3)
		return 0;

	mdelay(2);
	sci_glb_set(REG_AP_AHB_CA7_RST_SET, BIT(cpu));

	pmu_cfg = REG_PMU_APB_PD_CA7_C0_CFG + (cpu << 2);
	val = __raw_readl((void __iomem *)pmu_cfg);
	val |= BIT_PD_CA7_C1_FORCE_SHUTDOWN;
	val &= ~BIT_PD_CA7_C1_AUTO_SHUTDOWN_EN;
	__raw_writel(val, (void __iomem *)pmu_cfg);

	return 1;
}

static void sprd_cpu_die(unsigned int cpu)
{
	v7_exit_coherency_flush(louis);

	for (;;) {
		wfi();
		if (pen_release == (int)(cpu_logical_map(cpu) | CPU_UP_KEY_PART))
			break;
	}

	/* Soft restart secondary core */
	sci_secondary_startup();
}
#endif

static const struct smp_operations sprd_smp_ops __initconst = {
	.smp_init_cpus		= sprd_smp_init_cpus,
	.smp_prepare_cpus	= sprd_smp_prepare_cpus,
	.smp_secondary_init	= sprd_secondary_init,
	.smp_boot_secondary	= sprd_boot_secondary,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_kill		= sprd_cpu_kill,
	.cpu_die		= sprd_cpu_die,
#endif
};

CPU_METHOD_OF_DECLARE(sprd_sc7730se_smp, "sprd,sc7730se-smp", &sprd_smp_ops);
CPU_METHOD_OF_DECLARE(sprd_sc8830_smp, "sprd,sc8830-smp", &sprd_smp_ops);
