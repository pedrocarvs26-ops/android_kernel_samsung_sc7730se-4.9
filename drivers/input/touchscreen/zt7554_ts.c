/*
 * Zinitix ZT7554 Capacitive Touchscreen & Touchkey Driver for SM-T560 / SM-T561
 *
 * Adapted from drivers/input/touchscreen/zt7554_ts.c in the Samsung 3.10 kernel
 * for Linux 4.9.337.
 *
 * Supports:
 * - 5-finger Multi-Touch Type B protocol on 800x1280 panel (I2C1 @ 0x20, IRQ GPIO 94)
 * - 2 capacitive touchkeys (KEY_RECENT / KEY_APPSELECT and KEY_BACK)
 * - Automatic SC2723 regulator power-up (vdd28 + vddsim2) and ZT7554 NVM boot sequence
 *
 * Licensed under the GPL-2.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>

extern void sprd_i2c_ctl_chg_clk(unsigned int id_nr, unsigned int freq);

#define ZT7554_TS_DEVICE		"zt7554_ts"
#define MAX_SUPPORTED_FINGER_NUM	5
#define MAX_SUPPORTED_BUTTON_NUM	2
#define TOUCH_POINT_MODE		1

/* Register Map */
#define ZT7554_SWRESET_CMD		0x0000
#define ZT7554_WAKEUP_CMD		0x0001
#define ZT7554_CLEAR_INT_STATUS_CMD	0x0003
#define ZT7554_IDLE_CMD			0x0004
#define ZT7554_SLEEP_CMD		0x0005
#define ZT7554_TOUCH_MODE		0x0010
#define ZT7554_CHIP_REVISION		0x0011
#define ZT7554_FIRMWARE_VERSION		0x0012
#define ZT7554_DATA_VERSION_REG		0x0013
#define ZT7554_HW_ID			0x0014
#define ZT7554_SUPPORTED_FINGER_NUM	0x0015
#define ZT7554_EEPROM_INFO_REG		0x0018
#define ZT7554_INITIAL_TOUCH_MODE	0x0019
#define ZT7554_VENDOR_ID		0x001C
#define ZT7554_POINT_STATUS_REG		0x0080
#define ZT7554_ICON_STATUS_REG		0x00AA
#define ZT7554_BUTTON_SUPPORTED_NUM	0x00B0
#define ZT7554_X_RESOLUTION		0x00C0
#define ZT7554_Y_RESOLUTION		0x00C1
#define ZT7554_INT_ENABLE_FLAG		0x00F0
#define ZT7554_PERIODICAL_INT_INTERVAL	0x00F1
#define ZT7554_OPTIONAL_SETTING		0x0116
#define ZT7554_MINOR_FW_VERSION		0x0121
#define ZT7554_CHECKSUM_RESULT		0x012C

/* Status bits */
#define BIT_PT_CNT_CHANGE		0
#define BIT_DOWN			1
#define BIT_MOVE			2
#define BIT_UP				3
#define BIT_MUST_ZERO			13
#define BIT_ICON_EVENT			15

#define SUB_BIT_EXIST			0
#define SUB_BIT_DOWN			1
#define SUB_BIT_MOVE			2
#define SUB_BIT_UP			3

#define BIT_O_ICON0_DOWN		0
#define BIT_O_ICON0_UP			8

#define zinitix_bit_test(val, n)	((val) & (1U << (n)))

struct zt7554_coord {
	u16 x;
	u16 y;
	u8 width;
	u8 sub_status;
} __packed;

struct zt7554_point_info {
	u16 status;
	u16 event_flag;
	struct zt7554_coord coord[MAX_SUPPORTED_FINGER_NUM];
} __packed;

struct zt7554_ts_data {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regulator *reg_vdd;
	struct regulator *reg_avdd;
	int gpio_int;
	int gpio_ldo_en;
	int irq;
	u32 max_x;
	u32 max_y;
	u16 ic_int_mask;
	struct zt7554_point_info touch_info;
	struct zt7554_point_info reported_info;
};

static const u32 zt7554_button_keys[MAX_SUPPORTED_BUTTON_NUM] = {
	KEY_APPSELECT,
	KEY_BACK,
};

