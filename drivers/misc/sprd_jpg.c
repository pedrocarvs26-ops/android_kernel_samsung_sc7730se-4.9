// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC7730SE (TShark) JPEG codec, /dev/sprd_jpg.
 *
 * Like the VSP, the codec is programmed from userspace (the camera HAL and
 * the JPEG OMX/libjpeg glue of hardware/sprd), which maps the registers,
 * takes the engine and waits here for the encoder's MBIO (slice) and VLC
 * (frame) done interrupts.  The ioctl numbers and semantics are those of
 * the 3.10 vendor driver (include/video/sprd_jpg.h there), including its
 * device-wide completion state.
 *
 * clk_mm_i keeps the MM power domain up while the device is open, clk_jpg
 * gates and selects the codec clock and its inputs "level0".."level3" are
 * the CONFIG_FREQ/GET_FREQ levels.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_wakeup.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <soc/sprd/iomap.h>

/*
 * 3.10 vendor ABI; JPG_ACQUIRE is spelt JPG_ACQUAIRE there and JPG_WAIT is
 * JPG_ACQUAIRE_MBIO_VLC_DONE, which takes its argument by value.
 */
#define JPG_IOCTL_MAGIC		'm'
#define JPG_CONFIG_FREQ		_IOW(JPG_IOCTL_MAGIC, 1, unsigned int)
#define JPG_GET_FREQ		_IOR(JPG_IOCTL_MAGIC, 2, unsigned int)
#define JPG_ENABLE		_IO(JPG_IOCTL_MAGIC, 3)
#define JPG_DISABLE		_IO(JPG_IOCTL_MAGIC, 4)
#define JPG_ACQUIRE		_IO(JPG_IOCTL_MAGIC, 5)
#define JPG_RELEASE		_IO(JPG_IOCTL_MAGIC, 6)
#define JPG_START		_IO(JPG_IOCTL_MAGIC, 7)
#define JPG_RESET		_IO(JPG_IOCTL_MAGIC, 8)
#define JPG_WAIT		_IOR(JPG_IOCTL_MAGIC, 9, unsigned int)

/* JPG_WAIT arguments and results */
#define JPG_WAIT_MBIO		3
#define JPG_WAIT_VLC		1
#define JPG_WAIT_MBIO_DONE	0
#define JPG_WAIT_VLC_DONE	4
#define JPG_WAIT_FAILED		1

/* JPG_START status flags on top of the interrupt bits */
#define JPG_STATUS_SIGNAL	BIT(30)
#define JPG_STATUS_TIMEOUT	BIT(31)

#define JPG_TIMEOUT_MS		1000
#define JPG_MAX_LEVELS		4

#define JPG_INT_STS		0x20
#define JPG_INT_CLR		0x28
#define JPG_INT_BSM		BIT(0)
#define JPG_INT_VLC		BIT(1)
#define JPG_INT_MBIO		BIT(3)
#define JPG_INT_HANDLED		(JPG_INT_BSM | JPG_INT_VLC | JPG_INT_MBIO)
#define JPG_INT_ALL		(BIT(0) | BIT(1) | BIT(2) | BIT(3))

/* MM AHB soft reset, through its set/clear aliases (shared register) */
#define MM_AHB_RST_SET		((void __iomem *)REGS_MM_AHB_BASE + 0x1004)
#define MM_AHB_RST_CLR		((void __iomem *)REGS_MM_AHB_BASE + 0x2004)
#define MM_AHB_JPG_RST		BIT(6)

struct sprd_jpg;

struct sprd_jpg_fh {
	struct sprd_jpg *jpg;
	bool acquired;
	/* protected by jpg->clk_lock */
	bool clk_on;
};

struct sprd_jpg {
	struct device *dev;
	void __iomem *base;
	phys_addr_t phys;
	resource_size_t size;
	int irq;

	struct clk *clk_mm;
	struct clk *clk_jpg;
	struct clk *levels[JPG_MAX_LEVELS];
	unsigned int nlevels;
	/* the last CONFIG_FREQ level, applied again on every open */
	unsigned int level;

	/* the codec engine, JPG_ACQUIRE .. JPG_RELEASE */
	struct semaphore engine;

