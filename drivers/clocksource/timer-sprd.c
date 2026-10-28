/*
 * Spreadtrum SC7730SE / SC8830 Timer & Clocksource Driver
 *
 * Ported 1:1 from arch/arm/mach-sc/timer_sc8830.c in the working Samsung
 * SM-T560 / SM-T561 3.10 kernel:
 * - 4 per-CPU local clockevents (Channel 0 of AON_TMR, AP_TMR0, AP_TMR1, AP_TMR2;
 *   GIC SPIs 28, 29, 119, 121) registered via CPUHP_AP_MARCO_TIMER_STARTING
 *   with cpumask = cpumask_of(cpu) and ce->irq properly set.
 * - Broadcast clockevent (Channel 1 of AP_TMR0 at 0x40220020, GIC SPI 118).
 * - 32768 Hz 32-bit countdown clocksource & sched_clock (Channel 1 of AON_TMR
 *   at 0x40050020).
 * - 1000 Hz SYSCNT secondary clocksource at 0x40230000.
 *
 * Licensed under the GPL-2.
 */

#include <linux/init.h>
#include <linux/errno.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/clockchips.h>
#include <linux/clocksource.h>
#include <linux/sched_clock.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>

#include <soc/sprd/iomap.h>

#define LOCAL_TIMER_CNT		4

/* Timer channel register offsets (stride 0x20 per channel) */
#define TIMER_CH_OFFSET(id)	((id) * 0x20)
#define TIMER_LOAD		0x0000
#define TIMER_VALUE		0x0004
#define TIMER_CTL		0x0008
#define TIMER_INT		0x000C
#define TIMER_CNT_RD		0x0010

#define ONETIME_MODE		(0 << 6)
#define PERIOD_MODE		(1 << 6)
#define TIMER_DISABLE		(0 << 7)
#define TIMER_ENABLE		(1 << 7)
#define TIMER_NEW		(1 << 8)

#define TIMER_INT_EN		(1 << 0)
#define TIMER_INT_RAW_STS	(1 << 1)
#define TIMER_INT_MSK_STS	(1 << 2)
#define TIMER_INT_CLR		(1 << 3)
#define TIMER_INT_BUSY		(1 << 4)

#define EVENT_TIMER		0
#define BC_TIMER		1
#define SOURCE_TIMER		1
#define BC_CPU			1

/* SYSCNT register offsets (0x40230000, 1000 Hz) */
#define SYSCNT_COUNT		0x0004
#define SYSCNT_CTL		0x0008
#define SYSCNT_SHADOW_CNT	0x000C

static void __iomem *base_gptimer[LOCAL_TIMER_CNT] = {
	(void __iomem *)SPRD_GPTIMER_BASE,
	(void __iomem *)SPRD_APTIMER0_BASE,
	(void __iomem *)SPRD_APTIMER1_BASE,
	(void __iomem *)SPRD_APTIMER2_BASE,
};

static int irq_nr[LOCAL_TIMER_CNT];
static int bc_irq;
static void __iomem *syscnt_base = (void __iomem *)SPRD_SYSCNT_BASE;
static u32 gptimer_freq = 32768;

static DEFINE_PER_CPU(struct clock_event_device, sprd_local_evt);

static inline void __iomem *timer_reg(int idx, int ch, u32 reg)
{
	return base_gptimer[idx] + TIMER_CH_OFFSET(ch) + reg;
}

static inline void __gptimer_ctl(int idx, int ch, u32 enable, u32 mode)
{
	__raw_writel(enable | mode, timer_reg(idx, ch, TIMER_CTL));
}

/*
 * Per-CPU local clockevent callbacks (Channel 0 of base_gptimer[cpu])
 */
static int sprd_local_set_next_event(unsigned long cycles,
				     struct clock_event_device *c)
{
	int cpu = smp_processor_id();

	while (__raw_readl(timer_reg(cpu, EVENT_TIMER, TIMER_INT)) & TIMER_INT_BUSY)
		cpu_relax();

	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_DISABLE, ONETIME_MODE);
	__raw_writel(cycles, timer_reg(cpu, EVENT_TIMER, TIMER_LOAD));
	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_ENABLE, ONETIME_MODE);
	return 0;
}

static int sprd_local_set_periodic(struct clock_event_device *c)
{
	int cpu = smp_processor_id();

	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_DISABLE, PERIOD_MODE);
	__raw_writel(DIV_ROUND_CLOSEST(gptimer_freq, HZ),
		     timer_reg(cpu, EVENT_TIMER, TIMER_LOAD));
	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_ENABLE, PERIOD_MODE);
	__raw_writel(TIMER_INT_EN, timer_reg(cpu, EVENT_TIMER, TIMER_INT));
	return 0;
}

