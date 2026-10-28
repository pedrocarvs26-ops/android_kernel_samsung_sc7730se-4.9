/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) Keypad & EIC Keys Driver
 *
 * Adapted from drivers/input/keyboard/sc_keypad.c in the 3.10 kernel for
 * Samsung Galaxy Tab E 9.6 (SM-T560 / SM-T561) on Linux 4.9.337.
 *
 * Handles:
 * - Matrix keypad controller at 0x40250000 (Volume Up, Home)
 * - A-Die EIC Power Key (GPIO 321) and Volume Down Key (GPIO 330)
 * - Hall sensor flip cover switch (GPIO 167)
 * - Samsung /sys/class/sec/sec_key and /sys/class/sec/sec_power_key nodes
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/pm.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/input.h>
#include <linux/input/matrix_keypad.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>

#include <soc/sprd/iomap.h>

#define KPD_REG_BASE			SPRD_KPD_BASE

#define KPD_CTRL			(KPD_REG_BASE + 0x00)
#define KPD_EN				BIT(0)
#define KPD_SLEEP_EN			BIT(1)
#define KPD_LONG_KEY_EN			BIT(2)
#define KPDCTL_ROW_MSK_V1		(0xff << 16)
#define KPDCTL_COL_MSK_V1		(0xff << 8)

#define KPD_INT_EN			(KPD_REG_BASE + 0x04)
#define KPD_INT_RAW_STATUS		(KPD_REG_BASE + 0x08)
#define KPD_INT_MASK_STATUS		(KPD_REG_BASE + 0x0C)
#define KPD_INT_ALL			0xfff
#define KPD_INT_DOWNUP			0x0ff
#define KPD_INT_LONG			0xf00
#define KPD_PRESS_INT0			BIT(0)
#define KPD_PRESS_INT1			BIT(1)
#define KPD_PRESS_INT2			BIT(2)
#define KPD_PRESS_INT3			BIT(3)
#define KPD_RELEASE_INT0		BIT(4)
#define KPD_RELEASE_INT1		BIT(5)
#define KPD_RELEASE_INT2		BIT(6)
#define KPD_RELEASE_INT3		BIT(7)

#define KPD_INT_CLR			(KPD_REG_BASE + 0x10)
#define KPD_POLARITY			(KPD_REG_BASE + 0x18)
#define CFG_ROW_POLARITY		0x00ff
#define CFG_COL_POLARITY		0xff00

#define KPD_DEBOUNCE_CNT		(KPD_REG_BASE + 0x1C)
#define KPD_LONG_KEY_CNT		(KPD_REG_BASE + 0x20)
#define KPD_SLEEP_CNT			(KPD_REG_BASE + 0x24)
#define KPD_SLEEP_CNT_VALUE(_X_MS_)	((_X_MS_ * 32768) / 1000 - 1)
#define KPD_CLK_DIV_CNT			(KPD_REG_BASE + 0x28)
#define KPD_KEY_STATUS			(KPD_REG_BASE + 0x2C)

#define KPD_INT0_COL(_X_)		(((_X_) >> 0) & 0x7)
#define KPD_INT0_ROW(_X_)		(((_X_) >> 4) & 0x7)
#define KPD_INT1_COL(_X_)		(((_X_) >> 8) & 0x7)
#define KPD_INT1_ROW(_X_)		(((_X_) >> 12) & 0x7)
#define KPD_INT2_COL(_X_)		(((_X_) >> 16) & 0x7)
#define KPD_INT2_ROW(_X_)		(((_X_) >> 20) & 0x7)
#define KPD_INT3_COL(_X_)		(((_X_) >> 24) & 0x7)
#define KPD_INT3_ROW(_X_)		(((_X_) >> 28) & 0x7)

extern struct class *sec_class;

struct sci_keypad_platform_data {
	int rows_number;
	int cols_number;
	u32 rows_choose_hw;
	u32 cols_choose_hw;
	u32 debounce_time;
	const struct matrix_keymap_data *keymap_data;
	bool repeat;
	bool support_long_key;
	bool wakeup;
};

struct sci_keypad_t {
	struct input_dev *input_dev;
	int irq;
	int rows;
	int cols;
	int pb_gpio;
	int voldown_gpio;
	int flip_gpio;
};

static struct sci_keypad_t *g_sci_kpd;

static void __keypad_enable(void)
{
	sci_glb_set(REG_AON_APB_APB_RST0, BIT_KPD_SOFT_RST);
	mdelay(2);
	sci_glb_clr(REG_AON_APB_APB_RST0, BIT_KPD_SOFT_RST);
	sci_glb_set(REG_AON_APB_APB_EB0, BIT_KPD_EB);
	sci_glb_set(REG_AON_APB_APB_RTC_EB, BIT_KPD_RTC_EB);
}

