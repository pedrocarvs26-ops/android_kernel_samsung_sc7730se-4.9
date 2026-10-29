// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal framebuffer driver for the Spreadtrum SC8830/SC7730 DISPC.
 *
 * The bootloader already brings up the whole display pipeline (DISPC,
 * MIPI DSI host and panel) and leaves the DISPC OSD layer scanning out a
 * 32bpp framebuffer in reserved memory.  Instead of re-initializing the
 * pipeline (the job of a future full sprdfb port), this driver takes over
 * that running configuration and adds what Android's display stack needs
 * on top of a plain simple-framebuffer:
 *
 *  - optional OSD R/B switch, so the framebuffer can be RGBA (the format
 *    the Spreadtrum gralloc always renders) while the bootloader uses BGRA
 *  - FBIO_WAITFORVSYNC backed by the DISPC DPI vsync interrupt, with a
 *    60 Hz software fallback when no interrupt arrives
 *  - FBIOBLANK support (the backlight follows through the fb notifier)
 */

#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/math64.h>
#include <linux/sizes.h>
#include <linux/sprd-early-console.h>

#define DISPC_CTRL		0x0000
#define DISPC_SIZE_XY		0x0004
#define DISPC_IMG_CTRL		0x0020
#define DISPC_IMG_Y_BASE_ADDR	0x0024
#define DISPC_OSD_CTRL		0x0040
#define DISPC_OSD_BASE_ADDR	0x0044
#define DISPC_OSD_PITCH		0x0050
#define DISPC_INT_EN		0x0070
#define DISPC_INT_CLR		0x0074
#define DISPC_INT_STATUS	0x0078
#define DISPC_INT_RAW		0x007c
#define DISPC_DPI_CTRL		0x0080

#define DISPC_INT_DONE		BIT(0)
#define DISPC_INT_TE		BIT(1)
#define DISPC_INT_ERR		BIT(2)
#define DISPC_INT_EDPI_TE	BIT(3)
#define DISPC_INT_UPDATE_DONE	BIT(4)
#define DISPC_INT_DPI_VSYNC	BIT(5)

#define DISPC_OSD_EN		BIT(0)
#define DISPC_OSD_RB_SWITCH	BIT(15)

#define DISPC_DPI_REG_UPDATE	BIT(5)

#define SPRD_FB_VSYNC_PERIOD_NS	16666667ULL
#define SPRD_FB_VSYNC_TIMEOUT	msecs_to_jiffies(50)

struct sprd_dispc_fb {
	struct device *dev;
	void __iomem *regs;
	int irq;
	bool irq_on;
	bool hw_vsync;
	bool vsync_seen;
	bool err_reported;
	wait_queue_head_t vsync_wq;
	unsigned int vsync_count;
	u32 palette[16];
};

static irqreturn_t sprd_dispc_fb_irq(int irq, void *data)
{
	struct sprd_dispc_fb *par = data;
	u32 status = readl(par->regs + DISPC_INT_STATUS);

	if (!status)
		return IRQ_NONE;

	writel(status, par->regs + DISPC_INT_CLR);

	if (status & DISPC_INT_ERR) {
		/* Underflow etc.: report once and stop it from storming. */
		writel(readl(par->regs + DISPC_INT_EN) & ~DISPC_INT_ERR,
		       par->regs + DISPC_INT_EN);
		if (!par->err_reported) {
			par->err_reported = true;
			dev_warn(par->dev, "DISPC error interrupt (status %08x)\n",
				 status);
		}
	}

	if (status & DISPC_INT_DPI_VSYNC) {
		par->vsync_seen = true;
		par->vsync_count++;
		wake_up_interruptible_all(&par->vsync_wq);
	}

	return IRQ_HANDLED;
}

