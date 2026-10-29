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
 * Rewrite of the 3.10 Spreadtrum sc8830 platform code:
 *  - GPU_TOP power domain is driven through PMU_APB PD_GPU_TOP_CFG,
 *  - clk_gpu_axi (AON_APB GPU_EB) comes from DT,
 *  - the GPU core clock is the GPU_APB clk_gpu mux plus divider.  It lives
 *    inside the GPU power domain and loses its state on power-off, so it
 *    is reprogrammed on every power-up,
 *  - frequency scaling (vddcore is left alone): the Mali utilization
 *    callback picks a level and a work item switches the mux/divider while
 *    the GPU is held with mali_dev_pause(), as the 3.10 code did.
 *
 * Level rates are read from the source clocks, not from their names: on
 * this board clk_312m is CPLL/2 = 512 MHz.  Levels above 460.8 MHz are
 * opt-in through gpufreq_max_limit.
 */

#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/sizes.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/reboot.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>
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
#define   GPU_CLK_DIV_SHIFT		4

/* Used when no source clock rate is known: the fixed clock of v1..v18 */
#define GPU_SAFE_SEL			5	/* clk_384m */
#define GPU_SAFE_KHZ			384000
#define GPU_DEF_MAX_KHZ			460800	/* default ceiling */
#define GPU_ABS_MAX_KHZ			512000	/* never clock above this */

/* Governor.  Mali reports the load of each period as 0..256. */
#define GPU_LOAD_SCALE			256
#define GPU_LOAD_BUSY			230	/* ~90%: jump to the ceiling */
#define GPU_UP_PCT			70	/* up: keep <= 70% busy */
#define GPU_DOWN_PCT			60	/* down: only if <= 60% busy */

enum {
	GPU_SRC_460M8,
	GPU_SRC_384M,
	GPU_SRC_312M,
	GPU_SRC_256M,
	GPU_SRC_153M6,
	GPU_NR_SRCS
};

struct gpu_src {
	const char *name;	/* clock-names entry */
	u32 sel;		/* clk_gpu mux value */
	struct clk *clk;
	unsigned long rate;
};

/* clk_gpu mux inputs; 1 (clk_208m) and 3 (clk_300m, MPLL/CPU) are unused */
static struct gpu_src gpu_srcs[GPU_NR_SRCS] = {
	[GPU_SRC_460M8] = { .name = "clk_460m8", .sel = 6 },	/* WPLL/2 */
	[GPU_SRC_384M] = { .name = "clk_384m", .sel = 5 },	/* TDPLL/2 */
	[GPU_SRC_312M] = { .name = "clk_312m", .sel = 4 },	/* CPLL/2 */
	[GPU_SRC_256M] = { .name = "clk_256m", .sel = 2 },	/* TDPLL/3 */
	[GPU_SRC_153M6] = { .name = "clk_153m6", .sel = 0 },	/* TDPLL/5 */
};

/* Candidate levels; the first one wins when two give the same rate. */
static const struct {
	u8 src;
	u8 div;
} gpu_cands[] = {
	{ GPU_SRC_312M, 1 },	/* 512 MHz, opt-in */
	{ GPU_SRC_460M8, 1 },	/* 460.8 MHz */
	{ GPU_SRC_384M, 1 },	/* 384 MHz */
	{ GPU_SRC_256M, 1 },	/* 256 MHz */
	{ GPU_SRC_384M, 2 },	/* 192 MHz */
	{ GPU_SRC_153M6, 1 },	/* 153.6 MHz */
	{ GPU_SRC_256M, 2 },	/* 128 MHz */
};

#define GPU_NR_CANDS		ARRAY_SIZE(gpu_cands)

struct gpu_level {
	u32 khz;
	u32 ctrl;		/* GPU_APB_CLK_CTRL sel/div bits */
	u64 time_ns;		/* time spent powered at this level */
	u32 entries;
};

