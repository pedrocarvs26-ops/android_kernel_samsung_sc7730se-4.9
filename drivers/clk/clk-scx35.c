/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) Common Clock Framework Driver
 *
 * Adapted from drivers/clk/clk-sc.c in the Samsung SM-T560 / SM-T561 3.10 kernel
 * for Linux 4.9.337.
 *
 * Licensed under the GPL-2.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/spinlock.h>

#include <soc/sprd/iomap.h>

static DEFINE_SPINLOCK(sprd_clk_lock);

struct cfg_reg {
	void __iomem *reg;
	u32 msk;
};

struct clk_sprd {
	struct clk_hw hw;
	struct cfg_reg enb;
	struct cfg_reg pre;
	struct cfg_reg mul;
	struct cfg_reg sel;
	struct cfg_reg div;
	unsigned long fixed_rate;
	u8 flags;
};

#define to_clk_sprd(_hw) container_of(_hw, struct clk_sprd, hw)

#define in_range(b, first, len)	((b) >= (first) && (b) <= (first) + (len) - 1)
#define to_range(b, first, base) ((b) - (first) + (base))

static void __iomem *cfg_reg_p2v(u32 regp)
{
	unsigned long regv = 0;

	if (!regp)
		return NULL;

	if (in_range(regp, SPRD_AHB_PHYS, SPRD_AHB_SIZE))
		regv = to_range(regp, SPRD_AHB_PHYS, SPRD_AHB_BASE);
	else if (in_range(regp, SPRD_PMU_PHYS, SPRD_PMU_SIZE))
		regv = to_range(regp, SPRD_PMU_PHYS, SPRD_PMU_BASE);
	else if (in_range(regp, SPRD_AONAPB_PHYS, SPRD_AONAPB_SIZE))
		regv = to_range(regp, SPRD_AONAPB_PHYS, SPRD_AONAPB_BASE);
	else if (in_range(regp, SPRD_AONCKG_PHYS, SPRD_AONCKG_SIZE))
		regv = to_range(regp, SPRD_AONCKG_PHYS, SPRD_AONCKG_BASE);
	else if (in_range(regp, SPRD_GPUCKG_PHYS, SPRD_GPUCKG_SIZE))
		regv = to_range(regp, SPRD_GPUCKG_PHYS, SPRD_GPUCKG_BASE);
	else if (in_range(regp, SPRD_MMCKG_PHYS, SPRD_MMCKG_SIZE))
		regv = to_range(regp, SPRD_MMCKG_PHYS, SPRD_MMCKG_BASE);
	else if (in_range(regp, SPRD_MMAHB_PHYS, SPRD_MMAHB_SIZE))
		regv = to_range(regp, SPRD_MMAHB_PHYS, SPRD_MMAHB_BASE);
	else if (in_range(regp, SPRD_APBREG_PHYS, SPRD_APBREG_SIZE))
		regv = to_range(regp, SPRD_APBREG_PHYS, SPRD_APBREG_BASE);
	else if (in_range(regp, SPRD_APBCKG_PHYS, SPRD_APBCKG_SIZE))
		regv = to_range(regp, SPRD_APBCKG_PHYS, SPRD_APBCKG_BASE);
	else
		return ioremap(regp & PAGE_MASK, PAGE_SIZE) + (regp & ~PAGE_MASK);

	return (void __iomem *)regv;
}

static void of_read_reg(struct cfg_reg *cfg, const __be32 *cell)
{
	if (!cell)
		return;
	cfg->reg = cfg_reg_p2v(be32_to_cpu(cell[0]));
	cfg->msk = be32_to_cpu(cell[1]);
}

static void __glbreg_setclr(void __iomem *reg, u32 msk, int is_set)
{
	unsigned long flags;
	unsigned long addr = (unsigned long)reg;

	if (!reg || !msk)
		return;

	spin_lock_irqsave(&sprd_clk_lock, flags);
	if (is_set)
		__raw_writel(msk, (void __iomem *)(addr + 0x1000));
	else
		__raw_writel(msk, (void __iomem *)(addr + 0x2000));
	spin_unlock_irqrestore(&sprd_clk_lock, flags);
}

