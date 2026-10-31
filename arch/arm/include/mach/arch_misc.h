/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MACH_ARCH_MISC_H
#define __MACH_ARCH_MISC_H

#include <linux/types.h>

static inline int sprd_is_fpga(void) { return 0; }
static inline u32 sci_get_ana_chip_id(void) { return 0x27230000; }
static inline int sci_get_ana_chip_ver(void) { return 0xA090; }

#define SPRD_FLASH_LOW_CUR	110
#define SPRD_FLASH_HIGH_CUR	470

#endif /* __MACH_ARCH_MISC_H */
