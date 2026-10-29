/*
 * Zinitix ZT7554 Capacitive Touchscreen & Touchkey Driver for SM-T560 / SM-T561
 *
 * Adapted from drivers/input/touchscreen/zt7554_ts.c in the Samsung 3.10 kernel
 * for Linux 4.9.337.
 *
 * Supports:
 * - 5-finger Multi-Touch Type B protocol on 800x1280 panel (I2C1 @ 0x20, IRQ GPIO 82)
 * - 2 capacitive touchkeys (Samsung KEY_RECENT = 254 and KEY_BACK)
 * - Automatic regulator power-up and ZT7554 NVM boot sequence
 * - ESD watchdog: periodic chip interrupt + power-cycle recovery (as in 3.10)
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
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/ktime.h>
#include <linux/jiffies.h>
#include <linux/sysfs.h>

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
#define ZT7554_CALIBRATE_CMD		0x0006
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

/*
 * TOUCH_POINT_MODE 1 layout: status (0x80), event_flag (0x81), then one
 * 4-register (8 byte) slot per finger starting at 0x82, of which only the
 * first 6 bytes {x, y, width, sub_status} are valid. Only fingers flagged in
 * event_flag carry data (this is how the 3.10 driver reads them).
 */
#define ZT7554_COORD_REG(n)		(ZT7554_POINT_STATUS_REG + 2 + (n) * 4)

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

/* Tunables */
#define ZT7554_SCAN_RATE_HZ		100	/* periodic INT every 100 scans = 1 s */
#define ZT7554_POLL_MS			15
#define ZT7554_WD_PERIOD_MS		1000
#define ZT7554_WD_IDLE_TICKS		3	/* no chip INT for 3 s: probe it */
#define ZT7554_BAD_READ_LIMIT		50	/* 3.10: 50 retries, then power cycle */
#define ZT7554_MAX_RECOVERS		5	/* per minute, then give up */
#define ZT7554_STORM_PER_SEC		1000
#define ZT7554_STATS_TICKS		60
#define ZT7554_DEBUG_FRAMES		5
#define ZT7554_DEBUG_FINGERS		20
#define ZT7554_DEBUG_KEYS		20

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
	bool periodic_ok;
	struct mutex lock;
	struct delayed_work poll_work;	/* polling fallback if the IRQ path breaks */
	struct delayed_work wd_work;	/* 1 s ESD watchdog + stats */
	bool poll_mode;
	bool need_recover;
	bool recover_irq_off;
	bool dead;
	bool removing;
	bool quiet_warned;

	/* statistics (sysfs "stats") */
	unsigned int irq_count;
	unsigned int spurious;
	unsigned int events;		/* chip interrupts serviced (INT low) */
	unsigned int frames;		/* input frames reported */
	unsigned int multi_frames;	/* frames with more than one finger */
	unsigned int idle_ints;		/* status 0: periodic interrupt */
	unsigned int bad_status;
	unsigned int i2c_errors;
	unsigned int rescues;		/* INT low but no IRQ delivered */
	unsigned int reinits;
	u16 last_bad_status;
	unsigned int debug_frames;
	unsigned int debug_fingers;
	unsigned int debug_keys;

	/* state */
	unsigned int bad_streak;
	unsigned long spur_window;
	unsigned int spur_in_window;
	unsigned long recover_window;
	unsigned int recover_in_window;
	unsigned int wd_ticks;
	unsigned int wd_last_irqs;
	unsigned int wd_last_events;
	unsigned int wd_last_frames;
	unsigned int wd_idle_ticks;
	unsigned int wd_stuck_ticks;
	unsigned int wd_probe_fails;

	struct zt7554_point_info touch_info;
	struct zt7554_point_info reported_info;
};

/*
 * Samsung's KEY_RECENT (254; not in mainline input-event-codes.h). The ROM's
 * /vendor/usr/keylayout/sec_touchscreen.kl maps "key 254 APP_SWITCH" and
 * "key 158 BACK", matching the stock 3.10 driver's
 * BUTTON_MAPPING_KEY = {KEY_RECENT, KEY_BACK}. KEY_APPSELECT (580) and
 * KEY_MENU (139) are not APP_SWITCH in that layout, so the recents key
 * did nothing.
 */
