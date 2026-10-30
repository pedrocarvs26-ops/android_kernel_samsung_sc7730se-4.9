// SPDX-License-Identifier: GPL-2.0
/*
 * SC2723 fuel gauge (FGU) battery driver for SC7730SE boards
 *
 * Registers "battery", "ac" and "usb" power supplies (Samsung SM-T560):
 *  - battery voltage/current from the SC2723 FGU ADCs and capacity from
 *    the FGU coulomb counter, using the same model as the vendor 3.10
 *    sprd_fgu/sprd_battery code (efuse calibration, power-on OCV, capacity
 *    saved in the FGU user area, low-voltage corrections and rate-limited
 *    capacity smoothing),
 *  - battery temperature from the SC2723 AUXADC (NTC channel),
 *  - charger presence from the VCHG analog EIC line and the charger type
 *    (USB port or dedicated charger) from the Silicon Mitus SM5504 MUIC,
 *    which is only read,
 *  - the Summit SMB358 charger, programmed on every attach the way the
 *    vendor 3.10 kernel does it: dedicated chargers get 2000 mA fast
 *    charge behind an 1800 mA input limit with AICL, anything else runs in
 *    USB5 (500 mA) mode; 4320 mV float, lowered to 4180 mV below 10 or
 *    above 45 degC (vendor battery swelling protection), charging stopped
 *    below -5 or above 58 degC. Unlike the vendor code, the charger is not
 *    turned off at the end of charge: its own termination and auto-recharge
 *    stay enabled, and as its termination current includes the system load
 *    the end of charge is also taken from the gauge (fgu_smb358_status()).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/workqueue.h>

#include <soc/sprd/iomap.h>

/* SC2723 analog global registers/bits used here */
#define SC2723_XTL_WAIT_CTRL		SCI_ADDR(ANA_REGS_GLB_BASE, 0x00e8)
#define SC2723_ANA_FGU_EN		BIT(8)	/* ARM_MODULE_EN */
#define SC2723_ANA_EFS_EN		BIT(7)	/* ARM_MODULE_EN */
#define SC2723_ANA_ADC_EN		BIT(6)	/* ARM_MODULE_EN */
#define SC2723_CLK_AUXAD_EN		BIT(9)	/* ARM_CLK_EN */
#define SC2723_CLK_AUXADC_EN		BIT(8)	/* ARM_CLK_EN */
#define SC2723_RTC_FGU_EN		BIT(7)	/* RTC_CLK_EN */
#define SC2723_RTC_FGUA_EN		BIT(6)	/* RTC_CLK_EN */
#define SC2723_XTL_EN			BIT(8)	/* XTL_WAIT_CTRL */

/* FGU block, ADI slave + 0x500 */
#define FGU_REG(o)			SCI_ADDR(REGS_ADISLAVE_BASE, 0x0500 + (o))
#define FGU_START			FGU_REG(0x00)
#define FGU_VOLT_VAL			FGU_REG(0x20)
#define FGU_OCV_VAL			FGU_REG(0x24)
#define FGU_POCV_VAL			FGU_REG(0x28)
#define FGU_CURT_VAL			FGU_REG(0x2c)
#define FGU_CLBCNT_VALH			FGU_REG(0x68)
#define FGU_CLBCNT_VALL			FGU_REG(0x6c)
#define FGU_CLBCNT_QMAXL		FGU_REG(0x74)
#define FGU_RELAX_CURT_THRE		FGU_REG(0x80)
#define FGU_OCV_LAST_CNT		FGU_REG(0x8c)
#define FGU_CURT_OFFSET			FGU_REG(0x90)
#define FGU_USER_AREA_SET		FGU_REG(0xa0)
#define FGU_USER_AREA_CLEAR		FGU_REG(0xa4)
#define FGU_USER_AREA_STATUS		FGU_REG(0xa8)

#define FGU_VOLT_MASK			0x1fff
#define FGU_CURT_MASK			0x3fff
#define FGU_CUR_0MA			8192
#define FGU_OCV_VALID_CNT		20
/* coulomb counter counts per mAh per (1 A in current-ADC units): 3.6 * 2 Hz */
#define FGU_CC_PER_MAH_ADC		7200

#define FGU_UA_CAP_MASK			0x0fff
#define FGU_UA_TYPE_MASK		0xf000
#define FGU_UA_TYPE_SHIFT		12
#define FGU_UA_TYPE_FIRST		0xf
#define FGU_UA_TYPE_NORMAL		0x5
#define FGU_UA_CAP_INVALID		0xfff

/* uncalibrated defaults from the vendor driver */
#define FGU_DEF_NV4200			2752
#define FGU_DEF_NV3600			2374
#define FGU_DEF_ZERO			8338

/* SC2723 analog efuse, ADI slave + 0x200 */
#define EFS_REG(o)			SCI_ADDR(REGS_ADISLAVE_BASE, 0x0200 + (o))
#define EFS_DATA_RD			EFS_REG(0x04)
#define EFS_BLOCK_INDEX			EFS_REG(0x0c)
#define EFS_MODE_CTRL			EFS_REG(0x10)
#define EFS_RD_START			BIT(1)
#define EFS_STATUS			EFS_REG(0x14)
#define EFS_READ_BUSY			BIT(1)

/* SC2723 AUXADC, ADI slave + 0x300 */
#define ADC_REG(o)			SCI_ADDR(ANA_ADC_BASE, (o))
#define ADC_CTL				ADC_REG(0x00)
#define ADC_EN				BIT(0)
#define ADC_SW_CH_ON			BIT(1)
#define ADC_12BIT			BIT(2)
#define ADC_RUN_NUM(n)			((((n) - 1) & 0xf) << 4)
#define ADC_SW_CH_CFG			ADC_REG(0x04)
#define ADC_CH_ID(x)			((x) & 0x1f)
#define ADC_DAT				ADC_REG(0x4c)
#define ADC_IRQ_EN			ADC_REG(0x50)
#define ADC_IRQ_CLR			ADC_REG(0x54)
#define ADC_IRQ_RAW			ADC_REG(0x5c)
#define ADC_IRQ_BIT			BIT(0)
#define ADC_SAMPLES			15
#define ADC_DROP			3

/* SMB358 charger */
#define SMB358_CHG_CURRENT		0x00
#define SMB358_INPUT_LIMIT		0x01
#define SMB358_VARIOUS			0x02
#define SMB358_FLOAT_VOLTAGE		0x03
#define SMB358_CHG_CTRL			0x04
#define SMB358_PIN_CTRL			0x06
#define SMB358_THERM_A			0x07
#define SMB358_OTHER_A			0x09
#define SMB358_CFG_LAST			0x0d
#define SMB358_CMD_A			0x30
#define SMB358_CMD_B			0x31
#define SMB358_INT_STATUS_C		0x37
#define SMB358_STATUS_C			0x3d
#define SMB358_STATUS_E			0x3f
#define SMB358_STATUS_LAST		0x3f

/* CMD_A: bit 7 allows config writes, bit 1 enables charging */
#define SMB358_CMD_A_VOLATILE_WR	BIT(7)
#define SMB358_CMD_A_ON			0xc2
#define SMB358_CMD_A_OFF		0xc0
#define SMB358_CMD_A_HANDOVER		0x82	/* vendor shutdown value */
#define SMB358_CMD_B_MODE		0x03
#define SMB358_CMD_B_HC			0x03	/* INPUT_LIMIT applies */
#define SMB358_CMD_B_USB5		0x02	/* 500 mA input limit */
#define SMB358_STAT_E_AICL_DONE		BIT(4)
#define SMB358_STAT_E_AICL_MASK		0x0f
#define SMB358_STAT_C_EN		BIT(0)	/* charging enabled */
#define SMB358_STAT_C_CHG_MASK		0x06	/* pre, fast, taper */
#define SMB358_STAT_C_TAPER		0x06
#define SMB358_STAT_C_TERM		BIT(5)	/* a charge cycle terminated */
#define SMB358_INT_C_TERM		0x03	/* termination current hit */

/* values the vendor driver writes on attach */
#define SMB358_CUR_DCP			0xff	/* 2000 mA fast, 200 mA term */
#define SMB358_CUR_USB			0x5f	/* 600 mA fast, 200 mA term */
#define SMB358_INLIM_DCP		0x60	/* 1800 mA */
#define SMB358_INLIM_USB		0x20	/* 700 mA, capped by USB5 */
#define SMB358_INLIM_MASK		0xf0
#define SMB358_VARIOUS_AICL_OFF		0x81
#define SMB358_VARIOUS_AICL_ON		0x95
#define SMB358_FLOAT_NORMAL		0x29	/* 4320 mV */
#define SMB358_FLOAT_SWELLING		0x22	/* 4180 mV */
#define SMB358_PIN_CTRL_I2C		0x09	/* charge enable from CMD_A */
/* THERM_A bit 6 clear + OTHER_A bit 4 set: 3.15 V minimum system voltage */
#define SMB358_THERM_A_MINSYS		BIT(6)
#define SMB358_OTHER_A_MINSYS		BIT(4)
#define SMB358_THERM_A_DEFAULT		0xf0
/* vendor: 0xc0, no termination or auto-recharge (done in software) */
#define SMB358_CHG_CTRL_HW_TERM		0x00