	/* protects the completion state below */
	spinlock_t lock;
	u32 status;
	bool mbio_done;
	bool vlc_done;
	wait_queue_head_t mbio_wait;
	wait_queue_head_t vlc_wait;

	/* serializes codec clock users; the irq is unmasked while clocked */
	struct mutex clk_lock;
	unsigned int clk_users;
	struct wakeup_source *ws;

	struct miscdevice misc;
};

static void sprd_jpg_reset_state(struct sprd_jpg *jpg)
{
	spin_lock_irq(&jpg->lock);
	jpg->status = 0;
	jpg->mbio_done = false;
	jpg->vlc_done = false;
	spin_unlock_irq(&jpg->lock);
}

static irqreturn_t sprd_jpg_isr(int irq, void *data)
{
	struct sprd_jpg *jpg = data;
	u32 sts;

	sts = readl(jpg->base + JPG_INT_STS) & JPG_INT_HANDLED;
	if (!sts)
		return IRQ_NONE;
	writel(sts, jpg->base + JPG_INT_CLR);

	spin_lock(&jpg->lock);
	jpg->status |= sts;
	if (sts & JPG_INT_MBIO) {
		jpg->mbio_done = true;
		wake_up_interruptible(&jpg->mbio_wait);
	}
	if (sts & JPG_INT_VLC) {
		jpg->vlc_done = true;
		wake_up_interruptible(&jpg->vlc_wait);
	}
	spin_unlock(&jpg->lock);
	return IRQ_HANDLED;
}

/* Only while clocked; the open file keeps the MM domain up. */
static void sprd_jpg_clear_irqs(struct sprd_jpg *jpg)
{
	mutex_lock(&jpg->clk_lock);
	if (jpg->clk_users)
		writel(JPG_INT_ALL, jpg->base + JPG_INT_CLR);
	mutex_unlock(&jpg->clk_lock);
}

static int sprd_jpg_clk_on(struct sprd_jpg *jpg, struct sprd_jpg_fh *fh)
{
	int ret = 0;

	mutex_lock(&jpg->clk_lock);
	if (fh->clk_on)
		goto out;
	ret = clk_prepare_enable(jpg->clk_jpg);
	if (ret)
		goto out;
	if (!jpg->clk_users++) {
		__pm_stay_awake(jpg->ws);
		enable_irq(jpg->irq);
	}
	fh->clk_on = true;
out:
	mutex_unlock(&jpg->clk_lock);
	return ret;
}

static void sprd_jpg_clk_off(struct sprd_jpg *jpg, struct sprd_jpg_fh *fh)
{
	mutex_lock(&jpg->clk_lock);
	if (fh->clk_on) {
		fh->clk_on = false;
		if (!--jpg->clk_users) {
			disable_irq(jpg->irq);
			/* Nothing may be left pending for the next user. */
			writel(JPG_INT_ALL, jpg->base + JPG_INT_CLR);
			__pm_relax(jpg->ws);
		}
		clk_disable_unprepare(jpg->clk_jpg);
	}
	mutex_unlock(&jpg->clk_lock);
}

static int sprd_jpg_acquire(struct sprd_jpg *jpg, struct sprd_jpg_fh *fh)
{
	int ret;

	if (READ_ONCE(fh->acquired))
		return 0;
	ret = down_timeout(&jpg->engine, msecs_to_jiffies(JPG_TIMEOUT_MS));
	if (ret) {
		dev_warn_ratelimited(jpg->dev, "engine busy\n");
		return ret;
	}
	WRITE_ONCE(fh->acquired, true);
	return 0;
}

static void sprd_jpg_release_engine(struct sprd_jpg *jpg,
				    struct sprd_jpg_fh *fh)
{
	if (xchg(&fh->acquired, false))
		up(&jpg->engine);
}

