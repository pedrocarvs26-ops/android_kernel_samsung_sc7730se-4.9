/*
 * Broadcom Bluetooth low power mode driver (BCM43430 on SM-T560/T561).
 *
 * Supports both N_BRCM_HCI (25) line discipline ioctls and direct UART tty
 * ioctls (0x8003..0x8005) used by libbt-vendor when BT_WAKE_VIA_USERIAL_IOCTL
 * is enabled without BT_WAKE_USERIAL_LDISC.
 *
 * Copyright (C) 2013 Broadcom Corporation
 * Licensed under the GPL-2.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/irq.h>
#include <linux/gpio.h>
#include <linux/broadcom/bcm-bt-lpm.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/delay.h>
#include <linux/pm_wakeup.h>
#include <linux/irq.h>
#include <linux/wakelock.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>

#define N_BRCM_HCI 25

#ifndef BCM_BT_LPM_BT_WAKE_ASSERT
#define BCM_BT_LPM_BT_WAKE_ASSERT 0
#endif

#ifndef BCM_BT_LPM_BT_WAKE_DEASSERT
#define BCM_BT_LPM_BT_WAKE_DEASSERT (!(BCM_BT_LPM_BT_WAKE_ASSERT))
#endif

#ifndef BCM_BT_LPM_HOST_WAKE_ASSERT
#define BCM_BT_LPM_HOST_WAKE_ASSERT 0
#endif

#ifndef BCM_BT_LPM_HOST_WAKE_DEASSERT
#define BCM_BT_LPM_HOST_WAKE_DEASSERT (!(BCM_BT_LPM_HOST_WAKE_ASSERT))
#endif

#define BCM_SHARED_UART_MAGIC	0x80
#define TIO_ASSERT_BT_WAKE	_IO(BCM_SHARED_UART_MAGIC, 3)
#define TIO_DEASSERT_BT_WAKE	_IO(BCM_SHARED_UART_MAGIC, 4)
#define TIO_GET_BT_WAKE_STATE	_IO(BCM_SHARED_UART_MAGIC, 5)

struct bcm_bt_lpm_struct {
	spinlock_t bcm_bt_lpm_lock;
	struct uart_port *uport;
	int host_irq;
	bool gpios_ready;
	bool irq_ready;
};

struct bcm_bt_lpm_entry_struct {
	struct bcm_bt_lpm_platform_data *pdata;
	struct bcm_bt_lpm_struct *plpm;
	struct platform_device *pdev;
	struct wake_lock bt_wakelock;
	struct wake_lock host_wakelock;
};

static struct bcm_bt_lpm_entry_struct *priv_g;

struct bcm_bt_lpm_ldisc_data {
	int (*open)(struct tty_struct *);
	void (*close)(struct tty_struct *);
};

static struct tty_ldisc_ops bcm_bt_lpm_ldisc_ops;
static struct bcm_bt_lpm_ldisc_data bcm_bt_lpm_ldisc_saved;

int bcm_bt_lpm_assert_bt_wake(void)
{
	if (!priv_g || !priv_g->pdata || priv_g->pdata->bt_wake_gpio < 0)
		return -EFAULT;

	wake_lock(&priv_g->bt_wakelock);
	gpio_set_value(priv_g->pdata->bt_wake_gpio, BCM_BT_LPM_BT_WAKE_ASSERT);
	return 0;
}
EXPORT_SYMBOL(bcm_bt_lpm_assert_bt_wake);

int bcm_bt_lpm_deassert_bt_wake(void)
{
	if (!priv_g || !priv_g->pdata || priv_g->pdata->bt_wake_gpio < 0)
		return -EFAULT;

	gpio_set_value(priv_g->pdata->bt_wake_gpio, BCM_BT_LPM_BT_WAKE_DEASSERT);
	wake_unlock(&priv_g->bt_wakelock);
	return 0;
}
EXPORT_SYMBOL(bcm_bt_lpm_deassert_bt_wake);

static irqreturn_t bcm_bt_lpm_host_wake_isr(int irq, void *dev)
{
	unsigned int host_wake;
	unsigned long flags;
	struct bcm_bt_lpm_entry_struct *priv = dev;

	if (!priv)
		return IRQ_HANDLED;

	spin_lock_irqsave(&priv->plpm->bcm_bt_lpm_lock, flags);
	host_wake = gpio_get_value(priv->pdata->host_wake_gpio);
	if (BCM_BT_LPM_HOST_WAKE_ASSERT == host_wake) {
		wake_lock(&priv->host_wakelock);
		irq_set_irq_type(priv->plpm->host_irq, IRQF_TRIGGER_HIGH);
	} else {
		wake_unlock(&priv->host_wakelock);
		irq_set_irq_type(priv->plpm->host_irq, IRQF_TRIGGER_LOW);
	}
	spin_unlock_irqrestore(&priv->plpm->bcm_bt_lpm_lock, flags);

	return IRQ_HANDLED;
}

static int bcm_bt_lpm_ensure_gpios_and_irq(struct bcm_bt_lpm_entry_struct *priv)
{
	int rc;

	if (!priv || !priv->pdata || !priv->plpm)
		return -EFAULT;

	if (!priv->plpm->gpios_ready) {
		rc = devm_gpio_request_one(&priv->pdev->dev,
					   priv->pdata->bt_wake_gpio,
					   GPIOF_OUT_INIT_HIGH,
					   "bt_wake_gpio");
		if (rc) {
			pr_err("%s: failed to request bt_wake_gpio=%d: %d\n",
			       __func__, priv->pdata->bt_wake_gpio, rc);
			return rc;
		}
		gpio_direction_output(priv->pdata->bt_wake_gpio,
				      BCM_BT_LPM_BT_WAKE_DEASSERT);

		rc = devm_gpio_request_one(&priv->pdev->dev,
					   priv->pdata->host_wake_gpio,
					   GPIOF_IN,
					   "host_wake_gpio");
		if (rc) {
			pr_err("%s: failed to request host_wake_gpio=%d: %d\n",
			       __func__, priv->pdata->host_wake_gpio, rc);
			return rc;
		}
		gpio_direction_input(priv->pdata->host_wake_gpio);
		priv->plpm->gpios_ready = true;
	}

	if (!priv->plpm->irq_ready) {
		rc = gpio_to_irq(priv->pdata->host_wake_gpio);
		if (rc >= 0) {
			priv->plpm->host_irq = rc;
			rc = request_irq(priv->plpm->host_irq,
					 bcm_bt_lpm_host_wake_isr,
					 IRQF_TRIGGER_LOW | IRQF_NO_SUSPEND,
					 "bt_host_wake", priv);
			if (!rc)
				priv->plpm->irq_ready = true;
		}
	}

	return 0;
}

static int bcm_bt_get_bt_wake_state(unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	unsigned long tmp;

	if (!priv_g || !priv_g->pdata || priv_g->pdata->bt_wake_gpio < 0)
		return -EFAULT;

	tmp = gpio_get_value(priv_g->pdata->bt_wake_gpio);
	if (copy_to_user(uarg, &tmp, sizeof(tmp)))
		return -EFAULT;
	return 0;
}

int bcm_bt_lpm_uart_ioctl(struct uart_port *uport, unsigned int cmd,
			  unsigned long arg)
{
	switch (cmd) {
	case TIO_ASSERT_BT_WAKE:
		bcm_bt_lpm_ensure_gpios_and_irq(priv_g);
		return bcm_bt_lpm_assert_bt_wake();
	case TIO_DEASSERT_BT_WAKE:
		bcm_bt_lpm_ensure_gpios_and_irq(priv_g);
		return bcm_bt_lpm_deassert_bt_wake();
	case TIO_GET_BT_WAKE_STATE:
		bcm_bt_lpm_ensure_gpios_and_irq(priv_g);
		return bcm_bt_get_bt_wake_state(arg);
	default:
		return -ENOIOCTLCMD;
	}
}
EXPORT_SYMBOL(bcm_bt_lpm_uart_ioctl);

static int bcm_bt_lpm_tty_ioctl(struct tty_struct *tty, struct file *file,
				unsigned int cmd, unsigned long arg)
{
	int rc = bcm_bt_lpm_uart_ioctl(NULL, cmd, arg);
	if (rc != -ENOIOCTLCMD)
		return rc;
	return n_tty_ioctl_helper(tty, file, cmd, arg);
}

static int bcm_bt_lpm_tty_open(struct tty_struct *tty)
{
	struct uart_state *state = tty->driver_data;

	if (!priv_g)
		return -EFAULT;

	if (state)
		priv_g->plpm->uport = state->uart_port;

	bcm_bt_lpm_ensure_gpios_and_irq(priv_g);

	if (bcm_bt_lpm_ldisc_saved.open)
		return bcm_bt_lpm_ldisc_saved.open(tty);
	return 0;
}

static void bcm_bt_lpm_tty_close(struct tty_struct *tty)
{
	if (!priv_g || !priv_g->plpm)
		return;

	priv_g->plpm->uport = NULL;
	if (bcm_bt_lpm_ldisc_saved.close)
		bcm_bt_lpm_ldisc_saved.close(tty);
}

static int bcm_bt_lpm_tty_init(void)
{
	int err;

	n_tty_inherit_ops(&bcm_bt_lpm_ldisc_ops);
	bcm_bt_lpm_ldisc_ops.owner = THIS_MODULE;
	bcm_bt_lpm_ldisc_ops.name = "bcm_bt_lpm_tty";
	bcm_bt_lpm_ldisc_ops.ioctl = bcm_bt_lpm_tty_ioctl;
	bcm_bt_lpm_ldisc_saved.open = bcm_bt_lpm_ldisc_ops.open;
	bcm_bt_lpm_ldisc_saved.close = bcm_bt_lpm_ldisc_ops.close;
	bcm_bt_lpm_ldisc_ops.open = bcm_bt_lpm_tty_open;
	bcm_bt_lpm_ldisc_ops.close = bcm_bt_lpm_tty_close;

	err = tty_register_ldisc(N_BRCM_HCI, &bcm_bt_lpm_ldisc_ops);
	if (err)
		pr_err("can't register N_BRCM_HCI line discipline\n");
	return err;
}

static const struct of_device_id bcm_bt_lpm_of_match[] = {
	{ .compatible = "bcm,bcm-bt-lpm", },
	{ },
};
MODULE_DEVICE_TABLE(of, bcm_bt_lpm_of_match);

static int bcm_bt_lpm_probe(struct platform_device *pdev)
{
	int rc;

	priv_g = devm_kzalloc(&pdev->dev, sizeof(*priv_g), GFP_KERNEL);
	if (!priv_g)
		return -ENOMEM;

	priv_g->pdata = devm_kzalloc(&pdev->dev, sizeof(*priv_g->pdata),
				     GFP_KERNEL);
	if (!priv_g->pdata)
		return -ENOMEM;

	priv_g->plpm = devm_kzalloc(&pdev->dev, sizeof(*priv_g->plpm),
				    GFP_KERNEL);
	if (!priv_g->plpm)
		return -ENOMEM;

	spin_lock_init(&priv_g->plpm->bcm_bt_lpm_lock);

	if (pdev->dev.of_node) {
		priv_g->pdata->bt_wake_gpio = of_get_gpio(pdev->dev.of_node, 0);
		if (!gpio_is_valid(priv_g->pdata->bt_wake_gpio))
			return priv_g->pdata->bt_wake_gpio;

		priv_g->pdata->host_wake_gpio = of_get_gpio(pdev->dev.of_node, 1);
		if (!gpio_is_valid(priv_g->pdata->host_wake_gpio))
			return priv_g->pdata->host_wake_gpio;
	} else if (pdev->dev.platform_data) {
		struct bcm_bt_lpm_platform_data *pdata = pdev->dev.platform_data;
		priv_g->pdata->bt_wake_gpio = pdata->bt_wake_gpio;
		priv_g->pdata->host_wake_gpio = pdata->host_wake_gpio;
	} else {
		return -ENODEV;
	}

	priv_g->pdev = pdev;
	wake_lock_init(&priv_g->bt_wakelock, WAKE_LOCK_SUSPEND, "bt_wakelock");
	wake_lock_init(&priv_g->host_wakelock, WAKE_LOCK_SUSPEND, "host_wakelock");

	bcm_bt_lpm_ensure_gpios_and_irq(priv_g);

	rc = bcm_bt_lpm_tty_init();
	if (rc)
		return rc;

	pr_info("bcm_bt_lpm_probe: bt_wake=%d host_wake=%d\n",
		priv_g->pdata->bt_wake_gpio, priv_g->pdata->host_wake_gpio);
	return 0;
}

static int bcm_bt_lpm_remove(struct platform_device *pdev)
{
	if (priv_g) {
		if (priv_g->plpm && priv_g->plpm->irq_ready)
			free_irq(priv_g->plpm->host_irq, priv_g);
		tty_unregister_ldisc(N_BRCM_HCI);
		wake_lock_destroy(&priv_g->bt_wakelock);
		wake_lock_destroy(&priv_g->host_wakelock);
		priv_g = NULL;
	}
	return 0;
}

static struct platform_driver bcm_bt_lpm_platform_driver = {
	.probe = bcm_bt_lpm_probe,
	.remove = bcm_bt_lpm_remove,
	.driver = {
		.name = "bcm-bt-lpm",
		.owner = THIS_MODULE,
		.of_match_table = bcm_bt_lpm_of_match,
	},
};

static int __init bcm_bt_lpm_init(void)
{
	return platform_driver_register(&bcm_bt_lpm_platform_driver);
}

static void __exit bcm_bt_lpm_exit(void)
{
	platform_driver_unregister(&bcm_bt_lpm_platform_driver);
}

late_initcall(bcm_bt_lpm_init);
module_exit(bcm_bt_lpm_exit);

MODULE_DESCRIPTION("bcm-bt-lpm");
MODULE_AUTHOR("Broadcom");
MODULE_LICENSE("GPL");