/* SM5504 MUIC (read only) */
#define SM5504_DEVICE_ID		0x01
#define SM5504_ADC			0x07
#define SM5504_DEV_T1			0x0a
#define SM5504_DEV_T2			0x0b
#define SM5504_CHG_TYPE			0x24
#define SM5504_VENDOR_MASK		0x07
#define SM5504_VENDOR_ID		0x01
#define SM5504_T1_DCP			BIT(6)
#define SM5504_T1_CDP			BIT(5)
#define SM5504_T1_SDP			BIT(2)
#define SM5504_CT_DCP			BIT(0)
#define SM5504_CT_CDP			BIT(1)
#define SM5504_CT_SDP			BIT(2)
#define SM5504_CT_SDP_TIMEOUT		BIT(3)

#define FGU_I2C_MAX_TRIES		10

/* temperatures in 0.1 degC */
#define FGU_TEMP_DEFAULT		250
#define FGU_TEMP_HOT			580
#define FGU_TEMP_HOT_RECOVER		530
#define FGU_TEMP_COLD			(-50)
#define FGU_TEMP_COLD_RECOVER		0
/* vendor CONFIG_BATTERY_SWELLING: float voltage lowered outside 10..45 degC */
#define FGU_SWELL_HOT			450
#define FGU_SWELL_HOT_RECOVER		400
#define FGU_SWELL_COLD			100
#define FGU_SWELL_COLD_RECOVER		150
/*
 * Keep the reported value below Android's 68 degC emergency shutdown so a
 * bad NTC reading cannot shut the tablet down while this port is young;
 * overheat is still reported through POWER_SUPPLY_PROP_HEALTH.
 */
#define FGU_TEMP_REPORT_MAX		650
#define FGU_TEMP_ADC_MIN		100
#define FGU_TEMP_ADC_MAX		4000

/* capacity smoothing: seconds per 1% step (vendor ONE_PERCENT_TIME) */
#define FGU_STEP_S_DISCHG		40
#define FGU_STEP_S_CHG			60
#define FGU_LOW_DEBOUNCE		3
#define FGU_STALE_SAVED_DIFF		30

#define FGU_POLL			msecs_to_jiffies(20000)
#define FGU_POLL_FAST			msecs_to_jiffies(10000)
#define FGU_VCHG_DEBOUNCE		msecs_to_jiffies(300)
#define FGU_LOG_EVERY			15
/* charger type detection: first MUIC read after VBUS, then retries */
#define FGU_DET_DELAY			msecs_to_jiffies(700)
#define FGU_DET_RETRY			msecs_to_jiffies(500)
#define FGU_DET_TRIES			8
#define FGU_AICL_DELAY			msecs_to_jiffies(2000)
#define FGU_AICL_RETRY			msecs_to_jiffies(1000)
#define FGU_AICL_TRIES			10	/* restarted halfway */
#define FGU_CFG_MAX_FAILS		3
#define FGU_CHG_CHECK_EVERY		3	/* polls */

/* end of charge from the gauge, see fgu_smb358_status() */
#define FGU_FULL_MA			200	/* vendor first full check */
#define FGU_FULL_MARGIN_MV		50	/* below the float voltage */
#define FGU_FULL_EXIT_MV		70
#define FGU_FULL_S			60

#define FGU_DCP_INPUT_UA		1800000
#define FGU_USB_INPUT_UA		500000
#define FGU_VBUS_UV			5000000

enum fgu_cable {
	FGU_CABLE_NONE,
	FGU_CABLE_PENDING,	/* VBUS seen, MUIC detection running */
	FGU_CABLE_SDP,
	FGU_CABLE_CDP,
	FGU_CABLE_DCP,
	FGU_CABLE_OTHER,	/* unknown source, handled like a USB port */
};

static const char * const fgu_cable_str[] = {
	[FGU_CABLE_NONE]	= "none",
	[FGU_CABLE_PENDING]	= "pending",
	[FGU_CABLE_SDP]		= "usb",
	[FGU_CABLE_CDP]		= "usb-cdp",
	[FGU_CABLE_DCP]		= "dcp",
	[FGU_CABLE_OTHER]	= "unknown",
};

struct fgu_tab {
	s32 x;
	s32 y;
};

struct sc2723_fgu {
	struct device *dev;
	struct mutex lock;
	struct delayed_work work;
	struct power_supply *bat;
	struct power_supply *ac;
	struct power_supply *usb;
	struct delayed_work chg_work;

	struct gpio_desc *vchg;
	int vchg_irq;
	unsigned long irq_win;
	unsigned int irq_cnt;
	bool irq_storm;
	bool irq_storm_logged;

	struct device_node *chg_np;
	struct i2c_client *chg;
	unsigned int chg_tries;
	unsigned int chg_errs;
	int chg_stat_c;
	struct device_node *muic_np;
	struct i2c_client *muic;
	unsigned int muic_tries;

	/* charger control, see fgu_chg_work() */
	int cable;
	unsigned int det_tries;
	unsigned int cfg_fails;
	unsigned int aicl_tries;
	bool chg_queued;	/* chg_work queued by us */
	bool chg_cfg;		/* SMB358 set up for the current cable */
	bool chg_idle;		/* SMB358 back in USB5 mode, no cable */
	bool aicl_pending;
	bool chg_block;		/* charging stopped, battery too hot/cold */
	bool swelling;		/* float voltage lowered */
	bool sw_full;		/* end of charge seen by the gauge */
	long taper_s;		/* taper at low current since, or 0 */

	/* board data */
	int cnom;		/* mAh */
	int rint;		/* mOhm */
	int rsense_real;
	int rsense_spec;
	int relax_ma;
	int alm_mv;
	int uvlo_mv;
	int temp_ch;
	struct fgu_tab *ocv_tab;
	int ocv_len;
	struct fgu_tab *temp_tab;
	int temp_len;

	/* calibration */
	int vol_1000mv_adc;
	int vol_offset;
	int cur_1000ma_adc;
	int cur_offset;

	/* gauge model */
	int init_cap;
	s32 init_clbcnt;
	int warning_cap;
	int full_mv;
	int shutdown_mv;
	s32 avg_cc;
	ktime_t avg_t;

	/* reported state */
	int online;
	int status;
	int health;
	int ui_cap;
	int raw_cap;
	int charge_uah;
	int vbat_mv;
	int ocv_mv;
	bool ocv_hw;
	int cur_ma;
	int cur_avg_ma;
	int temp;
	int temp_raw;

	long cap_change_s;
	long last_query_s;
	int low_cnt;
	unsigned int polls;
};

static char *fgu_supplied_to[] = { "battery" };

static inline int fgu_rd(unsigned long reg)
{
	return sci_adi_read(reg) & 0xffff;
}

static long fgu_now_s(void)
{
	return (long)ktime_divns(ktime_get_boottime(), NSEC_PER_SEC);
}

static int fgu_adc2vol(struct sc2723_fgu *f, int adc)
{
	return (adc + f->vol_offset) * 1000 / f->vol_1000mv_adc;
}

static int fgu_adc2cur(struct sc2723_fgu *f, int adc)
{
	return adc * 1000 / f->cur_1000ma_adc;
}

static int fgu_cur2adc(struct sc2723_fgu *f, int ma)
{
	return ma * f->cur_1000ma_adc / 1000;
}

/* linear interpolation over a table that is monotonic in x */
static int fgu_interp(const struct fgu_tab *t, int n, int x)
{
	bool desc = t[0].x > t[n - 1].x;
	int i;

	if (desc ? x >= t[0].x : x <= t[0].x)
		return t[0].y;
	if (desc ? x <= t[n - 1].x : x >= t[n - 1].x)
		return t[n - 1].y;

	for (i = 1; i < n - 1; i++)
		if (desc ? x >= t[i].x : x <= t[i].x)
			break;

	return t[i - 1].y + (x - t[i - 1].x) * (t[i].y - t[i - 1].y) /
			    (t[i].x - t[i - 1].x);
}

static int fgu_vol2cap(struct sc2723_fgu *f, int mv)
{
	return fgu_interp(f->ocv_tab, f->ocv_len, mv);
}

/* ---- analog efuse ---- */

static int fgu_efuse_block(int blk)
{
	unsigned long timeout;
	int val = -ETIMEDOUT;

	sci_adi_set(ANA_REG_GLB_ARM_MODULE_EN, SC2723_ANA_EFS_EN);
	sci_adi_raw_write(EFS_BLOCK_INDEX, blk & 0x1f);
	sci_adi_raw_write(EFS_MODE_CTRL, EFS_RD_START);
	timeout = jiffies + msecs_to_jiffies(50);
	do {
		if (!(fgu_rd(EFS_STATUS) & EFS_READ_BUSY)) {
			val = ~fgu_rd(EFS_DATA_RD) & 0xff;
			break;
		}
		udelay(10);
	} while (time_before(jiffies, timeout));
	sci_adi_clr(ANA_REG_GLB_ARM_MODULE_EN, SC2723_ANA_EFS_EN);

	return val;
}

