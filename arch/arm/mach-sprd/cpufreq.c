/*
 * Spreadtrum SC7730SE (scx35 / scx30g) CPU Frequency Scaling Driver
 *
 * Adapted from arch/arm/mach-sc/cpufreq-scx35.c (3.10 kernel) for Linux 4.9.
 *
 * Controls Cortex-A7 quad-core DVFS by dynamically reparenting clk_mcu
 * through clk_tdpll (768 MHz) while re-locking clk_mpll (729..1300 MHz),
 * and adjusting vddarm regulator voltage via the SC2723 PMIC.
 *
 * Licensed under the GPL-2.
 */

#define pr_fmt(fmt) "sprd-cpufreq: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/cpu.h>
#include <linux/regulator/consumer.h>
#include <linux/mutex.h>
#include <soc/sprd/iomap.h>

#define SHARK_TDPLL_FREQ 768000

#ifndef REG_PMU_APB_MPLL_REL_CFG
#define REG_PMU_APB_MPLL_REL_CFG	SCI_ADDR(REGS_PMU_APB_BASE, 0x0094)
#endif

#ifndef BIT_MPLL_AP_SEL
#define BIT_MPLL_AP_SEL			BIT(0)
#endif

#ifndef BITS_CA7_MCU_CKG_DIV
#define BITS_CA7_MCU_CKG_DIV(_X_)	(((_X_) << 4) & (BIT(4)|BIT(5)|BIT(6)))
#endif

static DEFINE_MUTEX(sprd_cpufreq_lock);
static struct clk *clk_mcu;
static struct clk *clk_mpll;
static struct clk *clk_tdpll;
static struct regulator *reg_vddarm;

/*
 * Operating Performance Points for SC7730SE Cortex-A7 (TShark2).
 * Frequencies match sc8830t_cpufreq_table_data_es_1300 from vendor 3.10 kernel,
 * sorted in ascending order.
 */
static struct cpufreq_frequency_table sprd_freq_table[] = {
	{ .driver_data = 0, .frequency =  729600 },
	{ .driver_data = 1, .frequency =  768000 },
	{ .driver_data = 2, .frequency =  800000 },
	{ .driver_data = 3, .frequency =  960000 },
	{ .driver_data = 4, .frequency = 1036800 },
	{ .driver_data = 5, .frequency = 1190400 },
	{ .driver_data = 6, .frequency = 1200000 },
	{ .driver_data = 7, .frequency = 1300000 },
	{ .driver_data = 8, .frequency = CPUFREQ_TABLE_END },
};

/* Voltage targets in microvolts (uV) for each frequency OPP */
static const unsigned int sprd_vddarm_uv[] = {
	 880000, /*  729 MHz -> 0.880 V */
	 895000, /*  768 MHz -> 0.895 V */
	 910000, /*  800 MHz -> 0.910 V */
	 925000, /*  960 MHz -> 0.925 V */
	 940000, /* 1036 MHz -> 0.940 V */
	 955000, /* 1190 MHz -> 0.955 V */
	 970000, /* 1200 MHz -> 0.970 V */
	 985000, /* 1300 MHz -> 0.985 V */
};