#define ZT7554_KEY_RECENT	254

static const u32 zt7554_button_keys[MAX_SUPPORTED_BUTTON_NUM] = {
	ZT7554_KEY_RECENT,
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

/* vendor command enable, NVM init and program start (3.10 power_sequence) */
static int zt7554_boot_seq(struct zt7554_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	u16 chip_code = 0;
	int retry = 0;

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

	msleep(150);
	dev_info(&client->dev, "ZT7554 power sequence complete (chip_code=0x%04x)\n",
		 chip_code);
	return 0;

err_retry:
	if (++retry < 3) {
		msleep(20);
		goto retry_seq;
	}
	return -EIO;
}

static int zt7554_power_on(struct zt7554_ts_data *ts)
{
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

	msleep(50);
	return zt7554_boot_seq(ts);
}

static int zt7554_init_touch(struct zt7554_ts_data *ts, bool calibrate)
{
	struct i2c_client *client = ts->client;
	u16 vendor_id = 0, fw_ver = 0, minor_ver = 0, reg_ver = 0, periodic = 0;
	int i, err = 0;

#define ZT_W(reg, val)	do { if (zt7554_write_reg(client, reg, val) < 0) err++; } while (0)
	ZT_W(ZT7554_INT_ENABLE_FLAG, 0x0);
	if (zt7554_write_cmd(client, ZT7554_SWRESET_CMD) < 0)
		err++;
	mdelay(10);

	zt7554_read_data(client, ZT7554_VENDOR_ID, &vendor_id, 2);
	zt7554_read_data(client, ZT7554_FIRMWARE_VERSION, &fw_ver, 2);
	zt7554_read_data(client, ZT7554_MINOR_FW_VERSION, &minor_ver, 2);
	zt7554_read_data(client, ZT7554_DATA_VERSION_REG, &reg_ver, 2);

	ZT_W(ZT7554_X_RESOLUTION, (u16)ts->max_x);
	ZT_W(ZT7554_Y_RESOLUTION, (u16)ts->max_y);
	ZT_W(ZT7554_BUTTON_SUPPORTED_NUM, MAX_SUPPORTED_BUTTON_NUM);
	ZT_W(ZT7554_SUPPORTED_FINGER_NUM, MAX_SUPPORTED_FINGER_NUM);
	ZT_W(ZT7554_INITIAL_TOUCH_MODE, TOUCH_POINT_MODE);
	ZT_W(ZT7554_TOUCH_MODE, TOUCH_POINT_MODE);
	ZT_W(ZT7554_OPTIONAL_SETTING, 0);

	/* soft calibration, as the 3.10 driver does when re-initialising */
	if (calibrate && zt7554_write_cmd(client, ZT7554_CALIBRATE_CMD) < 0)
		err++;

	ts->ic_int_mask = BIT(BIT_PT_CNT_CHANGE) | BIT(BIT_DOWN) |
			  BIT(BIT_MOVE) | BIT(BIT_UP) | BIT(BIT_ICON_EVENT);
	ZT_W(ZT7554_INT_ENABLE_FLAG, ts->ic_int_mask);

	for (i = 0; i < 10; i++)
		zt7554_write_cmd(client, ZT7554_CLEAR_INT_STATUS_CMD);

	/*
	 * 3.10 ESD support: the chip raises an interrupt with status 0 every
	 * SCAN_RATE_HZ scans, so a hung chip can be told apart from an idle one.
	 */
	ZT_W(ZT7554_PERIODICAL_INT_INTERVAL, ZT7554_SCAN_RATE_HZ);
	if (zt7554_read_data(client, ZT7554_PERIODICAL_INT_INTERVAL, &periodic, 2) < 0)
		err++;
	ts->periodic_ok = periodic == ZT7554_SCAN_RATE_HZ;
#undef ZT_W

	dev_info(&client->dev,
		 "ZT7554 initialized (vendor=0x%04x, fw=%u.%u, reg=%u, %ux%u, periodic=%u, errors=%d)\n",
		 vendor_id, fw_ver, minor_ver, reg_ver, ts->max_x, ts->max_y,
		 periodic, err);
	return err ? -EIO : 0;
}

static int zt7554_read_points(struct zt7554_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	struct zt7554_point_info *ti = &ts->touch_info;
	int i, ret;

	memset(ti, 0, sizeof(*ti));
	/* status, event_flag and finger 0 are contiguous */
	ret = zt7554_read_data(client, ZT7554_POINT_STATUS_REG, ti,
			       offsetof(struct zt7554_point_info, coord[1]));
	if (ret < 0)
		return ret;
	if (!ti->status || !ti->event_flag)
		return 0;

	for (i = 1; i < MAX_SUPPORTED_FINGER_NUM; i++) {
		if (!zinitix_bit_test(ti->event_flag, i))
			continue;
		ret = zt7554_read_data(client, ZT7554_COORD_REG(i), &ti->coord[i],
				       sizeof(ti->coord[i]));
		if (ret < 0)
			return ret;
	}
	return 0;
}

static void zt7554_release_all(struct zt7554_ts_data *ts)
{
	struct input_dev *input = ts->input_dev;
	int i;

	for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
		input_mt_slot(input, i);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
	}
	for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++)
		input_report_key(input, zt7554_button_keys[i], 0);
	input_mt_sync_frame(input);
	input_sync(input);
	memset(&ts->reported_info, 0, sizeof(ts->reported_info));
}