static ssize_t key_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u32 keys_pressed = __raw_readl((void __iomem *)KPD_KEY_STATUS);
	int voldown = 1;

	if (g_sci_kpd && gpio_is_valid(g_sci_kpd->voldown_gpio))
		voldown = !gpio_get_value(g_sci_kpd->voldown_gpio);

	if ((keys_pressed & 0x80) || !voldown)
		return sprintf(buf, "PRESS\n");
	return sprintf(buf, "RELEASE\n");
}

static ssize_t powerkey_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int val = 1;

	if (g_sci_kpd && gpio_is_valid(g_sci_kpd->pb_gpio))
		val = !gpio_get_value(g_sci_kpd->pb_gpio);

	return sprintf(buf, "%s\n", !val ? "PRESS" : "RELEASE");
}

static DEVICE_ATTR(sec_key_pressed, 0444, key_show, NULL);
static DEVICE_ATTR(sec_power_key_pressed, 0444, powerkey_show, NULL);

static irqreturn_t sci_keypad_isr(int irq, void *dev_id)
{
	struct sci_keypad_t *sci_kpd = dev_id;
	unsigned long int_status = __raw_readl((void __iomem *)KPD_INT_MASK_STATUS);
	unsigned long key_status = __raw_readl((void __iomem *)KPD_KEY_STATUS);
	unsigned short *keycodes = sci_kpd->input_dev->keycode;
	unsigned int row_shift = get_count_order(sci_kpd->cols);
	unsigned short key;
	int col, row;
	u32 val;

	val = __raw_readl((void __iomem *)KPD_INT_CLR) | KPD_INT_ALL;
	__raw_writel(val, (void __iomem *)KPD_INT_CLR);

	pm_wakeup_event(&sci_kpd->input_dev->dev, 5000);

	if (int_status & KPD_PRESS_INT0) {
		col = KPD_INT0_COL(key_status);
		row = KPD_INT0_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 1);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_RELEASE_INT0) {
		col = KPD_INT0_COL(key_status);
		row = KPD_INT0_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 0);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_PRESS_INT1) {
		col = KPD_INT1_COL(key_status);
		row = KPD_INT1_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 1);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_RELEASE_INT1) {
		col = KPD_INT1_COL(key_status);
		row = KPD_INT1_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 0);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_PRESS_INT2) {
		col = KPD_INT2_COL(key_status);
		row = KPD_INT2_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 1);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_RELEASE_INT2) {
		col = KPD_INT2_COL(key_status);
		row = KPD_INT2_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 0);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_PRESS_INT3) {
		col = KPD_INT3_COL(key_status);
		row = KPD_INT3_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 1);
		input_sync(sci_kpd->input_dev);
	}
	if (int_status & KPD_RELEASE_INT3) {
		col = KPD_INT3_COL(key_status);
		row = KPD_INT3_ROW(key_status);
		key = keycodes[MATRIX_SCAN_CODE(row, col, row_shift)];
		input_report_key(sci_kpd->input_dev, key, 0);
		input_sync(sci_kpd->input_dev);
	}

	return IRQ_HANDLED;
}

static irqreturn_t sci_powerkey_isr(int irq, void *dev_id)
{
	static unsigned long last_value = 1;
	struct sci_keypad_t *sci_kpd = dev_id;
	unsigned long value = !gpio_get_value(sci_kpd->pb_gpio);

	pm_wakeup_event(&sci_kpd->input_dev->dev, 5000);

	if (last_value == value) {
		input_report_key(sci_kpd->input_dev, KEY_POWER, last_value);
		input_sync(sci_kpd->input_dev);
	}

	if (value) {
		input_report_key(sci_kpd->input_dev, KEY_POWER, 0);
		input_sync(sci_kpd->input_dev);
		irq_set_irq_type(irq, IRQF_TRIGGER_HIGH);
	} else {
		input_report_key(sci_kpd->input_dev, KEY_POWER, 1);
		input_sync(sci_kpd->input_dev);
		irq_set_irq_type(irq, IRQF_TRIGGER_LOW);
	}

	last_value = value;
	return IRQ_HANDLED;
}