/* Sorted fastest first: a lower index is a higher clock. */
static struct gpu_level gpu_levels[GPU_NR_CANDS];
static int gpu_nr_levels;
static int gpu_def_ceil;

int gpufreq_min_limit = -1;
int gpufreq_max_limit = -1;
static char gpufreq_table_buf[GPU_NR_CANDS * 12 + 1];
char *gpufreq_table = gpufreq_table_buf;

static void __iomem *pmu_base;
static void __iomem *gpu_apb_base;
static struct clk *gpu_axi_clk;	/* AON_APB GPU_EB; its prepare also clears FORCE_SHUTDOWN */
static unsigned long gpu_srcs_on;	/* sources enabled while powered */

/* Serializes power changes, level switches and the statistics. */
static DEFINE_MUTEX(gpu_power_lock);
/* Guards gpu_powered/gpu_cur/gpu_next, also used by the governor. */
static DEFINE_SPINLOCK(gpu_dfs_lock);
static bool gpu_powered;
static int gpu_cur;		/* level index the GPU runs at */
static int gpu_next;		/* level index the governor wants */

static bool gpu_domain_ok = true;

static struct workqueue_struct *gpu_dfs_wq;
static void sprd_gpu_dfs_work(struct work_struct *work);
static DECLARE_WORK(gpu_dfs_work, sprd_gpu_dfs_work);

/* Statistics, under gpu_power_lock (hint counters: governor only). */
static ktime_t gpu_stamp;
static u32 gpu_power_ups;
static u32 gpu_switches;
static u64 gpu_switch_ns;
static u64 gpu_switch_ns_max;
static u32 gpu_boost_hints;
static u32 gpu_low_hints;
static int gpu_seen_min = -1;
static int gpu_seen_max = -1;

/* Fastest level not above @khz, or the slowest level. */
static int sprd_gpu_level_le(u32 khz)
{
	int i;

	for (i = 0; i < gpu_nr_levels - 1; i++)
		if (gpu_levels[i].khz <= khz)
			break;
	return i;
}

/* Slowest level not below @khz, or the fastest level. */
static int sprd_gpu_level_ge(u32 khz)
{
	int i;

	for (i = gpu_nr_levels - 1; i > 0; i--)
		if (gpu_levels[i].khz >= khz)
			break;
	return i;
}

/*
 * Turn gpufreq_max_limit/gpufreq_min_limit (kHz, <= 0 means unset) into
 * the allowed level range [*fast, *slow].  The max limit wins a conflict.
 */
static void sprd_gpu_limits(int *fast, int *slow)
{
	int max = READ_ONCE(gpufreq_max_limit);
	int min = READ_ONCE(gpufreq_min_limit);
	int f, s;

	f = max > 0 ? sprd_gpu_level_le(max) : gpu_def_ceil;
	s = min > 0 ? sprd_gpu_level_ge(min) : gpu_nr_levels - 1;
	if (s < f)
		s = f;

	if (max != gpu_seen_max || min != gpu_seen_min) {
		gpu_seen_max = max;
		gpu_seen_min = min;
		pr_info("mali: gpufreq limits min %d max %d: %u..%u kHz\n",
			min, max, gpu_levels[s].khz, gpu_levels[f].khz);
	}
	*fast = f;
	*slow = s;
}

/*
 * Pick a level for the load (0..256) seen at level @cur during the last
 * period: the slowest level that keeps the GPU at most GPU_UP_PCT busy
 * when going faster, at most GPU_DOWN_PCT busy when going slower.
 */
static int sprd_gpu_pick(int cur, unsigned int load, int fast, int slow)
{
	u32 busy_khz;
	int next;

	if (load >= GPU_LOAD_BUSY)
		return fast;

	busy_khz = gpu_levels[cur].khz * load / GPU_LOAD_SCALE;
	next = sprd_gpu_level_ge(busy_khz * 100 / GPU_UP_PCT);
	if (next >= cur)
		next = max(sprd_gpu_level_ge(busy_khz * 100 / GPU_DOWN_PCT),
			   cur);
	return clamp(next, fast, slow);
}