/* JPG_START: wait for the frame (VLC done), report and reset the state. */
static long sprd_jpg_start(struct sprd_jpg *jpg, u32 __user *arg)
{
	long t;
	int ret = 0;
	u32 status;

	t = wait_event_interruptible_timeout(jpg->vlc_wait,
					     READ_ONCE(jpg->vlc_done),
					     msecs_to_jiffies(JPG_TIMEOUT_MS));

	spin_lock_irq(&jpg->lock);
	if (t == -ERESTARTSYS) {
		jpg->status |= JPG_STATUS_SIGNAL;
		ret = -EINVAL;
	} else if (!t) {
		jpg->status |= JPG_STATUS_TIMEOUT;
		ret = -ETIMEDOUT;
	}
	status = jpg->status;
	spin_unlock_irq(&jpg->lock);

	if (ret) {
		dev_err_ratelimited(jpg->dev, "start: %d\n", ret);
		sprd_jpg_clear_irqs(jpg);
	}
	sprd_jpg_reset_state(jpg);
	if (put_user(status, arg))
		return -EFAULT;
	return ret;
}

/* JPG_WAIT: one MBIO or VLC completion; returns 0/4 when done, else 1. */
static long sprd_jpg_wait(struct sprd_jpg *jpg, unsigned long what)
{
	wait_queue_head_t *wq;
	bool *done;
	u32 bit;
	long t;

	if (what == JPG_WAIT_MBIO) {
		wq = &jpg->mbio_wait;
		done = &jpg->mbio_done;
		bit = JPG_INT_MBIO;
	} else if (what == JPG_WAIT_VLC) {
		wq = &jpg->vlc_wait;
		done = &jpg->vlc_done;
		bit = JPG_INT_VLC;
	} else {
		return -EINVAL;
	}

	t = wait_event_interruptible_timeout(*wq, READ_ONCE(*done),
					     msecs_to_jiffies(JPG_TIMEOUT_MS));
	if (t <= 0) {
		dev_err_ratelimited(jpg->dev, "wait %lu: %ld\n", what,
				    t ? t : -ETIMEDOUT);
		sprd_jpg_clear_irqs(jpg);
	}

	spin_lock_irq(&jpg->lock);
	jpg->status &= ~bit;
	*done = false;
	spin_unlock_irq(&jpg->lock);

	if (t <= 0)
		return JPG_WAIT_FAILED;
	return what == JPG_WAIT_MBIO ? JPG_WAIT_MBIO_DONE : JPG_WAIT_VLC_DONE;
}

static long sprd_jpg_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct sprd_jpg_fh *fh = filp->private_data;
	struct sprd_jpg *jpg = fh->jpg;
	unsigned int level;
	unsigned long rate;

	switch (cmd) {
	case JPG_CONFIG_FREQ:
		if (get_user(level, (unsigned int __user *)arg))
			return -EFAULT;
		if (level >= jpg->nlevels)
			level = 0;
		if (clk_set_parent(jpg->clk_jpg, jpg->levels[level]))
			return -EINVAL;
		WRITE_ONCE(jpg->level, level);
		return 0;
	case JPG_GET_FREQ:
		rate = clk_get_rate(jpg->clk_jpg);
		for (level = jpg->nlevels - 1; level; level--)
			if (clk_get_rate(jpg->levels[level]) == rate)
				break;
		return put_user(level, (unsigned int __user *)arg);
	case JPG_ENABLE:
		return sprd_jpg_clk_on(jpg, fh);
	case JPG_DISABLE:
		sprd_jpg_clk_off(jpg, fh);
		return 0;
	case JPG_ACQUIRE:
		return sprd_jpg_acquire(jpg, fh);
	case JPG_RELEASE:
		sprd_jpg_release_engine(jpg, fh);
		return 0;
	case JPG_START:
		return sprd_jpg_start(jpg, (u32 __user *)arg);
	case JPG_RESET:
		writel(MM_AHB_JPG_RST, MM_AHB_RST_SET);
		writel(MM_AHB_JPG_RST, MM_AHB_RST_CLR);
		return 0;
	case JPG_WAIT:
		return sprd_jpg_wait(jpg, arg);
	default:
		return -EINVAL;
	}
}

static int sprd_jpg_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sprd_jpg_fh *fh = filp->private_data;

	/* The register window, whatever the offset (as on 3.10). */
	vma->vm_pgoff = 0;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return vm_iomap_memory(vma, fh->jpg->phys, fh->jpg->size);
}