static int sprd_local_set_oneshot(struct clock_event_device *c)
{
	int cpu = smp_processor_id();

	__raw_writel(DIV_ROUND_CLOSEST(gptimer_freq, HZ),
		     timer_reg(cpu, EVENT_TIMER, TIMER_LOAD));
	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_ENABLE, ONETIME_MODE);
	__raw_writel(TIMER_INT_EN, timer_reg(cpu, EVENT_TIMER, TIMER_INT));
	return 0;
}

static int sprd_local_shutdown(struct clock_event_device *c)
{
	int cpu = smp_processor_id();
	u32 saved;

	__raw_writel(TIMER_INT_CLR, timer_reg(cpu, EVENT_TIMER, TIMER_INT));
	saved = __raw_readl(timer_reg(cpu, EVENT_TIMER, TIMER_CTL)) & PERIOD_MODE;
	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_DISABLE, saved);
	return 0;
}

static int sprd_local_resume(struct clock_event_device *c)
{
	int cpu = smp_processor_id();
	u32 saved;

	saved = __raw_readl(timer_reg(cpu, EVENT_TIMER, TIMER_CTL)) & PERIOD_MODE;
	__gptimer_ctl(cpu, EVENT_TIMER, TIMER_ENABLE, saved);
	return 0;
}

static irqreturn_t sprd_local_timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	int cpu = smp_processor_id();
	u32 val;

	val = __raw_readl(timer_reg(cpu, EVENT_TIMER, TIMER_INT));
	val |= TIMER_INT_CLR;
	__raw_writel(val, timer_reg(cpu, EVENT_TIMER, TIMER_INT));

	if (evt && evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction sprd_local_irqs[LOCAL_TIMER_CNT] = {
	[0] = {
		.name		= "local_timer0",
		.flags		= IRQF_TIMER | IRQF_NOBALANCING | IRQF_IRQPOLL,
		.handler	= sprd_local_timer_interrupt,
	},
	[1] = {
		.name		= "local_timer1",
		.flags		= IRQF_TIMER | IRQF_NOBALANCING | IRQF_IRQPOLL,
		.handler	= sprd_local_timer_interrupt,
	},
	[2] = {
		.name		= "local_timer2",
		.flags		= IRQF_TIMER | IRQF_NOBALANCING | IRQF_IRQPOLL,
		.handler	= sprd_local_timer_interrupt,
	},
	[3] = {
		.name		= "local_timer3",
		.flags		= IRQF_TIMER | IRQF_NOBALANCING | IRQF_IRQPOLL,
		.handler	= sprd_local_timer_interrupt,
	},
};

static int sprd_local_timer_starting_cpu(unsigned int cpu)
{
	struct clock_event_device *evt;

	if (cpu >= LOCAL_TIMER_CNT || irq_nr[cpu] <= 0)
		return 0;

	evt = per_cpu_ptr(&sprd_local_evt, cpu);
	evt->name		= "local_timer";
	evt->features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC;
	evt->rating		= 250;
	evt->irq		= irq_nr[cpu];
	evt->cpumask		= cpumask_of(cpu);
	evt->set_state_shutdown	= sprd_local_shutdown;
	evt->set_state_periodic	= sprd_local_set_periodic;
	evt->set_state_oneshot	= sprd_local_set_oneshot;
	evt->tick_resume	= sprd_local_resume;
	evt->set_next_event	= sprd_local_set_next_event;

	irq_force_affinity(irq_nr[cpu], cpumask_of(cpu));
	clockevents_config_and_register(evt, gptimer_freq, 4, 0xf0000000);
	return 0;
}

static int sprd_local_timer_dying_cpu(unsigned int cpu)
{
	if (cpu >= LOCAL_TIMER_CNT)
		return 0;

	sprd_local_shutdown(per_cpu_ptr(&sprd_local_evt, cpu));
	return 0;
}

/*
 * Broadcast clockevent (Channel 1 of AP_TMR0 at 0x40220020, GIC SPI 118)
 */
static int sprd_bctimer_set_next_event(unsigned long cycles,
				       struct clock_event_device *c)
{
	while (__raw_readl(timer_reg(BC_CPU, BC_TIMER, TIMER_INT)) & TIMER_INT_BUSY)
		cpu_relax();

	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_DISABLE, ONETIME_MODE);
	__raw_writel(cycles, timer_reg(BC_CPU, BC_TIMER, TIMER_LOAD));
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_ENABLE, ONETIME_MODE);
	return 0;
}

