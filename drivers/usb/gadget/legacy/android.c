/*
 * Gadget Driver for Android (Linux 4.9 Composite/Function API adaptation)
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Mike Lockwood <lockwood@android.com>
 *         Benoit Goby <benoit@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/utsname.h>
#include <linux/platform_device.h>
#include <linux/if_ether.h>

#include <linux/usb/ch9.h>
#include <linux/usb/composite.h>
#include <linux/usb/gadget.h>
#include <linux/usb/f_mtp.h>
#include <linux/usb/f_accessory.h>

#include "../function/u_fs.h"
#include "../function/u_ether.h"
#include "../function/u_rndis.h"
#include "../function/f_mass_storage.h"
#include "../function/f_mtp.h"

extern int mtp_ctrlrequest(struct usb_composite_dev *cdev,
			   const struct usb_ctrlrequest *ctrl);
extern int acc_ctrlrequest(struct usb_composite_dev *cdev,
			   const struct usb_ctrlrequest *ctrl);
extern void acc_disconnect(void);

MODULE_AUTHOR("Mike Lockwood");
MODULE_DESCRIPTION("Android Composite USB Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

static const char longname[] = "Gadget Android";

/* Default vendor and product IDs, overridden by userspace */
#define VENDOR_ID		0x18D1
#define PRODUCT_ID		0x0001

#define USB_DEVICE_CLASS	0x00
#define USB_DEVICE_SUBCLASS	0x00
#define USB_DEVICE_PROTOCOL	0x01

extern void usb_first_enable_store_flag(void);

struct android_usb_function {
	char *name;
	void *config;

	struct device *dev;
	char *dev_name;
	struct device_attribute **attributes;

	/* for android_dev.enabled_functions */
	struct list_head enabled_list;

	int (*init)(struct android_usb_function *, struct usb_composite_dev *);
	void (*cleanup)(struct android_usb_function *);
	void (*enable)(struct android_usb_function *);
	void (*disable)(struct android_usb_function *);
	int (*bind_config)(struct android_usb_function *,
			   struct usb_configuration *);
	void (*unbind_config)(struct android_usb_function *,
			      struct usb_configuration *);
	int (*ctrlrequest)(struct android_usb_function *,
			   struct usb_composite_dev *,
			   const struct usb_ctrlrequest *);
};

struct android_dev {
	struct android_usb_function **functions;
	struct list_head enabled_functions;
	struct usb_composite_dev *cdev;
	struct device *dev;

	bool enabled;
	int disable_depth;
	struct mutex mutex;
	bool connected;
	bool sw_connected;
	struct work_struct work;
	char ffs_aliases[256];
};

static struct class *android_class;
static struct android_dev *_android_dev;
static int android_bind_config(struct usb_configuration *c);
static void android_unbind_config(struct usb_configuration *c);

#ifndef CONFIG_USB_CONFIGFS_UEVENT
struct device *create_function_device(char *name)
{
	if (android_class && !IS_ERR(android_class))
		return device_create(android_class, NULL,
				     MKDEV(0, 0), NULL, name);
	return ERR_PTR(-ENODEV);
}
EXPORT_SYMBOL_GPL(create_function_device);
#endif

#define STRING_MANUFACTURER_IDX		0
#define STRING_PRODUCT_IDX		1
#define STRING_SERIAL_IDX		2

static char manufacturer_string[256];
static char product_string[256];
static char serial_string[256];

static struct usb_string strings_dev[] = {
	[STRING_MANUFACTURER_IDX].s = manufacturer_string,
	[STRING_PRODUCT_IDX].s = product_string,
	[STRING_SERIAL_IDX].s = serial_string,
	{  }
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_device_descriptor device_desc = {
	.bLength              = sizeof(device_desc),
	.bDescriptorType      = USB_DT_DEVICE,
	.bcdUSB               = __constant_cpu_to_le16(0x0200),
	.bDeviceClass         = USB_DEVICE_CLASS,
	.bDeviceSubClass      = USB_DEVICE_SUBCLASS,
	.bDeviceProtocol      = USB_DEVICE_PROTOCOL,
	.idVendor             = __constant_cpu_to_le16(VENDOR_ID),
	.idProduct            = __constant_cpu_to_le16(PRODUCT_ID),
	.bcdDevice            = __constant_cpu_to_le16(0x0400),
	.bNumConfigurations   = 1,
};

static struct usb_configuration android_config_driver = {
	.label			= "android",
	.unbind			= android_unbind_config,
	.bConfigurationValue	= 1,
	.bmAttributes		= USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.MaxPower		= 500,
};

static void android_work(struct work_struct *data)
{
	struct android_dev *dev = container_of(data, struct android_dev, work);
	struct usb_composite_dev *cdev = dev->cdev;
	char *disconnected[2] = { "USB_STATE=DISCONNECTED", NULL };
	char *connected[2]    = { "USB_STATE=CONNECTED", NULL };
	char *configured[2]   = { "USB_STATE=CONFIGURED", NULL };
	char **uevent_envp = NULL;
	unsigned long flags;

	if (!cdev)
		return;
	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config)
		uevent_envp = configured;
	else if (dev->connected != dev->sw_connected)
		uevent_envp = dev->connected ? connected : disconnected;
	dev->sw_connected = dev->connected;
	spin_unlock_irqrestore(&cdev->lock, flags);

