/*
 * Broadcom BCM47520 GPS Power Control Driver
 *
 * Copyright (C) 2013 Samsung Electronics Co., Ltd.
 *
 * Licensed under the GPL-2.
 */

#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/types.h>
#include <linux/clk.h>
#include <linux/proc_fs.h>
#include <linux/irq.h>
#include <linux/module.h>

static struct device *gps_dev;
extern struct class *sec_class;

static int gps_clk_init(void)
{
	struct clk *gps_clk;
	struct clk *clk_parent;
	const char *clk_32k = "clk_aux0";

	gps_clk = clk_get(NULL, clk_32k);
	if (IS_ERR(gps_clk)) {
		pr_warn("%s: failed to get clk_aux0.\n", __func__);
		return -1;
	}
	clk_parent = clk_get(NULL, "ext_32k");
	if (!IS_ERR(clk_parent)) {
		clk_set_parent(gps_clk, clk_parent);
		clk_put(clk_parent);
	}
	clk_set_rate(gps_clk, 32000);
	if (!clk_prepare_enable(gps_clk))
		pr_info("%s: %s enabled!!\n", __func__, clk_32k);

	return 0;
}

static int gps_regulator_on(struct device_node *root_node)
{
	static struct regulator *gps_regulator = NULL;
	static struct regulator *tcxo_regulator = NULL;
	static unsigned int gps_regulator_volt = 1800000;
	static unsigned int tcxo_regulator_volt = 0;
	const char *reg = NULL;
	const char *reg_tcxo = NULL;

	/* regulator enable from dts */
	if (of_property_read_string(root_node, "gps-regulator", &reg)) {
		pr_err("%s: Failed to get main-regulator from dts.\n", __func__);
		return -1;
	}
	if (of_property_read_string(root_node, "tcxo-regulator", &reg_tcxo))
		pr_info("%s: tcxo-regulator field is empty from dts.\n", __func__);

	if (gps_regulator == NULL) {
		gps_regulator = regulator_get(NULL, reg);
		if (IS_ERR(gps_regulator)) {
			gps_regulator = NULL;
			return -1;
		}
	}
	if (tcxo_regulator == NULL && reg_tcxo) {
		tcxo_regulator = regulator_get(NULL, reg_tcxo);
		if (IS_ERR(tcxo_regulator)) {
			tcxo_regulator = NULL;
		}
	}
	regulator_set_voltage(gps_regulator, gps_regulator_volt, gps_regulator_volt);
	if (!regulator_enable(gps_regulator))
		pr_info("%s: (%s) main(%d uV) regulator turned ON!!\n", __func__, reg, gps_regulator_volt);
	if (tcxo_regulator && !of_property_read_u32(root_node, "tcxo-regulator-volt", &tcxo_regulator_volt)) {
		regulator_set_voltage(tcxo_regulator, tcxo_regulator_volt, tcxo_regulator_volt);
		if (!regulator_enable(tcxo_regulator))
			pr_info("%s: (%s) tcxo(%d uV) regulator turned ON!!\n", __func__, reg_tcxo, tcxo_regulator_volt);
	}

	return 0;
}

static unsigned int gps_pwr_on = 0;

static int __init gps_bcm47520_init(void)
{
	const char *gps_node = "broadcom,bcm47520";
	const char *gps_pwr_en = "gps-pwr-en";
	struct device_node *root_node = NULL;
	int ret = 0;

	pr_info("%s\n", __func__);

	if (!sec_class) {
		sec_class = class_create(THIS_MODULE, "sec");
		if (IS_ERR(sec_class)) {
			pr_err("%s: failed to create sec_class\n", __func__);
			return PTR_ERR(sec_class);
		}
	}

	gps_dev = device_create(sec_class, NULL, 0, NULL, "gps");
	if (IS_ERR(gps_dev)) {
		pr_err("%s: failed to create gps device\n", __func__);
		return PTR_ERR(gps_dev);
	}

	root_node = of_find_compatible_node(NULL, NULL, gps_node);
	if (!root_node) {
		pr_err("failed to get device node of %s\n", gps_node);
		ret = -ENODEV;
		goto err_sec_device_create;
	}
	if (gps_regulator_on(root_node)) {
		pr_err("gps_regulator_on() failed\n");
		ret = -ENODEV;
		goto err_find_node;
	}
	if (gps_clk_init()) {
		pr_err("gps_clk_init() failed\n");
		ret = -ENODEV;
		goto err_find_node;
	}
	gps_pwr_on = of_get_named_gpio(root_node, gps_pwr_en, 0);
	if (!gpio_is_valid(gps_pwr_on)) {
		pr_err("%s: Invalid gpio pin : %d\n", __func__, gps_pwr_on);
		ret = -ENODEV;
		goto err_find_node;
	}
	if (gpio_request(gps_pwr_on, "GPS_PWR_EN")) {
		pr_err("fail to request gpio(GPS_PWR_EN)\n");
		ret = -ENODEV;
		goto err_find_node;
	}
	gpio_direction_output(gps_pwr_on, 0);
	gpio_export(gps_pwr_on, true);
	gpio_export_link(gps_dev, "GPS_PWR_EN", gps_pwr_on);

	pr_info("%s: BCM47520 GPS power driver initialized successfully\n", __func__);
	return 0;

err_find_node:
	of_node_put(root_node);
err_sec_device_create:
	device_destroy(sec_class, gps_dev->devt);
	return ret;
}

late_initcall(gps_bcm47520_init);

MODULE_DESCRIPTION("Broadcom BCM47520 GPS Power Control Driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
