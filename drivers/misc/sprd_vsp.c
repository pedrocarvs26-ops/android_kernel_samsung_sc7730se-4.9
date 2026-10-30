// SPDX-License-Identifier: GPL-2.0
/*
 * Spreadtrum SC7730SE (TShark) VSP video codec, /dev/sprd_vsp.
 *
 * The codec is programmed from userspace (the OMX components and codec
 * libraries of hardware/sprd): they map the registers, take the engine,
 * start a frame and sleep in VSP_COMPLETE until its done or error
 * interrupt.  The ioctl numbers and semantics are those of the 3.10
 * vendor driver (include/video/sprd_vsp.h there).
 *
 * The block lives in the MM power domain: clk_mm_i keeps the domain up
 * while the device is open, clk_vsp gates and selects the codec clock and
 * its inputs "level0".."level3" are the CONFIG_FREQ/GET_FREQ levels.
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

/* 3.10 vendor ABI; VSP_ACQUIRE is spelt VSP_ACQUAIRE there */
#define VSP_IOCTL_MAGIC		'm'
#define VSP_CONFIG_FREQ		_IOW(VSP_IOCTL_MAGIC, 1, unsigned int)
#define VSP_GET_FREQ		_IOR(VSP_IOCTL_MAGIC, 2, unsigned int)
#define VSP_ENABLE		_IO(VSP_IOCTL_MAGIC, 3)
#define VSP_DISABLE		_IO(VSP_IOCTL_MAGIC, 4)
#define VSP_ACQUIRE		_IO(VSP_IOCTL_MAGIC, 5)
#define VSP_RELEASE		_IO(VSP_IOCTL_MAGIC, 6)
#define VSP_COMPLETE		_IO(VSP_IOCTL_MAGIC, 7)
#define VSP_RESET		_IO(VSP_IOCTL_MAGIC, 8)
#define VSP_HW_INFO		_IO(VSP_IOCTL_MAGIC, 9)
#define VSP_VERSION		_IO(VSP_IOCTL_MAGIC, 10)

/* VSP_VERSION answer: the vendor's chip enum (0 SHARK, 1 DOLPHIN, ...) */
#define VSP_VERSION_TSHARK	2

/* VSP_COMPLETE status flags on top of the interrupt bit */
#define VSP_STATUS_SIGNAL	BIT(30)
#define VSP_STATUS_TIMEOUT	BIT(31)

#define VSP_ACQUIRE_TIMEOUT_MS	500
#define VSP_COMPLETE_TIMEOUT_MS	200
#define VSP_MAX_LEVELS		4

/* ARM side of the OpenRISC interface */
#define VSP_ARM_INT_STS		0x10
#define VSP_ARM_INT_CLR		0x18
#define VSP_ARM_INT_ACC		BIT(2)
#define VSP_ARM_INT_ALL		(BIT(0) | BIT(1) | BIT(2))

/* codec global registers; one completion per interrupt, lowest bit first */
#define VSP_GLB_INT_STS		0x1000
#define VSP_GLB_INT_CLR		0x1008
#define VSP_INT_BSM_OVF		BIT(0)
#define VSP_INT_VLC_DONE	BIT(1)
#define VSP_INT_MBW_DONE	BIT(2)
#define VSP_INT_VLD_ERR		BIT(4)
#define VSP_INT_TIMEOUT		BIT(5)
#define VSP_INT_ALL		(VSP_INT_BSM_OVF | VSP_INT_VLC_DONE | \
				 VSP_INT_MBW_DONE | VSP_INT_VLD_ERR | \
				 VSP_INT_TIMEOUT)

/* MM AHB soft reset, through its set/clear aliases (shared register) */
#define MM_AHB_RST_SET		((void __iomem *)REGS_MM_AHB_BASE + 0x1004)
#define MM_AHB_RST_CLR		((void __iomem *)REGS_MM_AHB_BASE + 0x2004)
#define MM_AHB_VSP_RST		BIT(4)
#define AON_APB_EB0		((void __iomem *)REGS_AON_APB_BASE)

struct sprd_vsp;

struct sprd_vsp_fh {
	struct sprd_vsp *vsp;
	wait_queue_head_t wait;
	/* status and done are protected by vsp->lock */
	u32 status;
	bool done;
	bool acquired;
	/* protected by vsp->clk_lock */
	bool clk_on;
};