	if (uevent_envp) {
		kobject_uevent_env(&dev->dev->kobj, KOBJ_CHANGE, uevent_envp);
		pr_info("%s: sent uevent %s\n", __func__, uevent_envp[0]);
	}
}

static void android_enable(struct android_dev *dev)
{
	struct usb_composite_dev *cdev = dev->cdev;

	if (WARN_ON(!dev->disable_depth))
		return;

	if (--dev->disable_depth == 0) {
		usb_add_config(cdev, &android_config_driver,
			       android_bind_config);
		mdelay(50);
		usb_gadget_connect(cdev->gadget);
	}
}

static void android_disable(struct android_dev *dev)
{
	struct usb_composite_dev *cdev = dev->cdev;

	if (dev->disable_depth++ == 0) {
		usb_gadget_disconnect(cdev->gadget);
		usb_ep_dequeue(cdev->gadget->ep0, cdev->req);
		usb_remove_config(cdev, &android_config_driver);
	}
}

/*-------------------------------------------------------------------------*/
/* FunctionFS (adb) */

struct functionfs_config {
	bool opened;
	bool enabled;
	struct usb_function_instance *fi;
	struct usb_function *func;
	struct f_fs_opts *opts;
};

static int ffs_ready_callback(struct ffs_data *ffs);
static void ffs_closed_callback(struct ffs_data *ffs);

static int ffs_function_init(struct android_usb_function *f,
			     struct usb_composite_dev *cdev)
{
	struct functionfs_config *config;
	struct f_fs_opts *opts;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("ffs");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	opts = container_of(config->fi, struct f_fs_opts, func_inst);
	opts->no_configfs = true;
	opts->dev->ffs_ready_callback = ffs_ready_callback;
	opts->dev->ffs_closed_callback = ffs_closed_callback;
	ffs_single_dev(opts->dev);
	ffs_name_dev(opts->dev, "adb");

	config->opts = opts;
	f->config = config;
	return 0;
}

static void ffs_function_cleanup(struct android_usb_function *f)
{
	struct functionfs_config *config = f->config;

	if (!config)
		return;
	if (config->func)
		usb_put_function(config->func);
	if (config->fi)
		usb_put_function_instance(config->fi);
	kfree(config);
	f->config = NULL;
}

static void ffs_function_enable(struct android_usb_function *f)
{
	struct android_dev *dev = _android_dev;
	struct functionfs_config *config = f->config;

	config->enabled = true;
	if (!config->opened)
		android_disable(dev);
}

static void ffs_function_disable(struct android_usb_function *f)
{
	struct android_dev *dev = _android_dev;
	struct functionfs_config *config = f->config;

	config->enabled = false;
	if (!config->opened)
		android_enable(dev);
}

static int ffs_function_bind_config(struct android_usb_function *f,
				    struct usb_configuration *c)
{
	struct functionfs_config *config = f->config;
	int ret;

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		config->func = NULL;
		return ret;
	}

	ret = usb_add_function(c, config->func);
	if (ret) {
		usb_put_function(config->func);
		config->func = NULL;
	}
	return ret;
}

static void ffs_function_unbind_config(struct android_usb_function *f,
				       struct usb_configuration *c)
{
	struct functionfs_config *config = f->config;

	if (config && config->func) {
		usb_put_function(config->func);
		config->func = NULL;
	}
}

static ssize_t ffs_aliases_show(struct device *pdev,
				struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = _android_dev;
	int ret;

	mutex_lock(&dev->mutex);
	ret = sprintf(buf, "%s\n", dev->ffs_aliases);
	mutex_unlock(&dev->mutex);

	return ret;
}

static ssize_t ffs_aliases_store(struct device *pdev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct android_dev *dev = _android_dev;
	char buff[256];

	mutex_lock(&dev->mutex);
	if (dev->enabled) {
		mutex_unlock(&dev->mutex);
		return -EBUSY;
	}

	strlcpy(buff, buf, sizeof(buff));
	strlcpy(dev->ffs_aliases, strim(buff), sizeof(dev->ffs_aliases));
	mutex_unlock(&dev->mutex);

	return size;
}

static DEVICE_ATTR(aliases, S_IRUGO | S_IWUSR, ffs_aliases_show,
		   ffs_aliases_store);

static struct device_attribute *ffs_function_attributes[] = {
	&dev_attr_aliases,
	NULL
};

static struct android_usb_function ffs_function = {
	.name		= "ffs",
	.init		= ffs_function_init,
	.enable		= ffs_function_enable,
	.disable	= ffs_function_disable,
	.cleanup	= ffs_function_cleanup,
	.bind_config	= ffs_function_bind_config,
	.unbind_config	= ffs_function_unbind_config,
	.attributes	= ffs_function_attributes,
};

static int ffs_ready_callback(struct ffs_data *ffs)
{
	struct android_dev *dev = _android_dev;
	struct functionfs_config *config = ffs_function.config;

	if (!dev || !config)
		return -ENODEV;

	mutex_lock(&dev->mutex);
	config->opened = true;
	if (config->enabled)
		android_enable(dev);
	mutex_unlock(&dev->mutex);

	return 0;
}