/* ts->lock held. The power cycle itself runs from the watchdog work. */
static void zt7554_request_recover(struct zt7554_ts_data *ts)
{
	if (ts->need_recover || ts->dead || ts->removing)
		return;
	ts->need_recover = true;
	if (ts->irq > 0 && !ts->poll_mode) {
		/* nosync: we may be running in the IRQ thread itself */
		disable_irq_nosync(ts->irq);
		ts->recover_irq_off = true;
	}
	mod_delayed_work(system_wq, &ts->wd_work, 0);
}

/* ts->lock held, IRQ disabled (or poll mode) */
static int zt7554_recover(struct zt7554_ts_data *ts, const char *why)
{
	struct i2c_client *client = ts->client;
	int ret;

	ts->reinits++;
	dev_warn(&client->dev, "%s: power-cycling controller (#%u)\n", why, ts->reinits);
	zt7554_release_all(ts);

	/* tsp_ldo is an always-on fixed regulator: drive its enable directly */
	if (gpio_is_valid(ts->gpio_ldo_en)) {
		gpio_direction_output(ts->gpio_ldo_en, 0);
		msleep(50);		/* 3.10 CHIP_OFF_DELAY */
		gpio_direction_output(ts->gpio_ldo_en, 1);
		msleep(200);		/* 3.10 CHIP_ON_DELAY */
	}
	ret = zt7554_boot_seq(ts);
	if (!ret)
		ret = zt7554_init_touch(ts, true);
	if (ret)
		dev_err(&client->dev, "controller recovery failed (%d)\n", ret);

	ts->bad_streak = 0;
	ts->wd_probe_fails = 0;

	if (time_after(jiffies, ts->recover_window + 60 * HZ)) {
		ts->recover_window = jiffies;
		ts->recover_in_window = 0;
	}
	if (++ts->recover_in_window > ZT7554_MAX_RECOVERS) {
		ts->dead = true;
		dev_err(&client->dev, "%u recoveries within a minute, giving up\n",
			ts->recover_in_window);
	}
	return ret;
}