static int sprd_bctimer_set_periodic(struct clock_event_device *c)
{
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_DISABLE, PERIOD_MODE);
	__raw_writel(DIV_ROUND_CLOSEST(gptimer_freq, HZ),
		     timer_reg(BC_CPU, BC_TIMER, TIMER_LOAD));
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_ENABLE, PERIOD_MODE);
	__raw_writel(TIMER_INT_EN, timer_reg(BC_CPU, BC_TIMER, TIMER_INT));
	return 0;
}

static int sprd_bctimer_set_oneshot(struct clock_event_device *c)
{
	__raw_writel(DIV_ROUND_CLOSEST(gptimer_freq, HZ),
		     timer_reg(BC_CPU, BC_TIMER, TIMER_LOAD));
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_ENABLE, ONETIME_MODE);
	__raw_writel(TIMER_INT_EN, timer_reg(BC_CPU, BC_TIMER, TIMER_INT));
	return 0;
}

static int sprd_bctimer_shutdown(struct clock_event_device *c)
{
	u32 saved;

	__raw_writel(TIMER_INT_CLR, timer_reg(BC_CPU, BC_TIMER, TIMER_INT));
	saved = __raw_readl(timer_reg(BC_CPU, BC_TIMER, TIMER_CTL)) & PERIOD_MODE;
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_DISABLE, saved);
	return 0;
}

static int sprd_bctimer_resume(struct clock_event_device *c)
{
	u32 saved;

	saved = __raw_readl(timer_reg(BC_CPU, BC_TIMER, TIMER_CTL)) & PERIOD_MODE;
	__gptimer_ctl(BC_CPU, BC_TIMER, TIMER_ENABLE, saved);
	return 0;
}

static struct clock_event_device bctimer_event = {
	.name			= "bctimer",
	.features		= CLOCK_EVT_FEAT_ONESHOT | CLOCK_EVT_FEAT_PERIODIC,
	.rating			= 150,
	.set_state_shutdown	= sprd_bctimer_shutdown,
	.set_state_periodic	= sprd_bctimer_set_periodic,
	.set_state_oneshot	= sprd_bctimer_set_oneshot,
	.tick_resume		= sprd_bctimer_resume,
	.set_next_event		= sprd_bctimer_set_next_event,
};

static irqreturn_t sprd_bctimer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *evt = dev_id;
	u32 val;

	val = __raw_readl(timer_reg(BC_CPU, BC_TIMER, TIMER_INT));
	val |= TIMER_INT_CLR;
	__raw_writel(val, timer_reg(BC_CPU, BC_TIMER, TIMER_INT));

	if (evt->event_handler)
		evt->event_handler(evt);

	return IRQ_HANDLED;
}

static struct irqaction bctimer_irq = {
	.name		= "bctimer",
	.flags		= IRQF_TIMER | IRQF_IRQPOLL,
	.handler	= sprd_bctimer_interrupt,
	.dev_id		= &bctimer_event,
};

/*
 * GPTIMER0 Channel 1 (0x40050020): 32768 Hz 32-bit countdown timer used as
 * primary clocksource and sched_clock, matching 3.10 timer_sc8830.c.
 */
static u64 notrace sprd_gptimer1_read_sched_clock(void)
{
	return ~(u32)readl_relaxed(timer_reg(0, SOURCE_TIMER, TIMER_CNT_RD));
}

static cycle_t sprd_gptimer1_read_cycles(struct clocksource *cs)
{
	return (cycle_t)~readl_relaxed(timer_reg(0, SOURCE_TIMER, TIMER_CNT_RD));
}

static void sprd_gptimer1_resume(struct clocksource *cs)
{
	__gptimer_ctl(0, SOURCE_TIMER, TIMER_ENABLE | TIMER_NEW, PERIOD_MODE);
}

static void sprd_gptimer1_suspend(struct clocksource *cs)
{
	__gptimer_ctl(0, SOURCE_TIMER, TIMER_DISABLE, PERIOD_MODE);
}