static int zt7554_read_data(struct i2c_client *client, u16 reg, void *values, u16 length)
{
	int ret, retry = 5;
	__le16 le_reg = cpu_to_le16(reg);

	while (retry--) {
		ret = i2c_master_send(client, (const char *)&le_reg, 2);
		if (ret == 2)
			break;
		mdelay(1);
	}
	if (ret < 0)
		return ret;

	udelay(50);
	ret = i2c_master_recv(client, (char *)values, length);
	if (ret < 0)
		return ret;

	udelay(10);
	return length;
}

static int zt7554_write_reg(struct i2c_client *client, u16 reg, u16 value)
{
	int ret, retry = 5;
	u8 pkt[4];

	pkt[0] = reg & 0xff;
	pkt[1] = (reg >> 8) & 0xff;
	pkt[2] = value & 0xff;
	pkt[3] = (value >> 8) & 0xff;

	while (retry--) {
		ret = i2c_master_send(client, pkt, 4);
		if (ret == 4) {
			udelay(10);
			return 0;
		}
		mdelay(1);
	}
	return ret < 0 ? ret : -EIO;
}

static int zt7554_write_cmd(struct i2c_client *client, u16 cmd)
{
	int ret, retry = 5;
	__le16 le_cmd = cpu_to_le16(cmd);

	while (retry--) {
		ret = i2c_master_send(client, (const char *)&le_cmd, 2);
		if (ret == 2) {
			udelay(10);
			return 0;
		}
		mdelay(1);
	}
	return ret < 0 ? ret : -EIO;
}

static int zt7554_power_on(struct zt7554_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	u16 chip_code = 0;
	int retry = 0;

	if (gpio_is_valid(ts->gpio_ldo_en))
		gpio_direction_output(ts->gpio_ldo_en, 1);

	if (!IS_ERR_OR_NULL(ts->reg_avdd)) {
		regulator_set_voltage(ts->reg_avdd, 2800000, 2800000);
		WARN_ON(regulator_enable(ts->reg_avdd));
	}
	if (!IS_ERR_OR_NULL(ts->reg_vdd)) {
		regulator_set_voltage(ts->reg_vdd, 3300000, 3300000);
		WARN_ON(regulator_enable(ts->reg_vdd));
	}

	mdelay(50);

retry_seq:
	if (zt7554_write_reg(client, 0xc000, 0x0001) < 0)
		goto err_retry;
	udelay(10);

	if (zt7554_read_data(client, 0xcc00, &chip_code, 2) < 0)
		goto err_retry;
	udelay(10);

	if (zt7554_write_cmd(client, 0xc004) < 0)
		goto err_retry;
	udelay(10);

	if (zt7554_write_reg(client, 0xc002, 0x0001) < 0)
		goto err_retry;
	mdelay(2);

	if (zt7554_write_reg(client, 0xc001, 0x0001) < 0)
		goto err_retry;

	mdelay(150);
	dev_info(&client->dev, "ZT7554 power sequence complete (chip_code=0x%04x)\n",
		 chip_code);
	return 0;

err_retry:
	if (++retry < 3) {
		mdelay(20);
		goto retry_seq;
	}
	return -EIO;
}