static irqreturn_t sci_volumedownkey_isr(int irq, void *dev_id)
{
	static unsigned long last_value = 1;
	struct sci_keypad_t *sci_kpd = dev_id;
	unsigned long value = !gpio_get_value(sci_kpd->voldown_gpio);

	if (last_value == value) {
		input_report_key(sci_kpd->input_dev, KEY_VOLUMEDOWN, last_value);
		input_sync(sci_kpd->input_dev);
	}

	if (value) {
		input_report_key(sci_kpd->input_dev, KEY_VOLUMEDOWN, 0);
		input_sync(sci_kpd->input_dev);
		irq_set_irq_type(irq, IRQF_TRIGGER_HIGH);
	} else {
		input_report_key(sci_kpd->input_dev, KEY_VOLUMEDOWN, 1);
		input_sync(sci_kpd->input_dev);
		irq_set_irq_type(irq, IRQF_TRIGGER_LOW);
	}

	last_value = value;
	return IRQ_HANDLED;
}

static struct sci_keypad_platform_data *sci_keypad_parse_dt(struct device *dev,
							    struct sci_keypad_t *sci_kpd)
{
	struct sci_keypad_platform_data *pdata;
	struct device_node *np = dev->of_node, *key_np;
	struct matrix_keymap_data *keymap_data;
	u32 *keymap;
	u32 key_count;

	sci_kpd->pb_gpio = of_get_gpio(np, 0);
	sci_kpd->voldown_gpio = of_get_gpio(np, 1);
	sci_kpd->flip_gpio = of_get_gpio(np, 2);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return NULL;

	if (of_property_read_u32(np, "sprd,keypad-num-rows", &pdata->rows_number) ||
	    of_property_read_u32(np, "sprd,keypad-num-columns", &pdata->cols_number))
		return NULL;

	of_property_read_u32(np, "sprd,debounce_time", &pdata->debounce_time);
	of_property_read_u32(np, "sprd,keypad-rows-choose-hw", &pdata->rows_choose_hw);
	of_property_read_u32(np, "sprd,keypad-cols-choose-hw", &pdata->cols_choose_hw);

	if (!of_get_property(np, "linux,input-no-autorepeat", NULL))
		pdata->repeat = true;
	if (of_get_property(np, "sprd,support_long_key", NULL))
		pdata->support_long_key = true;
	if (of_get_property(np, "linux,input-wakeup", NULL))
		pdata->wakeup = true;

	keymap_data = devm_kzalloc(dev, sizeof(*keymap_data), GFP_KERNEL);
	if (!keymap_data)
		return NULL;

	key_count = of_get_child_count(np);
	keymap = devm_kcalloc(dev, key_count, sizeof(u32), GFP_KERNEL);
	if (!keymap)
		return NULL;

	keymap_data->keymap_size = key_count;
	keymap_data->keymap = keymap;
	pdata->keymap_data = keymap_data;

	for_each_child_of_node(np, key_np) {
		u32 row = 0, col = 0, key_code = 0;
		of_property_read_u32(key_np, "keypad,row", &row);
		of_property_read_u32(key_np, "keypad,column", &col);
		of_property_read_u32(key_np, "linux,code", &key_code);
		*keymap++ = KEY(row, col, key_code);
	}

	return pdata;
}