static void ffs_closed_callback(struct ffs_data *ffs)
{
	struct android_dev *dev = _android_dev;
	struct functionfs_config *config = ffs_function.config;

	if (!dev || !config)
		return;

	mutex_lock(&dev->mutex);
	if (config->enabled)
		android_disable(dev);
	config->opened = false;
	mutex_unlock(&dev->mutex);
}

/*-------------------------------------------------------------------------*/
/* ACM */

#define MAX_ACM_INSTANCES 4
struct acm_function_config {
	int instances;
	int instances_on;
	struct usb_function *f_acm[MAX_ACM_INSTANCES];
	struct usb_function_instance *f_acm_inst[MAX_ACM_INSTANCES];
};

static int acm_function_init(struct android_usb_function *f,
			     struct usb_composite_dev *cdev)
{
	int i, ret;
	struct acm_function_config *config;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;
	config->instances = 1;
	f->config = config;

	for (i = 0; i < MAX_ACM_INSTANCES; i++) {
		config->f_acm_inst[i] = usb_get_function_instance("acm");
		if (IS_ERR(config->f_acm_inst[i])) {
			ret = PTR_ERR(config->f_acm_inst[i]);
			goto err_inst;
		}
		config->f_acm[i] = usb_get_function(config->f_acm_inst[i]);
		if (IS_ERR(config->f_acm[i])) {
			ret = PTR_ERR(config->f_acm[i]);
			goto err_func;
		}
	}
	return 0;

err_inst:
	while (i-- > 0) {
		usb_put_function(config->f_acm[i]);
err_func:
		usb_put_function_instance(config->f_acm_inst[i]);
	}
	kfree(config);
	f->config = NULL;
	return ret;
}

static void acm_function_cleanup(struct android_usb_function *f)
{
	int i;
	struct acm_function_config *config = f->config;

	if (!config)
		return;
	for (i = 0; i < MAX_ACM_INSTANCES; i++) {
		usb_put_function(config->f_acm[i]);
		usb_put_function_instance(config->f_acm_inst[i]);
	}
	kfree(config);
	f->config = NULL;
}

static int acm_function_bind_config(struct android_usb_function *f,
				    struct usb_configuration *c)
{
	int i, ret = 0;
	struct acm_function_config *config = f->config;

	config->instances_on = config->instances;
	if (config->instances_on <= 0)
		config->instances_on = 1;

	for (i = 0; i < config->instances_on; i++) {
		ret = usb_add_function(c, config->f_acm[i]);
		if (ret) {
			pr_err("Could not bind acm%u config\n", i);
			while (i-- > 0)
				usb_remove_function(c, config->f_acm[i]);
			return ret;
		}
	}
	return 0;
}

static ssize_t acm_instances_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct acm_function_config *config = f->config;

	return sprintf(buf, "%d\n", config->instances);
}

static ssize_t acm_instances_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct acm_function_config *config = f->config;
	int value = 1;

	if (sscanf(buf, "%d", &value) == 1) {
		if (value > MAX_ACM_INSTANCES)
			value = MAX_ACM_INSTANCES;
		if (value < 0)
			value = 0;
		config->instances = value;
	}
	return size;
}

static DEVICE_ATTR(instances, S_IRUGO | S_IWUSR, acm_instances_show,
		   acm_instances_store);

static struct device_attribute *acm_function_attributes[] = {
	&dev_attr_instances,
	NULL
};

static struct android_usb_function acm_function = {
	.name		= "acm",
	.init		= acm_function_init,
	.cleanup	= acm_function_cleanup,
	.bind_config	= acm_function_bind_config,
	.attributes	= acm_function_attributes,
};

/*-------------------------------------------------------------------------*/
/* MTP & PTP */

struct mtp_ptp_config {
	struct usb_function_instance *fi;
	struct usb_function *func;
};

static int mtp_function_init(struct android_usb_function *f,
			     struct usb_composite_dev *cdev)
{
	struct mtp_ptp_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("mtp");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		usb_put_function_instance(config->fi);
		kfree(config);
		return ret;
	}

	f->config = config;
	return 0;
}

static void mtp_function_cleanup(struct android_usb_function *f)
{
	struct mtp_ptp_config *config = f->config;

	if (!config)
		return;
	if (config->func)
		usb_put_function(config->func);
	if (config->fi)
		usb_put_function_instance(config->fi);
	kfree(config);
	f->config = NULL;
}

static int mtp_function_bind_config(struct android_usb_function *f,
				    struct usb_configuration *c)
{
	struct mtp_ptp_config *config = f->config;

	return usb_add_function(c, config->func);
}

static int ptp_function_init(struct android_usb_function *f,
			     struct usb_composite_dev *cdev)
{
	struct mtp_ptp_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("ptp");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		usb_put_function_instance(config->fi);
		kfree(config);
		return ret;
	}

	f->config = config;
	return 0;
}

static int mtp_function_ctrlrequest(struct android_usb_function *f,
				    struct usb_composite_dev *cdev,
				    const struct usb_ctrlrequest *c)
{
	return mtp_ctrlrequest(cdev, c);
}