/* Charge the time since the last stamp to the current level. */
static void sprd_gpu_account(ktime_t now)
{
	gpu_levels[gpu_cur].time_ns += ktime_to_ns(ktime_sub(now, gpu_stamp));
	gpu_stamp = now;
}

static void sprd_gpu_set_ctrl(u32 ctrl)
{
	u32 val = readl(gpu_apb_base + GPU_APB_CLK_CTRL);

	val &= ~(GPU_CLK_SEL_MASK | GPU_CLK_DIV_MASK);
	writel(val | ctrl, gpu_apb_base + GPU_APB_CLK_CTRL);
	usleep_range(100, 150);
}

static void sprd_gpu_wait_for_ready(void)
{
	int timeout = 2000;

	while (readl(pmu_base + PMU_PWR_STATUS0_DBG) & PD_GPU_TOP_STATE_MASK) {
		if (--timeout == 0) {
			pr_err("mali: GPU power domain not ready, PWR_STATUS0_DBG=%08x\n",
			       readl(pmu_base + PMU_PWR_STATUS0_DBG));
			gpu_domain_ok = false;
			return;
		}
		usleep_range(50, 100);
	}
	gpu_domain_ok = true;
}

/* Hold every mux input's PLL while the GPU is up, as the 3.10 code did. */
static void sprd_gpu_srcs_enable(void)
{
	int i;

	for (i = 0; i < GPU_NR_SRCS; i++) {
		if (!gpu_srcs[i].clk || clk_prepare_enable(gpu_srcs[i].clk))
			continue;
		__set_bit(i, &gpu_srcs_on);
	}
}

static void sprd_gpu_srcs_disable(void)
{
	int i;

	for (i = 0; i < GPU_NR_SRCS; i++)
		if (__test_and_clear_bit(i, &gpu_srcs_on))
			clk_disable_unprepare(gpu_srcs[i].clk);
}

static void sprd_gpu_power_on(void)
{
	unsigned long flags;
	int fast, slow;
	u32 val;

	if (gpu_powered)
		return;

	sprd_gpu_srcs_enable();

	val = readl(pmu_base + PMU_PD_GPU_TOP_CFG);
	writel(val & ~PD_GPU_TOP_FORCE_SHUTDOWN, pmu_base + PMU_PD_GPU_TOP_CFG);
	usleep_range(100, 150);

	clk_prepare_enable(gpu_axi_clk);
	sprd_gpu_wait_for_ready();

	/* Every power-up starts at the ceiling, the governor scales down. */
	sprd_gpu_limits(&fast, &slow);
	sprd_gpu_set_ctrl(gpu_levels[fast].ctrl);

	spin_lock_irqsave(&gpu_dfs_lock, flags);
	gpu_powered = true;
	gpu_cur = fast;
	gpu_next = fast;
	spin_unlock_irqrestore(&gpu_dfs_lock, flags);

	gpu_stamp = ktime_get();
	gpu_levels[fast].entries++;
	gpu_power_ups++;
	gpu_cur_freq = gpu_levels[fast].khz;
}

static void sprd_gpu_power_off(void)
{
	unsigned long flags;
	u32 val;

	if (!gpu_powered)
		return;

	clk_disable_unprepare(gpu_axi_clk);

	val = readl(pmu_base + PMU_PD_GPU_TOP_CFG);
	writel(val | PD_GPU_TOP_FORCE_SHUTDOWN, pmu_base + PMU_PD_GPU_TOP_CFG);

	sprd_gpu_srcs_disable();

	spin_lock_irqsave(&gpu_dfs_lock, flags);
	gpu_powered = false;
	spin_unlock_irqrestore(&gpu_dfs_lock, flags);

	sprd_gpu_account(ktime_get());
	gpu_cur_freq = 0;
}

