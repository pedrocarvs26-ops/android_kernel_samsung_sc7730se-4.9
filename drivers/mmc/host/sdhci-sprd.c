/*
 * Spreadtrum SC7730SE / SC8830 (SCX30G / SCX35) SDHCI Host Controller Driver
 *
 * Adapted from drivers/mmc/host/sprd-sdhci-fix-of.c and sprd-sdhci-common-of.c
 * in the Samsung SM-T560 / SM-T561 3.10 kernel for Linux 4.9.337.
 *
 * Hardware specifics handled here:
 * 1. AP_AHB enable (`enb-bit`) and soft reset (`rst-bit`) before register access.
 * 2. Spreadtrum software reset quirk (`mask | 0x08` written to SDHCI_SOFTWARE_RESET).
 * 3. Spreadtrum clock divider formula (`div = (div == 0) ? 0 : (div - 1)`).
 * 4. DDR50 write/read positive/negative delay registers at offsets 0x80/0x84/0x88.
 * 5. Parent clock selection (`clk_emmc`, `clk_sdio0`, `clk_sdio1` -> `clk_384m` / `clk_192m` / `clk_96m`).
 *
 * Licensed under the GPL-2.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/slab.h>
#include <linux/io.h>

#include <soc/sprd/iomap.h>
#include "sdhci-pltfm.h"

#define SPRD_SDHCI_WRITE_DELAY		0x0080
#define SPRD_SDHCI_READ_POS_DELAY	0x0084
#define SPRD_SDHCI_READ_NEG_DELAY	0x0088

struct sprd_sdhci_host {
	struct sdhci_host *host;
	struct clk *clk;
	struct clk *clk_parent;
	u32 base_rate;
	u32 enb_bit;
	u32 rst_bit;
	u32 write_delay;
	u32 read_pos_delay;
	u32 read_neg_delay;
	int detect_gpio;
	int id;
};

static void sprd_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sprd_sdhci_host *sprd_host = sdhci_pltfm_priv(pltfm_host);
	unsigned long timeout = 100;

	if (mask & SDHCI_RESET_ALL) {
		u16 clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
		clk &= ~SDHCI_CLOCK_CARD_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		udelay(200);
		clk &= ~SDHCI_CLOCK_INT_EN;
		sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
		host->clock = 0;
	}

	/* Spreadtrum SDHCI IP requires bit 3 (0x08) set alongside reset mask */
	sdhci_writeb(host, mask | 0x08, SDHCI_SOFTWARE_RESET);

	while (sdhci_readb(host, SDHCI_SOFTWARE_RESET) & mask) {
		if (timeout == 0) {
			pr_err("%s: Reset 0x%x never completed.\n",
			       mmc_hostname(host->mmc), (int)mask);
			return;
		}
		timeout--;
		mdelay(1);
	}

	if ((mask & SDHCI_RESET_ALL) &&
	    (sprd_host->id == 1 || sprd_host->id == 2)) {
		u16 tmp_val = sdhci_readw(host, SDHCI_TRANSFER_MODE);
		tmp_val &= 0xF8FF;
		sdhci_writew(host, tmp_val, SDHCI_TRANSFER_MODE);
	}
}