static struct android_usb_function mtp_function = {
	.name		= "mtp",
	.init		= mtp_function_init,
	.cleanup	= mtp_function_cleanup,
	.bind_config	= mtp_function_bind_config,
	.ctrlrequest	= mtp_function_ctrlrequest,
};

static struct android_usb_function ptp_function = {
	.name		= "ptp",
	.init		= ptp_function_init,
	.cleanup	= mtp_function_cleanup,
	.bind_config	= mtp_function_bind_config,
	.ctrlrequest	= mtp_function_ctrlrequest,
};

/*-------------------------------------------------------------------------*/
/* RNDIS */

struct rndis_function_config {
	u8 ethaddr[ETH_ALEN];
	u32 vendorID;
	char manufacturer[256];
	bool wceis;
	struct usb_function_instance *fi;
	struct usb_function *func;
	struct f_rndis_opts *opts;
};

static int rndis_function_init(struct android_usb_function *f,
			       struct usb_composite_dev *cdev)
{
	struct rndis_function_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("rndis");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	config->opts = container_of(config->fi, struct f_rndis_opts, func_inst);
	f->config = config;
	return 0;
}

static void rndis_function_cleanup(struct android_usb_function *f)
{
	struct rndis_function_config *config = f->config;

	if (!config)
		return;
	if (config->func)
		usb_put_function(config->func);
	if (config->fi)
		usb_put_function_instance(config->fi);
	kfree(config);
	f->config = NULL;
}

static int rndis_function_bind_config(struct android_usb_function *f,
				      struct usb_configuration *c)
{
	struct rndis_function_config *config = f->config;
	int ret;

	config->opts->vendor_id = config->vendorID;
	config->opts->manufacturer = config->manufacturer;
	config->opts->wceis = config->wceis;

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		config->func = NULL;
		return ret;
	}

	ret = usb_add_function(c, config->func);
	if (ret) {
		usb_put_function(config->func);
		config->func = NULL;
	}
	return ret;
}

static void rndis_function_unbind_config(struct android_usb_function *f,
					 struct usb_configuration *c)
{
	struct rndis_function_config *config = f->config;

	if (config && config->func) {
		usb_put_function(config->func);
		config->func = NULL;
	}
}

static ssize_t rndis_manufacturer_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;

	return sprintf(buf, "%s\n", config->manufacturer);
}

static ssize_t rndis_manufacturer_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;

	if (size >= sizeof(config->manufacturer))
		return -EINVAL;
	if (sscanf(buf, "%255s", config->manufacturer) == 1)
		return size;
	return -1;
}
static DEVICE_ATTR(manufacturer, S_IRUGO | S_IWUSR, rndis_manufacturer_show,
		   rndis_manufacturer_store);

static ssize_t rndis_wceis_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;

	return sprintf(buf, "%d\n", config->wceis);
}

static ssize_t rndis_wceis_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;
	int value;

	if (sscanf(buf, "%d", &value) == 1) {
		config->wceis = !!value;
		return size;
	}
	return -EINVAL;
}
static DEVICE_ATTR(wceis, S_IRUGO | S_IWUSR, rndis_wceis_show,
		   rndis_wceis_store);

static ssize_t rndis_ethaddr_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *rndis = f->config;

	return sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n",
		rndis->ethaddr[0], rndis->ethaddr[1], rndis->ethaddr[2],
		rndis->ethaddr[3], rndis->ethaddr[4], rndis->ethaddr[5]);
}

static ssize_t rndis_ethaddr_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *rndis = f->config;
	int i;
	char *src;

	for (i = 0; i < ETH_ALEN; i++)
		rndis->ethaddr[i] = 0;
	rndis->ethaddr[0] = 0x02;
	src = serial_string;
	for (i = 0; (i < 256) && *src; i++)
		rndis->ethaddr[i % (ETH_ALEN - 1) + 1] ^= *src++;
	return size;
}
static DEVICE_ATTR(ethaddr, S_IRUGO | S_IWUSR, rndis_ethaddr_show,
		   rndis_ethaddr_store);

static ssize_t rndis_vendorID_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;

	return sprintf(buf, "%04x\n", config->vendorID);
}

static ssize_t rndis_vendorID_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct rndis_function_config *config = f->config;
	int value;

	if (sscanf(buf, "%04x", &value) == 1) {
		config->vendorID = value;
		return size;
	}
	return -EINVAL;
}
static DEVICE_ATTR(vendorID, S_IRUGO | S_IWUSR, rndis_vendorID_show,
		   rndis_vendorID_store);

static struct device_attribute *rndis_function_attributes[] = {
	&dev_attr_manufacturer,
	&dev_attr_wceis,
	&dev_attr_ethaddr,
	&dev_attr_vendorID,
	NULL
};

static struct android_usb_function rndis_function = {
	.name		= "rndis",
	.init		= rndis_function_init,
	.cleanup	= rndis_function_cleanup,
	.bind_config	= rndis_function_bind_config,
	.unbind_config	= rndis_function_unbind_config,
	.attributes	= rndis_function_attributes,
};

/*-------------------------------------------------------------------------*/
/* Mass Storage */

struct mass_storage_function_config {
	struct usb_function_instance *fi;
	struct usb_function *func;
	struct fsg_opts *opts;
	char inquiry_string[INQUIRY_STRING_LEN];
};

