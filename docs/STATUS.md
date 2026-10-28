# Status

Last updated: 2026-10-27.

## What this repository is

A complete, bootable Linux **4.9.337** kernel tree for the Samsung Galaxy Tab E 9.6
(`SM-T560` / `gtelwifi` and `SM-T561` / `gtel3g`, Spreadtrum SC7730SE `scx30g`/`scx35`),
with core platform bring-up and essential hardware drivers ported from the working
3.10.108 kernel (`android_kernel_samsung_gtel3g`).

## Port completeness

| Area | State |
| --- | --- |
| Machine descriptor & static peripheral iomap (`arch/arm/mach-sprd/sprd.c`, `include/soc/sprd/iomap.h`) | **done & verified** (`0xF5000000` static map matching 3.10) |
| Cortex-A7 SMP bring-up (`platsmp.c`, `headsmp.S`) | **done & fixed** (removed fatal `0x12000000` SCU abort; programmed `SCX30G` holding pen `0x14` and jump regs `0x18..0x24`) |
| Secondary IRQ controllers (`INTC0..3` & `AON_INTC` in `arch/arm/mach-sprd/irq.c`) | **done & fixed** (clocks enabled, 1-to-1 GIC-400 SPI pass-through enabled, legacy summary IRQs masked) |
| Clockevent + clocksource + sched_clock (`drivers/clocksource/timer-sprd.c`) | **done & fixed** (`GPTIMER0` Ch1 32,768 Hz clocksource + `sched_clock`, Ch0 clockevent with `TIMER_CTL_NEW`, `SYSCNT` at 1,000 Hz) |
| Bootloader FDT / `initrd` handoff (`arch/arm/boot/compressed/atags_to_fdt.c`) | **done & fixed** (preserves `/chosen/linux,initrd-start` and `linux,initrd-end` from S-Boot's FDT when booting with `CONFIG_ARM_APPENDED_DTB=y`) |
| Early framebuffer console + Samsung `sec_log` `/proc/last_kmsg` (`arch/arm/mach-sprd/early_console.c`) | **done** (live `800x1280` screen console from `.map_io` + `0x86B00000` `LOGM` ring buffer readable in TWRP `/proc/last_kmsg`) |
| On-screen console (`simplefb` + `fbcon` at `0x9ea44000`) | **done** |
| Persistent RAM console (`pstore`/`ramoops` at `0x89b00000` with `unbuffered;`) | **done** |
| ADI master/slave bus (`arch/arm/mach-sprd/adi.c`) | **done** (`0x40030000`/`0x40038000` hardware protocol) |
| Watchdog reboot (`sprd_restart` in `arch/arm/mach-sprd/sprd.c`) | **done** (SC2723 A-Die watchdog + AP watchdog fallback, supports `recovery`/`download`) |
| Common Clock Framework (`drivers/clk/clk-scx35.c` + `scx30g-clocks.dtsi`) | **done** |
| SC2723 PMIC regulators (`drivers/regulator/sc2723-regulator.c` + `sc2723-regulators.dtsi`) | **done** (all 24 DCDC/LDO rails registered) |
| GPIO & EIC (`drivers/gpio/gpio-sprd.c`) | **done** (`d_gpio`, `d_eic`, `a_gpio`, `a_eic` with linear IRQ domains) |
| Hardware I2C (`drivers/i2c/busses/i2c-sprd.c`) + GPIO I2C | **done** (`i2c0..i2c4` + `i2c_gpio9`) |
| eMMC / MicroSD / Wi-Fi SDIO (`drivers/mmc/host/sdhci-sprd.c`) | **done** (`sprd-sdhci.3` eMMC, `sprd-sdhci.0` SD, `sprd-sdhci.1` Wi-Fi with `mask \| 0x08` reset & `div - 1` divider) |
| Backlight (`drivers/video/backlight/rt8555-backlight.c`) | **done** (Richtek RT8555 on `i2c-gpio-9` + GPIO 214/190) |
| Keypad & Power/VolDown EIC keys (`drivers/input/keyboard/sc_keypad.c`) | **done** (`0x40250000` matrix + A-Die EIC power/voldown) |
| Touchscreen (`drivers/input/touchscreen/zt7554_ts.c`) | **done** (Zinitix ZT7554 on `i2c1@0x20`, GPIO 94 IRQ, 5-finger MT Type B + touchkeys) |