static void zt7554_process(struct zt7554_ts_data *ts)
{
	struct i2c_client *client = ts->client;
	struct input_dev *input = ts->input_dev;
	struct zt7554_point_info *ti = &ts->touch_info;
	u16 icon_event = 0, status;
	int i, ret, fingers = 0;

	if (gpio_is_valid(ts->gpio_int) && gpio_get_value(ts->gpio_int))
		return;

	ts->events++;
	ret = zt7554_read_points(ts);
	status = ti->status;
	zt7554_write_cmd(client, ZT7554_CLEAR_INT_STATUS_CMD);

	if (ret < 0 || status == 0xffff || status == 0x1 ||
	    zinitix_bit_test(status, BIT_MUST_ZERO)) {
		if (ret < 0)
			ts->i2c_errors++;
		else
			ts->bad_status++;
		ts->last_bad_status = status;
		if (++ts->bad_streak >= ZT7554_BAD_READ_LIMIT)
			zt7554_request_recover(ts);
		return;
	}
	ts->bad_streak = 0;

	/* periodic (ESD) interrupt: nothing to report */
	if (status == 0) {
		ts->idle_ints++;
		return;
	}

	if (ts->frames < 3)
		dev_info(&client->dev, "touch report #%u: status=0x%04x flags=0x%04x x=%u y=%u (%s)\n",
			 ts->frames + 1, status, ti->event_flag, ti->coord[0].x,
			 ti->coord[0].y, ts->poll_mode ? "poll" : "irq");

	if ((ti->event_flag & ~1U) && ts->debug_frames < ZT7554_DEBUG_FRAMES) {
		ts->debug_frames++;
		dev_info(&client->dev,
			 "mt frame #%u: status=0x%04x flags=0x%04x [%02x %u,%u] [%02x %u,%u] [%02x %u,%u] [%02x %u,%u] [%02x %u,%u]\n",
			 ts->debug_frames, status, ti->event_flag,
			 ti->coord[0].sub_status, ti->coord[0].x, ti->coord[0].y,
			 ti->coord[1].sub_status, ti->coord[1].x, ti->coord[1].y,
			 ti->coord[2].sub_status, ti->coord[2].x, ti->coord[2].y,
			 ti->coord[3].sub_status, ti->coord[3].x, ti->coord[3].y,
			 ti->coord[4].sub_status, ti->coord[4].x, ti->coord[4].y);
	}
	ts->frames++;

	if (zinitix_bit_test(status, BIT_ICON_EVENT)) {
		if (zt7554_read_data(client, ZT7554_ICON_STATUS_REG, &icon_event, 2) >= 0) {
			if (ts->debug_keys < ZT7554_DEBUG_KEYS) {
				ts->debug_keys++;
				dev_info(&client->dev, "icon event 0x%04x\n", icon_event);
			}
			for (i = 0; i < MAX_SUPPORTED_BUTTON_NUM; i++) {
				if (zinitix_bit_test(icon_event, BIT_O_ICON0_DOWN + i)) {
					input_report_key(input, zt7554_button_keys[i], 1);
				}
				if (zinitix_bit_test(icon_event, BIT_O_ICON0_UP + i)) {
					input_report_key(input, zt7554_button_keys[i], 0);
				}
			}
		} else {
			ts->i2c_errors++;
		}
	}

	for (i = 0; i < MAX_SUPPORTED_FINGER_NUM; i++) {
		struct zt7554_coord *c = &ti->coord[i];
		u8 sub = c->sub_status;
		u8 prev = ts->reported_info.coord[i].sub_status;

		if (zinitix_bit_test(sub, SUB_BIT_EXIST)) {
			u32 w = c->width ? : 1;

			fingers++;
			if (zinitix_bit_test(sub, SUB_BIT_DOWN) &&
			    ts->debug_fingers < ZT7554_DEBUG_FINGERS) {
				ts->debug_fingers++;
				dev_info(&client->dev, "finger %d down x=%u y=%u w=%u\n",
					 i, c->x, c->y, w);
			}
			input_mt_slot(input, i);
			input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
			input_report_abs(input, ABS_MT_TOUCH_MAJOR, w);
			input_report_abs(input, ABS_MT_PRESSURE, w);
			input_report_abs(input, ABS_MT_WIDTH_MAJOR, w);
			input_report_abs(input, ABS_MT_POSITION_X, c->x);
			input_report_abs(input, ABS_MT_POSITION_Y, c->y);
		} else if (zinitix_bit_test(sub, SUB_BIT_UP) ||
			   zinitix_bit_test(prev, SUB_BIT_EXIST)) {
			if (ts->debug_fingers < ZT7554_DEBUG_FINGERS) {
				ts->debug_fingers++;
				dev_info(&client->dev, "finger %d up\n", i);
			}
			memset(c, 0, sizeof(*c));
			input_mt_slot(input, i);
			input_mt_report_slot_state(input, MT_TOOL_FINGER, false);
		} else {
			memset(c, 0, sizeof(*c));
		}
	}
	if (fingers > 1)
		ts->multi_frames++;

	memcpy(&ts->reported_info, ti, sizeof(*ti));
	/*
	 * INPUT_MT_DIRECT advertises BTN_TOUCH; Android's InputReader treats a
	 * device that has BTN_TOUCH but never reports it pressed as hovering,
	 * so every frame must also go through the pointer emulation.
	 */
	input_mt_sync_frame(input);
	input_sync(input);
}

