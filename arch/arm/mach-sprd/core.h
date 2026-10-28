/*
 * Spreadtrum SC7730SE / SC8830 (SCX35 / SCX30G) Platform Internal Header
 *
 * Licensed under the GPL-2.
 */

#ifndef __ARCH_ARM_MACH_SPRD_CORE_H
#define __ARCH_ARM_MACH_SPRD_CORE_H

#include <linux/reboot.h>

void __init sprd_early_console_map_io(void);
void __init sprd_init_irq(void);
void sprd_restart(enum reboot_mode mode, const char *cmd);

#endif /* __ARCH_ARM_MACH_SPRD_CORE_H */