static int sprd_clk_prepare(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	int set = !!(c->flags & CLK_GATE_SET_TO_DISABLE);

	if (c->pre.reg)
		__glbreg_setclr(c->pre.reg, c->pre.msk, set ^ 1);
	return 0;
}

static void sprd_clk_unprepare(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	int set = !!(c->flags & CLK_GATE_SET_TO_DISABLE);

	if (c->pre.reg)
		__glbreg_setclr(c->pre.reg, c->pre.msk, set ^ 0);
}

static int sprd_clk_is_prepared(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	int ret, set = !!(c->flags & CLK_GATE_SET_TO_DISABLE);

	if (!c->pre.reg)
		return 1;

	ret = !!(__raw_readl(c->pre.reg) & c->pre.msk);
	return set ^ ret;
}

static int sprd_clk_enable(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);

	if (c->enb.reg)
		__glbreg_setclr(c->enb.reg, c->enb.msk, 1);
	return 0;
}

static void sprd_clk_disable(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);

	if (c->enb.reg)
		__glbreg_setclr(c->enb.reg, c->enb.msk, 0);
}

static int sprd_clk_is_enabled(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);

	if (!c->enb.reg)
		return 1;
	return !!(__raw_readl(c->enb.reg) & c->enb.msk);
}

static unsigned long sprd_clk_fixed_pll_recalc_rate(struct clk_hw *hw,
						    unsigned long parent_rate)
{
	return to_clk_sprd(hw)->fixed_rate;
}

/* SCX30G PLL CFG1 bit definitions */
#define BITS_PLL_KINT_MASK	GENMASK(31, 12)
#define BIT_PLL_DIV_S		BIT(10)
#define BIT_PLL_SDM_EN		BIT(6)
#define BITS_PLL_NINT_MASK	GENMASK(5, 0)
#define SHFT_PLL_KINT		12
#define SHFT_PLL_NINT		0

static unsigned long sprd_clk_adjustable_pll_recalc_rate(struct clk_hw *hw,
							 unsigned long parent_rate)
{
	struct clk_sprd *pll = to_clk_sprd(hw);
	unsigned int k = 0, mn, cfg1;
	unsigned int rate;

	if (!pll->mul.reg)
		return 0;

	cfg1 = __raw_readl(pll->mul.reg);
	mn = (cfg1 & BITS_PLL_NINT_MASK) >> SHFT_PLL_NINT;
	if (!mn)
		return 26000000UL;

	if (cfg1 & BIT_PLL_SDM_EN)
		k = (cfg1 & BITS_PLL_KINT_MASK) >> SHFT_PLL_KINT;

	rate = 26 * mn * 1000000 + DIV_ROUND_CLOSEST(26 * k * 100, 1048576) * 10000;
	return (unsigned long)rate;
}

static long sprd_clk_adjustable_pll_round_rate(struct clk_hw *hw,
					       unsigned long rate,
					       unsigned long *prate)
{
	return rate;
}

static int sprd_clk_adjustable_pll_set_rate(struct clk_hw *hw,
					    unsigned long rate,
					    unsigned long parent_rate)
{
	struct clk_sprd *pll = to_clk_sprd(hw);
	u32 old_rate = sprd_clk_adjustable_pll_recalc_rate(hw, 0) / 1000000;
	u32 k, mn, cfg1, val;
	unsigned long flags;
	int dly;

	if (!pll->mul.reg)
		return -EINVAL;

	mn = (rate / 1000000) / 26;
	k = DIV_ROUND_CLOSEST(((rate / 10000) - 26 * mn * 100) * 1048576, 26 * 100);

	cfg1 = (mn & BITS_PLL_NINT_MASK);
	if (k)
		cfg1 |= ((k << SHFT_PLL_KINT) & BITS_PLL_KINT_MASK) | BIT_PLL_SDM_EN;

	spin_lock_irqsave(&sprd_clk_lock, flags);
	val = __raw_readl(pll->mul.reg);
	val &= ~(BITS_PLL_KINT_MASK | BITS_PLL_NINT_MASK | BIT_PLL_SDM_EN);
	val |= cfg1;
	__raw_writel(val, pll->mul.reg);
	spin_unlock_irqrestore(&sprd_clk_lock, flags);

	dly = abs((int)(rate / 1000000) - (int)old_rate) * 50 / 100;
	if (dly > 0 && dly <= 1000)
		udelay(dly);

	return 0;
}