static int fgu_efuse_bits(int bit, int len, u32 *out)
{
	int first = bit / 8, last = DIV_ROUND_UP(bit + len, 8) - 1, i, b;
	u32 v = 0;

	for (i = first; i <= last; i++) {
		b = fgu_efuse_block(i);
		if (b < 0)
			return b;
		v |= (u32)b << ((i - first) * 8);
	}
	*out = (v >> (bit & 7)) & (BIT(len) - 1);

	return 0;
}

/* ---- calibration ---- */

static void fgu_cal_apply(struct sc2723_fgu *f, int nv4200, int nv3600,
			  int zero)
{
	f->vol_1000mv_adc = DIV_ROUND_CLOSEST((nv4200 - nv3600) * 10, 6);
	f->vol_offset = -(nv4200 * 10 - f->vol_1000mv_adc * 42) / 10;
	f->cur_offset = FGU_CUR_0MA - zero;
	f->cur_1000ma_adc = DIV_ROUND_CLOSEST(f->vol_1000mv_adc * 4 *
					      f->rsense_real, f->rsense_spec);
}

static void fgu_cal_init(struct sc2723_fgu *f)
{
	int b0, p0, p1, zero, vraw, mv;
	const char *src = "default";
	u32 a, b, c;

	fgu_cal_apply(f, FGU_DEF_NV4200, FGU_DEF_NV3600, FGU_DEF_ZERO);

	b0 = fgu_efuse_block(0);
	if (b0 < 0) {
		dev_warn(f->dev, "efuse: read timeout, using defaults\n");
	} else if (b0 & BIT(7)) {
		dev_info(f->dev, "efuse: FGU not calibrated (blk0 %#x)\n", b0);
	} else if (!fgu_efuse_bits(12 * 8 + 0, 9, &a) &&
		   !fgu_efuse_bits(14 * 8 + 2, 6, &b) &&
		   !fgu_efuse_bits(13 * 8 + 1, 9, &c)) {
		p0 = a + 6963 - 4096 - 256;
		p1 = p0 - 410 - b + 16;
		zero = (c & 0x1ff) + 8192 - 256;
		dev_info(f->dev,
			 "efuse: blk0 %#x raw %u/%u/%u -> 4200mV@%d 3600mV@%d 0mA@%d\n",
			 b0, a, b, c, p0, p1, zero);

		fgu_cal_apply(f, p0, p1, zero);
		vraw = fgu_rd(FGU_VOLT_VAL) & FGU_VOLT_MASK;
		mv = vraw ? fgu_adc2vol(f, vraw) : 0;
		if (mv >= 2800 && mv <= 4600) {
			src = "efuse";
		} else {
			dev_warn(f->dev,
				 "efuse: implausible vbat %dmV (raw %#x), using defaults\n",
				 mv, vraw);
			fgu_cal_apply(f, FGU_DEF_NV4200, FGU_DEF_NV3600,
				      FGU_DEF_ZERO);
		}
	} else {
		dev_warn(f->dev, "efuse: bit read timeout, using defaults\n");
	}

	dev_info(f->dev,
		 "cal(%s): vol_1000mv_adc=%d vol_offset=%d cur_1000ma_adc=%d cur_offset=%d hw_curt_offset=%#x\n",
		 src, f->vol_1000mv_adc, f->vol_offset, f->cur_1000ma_adc,
		 f->cur_offset, fgu_rd(FGU_CURT_OFFSET));
}

/* ---- AUXADC ---- */

static int fgu_cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int fgu_adc_read(int ch, int *out)
{
	int buf[ADC_SAMPLES], i, sum = 0, ret = 0;

	sci_adi_raw_write(ADC_SW_CH_CFG, ADC_CH_ID(ch));
	sci_adi_raw_write(ADC_CTL, fgu_rd(ADC_CTL) &
			  ~(ADC_EN | ADC_SW_CH_ON | ADC_12BIT));
	sci_adi_raw_write(ADC_IRQ_CLR, ADC_IRQ_BIT);
	sci_adi_raw_write(ADC_CTL, ADC_RUN_NUM(ADC_SAMPLES) | ADC_EN |
			  ADC_12BIT | ADC_SW_CH_ON);

	for (i = 0; i < 100; i++) {
		if (fgu_rd(ADC_IRQ_RAW) & ADC_IRQ_BIT)
			break;
		udelay(50);
	}
	if (i == 100) {
		ret = -ETIMEDOUT;
		goto out;
	}

	for (i = 0; i < ADC_SAMPLES; i++)
		buf[i] = fgu_rd(ADC_DAT) & 0xfff;
	sort(buf, ADC_SAMPLES, sizeof(int), fgu_cmp_int, NULL);
	for (i = ADC_DROP; i < ADC_SAMPLES - ADC_DROP; i++)
		sum += buf[i];
	*out = sum / (ADC_SAMPLES - 2 * ADC_DROP);
out:
	sci_adi_raw_write(ADC_CTL, fgu_rd(ADC_CTL) & ~ADC_EN);
	return ret;
}

static void fgu_read_temp(struct sc2723_fgu *f)
{
	int raw;

	if (!f->temp_tab)
		return;

	if (fgu_adc_read(f->temp_ch, &raw)) {
		dev_warn_ratelimited(f->dev, "temp: adc timeout\n");
		return;
	}
	f->temp_raw = raw;
	if (raw < FGU_TEMP_ADC_MIN || raw > FGU_TEMP_ADC_MAX) {
		dev_warn_ratelimited(f->dev, "temp: adc %d out of range\n",
				     raw);
		return;
	}
	f->temp = fgu_interp(f->temp_tab, f->temp_len, raw);
}

static int fgu_health(struct sc2723_fgu *f)
{
	if (f->health == POWER_SUPPLY_HEALTH_OVERHEAT &&
	    f->temp >= FGU_TEMP_HOT_RECOVER)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (f->health == POWER_SUPPLY_HEALTH_COLD &&
	    f->temp <= FGU_TEMP_COLD_RECOVER)
		return POWER_SUPPLY_HEALTH_COLD;
	if (f->temp >= FGU_TEMP_HOT)
		return POWER_SUPPLY_HEALTH_OVERHEAT;
	if (f->temp <= FGU_TEMP_COLD)
		return POWER_SUPPLY_HEALTH_COLD;
	return POWER_SUPPLY_HEALTH_GOOD;
}

/* ---- FGU ---- */

static s32 fgu_clbcnt(void)
{
	u32 a, b;
	int tries = 5;

	do {
		a = fgu_rd(FGU_CLBCNT_VALL) | (fgu_rd(FGU_CLBCNT_VALH) << 16);
		b = fgu_rd(FGU_CLBCNT_VALL) | (fgu_rd(FGU_CLBCNT_VALH) << 16);
	} while (a != b && --tries);

	return (s32)b;
}

/* FGU user area: SET/CLEAR are write-1 registers, STATUS reads back */
static void fgu_ua_write(u16 mask, u16 val)
{
	sci_adi_raw_write(FGU_USER_AREA_CLEAR, ~val & mask);
	sci_adi_raw_write(FGU_USER_AREA_SET, val & mask);
}

static void fgu_save_cap(int cap)
{
	fgu_ua_write(FGU_UA_CAP_MASK, cap);
}

static int fgu_ocv(struct sc2723_fgu *f, int vbat, int cur)
{
	int cnt = fgu_rd(FGU_OCV_LAST_CNT);
	int raw = fgu_rd(FGU_OCV_VAL) & FGU_VOLT_MASK;

	f->ocv_hw = cnt <= FGU_OCV_VALID_CNT && raw;
	if (f->ocv_hw)
		return fgu_adc2vol(f, raw);
	return vbat - cur * f->rint / 1000;
}

static void fgu_adjust(struct sc2723_fgu *f, int cap, s32 cc)
{
	f->init_cap = cap;
	f->init_clbcnt = cc;
}

/* vendor sprdfgu_read_capacity() */
static int fgu_soc(struct sc2723_fgu *f, s32 cc, int ocv, bool chg_full)
{
	int mah = fgu_adc2cur(f, DIV_ROUND_CLOSEST(cc - f->init_clbcnt,
						   FGU_CC_PER_MAH_ADC));
	int cap = f->init_cap + DIV_ROUND_CLOSEST(mah * 100, f->cnom);

	f->charge_uah = clamp(f->init_cap * f->cnom * 10 + mah * 1000, 0,
			      f->cnom * 1000);

	if (ocv >= f->full_mv && (cap < 100 || cap > 102)) {
		cap = 100;
		fgu_adjust(f, cap, cc);
	}
	if (chg_full && cap < 100)
		cap = 101;
	if (cap > 100) {
		cap = 100;
		fgu_adjust(f, cap, cc);
	}

	if (cap <= f->warning_cap && ocv > f->alm_mv) {
		cap = f->warning_cap + 1;
		fgu_adjust(f, cap, cc);
	} else if (cap <= 0 && ocv > f->shutdown_mv) {
		cap = 1;
	} else if (ocv < f->shutdown_mv) {
		cap = 0;
	} else if (cap > f->warning_cap && ocv < f->alm_mv) {
		cap = fgu_vol2cap(f, ocv);
		fgu_adjust(f, cap, cc);
	}

	return cap;
}

