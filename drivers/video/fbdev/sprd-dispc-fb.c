// SPDX-License-Identifier: GPL-2.0
/*
 * Framebuffer driver for the Spreadtrum SC8830/SC7730 DISPC.
 *
 * The bootloader already brings up the whole display pipeline (DISPC,
 * MIPI DSI host and panel) and leaves the DISPC OSD layer scanning out a
 * 32bpp framebuffer in reserved memory.  Instead of re-initializing the
 * pipeline (the job of a future full sprdfb port), this driver takes over
 * that running configuration and adds what Android's display stack needs
 * on top of a plain simple-framebuffer:
 *
 *  - page flipping: the reserved memory holds up to three screen pages
 *    (yres_virtual = 3 * yres, like the 3.10 sprdfb driver with
 *    CONFIG_FB_TRIPLE_FRAMEBUFFER), so gralloc hands the pages out as
 *    render targets and posting a frame is a pan instead of a full-screen
 *    memcpy.  A pan reprograms the OSD base address, which the DISPC
 *    latches at the next vsync.  Before queueing a flip we wait until the
 *    previous one has been latched, so with three pages recycled in FIFO
 *    order the page given back to the renderer is never still on screen.
 *  - optional OSD R/B switch, so the framebuffer can be RGBA (the format
 *    the Spreadtrum gralloc always renders) while the bootloader uses BGRA
 *  - FBIO_WAITFORVSYNC backed by the DISPC DPI vsync interrupt, with a
 *    60 Hz software fallback when no interrupt arrives
 *  - FBIOBLANK support (the backlight follows through the fb notifier)
 *  - flip and vsync statistics in sysfs (flip_stats)
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/fb.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/preempt.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/sprd-early-console.h>

#define DISPC_CTRL		0x0000
#define DISPC_SIZE_XY		0x0004
#define DISPC_IMG_CTRL		0x0020
#define DISPC_IMG_Y_BASE_ADDR	0x0024
#define DISPC_OSD_CTRL		0x0040
#define DISPC_OSD_BASE_ADDR	0x0044
#define DISPC_OSD_SIZE_XY	0x0048
#define DISPC_OSD_PITCH		0x004c
#define DISPC_OSD_DISP_XY	0x0050
#define DISPC_INT_EN		0x0070
#define DISPC_INT_CLR		0x0074
#define DISPC_INT_STATUS	0x0078
#define DISPC_INT_RAW		0x007c
#define DISPC_DPI_CTRL		0x0080
/* shadow copy of the OSD base address the DISPC is scanning out */
#define DISPC_SHDW_OSD_BASE_ADDR 0x00e4

#define DISPC_INT_DONE		BIT(0)
#define DISPC_INT_TE		BIT(1)
#define DISPC_INT_ERR		BIT(2)
#define DISPC_INT_EDPI_TE	BIT(3)
#define DISPC_INT_UPDATE_DONE	BIT(4)
#define DISPC_INT_DPI_VSYNC	BIT(5)
#define DISPC_INT_USED		(DISPC_INT_ERR | DISPC_INT_UPDATE_DONE | \
				 DISPC_INT_DPI_VSYNC)

#define DISPC_OSD_EN		BIT(0)
#define DISPC_OSD_RB_SWITCH	BIT(15)

/* latch the shadow registers at the next vsync (SW + vsync update mode) */
#define DISPC_DPI_REG_UPDATE	BIT(5)

#define SPRD_FB_MAX_PAGES	3
#define SPRD_FB_VSYNC_PERIOD_NS	16666667ULL
#define SPRD_FB_VSYNC_TIMEOUT	msecs_to_jiffies(50)
#define SPRD_FB_FLIP_TIMEOUT	msecs_to_jiffies(50)
/* consecutive flip timeouts before we stop waiting for flips */
#define SPRD_FB_FLIP_MAX_TIMEOUTS 3
#define SPRD_FB_FLIP_MAX_DUMPS	5
#define SPRD_FB_STATS_INTERVAL	4096

struct sprd_dispc_fb {
	struct device *dev;
	void __iomem *regs;
	int irq;
	bool irq_on;
	bool hw_vsync;
	bool vsync_seen;
	bool err_reported;
	bool rgba;
	unsigned int num_pages;
	wait_queue_head_t vsync_wq;	/* vsync and update-done events */