static void sprd_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sprd_sdhci_host *sprd_host = sdhci_pltfm_priv(pltfm_host);
	unsigned int div = 0, real_div;
	u16 clk = 0;
	unsigned long timeout;

	/* Sequence: disable SDCLK before changing divider */
	clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	clk &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);
	udelay(200);
	clk &= ~SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	host->mmc->actual_clock = 0;
	if (clock == 0)
		goto out;

	if (host->max_clk <= clock) {
		div = 1;
	} else {
		for (div = 2; div < SDHCI_MAX_DIV_SPEC_300; div += 2) {
			if ((host->max_clk / div) <= clock)
				break;
		}
	}
	real_div = div;
	/* Spreadtrum SDHCI v3.00 divider encoding: (real_div >> 1) - 1 */
	div >>= 1;
	if (div >= 1)
		div--;

	if (real_div)
		host->mmc->actual_clock = host->max_clk / real_div;

	clk = ((div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT) |
	      (((div & SDHCI_DIV_HI_MASK) >> SDHCI_DIV_MASK_LEN) << SDHCI_DIVIDER_HI_SHIFT);
	clk |= SDHCI_CLOCK_INT_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	timeout = 100;
	while (!((clk = sdhci_readw(host, SDHCI_CLOCK_CONTROL)) & SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never stabilised.\n",
			       mmc_hostname(host->mmc));
			return;
		}
		timeout--;
		mdelay(1);
	}

	clk |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, clk, SDHCI_CLOCK_CONTROL);

	if (host->quirks & SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK) {
		host->timeout_clk = host->mmc->actual_clock ?
				    DIV_ROUND_UP(host->mmc->actual_clock, 1000) :
				    DIV_ROUND_UP(clock, 1000);
	}

	if (sprd_host->write_delay)
		sdhci_writel(host, sprd_host->write_delay, SPRD_SDHCI_WRITE_DELAY);
	if (sprd_host->read_pos_delay)
		sdhci_writel(host, sprd_host->read_pos_delay, SPRD_SDHCI_READ_POS_DELAY);
	if (sprd_host->read_neg_delay)
		sdhci_writel(host, sprd_host->read_neg_delay, SPRD_SDHCI_READ_NEG_DELAY);

out:
	host->clock = clock;
}

static unsigned int sprd_sdhci_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sprd_sdhci_host *sprd_host = sdhci_pltfm_priv(pltfm_host);

	return sprd_host->base_rate;
}

static void sprd_sdhci_set_uhs_signaling(struct sdhci_host *host, unsigned int timing)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sprd_sdhci_host *sprd_host = sdhci_pltfm_priv(pltfm_host);
	u16 ctrl_2;

	ctrl_2 = sdhci_readw(host, SDHCI_HOST_CONTROL2);
	ctrl_2 &= ~SDHCI_CTRL_UHS_MASK;

	if (timing == MMC_TIMING_MMC_HS200)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
	else if (timing == MMC_TIMING_UHS_SDR12)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR12;
	else if (timing == MMC_TIMING_UHS_SDR25)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR25;
	else if (timing == MMC_TIMING_UHS_SDR50)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR50;
	else if (timing == MMC_TIMING_UHS_SDR104)
		ctrl_2 |= SDHCI_CTRL_UHS_SDR104;
	else if (timing == MMC_TIMING_UHS_DDR50 || timing == MMC_TIMING_MMC_DDR52) {
		ctrl_2 |= SDHCI_CTRL_UHS_DDR50;
		if (sprd_host->write_delay)
			sdhci_writel(host, sprd_host->write_delay, SPRD_SDHCI_WRITE_DELAY);
		if (sprd_host->read_pos_delay)
			sdhci_writel(host, sprd_host->read_pos_delay, SPRD_SDHCI_READ_POS_DELAY);
		if (sprd_host->read_neg_delay)
			sdhci_writel(host, sprd_host->read_neg_delay, SPRD_SDHCI_READ_NEG_DELAY);
	}

	sdhci_writew(host, ctrl_2, SDHCI_HOST_CONTROL2);
}