static int sprd_jpg_open(struct inode *inode, struct file *filp)
{
	struct sprd_jpg *jpg = container_of(filp->private_data,
					    struct sprd_jpg, misc);
	struct sprd_jpg_fh *fh;
	int ret;

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	if (!fh)
		return -ENOMEM;
	fh->jpg = jpg;

	ret = clk_prepare_enable(jpg->clk_mm);
	if (ret)
		goto err_free;
	ret = clk_set_parent(jpg->clk_jpg,
			     jpg->levels[READ_ONCE(jpg->level)]);
	if (ret)
		goto err_mm;
	/* 3.10 starts every open from a clean completion state */
	sprd_jpg_reset_state(jpg);
	filp->private_data = fh;
	return 0;

err_mm:
	clk_disable_unprepare(jpg->clk_mm);
err_free:
	kfree(fh);
	return ret;
}

static int sprd_jpg_release(struct inode *inode, struct file *filp)
{
	struct sprd_jpg_fh *fh = filp->private_data;
	struct sprd_jpg *jpg = fh->jpg;

	sprd_jpg_clk_off(jpg, fh);
	sprd_jpg_release_engine(jpg, fh);
	clk_disable_unprepare(jpg->clk_mm);
	kfree(fh);
	return 0;
}

static const struct file_operations sprd_jpg_fops = {
	.owner		= THIS_MODULE,
	.open		= sprd_jpg_open,
	.release	= sprd_jpg_release,
	.unlocked_ioctl	= sprd_jpg_ioctl,
	.mmap		= sprd_jpg_mmap,
};

static int sprd_jpg_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_jpg *jpg;
	struct resource *res;
	unsigned int i;
	int ret;

	jpg = devm_kzalloc(dev, sizeof(*jpg), GFP_KERNEL);
	if (!jpg)
		return -ENOMEM;
	jpg->dev = dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	jpg->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(jpg->base))
		return PTR_ERR(jpg->base);
	jpg->phys = res->start;
	jpg->size = resource_size(res);

	jpg->irq = platform_get_irq(pdev, 0);
	if (jpg->irq < 0)
		return jpg->irq;

	jpg->clk_mm = devm_clk_get(dev, "clk_mm_i");
	if (IS_ERR(jpg->clk_mm))
		return PTR_ERR(jpg->clk_mm);
	jpg->clk_jpg = devm_clk_get(dev, "clk_jpg");
	if (IS_ERR(jpg->clk_jpg))
		return PTR_ERR(jpg->clk_jpg);
	for (i = 0; i < JPG_MAX_LEVELS; i++) {
		char name[8];
		struct clk *clk;

		snprintf(name, sizeof(name), "level%u", i);
		clk = devm_clk_get(dev, name);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			break;
		}
		jpg->levels[i] = clk;
	}
	jpg->nlevels = i;
	if (!jpg->nlevels) {
		dev_err(dev, "no frequency levels\n");
		return -EINVAL;
	}

	sema_init(&jpg->engine, 1);
	spin_lock_init(&jpg->lock);
	init_waitqueue_head(&jpg->mbio_wait);
	init_waitqueue_head(&jpg->vlc_wait);
	mutex_init(&jpg->clk_lock);

	/* The registers need the codec clock: unmask only while it runs. */
	irq_set_status_flags(jpg->irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, jpg->irq, sprd_jpg_isr, 0, "sprd_jpg", jpg);
	if (ret)
		return ret;

	jpg->ws = wakeup_source_register("sprd_jpg");
	jpg->misc.minor = MISC_DYNAMIC_MINOR;
	jpg->misc.name = "sprd_jpg";
	jpg->misc.fops = &sprd_jpg_fops;
	jpg->misc.parent = dev;
	ret = misc_register(&jpg->misc);
	if (ret) {
		wakeup_source_unregister(jpg->ws);
		return ret;
	}

	dev_info(dev, "%u levels, level 0 at %lu Hz\n", jpg->nlevels,
		 clk_get_rate(jpg->levels[0]));
	return 0;
}

static const struct of_device_id sprd_jpg_of_match[] = {
	{ .compatible = "sprd,scx30g-jpg" },
	{ }
};

static struct platform_driver sprd_jpg_driver = {
	.probe = sprd_jpg_probe,
	.driver = {
		.name = "sprd-jpg",
		.of_match_table = sprd_jpg_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sprd_jpg_driver);