	/* protects the flip state and the vsync timestamps against the irq */
	spinlock_t lock;
	unsigned int vsync_count;
	ktime_t vsync_first;
	ktime_t vsync_last;
	u32 cur_base;		/* OSD base address of the last flip */
	bool flip_pending;	/* last flip not acknowledged by UPDATE_DONE */

	bool flip_sync;		/* wait for the previous flip to be latched */
	bool shadow_ok;		/* SHDW_OSD_BASE_ADDR reflects latched flips */
	unsigned int timeout_streak;
	unsigned long flips;
	unsigned long flip_waits;
	unsigned long flip_timeouts;
	unsigned long update_irqs;
	u64 wait_us_total;
	u32 wait_us_max;

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

	if (!(status & (DISPC_INT_UPDATE_DONE | DISPC_INT_DPI_VSYNC)))
		return IRQ_HANDLED;

	spin_lock(&par->lock);
	if (status & DISPC_INT_UPDATE_DONE) {
		par->flip_pending = false;
		par->update_irqs++;
	}
	if (status & DISPC_INT_DPI_VSYNC) {
		ktime_t now = ktime_get();

		if (!par->vsync_seen) {
			par->vsync_seen = true;
			par->vsync_first = now;
		}
		par->vsync_last = now;
		par->vsync_count++;
	}
	spin_unlock(&par->lock);

	wake_up_all(&par->vsync_wq);
	return IRQ_HANDLED;
}