static int zt7554_format_stats(struct zt7554_ts_data *ts, char *buf, size_t len)
{
	return scnprintf(buf, len,
			 "irq=%u spurious=%u events=%u frames=%u multi=%u idle=%u bad=%u(0x%04x) i2c_err=%u rescues=%u reinits=%u poll=%d dead=%d INT=%d periodic=%d\n",
			 ts->irq_count, ts->spurious, ts->events, ts->frames,
			 ts->multi_frames, ts->idle_ints, ts->bad_status,
			 ts->last_bad_status, ts->i2c_errors, ts->rescues,
			 ts->reinits, ts->poll_mode, ts->dead,
			 gpio_is_valid(ts->gpio_int) ? gpio_get_value(ts->gpio_int) : -1,
			 ts->periodic_ok);
}

static ssize_t stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct zt7554_ts_data *ts = i2c_get_clientdata(to_i2c_client(dev));

	return zt7554_format_stats(ts, buf, PAGE_SIZE);
}
static DEVICE_ATTR_RO(stats);

static irqreturn_t zt7554_ts_irq_handler(int irq, void *dev_id)
{
	struct zt7554_ts_data *ts = dev_id;
	int level;

	mutex_lock(&ts->lock);
	level = gpio_is_valid(ts->gpio_int) ? gpio_get_value(ts->gpio_int) : 0;
	if (++ts->irq_count <= 3)
		dev_info(&ts->client->dev, "irq #%u (INT=%d)\n", ts->irq_count, level);

	if (level) {
		/* INT already released: nothing pending. Many per second = storm. */
		ts->spurious++;
		if (time_after(jiffies, ts->spur_window + HZ)) {
			ts->spur_window = jiffies;
			ts->spur_in_window = 0;
		}
		if (++ts->spur_in_window >= ZT7554_STORM_PER_SEC && !ts->poll_mode) {
			ts->poll_mode = true;
			disable_irq_nosync(irq);
			dev_warn(&ts->client->dev,
				 "IRQ storm (%u spurious IRQs in 1 s), switching to %d ms polling\n",
				 ts->spur_in_window, ZT7554_POLL_MS);
			schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(ZT7554_POLL_MS));
		}
	} else {
		zt7554_process(ts);
	}
	mutex_unlock(&ts->lock);
	return IRQ_HANDLED;
}

/* Polling fallback, used only once the IRQ path proved unusable. */
static void zt7554_poll_work(struct work_struct *work)
{
	struct zt7554_ts_data *ts = container_of(to_delayed_work(work),
						 struct zt7554_ts_data, poll_work);
	bool again;

	mutex_lock(&ts->lock);
	again = ts->poll_mode && !ts->dead && !ts->removing;
	if (again)
		zt7554_process(ts);
	mutex_unlock(&ts->lock);

	if (again)
		schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(ZT7554_POLL_MS));
}