struct sprd_vsp {
	struct device *dev;
	void __iomem *base;
	phys_addr_t phys;
	resource_size_t size;
	int irq;
	u32 version;

	struct clk *clk_mm;
	struct clk *clk_vsp;
	struct clk *levels[VSP_MAX_LEVELS];
	unsigned int nlevels;
	/* the last CONFIG_FREQ level, applied again on every open */
	unsigned int level;

	/* the codec engine, VSP_ACQUIRE .. VSP_RELEASE */
	struct semaphore engine;
	/* protects owner and the owner's completion state */
	spinlock_t lock;
	struct sprd_vsp_fh *owner;

	/* serializes codec clock users; the irq is unmasked while clocked */
	struct mutex clk_lock;
	unsigned int clk_users;
	struct wakeup_source *ws;

	struct miscdevice misc;
};

/* As the 3.10 driver does after a timeout; BSM overflow is left alone. */
static void sprd_vsp_clear_irqs(struct sprd_vsp *vsp)
{
	writel(VSP_INT_ALL & ~VSP_INT_BSM_OVF, vsp->base + VSP_GLB_INT_CLR);
	writel(VSP_ARM_INT_ALL, vsp->base + VSP_ARM_INT_CLR);
}

static irqreturn_t sprd_vsp_isr(int irq, void *data)
{
	struct sprd_vsp *vsp = data;
	struct sprd_vsp_fh *fh;
	u32 sts, bit;

	sts = readl(vsp->base + VSP_GLB_INT_STS) & VSP_INT_ALL;
	if (!sts)
		return IRQ_NONE;
	bit = sts & -sts;
	writel(bit, vsp->base + VSP_GLB_INT_CLR);
	if (readl(vsp->base + VSP_ARM_INT_STS) & VSP_ARM_INT_ACC)
		writel(VSP_ARM_INT_ACC, vsp->base + VSP_ARM_INT_CLR);

	spin_lock(&vsp->lock);
	fh = vsp->owner;
	if (fh) {
		fh->status = bit;
		fh->done = true;
		wake_up_interruptible(&fh->wait);
	}
	spin_unlock(&vsp->lock);
	return IRQ_HANDLED;
}

static int sprd_vsp_clk_on(struct sprd_vsp *vsp, struct sprd_vsp_fh *fh)
{
	int ret = 0;

	mutex_lock(&vsp->clk_lock);
	if (fh->clk_on)
		goto out;
	ret = clk_prepare_enable(vsp->clk_vsp);
	if (ret)
		goto out;
	if (!vsp->clk_users++) {
		__pm_stay_awake(vsp->ws);
		enable_irq(vsp->irq);
	}
	fh->clk_on = true;
out:
	mutex_unlock(&vsp->clk_lock);
	return ret;
}

static void sprd_vsp_clk_off(struct sprd_vsp *vsp, struct sprd_vsp_fh *fh)
{
	mutex_lock(&vsp->clk_lock);
	if (fh->clk_on) {
		fh->clk_on = false;
		if (!--vsp->clk_users) {
			disable_irq(vsp->irq);
			/* Nothing may be left pending for the next user. */
			sprd_vsp_clear_irqs(vsp);
			__pm_relax(vsp->ws);
		}
		clk_disable_unprepare(vsp->clk_vsp);
	}
	mutex_unlock(&vsp->clk_lock);
}

static int sprd_vsp_acquire(struct sprd_vsp *vsp, struct sprd_vsp_fh *fh)
{
	int ret;

	if (READ_ONCE(fh->acquired))
		return 0;
	ret = down_timeout(&vsp->engine,
			   msecs_to_jiffies(VSP_ACQUIRE_TIMEOUT_MS));
	if (ret) {
		dev_warn_ratelimited(vsp->dev, "engine busy\n");
		return ret;
	}
	spin_lock_irq(&vsp->lock);
	vsp->owner = fh;
	fh->acquired = true;
	spin_unlock_irq(&vsp->lock);
	return 0;
}

static void sprd_vsp_release_engine(struct sprd_vsp *vsp,
				    struct sprd_vsp_fh *fh)
{
	bool held;