/* Measured vsync rate in mHz, 0 while unknown. */
static unsigned int sprd_dispc_fb_vsync_mhz(struct sprd_dispc_fb *par)
{
	unsigned long flags;
	unsigned int count;
	u64 ns, period;

	spin_lock_irqsave(&par->lock, flags);
	count = par->vsync_count;
	ns = ktime_to_ns(ktime_sub(par->vsync_last, par->vsync_first));
	spin_unlock_irqrestore(&par->lock, flags);

	if (count < 2)
		return 0;
	period = div64_u64(ns, count - 1);
	return period ? (unsigned int)div64_u64(1000000000000ULL, period) : 0;
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

static void sprd_dispc_fb_dump_flip(struct sprd_dispc_fb *par,
				    const char *why)
{
	dev_warn(par->dev,
		 "%s: CTRL=%08x DPI_CTRL=%08x INT_EN=%08x INT_RAW=%08x OSD_BASE=%08x SHDW_OSD_BASE=%08x cur=%08x pending=%d vsyncs=%u updates=%lu\n",
		 why, readl(par->regs + DISPC_CTRL),
		 readl(par->regs + DISPC_DPI_CTRL),
		 readl(par->regs + DISPC_INT_EN),
		 readl(par->regs + DISPC_INT_RAW),
		 readl(par->regs + DISPC_OSD_BASE_ADDR),
		 readl(par->regs + DISPC_SHDW_OSD_BASE_ADDR),
		 par->cur_base, READ_ONCE(par->flip_pending),
		 READ_ONCE(par->vsync_count), READ_ONCE(par->update_irqs));
}

static void sprd_dispc_fb_log_stats(struct sprd_dispc_fb *par)
{
	unsigned int mhz = sprd_dispc_fb_vsync_mhz(par);

	dev_info(par->dev,
		 "flips %lu, waited %lu (avg %llu us, max %u us), timeouts %lu, vsync %u.%03u Hz\n",
		 par->flips, par->flip_waits,
		 par->flip_waits ? div64_u64(par->wait_us_total,
					     par->flip_waits) : 0ULL,
		 par->wait_us_max, par->flip_timeouts, mhz / 1000, mhz % 1000);
}

/* Has the DISPC latched the OSD base address of the last flip? */
static bool sprd_dispc_fb_flip_latched(struct sprd_dispc_fb *par)
{
	if (par->shadow_ok)
		return readl(par->regs + DISPC_SHDW_OSD_BASE_ADDR) ==
		       READ_ONCE(par->cur_base);
	return !READ_ONCE(par->flip_pending);
}

/*
 * Triple buffering as in the 3.10 sprdfb driver: a flip does not wait for
 * itself to be latched, but the next flip first waits for the previous one.
 */
static void sprd_dispc_fb_wait_flip(struct sprd_dispc_fb *par)
{
	ktime_t start;
	long left;
	u32 us;

	if (!par->flip_sync || sprd_dispc_fb_flip_latched(par))
		return;
	/* never sleep in atomic context (e.g. a panic console) */
	if (in_atomic() || irqs_disabled() || oops_in_progress)
		return;

	par->flip_waits++;
	start = ktime_get();
	left = wait_event_timeout(par->vsync_wq,
				  sprd_dispc_fb_flip_latched(par),
				  SPRD_FB_FLIP_TIMEOUT);
	us = (u32)ktime_us_delta(ktime_get(), start);
	par->wait_us_total += us;
	if (us > par->wait_us_max)
		par->wait_us_max = us;
	if (left) {
		par->timeout_streak = 0;
		return;
	}

	par->flip_timeouts++;
	if (par->shadow_ok && !READ_ONCE(par->flip_pending)) {
		/* UPDATE_DONE came, but the shadow register never matched */
		sprd_dispc_fb_dump_flip(par,
			"shadow OSD base does not track flips, using UPDATE_DONE");
		par->shadow_ok = false;
		return;
	}
	if (par->flip_timeouts <= SPRD_FB_FLIP_MAX_DUMPS)
		sprd_dispc_fb_dump_flip(par, "flip not latched within 50 ms");
	if (++par->timeout_streak >= SPRD_FB_FLIP_MAX_TIMEOUTS) {
		dev_warn(par->dev, "flips never latch, no longer waiting for them\n");
		par->flip_sync = false;
	}
}

static int sprd_dispc_fb_pan_display(struct fb_var_screeninfo *var,
				     struct fb_info *info)
{
	struct sprd_dispc_fb *par = info->par;
	unsigned long flags;
	u32 base;

	if (var->xoffset ||
	    var->yoffset + info->var.yres > info->var.yres_virtual)
		return -EINVAL;

	base = info->fix.smem_start + var->yoffset * info->fix.line_length;
	if (base == par->cur_base)
		return 0;

	sprd_dispc_fb_wait_flip(par);

	spin_lock_irqsave(&par->lock, flags);
	writel(base, par->regs + DISPC_OSD_BASE_ADDR);
	writel(readl(par->regs + DISPC_DPI_CTRL) | DISPC_DPI_REG_UPDATE,
	       par->regs + DISPC_DPI_CTRL);
	par->cur_base = base;
	par->flip_pending = true;
	spin_unlock_irqrestore(&par->lock, flags);

	if (++par->flips == 1)
		dev_info(par->dev, "page flipping started (%u pages, %s)\n",
			 par->num_pages, !par->flip_sync ? "unsynchronized" :
			 par->shadow_ok ? "shadow-register sync" :
			 "UPDATE_DONE sync");
	else if (!(par->flips % SPRD_FB_STATS_INTERVAL))
		sprd_dispc_fb_log_stats(par);
	return 0;
}

static void sprd_dispc_fb_set_bitfields(struct fb_var_screeninfo *var,
					bool rgba)
{
	var->bits_per_pixel = 32;
	var->red.length = 8;
	var->green.length = 8;
	var->blue.length = 8;
	var->transp.length = 8;
	var->red.msb_right = 0;
	var->green.msb_right = 0;
	var->blue.msb_right = 0;
	var->transp.msb_right = 0;
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

/*
 * The mode is fixed by the bootloader: only the vertical pan offset can
 * change.  Everything else is normalized to the current mode, and
 * yres_virtual always covers all pages (gralloc derives its number of
 * framebuffers from it).
 */
static int sprd_dispc_fb_check_var(struct fb_var_screeninfo *var,
				   struct fb_info *info)
{
	struct sprd_dispc_fb *par = info->par;
	u32 yres_max = info->var.yres * par->num_pages;

	if (var->xres != info->var.xres || var->yres != info->var.yres ||
	    var->xoffset || var->yoffset > yres_max - var->yres)
		return -EINVAL;

	var->xres_virtual = info->var.xres;
	var->yres_virtual = yres_max;
	sprd_dispc_fb_set_bitfields(var, par->rgba);
	var->grayscale = 0;
	var->nonstd = 0;
	var->height = info->var.height;
	var->width = info->var.width;
	var->pixclock = info->var.pixclock;
	var->left_margin = info->var.left_margin;
	var->right_margin = info->var.right_margin;
	var->upper_margin = info->var.upper_margin;
	var->lower_margin = info->var.lower_margin;
	var->hsync_len = info->var.hsync_len;
	var->vsync_len = info->var.vsync_len;
	var->sync = info->var.sync;
	var->vmode = FB_VMODE_NONINTERLACED;
	var->rotate = 0;
	return 0;
}

static int sprd_dispc_fb_ioctl(struct fb_info *info, unsigned int cmd,
			       unsigned long arg)
{
	struct sprd_dispc_fb *par = info->par;
	int ret;

	switch (cmd) {
	case FBIO_WAITFORVSYNC:
		/*
		 * The fb core calls fb_ioctl() with info->lock held.  Keeping
		 * it for up to a frame would stall page flips from the
		 * composer behind the HWC vsync thread, so drop it while
		 * waiting (the Spreadtrum 3.10 kernel calls fb_ioctl()
		 * unlocked).  The wait touches no fb_info state.
		 * The Spreadtrum HWC passes a NULL argument: never touch it.
		 */
		mutex_unlock(&info->lock);
		ret = sprd_dispc_fb_wait_vsync(par);
		mutex_lock(&info->lock);
		return ret;
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
	.fb_check_var	= sprd_dispc_fb_check_var,
	.fb_setcolreg	= sprd_dispc_fb_setcolreg,
	.fb_blank	= sprd_dispc_fb_blank,
	.fb_pan_display	= sprd_dispc_fb_pan_display,
	.fb_ioctl	= sprd_dispc_fb_ioctl,
	.fb_fillrect	= cfb_fillrect,
	.fb_copyarea	= cfb_copyarea,
	.fb_imageblit	= cfb_imageblit,
};

static ssize_t flip_stats_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct fb_info *info = dev_get_drvdata(dev);
	struct sprd_dispc_fb *par = info->par;
	unsigned int mhz = sprd_dispc_fb_vsync_mhz(par);

	return scnprintf(buf, PAGE_SIZE,
			 "pages %u\nsync %s\nflips %lu\nwaits %lu\navg_wait_us %llu\nmax_wait_us %u\ntimeouts %lu\nupdate_done %lu\nvsyncs %u\nvsync_hz %u.%03u\nosd_base %08x\nshadow_osd_base %08x\n",
			 par->num_pages,
			 !par->flip_sync ? "off" :
			 par->shadow_ok ? "shadow" : "update_done",
			 par->flips, par->flip_waits,
			 par->flip_waits ? div64_u64(par->wait_us_total,
						     par->flip_waits) : 0ULL,
			 par->wait_us_max, par->flip_timeouts,
			 READ_ONCE(par->update_irqs),
			 READ_ONCE(par->vsync_count), mhz / 1000, mhz % 1000,
			 readl(par->regs + DISPC_OSD_BASE_ADDR),
			 readl(par->regs + DISPC_SHDW_OSD_BASE_ADDR));
}
static DEVICE_ATTR_RO(flip_stats);

/* Mask our interrupts and release the IRQ before par goes away. */
static void sprd_dispc_fb_stop_irq(struct sprd_dispc_fb *par)
{
	if (!par->irq_on)
		return;
	writel(readl(par->regs + DISPC_INT_EN) & ~DISPC_INT_USED,
	       par->regs + DISPC_INT_EN);
	devm_free_irq(par->dev, par->irq, par);
	par->irq_on = false;
	par->hw_vsync = false;
	par->flip_sync = false;
}

static int sprd_dispc_fb_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *mem_np;
	struct sprd_dispc_fb *par;
	struct fb_info *info;
	struct resource *res, mem;
	u32 width, height, stride, frame;
	u32 osd_ctrl, osd_base, osd_pitch, shdw_base;
	unsigned int num_pages;
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
	frame = stride * height;
	if (!frame || resource_size(&mem) < frame) {
		dev_err(dev, "memory-region too small\n");
		return -EINVAL;
	}
	num_pages = min_t(resource_size_t, SPRD_FB_MAX_PAGES,
			  resource_size(&mem) / frame);

	info = framebuffer_alloc(sizeof(*par), dev);
	if (!info)
		return -ENOMEM;
	platform_set_drvdata(pdev, info);
	par = info->par;
	par->dev = dev;
	par->num_pages = num_pages;
	init_waitqueue_head(&par->vsync_wq);
	spin_lock_init(&par->lock);

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
	osd_pitch = readl(par->regs + DISPC_OSD_PITCH);
	shdw_base = readl(par->regs + DISPC_SHDW_OSD_BASE_ADDR);
	dev_info(dev,
		 "bootloader state: CTRL=%08x DPI_CTRL=%08x INT_EN=%08x INT_RAW=%08x OSD_CTRL=%08x OSD_BASE=%08x SHDW_OSD_BASE=%08x OSD_SIZE=%08x OSD_PITCH=%08x OSD_DISP_XY=%08x IMG_CTRL=%08x IMG_BASE=%08x SIZE=%08x\n",
		 readl(par->regs + DISPC_CTRL),
		 readl(par->regs + DISPC_DPI_CTRL),
		 readl(par->regs + DISPC_INT_EN),
		 readl(par->regs + DISPC_INT_RAW),
		 osd_ctrl, osd_base, shdw_base,
		 readl(par->regs + DISPC_OSD_SIZE_XY), osd_pitch,
		 readl(par->regs + DISPC_OSD_DISP_XY),
		 readl(par->regs + DISPC_IMG_CTRL),
		 readl(par->regs + DISPC_IMG_Y_BASE_ADDR),
		 readl(par->regs + DISPC_SIZE_XY));
	/* the OSD pitch is in pixels; page offsets assume stride bytes/line */
	if (osd_pitch != stride / 4)
		dev_warn(dev, "OSD pitch %u px does not match stride %u\n",
			 osd_pitch, stride);

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
	par->rgba = rgba;

	/*
	 * Flips start from what the bootloader is scanning out.  Its shadow
	 * register has latched that long ago: if it reads back the same
	 * address, it can tell when later flips have been latched.
	 */
	par->cur_base = osd_base;
	par->shadow_ok = osd_base && shdw_base == osd_base;

	info->fix.smem_start = mem.start;
	info->fix.smem_len = frame * num_pages;
	info->fix.line_length = stride;
	info->fix.ypanstep = 1;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.accel = FB_ACCEL_NONE;
	strlcpy(info->fix.id, "sprd-dispc", sizeof(info->fix.id));

	info->var.xres = width;
	info->var.yres = height;
	info->var.xres_virtual = width;
	info->var.yres_virtual = height * num_pages;
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
	info->flags = FBINFO_DEFAULT | FBINFO_HWACCEL_YPAN;
	info->pseudo_palette = par->palette;
	info->screen_base = devm_ioremap_wc(dev, info->fix.smem_start,
					    info->fix.smem_len);
	if (!info->screen_base) {
		ret = -ENOMEM;
		goto err_release;
	}
	info->screen_size = info->fix.smem_len;

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
			writel(readl(par->regs + DISPC_INT_EN) | DISPC_INT_USED,
			       par->regs + DISPC_INT_EN);
			par->irq_on = true;
			par->hw_vsync = true;
			/* only interrupts can tell us that a flip latched */
			par->flip_sync = true;
		}
	}

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(dev, "register_framebuffer failed: %d\n", ret);
		goto err_irq;
	}

	if (device_create_file(dev, &dev_attr_flip_stats))
		dev_warn(dev, "cannot create flip_stats\n");

	dev_info(dev, "fb%d: %ux%u %s @ %pa, %u pages, %s vsync, %s flip sync\n",
		 info->node, width, height, rgba ? "RGBA" : "BGRA", &mem.start,
		 num_pages, par->hw_vsync ? "DPI irq" : "software",
		 !par->flip_sync ? "no" :
		 par->shadow_ok ? "shadow-register" : "UPDATE_DONE");
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

	device_remove_file(&pdev->dev, &dev_attr_flip_stats);
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
