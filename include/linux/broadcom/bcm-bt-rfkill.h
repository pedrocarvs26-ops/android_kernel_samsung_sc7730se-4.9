/*
 * Broadcom Bluetooth rfkill power control via GPIO.
 *
 * Copyright (C) 2013 Broadcom Corporation
 * Licensed under the GPL-2.
 */

#ifndef BCM_BT_RFKILL_SETTINGS_H
#define BCM_BT_RFKILL_SETTINGS_H

#include <linux/rfkill.h>

#define BCM_BT_RFKILL_VREG_ON           1
#define BCM_BT_RFKILL_VREG_OFF          0
#define BCM_BT_RFKILL_N_RESET_ON        0       /* keep in reset */
#define BCM_BT_RFKILL_N_RESET_OFF       1
#define BCM_BT_RFKILL_UNUSED_GPIO       (-1)

struct bcm_bt_rfkill_platform_data {
	int bcm_bt_rfkill_vreg_gpio;
	int bcm_bt_rfkill_n_reset_gpio;
	struct rfkill *rfkill;
};

#endif /* BCM_BT_RFKILL_SETTINGS_H */
