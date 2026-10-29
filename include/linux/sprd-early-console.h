/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SPRD_EARLY_CONSOLE_H
#define _LINUX_SPRD_EARLY_CONSOLE_H

#ifdef CONFIG_ARCH_SPRD
/*
 * Stop painting printk output into the bootloader framebuffer. The RAM
 * log that TWRP reads back as /proc/last_kmsg keeps being written.
 */
void sprd_early_console_panel_off(void);
#else
static inline void sprd_early_console_panel_off(void) { }
#endif

#endif /* _LINUX_SPRD_EARLY_CONSOLE_H */