/* vendor sprdbat_update_capacty(): move the shown value 1% per step time */
static int fgu_smooth(struct sc2723_fgu *f, int cap, int status, int cur,
		      long now)
{
	long flush = now - f->cap_change_s;
	long period = now - f->last_query_s;
	int ui = f->ui_cap, step, t;

	f->last_query_s = now;

	if (status == POWER_SUPPLY_STATUS_FULL)
		return 100;

	if (status == POWER_SUPPLY_STATUS_CHARGING) {
		t = FGU_STEP_S_CHG;
		if (ui >= 100)
			return 100;
		if (cap < ui && cur > 0)
			return ui;
	} else {
		t = FGU_STEP_S_DISCHG;
		if (cap > ui)
			return ui;
	}

	step = flush / t;
	if (period <= t)
		step = min(step, 1);

	return clamp(cap, ui - step, ui + step);
}

/* ---- SMB358 charger, SM5504 MUIC ---- */

static const u16 smb358_fast_ma[] = {
	200, 450, 600, 900, 1300, 1500, 1800, 2000
};
static const u16 smb358_term_ma[] = {
	30, 40, 60, 80, 100, 125, 150, 200
};
static const u16 smb358_inlim_ma[] = {
	300, 500, 700, 1000, 1200, 1500, 1800, 2000
};
static const u16 smb358_aicl_ma[] = {
	300, 500, 700, 1000, 1200, 1300, 1800, 2000
};

static int smb358_float_mv(u8 v)
{
	v &= 0x3f;
	if (v <= 0x2a)
		return 3500 + 20 * v;
	if (v == 0x2b)
		return 4350;
	return 3500 + 20 * (v - 1);
}

static void smb358_dump(struct sc2723_fgu *f, bool config)
{
	u8 regs[16];
	int i, v;

	if (config) {
		for (i = 0; i <= SMB358_CFG_LAST; i++) {
			v = i2c_smbus_read_byte_data(f->chg, i);
			regs[i] = v < 0 ? 0xee : v;
		}
		dev_info(f->dev, "smb358: cfg[00..0d] %*ph\n",
			 SMB358_CFG_LAST + 1, regs);
		dev_info(f->dev,
			 "smb358: float %dmV fast %dmA term %dmA input-limit %dmA\n",
			 smb358_float_mv(regs[3]), smb358_fast_ma[regs[0] >> 5],
			 smb358_term_ma[regs[0] & 7],
			 smb358_inlim_ma[min(regs[1] >> 4, 7)]);
	}

	for (i = 0; i <= SMB358_STATUS_LAST - SMB358_CMD_A; i++) {
		v = i2c_smbus_read_byte_data(f->chg, SMB358_CMD_A + i);
		regs[i] = v < 0 ? 0xee : v;
	}
	dev_info(f->dev, "smb358: cmd/status[30..3f] %*ph\n",
		 SMB358_STATUS_LAST - SMB358_CMD_A + 1, regs);
}

static struct i2c_client *fgu_i2c_get(struct sc2723_fgu *f,
				      struct device_node *np,
				      unsigned int *tries, const char *name,
				      u8 reg, int *val)
{
	struct i2c_client *c;

	if (!np || *tries >= FGU_I2C_MAX_TRIES)
		return NULL;

	(*tries)++;
	c = of_find_i2c_device_by_node(np);
	if (!c) {
		if (*tries == FGU_I2C_MAX_TRIES)
			dev_warn(f->dev, "%s: no i2c client\n", name);
		return NULL;
	}

	*val = i2c_smbus_read_byte_data(c, reg);
	if (*val < 0) {
		dev_warn(f->dev, "%s: %s not responding (try %u)\n", name,
			 dev_name(&c->dev), *tries);
		put_device(&c->dev);
		return NULL;
	}

	return c;
}

static void fgu_charger_attach(struct sc2723_fgu *f)
{
	int v = 0;

	if (!f->chg) {
		f->chg = fgu_i2c_get(f, f->chg_np, &f->chg_tries, "smb358",
				     SMB358_STATUS_C, &v);
		if (f->chg) {
			dev_info(f->dev, "smb358: using %s\n",
				 dev_name(&f->chg->dev));
			smb358_dump(f, true);
		}
	}

	if (!f->muic) {
		f->muic = fgu_i2c_get(f, f->muic_np, &f->muic_tries, "sm5504",
				      SM5504_DEVICE_ID, &v);
		if (f->muic && (v & SM5504_VENDOR_MASK) != SM5504_VENDOR_ID) {
			dev_warn(f->dev, "sm5504: unexpected device id %#04x\n",
				 v);
			put_device(&f->muic->dev);
			f->muic = NULL;
			f->muic_tries = FGU_I2C_MAX_TRIES;
		} else if (f->muic) {
			dev_info(f->dev, "sm5504: using %s (device id %#04x)\n",
				 dev_name(&f->muic->dev), v);
		}
	}
}

struct smb358_reg_val {
	u8 reg;
	u8 val;
};

static int smb358_write(struct sc2723_fgu *f, u8 reg, u8 val)
{
	int ret = i2c_smbus_write_byte_data(f->chg, reg, val);

	if (ret < 0)
		dev_warn(f->dev, "smb358: writing %02x to reg %02x failed: %d\n",
			 val, reg, ret);
	return ret;
}

/* read-modify-write, skipped if nothing changes */
static int smb358_update(struct sc2723_fgu *f, u8 reg, u8 mask, u8 val)
{
	int v = i2c_smbus_read_byte_data(f->chg, reg);

	if (v < 0)
		return v;
	val = (v & ~mask) | (val & mask);
	return val == v ? 0 : smb358_write(f, reg, val);
}

static bool fgu_is_dcp(struct sc2723_fgu *f)
{
	return f->cable == FGU_CABLE_DCP;
}

static u8 smb358_want_mode(struct sc2723_fgu *f)
{
	return fgu_is_dcp(f) ? SMB358_CMD_B_HC : SMB358_CMD_B_USB5;
}

static u8 smb358_want_cur(struct sc2723_fgu *f)
{
	return fgu_is_dcp(f) ? SMB358_CUR_DCP : SMB358_CUR_USB;
}

static u8 smb358_want_float(struct sc2723_fgu *f)
{
	return f->swelling ? SMB358_FLOAT_SWELLING : SMB358_FLOAT_NORMAL;
}

static u8 smb358_want_cmd_a(struct sc2723_fgu *f)
{
	return f->chg_block ? SMB358_CMD_A_OFF : SMB358_CMD_A_ON;
}

/* no cable: back to USB5 so that the next attach starts at 500 mA */
static void smb358_idle(struct sc2723_fgu *f)
{
	f->chg_idle = !smb358_update(f, SMB358_CMD_B, SMB358_CMD_B_MODE,
				     SMB358_CMD_B_USB5);
}

/*
 * Same register sequence as the attach path of the vendor smb358_charger.c,
 * minus its software end-of-charge handling and the safety timer and
 * thermal regulation writes, which keep their power-on values here.
 */
static void smb358_configure(struct sc2723_fgu *f)
{
	bool dcp = fgu_is_dcp(f);
	u8 mode = smb358_want_mode(f), cur = smb358_want_cur(f);
	u8 inlim = dcp ? SMB358_INLIM_DCP : SMB358_INLIM_USB;
	int therm = i2c_smbus_read_byte_data(f->chg, SMB358_THERM_A);
	int other = i2c_smbus_read_byte_data(f->chg, SMB358_OTHER_A);
	const struct smb358_reg_val seq[] = {
		/* charging off while reprogramming, config writes allowed */
		{ SMB358_CMD_A,		SMB358_CMD_A_OFF },
		{ SMB358_CMD_B,		mode },
		{ SMB358_CHG_CURRENT,	cur },
		{ SMB358_PIN_CTRL,	SMB358_PIN_CTRL_I2C },
		{ SMB358_INPUT_LIMIT,	inlim },
		/* AICL off and on again restarts input current detection */
		{ SMB358_VARIOUS,	SMB358_VARIOUS_AICL_OFF },
		{ SMB358_VARIOUS,	SMB358_VARIOUS_AICL_ON },
		{ SMB358_FLOAT_VOLTAGE,	smb358_want_float(f) },
		{ SMB358_CHG_CTRL,	SMB358_CHG_CTRL_HW_TERM },
		{ SMB358_THERM_A,	therm & ~SMB358_THERM_A_MINSYS },
		{ SMB358_OTHER_A,	other | SMB358_OTHER_A_MINSYS },
		{ SMB358_CMD_A,		smb358_want_cmd_a(f) },
	};
	int i, rb_cur, rb_inlim, rb_mode, errs = 0;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		if ((seq[i].reg == SMB358_THERM_A && therm < 0) ||
		    (seq[i].reg == SMB358_OTHER_A && other < 0)) {
			errs++;
			continue;
		}
		errs += smb358_write(f, seq[i].reg, seq[i].val) < 0;
	}

	rb_cur = i2c_smbus_read_byte_data(f->chg, SMB358_CHG_CURRENT);
	rb_inlim = i2c_smbus_read_byte_data(f->chg, SMB358_INPUT_LIMIT);
	rb_mode = i2c_smbus_read_byte_data(f->chg, SMB358_CMD_B);
	if (errs || rb_cur != cur || rb_inlim < 0 ||
	    (rb_inlim & SMB358_INLIM_MASK) != inlim || rb_mode < 0 ||
	    (rb_mode & SMB358_CMD_B_MODE) != mode) {
		f->cfg_fails++;
		dev_warn(f->dev,
			 "smb358: %s setup failed (%d errors, read back %d %d %d), attempt %u\n",
			 fgu_cable_str[f->cable], errs, rb_cur, rb_inlim,
			 rb_mode, f->cfg_fails);
		smb358_dump(f, true);
		return;
	}

	f->chg_cfg = true;
	dev_info(f->dev, "smb358: %s setup: fast %dmA, input %dmA (%s), float %dmV%s\n",
		 fgu_cable_str[f->cable], smb358_fast_ma[cur >> 5],
		 dcp ? smb358_inlim_ma[inlim >> 4] : 500,
		 dcp ? "aicl" : "usb5",
		 smb358_float_mv(smb358_want_float(f)),
		 f->chg_block ? ", charging stopped (temperature)" : "");
	smb358_dump(f, true);
}

