/*
 * Spreadtrum scx35 (SC8830 / SC7730SE) timer and system counter driver
 *
 * The SoC has no ARM architected timer, so timekeeping relies on the
 * always-on general purpose timers in the AON domain plus the free running
 * system counter.
 *
 * Register layout and clock gating recovered from the Spreadtrum 3.10
 * vendor kernel (arch/arm/mach-sc/timer_sc8830.c).
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) "sprd-timer: " fmt

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched_clock.h>

/* general purpose timer; each instance is 0x20 bytes apart */
#define TIMER_LOAD		0x00
#define TIMER_VALUE		0x04
#define TIMER_CTL		0x08
#define TIMER_INT		0x0c
#define TIMER_CNT_RD		0x10

#define TIMER_CTL_PERIOD_MODE	BIT(6)
#define TIMER_CTL_ENABLE	BIT(7)
#define TIMER_CTL_NEW		BIT(8)

#define TIMER_INT_EN		BIT(0)
#define TIMER_INT_RAW_STS	BIT(1)
#define TIMER_INT_MASK_STS	BIT(2)
#define TIMER_INT_CLR		BIT(3)
#define TIMER_INT_BUSY		BIT(4)

/* free running system counter */
#define SYSCNT_COUNT		0x04
#define SYSCNT_CTL		0x08
#define SYSCNT_SHADOW_CNT	0x0c

/* AON_APB_APB_EB0: clock gates for the timers, see __regs_aon_apb.h */
#define AON_APB_EB0		0x00
#define AON_APB_EB0_AP_SYST	BIT(10)
#define AON_APB_EB0_AON_TMR	BIT(11)
#define AON_APB_EB0_AP_TMR0	BIT(12)

#define SPRD_TIMER_DEFAULT_RATE	32768
#define SPRD_TIMER_BUSY_RETRY	1000

static void __iomem *sprd_event_base;
static void __iomem *sprd_syscnt_base;
static u32 sprd_timer_rate = SPRD_TIMER_DEFAULT_RATE;

/*
 * clocksource_of_init() runs long before the syscon/regmap infrastructure is
 * usable, so poke the AON_APB clock gates through a temporary mapping.
 */
static void __init sprd_timer_enable_gates(u32 mask)
{
	struct device_node *np;
	void __iomem *aon_apb;

	np = of_find_compatible_node(NULL, NULL, "sprd,scx35-aon-apb");
	if (!np) {
		pr_warn("no aon-apb node, relying on the bootloader\n");
		return;
	}

	aon_apb = of_iomap(np, 0);
	of_node_put(np);
	if (!aon_apb) {
		pr_warn("cannot map aon-apb, relying on the bootloader\n");
		return;
	}

	writel_relaxed(readl_relaxed(aon_apb + AON_APB_EB0) | mask,
		       aon_apb + AON_APB_EB0);
	iounmap(aon_apb);
}

/*
 * Only the mode and enable bits belong to us: the bootloader may have set
 * implementation specific bits (such as TIMER_CTL_NEW) in this register, and
 * clobbering them stops the timer from latching newly written values.
 */
static inline void sprd_timer_update_ctl(u32 clear, u32 set)
{
	u32 val = readl_relaxed(sprd_event_base + TIMER_CTL);

	val &= ~clear;
	val |= set;
	writel_relaxed(val, sprd_event_base + TIMER_CTL);
}

static inline void sprd_timer_disable(void)
{
	sprd_timer_update_ctl(TIMER_CTL_ENABLE, 0);
}

/* the timer latches a new value asynchronously, wait for it to settle */
static inline void sprd_timer_wait_idle(void)
{
	int retry = SPRD_TIMER_BUSY_RETRY;

	while ((readl_relaxed(sprd_event_base + TIMER_INT) & TIMER_INT_BUSY) &&
	       retry--)
		cpu_relax();
}

static int sprd_timer_set_next_event(unsigned long cycles,
				     struct clock_event_device *ce)
{
	sprd_timer_wait_idle();
	/* one shot: drop the periodic bit while the counter is reloaded */
	sprd_timer_update_ctl(TIMER_CTL_ENABLE | TIMER_CTL_PERIOD_MODE, 0);
	writel_relaxed(cycles, sprd_event_base + TIMER_LOAD);
	sprd_timer_update_ctl(0, TIMER_CTL_ENABLE);

	return 0;
}

static int sprd_timer_set_periodic(struct clock_event_device *ce)
{
	sprd_timer_wait_idle();
	sprd_timer_disable();
	writel_relaxed(sprd_timer_rate / HZ, sprd_event_base + TIMER_LOAD);
	writel_relaxed(TIMER_INT_EN, sprd_event_base + TIMER_INT);
	sprd_timer_update_ctl(0, TIMER_CTL_ENABLE | TIMER_CTL_PERIOD_MODE);

	return 0;
}