static int mass_storage_function_init(struct android_usb_function *f,
				      struct usb_composite_dev *cdev)
{
	struct mass_storage_function_config *config;
	struct fsg_opts *opts;
	struct fsg_config fsg;
	struct device *lun_dev;
	int i, ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("mass_storage");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	opts = fsg_opts_from_func_inst(config->fi);
	opts->no_configfs = true;
	fsg_common_remove_luns(opts->common);

	memset(&fsg, 0, sizeof(fsg));
	fsg.nluns = 2;
	for (i = 0; i < fsg.nluns; i++) {
		fsg.luns[i].removable = 1;
		fsg.luns[i].nofua = 1;
	}
	fsg.can_stall = 1;

	ret = fsg_common_set_cdev(opts->common, cdev, fsg.can_stall);
	if (ret)
		goto err_put;

	fsg_common_set_sysfs(opts->common, true);
	ret = fsg_common_create_luns(opts->common, &fsg);
	if (ret)
		goto err_put;

	fsg_common_set_inquiry_string(opts->common, NULL, NULL);

	lun_dev = fsg_common_get_lun_dev(opts->common, 0);
	if (lun_dev)
		ret = sysfs_create_link(&f->dev->kobj, &lun_dev->kobj, "lun");
	lun_dev = fsg_common_get_lun_dev(opts->common, 1);
	if (lun_dev)
		ret = sysfs_create_link(&f->dev->kobj, &lun_dev->kobj, "lun1");

	config->opts = opts;
	f->config = config;
	return 0;

err_put:
	usb_put_function_instance(config->fi);
	kfree(config);
	return ret;
}

static void mass_storage_function_cleanup(struct android_usb_function *f)
{
	struct mass_storage_function_config *config = f->config;

	if (!config)
		return;
	if (config->func)
		usb_put_function(config->func);
	if (config->fi)
		usb_put_function_instance(config->fi);
	kfree(config);
	f->config = NULL;
}

static int mass_storage_function_bind_config(struct android_usb_function *f,
					     struct usb_configuration *c)
{
	struct mass_storage_function_config *config = f->config;
	int ret;

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		config->func = NULL;
		return ret;
	}

	ret = usb_add_function(c, config->func);
	if (ret) {
		usb_put_function(config->func);
		config->func = NULL;
	}
	return ret;
}

static void mass_storage_function_unbind_config(struct android_usb_function *f,
						struct usb_configuration *c)
{
	struct mass_storage_function_config *config = f->config;

	if (config && config->func) {
		usb_put_function(config->func);
		config->func = NULL;
	}
}

static ssize_t mass_storage_inquiry_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct mass_storage_function_config *config = f->config;

	return sprintf(buf, "%s\n", config->inquiry_string);
}

static ssize_t mass_storage_inquiry_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t size)
{
	struct android_usb_function *f = dev_get_drvdata(dev);
	struct mass_storage_function_config *config = f->config;

	if (size >= sizeof(config->inquiry_string))
		return -EINVAL;
	if (sscanf(buf, "%28s", config->inquiry_string) != 1)
		return -EINVAL;
	return size;
}
static DEVICE_ATTR(inquiry_string, S_IRUGO | S_IWUSR,
		   mass_storage_inquiry_show, mass_storage_inquiry_store);

static struct device_attribute *mass_storage_function_attributes[] = {
	&dev_attr_inquiry_string,
	NULL
};

static struct android_usb_function mass_storage_function = {
	.name		= "mass_storage",
	.init		= mass_storage_function_init,
	.cleanup	= mass_storage_function_cleanup,
	.bind_config	= mass_storage_function_bind_config,
	.unbind_config	= mass_storage_function_unbind_config,
	.attributes	= mass_storage_function_attributes,
};

/*-------------------------------------------------------------------------*/
/* Accessory */

struct accessory_config {
	struct usb_function_instance *fi;
	struct usb_function *func;
};

static int accessory_function_init(struct android_usb_function *f,
				   struct usb_composite_dev *cdev)
{
	struct accessory_config *config;
	int ret;

	config = kzalloc(sizeof(*config), GFP_KERNEL);
	if (!config)
		return -ENOMEM;

	config->fi = usb_get_function_instance("accessory");
	if (IS_ERR(config->fi)) {
		ret = PTR_ERR(config->fi);
		kfree(config);
		return ret;
	}

	config->func = usb_get_function(config->fi);
	if (IS_ERR(config->func)) {
		ret = PTR_ERR(config->func);
		usb_put_function_instance(config->fi);
		kfree(config);
		return ret;
	}

	f->config = config;
	return 0;
}

static void accessory_function_cleanup(struct android_usb_function *f)
{
	struct accessory_config *config = f->config;

	if (!config)
		return;
	if (config->func)
		usb_put_function(config->func);
	if (config->fi)
		usb_put_function_instance(config->fi);
	kfree(config);
	f->config = NULL;
}

static int accessory_function_bind_config(struct android_usb_function *f,
					  struct usb_configuration *c)
{
	struct accessory_config *config = f->config;

	return usb_add_function(c, config->func);
}

static int accessory_function_ctrlrequest(struct android_usb_function *f,
					  struct usb_composite_dev *cdev,
					  const struct usb_ctrlrequest *c)
{
	return acc_ctrlrequest(cdev, c);
}