static int zt7554_init_touch(struct zt7554_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	u16 vendor_id = 0, fw_ver = 0, minor_ver = 0, reg_ver = 0;
	int i;

	zt7554_write_reg(client, ZT7554_INT_ENABLE_FLAG, 0x0);
	zt7554_write_cmd(client, ZT7554_SWRESET_CMD);
	mdelay(10);

	zt7554_read_data(client, ZT7554_VENDOR_ID, &vendor_id, 2);
	zt7554_read_data(client, ZT7554_FIRMWARE_VERSION, &fw_ver, 2);
	zt7554_read_data(client, ZT7554_MINOR_FW_VERSION, &minor_ver, 2);
	zt7554_read_data(client, ZT7554_DATA_VERSION_REG, &reg_ver, 2);

	zt7554_write_reg(client, ZT7554_X_RESOLUTION, (u16)ts->max_x);
	zt7554_write_reg(client, ZT7554_Y_RESOLUTION, (u16)ts->max_y);
	zt7554_write_reg(client, ZT7554_BUTTON_SUPPORTED_NUM, MAX_SUPPORTED_BUTTON_NUM);
	zt7554_write_reg(client, ZT7554_SUPPORTED_FINGER_NUM, MAX_SUPPORTED_FINGER_NUM);
	zt7554_write_reg(client, ZT7554_INITIAL_TOUCH_MODE, TOUCH_POINT_MODE);
	zt7554_write_reg(client, ZT7554_TOUCH_MODE, TOUCH_POINT_MODE);
	zt7554_write_reg(client, ZT7554_OPTIONAL_SETTING, 0);

	ts->ic_int_mask = BIT(BIT_PT_CNT_CHANGE) | BIT(BIT_DOWN) |
			  BIT(BIT_MOVE) | BIT(BIT_UP) | BIT(BIT_ICON_EVENT);
	zt7554_write_reg(client, ZT7554_INT_ENABLE_FLAG, ts->ic_int_mask);

	for (i = 0; i < 10; i++)
		zt7554_write_cmd(client, ZT7554_CLEAR_INT_STATUS_CMD);

	dev_info(&client->dev, "ZT7554 initialized (vendor=0x%04x, fw=%u.%u, reg=%u, %ux%u)\n",
		 vendor_id, fw_ver, minor_ver, reg_ver, ts->max_x, ts->max_y);
	return 0;
}

static irqreturn_t zt7554_ts_irq_handler(int irq, void *dev_id)
{
	struct zt7554_ts_data *ts = dev_id;
	struct i2c_client *client = ts->client;
	u16 icon_event = 0;
	int i, ret;

	if (gpio_is_valid(ts->gpio_int) && gpio_get_value(ts->gpio_int))
		return IRQ_HANDLED;

	ret = zt7554_read_data(client, ZT7554_POINT_STATUS_REG,
			       &ts->touch_info, sizeof(ts->touch_info));
	if (ret < 0 || ts->touch_info.status == 0xffff ||
	    zinitix_bit_test(ts->touch_info.status, BIT_MUST_ZERO)) {
		zt7554_write_cmd(client, ZT7554_CLEAR_INT_STATUS_CMD);
		return IRQ_HANDLED;
	}

	zt7554_write_cmd(client, ZT7554_CLEAR_INT_STATUS_CMD);

	if (ts->touch_info.status == 0)
		return IRQ_HANDLED;

	if (zinitix_bit_test(ts->touch_info.status, BIT_ICON_EVENT)) {
		if (zt7554_read_data(client, ZT7554_ICON_STATUS_REG, &icon_event, 2) >= 0) {
			for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++) {
				if (zinitix_bit_test(icon_event, BIT_O_ICON0_DOWN + i)) {
					input_report_key(ts->input_dev, zt7554_button_keys[i], 1);
					if (i == 0)
						input_report_key(ts->input_dev, KEY_MENU, 1);
				}
				if (zinitix_bit_test(icon_event, BIT_O_ICON0_UP + i)) {
					input_report_key(ts->input_dev, zt7554_button_keys[i], 0);
					if (i == 0)
						input_report_key(ts->input_dev, KEY_MENU, 0);
				}
			}
		}
	}

	for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
		u8 sub = ts->touch_info.coord[i].sub_status;
		u8 prev = ts->reported_info.coord[i].sub_status;

		if (zinitix_bit_test(sub, SUB_BIT_EXIST)) {
			u32 x = ts->touch_info.coord[i].x;
			u32 y = ts->touch_info.coord[i].y;
			u32 w = ts->touch_info.coord[i].width ? : 1;

			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, w);
			input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
		} else if (zinitix_bit_test(sub, SUB_BIT_UP) ||
			   zinitix_bit_test(prev, SUB_BIT_EXIST)) {
			memset(&ts->touch_info.coord[i], 0, sizeof(struct zt7554_coord));
			input_mt_slot(ts->input_dev, i);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
		}
	}

	memcpy(&ts->reported_info, &ts->touch_info, sizeof(ts->touch_info));
	input_sync(ts->input_dev);
	return IRQ_HANDLED;
}