static u8 sprd_clk_mux_get_parent(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	u32 shift, mask, val;

	if (!c->sel.reg || !c->sel.msk)
		return 0;

	shift = __ffs(c->sel.msk);
	mask = c->sel.msk >> shift;
	val = (__raw_readl(c->sel.reg) >> shift) & mask;
	if (val >= clk_hw_get_num_parents(hw))
		return 0;
	return (u8)val;
}

static int sprd_clk_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	unsigned long flags;
	u32 shift, mask, val;

	if (!c->sel.reg || !c->sel.msk)
		return 0;

	shift = __ffs(c->sel.msk);
	mask = c->sel.msk >> shift;

	spin_lock_irqsave(&sprd_clk_lock, flags);
	val = __raw_readl(c->sel.reg);
	val &= ~(mask << shift);
	val |= ((u32)index & mask) << shift;
	__raw_writel(val, c->sel.reg);
	spin_unlock_irqrestore(&sprd_clk_lock, flags);

	return 0;
}

static unsigned long sprd_clk_divider_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	u32 shift, mask, div;

	if (!c->div.reg || !c->div.msk)
		return parent_rate;

	shift = __ffs(c->div.msk);
	mask = c->div.msk >> shift;
	div = ((__raw_readl(c->div.reg) >> shift) & mask) + 1;
	return parent_rate / div;
}

static long sprd_clk_divider_round_rate(struct clk_hw *hw, unsigned long rate,
					unsigned long *prate)
{
	return rate;
}

static int sprd_clk_divider_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	unsigned long flags;
	u32 shift, mask, div, val;

	if (!c->div.reg || !c->div.msk || !rate)
		return 0;

	shift = __ffs(c->div.msk);
	mask = c->div.msk >> shift;
	div = DIV_ROUND_UP(parent_rate, rate);
	if (!div)
		div = 1;
	if (div - 1 > mask)
		div = mask + 1;

	spin_lock_irqsave(&sprd_clk_lock, flags);
	val = __raw_readl(c->div.reg);
	val &= ~(mask << shift);
	val |= ((div - 1) & mask) << shift;
	__raw_writel(val, c->div.reg);
	spin_unlock_irqrestore(&sprd_clk_lock, flags);

	return 0;
}

/*
 * MM power domain (VSP, JPG, DCAM/ISP, CSI)
 *
 * clk_mm ("sprd,mm-power-domain") is the domain switch: preparing it powers
 * MM_TOP up and opens the MM bus, unpreparing it saves the MM clock
 * selections and shuts the domain down again.  The clocks inside the domain
 * ("sprd,mm-domain") hold a clk_mm reference while they are prepared, so
 * their registers are only touched while the domain is up; while it is down
 * the mux selections live in the saved copy.  As on 3.10 the domain is
 * brought up when clk_mm registers, so that the boot state of the MM clocks
 * can be read, and a late initcall shuts it down unless it is in use.
 */
#define MM_PD_CFG		((void __iomem *)(SPRD_PMU_BASE + 0x1c))
#define MM_PD_FORCE_SHUTDOWN	BIT(25)
#define MM_PD_STATUS		((void __iomem *)(SPRD_PMU_BASE + 0xbc))
#define MM_PD_STATE		GENMASK(31, 28)
#define MM_PD_STATE_SHIFT	28
#define MM_PD_STATE_ON		0
#define MM_PD_STATE_OFF		7
#define MM_PD_POLL		100000
#define MM_EB_REG		((void __iomem *)(SPRD_AONAPB_BASE + 0x0))
#define MM_EB			BIT(25)
#define MM_AHB_EB		((void __iomem *)(SPRD_MMAHB_BASE + 0x0))
#define MM_CKG_EB		BIT(6)
#define MM_GEN_CKG_CFG		((void __iomem *)(SPRD_MMAHB_BASE + 0x8))
#define MM_MTX_AXI_AXI_CKG_EN	(BIT(8) | BIT(7))
/* MM_AHB_CFG and the nine clock selects after it, as saved by 3.10 */
#define MM_CKG_SAVED		((void __iomem *)(SPRD_MMCKG_BASE + 0x20))
#define MM_CKG_NR		10
#define MM_AHB_CFG_153M6	0x3