static struct android_usb_function accessory_function = {
	.name		= "accessory",
	.init		= accessory_function_init,
	.cleanup	= accessory_function_cleanup,
	.bind_config	= accessory_function_bind_config,
	.ctrlrequest	= accessory_function_ctrlrequest,
};

static struct android_usb_function *supported_functions[] = {
	&ffs_function,
	&acm_function,
	&mtp_function,
	&ptp_function,
	&rndis_function,
	&mass_storage_function,
	&accessory_function,
	NULL
};

static int android_init_functions(struct android_usb_function **functions,
				  struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;
	struct android_usb_function *f;
	struct device_attribute **attrs;
	struct device_attribute *attr;
	int err = 0;
	int index = 0;

	for (; (f = *functions++); index++) {
		f->dev_name = kasprintf(GFP_KERNEL, "f_%s", f->name);
		f->dev = device_create(android_class, dev->dev,
				       MKDEV(0, index + 1), f, f->dev_name);
		if (IS_ERR(f->dev)) {
			pr_err("%s: Failed to create dev %s\n", __func__,
			       f->dev_name);
			err = PTR_ERR(f->dev);
			goto err_create;
		}

		if (f->init) {
			err = f->init(f, cdev);
			if (err) {
				pr_err("%s: Failed to init %s (%d)\n",
				       __func__, f->name, err);
				goto err_out;
			}
		}

		attrs = f->attributes;
		if (attrs) {
			while ((attr = *attrs++) && !err)
				err = device_create_file(f->dev, attr);
		}
		if (err) {
			pr_err("%s: Failed to create function %s attributes\n",
			       __func__, f->name);
			goto err_out;
		}
	}
	return 0;

err_out:
	device_destroy(android_class, f->dev->devt);
err_create:
	kfree(f->dev_name);
	return err;
}

static void android_cleanup_functions(struct android_usb_function **functions)
{
	struct android_usb_function *f;

	while (*functions) {
		f = *functions++;
		if (f->dev) {
			device_destroy(android_class, f->dev->devt);
			kfree(f->dev_name);
		}
		if (f->cleanup)
			f->cleanup(f);
	}
}

static int android_bind_enabled_functions(struct android_dev *dev,
					  struct usb_configuration *c)
{
	struct android_usb_function *f;
	int ret;

	list_for_each_entry(f, &dev->enabled_functions, enabled_list) {
		ret = f->bind_config(f, c);
		if (ret) {
			pr_err("%s: %s failed (%d)\n", __func__, f->name, ret);
			return ret;
		}
	}
	return 0;
}

static void android_unbind_enabled_functions(struct android_dev *dev,
					     struct usb_configuration *c)
{
	struct android_usb_function *f;

	list_for_each_entry(f, &dev->enabled_functions, enabled_list) {
		if (f->unbind_config)
			f->unbind_config(f, c);
	}
}

static int android_enable_function(struct android_dev *dev, char *name)
{
	struct android_usb_function **functions = dev->functions;
	struct android_usb_function *f;

	while ((f = *functions++)) {
		if (!strcmp(name, f->name)) {
			list_add_tail(&f->enabled_list,
				      &dev->enabled_functions);
			return 0;
		}
	}
	return -EINVAL;
}

/*-------------------------------------------------------------------------*/
/* /sys/class/android_usb/android0/ interface */

static ssize_t functions_show(struct device *pdev,
			      struct device_attribute *attr, char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	struct android_usb_function *f;
	char *buff = buf;

	mutex_lock(&dev->mutex);
	list_for_each_entry(f, &dev->enabled_functions, enabled_list)
		buff += sprintf(buff, "%s,", f->name);
	mutex_unlock(&dev->mutex);

	if (buff != buf)
		*(buff - 1) = '\n';
	return buff - buf;
}

static ssize_t functions_store(struct device *pdev,
			       struct device_attribute *attr,
			       const char *buff, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	char *name;
	char buf[256], *b;
	char aliases[256], *a;
	int err;
	int is_ffs;
	int ffs_enabled = 0;

	mutex_lock(&dev->mutex);
	if (dev->enabled) {
		mutex_unlock(&dev->mutex);
		return -EBUSY;
	}

	INIT_LIST_HEAD(&dev->enabled_functions);

	strlcpy(buf, buff, sizeof(buf));
	b = strim(buf);

	while (b) {
		name = strsep(&b, ",");
		if (!name || !*name)
			continue;

		is_ffs = 0;
		strlcpy(aliases, dev->ffs_aliases, sizeof(aliases));
		a = aliases;

		while (a) {
			char *alias = strsep(&a, ",");
			if (alias && !strcmp(name, alias)) {
				is_ffs = 1;
				break;
			}
		}

		if (is_ffs) {
			if (ffs_enabled)
				continue;
			err = android_enable_function(dev, "ffs");
			if (err)
				pr_err("android_usb: Cannot enable ffs (%d)\n",
				       err);
			else
				ffs_enabled = 1;
			continue;
		}

		if (!strcmp(name, "none"))
			continue;

		err = android_enable_function(dev, name);
		if (err)
			pr_err("android_usb: Cannot enable '%s' (%d)\n",
			       name, err);
	}

	mutex_unlock(&dev->mutex);
	return size;
}