static struct clocksource sprd_gptimer1_clocksource = {
	.name		= "aon_timer_timer1",
	.rating		= 300,
	.read		= sprd_gptimer1_read_cycles,
	.resume		= sprd_gptimer1_resume,
	.suspend	= sprd_gptimer1_suspend,
	.mask		= CLOCKSOURCE_MASK(32),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int __init sprd_gptimer_init(struct device_node *np)
{
	int i, j, ret;

	for (i = 0; i < LOCAL_TIMER_CNT; i++) {
		void __iomem *mapped = of_iomap(np, i);
		if (mapped)
			base_gptimer[i] = mapped;

		irq_nr[i] = irq_of_parse_and_map(np, i);
	}
	bc_irq = irq_of_parse_and_map(np, LOCAL_TIMER_CNT);

	of_property_read_u32(np, "clock-frequency", &gptimer_freq);

	/* Disable all timer channels and clear pending interrupts (matching sci_enable_timer_early) */
	for (i = 0; i < LOCAL_TIMER_CNT; i++) {
		for (j = 0; j < 3; j++) {
			__gptimer_ctl(i, j, TIMER_DISABLE, 0);
			__raw_writel(TIMER_INT_CLR, timer_reg(i, j, TIMER_INT));
		}
	}

	/* Initialize AON_TMR Channel 1 as 32768 Hz free-running clocksource & sched_clock */
	__raw_writel(0, timer_reg(0, SOURCE_TIMER, TIMER_INT));
	__gptimer_ctl(0, SOURCE_TIMER, TIMER_DISABLE | TIMER_NEW, PERIOD_MODE);
	__raw_writel(0xFFFFFFFFU, timer_reg(0, SOURCE_TIMER, TIMER_LOAD));
	__gptimer_ctl(0, SOURCE_TIMER, TIMER_NEW, PERIOD_MODE);
	__gptimer_ctl(0, SOURCE_TIMER, TIMER_ENABLE | TIMER_NEW, PERIOD_MODE);

	sched_clock_register(sprd_gptimer1_read_sched_clock, 32, gptimer_freq);
	clocksource_register_hz(&sprd_gptimer1_clocksource, gptimer_freq);

	/* Register per-CPU local timer IRQs BEFORE registering clockevents */
	for (i = 0; i < LOCAL_TIMER_CNT; i++) {
		if (irq_nr[i] <= 0)
			continue;
		sprd_local_irqs[i].dev_id = per_cpu_ptr(&sprd_local_evt, i);
		ret = setup_irq(irq_nr[i], &sprd_local_irqs[i]);
		if (ret)
			pr_err("sprd-gptimer: failed to setup local_timer%d irq %d: %d\n",
			       i, irq_nr[i], ret);
	}

	/* Install and invoke per-CPU local timer hotplug callback (starts CPU0 timer now) */
	cpuhp_setup_state(CPUHP_AP_MARCO_TIMER_STARTING,
			  "AP_MARCO_TIMER_STARTING",
			  sprd_local_timer_starting_cpu,
			  sprd_local_timer_dying_cpu);

	/* Register broadcast timer on AP_TMR0 Channel 1 (GIC SPI 118) */
	if (bc_irq > 0) {
		bctimer_event.irq = bc_irq;
		bctimer_event.cpumask = cpu_possible_mask;
		ret = setup_irq(bc_irq, &bctimer_irq);
		if (ret)
			pr_err("sprd-gptimer: failed to setup bctimer irq %d: %d\n",
			       bc_irq, ret);
		clockevents_config_and_register(&bctimer_event, gptimer_freq,
						2, 0xffffffffU);
	}

	pr_info("sprd-gptimer: registered 32768 Hz clocksource, 4 local timers & bctimer (cpu0 irq %d, bc irq %d)\n",
		irq_nr[0], bc_irq);
	return 0;
}

CLOCKSOURCE_OF_DECLARE(sprd_gptimer, "sprd,sc8830-timer", sprd_gptimer_init);
CLOCKSOURCE_OF_DECLARE(sprd_sc7730se_timer, "sprd,sc7730se-timer", sprd_gptimer_init);
CLOCKSOURCE_OF_DECLARE(sprd_scx35_timer, "sprd,scx35-timer", sprd_gptimer_init);

/*
 * SYSCNT (0x40230000): 1000 Hz (1 ms) secondary counter
 */
static cycle_t sprd_syscnt_read(struct clocksource *cs)
{
	return readl_relaxed(syscnt_base + SYSCNT_SHADOW_CNT);
}

static int __init sprd_syscnt_init(struct device_node *np)
{
	void __iomem *mapped;
	u32 freq = 1000;

	mapped = of_iomap(np, 0);
	if (mapped)
		syscnt_base = mapped;

	__raw_writel(0, syscnt_base + SYSCNT_CTL);

	of_property_read_u32(np, "clock-frequency", &freq);
	if (freq > 2000)
		freq = 1000;

	clocksource_mmio_init(syscnt_base + SYSCNT_SHADOW_CNT,
			      "syscnt", freq, 200, 32,
			      sprd_syscnt_read);
	return 0;
}

CLOCKSOURCE_OF_DECLARE(sprd_syscnt, "sprd,sc8830-syscnt", sprd_syscnt_init);
CLOCKSOURCE_OF_DECLARE(sprd_sc7730se_syscnt, "sprd,sc7730se-syscnt", sprd_syscnt_init);
