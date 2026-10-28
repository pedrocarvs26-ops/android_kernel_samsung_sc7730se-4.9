/*
 * Spreadtrum SC7730SE / SC8830 Secondary Interrupt Controllers (INTC0..3 & AON_INTC)
 *
 * Matches arch/arm/mach-sc/irq.c from the working Samsung SM-T560 / SM-T561 3.10 kernel:
 * - Four AP secondary interrupt controllers (INTC0..3 at 0x71400000..0x71700000)
 * - One AON interrupt mux controller (SPRD_INT_BASE at 0x40200000)
 * - Disables Cortex-A7 legacy IRQ PPI 29 after GIC-400 initialization
 * - Unmasks/masks the corresponding INTC0..3 and AON_INTC lines whenever
 *   GIC-400 unmasks/masks an SPI interrupt.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/io.h>
#include <linux/spinlock.h>
#include <linux/export.h>

#include <soc/sprd/iomap.h>
#include "core.h"

#define INTC_IRQ_MSK_STS	0x0000
#define INTC_IRQ_RAW		0x0004
#define INTC_IRQ_EN		0x0008
#define INTC_IRQ_DIS		0x000C
#define INTC_IRQ_SOFT		0x0010
#define INTC_FIQ_STS		0x0020
#define INTC_FIQ_EN		0x0028
#define INTC_FIQ_DIS		0x002C

#define LEGACY_IRQ_BIT		29
#define GIC_DIST_ENABLE_CLEAR	0x180

struct sprd_intc_bank {
	unsigned long base;
	u32 min_spi;
	u32 max_spi;
	u32 bit_offset;
};

struct sprd_intc_mux {
	u32 spi;
	unsigned long base;
	u32 bit;
	u32 is_unmask;
};

static const struct sprd_intc_bank sprd_intc_banks[] = {
	{ SPRD_INTC0_BASE,  2,  31,  0 },
	{ SPRD_INTC1_BASE, 34,  63, 32 },
	{ SPRD_INTC2_BASE, 66,  95, 64 },
	{ SPRD_INTC3_BASE, 98, 124, 96 },
};

static struct sprd_intc_mux sprd_aon_mux[] = {
	{  30, SPRD_INT_BASE,  2, 0 }, {  31, SPRD_INT_BASE,  2, 0 },
	{  28, SPRD_INT_BASE,  2, 0 }, { 121, SPRD_INT_BASE,  2, 0 },
	{ 120, SPRD_INT_BASE,  2, 0 }, { 119, SPRD_INT_BASE,  2, 0 },
	{ 118, SPRD_INT_BASE,  2, 0 }, {  29, SPRD_INT_BASE,  2, 0 },
	{  21, SPRD_INT_BASE,  3, 0 }, {  22, SPRD_INT_BASE,  3, 0 },
	{  23, SPRD_INT_BASE,  3, 0 }, {  24, SPRD_INT_BASE,  3, 0 },
	{  35, SPRD_INT_BASE,  4, 0 }, {  36, SPRD_INT_BASE,  4, 0 },
	{  38, SPRD_INT_BASE,  4, 0 }, {  25, SPRD_INT_BASE,  4, 0 },
	{  27, SPRD_INT_BASE,  4, 0 }, {  20, SPRD_INT_BASE,  4, 0 },
	{  34, SPRD_INT_BASE,  4, 0 },
	{  41, SPRD_INT_BASE,  5, 0 }, {  40, SPRD_INT_BASE,  5, 0 },
	{  42, SPRD_INT_BASE,  5, 0 }, {  44, SPRD_INT_BASE,  5, 0 },
	{  45, SPRD_INT_BASE,  5, 0 }, {  43, SPRD_INT_BASE,  5, 0 },
	{  39, SPRD_INT_BASE,  6, 0 },
	{  85, SPRD_INT_BASE,  7, 0 }, {  84, SPRD_INT_BASE,  7, 0 },
	{  83, SPRD_INT_BASE,  7, 0 },
	{  67, SPRD_INT_BASE,  8, 0 }, {  68, SPRD_INT_BASE,  8, 0 },
	{  69, SPRD_INT_BASE,  8, 0 },
	{  70, SPRD_INT_BASE,  9, 0 }, {  71, SPRD_INT_BASE,  9, 0 },
	{  72, SPRD_INT_BASE,  9, 0 },
	{  73, SPRD_INT_BASE, 10, 0 }, {  74, SPRD_INT_BASE, 10, 0 },
	{ 123, SPRD_INT_BASE, 11, 0 }, { 124, SPRD_INT_BASE, 11, 0 },
	{  26, SPRD_INT_BASE, 12, 0 }, { 122, SPRD_INT_BASE, 12, 0 },
	{  86, SPRD_INT_BASE, 13, 0 },
	{  37, SPRD_INT_BASE, 14, 0 },
};

static DEFINE_RAW_SPINLOCK(sprd_intc_lock);
static bool sprd_intc_ready;

static void sprd_mux_update(u32 spi, u32 offset, u32 is_unmask)
{
	int i, j;
	bool keep_enabled = false;

	for (i = 0; i < ARRAY_SIZE(sprd_aon_mux); i++) {
		if (sprd_aon_mux[i].spi != spi)
			continue;

		sprd_aon_mux[i].is_unmask = !!is_unmask;
		if (is_unmask) {
			__raw_writel(BIT(sprd_aon_mux[i].bit),
				     (void __iomem *)(sprd_aon_mux[i].base + offset));
		} else {
			for (j = 0; j < ARRAY_SIZE(sprd_aon_mux); j++) {
				if (sprd_aon_mux[j].bit == sprd_aon_mux[i].bit &&
				    sprd_aon_mux[j].is_unmask) {
					keep_enabled = true;
					break;
				}
			}
			if (!keep_enabled)
				__raw_writel(BIT(sprd_aon_mux[i].bit),
					     (void __iomem *)(sprd_aon_mux[i].base + offset));
		}
		return;
	}
}

void sprd_intc_unmask(u32 gic_hwirq)
{
	unsigned long flags;
	u32 spi;
	int i;

	if (!sprd_intc_ready || gic_hwirq < 32)
		return;

	spi = gic_hwirq - 32;

	raw_spin_lock_irqsave(&sprd_intc_lock, flags);
	sprd_mux_update(spi, INTC_IRQ_EN, 1);
	for (i = 0; i < ARRAY_SIZE(sprd_intc_banks); i++) {
		if (spi >= sprd_intc_banks[i].min_spi &&
		    spi <= sprd_intc_banks[i].max_spi) {
			u32 bit = spi - sprd_intc_banks[i].bit_offset;
			__raw_writel(BIT(bit),
				     (void __iomem *)(sprd_intc_banks[i].base + INTC_IRQ_EN));
			break;
		}
	}
	raw_spin_unlock_irqrestore(&sprd_intc_lock, flags);
}
EXPORT_SYMBOL(sprd_intc_unmask);

void sprd_intc_mask(u32 gic_hwirq)
{
	unsigned long flags;
	u32 spi;
	int i;

	if (!sprd_intc_ready || gic_hwirq < 32)
		return;

	spi = gic_hwirq - 32;

	raw_spin_lock_irqsave(&sprd_intc_lock, flags);
	sprd_mux_update(spi, INTC_IRQ_DIS, 0);
	for (i = 0; i < ARRAY_SIZE(sprd_intc_banks); i++) {
		if (spi >= sprd_intc_banks[i].min_spi &&
		    spi <= sprd_intc_banks[i].max_spi) {
			u32 bit = spi - sprd_intc_banks[i].bit_offset;
			__raw_writel(BIT(bit),
				     (void __iomem *)(sprd_intc_banks[i].base + INTC_IRQ_DIS));
			break;
		}
	}
	raw_spin_unlock_irqrestore(&sprd_intc_lock, flags);
}
EXPORT_SYMBOL(sprd_intc_mask);

void __init sprd_init_irq(void)
{
	int i;

	/* Enable INTC0..3 clocks in AP_APB_EB (bits 19..22) and AON_INTC in AON_APB_EB0 (bit 15) */
	sci_glb_set(REG_AP_APB_APB_EB,
		    BIT_INTC0_EB | BIT_INTC1_EB | BIT_INTC2_EB | BIT_INTC3_EB);
	sci_glb_set(REG_AON_APB_APB_EB0, BIT_INTC_EB);

	for (i = 0; i < ARRAY_SIZE(sprd_intc_banks); i++) {
		__raw_writel(~0U, (void __iomem *)(sprd_intc_banks[i].base + INTC_IRQ_DIS));
		__raw_writel(~0U, (void __iomem *)(sprd_intc_banks[i].base + INTC_FIQ_DIS));
	}
	__raw_writel(~0U, (void __iomem *)(SPRD_INT_BASE + INTC_IRQ_DIS));
	__raw_writel(~0U, (void __iomem *)(SPRD_INT_BASE + INTC_FIQ_DIS));

	sprd_intc_ready = true;

	/* Initialize primary GIC-400 first, matching 3.10 sci_init_irq() */
	irqchip_init();

	/* Disable legacy IRQ PPI 29 in GIC Distributor after irqchip_init() */
	__raw_writel(BIT(LEGACY_IRQ_BIT),
		     (void __iomem *)(CORE_GIC_DIS_VA + GIC_DIST_ENABLE_CLEAR));
}
