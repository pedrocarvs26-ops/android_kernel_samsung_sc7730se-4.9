#
# Makefile for DWC_otg Highspeed USB controller driver (SC7730SE / SCX35)
#

ifeq ($(CONFIG_USB_GADGET_DEBUG),y)
EXTRA_CFLAGS		+= -DDEBUG
endif

EXTRA_CFLAGS		+= -DDWC_DEVICE_ONLY
EXTRA_CFLAGS		+= -DDWC_LINUX
EXTRA_CFLAGS		+= -DCONFIG_ARCH_SCX35
EXTRA_CFLAGS		+= -DCONFIG_USB_CORE_IP_293A
EXTRA_CFLAGS		+= -DCONFIG_USB_EXTERNAL_DETECT
EXTRA_CFLAGS		+= -Wno-unused-function -Wno-unused-variable

obj-y			+= dwc_otg.o usb_hw.o

dwc_otg-objs		:= dwc_otg_driver.o dwc_otg_attr.o
dwc_otg-objs		+= dwc_otg_cil.o dwc_otg_cil_intr.o
dwc_otg-objs		+= dwc_otg_pcd_linux.o dwc_otg_pcd.o dwc_otg_pcd_intr.o
dwc_otg-objs		+= dwc_common_linux.o dwc_otg_adp.o