/* power_mode: 0 = on, 1 = runtime suspend, 2 = system suspend */
void mali_platform_power_mode_change(int power_mode)
{
	mutex_lock(&gpu_power_lock);
	if (power_mode == 0)
		sprd_gpu_power_on();
	else
		sprd_gpu_power_off();
	mutex_unlock(&gpu_power_lock);
}

static void sprd_gpu_dfs_work(struct work_struct *work)
{
	unsigned long flags;
	ktime_t start;
	bool todo;
	int next;
	u64 ns;

	spin_lock_irqsave(&gpu_dfs_lock, flags);
	todo = gpu_powered && gpu_next != gpu_cur;
	spin_unlock_irqrestore(&gpu_dfs_lock, flags);
	if (!todo)
		return;

	/* Wait for the running jobs and keep new ones off the GPU. */
	start = ktime_get();
	mali_dev_pause();

	mutex_lock(&gpu_power_lock);
	spin_lock_irqsave(&gpu_dfs_lock, flags);
	next = gpu_next;
	spin_unlock_irqrestore(&gpu_dfs_lock, flags);

	if (gpu_powered && next != gpu_cur) {
		sprd_gpu_set_ctrl(gpu_levels[next].ctrl);
		sprd_gpu_account(ktime_get());

		spin_lock_irqsave(&gpu_dfs_lock, flags);
		gpu_cur = next;
		spin_unlock_irqrestore(&gpu_dfs_lock, flags);

		gpu_levels[next].entries++;
		gpu_switches++;
		gpu_cur_freq = gpu_levels[next].khz;
		ns = ktime_to_ns(ktime_sub(ktime_get(), start));
		gpu_switch_ns += ns;
		if (ns > gpu_switch_ns_max)
			gpu_switch_ns_max = ns;
	}
	mutex_unlock(&gpu_power_lock);

	mali_dev_resume();
}

/* Called from the Mali utilization timer (softirq) every period. */
void mali_platform_utilization(struct mali_gpu_utilization_data *data)
{
	unsigned int load = data->utilization_gpu;
	int hint = READ_ONCE(gpu_level);
	unsigned long flags;
	bool kick = false;
	int fast, slow;

	if (!gpu_dfs_wq)
		return;

	/*
	 * Session hints (MALI_IOC_SET_GPU_LEVEL) are consumed per period
	 * like in the 3.10 code: 9 and 10 boost to the ceiling, the "pin
	 * low" ones (5, 7) are only counted.
	 */
	if (hint > 1) {
		WRITE_ONCE(gpu_level, 1);
		if (hint >= 9) {
			gpu_boost_hints++;
			load = GPU_LOAD_SCALE;
		} else {
			gpu_low_hints++;
		}
	}

	/* An idle period: the GPU powers down next, nothing to scale. */
	if (!load)
		return;

	sprd_gpu_limits(&fast, &slow);

	spin_lock_irqsave(&gpu_dfs_lock, flags);
	if (gpu_powered) {
		gpu_next = sprd_gpu_pick(gpu_cur, load, fast, slow);
		kick = gpu_next != gpu_cur;
	}
	spin_unlock_irqrestore(&gpu_dfs_lock, flags);

	if (kick)
		queue_work(gpu_dfs_wq, &gpu_dfs_work);
}

/* Statistics text; gpu_power_lock must be held. */
static int sprd_gpu_stats_fmt(char *buf, size_t size)
{
	u64 avg_ns = 0;
	int i, len = 0;

	if (gpu_powered)
		sprd_gpu_account(ktime_get());

	for (i = 0; i < gpu_nr_levels; i++)
		len += scnprintf(buf + len, size - len,
				 "%6u kHz: %llu ms, %u\n", gpu_levels[i].khz,
				 div_u64(gpu_levels[i].time_ns, NSEC_PER_MSEC),
				 gpu_levels[i].entries);

	if (gpu_switches)
		avg_ns = div_u64(gpu_switch_ns, gpu_switches);
	len += scnprintf(buf + len, size - len,
			 "power-ups %u, switches %u (avg %llu us, max %llu us)\n",
			 gpu_power_ups, gpu_switches,
			 div_u64(avg_ns, NSEC_PER_USEC),
			 div_u64(gpu_switch_ns_max, NSEC_PER_USEC));
	len += scnprintf(buf + len, size - len, "hints: boost %u, low %u\n",
			 gpu_boost_hints, gpu_low_hints);
	return len;
}

