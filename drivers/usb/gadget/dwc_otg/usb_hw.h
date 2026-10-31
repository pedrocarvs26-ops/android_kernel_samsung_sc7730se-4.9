#ifndef _USB_HW_H
#define _USB_HW_H

#include <linux/types.h>
#include <linux/platform_device.h>

struct sprd_usb_platform_data {
	u32 gpio_num;
	int gpio_boost;
	int gpio_chgdet;
	int gpio_otgdet;
	int ldo_gpio;
};

struct usb_hotplug_callback {
	int (*plugin)(int usb_cable, void *data);
	int (*plugout)(int usb_cable, void *data);
	void *data;
};

enum vbus_irq_type {
	VBUS_PLUG_IN,
	VBUS_PLUG_OUT
};

enum id_irq_type {
	OTG_CABLE_PLUG_IN,
	OTG_CABLE_PLUG_OUT
};

extern void usb_phy_init(struct platform_device *_dev);
extern void usb_phy_tune_host(void);
extern void usb_phy_tune_dev(void);
extern void usb_phy_ahb_rst(void);
extern void udc_enable(void);
extern void udc_disable(void);
extern void udc_phy_down(void);
extern void udc_phy_up(void);

extern int usb_alloc_vbus_irq(int gpio);
extern void usb_free_vbus_irq(int irq, int gpio);
extern int usb_get_vbus_irq(void);
extern int usb_get_vbus_state(void);
extern void usb_set_vbus_irq_type(int irq, int irq_type);
extern int usb_register_hotplug_callback(struct usb_hotplug_callback *cb);

extern void sprd_usb_vbus_set(bool enable);

#endif
