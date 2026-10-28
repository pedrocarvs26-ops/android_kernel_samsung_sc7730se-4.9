/*
 * Spreadtrum SC7730SE / SC8830 Timer & Clocksource Driver
 *
 * Matches arch/arm/mach-sc/timer_sc8830.c from the working 3.10 kernel:
 * - Timer 0 at GPTIMER base + 0x00 (32768 Hz): Clock Event (oneshot/periodic)
 *   Requires TIMER_CTL_NEW (BIT(8)) for immediate counter reload.
 * - Timer 1 at GPTIMER base + 0x20 (32768 Hz): 32-bit Clocksource & sched_clock
 *   Free-running 32-bit countdown timer inverted (~readl) to count up at 32768 Hz.
 * - SYSCNT at 0x40230000 (1000 Hz): Secondary 1 kHz millisecond counter.
 *
 * Licensed under the GPL-2.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/sched_clock.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

/* Timer n register offsets (stride 0x20 per timer channel) */
#define TIMER_CH_OFFSET(id)	((id) * 0x20)
#define TIMER_LOAD		0x00
#define TIMER_VALUE		0x04
#define TIMER_CTL		0x08
#define TIMER_INT		0x0C
#define TIMER_CNT_RD		0x10

/* TIMER_CTL bits */
#define TIMER_CTL_PERIOD_MODE	BIT(6)
#define TIMER_CTL_ENABLE	BIT(7)
#define TIMER_CTL_NEW		BIT(8)

/* TIMER_INT bits */
#define TIMER_INT_EN		BIT(0)
#define TIMER_INT_RAW_STS	BIT(1)
#define TIMER_INT_MSK_STS	BIT(2)
#define TIMER_INT_CLR		BIT(3)
#define TIMER_INT_BUSY		BIT(4)

#define EVENT_TIMER_ID		0	/* Channel 0: clock_event_device */
#define SOURCE_TIMER_ID		1	/* Channel 1: 32768 Hz clocksource & sched_clock */

/* SYSCNT register offsets (0x40230000, 1000 Hz) */
#define SYSCNT_SHADOW_CNT	0x000C
#define SYSCNT_CTL		0x0008

static void __iomem *gptimer_base;
static void __iomem *syscnt_base;
static u32 gptimer_freq = 32768;

static inline void gptimer_ctl_write(int id, u32 val)
{
	writel_relaxed(val | TIMER_CTL_NEW,
		       gptimer_base + TIMER_CH_OFFSET(id) + TIMER_CTL);
}

static int sprd_timer_shutdown(struct clock_event_device *evt)
{
	u32 saved = readl_relaxed(gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_CTL) &
		    TIMER_CTL_PERIOD_MODE;

	writel_relaxed(0, gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);
	gptimer_ctl_write(EVENT_TIMER_ID, saved);
	return 0;
}

static int sprd_timer_set_oneshot(struct clock_event_device *evt)
{
	gptimer_ctl_write(EVENT_TIMER_ID, 0);
	writel_relaxed(TIMER_INT_EN,
		       gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);
	return 0;
}

static int sprd_timer_set_periodic(struct clock_event_device *evt)
{
	gptimer_ctl_write(EVENT_TIMER_ID, TIMER_CTL_PERIOD_MODE);
	writel_relaxed(DIV_ROUND_CLOSEST(gptimer_freq, HZ),
		       gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_LOAD);
	gptimer_ctl_write(EVENT_TIMER_ID, TIMER_CTL_PERIOD_MODE | TIMER_CTL_ENABLE);
	writel_relaxed(TIMER_INT_EN,
		       gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);
	return 0;
}

static int sprd_timer_set_next_event(unsigned long cycles,
				     struct clock_event_device *evt)
{
	while (readl_relaxed(gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT) &
	       TIMER_INT_BUSY)
		cpu_relax();

	gptimer_ctl_write(EVENT_TIMER_ID, 0);
	writel_relaxed(cycles,
		       gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_LOAD);
	gptimer_ctl_write(EVENT_TIMER_ID, TIMER_CTL_ENABLE);
	return 0;
}

static struct clock_event_device sprd_clockevent = {
	.name			= "sprd-gptimer0",
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 250,
	.set_state_shutdown	= sprd_timer_shutdown,
	.set_state_oneshot	= sprd_timer_set_oneshot,
	.set_state_periodic	= sprd_timer_set_periodic,
	.set_next_event		= sprd_timer_set_next_event,
};