static int sprd_gpu_stats_get(char *buf, const struct kernel_param *kp)
{
	int len;

	mutex_lock(&gpu_power_lock);
	len = sprd_gpu_stats_fmt(buf, PAGE_SIZE);
	mutex_unlock(&gpu_power_lock);
	return len;
}

static int sprd_gpu_stats_set(const char *val, const struct kernel_param *kp)
{
	return -EPERM;
}

static const struct kernel_param_ops sprd_gpu_stats_ops = {
	.set = sprd_gpu_stats_set,
	.get = sprd_gpu_stats_get,
};
module_param_cb(gpufreq_stats, &sprd_gpu_stats_ops, NULL, 0444);
MODULE_PARM_DESC(gpufreq_stats, "GPU DVFS statistics");

/* Leave the statistics in the kernel log (and last_kmsg) on reboot. */
static int sprd_gpu_reboot_notify(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	static char buf[512];
	char *p = buf, *line;

	if (!mutex_trylock(&gpu_power_lock))
		return NOTIFY_DONE;
	sprd_gpu_stats_fmt(buf, sizeof(buf));
	mutex_unlock(&gpu_power_lock);

	while ((line = strsep(&p, "\n")) && *line)
		pr_info("mali: gpufreq %s\n", line);
	return NOTIFY_DONE;
}

static struct notifier_block sprd_gpu_reboot_nb = {
	.notifier_call = sprd_gpu_reboot_notify,
};

static bool gpu_nb_registered;

static void sprd_gpu_get_srcs(struct device *dev, struct device_node *np)
{
	struct gpu_src *s;

	for (s = gpu_srcs; s < gpu_srcs + GPU_NR_SRCS; s++) {
		s->clk = of_clk_get_by_name(np, s->name);
		if (IS_ERR(s->clk)) {
			dev_warn(dev, "no %s: %ld\n", s->name, PTR_ERR(s->clk));
			s->clk = NULL;
			continue;
		}
		s->rate = clk_get_rate(s->clk);
	}
}

static void sprd_gpu_put_srcs(void)
{
	struct gpu_src *s;

	for (s = gpu_srcs; s < gpu_srcs + GPU_NR_SRCS; s++) {
		if (s->clk)
			clk_put(s->clk);
		s->clk = NULL;
	}
}

/* Build gpu_levels[] (fastest first, no duplicates) from gpu_cands[]. */
static void sprd_gpu_build_levels(void)
{
	unsigned int i;
	int j, n = 0, len = 0;

	for (i = 0; i < GPU_NR_CANDS; i++) {
		const struct gpu_src *s = &gpu_srcs[gpu_cands[i].src];
		u32 div = gpu_cands[i].div;
		u32 khz = s->rate / 1000 / div;

		if (!s->clk || !khz || khz > GPU_ABS_MAX_KHZ)
			continue;

		j = 0;
		while (j < n && gpu_levels[j].khz > khz)
			j++;
		if (j < n && gpu_levels[j].khz == khz)
			continue;

		memmove(&gpu_levels[j + 1], &gpu_levels[j],
			(n - j) * sizeof(*gpu_levels));
		gpu_levels[j].khz = khz;
		gpu_levels[j].ctrl = s->sel | ((div - 1) << GPU_CLK_DIV_SHIFT);
		n++;
	}

	if (!n) {
		gpu_levels[0].khz = GPU_SAFE_KHZ;
		gpu_levels[0].ctrl = GPU_SAFE_SEL;
		n = 1;
	}
	gpu_nr_levels = n;
	gpu_def_ceil = sprd_gpu_level_le(GPU_DEF_MAX_KHZ);

	/* Same layout as the 3.10 gpufreq_table: "index  kHz" lines. */
	for (j = 0; j < n; j++)
		len += scnprintf(gpufreq_table_buf + len,
				 sizeof(gpufreq_table_buf) - len, "%2d  %6u\n",
				 j, gpu_levels[j].khz);
}