/* Sleep until the next 60 Hz boundary of CLOCK_MONOTONIC. */
static int sprd_dispc_fb_soft_vsync(void)
{
	u64 now = ktime_get_ns();
	ktime_t next = ns_to_ktime((div64_u64(now, SPRD_FB_VSYNC_PERIOD_NS) + 1) *
				   SPRD_FB_VSYNC_PERIOD_NS);

	set_current_state(TASK_INTERRUPTIBLE);
	schedule_hrtimeout(&next, HRTIMER_MODE_ABS);
	return 0;
}

static int sprd_dispc_fb_wait_vsync(struct sprd_dispc_fb *par)
{
	unsigned int count;
	long ret;

	if (!par->hw_vsync)
		return sprd_dispc_fb_soft_vsync();

	count = READ_ONCE(par->vsync_count);
	ret = wait_event_interruptible_timeout(par->vsync_wq,
			READ_ONCE(par->vsync_count) != count,
			SPRD_FB_VSYNC_TIMEOUT);
	if (ret > 0)
		return 0;
	if (ret < 0)
		return ret;

	if (!par->vsync_seen) {
		dev_warn(par->dev,
			 "no DPI vsync interrupt, using software vsync\n");
		par->hw_vsync = false;
	}
	return 0;
}

static int sprd_dispc_fb_ioctl(struct fb_info *info, unsigned int cmd,
			       unsigned long arg)
{
	struct sprd_dispc_fb *par = info->par;

	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		/* The Spreadtrum HWC passes a NULL argument: never touch it. */
		return sprd_dispc_fb_wait_vsync(par);
	default:
		return -ENOTTY;
	}
}

static int sprd_dispc_fb_blank(int blank, struct fb_info *info)
{
	/*
	 * The panel keeps running; accepting the request lets the fb
	 * notifier chain switch the backlight on and off.
	 */
	return 0;
}

static int sprd_dispc_fb_setcolreg(u_int regno, u_int red, u_int green,
				   u_int blue, u_int transp,
				   struct fb_info *info)
{
	u32 *pal = info->pseudo_palette;
	u32 cr = red >> (16 - info->var.red.length);
	u32 cg = green >> (16 - info->var.green.length);
	u32 cb = blue >> (16 - info->var.blue.length);

	if (regno >= 16)
		return -EINVAL;

	pal[regno] = (cr << info->var.red.offset) |
		     (cg << info->var.green.offset) |
		     (cb << info->var.blue.offset) |
		     (0xffU << info->var.transp.offset);
	return 0;
}

static int sprd_dispc_fb_open(struct fb_info *info, int user)
{
	if (user)
		sprd_early_console_panel_off();
	return 0;
}

static struct fb_ops sprd_dispc_fb_ops = {
	.owner		= THIS_MODULE,
	.fb_open	= sprd_dispc_fb_open,
	.fb_setcolreg	= sprd_dispc_fb_setcolreg,
	.fb_blank	= sprd_dispc_fb_blank,
	.fb_ioctl	= sprd_dispc_fb_ioctl,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static void sprd_dispc_fb_set_bitfields(struct fb_var_screeninfo *var,
					bool rgba)
{
	var->bits_per_pixel = 32;
	var->red.length = 8;
	var->green.length = 8;
	var->blue.length = 8;
	var->transp.length = 8;
	var->green.offset = 8;
	var->transp.offset = 24;
	if (rgba) {
		var->red.offset = 0;
		var->blue.offset = 16;
	} else {
		var->red.offset = 16;
		var->blue.offset = 0;
	}
}

/* Mask our interrupts and release the IRQ before par goes away. */
static void sprd_dispc_fb_stop_irq(struct sprd_dispc_fb *par)
{
	if (!par->irq_on)
		return;
	writel(readl(par->regs + DISPC_INT_EN) &
	       ~(DISPC_INT_DPI_VSYNC | DISPC_INT_ERR),
	       par->regs + DISPC_INT_EN);
	devm_free_irq(par->dev, par->irq, par);
	par->irq_on = false;
	par->hw_vsync = false;
}

static int sprd_dispc_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *mem_np;
	struct sprd_dispc_fb *par;
	struct fb_info *info;
	struct resource *res, mem;
	u32 width, height, stride;
	u32 osd_ctrl, osd_base;
	bool rgba = false;
	int ret;

