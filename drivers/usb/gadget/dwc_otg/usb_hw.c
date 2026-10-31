/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */
#include <linux/module.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <soc/sprd/iomap.h>
#include "usb_hw.h"

#define USB_LDO_NAME	"vddusb"
#define USB_CLK_NAME	"clk_usb_ref"

static int gpio_vbus = 0xffffffff;
static uint32_t tune_from_uboot = 0x44076e33;
static uint32_t tune_host = 0x44073133;

#define GPIO_INVALID 0xffffffff

extern int ldo_gpio;

static int __init usb_phy_tune_get(char *str)
{
	if (str != NULL)
		sscanf(&str[0], "%x", &tune_from_uboot);
	if (tune_from_uboot == 0)
		tune_from_uboot = 0x44076e33;
	pr_info("Usb_hw.c: [%s]usb phy tune from uboot: 0x%x\n",
		__func__, tune_from_uboot);
	return 1;
}
__setup("usb_phy_tune=", usb_phy_tune_get);

static void usb_ldo_switch(int is_on)
{
	struct regulator *usb_regulator = regulator_get(NULL, USB_LDO_NAME);

	if (!IS_ERR_OR_NULL(usb_regulator)) {
		if (is_on) {
			regulator_set_voltage(usb_regulator, 3300000, 3300000);
			if (regulator_enable(usb_regulator))
				pr_warn("usb_hw: regulator_enable(vddusb) failed\n");
		} else {
			if (regulator_is_enabled(usb_regulator))
				regulator_disable(usb_regulator);
		}
		regulator_put(usb_regulator);
	}
}

static void usb_enable_module(int en)
{
	if (en) {
		sci_glb_clr(REG_AON_APB_PWR_CTRL, BIT(0));
		sci_glb_set(REG_AP_AHB_AHB_EB, BIT_USB_EB);
	} else {
		sci_glb_set(REG_AON_APB_PWR_CTRL, BIT(0));
		sci_glb_clr(REG_AP_AHB_AHB_EB, BIT_USB_EB);
	}
}

void usb_phy_init(struct platform_device *_dev)
{
	struct device_node *np = _dev->dev.of_node;

	if (np) {
		if (of_property_read_u32(np, "tune_value_host", &tune_host))
			pr_info("usb_hw: using default tune_value_host 0x%x\n",
				tune_host);
		if (of_property_read_u32(np, "tune_value", &tune_from_uboot))
			pr_info("usb_hw: using default tune_value 0x%x\n",
				tune_from_uboot);
	}
	pr_info("Usb_hw.c: [%s] tune_dev=0x%x tune_host=0x%x\n",
		__func__, tune_from_uboot, tune_host);

	sci_glb_set(REG_AP_APB_USB_PHY_CTRL, BIT(9));
	__raw_writel(tune_from_uboot, (void __iomem *)REG_AP_APB_USB_PHY_TUNE);
}

void usb_phy_tune_host(void)
{
	u32 phy_tune = 0;

	__raw_writel(tune_host, (void __iomem *)REG_AP_APB_USB_PHY_TUNE);
	phy_tune = __raw_readl((void __iomem *)REG_AP_APB_USB_PHY_TUNE);
	pr_info("Usb_hw.c: [%s] set tune_host=0x%x\n", __func__, phy_tune);
}

void usb_phy_tune_dev(void)
{
	u32 phy_tune = 0;

	__raw_writel(tune_from_uboot, (void __iomem *)REG_AP_APB_USB_PHY_TUNE);
	phy_tune = __raw_readl((void __iomem *)REG_AP_APB_USB_PHY_TUNE);
	pr_info("Usb_hw.c: [%s] set tune_from_uboot=0x%x\n", __func__, phy_tune);
}

void usb_phy_ahb_rst(void)
{
	sci_glb_set(REG_AP_AHB_AHB_RST, BIT(7));
	mdelay(3);
	sci_glb_clr(REG_AP_AHB_AHB_RST, BIT(7));
	mdelay(3);
}

static void usb_startup(void)
{
	usb_ldo_switch(1);
	mdelay(10);
	usb_enable_module(1);
	mdelay(2);
	sci_glb_set(REG_AP_AHB_AHB_RST, BIT(5) | BIT(6) | BIT(7));
	mdelay(5);
	sci_glb_clr(REG_AP_AHB_AHB_RST, BIT(5) | BIT(6) | BIT(7));
	mdelay(3);
}

void udc_enable(void)
{
	pr_info("%s\n", __func__);
	usb_startup();
}

void udc_disable(void)
{
	pr_info("%s\n", __func__);
	usb_enable_module(0);
	usb_ldo_switch(0);
}

int usb_alloc_vbus_irq(int gpio)
{
	int irq = -1;

	if (gpio_is_valid(gpio)) {
		gpio_request(gpio, "sprd_otg");
		gpio_direction_input(gpio);
		irq = gpio_to_irq(gpio);
		gpio_vbus = gpio;
	}
	return irq;
}

void usb_free_vbus_irq(int irq, int gpio)
{
	if (gpio_is_valid(gpio))
		gpio_free(gpio);
}

int usb_get_vbus_irq(void)
{
	if (gpio_is_valid(gpio_vbus))
		return gpio_to_irq(gpio_vbus);
	return -1;
}

int usb_get_vbus_state(void)
{
	if (gpio_is_valid(gpio_vbus))
		return !!gpio_get_value(gpio_vbus);
	return 1;
}

void usb_set_vbus_irq_type(int irq, int irq_type)
{
	if (irq < 0)
		return;
	if (irq_type == VBUS_PLUG_IN)
		irq_set_irq_type(irq, IRQ_TYPE_LEVEL_HIGH);
	else if (irq_type == VBUS_PLUG_OUT)
		irq_set_irq_type(irq, IRQ_TYPE_LEVEL_LOW);
}