static void sprd_gpu_print_levels(struct device *dev)
{
	char buf[128];
	int i, len = 0;

	for (i = 0; i < GPU_NR_SRCS; i++)
		len += scnprintf(buf + len, sizeof(buf) - len, " %s=%lu",
				 gpu_srcs[i].name, gpu_srcs[i].rate / 1000);
	dev_info(dev, "clk_gpu sources (kHz):%s\n", buf);

	len = 0;
	for (i = 0; i < gpu_nr_levels; i++)
		len += scnprintf(buf + len, sizeof(buf) - len, " %u%s",
				 gpu_levels[i].khz,
				 i < gpu_def_ceil ? "*" : "");
	dev_info(dev, "levels (kHz, * = opt-in):%s; ceiling %u, DVFS %s\n",
		 buf, gpu_levels[gpu_def_ceil].khz, gpu_dfs_wq ? "on" : "off");
}

int mali_power_initialize(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	u32 val;

	if (!np)
		np = of_find_matching_node(NULL, gpu_ids);
	if (!np)
		return -ENODEV;

	pmu_base = ioremap(SPRD_PMU_APB_PHYS, SZ_4K);
	gpu_apb_base = ioremap(SPRD_GPU_APB_PHYS, SZ_4K);
	if (!pmu_base || !gpu_apb_base) {
		dev_err(dev, "cannot map PMU/GPU APB registers\n");
		goto err_unmap;
	}

	gpu_axi_clk = of_clk_get_by_name(np, "clk_gpu_axi");
	if (IS_ERR(gpu_axi_clk))
		gpu_axi_clk = of_clk_get(np, 0);
	if (IS_ERR(gpu_axi_clk)) {
		dev_err(dev, "cannot get clk_gpu_axi: %ld\n",
			PTR_ERR(gpu_axi_clk));
		gpu_axi_clk = NULL;
		goto err_unmap;
	}

	sprd_gpu_get_srcs(dev, np);
	sprd_gpu_build_levels();
	if (gpu_nr_levels > 1) {
		gpu_dfs_wq = alloc_ordered_workqueue("gpu_dfs",
						     WQ_FREEZABLE | WQ_HIGHPRI);
		if (!gpu_dfs_wq)
			dev_warn(dev, "no workqueue, GPU DVFS off\n");
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
		dev_err(dev, "GPU power domain did not come up, not probing\n");
		goto err_put;
	}
	dev_info(dev, "PD_GPU_TOP_CFG=%08x PWR_STATUS0_DBG=%08x GPU_CLK_CTRL=%08x\n",
		 readl(pmu_base + PMU_PD_GPU_TOP_CFG),
		 readl(pmu_base + PMU_PWR_STATUS0_DBG),
		 readl(gpu_apb_base + GPU_APB_CLK_CTRL));

	pm_runtime_set_autosuspend_delay(dev, 50);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_enable(dev);

	if (!gpu_nb_registered)
		gpu_nb_registered =
			!register_reboot_notifier(&sprd_gpu_reboot_nb);
	sprd_gpu_print_levels(dev);
	return 0;

err_put:
	if (gpu_dfs_wq)
		destroy_workqueue(gpu_dfs_wq);
	gpu_dfs_wq = NULL;
	sprd_gpu_put_srcs();
	clk_put(gpu_axi_clk);
	gpu_axi_clk = NULL;
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