static void sprd_set_clock(unsigned int freq)
{
	int ret;

	/*
	 * When reprogramming MPLL, Cortex-A7 must not run directly from it,
	 * as MPLL may unlock or glitch during the multiplier write.
	 * Temporarily reparent clk_mcu to clk_tdpll (fixed, stable 768 MHz).
	 */
	ret = clk_set_parent(clk_mcu, clk_tdpll);
	if (ret)
		pr_err("failed to reparent clk_mcu to tdpll: %d\n", ret);

	if (freq == SHARK_TDPLL_FREQ) {
		/* Stay on TDPLL at 768 MHz, clear divider */
		sci_glb_clr(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
	} else if (freq == SHARK_TDPLL_FREQ / 2) {
		/* 384 MHz: divide TDPLL by 2 and release MPLL for power saving */
		sci_glb_set(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
		sci_glb_clr(REG_PMU_APB_MPLL_REL_CFG, BIT_MPLL_AP_SEL);
	} else {
		/* Ensure MPLL AP output gate is asserted */
		if (!(sci_glb_read(REG_PMU_APB_MPLL_REL_CFG, BIT_MPLL_AP_SEL))) {
			sci_glb_set(REG_PMU_APB_MPLL_REL_CFG, BIT_MPLL_AP_SEL);
			udelay(500);
		}

		/* Set new MPLL rate */
		ret = clk_set_rate(clk_mpll, (unsigned long)freq * 1000);
		if (ret)
			pr_err("failed to set clk_mpll rate to %u kHz: %d\n", freq, ret);

		/* Switch clk_mcu back to clk_mpll */
		ret = clk_set_parent(clk_mcu, clk_mpll);
		if (ret)
			pr_err("failed to reparent clk_mcu to mpll: %d\n", ret);

		/* Clear divider */
		sci_glb_clr(REG_AP_AHB_CA7_CKG_CFG, BITS_CA7_MCU_CKG_DIV(1));
	}
}

static int sprd_cpufreq_target_index(struct cpufreq_policy *policy, unsigned int index)
{
	unsigned int new_freq = sprd_freq_table[index].frequency;
	unsigned int old_freq = policy->cur;
	unsigned int target_uv = sprd_vddarm_uv[index];
	int ret;

	if (old_freq == new_freq)
		return 0;

	mutex_lock(&sprd_cpufreq_lock);

	/* Scaling UP: increase voltage first, then frequency */
	if (new_freq > old_freq) {
		if (reg_vddarm) {
			ret = regulator_set_voltage(reg_vddarm, target_uv, target_uv);
			if (ret)
				pr_warn("failed to raise vddarm to %u uV: %d\n", target_uv, ret);
		}
		sprd_set_clock(new_freq);
	} else {
		/* Scaling DOWN: decrease frequency first, then voltage */
		sprd_set_clock(new_freq);
		if (reg_vddarm) {
			ret = regulator_set_voltage(reg_vddarm, target_uv, target_uv);
			if (ret)
				pr_warn("failed to lower vddarm to %u uV: %d\n", target_uv, ret);
		}
	}

	mutex_unlock(&sprd_cpufreq_lock);
	return 0;
}

static unsigned int sprd_cpufreq_get(unsigned int cpu)
{
	if (!clk_mcu)
		return 0;
	return (unsigned int)(clk_get_rate(clk_mcu) / 1000);
}

static int sprd_cpufreq_init(struct cpufreq_policy *policy)
{
	int ret;

	policy->clk = clk_mcu;
	policy->cur = sprd_cpufreq_get(policy->cpu);
	policy->freq_table = sprd_freq_table;
	policy->cpuinfo.transition_latency = 100000; /* 100 us */
	policy->shared_type = CPUFREQ_SHARED_TYPE_ANY;
	cpumask_setall(policy->cpus);

	ret = cpufreq_table_validate_and_show(policy, sprd_freq_table);
	if (ret) {
		pr_err("failed to validate frequency table: %d\n", ret);
		return ret;
	}

	pr_info("initialized on CPU%d at %u kHz (min %u, max %u)\n",
		policy->cpu, policy->cur, policy->cpuinfo.min_freq, policy->cpuinfo.max_freq);
	return 0;
}

static struct cpufreq_driver sprd_cpufreq_driver = {
	.name		= "sprd-cpufreq",
	.flags		= CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= sprd_cpufreq_target_index,
	.get		= sprd_cpufreq_get,
	.init		= sprd_cpufreq_init,
	.attr		= cpufreq_generic_attr,
};

static int __init sprd_cpufreq_module_init(void)
{
	int ret;

	clk_mcu = clk_get_sys(NULL, "clk_mcu");
	if (IS_ERR(clk_mcu)) {
		pr_err("failed to get clk_mcu: %ld\n", PTR_ERR(clk_mcu));
		return PTR_ERR(clk_mcu);
	}

	clk_mpll = clk_get_sys(NULL, "clk_mpll");
	if (IS_ERR(clk_mpll)) {
		pr_err("failed to get clk_mpll: %ld\n", PTR_ERR(clk_mpll));
		return PTR_ERR(clk_mpll);
	}

	clk_tdpll = clk_get_sys(NULL, "clk_tdpll");
	if (IS_ERR(clk_tdpll)) {
		pr_err("failed to get clk_tdpll: %ld\n", PTR_ERR(clk_tdpll));
		return PTR_ERR(clk_tdpll);
	}

	reg_vddarm = regulator_get(NULL, "vddarm");
	if (IS_ERR(reg_vddarm)) {
		pr_warn("failed to get vddarm regulator: %ld (proceeding without voltage scaling)\n",
			PTR_ERR(reg_vddarm));
		reg_vddarm = NULL;
	}

	ret = cpufreq_register_driver(&sprd_cpufreq_driver);
	if (ret) {
		pr_err("failed to register cpufreq driver: %d\n", ret);
		return ret;
	}

	pr_info("Spreadtrum SC7730SE cpufreq driver registered successfully\n");
	return 0;
}
late_initcall(sprd_cpufreq_module_init);

MODULE_DESCRIPTION("Spreadtrum SC7730SE CPUFreq Driver");
MODULE_AUTHOR("Pedro / Antigravity");
MODULE_LICENSE("GPL v2");
