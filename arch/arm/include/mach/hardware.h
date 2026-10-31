/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MACH_HARDWARE_H
#define __MACH_HARDWARE_H

#include <linux/types.h>
#include <linux/sizes.h>
#include <soc/sprd/iomap.h>

#define SPRD_DCAM_PHYS		0x60800000UL
#define SPRD_DCAM_SIZE		SZ_64K

#define SPRD_ISP_PHYS		0x60A00000UL
#define SPRD_ISP_SIZE		SZ_32K

#define SPRD_CSI2_PHYS		0x60C00000UL
#define SPRD_CSI2_SIZE		SZ_4K

#ifdef __KERNEL__
extern uint32_t dcam_regbase;
#undef DCAM_BASE
#define DCAM_BASE		dcam_regbase
#undef SPRD_DCAM_BASE
#define SPRD_DCAM_BASE		dcam_regbase

extern void __iomem *g_sprd_isp_base;
#undef SPRD_ISP_BASE
#define SPRD_ISP_BASE		((unsigned long)g_sprd_isp_base)
#endif

#ifndef SPRD_ADISLAVE_BASE
#define SPRD_ADISLAVE_BASE	(SPRD_ADI_BASE + 0x00008000)
#endif
#define SPRD_FLASH_OFST		0x0000
#define SPRD_FLASH_CTRL_BIT	BIT(0)
#define SPRD_FLASH_LOW_VAL	0x3
#define SPRD_FLASH_HIGH_VAL	0xf

#endif /* __MACH_HARDWARE_H */