static struct clk *sprd_mm_clk;
static bool sprd_mm_on;
static bool sprd_mm_saved;
static u32 sprd_mm_ckg[MM_CKG_NR];

static void sprd_mm_rmw(void __iomem *reg, u32 msk, bool set)
{
	unsigned long flags;
	u32 val;

	spin_lock_irqsave(&sprd_clk_lock, flags);
	val = __raw_readl(reg);
	__raw_writel(set ? val | msk : val & ~msk, reg);
	spin_unlock_irqrestore(&sprd_clk_lock, flags);
}

/*
 * Also used from time_init(), before the delay loop is calibrated, so the
 * bound is generous; a timeout is reported and never ends in a bus access.
 */
static bool sprd_mm_pd_wait(u32 state)
{
	int i;

	for (i = 0; i < MM_PD_POLL; i++) {
		u32 s1 = __raw_readl(MM_PD_STATUS) & MM_PD_STATE;
		u32 s2 = __raw_readl(MM_PD_STATUS) & MM_PD_STATE;

		/* as on 3.10, trust the status only once it reads stable */
		if (s1 == s2 && s1 == state << MM_PD_STATE_SHIFT)
			return true;
		udelay(1);
	}
	return false;
}

static void sprd_mm_save(void)
{
	int i;

	for (i = 0; i < MM_CKG_NR; i++)
		sprd_mm_ckg[i] = __raw_readl(MM_CKG_SAVED + 4 * i);
	sprd_mm_saved = true;
}

static int sprd_mm_power_on(void)
{
	int i;

	if (sprd_mm_on)
		return 0;

	__glbreg_setclr(MM_PD_CFG, MM_PD_FORCE_SHUTDOWN, 0);
	if (!sprd_mm_pd_wait(MM_PD_STATE_ON)) {
		pr_err("mm domain: power-up timeout, cfg %08x status %08x\n",
		       __raw_readl(MM_PD_CFG), __raw_readl(MM_PD_STATUS));
		return -ETIMEDOUT;
	}

	__glbreg_setclr(MM_EB_REG, MM_EB, 1);
	__glbreg_setclr(MM_AHB_EB, MM_CKG_EB, 1);
	sprd_mm_rmw(MM_GEN_CKG_CFG, MM_MTX_AXI_AXI_CKG_EN, true);
	sprd_mm_rmw(MM_CKG_SAVED, MM_AHB_CFG_153M6, true);
	if (sprd_mm_saved)
		for (i = 0; i < MM_CKG_NR; i++)
			__raw_writel(sprd_mm_ckg[i], MM_CKG_SAVED + 4 * i);

	WRITE_ONCE(sprd_mm_on, true);
	return 0;
}

static void sprd_mm_power_off(void)
{
	if (!sprd_mm_on)
		return;

	sprd_mm_save();
	WRITE_ONCE(sprd_mm_on, false);
	__glbreg_setclr(MM_EB_REG, MM_EB, 0);
	__glbreg_setclr(MM_PD_CFG, MM_PD_FORCE_SHUTDOWN, 1);
	if (!sprd_mm_pd_wait(MM_PD_STATE_OFF))
		pr_warn("mm domain: shutdown timeout, cfg %08x status %08x\n",
			__raw_readl(MM_PD_CFG), __raw_readl(MM_PD_STATUS));
}

static int sprd_mm_domain_prepare(struct clk_hw *hw)
{
	return sprd_mm_power_on();
}

static void sprd_mm_domain_unprepare(struct clk_hw *hw)
{
	sprd_mm_power_off();
}

static int sprd_mm_domain_is_enabled(struct clk_hw *hw)
{
	return READ_ONCE(sprd_mm_on);
}

