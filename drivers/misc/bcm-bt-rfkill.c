/*
 * Broadcom Bluetooth rfkill power control via GPIO (BCM43430 on SM-T560/T561).
 *
 * Copyright (C) 2013 Broadcom Corporation
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/broadcom/bcm-bt-rfkill.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include <linux/clk.h>

static void bt_clk_init(void)
{
	struct clk *bt_clk;
	struct clk *clk_parent;

	bt_clk = clk_get(NULL, "clk_aux0");
	if (IS_ERR(bt_clk)) {
		pr_warn("bcm-bt-rfkill: failed to get clk_aux0\n");
		return;
	}

	clk_parent = clk_get(NULL, "ext_32k");
	if (!IS_ERR(clk_parent)) {
		clk_set_parent(bt_clk, clk_parent);
		clk_put(clk_parent);
	}

	clk_set_rate(bt_clk, 32768);
	clk_prepare_enable(bt_clk);
	clk_put(bt_clk);
}

static int bcm_bt_rfkill_set_power(void *data, bool blocked)
{
	struct bcm_bt_rfkill_platform_data *pdata = data;
	int vreg_gpio = pdata->bcm_bt_rfkill_vreg_gpio;

	if (!blocked) {
		if (gpio_is_valid(vreg_gpio))
			gpio_set_value(vreg_gpio, BCM_BT_RFKILL_VREG_ON);
		pr_info("bcm_bt_rfkill_setpower: unblocked vreg:%s\n",
			gpio_get_value(vreg_gpio) ? "High [POWER ON]" : "Low [POWER_OFF]");
		bt_clk_init();
	} else {
		if (gpio_is_valid(vreg_gpio))
			gpio_set_value(vreg_gpio, BCM_BT_RFKILL_VREG_OFF);
		pr_info("bcm_bt_rfkill_setpower: blocked vreg:%s\n",
			gpio_get_value(vreg_gpio) ? "High [POWER ON]" : "Low [POWER_OFF]");
	}
	return 0;
}

static const struct rfkill_ops bcm_bt_rfkill_ops = {
	.set_block = bcm_bt_rfkill_set_power,
};

static const struct of_device_id bcm_bt_rfkill_of_match[] = {
	{ .compatible = "bcm,bcm-bt-rfkill", },
	{},
};
MODULE_DEVICE_TABLE(of, bcm_bt_rfkill_of_match);

static int bcm_bt_rfkill_probe(struct platform_device *pdev)
{
	int rc = -EINVAL;
	struct bcm_bt_rfkill_platform_data *pdata;

	pdata = devm_kzalloc(&pdev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	if (pdev->dev.of_node) {
		pdata->bcm_bt_rfkill_vreg_gpio = of_get_gpio(pdev->dev.of_node, 0);
		if (!gpio_is_valid(pdata->bcm_bt_rfkill_vreg_gpio)) {
			pr_err("%s: ERROR Invalid bcm-bt-rfkill-vreg-gpio\n",
			       __func__);
			return pdata->bcm_bt_rfkill_vreg_gpio;
		}
		pdev->dev.platform_data = pdata;
	} else if (pdev->dev.platform_data) {
		struct bcm_bt_rfkill_platform_data *pdata1 = pdev->dev.platform_data;
		pdata->bcm_bt_rfkill_vreg_gpio = pdata1->bcm_bt_rfkill_vreg_gpio;
	} else {
		return -ENODEV;
	}

	rc = devm_gpio_request_one(&pdev->dev,
				   pdata->bcm_bt_rfkill_vreg_gpio,
				   GPIOF_OUT_INIT_LOW,
				   "bcm_bt_rfkill_vreg_gpio");
	if (rc) {
		pr_err("%s: ERROR bcm_bt_rfkill_vreg_gpio request failed\n",
		       __func__);
		goto out;
	}

	gpio_export(pdata->bcm_bt_rfkill_vreg_gpio, false);
	gpio_direction_output(pdata->bcm_bt_rfkill_vreg_gpio,
			      BCM_BT_RFKILL_VREG_OFF);

	pdata->rfkill = rfkill_alloc("bcm-bt-rfkill", &pdev->dev,
				     RFKILL_TYPE_BLUETOOTH,
				     &bcm_bt_rfkill_ops, pdata);
	if (!pdata->rfkill) {
		rc = -ENOMEM;
		goto out;
	}

	rfkill_init_sw_state(pdata->rfkill, false);

	rc = rfkill_register(pdata->rfkill);
	if (rc) {
		rfkill_destroy(pdata->rfkill);
		goto out;
	}

	pr_info("bcm_bt_rfkill_probe: registered vreg_gpio=%d\n",
		pdata->bcm_bt_rfkill_vreg_gpio);
	return 0;

out:
	if (pdev->dev.of_node)
		pdev->dev.platform_data = NULL;
	return rc;
}

static int bcm_bt_rfkill_remove(struct platform_device *pdev)
{
	struct bcm_bt_rfkill_platform_data *pdata = pdev->dev.platform_data;

	if (pdata) {
		rfkill_unregister(pdata->rfkill);
		rfkill_destroy(pdata->rfkill);
		if (pdev->dev.of_node)
			pdev->dev.platform_data = NULL;
	}
	return 0;
}

static struct platform_driver bcm_bt_rfkill_platform_driver = {
	.probe = bcm_bt_rfkill_probe,
	.remove = bcm_bt_rfkill_remove,
	.driver = {
		.name = "bcm-bt-rfkill",
		.owner = THIS_MODULE,
		.of_match_table = bcm_bt_rfkill_of_match,
	},
};

static int __init bcm_bt_rfkill_init(void)
{
	return platform_driver_register(&bcm_bt_rfkill_platform_driver);
}

static void __exit bcm_bt_rfkill_exit(void)
{
	platform_driver_unregister(&bcm_bt_rfkill_platform_driver);
}

late_initcall(bcm_bt_rfkill_init);
module_exit(bcm_bt_rfkill_exit);

MODULE_DESCRIPTION("bcm-bt-rfkill");
MODULE_AUTHOR("Broadcom");
MODULE_LICENSE("GPL");