static const struct sdhci_ops sprd_sdhci_ops = {
	.set_clock		= sprd_sdhci_set_clock,
	.get_max_clock		= sprd_sdhci_get_max_clock,
	.set_bus_width		= sdhci_set_bus_width,
	.reset			= sprd_sdhci_reset,
	.set_uhs_signaling	= sprd_sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data sprd_sdhci_pdata = {
	.quirks = SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_BROKEN_CARD_DETECTION |
		  SDHCI_QUIRK_CLOCK_BEFORE_RESET |
		  SDHCI_QUIRK_MISSING_CAPS |
		  SDHCI_QUIRK_DELAY_AFTER_POWER,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   SDHCI_QUIRK2_BROKEN_HS200,
	.ops = &sprd_sdhci_ops,
};

static int sprd_sdhci_probe(struct platform_device *pdev)
{
	struct device_node *np = pdev->dev.of_node;
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sprd_sdhci_host *sprd_host;
	struct resource *res;
	const char *vmmc_name = NULL, *vqmmc_name = NULL;
	u32 max_freq = 52000000;
	u32 host_caps_mask = 0;
	int ret;

	host = sdhci_pltfm_init(pdev, &sprd_sdhci_pdata, sizeof(*sprd_host));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	sprd_host = sdhci_pltfm_priv(pltfm_host);
	sprd_host->host = host;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res) {
		switch (res->start) {
		case 0x20300000:
			sprd_host->id = 0;
			sprd_host->enb_bit = BIT_SDIO0_EB;
			sprd_host->rst_bit = BIT(11);
			break;
		case 0x20400000:
			sprd_host->id = 1;
			sprd_host->enb_bit = BIT_SDIO1_EB;
			sprd_host->rst_bit = BIT(12);
			break;
		case 0x20500000:
			sprd_host->id = 2;
			sprd_host->enb_bit = BIT_SDIO2_EB;
			sprd_host->rst_bit = BIT(13);
			break;
		case 0x20600000:
		default:
			sprd_host->id = 3;
			sprd_host->enb_bit = BIT_EMMC_EB;
			sprd_host->rst_bit = BIT(14);
			break;
		}
	}

	of_property_read_u32(np, "id", &sprd_host->id);
	of_property_read_u32(np, "enb-bit", &sprd_host->enb_bit);
	of_property_read_u32(np, "rst-bit", &sprd_host->rst_bit);
	of_property_read_u32(np, "max-frequency", &max_freq);
	of_property_read_u32(np, "host-caps-mask", &host_caps_mask);
	of_property_read_u32(np, "write-delay", &sprd_host->write_delay);
	of_property_read_u32(np, "read-pos-delay", &sprd_host->read_pos_delay);
	of_property_read_u32(np, "read-neg-delay", &sprd_host->read_neg_delay);

	if (sprd_host->id == 0)
		host->quirks2 |= SDHCI_QUIRK2_NO_1_8_V;

	/* Enable AHB gate and pulse controller soft reset */
	if (sprd_host->enb_bit)
		sci_glb_set(REG_AP_AHB_AHB_EB, sprd_host->enb_bit);
	udelay(200);
	if (sprd_host->rst_bit) {
		sci_glb_set(REG_AP_AHB_AHB_RST, sprd_host->rst_bit);
		udelay(100);
		sci_glb_clr(REG_AP_AHB_AHB_RST, sprd_host->rst_bit);
		udelay(100);
	}

	/* Enable optional named regulators from legacy 3.10 DT properties */
	if (!of_property_read_string(np, "vdd-vmmc", &vmmc_name) && vmmc_name) {
		struct regulator *reg = regulator_get(NULL, vmmc_name);
		if (!IS_ERR(reg)) {
			if (sprd_host->id == 0)
				regulator_set_voltage(reg, 3000000, 3000000);
			if (!regulator_is_enabled(reg))
				WARN_ON(regulator_enable(reg));
		}
	}
	if (!of_property_read_string(np, "vdd-vqmmc", &vqmmc_name) && vqmmc_name) {
		struct regulator *reg = regulator_get(NULL, vqmmc_name);
		if (!IS_ERR(reg)) {
			u32 vq_uv = 0;
			if (!of_property_read_u32(np, "vqmmc-voltage-level", &vq_uv) && vq_uv)
				regulator_set_voltage(reg, vq_uv, vq_uv);
			if (!regulator_is_enabled(reg))
				WARN_ON(regulator_enable(reg));
		}
	}

	/* Configure controller & parent clocks */
	sprd_host->clk = of_clk_get(np, 0);
	if (!IS_ERR_OR_NULL(sprd_host->clk)) {
		sprd_host->clk_parent = of_clk_get(np, 1);
		if (!IS_ERR_OR_NULL(sprd_host->clk_parent))
			clk_set_parent(sprd_host->clk, sprd_host->clk_parent);
		clk_prepare_enable(sprd_host->clk);
		sprd_host->base_rate = clk_get_rate(sprd_host->clk);
	}
	if (!sprd_host->base_rate)
		sprd_host->base_rate = max_freq ? max_freq : 52000000;

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err_pltfm;

	/* Support legacy 3.10 integer cd-gpios property if standard GPIO descriptor is absent */
	if (!of_property_read_u32(np, "cd-gpios", &sprd_host->detect_gpio) &&
	    sprd_host->detect_gpio > 0 &&
	    !(host->mmc->caps & MMC_CAP_NONREMOVABLE)) {
		mmc_gpio_request_cd(host->mmc, sprd_host->detect_gpio, 0);
	}

	if (of_find_property(np, "sdhci-cap-hw-reset", NULL))
		host->mmc->caps |= MMC_CAP_HW_RESET;
	if (of_find_property(np, "sdhci-cap-1v8-ddr", NULL))
		host->mmc->caps |= MMC_CAP_1_8V_DDR;
	if (of_find_property(np, "sdhci-cap2-hc-erase-sz", NULL))
		host->mmc->caps2 |= MMC_CAP2_HC_ERASE_SZ;

	host->mmc->caps |= MMC_CAP_MMC_HIGHSPEED | MMC_CAP_SD_HIGHSPEED | MMC_CAP_ERASE;

	host->caps = sdhci_readl(host, SDHCI_CAPABILITIES) & ~host_caps_mask;
	host->caps1 = sdhci_readl(host, SDHCI_CAPABILITIES_1);
	if (sprd_host->id == 3) {
		host->caps |= SDHCI_CAN_VDD_180;
		host->ocr_avail_mmc = MMC_VDD_165_195;
	} else {
		host->caps |= (SDHCI_CAN_VDD_300 | SDHCI_CAN_VDD_330 | SDHCI_CAN_VDD_180) &
			      ~host_caps_mask;
		host->ocr_avail_mmc = MMC_VDD_29_30 | MMC_VDD_30_31 | MMC_VDD_32_33 |
				      MMC_VDD_33_34 | MMC_VDD_165_195;
	}
	host->ocr_avail_sd = MMC_VDD_29_30 | MMC_VDD_30_31 | MMC_VDD_32_33 |
			     MMC_VDD_33_34;
	host->ocr_avail_sdio = MMC_VDD_29_30 | MMC_VDD_30_31 | MMC_VDD_32_33 |
			       MMC_VDD_33_34 | MMC_VDD_165_195;

	ret = sdhci_add_host(host);
	if (ret)
		goto err_pltfm;

	flush_delayed_work(&host->mmc->detect);

	dev_info(&pdev->dev, "Spreadtrum SDHCI host %d initialized at 0x%08lx (base_clk=%u Hz)\n",
		 sprd_host->id, res ? (unsigned long)res->start : 0UL, sprd_host->base_rate);
	return 0;

err_pltfm:
	if (!IS_ERR_OR_NULL(sprd_host->clk))
		clk_disable_unprepare(sprd_host->clk);
	sdhci_pltfm_free(pdev);
	return ret;
}

static const struct of_device_id sprd_sdhci_of_match[] = {
	{ .compatible = "sprd,sdhci-shark", },
	{ .compatible = "sprd,sdhci-dolphin", },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_sdhci_of_match);

static struct platform_driver sprd_sdhci_driver = {
	.driver = {
		.name		= "sprd-sdhci",
		.of_match_table	= sprd_sdhci_of_match,
	},
	.probe	= sprd_sdhci_probe,
	.remove	= sdhci_pltfm_unregister,
};

module_platform_driver(sprd_sdhci_driver);

MODULE_DESCRIPTION("Spreadtrum SC7730SE / SC8830 SDHCI Host Driver");
MODULE_LICENSE("GPL v2");
