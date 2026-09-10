# Porting notes: Linux 4.9.337 on SC7730SE

## 1. Approach

* Start from a **pristine kernel.org tarball** (`linux-4.9.337`, the final 4.9 LTS release)
  committed untouched as the first commit, so every SoC change is visible as a diff on top of
  upstream and can be rebased onto another 4.9.x or forward-ported later.
* Write a **new mainline-style machine directory** (`arch/arm/mach-sprd/`) instead of importing
  the vendor `arch/arm/mach-sc/`. The vendor code is a 3.10-era board-file design with a huge
  amount of out-of-tree infrastructure (`sci_glb_regs`, custom iomap at `0xF5000000`, vendor
  clock/regulator frameworks) that cannot be dropped into 4.9.
* **Device-tree first**: no board files, no static platform devices, no hardcoded virtual
  address mappings.
* Use **upstream drivers wherever they exist** (`sprd_serial` is the only usable one for this
  SoC generation) and write minimal new drivers for what timekeeping and SMP require.

## 2. Files added

| File | Purpose |
|---|---|
| `arch/arm/mach-sprd/Kconfig` | `ARCH_SPRD` (multi_v7) + `ARCH_SC7730SE`, selects `ARM_GIC`, `COMMON_CLK`, `GENERIC_IRQ_CHIP`, `HAVE_ARM_SCU if SMP`, `MFD_SYSCON`, `SPRD_TIMER`, `ARM_ERRATA_764369 if SMP`, `PINCTRL` |
| `arch/arm/mach-sprd/Makefile` | `sprd.o` + `platsmp.o` when `CONFIG_SMP` |
| `arch/arm/mach-sprd/core.h` | shared prototypes and the `sprd_smp_ops` declaration |
| `arch/arm/mach-sprd/sprd.c` | `DT_MACHINE_START`, dt_compat `sprd,sc7730se` / `sc7730` / `sc8830` / `scx35` |
| `arch/arm/mach-sprd/platsmp.c` | DT-driven SMP: AP_AHB holding pen + per-CPU jump address, PMU_APB power domains, SCU init, CPU hotplug |
| `drivers/clocksource/timer-sprd.c` | `sprd,scx35-timer` clockevent (periodic + oneshot) and `sprd,scx35-syscnt` clocksource + `sched_clock` |
| `arch/arm/boot/dts/sc7730se.dtsi` | SoC description: CPUs, PMU, GIC, SCU, syscon nodes, timers, 4x UART, fixed clocks |
| `arch/arm/boot/dts/sc7730se-gtelwifi.dts` | SM-T560 board: memory, vendor carve-outs, console, bootargs |
| `arch/arm/boot/dts/sc7730se-gtel3g.dts` | SM-T561 board |
| `arch/arm/configs/sc7730se_defconfig` | small bring-up config, serial console first |

## 3. Upstream files touched (wire-in only)

| File | Change |
|---|---|
| `arch/arm/Kconfig` | `source "arch/arm/mach-sprd/Kconfig"` |
| `arch/arm/Makefile` | `machine-$(CONFIG_ARCH_SPRD) += sprd` |
| `arch/arm/boot/dts/Makefile` | `dtb-$(CONFIG_ARCH_SPRD) += sc7730se-gtelwifi.dtb sc7730se-gtel3g.dtb` |
| `drivers/clocksource/Kconfig` | new `config SPRD_TIMER` |
| `drivers/clocksource/Makefile` | `obj-$(CONFIG_SPRD_TIMER) += timer-sprd.o` |

No other upstream file is modified, which keeps the port easy to review and rebase.

## 4. Design decisions

1. **4.9 instead of 4.4** — 4.9 is the newest LTS of that era, has `sprd_serial` with an
   `earlycon` hook, and its `clockevents` / `clocksource_of_init` / `smp_operations` APIs are
   already the modern ones, so new code written here also applies to newer kernels.