static int sprd_timer_set_oneshot(struct clock_event_device *ce)
{
	sprd_timer_wait_idle();
	sprd_timer_update_ctl(TIMER_CTL_ENABLE | TIMER_CTL_PERIOD_MODE, 0);
	writel_relaxed(TIMER_INT_EN, sprd_event_base + TIMER_INT);

	return 0;
}

static int sprd_timer_shutdown(struct clock_event_device *ce)
{
	sprd_timer_disable();
	writel_relaxed(TIMER_INT_CLR, sprd_event_base + TIMER_INT);

	return 0;
}

static irqreturn_t sprd_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *ce = dev_id;
	u32 val;

	val = readl_relaxed(sprd_event_base + TIMER_INT);
	writel_relaxed(val | TIMER_INT_CLR, sprd_event_base + TIMER_INT);

	if (clockevent_state_oneshot(ce))
		sprd_timer_disable();

	ce->event_handler(ce);

	return IRQ_HANDLED;
}

static struct clock_event_device sprd_clockevent = {
	.name			= "sprd_timer",
	.rating			= 300,
	.features		= CLOCK_EVT_FEAT_PERIODIC | CLOCK_EVT_FEAT_ONESHOT,
	.set_state_shutdown	= sprd_timer_shutdown,
	.set_state_periodic	= sprd_timer_set_periodic,
	.set_state_oneshot	= sprd_timer_set_oneshot,
	.tick_resume		= sprd_timer_shutdown,
	.set_next_event		= sprd_timer_set_next_event,
};

static int __init sprd_timer_init(struct device_node *np)
{
	struct clk *clk;
	int irq, ret;

	sprd_timer_enable_gates(AON_APB_EB0_AON_TMR | AON_APB_EB0_AP_TMR0);

	sprd_event_base = of_iomap(np, 0);
	if (!sprd_event_base) {
		pr_err("cannot map the timer registers\n");
		return -ENXIO;
	}

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0) {
		pr_err("cannot parse the timer interrupt\n");
		return -EINVAL;
	}

	clk = of_clk_get(np, 0);
	if (!IS_ERR(clk)) {
		clk_prepare_enable(clk);
		if (clk_get_rate(clk))
			sprd_timer_rate = clk_get_rate(clk);
	}

	/* leave the hardware in a known state */
	sprd_timer_update_ctl(TIMER_CTL_ENABLE | TIMER_CTL_PERIOD_MODE, 0);
	writel_relaxed(TIMER_INT_CLR, sprd_event_base + TIMER_INT);

	ret = request_irq(irq, sprd_timer_interrupt,
			  IRQF_TIMER | IRQF_IRQPOLL, "sprd_timer",
			  &sprd_clockevent);
	if (ret) {
		pr_err("cannot request irq %d (%d)\n", irq, ret);
		return ret;
	}

	sprd_clockevent.irq = irq;
	/*
	 * This is a single global timer, not a per CPU one; advertise it for
	 * every CPU so that the tick layer can use it as broadcast device.
	 */
	sprd_clockevent.cpumask = cpu_possible_mask;

	clockevents_config_and_register(&sprd_clockevent, sprd_timer_rate,
					2, 0xfffffffe);

	pr_info("clockevent running at %u Hz (irq %d)\n",
		sprd_timer_rate, irq);

	return 0;
}

static u64 notrace sprd_syscnt_sched_read(void)
{
	return (u64)readl_relaxed(sprd_syscnt_base + SYSCNT_SHADOW_CNT);
}

static int __init sprd_syscnt_init(struct device_node *np)
{
	unsigned long rate = SPRD_TIMER_DEFAULT_RATE;
	struct clk *clk;
	int ret;

	sprd_timer_enable_gates(AON_APB_EB0_AP_SYST);

	sprd_syscnt_base = of_iomap(np, 0);
	if (!sprd_syscnt_base) {
		pr_err("cannot map the system counter\n");
		return -ENXIO;
	}

	clk = of_clk_get(np, 0);
	if (!IS_ERR(clk)) {
		clk_prepare_enable(clk);
		if (clk_get_rate(clk))
			rate = clk_get_rate(clk);
	}

	/* the counter is read only for us, keep its interrupt masked */
	writel_relaxed(0, sprd_syscnt_base + SYSCNT_CTL);

	ret = clocksource_mmio_init(sprd_syscnt_base + SYSCNT_SHADOW_CNT,
				    "sprd_syscnt", rate, 200, 32,
				    clocksource_mmio_readl_up);
	if (ret) {
		pr_err("cannot register the clocksource (%d)\n", ret);
		return ret;
	}

	sched_clock_register(sprd_syscnt_sched_read, 32, rate);

	pr_info("clocksource running at %lu Hz\n", rate);

	return 0;
}

CLOCKSOURCE_OF_DECLARE(sprd_timer, "sprd,scx35-timer", sprd_timer_init);
CLOCKSOURCE_OF_DECLARE(sprd_syscnt, "sprd,scx35-syscnt", sprd_syscnt_init);