/* temperature limits, applied on every poll while a cable is present */
static void smb358_apply_limits(struct sc2723_fgu *f)
{
	smb358_update(f, SMB358_FLOAT_VOLTAGE, 0xff, smb358_want_float(f));
	smb358_update(f, SMB358_CMD_A, 0xff, smb358_want_cmd_a(f));
}

/* returns false while the AICL result should be checked again */
static bool smb358_aicl(struct sc2723_fgu *f)
{
	int e = i2c_smbus_read_byte_data(f->chg, SMB358_STATUS_E);

	f->aicl_tries++;
	if (e >= 0 && (e & SMB358_STAT_E_AICL_DONE)) {
		dev_info(f->dev, "smb358: aicl done, input limit %dmA (status_e %02x, check %u)\n",
			 smb358_aicl_ma[min(e & SMB358_STAT_E_AICL_MASK, 7)],
			 e, f->aicl_tries);
		return true;
	}
	if (f->aicl_tries >= FGU_AICL_TRIES) {
		dev_warn(f->dev, "smb358: aicl not done (status_e %d)\n", e);
		smb358_dump(f, false);
		return true;
	}
	if (f->aicl_tries == FGU_AICL_TRIES / 2) {
		dev_info(f->dev, "smb358: aicl not done (status_e %d), restarting it\n",
			 e);
		smb358_write(f, SMB358_VARIOUS, SMB358_VARIOUS_AICL_OFF);
		smb358_write(f, SMB358_VARIOUS, SMB358_VARIOUS_AICL_ON);
	}
	return false;
}

static int fgu_muic_detect(struct sc2723_fgu *f)
{
	bool last = ++f->det_tries >= FGU_DET_TRIES;
	int t1, t2, adc, ct, cable;

	if (!f->muic)
		fgu_charger_attach(f);
	if (!f->muic) {
		if (!last && f->muic_np && f->muic_tries < FGU_I2C_MAX_TRIES)
			return FGU_CABLE_PENDING;
		dev_info(f->dev, "cable: no MUIC, assuming a USB port\n");
		return FGU_CABLE_OTHER;
	}

	t1 = i2c_smbus_read_byte_data(f->muic, SM5504_DEV_T1);
	t2 = i2c_smbus_read_byte_data(f->muic, SM5504_DEV_T2);
	adc = i2c_smbus_read_byte_data(f->muic, SM5504_ADC);
	ct = i2c_smbus_read_byte_data(f->muic, SM5504_CHG_TYPE);
	if (t1 < 0 || t2 < 0 || adc < 0 || ct < 0) {
		dev_warn(f->dev, "sm5504: read failed (%d %d %d %d)\n",
			 t1, t2, adc, ct);
		return last ? FGU_CABLE_OTHER : FGU_CABLE_PENDING;
	}

	/* any sign of a data port wins: never pull more than 500 mA there */
	if ((t1 & SM5504_T1_CDP) || (ct & SM5504_CT_CDP))
		cable = FGU_CABLE_CDP;
	else if ((t1 & SM5504_T1_SDP) ||
		 (ct & (SM5504_CT_SDP | SM5504_CT_SDP_TIMEOUT)))
		cable = FGU_CABLE_SDP;
	else if ((t1 & SM5504_T1_DCP) || (ct & SM5504_CT_DCP))
		cable = FGU_CABLE_DCP;
	else if (last)
		cable = FGU_CABLE_OTHER;
	else
		return FGU_CABLE_PENDING;

	dev_info(f->dev, "cable: %s (dev_t1 %02x dev_t2 %02x adc %02x chg_type %02x, try %u)\n",
		 fgu_cable_str[cable], t1, t2, adc, ct, f->det_tries);
	return cable;
}

/*
 * The SMB358 ends a charge cycle on its output current, which includes the
 * system load. With the tablet kept awake while charging (see
 * fgu_cable_start()) that may never drop below the 200 mA termination
 * current, leaving the battery at the float voltage, "charging", for hours.
 * So, like the vendor code (200 mA first full check), the end of charge is
 * also taken from the gauge: taper phase at the float voltage with less
 * than FGU_FULL_MA going into the battery for FGU_FULL_S. The charger keeps
 * floating the battery; "full" lasts until the cable is removed or the
 * battery is drawn down below the float voltage.
 *
 * With the float voltage lowered or charging stopped for temperature, the
 * end of charge does not mean a full battery: report "not charging" then,
 * which also keeps fgu_soc() from re-anchoring the gauge at 100%.
 */