2. **One global clockevent instead of the vendor's four per-CPU timers.** The vendor mapped one
   timer per core (`GPTIMER`, `APTIMER0..2`, irqs 28/29/119/121) plus a broadcast timer
   (irq 118). This port registers a single clockevent with `cpumask = cpu_possible_mask` and
   lets the tick layer use broadcast + `dummy_timer` for the other cores. ARM selects
   `ARCH_HAS_TICK_BROADCAST` automatically when `GENERIC_CLOCKEVENTS_BROADCAST` is on (SMP),
   so `dummy_timer.o` is built. Per-CPU timers are a straightforward later optimisation.
3. **Clock gates poked with a temporary mapping, not syscon/regmap.** `clocksource_of_init()`
   runs from `time_init()`, long before the syscon platform driver is probed, so
   `timer-sprd.c` looks the `sprd,scx35-aon-apb` node up in the DT, `of_iomap()`s it, sets the
   `APB_EB0` bits and unmaps. This removes an init-order landmine.
4. **32 bit reads of `SYSCNT_SHADOW_CNT`.** The vendor used `clocksource_mmio_readw_up`
   (16 bit) which wraps every 2 seconds at 32.768 kHz; `readl_up` with a 32 bit mask is used
   here instead.
5. **No holding-pen assembly.** The bootloader-less bring-up writes
   `virt_to_phys(secondary_startup)` into the AP_AHB jump slot, so `headsmp.S` from the vendor
   tree is unnecessary.
6. **Appended DTB.** The Samsung bootloader on this device does not pass a DTB, so
   `CONFIG_ARM_APPENDED_DTB` and `CONFIG_ARM_ATAG_DTB_COMPAT` are mandatory and the deliverable
   is `zImage-dtb` (`cat zImage foo.dtb`).

## 5. Known risks and unverified assumptions

* **Nothing here has been run on hardware.** The port compiles; boot is unproven.
* **Console index.** `console=ttyS1` assumes `sprd_serial` enumerates the enabled ports in DT
  order with UART0 first. If the console is silent, try `ttyS0`; `earlycon` is address-based
  and therefore reliable.
* **No clock driver.** Only fixed clocks (`ext_26m`, `ext_32k`, `clk_48m`) are described. The
  real `scx30g` gate/PLL tree is not implemented, so anything beyond the UART and the timers
  will not get a clock.
* **No pinctrl.** UART pin muxing is inherited from the bootloader.
* **No PMIC / regulators** (SC2723 over ADI), so no rail can be turned on from Linux.
* **SMP power sequencing timings** are copied from the vendor code and unverified; use `nosmp`
  or `maxcpus=1` for the first boot attempt.
* **Bootloader watchdog.** If the kernel stalls early the device may reset before any output
  appears; that is not necessarily a kernel hang.
* **Memory carve-outs** are copied from the vendor DT; the modem region must stay reserved even
  though no modem driver exists.

## 6. Roadmap

1. **Milestone 1 — serial console.** Reaching
   `Kernel panic - not syncing: VFS: Unable to mount root fs` over UART means the CPU, MMU,
   GIC, timers and console all work. That is the real target of this stage.
2. **Clock driver** for the `scx30g` gates and PLLs (`drivers/clk/sprd/` style, backported).
3. **Pinctrl + GPIO/EIC**.
4. **SC2723 PMIC** (ADI transport at `0x40038800`) and its regulators.
5. **eMMC** (`0x20600000`, sdhci variant) to get a real root filesystem.
6. **Display**: `simple-framebuffer` handover first, then the LCDC/DSI blocks.
7. **USB, Wi-Fi, touchscreen, sensors, thermal, cpufreq**.
8. **GPU**: Lima requires >= 5.2, so Mali-400 acceleration is out of scope for 4.9; software
   rendering only.

## 7. Build and test workflow

```sh
# on the build host
export ARCH=arm
export CROSS_COMPILE="ccache /path/to/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-"
make sc7730se_defconfig
make -j"$(nproc)" zImage dtbs
cat arch/arm/boot/zImage arch/arm/boot/dts/sc7730se-gtelwifi.dtb > zImage-dtb
```

Then repack `zImage-dtb` into the stock `boot.img` (same base, page size and ramdisk) and
flash with Odin. Watch the UART at 115200 8N1 while the device boots.
