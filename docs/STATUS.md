# Status

Last updated: 2026-09-10.

## What this repository is

A complete Linux **4.9.337** source tree (pristine kernel.org release as the first
commit) with a new Spreadtrum SC7730SE platform port committed on top of it. Everything
needed to build is in the tree; nothing has to be downloaded or patched in.

## Port completeness

| Area | State |
| --- | --- |
| Machine descriptor (`arch/arm/mach-sprd`) | done |
| SMP bring-up and CPU hotplug (`platsmp.c`) | done, **untested on hardware** |
| Clockevent + clocksource + sched_clock (`drivers/clocksource/timer-sprd.c`) | done, **untested on hardware** |
| Device trees for SM-T560 and SM-T561 | done |
| Bring-up defconfig (`sc7730se_defconfig`) | done |
| Serial console (upstream `sprd_serial`, no vendor code) | wired up, `ttyS1` assumption unverified |
| GIC, SCU, syscon nodes | done |
| Clock controller driver | **not started** (fixed clocks only) |
| pinctrl / GPIO / EIC | **not started** |
| SC2723 PMIC and regulators | **not started** |
| eMMC (`sdhci-sprd`) | **not started**, so no real rootfs yet |
| Display, USB, Wi-Fi, touch, sensors, GPU | **not started** |

## Build status

The port was written and reviewed on a machine where building is not permitted, so
**no binary has been produced yet**. What was verified before the build had to be
stopped:

* `make sc7730se_defconfig` resolves with **no unknown-symbol warnings**, so the Kconfig
  wiring (`ARCH_SPRD`, `ARCH_SC7730SE`, `SPRD_TIMER`, `SERIAL_SPRD`) is consistent.
* `arch/arm/mach-sprd/sprd.o`, `arch/arm/mach-sprd/platsmp.o` and
  `arch/arm/mach-sprd/built-in.o` **compiled cleanly**.
* The build reached `drivers/`, `fs/ext4/` and `block/` with **zero** `error:` lines
  before it was stopped. It never reached `LD vmlinux`, so link errors and `dtc`
  warnings are still unknown.

Build it yourself with GitHub Codespaces or Actions, see the README. Use a **GCC 8 or 9**
cross toolchain: GCC 10 switched to `-fno-common` and breaks Linux 4.9 with
"multiple definition" link errors. `port/build.sh` detects this and warns.

## Most likely first failures

In rough order of probability, so you know where to look:

1. **Link errors from `platsmp.c`** if a symbol moved in 4.9
   (`secondary_startup`, `v7_exit_coherency_flush`, `cpu_do_idle`, `scu_enable`).
2. **`dtc` warnings** about unit addresses on `scu@12000000` / `syscnt@40230000`, or
   about the missing `#clock-cells` on nodes that reference clocks. Warnings are not
   fatal.
3. **Silence on the serial port.** The console index is an assumption: the vendor kernel
   does not use `ttyS*` names. If `ttyS1` is wrong, try `console=ttyS0`, and keep
   `earlycon=sprd_serial,0x70100000` which does not depend on the index at all.
4. **Hang right after `smp_prepare_cpus`.** Boot with `nosmp` or `maxcpus=1` first; the
   power-up sequence in `platsmp.c` is transcribed from the vendor kernel but its delays
   and the SCU enable were never validated.
5. **`VFS: Unable to mount root fs`.** Expected: there is no eMMC driver yet. Reaching
   this message means the port booted, which is the current milestone.

## Roadmap

1. Serial console boot up to `VFS: Unable to mount root fs`.
2. scx30g clock gates (`drivers/clk/sprd`) so peripherals beyond the UART can be clocked.
3. pinctrl, GPIO and EIC.
4. SC2723 PMIC and regulators (upstream `sc27xx` drivers).
5. `sdhci-sprd` for eMMC, then a real rootfs.
6. `simple-framebuffer` on the bootloader framebuffer, then DRM.
7. USB, Wi-Fi, touchscreen, sensors.
8. Mali-400: out of scope here, Lima needs kernel 5.2 or newer.
