/*
 * Copyright (C) 2010-2012 ARM Limited. All rights reserved.
 * Copyright (C) 2013 Spreadtrum Communications Inc.
 *
 * This program is free software and is provided to you under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation, and any use by you of this program is subject to the terms of such GNU licence.
 *
 * A copy of the licence is included with the program, and can also be obtained from Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * SC7730SE (SCX30G, "tshark") Mali-400 MP2 platform glue for Linux 4.9.
 *
 * Minimal rewrite of the 3.10 Spreadtrum sc8830 platform code:
 *  - GPU_TOP power domain is driven through PMU_APB PD_GPU_TOP_CFG,
 *  - clk_gpu_axi (AON_APB GPU_EB) comes from DT,
 *  - the GPU core clock runs at a fixed 384 MHz (TDPLL/2, div 1).  The
 *    GPU_APB clock mux lives inside the GPU power domain and loses its
 *    state on power-off, so it is reprogrammed on every power-up.
 * No DVFS (yet).
 */

#include <linux/platform_device.h>
#include <linux/version.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/mali/mali_utgard.h>

#include "mali_kernel_common.h"
#include "mali_kernel_linux.h"
#include "base.h"

#define SPRD_PMU_APB_PHYS		0x402b0000
#define  PMU_PD_GPU_TOP_CFG		0x0020
#define   PD_GPU_TOP_FORCE_SHUTDOWN	BIT(25)
#define   PD_GPU_TOP_DLY_MASK		0x00ffffff
#define   PD_GPU_TOP_DLY_VAL		0x00010101	/* PWR_ON_DLY=1, PWR_ON_SEQ_DLY=1, ISO_ON_DLY=1 */
#define  PMU_PWR_STATUS0_DBG		0x00bc
#define   PD_GPU_TOP_STATE_MASK		(0xfU << 24)	/* 0 == powered on */

#define SPRD_GPU_APB_PHYS		0x60100000
#define  GPU_APB_CLK_CTRL		0x0004
#define   GPU_CLK_SEL_MASK		0x7
#define   GPU_CLK_DIV_MASK		(0x3 << 4)	/* divider - 1 */

/* clk_gpu mux: 0=153.6M 1=208M 2=256M 3=300M 4=312M 5=384M 6=460.8M */
#define GPU_CLK_SEL_384M		5
#define GPU_FREQ_KHZ			384000

extern int gpu_cur_freq;

int gpufreq_min_limit = -1;
int gpufreq_max_limit = -1;
static char gpufreq_table_buf[16] = "384000";
char *gpufreq_table = gpufreq_table_buf;

static void __iomem *pmu_base;
static void __iomem *gpu_apb_base;
static struct clk *gpu_axi_clk;	/* AON_APB GPU_EB; its prepare also clears FORCE_SHUTDOWN */
static struct clk *gpu_src_clk;	/* clk_384m: keeps TDPLL referenced while the GPU runs */
static DEFINE_MUTEX(gpu_power_lock);
static bool gpu_powered;

static bool gpu_domain_ok = true;

static void sprd_gpu_wait_for_ready(void)
{
	int timeout = 2000;

	while (readl(pmu_base + PMU_PWR_STATUS0_DBG) & PD_GPU_TOP_STATE_MASK) {
		if (0 == --timeout) {
			pr_err("mali: GPU power domain not ready, PWR_STATUS0_DBG=%08x\n",
			       readl(pmu_base + PMU_PWR_STATUS0_DBG));
			gpu_domain_ok = false;
			return;
		}
		udelay(50);
	}
	gpu_domain_ok = true;
}

static void sprd_gpu_power_on(void)
{
	u32 val;

	if (gpu_powered)
		return;

	if (gpu_src_clk)
		clk_prepare_enable(gpu_src_clk);

	val = readl(pmu_base + PMU_PD_GPU_TOP_CFG);
	writel(val & ~PD_GPU_TOP_FORCE_SHUTDOWN, pmu_base + PMU_PD_GPU_TOP_CFG);
	udelay(100);

	clk_prepare_enable(gpu_axi_clk);
	sprd_gpu_wait_for_ready();

	val = readl(gpu_apb_base + GPU_APB_CLK_CTRL);
	val &= ~(GPU_CLK_SEL_MASK | GPU_CLK_DIV_MASK);
	val |= GPU_CLK_SEL_384M;	/* div field 0: /1 */
	writel(val, gpu_apb_base + GPU_APB_CLK_CTRL);
	udelay(100);

	gpu_powered = true;
	gpu_cur_freq = GPU_FREQ_KHZ;
}