static int fgu_smb358_status(struct sc2723_fgu *f, int c, int ic)
{
	bool limited = f->swelling || f->chg_block;
	bool done = (c & SMB358_STAT_C_TERM) || (ic & SMB358_INT_C_TERM);
	bool charging = (c & SMB358_STAT_C_EN) && (c & SMB358_STAT_C_CHG_MASK);
	int float_mv = smb358_float_mv(smb358_want_float(f));
	long now = fgu_now_s();
	bool end;

	if (f->sw_full &&
	    (limited || f->vbat_mv < float_mv - FGU_FULL_EXIT_MV)) {
		f->sw_full = false;
		dev_info(f->dev, "charger: not full anymore, vbat %dmV temp %d\n",
			 f->vbat_mv, f->temp);
	}

	if (charging &&
	    (c & SMB358_STAT_C_CHG_MASK) == SMB358_STAT_C_TAPER &&
	    f->vbat_mv >= float_mv - FGU_FULL_MARGIN_MV &&
	    f->cur_avg_ma < FGU_FULL_MA) {
		if (!f->taper_s)
			f->taper_s = now;
	} else {
		f->taper_s = 0;
	}
	end = f->taper_s && now - f->taper_s >= FGU_FULL_S;

	if (limited) {
		if (done || end || !charging)
			return POWER_SUPPLY_STATUS_NOT_CHARGING;
		return POWER_SUPPLY_STATUS_CHARGING;
	}

	if (end && !f->sw_full) {
		f->sw_full = true;
		dev_info(f->dev, "charger: full, vbat %dmV avg %dmA for %lds, gauge was %d%% (shown %d%%)\n",
			 f->vbat_mv, f->cur_avg_ma, now - f->taper_s,
			 f->raw_cap, f->ui_cap);
	}
	if (done || f->sw_full)
		return POWER_SUPPLY_STATUS_FULL;
	if (charging)
		return POWER_SUPPLY_STATUS_CHARGING;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

static int fgu_charge_status(struct sc2723_fgu *f, int cur)
{
	int c, ic;

	if (!f->online) {
		f->sw_full = false;
		f->taper_s = 0;
		return POWER_SUPPLY_STATUS_DISCHARGING;
	}

	if (f->chg) {
		c = i2c_smbus_read_byte_data(f->chg, SMB358_STATUS_C);
		ic = i2c_smbus_read_byte_data(f->chg, SMB358_INT_STATUS_C);
		if (c >= 0 && ic >= 0) {
			f->chg_stat_c = c;
			return fgu_smb358_status(f, c, ic);
		}
		f->chg_stat_c = -1;
		if (f->chg_errs++ < 5)
			dev_warn(f->dev, "smb358: status read failed (%d/%d)\n",
				 c, ic);
	}

	/* no charger telemetry: best guess from the gauge */
	if (f->raw_cap >= 100)
		return POWER_SUPPLY_STATUS_FULL;
	if (cur > 0 || f->cur_avg_ma > 0)
		return POWER_SUPPLY_STATUS_CHARGING;
	return POWER_SUPPLY_STATUS_NOT_CHARGING;
}

/* ---- monitor ---- */

static const char * const fgu_status_str[] = {
	[POWER_SUPPLY_STATUS_UNKNOWN]		= "unknown",
	[POWER_SUPPLY_STATUS_CHARGING]		= "charging",
	[POWER_SUPPLY_STATUS_DISCHARGING]	= "discharging",
	[POWER_SUPPLY_STATUS_NOT_CHARGING]	= "not-charging",
	[POWER_SUPPLY_STATUS_FULL]		= "full",
};

static void fgu_log(struct sc2723_fgu *f, s32 cc)
{
	dev_info(f->dev,
		 "cap %d%% (gauge %d%%) vbat %dmV ocv %dmV%s cur %dmA avg %dmA cc %d temp %d (adc %d) %s cable %s chg %02x\n",
		 f->ui_cap, f->raw_cap, f->vbat_mv, f->ocv_mv,
		 f->ocv_hw ? "(hw)" : "", f->cur_ma, f->cur_avg_ma, cc,
		 f->temp, f->temp_raw, fgu_status_str[f->status],
		 fgu_cable_str[f->cable], f->chg_stat_c & 0xff);
}

/* vendor thresholds, see the FGU_TEMP_* and FGU_SWELL_* definitions */
static void fgu_thermal(struct sc2723_fgu *f)
{
	bool block = f->health == POWER_SUPPLY_HEALTH_OVERHEAT ||
		     f->health == POWER_SUPPLY_HEALTH_COLD;
	bool swell = f->swelling;

	if (f->temp >= FGU_SWELL_HOT || f->temp <= FGU_SWELL_COLD)
		swell = true;
	else if (f->temp <= FGU_SWELL_HOT_RECOVER &&
		 f->temp >= FGU_SWELL_COLD_RECOVER)
		swell = false;

	if (block != f->chg_block || swell != f->swelling)
		dev_info(f->dev, "charger: temp %d, charging %s, float %dmV\n",
			 f->temp, block ? "stopped" : "allowed",
			 smb358_float_mv(swell ? SMB358_FLOAT_SWELLING :
					 SMB358_FLOAT_NORMAL));
	f->chg_block = block;
	f->swelling = swell;
}

static void fgu_ext_changed(struct sc2723_fgu *f)
{
	power_supply_changed(f->ac);
	if (f->usb)
		power_supply_changed(f->usb);
}

/* caller holds f->lock */
static void fgu_chg_queue(struct sc2723_fgu *f, unsigned long delay)
{
	f->chg_queued = true;
	mod_delayed_work(system_freezable_wq, &f->chg_work, delay);
}

/*
 * Stay awake while a charger is attached: without RTC alarm wakeups the
 * temperature checks would stop in suspend while the SMB358 keeps
 * charging at up to 2 A.
 */
static void fgu_cable_start(struct sc2723_fgu *f, unsigned long delay)
{
	pm_stay_awake(f->dev);
	f->cable = FGU_CABLE_PENDING;
	f->det_tries = 0;
	f->cfg_fails = 0;
	f->chg_cfg = false;
	f->chg_idle = false;
	f->aicl_pending = false;
	fgu_chg_queue(f, delay);
}

static void fgu_cable_stop(struct sc2723_fgu *f)
{
	cancel_delayed_work(&f->chg_work);
	f->chg_queued = false;
	f->cable = FGU_CABLE_NONE;
	f->chg_cfg = false;
	f->aicl_pending = false;
	if (f->chg)
		smb358_idle(f);
	pm_relax(f->dev);
}

static void fgu_charger_poll(struct sc2723_fgu *f)
{
	int b, c;

	fgu_thermal(f);

	if (!f->online) {
		if (f->chg && !f->chg_idle)
			smb358_idle(f);
		return;
	}

	/* charger plugged in before boot */
	if (f->cable == FGU_CABLE_NONE) {
		fgu_cable_start(f, 0);
		return;
	}

	if (f->chg && f->cable != FGU_CABLE_PENDING)
		smb358_apply_limits(f);

	if (f->chg && f->chg_cfg && !(f->polls % FGU_CHG_CHECK_EVERY)) {
		b = i2c_smbus_read_byte_data(f->chg, SMB358_CMD_B);
		c = i2c_smbus_read_byte_data(f->chg, SMB358_CHG_CURRENT);
		if (b >= 0 && c >= 0 &&
		    ((b & SMB358_CMD_B_MODE) != smb358_want_mode(f) ||
		     c != smb358_want_cur(f))) {
			dev_warn(f->dev, "smb358: setup lost (cmd_b %02x chg_current %02x), redoing it\n",
				 b, c);
			f->chg_cfg = false;
		}
	}

	/* resume, late SMB358 probe, lost or failed setup */
	if (!f->chg_queued &&
	    (f->cable == FGU_CABLE_PENDING || f->aicl_pending ||
	     (f->chg && !f->chg_cfg && f->cfg_fails < FGU_CFG_MAX_FAILS)))
		fgu_chg_queue(f, 0);
}

/* returns true if the battery supply changed; *ext set if "ac"/"usb" did */
static bool fgu_update(struct sc2723_fgu *f, bool *ext)
{
	int vraw, craw, cur, cap, ui, status, health, online;
	ktime_t now = ktime_get_boottime();
	long now_s = fgu_now_s();
	bool changed = false, log = f->polls < 3 ||
				    !(f->polls % FGU_LOG_EVERY);
	s64 dt;
	s32 cc;

	f->polls++;

	if (f->vchg) {
		online = gpiod_get_value(f->vchg);
		if (online >= 0 && online != f->online) {
			f->online = online;
			*ext = changed = log = true;
			if (f->chg)
				smb358_dump(f, false);
			if (online)
				fgu_cable_start(f, FGU_DET_DELAY);
			else
				fgu_cable_stop(f);
		}
	}

	vraw = fgu_rd(FGU_VOLT_VAL) & FGU_VOLT_MASK;
	craw = fgu_rd(FGU_CURT_VAL) & FGU_CURT_MASK;
	if (!vraw || !craw) {
		dev_warn_ratelimited(f->dev,
				     "bogus sample (volt %#x curt %#x), skipped\n",
				     vraw, craw);
		return changed;
	}
	f->vbat_mv = fgu_adc2vol(f, vraw);
	f->cur_ma = cur = fgu_adc2cur(f, craw - FGU_CUR_0MA);
	f->ocv_mv = fgu_ocv(f, f->vbat_mv, cur);
	cc = fgu_clbcnt();

	dt = ktime_ms_delta(now, f->avg_t);
	if (dt >= 5000) {
		f->cur_avg_ma = (int)div64_s64((s64)(cc - f->avg_cc) * 1000000,
					       2 * dt * f->cur_1000ma_adc);
		f->avg_cc = cc;
		f->avg_t = now;
	}

	fgu_charger_attach(f);
	status = fgu_charge_status(f, cur);

	cap = fgu_soc(f, cc, f->ocv_mv, status == POWER_SUPPLY_STATUS_FULL);
	f->raw_cap = cap;
	ui = fgu_smooth(f, cap, status, cur, now_s);

	/* never report 0% (Android powers off) on a single bad sample */
	if (f->vbat_mv < f->uvlo_mv || cap <= 0)
		f->low_cnt++;
	else
		f->low_cnt = 0;
	if (f->vbat_mv < f->uvlo_mv && f->low_cnt >= FGU_LOW_DEBOUNCE &&
	    status != POWER_SUPPLY_STATUS_CHARGING &&
	    status != POWER_SUPPLY_STATUS_FULL)
		ui = 0;
	if (ui <= 0)
		ui = f->low_cnt >= FGU_LOW_DEBOUNCE ? 0 : 1;

	if (ui != f->ui_cap) {
		f->ui_cap = ui;
		f->cap_change_s = now_s;
		fgu_save_cap(ui);
		changed = true;
	}

	fgu_read_temp(f);
	health = fgu_health(f);
	if (health != f->health) {
		f->health = health;
		changed = log = true;
	}
	if (status != f->status) {
		f->status = status;
		changed = log = true;
	}

	fgu_charger_poll(f);

	if (log)
		fgu_log(f, cc);

	return changed;
}

static void fgu_work(struct work_struct *work)
{
	struct sc2723_fgu *f = container_of(to_delayed_work(work),
					    struct sc2723_fgu, work);
	bool ext = false, bat;
	unsigned long delay;

	mutex_lock(&f->lock);
	bat = fgu_update(f, &ext);
	delay = (f->ui_cap <= 5 || f->vchg_irq <= 0 || f->irq_storm) ?
		FGU_POLL_FAST : FGU_POLL;
	if (f->irq_storm && !f->irq_storm_logged) {
		f->irq_storm_logged = true;
		dev_warn(f->dev, "vchg: irq storm, falling back to polling\n");
	}
	mutex_unlock(&f->lock);

	if (ext)
		fgu_ext_changed(f);
	if (bat)
		power_supply_changed(f->bat);

	queue_delayed_work(system_freezable_wq, &f->work, delay);
}

/*
 * Charger state machine, kicked by VBUS changes: charger type detection
 * (retried until the MUIC reports a type), SMB358 setup for that type,
 * then AICL result checks on dedicated chargers.
 */
static void fgu_chg_work(struct work_struct *work)
{
	struct sc2723_fgu *f = container_of(to_delayed_work(work),
					    struct sc2723_fgu, chg_work);
	bool ext = false;

	mutex_lock(&f->lock);
	f->chg_queued = false;
	if (!f->online)
		goto out;

	if (f->cable == FGU_CABLE_PENDING) {
		f->cable = fgu_muic_detect(f);
		if (f->cable == FGU_CABLE_PENDING) {
			fgu_chg_queue(f, FGU_DET_RETRY);
			goto out;
		}
		ext = true;
	}

	if (!f->chg)
		goto out;

	if (!f->chg_cfg) {
		if (f->cfg_fails >= FGU_CFG_MAX_FAILS)
			goto out;
		smb358_configure(f);
		if (f->chg_cfg && fgu_is_dcp(f)) {
			f->aicl_pending = true;
			f->aicl_tries = 0;
			fgu_chg_queue(f, FGU_AICL_DELAY);
		}
		goto out;
	}

	if (f->aicl_pending) {
		if (smb358_aicl(f))
			f->aicl_pending = false;
		else
			fgu_chg_queue(f, FGU_AICL_RETRY);
	}
out:
	mutex_unlock(&f->lock);

	if (ext)
		fgu_ext_changed(f);
}

static irqreturn_t fgu_vchg_isr(int irq, void *data)
{
	struct sc2723_fgu *f = data;
	int level = gpiod_get_value(f->vchg);

	if (time_after(jiffies, f->irq_win + HZ)) {
		f->irq_win = jiffies;
		f->irq_cnt = 0;
	}
	if (++f->irq_cnt > 50) {
		disable_irq_nosync(irq);
		f->irq_storm = true;
	} else {
		/* EIC lines are level only: arm for the opposite level */
		irq_set_irq_type(irq, level ? IRQ_TYPE_LEVEL_LOW :
					      IRQ_TYPE_LEVEL_HIGH);
	}

	pm_wakeup_event(f->dev, 1000);
	mod_delayed_work(system_freezable_wq, &f->work, FGU_VCHG_DEBOUNCE);

	return IRQ_HANDLED;
}

/* ---- init ---- */

static void fgu_hw_init(struct sc2723_fgu *f)
{
	dev_info(f->dev, "hw: module_en %#x clk_en %#x rtc_clk_en %#x\n",
		 fgu_rd(ANA_REG_GLB_ARM_MODULE_EN),
		 fgu_rd(ANA_REG_GLB_ARM_CLK_EN),
		 fgu_rd(ANA_REG_GLB_RTC_CLK_EN));

	sci_adi_set(ANA_REG_GLB_ARM_MODULE_EN, SC2723_ANA_FGU_EN);
	sci_adi_set(ANA_REG_GLB_RTC_CLK_EN,
		    SC2723_RTC_FGU_EN | SC2723_RTC_FGUA_EN);

	sci_adi_set(ANA_REG_GLB_ARM_MODULE_EN, SC2723_ANA_ADC_EN);
	sci_adi_set(ANA_REG_GLB_ARM_CLK_EN,
		    SC2723_CLK_AUXADC_EN | SC2723_CLK_AUXAD_EN);
	sci_adi_set(SC2723_XTL_WAIT_CTRL, SC2723_XTL_EN);
	sci_glb_set(REG_AON_APB_SINDRV_CTRL, BIT_SINDRV_ENA);
	sci_adi_raw_write(ADC_IRQ_EN, 0);
	sci_adi_raw_write(ADC_IRQ_CLR, ADC_IRQ_BIT);
}

static void fgu_init_capacity(struct sc2723_fgu *f)
{
	int ua = fgu_rd(FGU_USER_AREA_STATUS);
	int type = (ua & FGU_UA_TYPE_MASK) >> FGU_UA_TYPE_SHIFT;
	int saved = ua & FGU_UA_CAP_MASK;
	int vraw = fgu_rd(FGU_VOLT_VAL) & FGU_VOLT_MASK;
	int craw = fgu_rd(FGU_CURT_VAL) & FGU_CURT_MASK;
	int vbat = fgu_adc2vol(f, vraw);
	int cur = fgu_adc2cur(f, craw - FGU_CUR_0MA + f->cur_offset);
	int est_ocv = vbat - cur * f->rint / 1000;
	int est = fgu_vol2cap(f, est_ocv);
	bool sane = vraw && craw && vbat >= 2800 && vbat <= 4600;
	int cap, pocv_raw, pocv = 0, pcur = 0;
	const char *src;

	if (type == FGU_UA_TYPE_FIRST || saved == FGU_UA_CAP_INVALID) {
		pocv_raw = fgu_rd(FGU_POCV_VAL) & FGU_VOLT_MASK;
		if (pocv_raw && !f->online) {
			pocv = fgu_adc2vol(f, pocv_raw);
			pcur = fgu_adc2cur(f, (fgu_rd(FGU_CLBCNT_QMAXL) << 1) -
					   FGU_CUR_0MA + f->cur_offset);
		}
		if (pocv >= 2800 && pocv <= 4600) {
			cap = fgu_vol2cap(f, pocv - pcur * f->rint / 1000);
			src = "first boot, power-on ocv";
		} else if (sane) {
			cap = est;
			src = "first boot, soft ocv";
		} else {
			cap = 50;
			src = "first boot, no usable sample";
		}
	} else if (saved > 100) {
		cap = sane ? est : 50;
		src = sane ? "invalid saved value, soft ocv" :
			     "invalid saved value, no usable sample";
	} else if (sane && !f->online &&
		   abs(saved - est) > FGU_STALE_SAVED_DIFF) {
		/* only on battery: charging skews the estimate upwards */
		cap = est;
		src = "stale saved value, soft ocv";
	} else {
		cap = saved;
		src = "saved";
	}
	cap = clamp(cap, 1, 100);

	dev_info(f->dev,
		 "init: %d%% from %s (user area %#06x, vbat %dmV cur %dmA est %d%%, pocv %dmV pcur %dmA)\n",
		 cap, src, ua, vbat, cur, est, pocv, pcur);

	fgu_ua_write(FGU_UA_TYPE_MASK, FGU_UA_TYPE_NORMAL << FGU_UA_TYPE_SHIFT);
	fgu_save_cap(cap);

	f->init_clbcnt = f->avg_cc = fgu_clbcnt();
	f->avg_t = ktime_get_boottime();
	f->init_cap = f->ui_cap = f->raw_cap = cap;
	f->charge_uah = cap * f->cnom * 10;
	f->vbat_mv = vbat;
	f->ocv_mv = est_ocv;
	f->cur_ma = cur;
	f->cap_change_s = f->last_query_s = fgu_now_s();
}

static u32 fgu_prop(struct device_node *np, const char *name, u32 def)
{
	u32 v;

	return of_property_read_u32(np, name, &v) ? def : v;
}

static int fgu_read_table(struct device *dev, const char *name,
			  struct fgu_tab **tab, int *len)
{
	int n = of_property_count_u32_elems(dev->of_node, name);

	if (n < 4 || n % 2)
		return -EINVAL;

	*tab = devm_kcalloc(dev, n / 2, sizeof(**tab), GFP_KERNEL);
	if (!*tab)
		return -ENOMEM;
	*len = n / 2;

	return of_property_read_u32_array(dev->of_node, name, (u32 *)*tab, n);
}

static int fgu_parse_dt(struct sc2723_fgu *f)
{
	struct device_node *np = f->dev->of_node;
	int ret;

	f->cnom = fgu_prop(np, "sprd,cnom-mah", 5000);
	f->rint = fgu_prop(np, "sprd,rint-mohm", 220);
	f->rsense_real = fgu_prop(np, "sprd,rsense-real", 200);
	f->rsense_spec = fgu_prop(np, "sprd,rsense-spec", 200);
	f->relax_ma = fgu_prop(np, "sprd,relax-current-ma", 50);
	f->alm_mv = fgu_prop(np, "sprd,alm-vbat-mv", 3500);
	f->uvlo_mv = fgu_prop(np, "sprd,soft-uvlo-mv", 3050);
	f->temp_ch = fgu_prop(np, "sprd,temp-adc-channel", 1);
	if (!f->cnom || !f->rsense_real || !f->rsense_spec)
		return -EINVAL;

	ret = fgu_read_table(f->dev, "sprd,ocv-table", &f->ocv_tab,
			     &f->ocv_len);
	if (ret) {
		dev_err(f->dev, "bad sprd,ocv-table: %d\n", ret);
		return ret;
	}
	if (fgu_read_table(f->dev, "sprd,temp-table", &f->temp_tab,
			   &f->temp_len)) {
		dev_warn(f->dev, "no sprd,temp-table, reporting %d.%d degC\n",
			 FGU_TEMP_DEFAULT / 10, FGU_TEMP_DEFAULT % 10);
		f->temp_tab = NULL;
	}

	f->full_mv = f->ocv_tab[0].x;
	f->shutdown_mv = f->ocv_tab[f->ocv_len - 1].x;
	f->warning_cap = fgu_vol2cap(f, f->alm_mv);
	f->chg_np = of_parse_phandle(np, "sprd,charger", 0);
	f->muic_np = of_parse_phandle(np, "sprd,muic", 0);

	return 0;
}

/* ---- power supplies ---- */

static int fgu_bat_get(struct power_supply *psy, enum power_supply_property psp,
		       union power_supply_propval *val)
{
	struct sc2723_fgu *f = power_supply_get_drvdata(psy);
	int raw, ret = 0;

	mutex_lock(&f->lock);
	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = f->status;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = f->health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = 1;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		raw = fgu_rd(FGU_VOLT_VAL) & FGU_VOLT_MASK;
		if (raw)
			f->vbat_mv = fgu_adc2vol(f, raw);
		val->intval = f->vbat_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = f->ocv_mv * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		raw = fgu_rd(FGU_CURT_VAL) & FGU_CURT_MASK;
		if (raw)
			f->cur_ma = fgu_adc2cur(f, raw - FGU_CUR_0MA);
		val->intval = f->cur_ma * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		val->intval = f->cur_avg_ma * 1000;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = f->ui_cap;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = min(f->temp, FGU_TEMP_REPORT_MAX);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		val->intval = f->cnom * 1000;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = f->charge_uah;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&f->lock);

	return ret;
}

