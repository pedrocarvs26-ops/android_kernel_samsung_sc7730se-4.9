/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) ADI Master Bus Driver
 *
 * Provides read/write access to the SC2723 (A-Die) analog/PMIC registers
 * mapped at SPRD_ADISLAVE_BASE (virtual 0xF5388000, physical 0x40038000)
 * via the ADI controller at SPRD_ADI_BASE (virtual 0xF5380000, physical 0x40030000).
 *
 * Adapted from arch/arm/mach-sc/adi.c in the 3.10 kernel.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/delay.h>
#include <linux/ratelimit.h>
#include <linux/spinlock.h>
#include <linux/io.h>

#include <soc/sprd/iomap.h>
#include "core.h"

#define CTL_ADI_BASE			SPRD_ADI_BASE

#define REG_ADI_CTRL0			(CTL_ADI_BASE + 0x04)
#define REG_ADI_CHNL_PRI		(CTL_ADI_BASE + 0x08)
#define REG_ADI_INT_RAW			(CTL_ADI_BASE + 0x10)
#define REG_ADI_GSSI_CFG0		(CTL_ADI_BASE + 0x1C)
#define REG_ADI_GSSI_CFG1		(CTL_ADI_BASE + 0x20)
#define REG_ADI_RD_CMD			(CTL_ADI_BASE + 0x24)
#define REG_ADI_RD_DATA			(CTL_ADI_BASE + 0x28)
#define REG_ADI_FIFO_STS		(CTL_ADI_BASE + 0x2C)
#define REG_ADI_CHNL_EN			(CTL_ADI_BASE + 0x3C)

#define BIT_RD_CMD_BUSY			BIT(31)
#define SHIFT_RD_ADDR			16
#define MASK_RD_VALU			0xFFFF
#define BIT_FIFO_FULL			BIT(11)
#define BIT_FIFO_EMPTY			BIT(10)
#define VALUE_CH_PRI			0x0

#define ANA_VIRT_BASE			SPRD_ADISLAVE_BASE
#define ANA_PHYS_BASE			SPRD_ADISLAVE_PHYS
#define ANA_ADDR_SIZE			SZ_4K

static DEFINE_SPINLOCK(adi_lock);
static u32 readback_addr_mak __read_mostly = 0xfff;
static bool adi_initialized;

#define FIFO_IS_FULL()	(__raw_readl((void __iomem *)REG_ADI_FIFO_STS) & BIT_FIFO_FULL)
#define TO_ADDR(_x_)	(((_x_) >> SHIFT_RD_ADDR) & readback_addr_mak)

int sci_is_adi_vaddr(unsigned long vaddr)
{
	return (vaddr >= ANA_VIRT_BASE && vaddr <= (ANA_VIRT_BASE + ANA_ADDR_SIZE));
}
EXPORT_SYMBOL(sci_is_adi_vaddr);

int sci_adi_p2v(unsigned long paddr, unsigned long *vaddr)
{
	if (paddr < ANA_PHYS_BASE || paddr > (ANA_PHYS_BASE + ANA_ADDR_SIZE))
		return -EINVAL;

	*vaddr = paddr - ANA_PHYS_BASE + ANA_VIRT_BASE;
	return 0;
}
EXPORT_SYMBOL(sci_adi_p2v);

static inline unsigned long __adi_translate_addr(unsigned long regvddr)
{
	if (regvddr >= ANA_PHYS_BASE && regvddr <= (ANA_PHYS_BASE + ANA_ADDR_SIZE))
		return regvddr;
	return regvddr - ANA_VIRT_BASE + ANA_PHYS_BASE;
}

static inline unsigned long __adi_virt_addr(unsigned long reg)
{
	if (reg >= ANA_PHYS_BASE && reg <= (ANA_PHYS_BASE + ANA_ADDR_SIZE))
		return reg - ANA_PHYS_BASE + ANA_VIRT_BASE;
	return reg;
}