static int sci_keypad_probe(struct platform_device *pdev)
{
	struct sci_keypad_t *sci_kpd;
	struct input_dev *input_dev;
	struct sci_keypad_platform_data *pdata;
	unsigned int row_shift, keycodemax;
	u32 value;
	int error;

	sci_kpd = devm_kzalloc(&pdev->dev, sizeof(*sci_kpd) + 128 * sizeof(unsigned short),
			       GFP_KERNEL);
	if (!sci_kpd)
		return -ENOMEM;

	pdata = sci_keypad_parse_dt(&pdev->dev, sci_kpd);
	if (!pdata)
		return -EINVAL;

	row_shift = get_count_order(pdata->cols_number);
	keycodemax = pdata->rows_number << row_shift;

	input_dev = devm_input_allocate_device(&pdev->dev);
	if (!input_dev)
		return -ENOMEM;

	sci_kpd->input_dev = input_dev;
	sci_kpd->rows = pdata->rows_number;
	sci_kpd->cols = pdata->cols_number;
	g_sci_kpd = sci_kpd;
	platform_set_drvdata(pdev, sci_kpd);

	__keypad_enable();

	__raw_writel(KPD_INT_ALL, (void __iomem *)KPD_INT_CLR);
	__raw_writel(CFG_ROW_POLARITY | CFG_COL_POLARITY, (void __iomem *)KPD_POLARITY);
	__raw_writel(1, (void __iomem *)KPD_CLK_DIV_CNT);
	__raw_writel(0xc, (void __iomem *)KPD_LONG_KEY_CNT);
	__raw_writel(0x5, (void __iomem *)KPD_DEBOUNCE_CNT);

	sci_kpd->irq = platform_get_irq(pdev, 0);
	if (sci_kpd->irq < 0)
		return -ENODEV;

	error = devm_request_irq(&pdev->dev, sci_kpd->irq, sci_keypad_isr,
				 IRQF_NO_SUSPEND, "sci-keypad", sci_kpd);
	if (error)
		return error;

	input_dev->name = "sci-keypad";
	input_dev->phys = "sci-key/input0";
	input_dev->dev.parent = &pdev->dev;
	input_set_drvdata(input_dev, sci_kpd);
	input_dev->id.bustype = BUS_HOST;
	input_dev->id.vendor = 0x0001;
	input_dev->id.product = 0x0001;
	input_dev->id.version = 0x0100;
	input_dev->keycode = (unsigned short *)(sci_kpd + 1);
	input_dev->keycodesize = sizeof(unsigned short);
	input_dev->keycodemax = keycodemax;

	matrix_keypad_build_keymap(pdata->keymap_data, NULL,
				   pdata->rows_number, pdata->cols_number,
				   input_dev->keycode, input_dev);

	__set_bit(KEY_POWER, input_dev->keybit);
	__set_bit(KEY_VOLUMEDOWN, input_dev->keybit);
	__set_bit(KEY_VOLUMEUP, input_dev->keybit);
	__set_bit(KEY_HOME, input_dev->keybit);
	__set_bit(EV_KEY, input_dev->evbit);

	error = input_register_device(input_dev);
	if (error)
		return error;

	device_init_wakeup(&pdev->dev, 1);
	enable_irq_wake(sci_kpd->irq);

	value = KPD_INT_DOWNUP | (pdata->support_long_key ? KPD_INT_LONG : 0);
	__raw_writel(value, (void __iomem *)KPD_INT_EN);
	__raw_writel(KPD_SLEEP_CNT_VALUE(1000), (void __iomem *)KPD_SLEEP_CNT);

	pdata->rows_choose_hw &= KPDCTL_ROW_MSK_V1;
	pdata->cols_choose_hw &= KPDCTL_COL_MSK_V1;
	value = KPD_EN | KPD_SLEEP_EN | pdata->rows_choose_hw | pdata->cols_choose_hw;
	if (pdata->support_long_key)
		value |= KPD_LONG_KEY_EN;
	__raw_writel(value, (void __iomem *)KPD_CTRL);

	if (gpio_is_valid(sci_kpd->pb_gpio)) {
		devm_gpio_request_one(&pdev->dev, sci_kpd->pb_gpio, GPIOF_IN, "powerkey");
		error = devm_request_irq(&pdev->dev, gpio_to_irq(sci_kpd->pb_gpio),
					 sci_powerkey_isr,
					 IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
					 "powerkey", sci_kpd);
		if (!error)
			enable_irq_wake(gpio_to_irq(sci_kpd->pb_gpio));
	}

	if (gpio_is_valid(sci_kpd->voldown_gpio)) {
		devm_gpio_request_one(&pdev->dev, sci_kpd->voldown_gpio, GPIOF_IN, "volume down");
		devm_request_irq(&pdev->dev, gpio_to_irq(sci_kpd->voldown_gpio),
				 sci_volumedownkey_isr,
				 IRQF_TRIGGER_HIGH | IRQF_NO_SUSPEND,
				 "volume down", sci_kpd);
	}

	if (sec_class) {
		struct device *key_dev = device_create(sec_class, NULL, 0, NULL, "sec_key");
		struct device *pwr_dev = device_create(sec_class, NULL, 0, NULL, "sec_power_key");
		if (!IS_ERR_OR_NULL(key_dev))
			WARN_ON(device_create_file(key_dev, &dev_attr_sec_key_pressed));
		if (!IS_ERR_OR_NULL(pwr_dev))
			WARN_ON(device_create_file(pwr_dev, &dev_attr_sec_power_key_pressed));
	}

	dev_info(&pdev->dev, "Spreadtrum keypad initialized (pb_gpio=%d, voldown_gpio=%d)\n",
		 sci_kpd->pb_gpio, sci_kpd->voldown_gpio);
	return 0;
}

static const struct of_device_id keypad_match_table[] = {
	{ .compatible = "sprd,sci-keypad", },
	{ }
};
MODULE_DEVICE_TABLE(of, keypad_match_table);

static struct platform_driver sci_keypad_driver = {
	.probe	= sci_keypad_probe,
	.driver	= {
		.name		= "sci-keypad",
		.of_match_table	= keypad_match_table,
	},
};

module_platform_driver(sci_keypad_driver);

MODULE_DESCRIPTION("Spreadtrum SC7730SE Keypad Driver");
MODULE_LICENSE("GPL v2");