static int sprd_mm_clk_prepare(struct clk_hw *hw)
{
	int ret;

	if (!sprd_mm_clk)
		return -ENODEV;

	/* prepare_lock is recursive for its owner */
	ret = clk_prepare(sprd_mm_clk);
	if (ret)
		return ret;
	return sprd_clk_prepare(hw);
}

static void sprd_mm_clk_unprepare(struct clk_hw *hw)
{
	sprd_clk_unprepare(hw);
	clk_unprepare(sprd_mm_clk);
}

/* read-modify-write, as on 3.10: the MM gates never used the aliases */
static int sprd_mm_clk_enable(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);

	if (c->enb.reg && READ_ONCE(sprd_mm_on))
		sprd_mm_rmw(c->enb.reg, c->enb.msk, true);
	return 0;
}

static void sprd_mm_clk_disable(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);

	if (c->enb.reg && READ_ONCE(sprd_mm_on))
		sprd_mm_rmw(c->enb.reg, c->enb.msk, false);
}

static int sprd_mm_clk_is_enabled(struct clk_hw *hw)
{
	return READ_ONCE(sprd_mm_on) ? sprd_clk_is_enabled(hw) : 0;
}

static u32 *sprd_mm_saved_reg(void __iomem *reg)
{
	unsigned long off = (unsigned long)reg - (unsigned long)MM_CKG_SAVED;

	return off < sizeof(sprd_mm_ckg) ? &sprd_mm_ckg[off / 4] : NULL;
}

static u8 sprd_mm_mux_get_parent(struct clk_hw *hw)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	u32 *saved, val;

	if (READ_ONCE(sprd_mm_on))
		return sprd_clk_mux_get_parent(hw);

	saved = sprd_mm_saved_reg(c->sel.reg);
	if (!saved || !c->sel.msk)
		return 0;
	val = (*saved & c->sel.msk) >> __ffs(c->sel.msk);
	return val < clk_hw_get_num_parents(hw) ? val : 0;
}

static int sprd_mm_mux_set_parent(struct clk_hw *hw, u8 index)
{
	struct clk_sprd *c = to_clk_sprd(hw);
	u32 *saved;

	if (READ_ONCE(sprd_mm_on))
		return sprd_clk_mux_set_parent(hw, index);

	/* written back by the restore on the next power-up */
	saved = sprd_mm_saved_reg(c->sel.reg);
	if (saved && c->sel.msk)
		*saved = (*saved & ~c->sel.msk) |
			 (((u32)index << __ffs(c->sel.msk)) & c->sel.msk);
	return 0;
}

static const struct clk_ops sprd_mm_domain_ops = {
	.prepare	= sprd_mm_domain_prepare,
	.unprepare	= sprd_mm_domain_unprepare,
	.is_enabled	= sprd_mm_domain_is_enabled,
};

static const struct clk_ops sprd_mm_gate_ops = {
	.prepare	= sprd_mm_clk_prepare,
	.unprepare	= sprd_mm_clk_unprepare,
	.enable		= sprd_mm_clk_enable,
	.disable	= sprd_mm_clk_disable,
	.is_enabled	= sprd_mm_clk_is_enabled,
};

static const struct clk_ops sprd_mm_mux_ops = {
	.prepare	= sprd_mm_clk_prepare,
	.unprepare	= sprd_mm_clk_unprepare,
	.enable		= sprd_mm_clk_enable,
	.disable	= sprd_mm_clk_disable,
	.is_enabled	= sprd_mm_clk_is_enabled,
	.get_parent	= sprd_mm_mux_get_parent,
	.set_parent	= sprd_mm_mux_set_parent,
};

static const struct clk_ops sprd_mm_clk_composite_ops = {
	.prepare	= sprd_mm_clk_prepare,
	.unprepare	= sprd_mm_clk_unprepare,
	.enable		= sprd_mm_clk_enable,
	.disable	= sprd_mm_clk_disable,
	.is_enabled	= sprd_mm_clk_is_enabled,
	.get_parent	= sprd_mm_mux_get_parent,
	.set_parent	= sprd_mm_mux_set_parent,
	.recalc_rate	= sprd_clk_divider_recalc_rate,
	.round_rate	= sprd_clk_divider_round_rate,
	.set_rate	= sprd_clk_divider_set_rate,
};