	spin_lock_irq(&vsp->lock);
	held = fh->acquired;
	fh->acquired = false;
	if (vsp->owner == fh)
		vsp->owner = NULL;
	spin_unlock_irq(&vsp->lock);
	if (held)
		up(&vsp->engine);
}

static long sprd_vsp_complete(struct sprd_vsp *vsp, struct sprd_vsp_fh *fh,
			      u32 __user *arg)
{
	long t;
	int ret = 0;
	u32 status;

	t = wait_event_interruptible_timeout(fh->wait, READ_ONCE(fh->done),
			msecs_to_jiffies(VSP_COMPLETE_TIMEOUT_MS));

	spin_lock_irq(&vsp->lock);
	if (t == -ERESTARTSYS) {
		/* 3.10 keeps the state and fails without a restart */
		fh->status |= VSP_STATUS_SIGNAL;
		status = fh->status;
		ret = -EINVAL;
	} else {
		fh->status &= ~VSP_STATUS_SIGNAL;
		if (!t) {
			fh->status |= VSP_STATUS_TIMEOUT;
			ret = -ETIMEDOUT;
		}
		status = fh->status;
		fh->status = 0;
		fh->done = false;
	}
	spin_unlock_irq(&vsp->lock);

	if (ret == -ETIMEDOUT) {
		dev_err_ratelimited(vsp->dev, "frame timed out\n");
		mutex_lock(&vsp->clk_lock);
		if (vsp->clk_users)
			sprd_vsp_clear_irqs(vsp);
		mutex_unlock(&vsp->clk_lock);
	}
	if (put_user(status, arg))
		return -EFAULT;
	return ret;
}

static long sprd_vsp_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long arg)
{
	struct sprd_vsp_fh *fh = filp->private_data;
	struct sprd_vsp *vsp = fh->vsp;
	unsigned int level;
	unsigned long rate;

	switch (cmd) {
	case VSP_CONFIG_FREQ:
		if (get_user(level, (unsigned int __user *)arg))
			return -EFAULT;
		if (level >= vsp->nlevels)
			level = 0;
		if (clk_set_parent(vsp->clk_vsp, vsp->levels[level]))
			return -EINVAL;
		WRITE_ONCE(vsp->level, level);
		return 0;
	case VSP_GET_FREQ:
		rate = clk_get_rate(vsp->clk_vsp);
		for (level = vsp->nlevels - 1; level; level--)
			if (clk_get_rate(vsp->levels[level]) == rate)
				break;
		return put_user(level, (unsigned int __user *)arg);
	case VSP_ENABLE:
		return sprd_vsp_clk_on(vsp, fh);
	case VSP_DISABLE:
		sprd_vsp_clk_off(vsp, fh);
		return 0;
	case VSP_ACQUIRE:
		return sprd_vsp_acquire(vsp, fh);
	case VSP_RELEASE:
		sprd_vsp_release_engine(vsp, fh);
		return 0;
	case VSP_COMPLETE:
		return sprd_vsp_complete(vsp, fh, (u32 __user *)arg);
	case VSP_RESET:
		/* the open file keeps the MM domain, and so MM AHB, up */
		writel(MM_AHB_VSP_RST, MM_AHB_RST_SET);
		writel(MM_AHB_VSP_RST, MM_AHB_RST_CLR);
		return 0;
	case VSP_HW_INFO:
		return put_user(readl(AON_APB_EB0), (u32 __user *)arg);
	case VSP_VERSION:
		return put_user(vsp->version, (u32 __user *)arg);
	default:
		return -EINVAL;
	}
}

static int sprd_vsp_mmap(struct file *filp, struct vm_area_struct *vma)
{
	struct sprd_vsp_fh *fh = filp->private_data;

	/* The register window, whatever the offset (as on 3.10). */
	vma->vm_pgoff = 0;
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	return vm_iomap_memory(vma, fh->vsp->phys, fh->vsp->size);
}

static int sprd_vsp_open(struct inode *inode, struct file *filp)
{
	struct sprd_vsp *vsp = container_of(filp->private_data,
					    struct sprd_vsp, misc);
	struct sprd_vsp_fh *fh;
	int ret;

	fh = kzalloc(sizeof(*fh), GFP_KERNEL);
	if (!fh)
		return -ENOMEM;
	fh->vsp = vsp;
	init_waitqueue_head(&fh->wait);

	ret = clk_prepare_enable(vsp->clk_mm);
	if (ret)
		goto err_free;
	ret = clk_set_parent(vsp->clk_vsp,
			     vsp->levels[READ_ONCE(vsp->level)]);
	if (ret)
		goto err_mm;
	filp->private_data = fh;
	return 0;

err_mm:
	clk_disable_unprepare(vsp->clk_mm);
err_free:
	kfree(fh);
	return ret;
}

