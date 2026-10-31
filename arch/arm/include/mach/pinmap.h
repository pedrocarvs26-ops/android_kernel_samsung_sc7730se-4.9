/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MACH_PINMAP_H
#define __MACH_PINMAP_H

#include <linux/types.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <soc/sprd/iomap.h>

#define BITS_PIN_DS(_x_)                ( ((_x_) << 18) & (BIT(18)|BIT(19)|BIT(20)|BIT(21)) )
#define BIT_PIN_SLP_AP                  ( BIT(13) )
#define BIT_PIN_SLP_CP0                 ( BIT(14) )
#define BIT_PIN_SLP_CP1                 ( BIT(15) )
#define BIT_PIN_SLP_CP2                 ( BIT(16) )
#define BITS_PIN_SLP(_x_)               ( ((_x_) << 13) & (BIT(13)|BIT(14)|BIT(15)|BIT(16)) )
#define BIT_PIN_WPU_SEL                 ( BIT(12) )
#define BIT_PIN_WPUS                    ( BIT(12) )
#define BIT_PIN_WPU                     ( BIT(7) )
#define BIT_PIN_WPD                     ( BIT(6) )
#define BITS_PIN_AF(_x_)                ( ((_x_) << 4) & (BIT(4)|BIT(5)) )
#define BIT_PIN_SLP_WPU                 ( BIT(3) )
#define BIT_PIN_SLP_WPD                 ( BIT(2) )
#define BIT_PIN_SLP_IE                  ( BIT(1) )
#define BIT_PIN_SLP_OE                  ( BIT(0) )

#define BIT_PIN_NUL                     ( 0 )
#define BIT_PIN_SLP_NUL                 ( 0 )
#define BIT_PIN_SLP_Z                   ( 0 )
#define BIT_PIN_NULL                    ( 0 )

#define REG_PIN_TRACEDAT6               ( 0x0040 )
#define REG_PIN_CCIRCK0                 ( 0x0304 )
#define REG_PIN_CCIRCK1                 ( 0x0308 )
#define REG_PIN_CCIRMCLK                ( 0x030c )
#define REG_PIN_CCIRHS                  ( 0x0310 )
#define REG_PIN_CCIRVS                  ( 0x0314 )
#define REG_PIN_CCIRD0                  ( 0x0318 )
#define REG_PIN_CCIRD1                  ( 0x031c )
#define REG_PIN_CCIRD2                  ( 0x0320 )
#define REG_PIN_CCIRD3                  ( 0x0324 )
#define REG_PIN_CCIRD4                  ( 0x0328 )
#define REG_PIN_CCIRD5                  ( 0x032c )
#define REG_PIN_CCIRD6                  ( 0x0330 )
#define REG_PIN_CCIRD7                  ( 0x0334 )
#define REG_PIN_CCIRD8                  ( 0x0338 )
#define REG_PIN_CCIRD9                  ( 0x033c )
#define REG_PIN_CCIRRST                 ( 0x0340 )
#define REG_PIN_CCIRPD1                 ( 0x0344 )
#define REG_PIN_CCIRPD0                 ( 0x0348 )
#define REG_PIN_SCL0                    ( 0x034c )
#define REG_PIN_SDA0                    ( 0x0350 )
#define REG_PIN_KEYOUT2                 ( 0x035c )

static inline void pinmap_set(u32 offset, u32 val)
{
	writel_relaxed(val, (void __iomem *)(SPRD_PIN_BASE + offset));
}

static inline u32 pinmap_get(u32 offset)
{
	return readl_relaxed((void __iomem *)(SPRD_PIN_BASE + offset));
}

#endif /* __MACH_PINMAP_H */