/* "ac" for dedicated chargers, "usb" for everything else */
static int fgu_ext_get(struct power_supply *psy,
		       enum power_supply_property psp,
		       union power_supply_propval *val)
{
	struct sc2723_fgu *f = power_supply_get_drvdata(psy);
	bool mains = psy->desc->type == POWER_SUPPLY_TYPE_MAINS;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		/* "usb" also covers the detection window */
		val->intval = f->online &&
			      (f->cable == FGU_CABLE_DCP) == mains;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = mains ? FGU_DCP_INPUT_UA : FGU_USB_INPUT_UA;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = FGU_VBUS_UV;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property fgu_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
};

static enum power_supply_property fgu_ext_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
};

static const struct power_supply_desc fgu_bat_desc = {
	.name		= "battery",
	.type		= POWER_SUPPLY_TYPE_BATTERY,
	.properties	= fgu_bat_props,
	.num_properties	= ARRAY_SIZE(fgu_bat_props),
	.get_property	= fgu_bat_get,
};

static const struct power_supply_desc fgu_ac_desc = {
	.name		= "ac",
	.type		= POWER_SUPPLY_TYPE_MAINS,
	.properties	= fgu_ext_props,
	.num_properties	= ARRAY_SIZE(fgu_ext_props),
	.get_property	= fgu_ext_get,
};