static int zt7554_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct zt7554_ts_data *ts;
	struct input_dev *input_dev;
	int i, ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EIO;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->max_x = 800;
	ts->max_y = 1280;
	ts->gpio_int = -EINVAL;

	if (np) {
		ts->gpio_int = of_get_named_gpio(np, "tsp,irq-gpio", 0);
		if (!gpio_is_valid(ts->gpio_int))
			ts->gpio_int = of_get_gpio(np, 0);
		of_property_read_u32(np, "zt7554,x_resolution", &ts->max_x);
		of_property_read_u32(np, "zt7554,y_resolution", &ts->max_y);
		of_property_read_u32(np, "tsp,x_resolution", &ts->max_x);
		of_property_read_u32(np, "tsp,y_resolution", &ts->max_y);
	}
	if (!gpio_is_valid(ts->gpio_int))
		ts->gpio_int = 82; /* GPIO 82 TSP_INT on SM-T560/T561 */

	ts->gpio_ldo_en = 156; /* GPIO 156 tsp_ldo 3.3V enable on SM-T560/T561 */
	if (gpio_is_valid(ts->gpio_ldo_en))
		devm_gpio_request_one(&client->dev, ts->gpio_ldo_en,
				      GPIOF_OUT_INIT_HIGH, "tsp_ldo_en");

	ts->reg_vdd = regulator_get(&client->dev, "tsp_vdd");
	if (IS_ERR(ts->reg_vdd))
		ts->reg_vdd = regulator_get(NULL, "tsp_ldo");
	ts->reg_avdd = regulator_get(NULL, "vdd28");

	if (gpio_is_valid(ts->gpio_int)) {
		devm_gpio_request_one(&client->dev, ts->gpio_int, GPIOF_IN, "tsp_int");
		ts->irq = gpio_to_irq(ts->gpio_int);
	} else {
		ts->irq = client->irq;
	}

	ret = zt7554_power_on(ts);
	if (ret < 0) {
		dev_warn(&client->dev, "ZT7554 power sequence failed (%d), continuing\n", ret);
	} else {
		zt7554_init_touch(ts);
		sprd_i2c_ctl_chg_clk(1, 400000);
	}

	input_dev = devm_input_allocate_device(&client->dev);
	if (!input_dev)
		return -ENOMEM;

	ts->input_dev = input_dev;
	input_dev->name = "sec_touchscreen";
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	__set_bit(EV_SYN, input_dev->evbit);
	__set_bit(EV_KEY, input_dev->evbit);
	__set_bit(EV_ABS, input_dev->evbit);
	__set_bit(INPUT_PROP_DIRECT, input_dev->propbit);

	for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++)
		__set_bit(zt7554_button_keys[i], input_dev->keybit);
	__set_bit(KEY_MENU, input_dev->keybit);

	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, ts->max_x - 1, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, ts->max_y - 1, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_WIDTH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);

	input_mt_init_slots(input_dev, MAX_SUPPORTED_FINGER_NUM, INPUT_MT_DIRECT);

	input_set_drvdata(input_dev, ts);
	i2c_set_clientdata(client, ts);

	ret = input_register_device(input_dev);
	if (ret)
		return ret;

	if (ts->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, ts->irq, NULL,
						zt7554_ts_irq_handler,
						IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
						ZT7554_TS_DEVICE, ts);
		if (ret)
			dev_warn(&client->dev, "failed to request irq %d: %d\n", ts->irq, ret);
	}

	return 0;
}

static const struct i2c_device_id zt7554_idtable[] = {
	{ ZT7554_TS_DEVICE, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, zt7554_idtable);

static const struct of_device_id zt7554_of_match[] = {
	{ .compatible = "zinitix,zt7554_ts" },
	{ .compatible = "zinitix,zt7554" },
	{ }
};
MODULE_DEVICE_TABLE(of, zt7554_of_match);

static struct i2c_driver zt7554_ts_driver = {
	.driver = {
		.name		= ZT7554_TS_DEVICE,
		.of_match_table	= zt7554_of_match,
	},
	.id_table	= zt7554_idtable,
	.probe		= zt7554_ts_probe,
};

module_i2c_driver(zt7554_ts_driver);

MODULE_DESCRIPTION("Zinitix ZT7554 Touchscreen Driver for SM-T560/T561");
MODULE_LICENSE("GPL v2");
