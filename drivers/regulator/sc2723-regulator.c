/*
 * Spreadtrum SC2723 (A-Die PMIC) Regulator Driver for Linux 4.9
 *
 * Adapted from drivers/regulator/sc2723-regulator_dt.c in the 3.10 kernel for
 * Samsung Galaxy Tab E 9.6 (SM-T560 / SM-T561).
 *
 * Controls SC2723 DCDCs and LDOs (vddcore, vddarm, vddmem, vddemmccore,
 * vddsdcore, vddsdio, vdd18, vdd28, vddusb, vddwifipa, etc.) via ADI bus.
 *
 * Licensed under the GPL-2.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/regulator/of_regulator.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include <soc/sprd/iomap.h>

#define REGS_ANA_GLB_PHYS		(SPRD_ADI_PHYS + 0x8800)
#define REGS_ANA_GLB_BASE		(SPRD_ADI_BASE + 0x8800)
#define REGS_ANA_GLB_SIZE		SZ_2K

#define ANA_REG_GLB_MIXED_CTRL0		(REGS_ANA_GLB_BASE + 0x0118)
#define BIT_DCDC_V_CTRL_MODE		BIT(14)

#define ANA_REG_OR(_r, _b)		sci_adi_write(_r, _b, 0)
#define ANA_REG_BIC(_r, _b)		sci_adi_write(_r, 0, _b)
#define ANA_REG_GET(_r)			sci_adi_read(_r)
#define ANA_REG_SET(_r, _v, _m)		sci_adi_write((_r), ((_v) & (_m)), (_m))

struct sci_regulator_regs {
	int typ;
	unsigned long pd_set;
	u32 pd_set_bit;
	unsigned long pwr_sel;
	u32 pwr_sel_bit;
	unsigned long vol_trm;
	u32 vol_trm_bits;
	unsigned long vol_ctl;
	u32 vol_ctl_bits;
	u32 min_uV, step_uV, vol_def;
	u32 vol_sel_cnt;
	const u32 *vol_sel;
};

struct sci_regulator_desc {
	struct regulator_desc desc;
	struct regulator_init_data *init_data;
	struct sci_regulator_regs regs;
	struct regulator_dev *rdev;
};

static const u32 dcdc_vol_select[] = {
	1100000, 700000, 800000, 900000, 1000000, 650000, 1200000, 1300000
};

static unsigned long phy2vir(u32 reg_phy)
{
	if (!reg_phy)
		return 0;
	if (reg_phy >= REGS_ANA_GLB_PHYS && reg_phy < REGS_ANA_GLB_PHYS + REGS_ANA_GLB_SIZE)
		return (reg_phy - REGS_ANA_GLB_PHYS) + REGS_ANA_GLB_BASE;
	return 0;
}

static struct sci_regulator_desc *__get_desc(struct regulator_dev *rdev)
{
	return container_of(rdev->desc, struct sci_regulator_desc, desc);
}

static int ldo_turn_on(struct regulator_dev *rdev)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	if (regs->pd_set == ANA_REG_GLB_LDO_DCDC_PD)
		sci_adi_raw_write(ANA_REG_GLB_PWR_WR_PROT_VALUE, 0x6e7f);

	if (regs->pd_set)
		ANA_REG_BIC(regs->pd_set, regs->pd_set_bit);

	if (regs->pd_set == ANA_REG_GLB_LDO_DCDC_PD)
		sci_adi_raw_write(ANA_REG_GLB_PWR_WR_PROT_VALUE, 0);

	return 0;
}

static int ldo_turn_off(struct regulator_dev *rdev)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	/* Never power down default-on system rails */
	if (regs->typ & BIT(4))
		return 0;

	if (regs->pd_set == ANA_REG_GLB_LDO_DCDC_PD)
		sci_adi_raw_write(ANA_REG_GLB_PWR_WR_PROT_VALUE, 0x6e7f);

	if (regs->pd_set)
		ANA_REG_OR(regs->pd_set, regs->pd_set_bit);

	if (regs->pd_set == ANA_REG_GLB_LDO_DCDC_PD)
		sci_adi_raw_write(ANA_REG_GLB_PWR_WR_PROT_VALUE, 0);

	return 0;
}

static int ldo_is_on(struct regulator_dev *rdev)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	if (regs->pd_set)
		return !(ANA_REG_GET(regs->pd_set) & regs->pd_set_bit);
	return 1;
}