static inline int __adi_fifo_drain(void)
{
	int cnt = 2000;

	while (!(__raw_readl((void __iomem *)REG_ADI_FIFO_STS) & BIT_FIFO_EMPTY) && --cnt)
		udelay(1);

	return (cnt == 0) ? -ETIMEDOUT : 0;
}

static inline int __adi_read(unsigned long reg_paddr, unsigned int *v)
{
	unsigned long val;
	int cnt = 3000;

	__raw_writel((u32)reg_paddr, (void __iomem *)REG_ADI_RD_CMD);

	do {
		val = __raw_readl((void __iomem *)REG_ADI_RD_DATA);
	} while ((val & BIT_RD_CMD_BUSY) && --cnt);

	if (!cnt || !v || TO_ADDR(val) != (reg_paddr & readback_addr_mak))
		return -EIO;

	*v = val & MASK_RD_VALU;
	return 0;
}

static inline int __adi_write_sync(unsigned long reg_vaddr, u16 val)
{
	int cnt = 2000;

	__adi_fifo_drain();
	while (FIFO_IS_FULL() && --cnt)
		udelay(1);

	__raw_writel(val, (void __iomem *)reg_vaddr);
	return __adi_fifo_drain();
}

int sci_adi_read(unsigned long reg)
{
	unsigned long flags;
	unsigned int val = 0;
	int ret;

	if (!adi_initialized)
		sci_adi_init();

	spin_lock_irqsave(&adi_lock, flags);
	ret = __adi_read(__adi_translate_addr(reg), &val);
	spin_unlock_irqrestore(&adi_lock, flags);

	if (ret) {
		pr_warn_ratelimited("SPRD ADI: read timeout at 0x%lx\n", reg);
		return 0;
	}
	return (int)val;
}
EXPORT_SYMBOL(sci_adi_read);

int sci_adi_write_fast(unsigned long reg, u16 val, u32 sync)
{
	unsigned long flags;

	if (!adi_initialized)
		sci_adi_init();

	spin_lock_irqsave(&adi_lock, flags);
	__adi_write_sync(__adi_virt_addr(reg), val);
	spin_unlock_irqrestore(&adi_lock, flags);

	return 0;
}
EXPORT_SYMBOL(sci_adi_write_fast);

int sci_adi_raw_write(unsigned long reg, u16 val)
{
	return sci_adi_write_fast(reg, val, 1);
}
EXPORT_SYMBOL(sci_adi_raw_write);

int sci_adi_write(unsigned long reg, u16 or_val, u16 clear_msk)
{
	unsigned long flags;
	unsigned int val = 0;
	int ret;

	if (!adi_initialized)
		sci_adi_init();

	spin_lock_irqsave(&adi_lock, flags);
	ret = __adi_read(__adi_translate_addr(reg), &val);
	if (!ret)
		__adi_write_sync(__adi_virt_addr(reg), (val & ~clear_msk) | or_val);
	spin_unlock_irqrestore(&adi_lock, flags);

	return ret;
}
EXPORT_SYMBOL(sci_adi_write);

int sci_adi_init(void)
{
	u32 value;

	if (adi_initialized)
		return 0;

	/* Enable ADI clock in AON_APB_EB0 */
	sci_glb_set(REG_AON_APB_APB_EB0, BIT_ADI_EB);

	__raw_writel(VALUE_CH_PRI, (void __iomem *)REG_ADI_CHNL_PRI);

	value = __raw_readl((void __iomem *)REG_ADI_CHNL_EN);
	value |= BIT(2) | BIT(3);
	__raw_writel(value, (void __iomem *)REG_ADI_CHNL_EN);

	value = __raw_readl((void __iomem *)REG_ADI_GSSI_CFG0);
	if (value) {
		u32 bits = (value & 0x3f) - ((value >> 11) & 0x1f) - 1;
		if (bits > 0 && bits < 16)
			readback_addr_mak = (1U << (bits + 2)) - 1;
	}

	adi_initialized = true;
	return 0;
}
EXPORT_SYMBOL(sci_adi_init);
