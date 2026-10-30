/*
 * Spreadtrum SC7730SE / Samsung SM-T560 & SM-T561 Broadcom BCM43430 Wi-Fi glue
 *
 * Adapted from arch/arm/mach-sc/board-tshark-wifi.c in the vendor 3.10 kernel.
 * Provides preallocated SKB/scan buffers, 32kHz LPO clock (clk_aux0),
 * WL_REG_ON (GPIO 230) power control, WL_HOST_WAKE (GPIO 148) OOB IRQ,
 * and SDIO1 (mmc2) virtual card detection for bcmdhd.
 *
 * Licensed under the GPL-2.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/skbuff.h>
#include <linux/wlan_plat.h>
#include <linux/regulator/consumer.h>
#include <linux/export.h>
#include <linux/clk.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/mmc/host.h>

#include <soc/sprd/iomap.h>

#define GPIO_WIFI_SHUTDOWN		230
#define GPIO_WIFI_IRQ			148

#define WLAN_STATIC_SCAN_BUF0		5
#define WLAN_STATIC_SCAN_BUF1		6
#define WLAN_STATIC_DHD_INFO_BUF	7
#define WLAN_SCAN_BUF_SIZE		(64 * 1024)
#define WLAN_DHD_INFO_BUF_SIZE		(16 * 1024)
#define PREALLOC_WLAN_SEC_NUM		4
#define PREALLOC_WLAN_BUF_NUM		160
#define PREALLOC_WLAN_SECTION_HEADER	24
#define WLAN_SECTION_SIZE_0		(PREALLOC_WLAN_BUF_NUM * 128)
#define WLAN_SECTION_SIZE_1		(PREALLOC_WLAN_BUF_NUM * 128)
#define WLAN_SECTION_SIZE_2		(PREALLOC_WLAN_BUF_NUM * 512)
#define WLAN_SECTION_SIZE_3		(PREALLOC_WLAN_BUF_NUM * 1024)
#define DHD_SKB_HDRSIZE			336
#define DHD_SKB_1PAGE_BUFSIZE		((PAGE_SIZE * 1) - DHD_SKB_HDRSIZE)
#define DHD_SKB_2PAGE_BUFSIZE		((PAGE_SIZE * 2) - DHD_SKB_HDRSIZE)
#define DHD_SKB_4PAGE_BUFSIZE		((PAGE_SIZE * 4) - DHD_SKB_HDRSIZE)
#define WLAN_SKB_BUF_NUM		17

static struct mmc_host *wlan_mmc;
static int wlan_device_cd;
static int wlan_device_power_state;
static int wlan_device_reset_state;
static void (*wifi_status_cb)(int card_present, void *dev_id);
static void *wifi_status_cb_devid;

static struct sk_buff *wlan_static_skb[WLAN_SKB_BUF_NUM];

struct wlan_mem_prealloc {
	void *mem_ptr;
	unsigned long size;
};

static struct wlan_mem_prealloc wlan_mem_array[PREALLOC_WLAN_SEC_NUM] = {
	{ NULL, (WLAN_SECTION_SIZE_0 + PREALLOC_WLAN_SECTION_HEADER) },
	{ NULL, (WLAN_SECTION_SIZE_1 + PREALLOC_WLAN_SECTION_HEADER) },
	{ NULL, (WLAN_SECTION_SIZE_2 + PREALLOC_WLAN_SECTION_HEADER) },
	{ NULL, (WLAN_SECTION_SIZE_3 + PREALLOC_WLAN_SECTION_HEADER) },
};

static void *wlan_static_scan_buf0;
static void *wlan_static_scan_buf1;
static void *wlan_static_dhd_info_buf;

static void *wifi_mem_prealloc(int section, unsigned long size)
{
	if (section == PREALLOC_WLAN_SEC_NUM)
		return wlan_static_skb;
	if (section == WLAN_STATIC_SCAN_BUF0)
		return wlan_static_scan_buf0;
	if (section == WLAN_STATIC_SCAN_BUF1)
		return wlan_static_scan_buf1;

	if (section == WLAN_STATIC_DHD_INFO_BUF) {
		if (size > WLAN_DHD_INFO_BUF_SIZE)
			return NULL;
		return wlan_static_dhd_info_buf;
	}

	if (section < 0 || section > PREALLOC_WLAN_SEC_NUM)
		return NULL;

	if (wlan_mem_array[section].size < size)
		return NULL;

	return wlan_mem_array[section].mem_ptr;
}

static int __init init_wifi_mem(void)
{
	int i, j;

	for (i = 0; i < 8; i++) {
		wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_1PAGE_BUFSIZE);
		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	for (; i < 16; i++) {
		wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_2PAGE_BUFSIZE);
		if (!wlan_static_skb[i])
			goto err_skb_alloc;
	}

	wlan_static_skb[i] = dev_alloc_skb(DHD_SKB_4PAGE_BUFSIZE);
	if (!wlan_static_skb[i])
		goto err_skb_alloc;

	for (i = 0; i < PREALLOC_WLAN_SEC_NUM; i++) {
		wlan_mem_array[i].mem_ptr =
		    kmalloc(wlan_mem_array[i].size, GFP_KERNEL);
		if (!wlan_mem_array[i].mem_ptr)
			goto err_mem_alloc;
	}

	wlan_static_scan_buf0 = kmalloc(WLAN_SCAN_BUF_SIZE, GFP_KERNEL);
	if (!wlan_static_scan_buf0)
		goto err_mem_alloc;

	wlan_static_scan_buf1 = kmalloc(WLAN_SCAN_BUF_SIZE, GFP_KERNEL);
	if (!wlan_static_scan_buf1)
		goto err_mem_alloc;

	wlan_static_dhd_info_buf = kmalloc(WLAN_DHD_INFO_BUF_SIZE, GFP_KERNEL);
	if (!wlan_static_dhd_info_buf)
		goto err_mem_alloc;

	pr_info("%s: WIFI MEM Allocated\n", __func__);
	return 0;

err_mem_alloc:
	pr_err("Failed to mem_alloc for WLAN\n");
	for (j = 0; j < i; j++)
		kfree(wlan_mem_array[j].mem_ptr);
	i = WLAN_SKB_BUF_NUM;

err_skb_alloc:
	pr_err("Failed to skb_alloc for WLAN\n");
	for (j = 0; j < i; j++)
		dev_kfree_skb(wlan_static_skb[j]);
	return -ENOMEM;
}

static void wlan_clk_init(void)
{
	struct clk *wlan_clk;
	struct clk *clk_parent;

	wlan_clk = clk_get(NULL, "clk_aux0");
	if (IS_ERR(wlan_clk)) {
		pr_warn("wifi: failed to get clk_aux0\n");
		return;
	}

	clk_parent = clk_get(NULL, "ext_32k");
	if (!IS_ERR(clk_parent)) {
		clk_set_parent(wlan_clk, clk_parent);
		clk_put(clk_parent);
	}

	clk_set_rate(wlan_clk, 32768);
	clk_prepare_enable(wlan_clk);
	clk_put(wlan_clk);
}

extern void mmc_ctrl_power(struct mmc_host *host, bool onoff);

void wlan_set_mmchost(struct mmc_host *mmc)
{
	if (!wlan_mmc)
		wlan_mmc = mmc;
}
EXPORT_SYMBOL(wlan_set_mmchost);

static int wlan_device_power(int on)
{
	pr_info("%s: %d\n", __func__, on);

	gpio_direction_output(GPIO_WIFI_SHUTDOWN, on ? 1 : 0);
	wlan_device_power_state = on;

	if (wlan_mmc)
		mmc_ctrl_power(wlan_mmc, on);

	return 0;
}

static int wlan_device_reset(int on)
{
	wlan_device_reset_state = on;
	return 0;
}

int wlan_device_status_register(void (*callback)(int card_present, void *dev_id),
				void *dev_id)
{
	if (wifi_status_cb)
		return -EAGAIN;
	wifi_status_cb = callback;
	wifi_status_cb_devid = dev_id;
	return 0;
}
EXPORT_SYMBOL(wlan_device_status_register);

static int wlan_device_set_carddetect(int val)
{
	pr_info("%s: %d\n", __func__, val);
	wlan_device_cd = val;

	if (wlan_mmc) {
		wlan_mmc->rescan_entered = 0;
		mmc_detect_change(wlan_mmc, 0);
	} else {
		pr_warn("%s: wlan_mmc is null, carddetect failed\n", __func__);
	}
	return 0;
}

static struct wifi_platform_data wlan_device_control = {
	.set_power      = wlan_device_power,
	.set_reset      = wlan_device_reset,
	.set_carddetect = wlan_device_set_carddetect,
	.mem_prealloc   = wifi_mem_prealloc,
};

static struct resource wlan_resources[] = {
	[0] = {
		.start  = SPRD_SDIO1_PHYS,
		.end    = SPRD_SDIO1_PHYS + SZ_4K - 1,
		.flags  = IORESOURCE_MEM,
	},
	[1] = {
		.name   = "bcmdhd_wlan_irq",
		.flags  = IORESOURCE_IRQ | IORESOURCE_IRQ_HIGHLEVEL |
			  IORESOURCE_IRQ_SHAREABLE | IRQF_NO_SUSPEND,
	},
};

static struct platform_device sprd_wlan_device = {
	.name           = "bcmdhd_wlan",
	.id             = 1,
	.dev            = {
		.platform_data = &wlan_device_control,
	},
	.resource       = wlan_resources,
	.num_resources  = ARRAY_SIZE(wlan_resources),
};

static int __init wlan_device_init(void)
{
	int irq;

	init_wifi_mem();
	wlan_clk_init();

	if (gpio_request(GPIO_WIFI_IRQ, "oob_irq") == 0)
		gpio_direction_input(GPIO_WIFI_IRQ);

	irq = gpio_to_irq(GPIO_WIFI_IRQ);
	wlan_resources[1].start = irq;
	wlan_resources[1].end = irq;

	if (gpio_request(GPIO_WIFI_SHUTDOWN, "wifi_pwd") == 0)
		gpio_direction_output(GPIO_WIFI_SHUTDOWN, 0);

	return platform_device_register(&sprd_wlan_device);
}

late_initcall(wlan_device_init);