static int ldo_set_voltage(struct regulator_dev *rdev, int min_uV,
			   int max_uV, unsigned int *selector)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	if (!regs->vol_trm || !regs->step_uV || min_uV < (int)regs->min_uV)
		return 0;

	{
		int shft = __ffs(regs->vol_trm_bits);
		u32 max_trim = regs->vol_trm_bits >> shft;
		u32 trim = DIV_ROUND_UP((u32)(min_uV - regs->min_uV), regs->step_uV);

		if (trim > max_trim)
			trim = max_trim;

		if (selector)
			*selector = trim;

		ANA_REG_SET(regs->vol_trm, trim << shft, regs->vol_trm_bits);
	}
	return 0;
}

static int ldo_get_voltage(struct regulator_dev *rdev)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	if (regs->vol_trm && regs->vol_trm_bits) {
		int shft = __ffs(regs->vol_trm_bits);
		u32 trim = (ANA_REG_GET(regs->vol_trm) & regs->vol_trm_bits) >> shft;

		return regs->min_uV + trim * regs->step_uV;
	}
	return regs->vol_def ? (int)regs->vol_def : (int)regs->min_uV;
}

static int dcdc_get_voltage(struct regulator_dev *rdev)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;
	u32 vol = regs->min_uV;

	if (regs->vol_ctl && regs->vol_ctl_bits && regs->vol_sel) {
		int shft_ctl = __ffs(regs->vol_ctl_bits);
		int shft_trm = __ffs(regs->vol_trm_bits);
		u32 idx = (ANA_REG_GET(regs->vol_ctl) & regs->vol_ctl_bits) >> shft_ctl;

		if (idx < regs->vol_sel_cnt) {
			vol = regs->vol_sel[idx];
			if (regs->vol_trm) {
				u32 trim = (ANA_REG_GET(regs->vol_trm) & regs->vol_trm_bits) >> shft_trm;
				vol += trim * regs->step_uV;
			}
		}
		return (int)vol;
	}

	return ldo_get_voltage(rdev);
}

static int dcdc_set_voltage(struct regulator_dev *rdev, int min_uV,
			    int max_uV, unsigned int *selector)
{
	struct sci_regulator_desc *desc = __get_desc(rdev);
	struct sci_regulator_regs *regs = &desc->regs;

	if (regs->vol_ctl && regs->vol_ctl_bits && regs->vol_sel) {
		int i, best_i = -1;
		int best_diff = INT_MAX;

		for (i = 0; i < (int)regs->vol_sel_cnt; i++) {
			int diff = min_uV - (int)regs->vol_sel[i];
			if (diff >= 0 && diff < best_diff) {
				best_diff = diff;
				best_i = i;
			}
		}
		if (best_i >= 0) {
			int shft_ctl = __ffs(regs->vol_ctl_bits);
			int shft_trm = __ffs(regs->vol_trm_bits);
			u32 trim = regs->step_uV ? DIV_ROUND_UP(best_diff, regs->step_uV) : 0;

			if (regs->vol_trm)
				ANA_REG_SET(regs->vol_trm, trim << shft_trm, regs->vol_trm_bits);
			ANA_REG_SET(regs->vol_ctl, (u32)best_i << shft_ctl, regs->vol_ctl_bits);
			return 0;
		}
	}

	return ldo_set_voltage(rdev, min_uV, max_uV, selector);
}

static struct regulator_ops sc2723_ldo_ops = {
	.enable		= ldo_turn_on,
	.disable	= ldo_turn_off,
	.is_enabled	= ldo_is_on,
	.set_voltage	= ldo_set_voltage,
	.get_voltage	= ldo_get_voltage,
};

static struct regulator_ops sc2723_dcdc_ops = {
	.enable		= ldo_turn_on,
	.disable	= ldo_turn_off,
	.is_enabled	= ldo_is_on,
	.set_voltage	= dcdc_set_voltage,
	.get_voltage	= dcdc_get_voltage,
};

static void of_regu_read_reg(struct device_node *np, int idx,
			     struct sci_regulator_regs *regs)
{
	const __be32 *cell;
	u32 reg_phy, msk;
	int count = 0;

	cell = of_get_property(np, "reg", &count);
	if (!cell || (idx * 2 + 1) >= (count / 4))
		return;

	reg_phy = be32_to_cpu(cell[idx * 2]);
	msk = be32_to_cpu(cell[idx * 2 + 1]);

	switch (idx) {
	case 0:
		regs->pd_set = phy2vir(reg_phy);
		regs->pd_set_bit = msk;
		break;
	case 1:
		regs->vol_trm = phy2vir(reg_phy);
		regs->vol_trm_bits = msk;
		break;
	case 2:
		regs->pwr_sel = phy2vir(reg_phy);
		regs->pwr_sel_bit = msk;
		break;
	}
}

