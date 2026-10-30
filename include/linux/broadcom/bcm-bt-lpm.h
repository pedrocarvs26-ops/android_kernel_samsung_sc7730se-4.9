/*
 * Broadcom Bluetooth low power mode driver platform data definition.
 *
 * Copyright (C) 2013 Broadcom Corporation
 * Licensed under the GPL-2.
 */

#ifndef BCM_BT_LPM_SETTINGS_H
#define BCM_BT_LPM_SETTINGS_H

struct uart_port;

struct bcm_bt_lpm_platform_data {
	int bt_wake_gpio;
	int host_wake_gpio;
};

int bcm_bt_lpm_assert_bt_wake(void);
int bcm_bt_lpm_deassert_bt_wake(void);
int bcm_bt_lpm_uart_ioctl(struct uart_port *uport, unsigned int cmd,
			  unsigned long arg);

#endif /* BCM_BT_LPM_SETTINGS_H */
