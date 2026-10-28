/*
 * RT8555 Backlight Driver for Samsung Galaxy Tab E 9.6 (SM-T560 / SM-T561)
 *
 * Adapted from drivers/video/backlight/rt8555-backlight.c in the 3.10 kernel
 * for Linux 4.9.337. Registers both the I2C client and a standard Linux
 * backlight_device (/sys/class/backlight/panel).
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/backlight.h>
#include <linux/fb.h>

struct rt8555_bl_data {
	struct i2c_client *client;
	struct backlight_device *bd;
	int gpio_en;
	int gpio_pwm;
	int current_brightness;
};

static struct rt8555_bl_data *g_rt8555;

int backlight_i2c_read_sys(u8 reg, u8 *val)
{
	int err = -ENODEV, retry = 3;

	if (!g_rt8555 || !g_rt8555->client)
		return -ENODEV;

	while (retry--) {
		err = i2c_smbus_read_i2c_block_data(g_rt8555->client, reg, 1, val);
		if (err >= 0)
			return err;
	}
	return err;
}
EXPORT_SYMBOL(backlight_i2c_read_sys);

int backlight_i2c_write_sys(u8 reg, u8 val)
{
	int err = -ENODEV, retry = 3;
	u8 temp_val = val;

	if (!g_rt8555 || !g_rt8555->client)
		return -ENODEV;

	while (retry--) {
		err = i2c_smbus_write_i2c_block_data(g_rt8555->client, reg, 1, &temp_val);
		if (err >= 0)
			return err;
	}
	return err;
}
EXPORT_SYMBOL(backlight_i2c_write_sys);

static void rt8555_register_init(void)
{
	backlight_i2c_write_sys(0x02, 0xB6); /* Current Setting */
	backlight_i2c_write_sys(0x08, 0x04); /* LED Driver Headroom */
	backlight_i2c_write_sys(0x50, 0x05);
	backlight_i2c_write_sys(0x51, 0x08);
}

static int rt8555_bl_update_status(struct backlight_device *bd)
{
	struct rt8555_bl_data *data = bl_get_data(bd);
	int brightness = bd->props.brightness;

	if (bd->props.power != FB_BLANK_UNBLANK ||
	    bd->props.fb_blank != FB_BLANK_UNBLANK ||
	    bd->props.state & BL_CORE_FBBLANK)
		brightness = 0;

	if (brightness > 0) {
		if (data->current_brightness == 0) {
			if (gpio_is_valid(data->gpio_en))
				gpio_direction_output(data->gpio_en, 1);
			if (gpio_is_valid(data->gpio_pwm))
				gpio_direction_output(data->gpio_pwm, 1);
			msleep(10);
			rt8555_register_init();
		}
	} else {
		if (gpio_is_valid(data->gpio_en))
			gpio_direction_output(data->gpio_en, 0);
	}

	data->current_brightness = brightness;
	return 0;
}

static int rt8555_bl_get_brightness(struct backlight_device *bd)
{
	struct rt8555_bl_data *data = bl_get_data(bd);

	return data->current_brightness;
}

static const struct backlight_ops rt8555_bl_ops = {
	.update_status	= rt8555_bl_update_status,
	.get_brightness	= rt8555_bl_get_brightness,
};

static int rt8555_backlight_probe(struct i2c_client *client,
				  const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct rt8555_bl_data *data;
	struct backlight_properties props;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EIO;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	data->gpio_en = -EINVAL;
	data->gpio_pwm = 214; /* GPIO 214 PWM3_BL ctrl pin on SM-T560/T561 */

	if (np)
		data->gpio_en = of_get_named_gpio(np, "backlight-en-gpio", 0);
	if (!gpio_is_valid(data->gpio_en))
		data->gpio_en = 155; /* GPIO 155 BL_EN on SM-T560/T561 */

	if (gpio_is_valid(data->gpio_en)) {
		devm_gpio_request_one(&client->dev, data->gpio_en,
				      GPIOF_OUT_INIT_HIGH, "BACKLIGHT_EN_GPIO");
	}
	if (gpio_is_valid(data->gpio_pwm)) {
		devm_gpio_request_one(&client->dev, data->gpio_pwm,
				      GPIOF_OUT_INIT_HIGH, "BACKLIGHT_PWM_GPIO");
	}

	g_rt8555 = data;
	i2c_set_clientdata(client, data);

	msleep(10);
	rt8555_register_init();
	data->current_brightness = 255;

	memset(&props, 0, sizeof(props));
	props.type = BACKLIGHT_RAW;
	props.max_brightness = 255;
	props.brightness = 255;

	data->bd = devm_backlight_device_register(&client->dev, "panel",
						  &client->dev, data,
						  &rt8555_bl_ops, &props);
	if (IS_ERR(data->bd))
		dev_warn(&client->dev, "failed to register backlight device\n");

	dev_info(&client->dev, "RT8555 backlight initialized (en_gpio=%d)\n",
		 data->gpio_en);
	return 0;
}

static const struct i2c_device_id rt8555_backlight_id[] = {
	{ "RICHTEK-RT8555", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, rt8555_backlight_id);

static const struct of_device_id rt8555_backlight_match_table[] = {
	{ .compatible = "Richtek, RT8555" },
	{ .compatible = "richtek,rt8555" },
	{ }
};
MODULE_DEVICE_TABLE(of, rt8555_backlight_match_table);

static struct i2c_driver rt8555_backlight_driver = {
	.driver = {
		.name		= "RICHTEK-RT8555",
		.of_match_table	= rt8555_backlight_match_table,
	},
	.id_table	= rt8555_backlight_id,
	.probe		= rt8555_backlight_probe,
};

module_i2c_driver(rt8555_backlight_driver);

MODULE_DESCRIPTION("RT8555 Backlight Driver for SM-T560/T561");
MODULE_LICENSE("GPL v2");