static int sprd_vsp_release(struct inode *inode, struct file *filp)
{
	struct sprd_vsp_fh *fh = filp->private_data;
	struct sprd_vsp *vsp = fh->vsp;

	sprd_vsp_clk_off(vsp, fh);
	sprd_vsp_release_engine(vsp, fh);
	clk_disable_unprepare(vsp->clk_mm);
	kfree(fh);
	return 0;
}

static const struct file_operations sprd_vsp_fops = {
	.owner		= THIS_MODULE,
	.open		= sprd_vsp_open,
	.release	= sprd_vsp_release,
	.unlocked_ioctl	= sprd_vsp_ioctl,
	.mmap		= sprd_vsp_mmap,
};

static int sprd_vsp_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sprd_vsp *vsp;
	struct resource *res;
	unsigned int i;
	int ret;

	vsp = devm_kzalloc(dev, sizeof(*vsp), GFP_KERNEL);
	if (!vsp)
		return -ENOMEM;
	vsp->dev = dev;
	vsp->version = (unsigned long)of_device_get_match_data(dev);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	vsp->base = devm_ioremap_resource(dev, res);
	if (IS_ERR(vsp->base))
		return PTR_ERR(vsp->base);
	vsp->phys = res->start;
	vsp->size = resource_size(res);

	vsp->irq = platform_get_irq(pdev, 0);
	if (vsp->irq < 0)
		return vsp->irq;

	vsp->clk_mm = devm_clk_get(dev, "clk_mm_i");
	if (IS_ERR(vsp->clk_mm))
		return PTR_ERR(vsp->clk_mm);
	vsp->clk_vsp = devm_clk_get(dev, "clk_vsp");
	if (IS_ERR(vsp->clk_vsp))
		return PTR_ERR(vsp->clk_vsp);
	for (i = 0; i < VSP_MAX_LEVELS; i++) {
		char name[8];
		struct clk *clk;

		snprintf(name, sizeof(name), "level%u", i);
		clk = devm_clk_get(dev, name);
		if (IS_ERR(clk)) {
			if (PTR_ERR(clk) == -EPROBE_DEFER)
				return -EPROBE_DEFER;
			break;
		}
		vsp->levels[i] = clk;
	}
	vsp->nlevels = i;
	if (!vsp->nlevels) {
		dev_err(dev, "no frequency levels\n");
		return -EINVAL;
	}

	sema_init(&vsp->engine, 1);
	spin_lock_init(&vsp->lock);
	mutex_init(&vsp->clk_lock);

	/* The registers need the codec clock: unmask only while it runs. */
	irq_set_status_flags(vsp->irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(dev, vsp->irq, sprd_vsp_isr, 0, "sprd_vsp", vsp);
	if (ret)
		return ret;

	vsp->ws = wakeup_source_register("sprd_vsp");
	vsp->misc.minor = MISC_DYNAMIC_MINOR;
	vsp->misc.name = "sprd_vsp";
	vsp->misc.fops = &sprd_vsp_fops;
	vsp->misc.parent = dev;
	ret = misc_register(&vsp->misc);
	if (ret) {
		wakeup_source_unregister(vsp->ws);
		return ret;
	}

	dev_info(dev, "version %u, %u levels, level 0 at %lu Hz\n",
		 vsp->version, vsp->nlevels, clk_get_rate(vsp->levels[0]));
	return 0;
}

static const struct of_device_id sprd_vsp_of_match[] = {
	{ .compatible = "sprd,scx30g-vsp", .data = (void *)VSP_VERSION_TSHARK },
	{ }
};

static struct platform_driver sprd_vsp_driver = {
	.probe = sprd_vsp_probe,
	.driver = {
		.name = "sprd-vsp",
		.of_match_table = sprd_vsp_of_match,
		.suppress_bind_attrs = true,
	},
};
builtin_platform_driver(sprd_vsp_driver);
