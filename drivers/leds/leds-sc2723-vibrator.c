// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC2723 PMIC vibrator driver.
 *
 * The SC2723 drives the vibration motor straight from its dedicated
 * "VIBR" LDO, so turning the motor on/off is just powering that LDO up or
 * down.  The motor is exposed as an LED class device called "vibrator" so
 * Android's vibrator HAL can drive it through the "transient" LED trigger
 * (/sys/class/leds/vibrator/{activate,duration,state}), which is the
 * upstream replacement for the old Android timed_output interface.
 *
 * Based on the register sequence of arch/arm/mach-sc/sprd_vibrator_2723.c
 * from the Samsung/Spreadtrum 3.10 kernel.
 */

#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/slab.h>

#include <soc/sprd/iomap.h>

#define ANA_REG_GLB_VIBR_CTRL0		SCI_ADDR(ANA_REGS_GLB_BASE, 0x00f8)
#define BIT_SLP_LDOVIBR_PD_EN		BIT(9)
#define BIT_LDO_VIBR_PD			BIT(8)
#define BITS_LDO_VIBR_V_MASK		0xff

#define BIT_RTC_VIBR_EN			BIT(5)	/* in ANA_REG_GLB_RTC_CLK_EN */

#define SC2723_VIBR_V_DEFAULT		0xd2

struct sc2723_vibrator {
	struct led_classdev cdev;
	struct wakeup_source *ws;
	bool on;
};

static void sc2723_vibrator_power(bool on)
{
	if (on)
		sci_adi_clr(ANA_REG_GLB_VIBR_CTRL0,
			    BIT_LDO_VIBR_PD | BIT_SLP_LDOVIBR_PD_EN);
	else
		sci_adi_set(ANA_REG_GLB_VIBR_CTRL0,
			    BIT_LDO_VIBR_PD | BIT_SLP_LDOVIBR_PD_EN);
}

static int sc2723_vibrator_set(struct led_classdev *cdev,
			       enum led_brightness brightness)
{
	struct sc2723_vibrator *vib =
		container_of(cdev, struct sc2723_vibrator, cdev);
	bool on = brightness != LED_OFF;

	if (on == vib->on)
		return 0;

	vib->on = on;
	if (on) {
		__pm_stay_awake(vib->ws);
		sc2723_vibrator_power(true);
	} else {
		sc2723_vibrator_power(false);
		__pm_relax(vib->ws);
	}

	return 0;
}

static int sc2723_vibrator_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sc2723_vibrator *vib;
	u32 voltage = SC2723_VIBR_V_DEFAULT;
	int ret;

	vib = devm_kzalloc(&pdev->dev, sizeof(*vib), GFP_KERNEL);
	if (!vib)
		return -ENOMEM;

	of_property_read_u32(np, "sprd,vibr-voltage", &voltage);

	/* Clock the vibrator block, program the LDO voltage, start off. */
	sci_adi_set(ANA_REG_GLB_RTC_CLK_EN, BIT_RTC_VIBR_EN);
	sci_adi_write(ANA_REG_GLB_VIBR_CTRL0, voltage & BITS_LDO_VIBR_V_MASK,
		      BITS_LDO_VIBR_V_MASK);
	sc2723_vibrator_power(false);

	vib->ws = wakeup_source_register("vibrator");
	if (!vib->ws)
		return -ENOMEM;

	vib->cdev.name = "vibrator";
	vib->cdev.max_brightness = 1;
	vib->cdev.brightness_set_blocking = sc2723_vibrator_set;
	vib->cdev.default_trigger = "transient";

	ret = led_classdev_register(&pdev->dev, &vib->cdev);
	if (ret) {
		wakeup_source_unregister(vib->ws);
		return ret;
	}

	platform_set_drvdata(pdev, vib);
	dev_info(&pdev->dev, "SC2723 vibrator ready (VIBR LDO code 0x%02x)\n",
		 voltage & BITS_LDO_VIBR_V_MASK);
	return 0;
}

static int sc2723_vibrator_remove(struct platform_device *pdev)
{
	struct sc2723_vibrator *vib = platform_get_drvdata(pdev);

	led_classdev_unregister(&vib->cdev);
	sc2723_vibrator_power(false);
	wakeup_source_unregister(vib->ws);
	return 0;
}

static void sc2723_vibrator_shutdown(struct platform_device *pdev)
{
	sc2723_vibrator_power(false);
}

static const struct of_device_id sc2723_vibrator_of_match[] = {
	{ .compatible = "sprd,sc2723-vibrator" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2723_vibrator_of_match);

static struct platform_driver sc2723_vibrator_driver = {
	.probe = sc2723_vibrator_probe,
	.remove = sc2723_vibrator_remove,
	.shutdown = sc2723_vibrator_shutdown,
	.driver = {
		.name = "sc2723-vibrator",
		.of_match_table = sc2723_vibrator_of_match,
	},
};
module_platform_driver(sc2723_vibrator_driver);

MODULE_DESCRIPTION("Spreadtrum SC2723 PMIC vibrator (LED class)");
MODULE_LICENSE("GPL v2");