	if (of_property_read_u32(np, "width", &width) ||
	    of_property_read_u32(np, "height", &height) ||
	    of_property_read_u32(np, "stride", &stride)) {
		dev_err(dev, "missing width/height/stride\n");
		return -EINVAL;
	}

	mem_np = of_parse_phandle(np, "memory-region", 0);
	if (!mem_np) {
		dev_err(dev, "missing memory-region\n");
		return -EINVAL;
	}
	ret = of_address_to_resource(mem_np, 0, &mem);
	of_node_put(mem_np);
	if (ret)
		return ret;
	if (resource_size(&mem) < (resource_size_t)stride * height) {
		dev_err(dev, "memory-region too small\n");
		return -EINVAL;
	}

	info = framebuffer_alloc(sizeof(*par), dev);
	if (!info)
		return -ENOMEM;
	platform_set_drvdata(pdev, info);
	par = info->par;
	par->dev = dev;
	init_waitqueue_head(&par->vsync_wq);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res)
		par->regs = devm_ioremap(dev, res->start, resource_size(res));
	if (!par->regs) {
		dev_err(dev, "cannot map DISPC registers\n");
		ret = -ENOMEM;
		goto err_release;
	}

	osd_ctrl = readl(par->regs + DISPC_OSD_CTRL);
	osd_base = readl(par->regs + DISPC_OSD_BASE_ADDR);
	dev_info(dev,
		 "bootloader state: CTRL=%08x DPI_CTRL=%08x INT_EN=%08x INT_RAW=%08x OSD_CTRL=%08x OSD_BASE=%08x OSD_PITCH=%08x IMG_CTRL=%08x IMG_BASE=%08x SIZE=%08x\n",
		 readl(par->regs + DISPC_CTRL),
		 readl(par->regs + DISPC_DPI_CTRL),
		 readl(par->regs + DISPC_INT_EN),
		 readl(par->regs + DISPC_INT_RAW),
		 osd_ctrl, osd_base,
		 readl(par->regs + DISPC_OSD_PITCH),
		 readl(par->regs + DISPC_IMG_CTRL),
		 readl(par->regs + DISPC_IMG_Y_BASE_ADDR),
		 readl(par->regs + DISPC_SIZE_XY));

	if (of_property_read_bool(np, "sprd,osd-rb-switch")) {
		if ((osd_ctrl & DISPC_OSD_EN) && osd_base == (u32)mem.start) {
			if (!(osd_ctrl & DISPC_OSD_RB_SWITCH)) {
				writel(osd_ctrl | DISPC_OSD_RB_SWITCH,
				       par->regs + DISPC_OSD_CTRL);
				writel(readl(par->regs + DISPC_DPI_CTRL) |
				       DISPC_DPI_REG_UPDATE,
				       par->regs + DISPC_DPI_CTRL);
			}
			rgba = true;
		} else {
			dev_warn(dev,
				 "OSD layer not scanning out %pa, keeping BGRA\n",
				 &mem.start);
		}
	} else {
		rgba = !!(osd_ctrl & DISPC_OSD_RB_SWITCH);
	}

	info->fix.smem_start = mem.start;
	info->fix.smem_len = stride * height;
	info->fix.line_length = stride;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	strlcpy(info->fix.id, "sprd-dispc", sizeof(info->fix.id));

	info->var.xres = width;
	info->var.yres = height;
	info->var.xres_virtual = width;
	info->var.yres_virtual = height;
	info->var.activate = FB_ACTIVATE_NOW;
	/* 9.6" 800x1280 panel, ~130x207 mm active area (~157 dpi) */
	info->var.height = 207;
	info->var.width = 130;
	/* no porch info from the bootloader: pick a pixclock that gives 60 Hz */
	info->var.pixclock = (u32)div_u64(1000000000000ULL,
					   (u64)width * height * 60);
	info->var.vmode = FB_VMODE_NONINTERLACED;
	sprd_dispc_fb_set_bitfields(&info->var, rgba);

	info->apertures = alloc_apertures(1);
	if (!info->apertures) {
		ret = -ENOMEM;
		goto err_release;
	}
	info->apertures->ranges[0].base = info->fix.smem_start;
	info->apertures->ranges[0].size = info->fix.smem_len;

	info->fbops = &sprd_dispc_fb_ops;
	info->flags = FBINFO_DEFAULT;
	info->pseudo_palette = par->palette;
	info->screen_base = devm_ioremap_wc(dev, info->fix.smem_start,
					    info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto err_release;
	}
	info->screen_size = info->fix.smem_len;

	/*
	 * One-shot bandwidth probe on the last 256 KiB (bottom rows): gralloc
	 * memcpy()s every frame into this memory while page flipping is not
	 * available, so its write speed bounds the UI frame rate.
	 */
	if (info->fix.smem_len >= SZ_256K) {
		void __iomem *p = info->screen_base + info->fix.smem_len - SZ_256K;
		ktime_t t0 = ktime_get();
		s64 us;

		memset_io(p, 0, SZ_256K);
		wmb();
		us = ktime_us_delta(ktime_get(), t0);
		dev_info(dev, "fb write: 256 KiB in %lld us (%lld MB/s)\n",
			 us, us > 0 ? div64_s64(256LL * 1024 * 1000000 / 1048576, us) : -1LL);
	}

	par->irq = platform_get_irq(pdev, 0);
	if (par->irq > 0) {
		/* Drop whatever the bootloader left pending, then listen. */
		writel(readl(par->regs + DISPC_INT_RAW),
		       par->regs + DISPC_INT_CLR);
		ret = devm_request_irq(dev, par->irq, sprd_dispc_fb_irq, 0,
				       "sprd-dispc-fb", par);
		if (ret) {
			dev_warn(dev, "cannot get IRQ %d (%d), software vsync\n",
				 par->irq, ret);
		} else {
			writel(readl(par->regs + DISPC_INT_EN) |
			       DISPC_INT_DPI_VSYNC | DISPC_INT_ERR,
			       par->regs + DISPC_INT_EN);
			par->irq_on = true;
			par->hw_vsync = true;
		}
	}

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(dev, "register_framebuffer failed: %d\n", ret);
		goto err_irq;
	}

	dev_info(dev, "fb%d: %ux%u %s @ %pa, %s vsync\n", info->node,
		 width, height, rgba ? "RGBA" : "BGRA", &mem.start,
		 par->hw_vsync ? "DPI irq" : "software");
	return 0;

err_irq:
	sprd_dispc_fb_stop_irq(par);
err_release:
	framebuffer_release(info);
	return ret;
}

static int sprd_dispc_fb_remove(struct platform_device *pdev)
{
	struct fb_info *info = platform_get_drvdata(pdev);
	struct sprd_dispc_fb *par = info->par;

	unregister_framebuffer(info);
	sprd_dispc_fb_stop_irq(par);
	framebuffer_release(info);
	return 0;
}

static const struct of_device_id sprd_dispc_fb_of_match[] = {
	{ .compatible = "sprd,sc8830-dispc-fb" },
	{ }
};
MODULE_DEVICE_TABLE(of, sprd_dispc_fb_of_match);

static struct platform_driver sprd_dispc_fb_driver = {
	.probe = sprd_dispc_fb_probe,
	.remove = sprd_dispc_fb_remove,
	.driver = {
		.name = "sprd-dispc-fb",
		.of_match_table = sprd_dispc_fb_of_match,
	},
};
module_platform_driver(sprd_dispc_fb_driver);

MODULE_DESCRIPTION("Spreadtrum SC8830 DISPC framebuffer (bootloader-initialized)");
MODULE_LICENSE("GPL v2");