static const struct clk_ops sprd_clk_fixed_pll_ops = {
	.prepare	= sprd_clk_prepare,
	.unprepare	= sprd_clk_unprepare,
	.is_prepared	= sprd_clk_is_prepared,
	.recalc_rate	= sprd_clk_fixed_pll_recalc_rate,
};

static const struct clk_ops sprd_clk_adjustable_pll_ops = {
	.prepare	= sprd_clk_prepare,
	.unprepare	= sprd_clk_unprepare,
	.is_prepared	= sprd_clk_is_prepared,
	.round_rate	= sprd_clk_adjustable_pll_round_rate,
	.set_rate	= sprd_clk_adjustable_pll_set_rate,
	.recalc_rate	= sprd_clk_adjustable_pll_recalc_rate,
};

static const struct clk_ops sprd_clk_gate_ops = {
	.prepare	= sprd_clk_prepare,
	.unprepare	= sprd_clk_unprepare,
	.enable		= sprd_clk_enable,
	.disable	= sprd_clk_disable,
	.is_enabled	= sprd_clk_is_enabled,
};

static const struct clk_ops sprd_clk_mux_ops = {
	.enable		= sprd_clk_enable,
	.disable	= sprd_clk_disable,
	.is_enabled	= sprd_clk_is_enabled,
	.get_parent	= sprd_clk_mux_get_parent,
	.set_parent	= sprd_clk_mux_set_parent,
};

static const struct clk_ops sprd_clk_divider_ops = {
	.enable		= sprd_clk_enable,
	.disable	= sprd_clk_disable,
	.is_enabled	= sprd_clk_is_enabled,
	.recalc_rate	= sprd_clk_divider_recalc_rate,
	.round_rate	= sprd_clk_divider_round_rate,
	.set_rate	= sprd_clk_divider_set_rate,
};

static const struct clk_ops sprd_clk_composite_ops = {
	.enable		= sprd_clk_enable,
	.disable	= sprd_clk_disable,
	.is_enabled	= sprd_clk_is_enabled,
	.get_parent	= sprd_clk_mux_get_parent,
	.set_parent	= sprd_clk_mux_set_parent,
	.recalc_rate	= sprd_clk_divider_recalc_rate,
	.round_rate	= sprd_clk_divider_round_rate,
	.set_rate	= sprd_clk_divider_set_rate,
};

static struct clk_onecell_data clk_data;

static void __init file_clk_data(struct clk *clk, const char *clk_name)
{
	static int clk_idx;

	if (clk_data.clks && clk_idx < clk_data.clk_num && !IS_ERR_OR_NULL(clk))
		clk_data.clks[clk_idx++] = clk;
}

static void __init sprd_clk_register(struct device *dev,
				     struct device_node *node,
				     struct clk_sprd *c,
				     struct clk_init_data *init)
{
	struct clk *clk;
	const char *clk_name = init->name;

	c->hw.init = init;
	clk = clk_register(dev, &c->hw);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		file_clk_data(clk, clk_name);
	}
}

static void __init of_sprd_fixed_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	u32 rate;

	if (of_property_read_u32(node, "clock-frequency", &rate))
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);
	clk = clk_register_fixed_rate(NULL, clk_name, NULL, CLK_IGNORE_UNUSED, rate);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		file_clk_data(clk, clk_name);
	}
}

static void __init of_sprd_fixed_factor_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char *parent_name;
	u32 div, mult;

	if (of_property_read_u32(node, "clock-div", &div) ||
	    of_property_read_u32(node, "clock-mult", &mult))
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);
	parent_name = of_clk_get_parent_name(node, 0);

	clk = clk_register_fixed_factor(NULL, clk_name, parent_name, CLK_IGNORE_UNUSED, mult, div);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		file_clk_data(clk, clk_name);
	}
}