static irqreturn_t sprd_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	u32 val;

	/* Stop timer in oneshot mode */
	if (clockevent_state_oneshot(evt))
		gptimer_ctl_write(EVENT_TIMER_ID, 0);

	/* Clear interrupt */
	val = readl_relaxed(gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);
	val |= TIMER_INT_CLR;
	writel_relaxed(val, gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction sprd_timer_irq = {
	.name		= "gp_timer0",
	.flags		= IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= sprd_timer_interrupt,
	.dev_id		= &sprd_clockevent,
};

/*
 * GPTIMER Channel 1 (0x40050020): 32768 Hz 32-bit countdown timer used as
 * primary clocksource and sched_clock, matching 3.10 timer_sc8830.c.
 */
static u64 notrace sprd_gptimer1_read_sched_clock(void)
{
	return ~readl_relaxed(gptimer_base + TIMER_CH_OFFSET(SOURCE_TIMER_ID) + TIMER_CNT_RD);
}

static cycle_t sprd_gptimer1_read_cycles(struct clocksource *cs)
{
	return (cycle_t)~readl_relaxed(gptimer_base + TIMER_CH_OFFSET(SOURCE_TIMER_ID) + TIMER_CNT_RD);
}

static void sprd_gptimer1_resume(struct clocksource *cs)
{
	gptimer_ctl_write(SOURCE_TIMER_ID, TIMER_CTL_PERIOD_MODE | TIMER_CTL_ENABLE);
}

static struct clocksource sprd_gptimer1_clocksource = {
	.name	= "gp_timer1_32k",
	.rating	= 300,
	.read	= sprd_gptimer1_read_cycles,
	.resume	= sprd_gptimer1_resume,
	.mask	= CLOCKSOURCE_MASK(32),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int __init sprd_gptimer_init(struct device_node *np)
{
	int irq, ret;

	gptimer_base = of_iomap(np, 0);
	if (!gptimer_base)
		return -ENXIO;

	irq = irq_of_parse_and_map(np, 0);
	if (irq <= 0)
		return -EINVAL;

	of_property_read_u32(np, "clock-frequency", &gptimer_freq);

	/* Initialize Channel 1 as free-running 32768 Hz countdown clocksource */
	writel_relaxed(0, gptimer_base + TIMER_CH_OFFSET(SOURCE_TIMER_ID) + TIMER_INT);
	gptimer_ctl_write(SOURCE_TIMER_ID, 0);
	writel_relaxed(0xFFFFFFFFU, gptimer_base + TIMER_CH_OFFSET(SOURCE_TIMER_ID) + TIMER_LOAD);
	gptimer_ctl_write(SOURCE_TIMER_ID, TIMER_CTL_PERIOD_MODE | TIMER_CTL_ENABLE);

	sched_clock_register(sprd_gptimer1_read_sched_clock, 32, gptimer_freq);
	clocksource_register_hz(&sprd_gptimer1_clocksource, gptimer_freq);

	/* Initialize Channel 0 as clockevent */
	writel_relaxed(0, gptimer_base + TIMER_CH_OFFSET(EVENT_TIMER_ID) + TIMER_INT);
	gptimer_ctl_write(EVENT_TIMER_ID, 0);

	sprd_clockevent.cpumask = cpu_possible_mask;
	clockevents_config_and_register(&sprd_clockevent, gptimer_freq,
					0x4, 0x7fffffff);

	ret = setup_irq(irq, &sprd_timer_irq);
	if (ret)
		pr_err("sprd-gptimer: failed to request irq %d: %d\n", irq, ret);

	pr_info("sprd-gptimer: registered 32768 Hz clocksource & clockevent (irq %d)\n", irq);
	return 0;
}

CLOCKSOURCE_OF_DECLARE(sprd_gptimer, "sprd,sc8830-timer", sprd_gptimer_init);
CLOCKSOURCE_OF_DECLARE(sprd_sc7730se_timer, "sprd,sc7730se-timer", sprd_gptimer_init);

/*
 * SYSCNT (0x40230000): 1000 Hz (1 ms) secondary counter
 */
static cycle_t sprd_syscnt_read(struct clocksource *cs)
{
	return readl_relaxed(syscnt_base + SYSCNT_SHADOW_CNT);
}

static int __init sprd_syscnt_init(struct device_node *np)
{
	u32 freq = 1000;

	syscnt_base = of_iomap(np, 0);
	if (!syscnt_base)
		return -ENXIO;

	of_property_read_u32(np, "clock-frequency", &freq);
	if (freq > 2000)
		freq = 1000; /* Hardware SYSCNT on SC7730SE ticks at 1000 Hz */

	clocksource_mmio_init(syscnt_base + SYSCNT_SHADOW_CNT,
			      "sprd-syscnt-1khz", freq, 150, 32,
			      sprd_syscnt_read);
	return 0;
}

CLOCKSOURCE_OF_DECLARE(sprd_syscnt, "sprd,sc8830-syscnt", sprd_syscnt_init);
CLOCKSOURCE_OF_DECLARE(sprd_sc7730se_syscnt, "sprd,sc7730se-syscnt", sprd_syscnt_init);
