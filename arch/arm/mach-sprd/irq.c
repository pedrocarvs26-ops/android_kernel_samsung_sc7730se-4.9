/*
 * Spreadtrum SC7730SE / SC8830 Secondary Interrupt Controllers (INTC0..3 & AON_INTC)
 *
 * On SC7730SE (SCX35/SCX30G), peripheral interrupts pass 1-to-1 through four AP
 * secondary interrupt controllers (INTC0..3 at 0x71400000..0x71700000, bits 2..31)
 * and one AON interrupt controller (AON_INTC at 0x40200000, bits 2..7) before
 * reaching their dedicated Cortex-A7 GIC-400 SPI inputs (SPI 2..123 -> hwirq 34..155).
 *
 * Bits 0 and 1 of INTC0..3 are legacy summary OR-outputs connected to GIC SPI 0/1;
 * those must be disabled while bits 2..31 are enabled for 1-to-1 GIC SPI routing.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/io.h>

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

void __init sprd_init_irq(void)
{
	static const unsigned long intc_bases[] = {
		SPRD_INTC0_BASE,
		SPRD_INTC1_BASE,
		SPRD_INTC2_BASE,
		SPRD_INTC3_BASE,
	};
	int i;

	/* Enable INTC0..3 clocks in AP_APB_EB and AON_INTC clock in AON_APB_EB0 */
	sci_glb_set(REG_AP_APB_APB_EB,
		    BIT_INTC0_EB | BIT_INTC1_EB | BIT_INTC2_EB | BIT_INTC3_EB);
	sci_glb_set(REG_AON_APB_APB_EB0, BIT_INTC_EB);

	/*
	 * Configure INTC0..3:
	 * - Disable all FIQs
	 * - Disable bits 0..1 (legacy summary lines)
	 * - Enable bits 2..31 for 1-to-1 pass-through to GIC-400 SPI 2..123
	 *   (GIC-400 masks each individual SPI until request_irq() is called)
	 */
	for (i = 0; i < ARRAY_SIZE(intc_bases); i++) {
		__raw_writel(~0U, (void __iomem *)(intc_bases[i] + INTC_FIQ_DIS));
		__raw_writel(~0U, (void __iomem *)(intc_bases[i] + INTC_IRQ_DIS));
		__raw_writel(0xFFFFFFFCU, (void __iomem *)(intc_bases[i] + INTC_IRQ_EN));
	}

	/*
	 * Configure AON_INTC (0x40200000):
	 * Enable bits 2..7 (KPD, GPIO, AP_TMR0/GPTIMER, EIC, ADI/ANA, SYST)
	 */
	__raw_writel(~0U, (void __iomem *)(SPRD_INT_BASE + INTC_FIQ_DIS));
	__raw_writel(~0U, (void __iomem *)(SPRD_INT_BASE + INTC_IRQ_DIS));
	__raw_writel(0x000000FCU, (void __iomem *)(SPRD_INT_BASE + INTC_IRQ_EN));

	/*
	 * Mask legacy GIC PPI 29 and SPI 0..3 left enabled by S-Boot before
	 * initializing the GIC-400 driver.
	 */
	__raw_writel(BIT(29), (void __iomem *)(CORE_GIC_DIS_VA + 0x180));
	__raw_writel(0xFFFFFFFFU, (void __iomem *)(CORE_GIC_DIS_VA + 0x184));

	irqchip_init();
}