static void __init of_sprd_fixed_pll_clk_setup(struct device_node *node)
{
	struct clk_sprd *c;
	const char *clk_name = node->name;
	struct clk_init_data init = {
		.ops = &sprd_clk_fixed_pll_ops,
		.flags = CLK_IGNORE_UNUSED,
		.num_parents = 0,
	};
	u32 rate;
	const __be32 *prereg;

	if (of_property_read_u32(node, "clock-frequency", &rate))
		return;

	prereg = of_get_address(node, 0, NULL, NULL);
	if (!prereg)
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);
	init.name = clk_name;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;

	c->fixed_rate = rate;
	of_read_reg(&c->pre, prereg);
	sprd_clk_register(NULL, node, c, &init);
}

static void __init of_sprd_adjustable_pll_clk_setup(struct device_node *node)
{
	struct clk_sprd *c;
	const char *clk_name = node->name;
	struct clk_init_data init = {
		.ops = &sprd_clk_adjustable_pll_ops,
		.flags = CLK_IGNORE_UNUSED,
		.num_parents = 0,
	};
	const __be32 *mulreg, *prereg;

	mulreg = of_get_address(node, 0, NULL, NULL);
	prereg = of_get_address(node, 1, NULL, NULL);
	if (!mulreg || !prereg)
		return;

	of_property_read_string(node, "clock-output-names", &clk_name);
	init.name = clk_name;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;

	of_read_reg(&c->mul, mulreg);
	of_read_reg(&c->pre, prereg);
	sprd_clk_register(NULL, node, c, &init);
}

static void __init of_sprd_gate_clk_setup(struct device_node *node)
{
	struct clk_sprd *c;
	const char *clk_name = node->name;
	const char *parent_name;
	struct clk_init_data init = {
		.ops = &sprd_clk_gate_ops,
		.flags = CLK_IGNORE_UNUSED,
	};
	const __be32 *enbreg, *prereg;

	enbreg = of_get_address(node, 0, NULL, NULL);
	if (!enbreg)
		return;
	prereg = of_get_address(node, 1, NULL, NULL);

	of_property_read_string(node, "clock-output-names", &clk_name);
	parent_name = of_clk_get_parent_name(node, 0);

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return;

	init.name = clk_name;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	of_read_reg(&c->enb, enbreg);
	of_read_reg(&c->pre, prereg);
	if ((unsigned long)c->pre.reg & 1) {
		c->pre.reg = (void __iomem *)((unsigned long)c->pre.reg & ~3UL);
		c->flags |= CLK_GATE_SET_TO_DISABLE;
	}

	if (of_property_read_bool(node, "sprd,mm-power-domain")) {
		init.ops = &sprd_mm_domain_ops;
		/* the MM muxes registering next read their boot selection */
		if (!sprd_mm_power_on())
			sprd_mm_save();
	} else if (of_property_read_bool(node, "sprd,mm-domain")) {
		init.ops = &sprd_mm_gate_ops;
	}

	sprd_clk_register(NULL, node, c, &init);

	if (init.ops == &sprd_mm_domain_ops && !IS_ERR_OR_NULL(c->hw.clk))
		sprd_mm_clk = c->hw.clk;
}