static ssize_t enable_show(struct device *pdev, struct device_attribute *attr,
			   char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);

	return sprintf(buf, "%d\n", dev->enabled);
}

static ssize_t enable_store(struct device *pdev, struct device_attribute *attr,
			    const char *buff, size_t size)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	struct usb_composite_dev *cdev = dev->cdev;
	struct android_usb_function *f;
	int enabled = 0;

	if (!cdev)
		return 0;

	mutex_lock(&dev->mutex);
	usb_first_enable_store_flag();
	sscanf(buff, "%d", &enabled);
	pr_info("android_usb: %s(%d) enable=%d (was %d)\n",
		current->comm, current->pid, enabled, dev->enabled);

	if (enabled && !dev->enabled) {
		cdev->desc.idVendor = device_desc.idVendor;
		cdev->desc.idProduct = device_desc.idProduct;
		cdev->desc.bcdDevice = device_desc.bcdDevice;
		cdev->desc.bDeviceClass = device_desc.bDeviceClass;
		cdev->desc.bDeviceSubClass = device_desc.bDeviceSubClass;
		cdev->desc.bDeviceProtocol = device_desc.bDeviceProtocol;
		list_for_each_entry(f, &dev->enabled_functions, enabled_list) {
			if (f->enable)
				f->enable(f);
		}
		android_enable(dev);
		dev->enabled = true;
	} else if (!enabled && dev->enabled) {
		android_disable(dev);
		list_for_each_entry(f, &dev->enabled_functions, enabled_list) {
			if (f->disable)
				f->disable(f);
		}
		dev->enabled = false;
	}

	mutex_unlock(&dev->mutex);
	return size;
}

static ssize_t state_show(struct device *pdev, struct device_attribute *attr,
			  char *buf)
{
	struct android_dev *dev = dev_get_drvdata(pdev);
	struct usb_composite_dev *cdev = dev->cdev;
	char *state = "DISCONNECTED";
	unsigned long flags;

	if (!cdev)
		goto out;

	spin_lock_irqsave(&cdev->lock, flags);
	if (cdev->config)
		state = "CONFIGURED";
	else if (dev->connected)
		state = "CONNECTED";
	spin_unlock_irqrestore(&cdev->lock, flags);
out:
	return sprintf(buf, "%s\n", state);
}

#define DESCRIPTOR_ATTR(field, format_string)				\
static ssize_t								\
field ## _show(struct device *dev, struct device_attribute *attr,	\
		char *buf)						\
{									\
	return sprintf(buf, format_string, device_desc.field);		\
}									\
static ssize_t								\
field ## _store(struct device *dev, struct device_attribute *attr,	\
		const char *buf, size_t size)				\
{									\
	int value;							\
	if (sscanf(buf, format_string, &value) == 1) {			\
		device_desc.field = value;				\
		return size;						\
	}								\
	return -1;							\
}									\
static DEVICE_ATTR(field, S_IRUGO | S_IWUSR, field ## _show, field ## _store);

#define DESCRIPTOR_STRING_ATTR(field, buffer)				\
static ssize_t								\
field ## _show(struct device *dev, struct device_attribute *attr,	\
		char *buf)						\
{									\
	return sprintf(buf, "%s", buffer);				\
}									\
static ssize_t								\
field ## _store(struct device *dev, struct device_attribute *attr,	\
		const char *buf, size_t size)				\
{									\
	if (size >= sizeof(buffer))					\
		return -EINVAL;						\
	strlcpy(buffer, buf, sizeof(buffer));				\
	strim(buffer);							\
	return size;							\
}									\
static DEVICE_ATTR(field, S_IRUGO | S_IWUSR, field ## _show, field ## _store);

DESCRIPTOR_ATTR(idVendor, "%04x\n")
DESCRIPTOR_ATTR(idProduct, "%04x\n")
DESCRIPTOR_ATTR(bcdDevice, "%04x\n")
DESCRIPTOR_ATTR(bDeviceClass, "%02x\n")
DESCRIPTOR_ATTR(bDeviceSubClass, "%02x\n")
DESCRIPTOR_ATTR(bDeviceProtocol, "%02x\n")
DESCRIPTOR_STRING_ATTR(iManufacturer, manufacturer_string)
DESCRIPTOR_STRING_ATTR(iProduct, product_string)
DESCRIPTOR_STRING_ATTR(iSerial, serial_string)

static DEVICE_ATTR(functions, S_IRUGO | S_IWUSR, functions_show,
		   functions_store);
static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR, enable_show, enable_store);
static DEVICE_ATTR(state, S_IRUGO, state_show, NULL);

static struct device_attribute *android_usb_attributes[] = {
	&dev_attr_idVendor,
	&dev_attr_idProduct,
	&dev_attr_bcdDevice,
	&dev_attr_bDeviceClass,
	&dev_attr_bDeviceSubClass,
	&dev_attr_bDeviceProtocol,
	&dev_attr_iManufacturer,
	&dev_attr_iProduct,
	&dev_attr_iSerial,
	&dev_attr_functions,
	&dev_attr_enable,
	&dev_attr_state,
	NULL
};