static const struct power_supply_desc fgu_usb_desc = {
	.name		= "usb",
	.type		= POWER_SUPPLY_TYPE_USB,
	.properties	= fgu_ext_props,
	.num_properties	= ARRAY_SIZE(fgu_ext_props),
	.get_property	= fgu_ext_get,
};

static int sc2723_fgu_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct power_supply_config bcfg = {}, ecfg = {};
	struct sc2723_fgu *f;
	int ret, level;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = dev;
	mutex_init(&f->lock);
	INIT_DELAYED_WORK(&f->work, fgu_work);
	INIT_DELAYED_WORK(&f->chg_work, fgu_chg_work);
	f->status = POWER_SUPPLY_STATUS_DISCHARGING;
	f->health = POWER_SUPPLY_HEALTH_GOOD;
	f->temp = FGU_TEMP_DEFAULT;
	f->chg_stat_c = -1;
	platform_set_drvdata(pdev, f);

	ret = fgu_parse_dt(f);
	if (ret)
		return ret;

	f->vchg = devm_gpiod_get_optional(dev, "vchg", GPIOD_IN);
	if (IS_ERR(f->vchg)) {
		ret = PTR_ERR(f->vchg);
		goto err_np;
	}
	if (f->vchg)
		f->online = gpiod_get_value(f->vchg) > 0;

	fgu_hw_init(f);
	fgu_cal_init(f);
	sci_adi_write(FGU_RELAX_CURT_THRE,
		      fgu_cur2adc(f, f->relax_ma) & FGU_CURT_MASK,
		      FGU_CURT_MASK);
	udelay(130);

	fgu_init_capacity(f);
	fgu_read_temp(f);
	f->health = fgu_health(f);
	if (f->online)
		f->status = POWER_SUPPLY_STATUS_NOT_CHARGING;

	ecfg.drv_data = f;
	ecfg.supplied_to = fgu_supplied_to;
	ecfg.num_supplicants = ARRAY_SIZE(fgu_supplied_to);
	f->ac = devm_power_supply_register(dev, &fgu_ac_desc, &ecfg);
	if (IS_ERR(f->ac)) {
		ret = PTR_ERR(f->ac);
		dev_err(dev, "failed to register ac: %d\n", ret);
		goto err_np;
	}
	f->usb = devm_power_supply_register(dev, &fgu_usb_desc, &ecfg);
	if (IS_ERR(f->usb)) {
		dev_warn(dev, "failed to register usb: %ld\n",
			 PTR_ERR(f->usb));
		f->usb = NULL;
	}

	bcfg.drv_data = f;
	bcfg.of_node = dev->of_node;
	f->bat = devm_power_supply_register(dev, &fgu_bat_desc, &bcfg);
	if (IS_ERR(f->bat)) {
		ret = PTR_ERR(f->bat);
		dev_err(dev, "failed to register battery: %d\n", ret);
		goto err_np;
	}

	device_init_wakeup(dev, true);

	f->vchg_irq = f->vchg ? gpiod_to_irq(f->vchg) : -ENOENT;
	if (f->vchg_irq > 0) {
		level = gpiod_get_value(f->vchg);
		f->irq_win = jiffies;
		ret = devm_request_irq(dev, f->vchg_irq, fgu_vchg_isr,
				       (level ? IRQF_TRIGGER_LOW :
						IRQF_TRIGGER_HIGH) |
				       IRQF_NO_SUSPEND, "sc2723-fgu-vchg", f);
		if (ret) {
			dev_warn(dev, "vchg: no irq (%d), polling\n", ret);
			f->vchg_irq = ret;
		} else if (enable_irq_wake(f->vchg_irq)) {
			dev_info(dev, "vchg: irq %d is not a wakeup source\n",
				 f->vchg_irq);
		}
	} else if (f->vchg) {
		dev_warn(dev, "vchg: no irq mapping (%d), polling\n",
			 f->vchg_irq);
	}

	dev_info(dev, "cnom %dmAh rint %dmOhm alm %dmV uvlo %dmV warn %d%% ocv %d..%dmV, online %d irq %d\n",
		 f->cnom, f->rint, f->alm_mv, f->uvlo_mv, f->warning_cap,
		 f->shutdown_mv, f->full_mv, f->online, f->vchg_irq);

	queue_delayed_work(system_freezable_wq, &f->work, 0);

	return 0;

err_np:
	of_node_put(f->muic_np);
	of_node_put(f->chg_np);
	return ret;
}

static int sc2723_fgu_remove(struct platform_device *pdev)
{
	struct sc2723_fgu *f = platform_get_drvdata(pdev);

	if (f->vchg_irq > 0)
		disable_irq(f->vchg_irq);
	cancel_delayed_work_sync(&f->work);
	cancel_delayed_work_sync(&f->chg_work);
	if (f->muic)
		put_device(&f->muic->dev);
	if (f->chg)
		put_device(&f->chg->dev);
	of_node_put(f->muic_np);
	of_node_put(f->chg_np);

	return 0;
}

/* hand the charger over the way the vendor kernel does on power off */
static void sc2723_fgu_shutdown(struct platform_device *pdev)
{
	struct sc2723_fgu *f = platform_get_drvdata(pdev);

	if (f->vchg_irq > 0)
		disable_irq(f->vchg_irq);
	cancel_delayed_work_sync(&f->work);
	cancel_delayed_work_sync(&f->chg_work);

	mutex_lock(&f->lock);
	if (f->chg) {
		smb358_write(f, SMB358_THERM_A, SMB358_THERM_A_DEFAULT);
		smb358_write(f, SMB358_CMD_A, SMB358_CMD_A_HANDOVER);
		smb358_write(f, SMB358_CMD_A, SMB358_CMD_A_HANDOVER &
			     ~SMB358_CMD_A_VOLATILE_WR);
	}
	mutex_unlock(&f->lock);
}

static int __maybe_unused sc2723_fgu_suspend(struct device *dev)
{
	struct sc2723_fgu *f = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&f->work);
	cancel_delayed_work_sync(&f->chg_work);
	mutex_lock(&f->lock);
	f->chg_queued = false;	/* fgu_update() requeues it on resume */
	mutex_unlock(&f->lock);
	return 0;
}

static int __maybe_unused sc2723_fgu_resume(struct device *dev)
{
	struct sc2723_fgu *f = dev_get_drvdata(dev);

	mod_delayed_work(system_freezable_wq, &f->work, 0);
	return 0;
}

static SIMPLE_DEV_PM_OPS(sc2723_fgu_pm_ops, sc2723_fgu_suspend,
			 sc2723_fgu_resume);

static const struct of_device_id sc2723_fgu_of_match[] = {
	{ .compatible = "sprd,sc2723-fgu" },
	{ }
};
MODULE_DEVICE_TABLE(of, sc2723_fgu_of_match);

static struct platform_driver sc2723_fgu_driver = {
	.probe		= sc2723_fgu_probe,
	.remove		= sc2723_fgu_remove,
	.shutdown	= sc2723_fgu_shutdown,
	.driver		= {
		.name		= "sc2723-fgu",
		.of_match_table	= sc2723_fgu_of_match,
		.pm		= &sc2723_fgu_pm_ops,
	},
};
module_platform_driver(sc2723_fgu_driver);

MODULE_DESCRIPTION("SC2723 fuel gauge battery driver for SC7730SE");
MODULE_LICENSE("GPL v2");