static int sci_regulator_probe(struct platform_device *pdev)
{
	struct device_node *dev_np = pdev->dev.of_node;
	struct device_node *child_np;
	struct sci_regulator_desc *sci_desc_list, *sci_desc;
	u16 ana_mixed_ctl;
	int regu_cnt, id = 1;

	if (!dev_np)
		return -ENODEV;

	regu_cnt = of_get_child_count(dev_np);
	if (regu_cnt <= 1)
		return -EINVAL;

	sci_desc_list = devm_kcalloc(&pdev->dev, regu_cnt,
				     sizeof(*sci_desc_list), GFP_KERNEL);
	if (!sci_desc_list)
		return -ENOMEM;

	ana_mixed_ctl = ANA_REG_GET(ANA_REG_GLB_MIXED_CTRL0);
	sci_desc = sci_desc_list;

	for_each_child_of_node(dev_np, child_np) {
		struct regulator_config config = { };
		struct sci_regulator_regs *regs = &sci_desc->regs;
		struct regulator_dev *rdev;

		if (!strcmp(child_np->name, "dummy"))
			continue;

		sci_desc->desc.name = child_np->name;
		sci_desc->desc.id = id++;
		sci_desc->desc.type = REGULATOR_VOLTAGE;
		sci_desc->desc.owner = THIS_MODULE;

		sci_desc->init_data = of_get_regulator_init_data(&pdev->dev,
								 child_np,
								 &sci_desc->desc);
		if (!sci_desc->init_data)
			continue;

		sci_desc->init_data->constraints.valid_modes_mask |=
			REGULATOR_MODE_NORMAL | REGULATOR_MODE_STANDBY;
		sci_desc->init_data->constraints.valid_ops_mask |=
			REGULATOR_CHANGE_MODE | REGULATOR_CHANGE_STATUS |
			REGULATOR_CHANGE_VOLTAGE;

		regs->typ = of_property_read_bool(child_np, "dcdc") ? 2 : 0;
		if (of_property_read_bool(child_np, "default-on")) {
			regs->typ |= BIT(4);
			sci_desc->init_data->constraints.boot_on = 1;
		}

		of_regu_read_reg(child_np, 0, regs);
		of_regu_read_reg(child_np, 1, regs);
		of_regu_read_reg(child_np, 2, regs);

		regs->min_uV = sci_desc->init_data->constraints.min_uV;
		of_property_read_u32(child_np, "regulator-step-microvolt", &regs->step_uV);
		of_property_read_u32(child_np, "regulator-default-microvolt", &regs->vol_def);

		if (!(ana_mixed_ctl & BIT_DCDC_V_CTRL_MODE) &&
		    (!strcmp(sci_desc->desc.name, "vddcore") ||
		     !strcmp(sci_desc->desc.name, "vddarm"))) {
			regs->vol_ctl = regs->vol_trm;
			regs->vol_trm_bits = 0x1f;
			regs->vol_ctl_bits = 0xe0;
			regs->vol_sel_cnt = ARRAY_SIZE(dcdc_vol_select);
			regs->vol_sel = dcdc_vol_select;
		}

		sci_desc->desc.ops = (regs->typ & 2) ? &sc2723_dcdc_ops : &sc2723_ldo_ops;

		config.dev = &pdev->dev;
		config.init_data = sci_desc->init_data;
		config.driver_data = sci_desc;
		config.of_node = child_np;

		rdev = devm_regulator_register(&pdev->dev, &sci_desc->desc, &config);
		if (IS_ERR(rdev)) {
			dev_warn(&pdev->dev, "failed to register %s: %ld\n",
				 sci_desc->desc.name, PTR_ERR(rdev));
			continue;
		}

		sci_desc->rdev = rdev;
		sci_desc++;
	}

	dev_info(&pdev->dev, "SC2723 PMIC regulators initialized (%d rails)\n", id - 1);
	return 0;
}

static const struct of_device_id sprd_regulator_of_match[] = {
	{ .compatible = "sprd,sc2723-regulator", },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_regulator_of_match);

static struct platform_driver sci_regulator_driver = {
	.driver = {
		.name		= "sc2723-regulator",
		.of_match_table	= sprd_regulator_of_match,
	},
	.probe = sci_regulator_probe,
};

static int __init sci_regulator_init(void)
{
	return platform_driver_register(&sci_regulator_driver);
}
subsys_initcall(sci_regulator_init);