static void sprd_gpu_power_off(void)
{
	u32 val;

	if (!gpu_powered)
		return;

	clk_disable_unprepare(gpu_axi_clk);

	val = readl(pmu_base + PMU_PD_GPU_TOP_CFG);
	writel(val | PD_GPU_TOP_FORCE_SHUTDOWN, pmu_base + PMU_PD_GPU_TOP_CFG);

	if (gpu_src_clk)
		clk_disable_unprepare(gpu_src_clk);

	gpu_powered = false;
	gpu_cur_freq = 0;
}

/* power_mode: 0 = on, 1 = light sleep (runtime suspend), 2 = deep sleep (system suspend) */
void mali_platform_power_mode_change(int power_mode)
{
	mutex_lock(&gpu_power_lock);
	if (0 == power_mode)
		sprd_gpu_power_on();
	else
		sprd_gpu_power_off();
	mutex_unlock(&gpu_power_lock);
}

int mali_power_initialize(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	u32 val;

	if (!np)
		np = of_find_matching_node(NULL, gpu_ids);
	if (!np)
		return -ENODEV;

	pmu_base = ioremap(SPRD_PMU_APB_PHYS, SZ_4K);
	gpu_apb_base = ioremap(SPRD_GPU_APB_PHYS, SZ_4K);
	if (!pmu_base || !gpu_apb_base) {
		dev_err(&pdev->dev, "cannot map PMU/GPU APB registers\n");
		goto err_unmap;
	}

	gpu_axi_clk = of_clk_get_by_name(np, "clk_gpu_axi");
	if (IS_ERR(gpu_axi_clk))
		gpu_axi_clk = of_clk_get(np, 0);
	if (IS_ERR(gpu_axi_clk)) {
		dev_err(&pdev->dev, "cannot get clk_gpu_axi: %ld\n", PTR_ERR(gpu_axi_clk));
		gpu_axi_clk = NULL;
		goto err_unmap;
	}

	gpu_src_clk = of_clk_get_by_name(np, "clk_384m");
	if (IS_ERR(gpu_src_clk)) {
		dev_warn(&pdev->dev, "no clk_384m (%ld), not referencing TDPLL\n", PTR_ERR(gpu_src_clk));
		gpu_src_clk = NULL;
	}

	/* Power-up sequencing delays, as programmed by the 3.10 driver. */
	val = readl(pmu_base + PMU_PD_GPU_TOP_CFG);
	val = (val & ~PD_GPU_TOP_DLY_MASK) | PD_GPU_TOP_DLY_VAL;
	writel(val, pmu_base + PMU_PD_GPU_TOP_CFG);

	/* The core probe below touches GPU registers, so power up now. */
	mali_platform_power_mode_change(0);
	if (!gpu_domain_ok) {
		/* Touching GPU registers now could hang the bus: bail out. */
		mali_platform_power_mode_change(2);
		dev_err(&pdev->dev, "GPU power domain did not come up, not probing\n");
		goto err_unmap;
	}
	dev_info(&pdev->dev, "PD_GPU_TOP_CFG=%08x PWR_STATUS0_DBG=%08x GPU_CLK_CTRL=%08x\n",
		 readl(pmu_base + PMU_PD_GPU_TOP_CFG), readl(pmu_base + PMU_PWR_STATUS0_DBG),
		 readl(gpu_apb_base + GPU_APB_CLK_CTRL));

	pm_runtime_set_autosuspend_delay(&pdev->dev, 50);
	pm_runtime_use_autosuspend(&pdev->dev);
	pm_runtime_enable(&pdev->dev);

	dev_info(&pdev->dev, "Mali-400 power domain up, core clock %d kHz\n", GPU_FREQ_KHZ);
	return 0;

err_unmap:
	if (gpu_apb_base)
		iounmap(gpu_apb_base);
	if (pmu_base)
		iounmap(pmu_base);
	gpu_apb_base = NULL;
	pmu_base = NULL;
	return -ENODEV;
}

void mali_platform_device_release(struct device *device)
{
	MALI_DEBUG_PRINT(4, ("mali_platform_device_release() called\n"));
}

void mali_platform_utilization(struct mali_gpu_utilization_data *data)
{
	/* Fixed frequency: nothing to scale. */
}
