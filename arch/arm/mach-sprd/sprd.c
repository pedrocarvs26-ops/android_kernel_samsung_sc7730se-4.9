/*
 * Spreadtrum SC7730SE / SC8830 (SM-T560 gtelwifi / SM-T561 gtel3g) Machine Support
 *
 * Unifies static peripheral I/O mapping (SCI_IOMAP_BASE = 0xF5000000),
 * early framebuffer + sec_log console, INTC0..3 + AON_INTC IRQ routing,
 * ADI master bus initialization, Samsung sec_class creation, SDHCI auxdata
 * device naming (sprd-sdhci.0..3), and SC2723 PMIC watchdog system restart.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/export.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/clk-provider.h>
#include <linux/clocksource.h>
#include <linux/reboot.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>
#include <asm/system_misc.h>

#include <soc/sprd/iomap.h>
#include "core.h"

/* Exported for Samsung touchscreen/backlight/battery sysfs nodes */
struct class *sec_class;
EXPORT_SYMBOL(sec_class);

#define IOTABLE_ENTRY(t) \
	{ \
		.virtual	= SPRD_##t##_BASE, \
		.pfn		= __phys_to_pfn(SPRD_##t##_PHYS), \
		.length		= SPRD_##t##_SIZE, \
		.type		= MT_DEVICE_NONSHARED, \
	}

static struct map_desc sprd_io_desc[] __initdata = {
	IOTABLE_ENTRY(CORE),
	IOTABLE_ENTRY(DMA0),
	IOTABLE_ENTRY(USB),
	IOTABLE_ENTRY(SDIO0),
	IOTABLE_ENTRY(SDIO1),
	IOTABLE_ENTRY(SDIO2),
	IOTABLE_ENTRY(EMMC),
	IOTABLE_ENTRY(DISPC0),
	IOTABLE_ENTRY(AUDIO),
	IOTABLE_ENTRY(VBC),
	IOTABLE_ENTRY(AHB),
	IOTABLE_ENTRY(AONCKG),
	IOTABLE_ENTRY(GPUCKG),
	IOTABLE_ENTRY(MMCKG),
	IOTABLE_ENTRY(MMAHB),
	IOTABLE_ENTRY(DCAM),
	IOTABLE_ENTRY(ISP),
	IOTABLE_ENTRY(CSI2),
	IOTABLE_ENTRY(INT),
	IOTABLE_ENTRY(EIC),
	IOTABLE_ENTRY(EFUSE),
	IOTABLE_ENTRY(KPD),
	IOTABLE_ENTRY(SYSCNT),
	IOTABLE_ENTRY(PWM),
	IOTABLE_ENTRY(WDG),
	IOTABLE_ENTRY(CA7WDG),
	IOTABLE_ENTRY(GPIO),
	IOTABLE_ENTRY(PIN),
	IOTABLE_ENTRY(PMU),
	IOTABLE_ENTRY(IPI),
	IOTABLE_ENTRY(AONAPB),
	IOTABLE_ENTRY(APBCKG),
	IOTABLE_ENTRY(UART0),
	IOTABLE_ENTRY(UART1),
	IOTABLE_ENTRY(UART2),
	IOTABLE_ENTRY(UART3),
	IOTABLE_ENTRY(UART4),
	IOTABLE_ENTRY(I2C0),
	IOTABLE_ENTRY(I2C1),
	IOTABLE_ENTRY(I2C2),
	IOTABLE_ENTRY(I2C3),
	IOTABLE_ENTRY(I2C4),
	IOTABLE_ENTRY(SPI0),
	IOTABLE_ENTRY(SPI1),
	IOTABLE_ENTRY(SPI2),
	IOTABLE_ENTRY(INTC0),
	IOTABLE_ENTRY(INTC1),
	IOTABLE_ENTRY(INTC2),
	IOTABLE_ENTRY(INTC3),
	IOTABLE_ENTRY(APBREG),
	IOTABLE_ENTRY(ADI),
	IOTABLE_ENTRY(GPTIMER),
	IOTABLE_ENTRY(APTIMER0),
	IOTABLE_ENTRY(APTIMER1),
	IOTABLE_ENTRY(APTIMER2),
};

/* Preserve 3.10 device names for SDIO/eMMC so clock lookups & by-name symlinks match */
static const struct of_dev_auxdata sprd_auxdata_lookup[] __initconst = {
	OF_DEV_AUXDATA("sprd,sdhci-shark", 0x20300000, "sprd-sdhci.0", NULL),
	OF_DEV_AUXDATA("sprd,sdhci-shark", 0x20400000, "sprd-sdhci.1", NULL),
	OF_DEV_AUXDATA("sprd,sdhci-shark", 0x20500000, "sprd-sdhci.2", NULL),
	OF_DEV_AUXDATA("sprd,sdhci-shark", 0x20600000, "sprd-sdhci.3", NULL),
	OF_DEV_AUXDATA("sprd,sp8830-i2c",  0x70500000, "sprd-i2c.0",   NULL),
	OF_DEV_AUXDATA("sprd,sp8830-i2c",  0x70600000, "sprd-i2c.1",   NULL),
	OF_DEV_AUXDATA("sprd,sp8830-i2c",  0x70700000, "sprd-i2c.2",   NULL),
	OF_DEV_AUXDATA("sprd,sp8830-i2c",  0x70800000, "sprd-i2c.3",   NULL),
	OF_DEV_AUXDATA("sprd,sp8830-i2c",  0x70900000, "sprd-i2c.4",   NULL),
	{ /* sentinel */ }
};

static void __init sprd_map_io(void)
{
	iotable_init(sprd_io_desc, ARRAY_SIZE(sprd_io_desc));
	sprd_early_console_map_io();
}

static void __init sprd_init_early(void)
{
	sci_adi_init();
}

static void __init sprd_init_time(void)
{
	/* Enable AP_TMR0..2, AON_TMR, AP_SYST, and AON_SYST APB & 32kHz RTC clocks */
	sci_glb_set(REG_AON_APB_APB_EB0,
		    BIT_AON_TMR_EB | BIT_AP_TMR0_EB | BIT_AP_SYST_EB | BIT_AON_SYST_EB);
	sci_glb_set(REG_AON_APB_APB_EB1,
		    BIT_AP_TMR1_EB | BIT_AP_TMR2_EB);
	sci_glb_set(REG_AON_APB_APB_RTC_EB,
		    BIT_ARCH_RTC_EB | BIT_AON_SYST_RTC_EB | BIT_AP_SYST_RTC_EB |
		    BIT_AON_TMR_RTC_EB | BIT_AP_TMR0_RTC_EB |
		    BIT_AP_TMR1_RTC_EB | BIT_AP_TMR2_RTC_EB);

	of_clk_init(NULL);
	clocksource_probe();
}

static void __init sprd_init_machine(void)
{
	sec_class = class_create(THIS_MODULE, "sec");
	if (IS_ERR(sec_class)) {
		pr_warn("SPRD: failed to create sec_class (%ld)\n", PTR_ERR(sec_class));
		sec_class = NULL;
	}

	of_platform_default_populate(NULL, sprd_auxdata_lookup, NULL);
}

/*
 * SC2723 (A-Die) Watchdog Reset for Reboot / Recovery / Download Mode
 */
#define ANA_WDG_LOAD_LOW	(ANA_WDG_BASE + 0x00)
#define ANA_WDG_LOAD_HIGH	(ANA_WDG_BASE + 0x04)
#define ANA_WDG_CTRL		(ANA_WDG_BASE + 0x08)
#define ANA_WDG_INT_CLR		(ANA_WDG_BASE + 0x0C)
#define ANA_WDG_LOCK		(ANA_WDG_BASE + 0x20)

#define WDG_NEW_VER_EN		BIT(3)
#define WDG_CNT_EN_BIT		BIT(1)
#define WDG_RST_EN_BIT		BIT(3)
#define WDG_LD_BUSY_BIT		BIT(4)
#define WDG_UNLOCK_KEY		0xE551

#define HWRST_STATUS_RECOVERY	0x0020
#define HWRST_STATUS_NORMAL	0x0040
#define HWRST_STATUS_ALARM	0x0050
#define HWRST_STATUS_SLEEP	0x0060
#define HWRST_STATUS_FASTBOOT	0x0030
#define HWRST_STATUS_AUTODLOADER 0x00A0

void sprd_restart(enum reboot_mode mode, const char *cmd)
{
	u16 rst_mode = HWRST_STATUS_NORMAL;
	u32 val;
	int cnt = 1000;

	if (cmd) {
		if (!strcmp(cmd, "recovery"))
			rst_mode = HWRST_STATUS_RECOVERY;
		else if (!strcmp(cmd, " bootloader") || !strcmp(cmd, "bootloader") ||
			 !strcmp(cmd, "download"))
			rst_mode = HWRST_STATUS_AUTODLOADER;
		else if (!strcmp(cmd, "fastboot"))
			rst_mode = HWRST_STATUS_FASTBOOT;
		else if (!strcmp(cmd, "alarm"))
			rst_mode = HWRST_STATUS_ALARM;
	}

	/* Record reboot reason in SC2723 ANA_REG_GLB_HWRST_RTC[15:8] */
	val = sci_adi_read(ANA_REG_GLB_HWRST_RTC);
	val &= ~0xFF00;
	val |= (rst_mode & 0xFF) << 8;
	sci_adi_raw_write(ANA_REG_GLB_HWRST_RTC, (u16)val);

	/* Enable A-Die Watchdog module and RTC clock */
	sci_adi_set(ANA_REG_GLB_ARM_MODULE_EN, BIT_ANA_WDG_EN);
	sci_adi_set(ANA_REG_GLB_RTC_CLK_EN, BIT_RTC_WDG_EN);

	/* Unlock A-Die Watchdog and program a 50ms reset timeout (32768 Hz * 0.05s = 1638) */
	sci_adi_raw_write(ANA_WDG_LOCK, WDG_UNLOCK_KEY);
	sci_adi_set(ANA_WDG_CTRL, WDG_NEW_VER_EN);

	while ((sci_adi_read(ANA_WDG_CTRL) & WDG_LD_BUSY_BIT) && --cnt)
		udelay(10);

	sci_adi_raw_write(ANA_WDG_LOAD_HIGH, 0);
	sci_adi_raw_write(ANA_WDG_LOAD_LOW, 1638);
	sci_adi_set(ANA_WDG_CTRL, WDG_CNT_EN_BIT | WDG_RST_EN_BIT);

	/* Also trigger AP Watchdog at 0x40290000 as fallback */
	sci_glb_set(REG_AON_APB_APB_EB1, BIT_AP_WDG_EB);
	sci_glb_set(REG_AON_APB_APB_RTC_EB, BIT_AP_WDG_RTC_EB);
	__raw_writel(WDG_UNLOCK_KEY, (void __iomem *)(SPRD_WDG_BASE + 0x20));
	__raw_writel(WDG_NEW_VER_EN, (void __iomem *)(SPRD_WDG_BASE + 0x08));
	__raw_writel(0, (void __iomem *)(SPRD_WDG_BASE + 0x04));
	__raw_writel(100, (void __iomem *)(SPRD_WDG_BASE + 0x00));
	__raw_writel(WDG_NEW_VER_EN | WDG_CNT_EN_BIT | WDG_RST_EN_BIT,
		     (void __iomem *)(SPRD_WDG_BASE + 0x08));

	mdelay(1000);
	while (1)
		cpu_relax();
}

static const char * const sprd_dt_compat[] __initconst = {
	"sprd,sc7730se",
	"sprd,sc8830",
	"sprd,sp8830",
	"sprd,scx35",
	NULL
};

DT_MACHINE_START(SCPHONE, "sc8830")
	.dt_compat	= sprd_dt_compat,
	.map_io		= sprd_map_io,
	.init_early	= sprd_init_early,
	.init_irq	= sprd_init_irq,
	.init_time	= sprd_init_time,
	.init_machine	= sprd_init_machine,
	.restart	= sprd_restart,
MACHINE_END