/*
 * 1 s watchdog (3.10 ESD timer equivalent, but it only power-cycles a chip
 * that is really gone):
 *  - runs the power cycle requested by process() after repeated bad reads;
 *  - services an INT that stays low without an IRQ being delivered, and falls
 *    back to polling if that keeps happening;
 *  - if the chip has been silent for ZT7554_WD_IDLE_TICKS s despite its
 *    periodic interrupt, checks that it still answers and still has our
 *    configuration (a chip reset loses it), else power-cycles it;
 *  - logs a stats line every minute while the panel is being used.
 */
static void zt7554_wd_work(struct work_struct *work)
{
	struct zt7554_ts_data *ts = container_of(to_delayed_work(work),
						 struct zt7554_ts_data, wd_work);
	struct i2c_client *client = ts->client;
	bool irq_off = false, go_poll = false, stuck;
	char buf[256];
	u16 val = 0;
	int ret;

	mutex_lock(&ts->lock);
	if (ts->removing) {
		mutex_unlock(&ts->lock);
		return;
	}

	if (ts->need_recover) {
		zt7554_recover(ts, "repeated bad reads");
		ts->need_recover = false;
		irq_off = ts->recover_irq_off && !ts->dead;
		ts->recover_irq_off = false;
		goto out;
	}
	if (ts->dead)
		goto stats;

	stuck = ts->irq > 0 && !ts->poll_mode && gpio_is_valid(ts->gpio_int) &&
		!gpio_get_value(ts->gpio_int) && ts->irq_count == ts->wd_last_irqs;
	if (stuck) {
		mutex_unlock(&ts->lock);
		msleep(10);
		mutex_lock(&ts->lock);
		stuck = !ts->poll_mode && !gpio_get_value(ts->gpio_int) &&
			ts->irq_count == ts->wd_last_irqs;
	}
	if (stuck) {
		ts->rescues++;
		zt7554_process(ts);
		if (++ts->wd_stuck_ticks >= 3 && !ts->poll_mode) {
			ts->poll_mode = true;
			go_poll = true;
			dev_warn(&client->dev,
				 "INT low with no IRQ (irq %d): falling back to %d ms polling\n",
				 ts->irq, ZT7554_POLL_MS);
		}
	} else {
		ts->wd_stuck_ticks = 0;
	}

	if (ts->events != ts->wd_last_events) {
		ts->wd_idle_ticks = 0;
	} else if (++ts->wd_idle_ticks >= ZT7554_WD_IDLE_TICKS) {
		ts->wd_idle_ticks = 0;
		ret = zt7554_read_data(client, ZT7554_PERIODICAL_INT_INTERVAL, &val, 2);
		if (ret < 0 || (ts->periodic_ok && val != ZT7554_SCAN_RATE_HZ)) {
			dev_warn(&client->dev, "controller probe failed (ret=%d, periodic=%u)\n",
				 ret, val);
			if (++ts->wd_probe_fails >= 2) {
				if (ts->irq > 0 && !ts->poll_mode) {
					disable_irq_nosync(ts->irq);
					irq_off = true;
				}
				zt7554_recover(ts, "controller stopped answering");
				if (ts->dead)
					irq_off = false;
				goto out;
			}
		} else {
			ts->wd_probe_fails = 0;
			if (!ts->quiet_warned) {
				ts->quiet_warned = true;
				dev_info(&client->dev,
					 "no chip interrupt for %d s but it answers (periodic=%u)\n",
					 ZT7554_WD_IDLE_TICKS, val);
			}
		}
	}

stats:
	if (++ts->wd_ticks % ZT7554_STATS_TICKS == 0 && ts->frames != ts->wd_last_frames) {
		ts->wd_last_frames = ts->frames;
		zt7554_format_stats(ts, buf, sizeof(buf));
		dev_info(&client->dev, "stats: %s", buf);
	}
out:
	ts->wd_last_irqs = ts->irq_count;
	ts->wd_last_events = ts->events;
	mutex_unlock(&ts->lock);

	if (irq_off)
		enable_irq(ts->irq);
	if (go_poll) {
		disable_irq(ts->irq);
		schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(ZT7554_POLL_MS));
	}
	schedule_delayed_work(&ts->wd_work, msecs_to_jiffies(ZT7554_WD_PERIOD_MS));
}