static struct clk_sprd *__init __of_sprd_composite_clk_setup(struct device_node *node,
							     int has_mux,
							     int has_div)
{
	struct clk_sprd *c;
	const char *clk_name = node->name;
	const char **parent_names = NULL;
	struct clk_init_data init = {
		.flags = CLK_IGNORE_UNUSED,
	};
	const __be32 *selreg = NULL, *divreg = NULL, *enbreg = NULL, *prereg = NULL;
	int idx = 0, i, num_parents = 0;

	if (has_mux)
		selreg = of_get_address(node, idx++, NULL, NULL);
	if (has_div)
		divreg = of_get_address(node, idx++, NULL, NULL);

	enbreg = of_get_address(node, idx++, NULL, NULL);
	if (enbreg)
		prereg = of_get_address(node, idx++, NULL, NULL);

	of_property_read_string(node, "clock-output-names", &clk_name);
	init.name = clk_name;

	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return NULL;

	of_read_reg(&c->enb, enbreg);

	num_parents = of_clk_get_parent_count(node);
	if (num_parents > 0) {
		parent_names = kcalloc(num_parents, sizeof(const char *), GFP_KERNEL);
		if (!parent_names) {
			kfree(c);
			return NULL;
		}
		for (i = 0; i < num_parents; i++)
			parent_names[i] = of_clk_get_parent_name(node, i);
	}
	init.parent_names = parent_names;
	init.num_parents = num_parents;

	if (selreg) {
		of_read_reg(&c->sel, selreg);
		of_read_reg(&c->pre, prereg);
		init.ops = &sprd_clk_mux_ops;
	}
	if (divreg) {
		of_read_reg(&c->div, divreg);
		init.ops = &sprd_clk_divider_ops;
	}
	if (selreg && divreg)
		init.ops = &sprd_clk_composite_ops;
	if (of_property_read_bool(node, "sprd,mm-domain")) {
		if (selreg && !divreg)
			init.ops = &sprd_mm_mux_ops;
		else if (selreg && divreg)
			init.ops = &sprd_mm_clk_composite_ops;
		else
			pr_warn("%s: sprd,mm-domain is only supported on muxes and composite clocks\n",
				clk_name);
	}

	sprd_clk_register(NULL, node, c, &init);
	kfree(parent_names);
	return c;
}

static void __init of_sprd_muxed_clk_setup(struct device_node *node)
{
	__of_sprd_composite_clk_setup(node, 1, 0);
}

static void __init of_sprd_divider_clk_setup(struct device_node *node)
{
	__of_sprd_composite_clk_setup(node, 0, 1);
}

static void __init of_sprd_composite_clk_setup(struct device_node *node)
{
	__of_sprd_composite_clk_setup(node, 1, 1);
}

static void __init sprd_clocks_init(struct device_node *node)
{
	int num = of_get_child_count(node);

	if (num > 0) {
		clk_data.clks = kcalloc(num, sizeof(struct clk *), GFP_KERNEL);
		if (clk_data.clks) {
			clk_data.clk_num = num;
			of_clk_add_provider(node, of_clk_src_onecell_get, &clk_data);
		}
	}
}

CLK_OF_DECLARE(scx15_clock, "sprd,scx15-clocks", sprd_clocks_init);
CLK_OF_DECLARE(scx35_clock, "sprd,scx35-clocks", sprd_clocks_init);
CLK_OF_DECLARE(scx30g_clock, "sprd,scx30g-clocks", sprd_clocks_init);
CLK_OF_DECLARE(scx35l_clock, "sprd,scx35l-clocks", sprd_clocks_init);
CLK_OF_DECLARE(fixed_clock, "sprd,fixed-clock", of_sprd_fixed_clk_setup);
CLK_OF_DECLARE(fixed_factor_clock, "sprd,fixed-factor-clock",
	       of_sprd_fixed_factor_clk_setup);
CLK_OF_DECLARE(fixed_pll_clock, "sprd,fixed-pll-clock",
	       of_sprd_fixed_pll_clk_setup);
CLK_OF_DECLARE(adjustable_pll_clock, "sprd,adjustable-pll-clock",
	       of_sprd_adjustable_pll_clk_setup);
CLK_OF_DECLARE(gate_clock, "sprd,gate-clock", of_sprd_gate_clk_setup);
CLK_OF_DECLARE(muxed_clock, "sprd,muxed-clock", of_sprd_muxed_clk_setup);
CLK_OF_DECLARE(divider_clock, "sprd,divider-clock", of_sprd_divider_clk_setup);
CLK_OF_DECLARE(composite_clock, "sprd,composite-dev-clock",
	       of_sprd_composite_clk_setup);

/* Drop the boot-time MM domain power-up if no driver holds clk_mm. */
static int __init sprd_mm_domain_late_init(void)
{
	if (!sprd_mm_clk || clk_prepare(sprd_mm_clk))
		return 0;
	clk_unprepare(sprd_mm_clk);
	pr_info("mm domain: %s\n", READ_ONCE(sprd_mm_on) ? "in use" : "off");
	return 0;
}
late_initcall(sprd_mm_domain_late_init);