/*-------------------------------------------------------------------------*/
/* Composite driver */

static int android_bind_config(struct usb_configuration *c)
{
	struct android_dev *dev = _android_dev;

	return android_bind_enabled_functions(dev, c);
}

static void android_unbind_config(struct usb_configuration *c)
{
	struct android_dev *dev = _android_dev;

	android_unbind_enabled_functions(dev, c);
}

static int android_bind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;
	struct usb_gadget *gadget = cdev->gadget;
	int id, ret;

	usb_gadget_disconnect(gadget);

	ret = android_init_functions(dev->functions, cdev);
	if (ret)
		return ret;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_MANUFACTURER_IDX].id = id;
	device_desc.iManufacturer = id;

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_PRODUCT_IDX].id = id;
	device_desc.iProduct = id;

	strncpy(manufacturer_string, "Samsung", sizeof(manufacturer_string) - 1);
	strncpy(product_string, "SM-T560", sizeof(product_string) - 1);
	strncpy(serial_string, "0123456789ABCDEF", sizeof(serial_string) - 1);

	id = usb_string_id(cdev);
	if (id < 0)
		return id;
	strings_dev[STRING_SERIAL_IDX].id = id;
	device_desc.iSerialNumber = id;

	usb_gadget_set_selfpowered(gadget);
	dev->cdev = cdev;

	return 0;
}

static int android_usb_unbind(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;

	cancel_work_sync(&dev->work);
	android_cleanup_functions(dev->functions);
	return 0;
}

static int (*composite_setup_func)(struct usb_gadget *gadget,
				   const struct usb_ctrlrequest *c);

static int android_setup(struct usb_gadget *gadget,
			 const struct usb_ctrlrequest *c)
{
	struct android_dev *dev = _android_dev;
	struct usb_composite_dev *cdev = get_gadget_data(gadget);
	struct usb_request *req = cdev->req;
	struct android_usb_function *f;
	int value = -EOPNOTSUPP;
	unsigned long flags;

	req->zero = 0;
	req->length = 0;
	gadget->ep0->driver_data = cdev;

	list_for_each_entry(f, &dev->enabled_functions, enabled_list) {
		if (f->ctrlrequest) {
			value = f->ctrlrequest(f, cdev, c);
			if (value >= 0)
				break;
		}
	}

	if (value < 0)
		value = acc_ctrlrequest(cdev, c);

	if (value < 0)
		value = composite_setup_func(gadget, c);

	spin_lock_irqsave(&cdev->lock, flags);
	if (!dev->connected) {
		dev->connected = 1;
		schedule_work(&dev->work);
	} else if (c->bRequest == USB_REQ_SET_CONFIGURATION && cdev->config) {
		schedule_work(&dev->work);
	}
	spin_unlock_irqrestore(&cdev->lock, flags);

	return value;
}

static void android_disconnect(struct usb_composite_dev *cdev)
{
	struct android_dev *dev = _android_dev;

	acc_disconnect();
	dev->connected = 0;
	schedule_work(&dev->work);
}

static struct usb_composite_driver android_usb_driver = {
	.name		= "android_usb",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.bind		= android_bind,
	.unbind		= android_usb_unbind,
	.disconnect	= android_disconnect,
	.max_speed	= USB_SPEED_HIGH,
};

static int android_create_device(struct android_dev *dev)
{
	struct device_attribute **attrs = android_usb_attributes;
	struct device_attribute *attr;
	int err;

	dev->dev = device_create(android_class, NULL,
				 MKDEV(0, 0), NULL, "android0");
	if (IS_ERR(dev->dev))
		return PTR_ERR(dev->dev);

	dev_set_drvdata(dev->dev, dev);

	while ((attr = *attrs++)) {
		err = device_create_file(dev->dev, attr);
		if (err) {
			device_destroy(android_class, dev->dev->devt);
			return err;
		}
	}
	return 0;
}

static int __init init(void)
{
	struct android_dev *dev;
	int err;

	android_class = class_create(THIS_MODULE, "android_usb");
	if (IS_ERR(android_class))
		return PTR_ERR(android_class);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err_dev;
	}

	dev->disable_depth = 1;
	dev->functions = supported_functions;
	strlcpy(dev->ffs_aliases, "adb", sizeof(dev->ffs_aliases));
	INIT_LIST_HEAD(&dev->enabled_functions);
	INIT_WORK(&dev->work, android_work);
	mutex_init(&dev->mutex);

	err = android_create_device(dev);
	if (err) {
		pr_err("%s: failed to create android device %d\n",
		       __func__, err);
		goto err_create;
	}

	_android_dev = dev;

	err = usb_composite_probe(&android_usb_driver);
	if (err) {
		pr_err("%s: failed to probe driver %d\n", __func__, err);
		goto err_create;
	}

	composite_setup_func = android_usb_driver.gadget_driver.setup;
	android_usb_driver.gadget_driver.setup = android_setup;

	return 0;

err_create:
	kfree(dev);
err_dev:
	class_destroy(android_class);
	return err;
}
late_initcall(init);

static void __exit cleanup(void)
{
	usb_composite_unregister(&android_usb_driver);
	class_destroy(android_class);
	kfree(_android_dev);
	_android_dev = NULL;
}
module_exit(cleanup);