static int zt7554_ts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct device_node *np = client->dev.of_node;
	struct zt7554_ts_data *ts;
	struct input_dev *input_dev;
	int i, ret;
	ktime_t t0 = ktime_get();

#define ZT_STAMP(what) dev_info(&client->dev, "probe +%lld ms: %s\n", \
				ktime_ms_delta(ktime_get(), t0), what)

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EIO;

	ts = devm_kzalloc(&client->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->max_x = 800;
	ts->max_y = 1280;
	ts->gpio_int = -EINVAL;
	ts->spur_window = jiffies;
	ts->recover_window = jiffies;

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

	mutex_init(&ts->lock);
	INIT_DELAYED_WORK(&ts->poll_work, zt7554_poll_work);
	INIT_DEFERRABLE_WORK(&ts->wd_work, zt7554_wd_work);
	ZT_STAMP("gpios/regulators");

	ret = zt7554_power_on(ts);
	ZT_STAMP("power_on");
	if (ret < 0) {
		dev_warn(&client->dev, "ZT7554 power sequence failed (%d), continuing\n", ret);
	} else {
		zt7554_init_touch(ts, false);
		sprd_i2c_ctl_chg_clk(1, 400000);
	}
	ZT_STAMP("init_touch");

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
	ZT_STAMP("input registered");

	if (device_create_file(&client->dev, &dev_attr_stats))
		dev_warn(&client->dev, "failed to create stats attribute\n");

	if (ts->irq > 0) {
		ret = devm_request_threaded_irq(&client->dev, ts->irq, NULL,
						zt7554_ts_irq_handler,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						ZT7554_TS_DEVICE, ts);
		if (ret) {
			dev_warn(&client->dev, "failed to request irq %d: %d\n", ts->irq, ret);
			ts->irq = 0;
		}
	}
	dev_info(&client->dev, "irq %d (gpio %d, level-low), INT=%d\n", ts->irq,
		 ts->gpio_int, gpio_is_valid(ts->gpio_int) ? gpio_get_value(ts->gpio_int) : -1);
	ZT_STAMP("irq requested");

	if (ts->irq <= 0 && gpio_is_valid(ts->gpio_int)) {
		ts->poll_mode = true;
		schedule_delayed_work(&ts->poll_work, msecs_to_jiffies(ZT7554_POLL_MS));
	}
	schedule_delayed_work(&ts->wd_work, msecs_to_jiffies(ZT7554_WD_PERIOD_MS));

	return 0;
#undef ZT_STAMP
}

static int zt7554_ts_remove(struct i2c_client *client)
{
	struct zt7554_ts_data *ts = i2c_get_clientdata(client);

	mutex_lock(&ts->lock);
	ts->removing = true;
	mutex_unlock(&ts->lock);

	device_remove_file(&client->dev, &dev_attr_stats);
	if (ts->irq > 0)
		devm_free_irq(&client->dev, ts->irq, ts);
	cancel_delayed_work_sync(&ts->wd_work);
	cancel_delayed_work_sync(&ts->poll_work);
	if (!IS_ERR_OR_NULL(ts->reg_vdd))
		regulator_put(ts->reg_vdd);
	if (!IS_ERR_OR_NULL(ts->reg_avdd))
		regulator_put(ts->reg_avdd);
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
		/* the power-up sequence sleeps ~250 ms: don't hold up boot */
		.probe_type	= PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table	= zt7554_idtable,
	.probe		= zt7554_ts_probe,
	.remove		= zt7554_ts_remove,
};

module_i2c_driver(zt7554_ts_driver);

MODULE_DESCRIPTION("Zinitix ZT7554 Touchscreen Driver for SM-T560/T561");
MODULE_LICENSE("GPL v2");
