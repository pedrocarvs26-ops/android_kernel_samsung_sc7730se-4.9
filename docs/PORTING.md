# Porting notes: Linux 4.9.337 on SC7730SE

Updated 2026-10-31. The port now boots Android 11 on SM-T560; consult
[STATUS](STATUS.md) for measured/owner-confirmed functions and open work.

## Architecture and scope

This is a complete Linux 4.9.337 source tree with SC7730SE support built on top.
The machine directory is `arch/arm/mach-sprd/`; reference behavior comes from the
Samsung/Spreadtrum 3.10 trees. It is a vendor-driver forward port with compatibility
work, not an unchanged upstream kernel or a promise of upstream-only drivers.

Device trees describe gtelwifi and gtel3g. The port uses ADI access, vendor static
I/O mappings, Common Clock Framework clocks, regulators, GPIO/EIC and I2C, alongside
ported storage, multimedia and Android support. The original goals of “no static
mapping” and “only wiring changes to upstream files” no longer describe this tree.

## Platform and driver areas

| Area | Source / role |
| --- | --- |
| Platform / SMP / IRQ / ADI | `arch/arm/mach-sprd/`, `include/soc/sprd/iomap.h` |
| Timing | `drivers/clocksource/timer-sprd.c` |
| Clocks / power rails | `drivers/clk/clk-scx35.c`, `drivers/regulator/sc2723-regulator.c` |
| Device descriptions / configuration | `arch/arm/boot/dts/sc7730se*.dts*`, `arch/arm/configs/sc7730se_defconfig` |
| Storage / USB | `drivers/mmc/host/sdhci-sprd.c`, vendor USB gadget/OTG integration |
| Graphics / multimedia | Ported Mali, display/GSP, DCAM, ISP, sensor, JPEG/VSP and memory-management support |
| Audio | Spreadtrum ASoC/SC2723 codec, DMA and DAPM routes |
| Input / sensors | Zinitix touch, keypad, K2HH, SX9500, headset switch and evdev compatibility |
| GPS power | `drivers/misc/sec_gps_bcm47520.c`; navigation also requires Android userspace |
| Vibration | `drivers/leds/leds-sc2723-vibrator.c`; physical vibration remains unconfirmed |

## Bring-up lessons retained in the current tree

- SMP and IRQ setup are SoC-specific. SCX30G holding-pen/jump registers differ
  from older layouts; use the implemented definitions, not generic Cortex-A7
  register assumptions.
- Preserve S-Boot's boot image layout and installed-ROM ramdisk when repacking.
  Appended DTB configuration and initrd handoff must remain consistent with the
  bootloader. Do not enable ATAG/DTB compatibility blindly: bootloader command-line
  merging previously overwrote useful diagnostic boot arguments.
- Multimedia domains may be off during early platform mapping. Mapping DCAM/ISP/
  CSI2 as early static devices caused boot failure; driver-time mappings and
  correct power/clock sequencing are essential.
- Mali-400 support uses the ported vendor stack; the old statement that 4.9 means
  software-rendering-only is obsolete. Graphics still depend on matching userspace.
- Camera capture needs the entire sensor, DCAM/ISP, memory, encoding and audio
  pipeline. Saving a photo does not by itself validate video recording.
- v38 connected microphone input routes and corrected ADC capture stream names.
  Before that, video finalization stalled waiting for audio frames.
- v41 handles the legacy ioctl collision (`0x40044591`) that revoked the sensor
  HAL's evdev descriptor. Sensor registration alone did not prove data delivery.
- v42 supplies UART3 and its clock for GPS. GNSS then exposed an independent
  Android HIDL NMEA-buffer abort; that fix belongs in Android userspace, not the
  kernel. See [STATUS](STATUS.md) for its publication boundary.

## Build and test workflow

```sh
export CROSS_COMPILE=/path/to/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
JOBS=4 ./port/build.sh
STOCK=/path/to/known-working-boot.img ./port/mkboot.sh
```

The build helper prefers GNU `ld.bfd`: gold rejects the linker script's `OVERLAY`.
The AOSP ARM GCC 4.9 toolchain is the development reference; validate other
compilers separately. Keep output directories and the intended board explicit.

Back up the current boot image, install to the correct Boot/KERNEL partition,
then verify `uname`, Android boot and each changed hardware path. The SM-T561
board is not physically validated by SM-T560 tests.

Use ADB logs on a booted device and recovery logs for boot failures. See
[DEBUG-TWRP](DEBUG-TWRP.md). A root-filesystem panic was an early milestone and is
now a regression, not a successful end state.

## Current priorities

Suspend/idle measurements, thermal protection, SELinux Enforcing, remaining
physical peripheral tests, reproducible userspace delivery and separate SM-T561
validation. GPS work is deferred. Do not turn implementation claims into functional
checkmarks without hardware evidence.
