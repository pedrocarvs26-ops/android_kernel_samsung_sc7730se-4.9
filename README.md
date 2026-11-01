# Linux 4.9.337 for Samsung Galaxy Tab E 9.6 (SC7730SE)

A Linux **4.9.337** port for the Spreadtrum SC7730SE, developed against the
Samsung/Spreadtrum 3.10 vendor sources and tested with **LineageOS 18.1 / Android 11**.

**Status updated 2026-10-31:** the kernel boots and is in use on a physical
**SM-T560 (`gtelwifi`)**. Display, touch, Wi-Fi, Bluetooth, cameras, audio,
accelerometer, USB modes and other everyday functions work. This is an experimental
port with remaining validation work; it is not a claim that every subsystem is complete.

| Target | Validation |
| --- | --- |
| SM-T560 / `gtelwifi` / Wi-Fi | Physical-device testing and owner confirmations |
| SM-T561 / `gtel3g` / 3G | Board source exists; no equivalent physical validation in this work, including modem/telephony |

Hardware: four Cortex-A7 cores up to 1.3 GHz, Mali-400 MP2, 1.5 GB RAM,
800 x 1280 display. The Android userspace and proprietary HALs are part of the
working setup; replacing the kernel alone does not reproduce every userspace fix.

## Working and pending features

| Feature | Current evidence |
| --- | --- |
| Android boot, display/backlight, touch and internal storage | Working on SM-T560 |
| Mali graphics / composition | Drivers integrated; usable graphical interface; no current FPS benchmark claimed |
| Wi-Fi | Connected with Android internet validation |
| Bluetooth | Owner confirms working |
| Front/rear cameras | Owner confirms photos and video working |
| Speaker and microphone | Playback and recording work; recorded video contains audio |
| Accelerometer / screen rotation | Owner-confirmed after v41; changing XYZ events verified in Android |
| Physical buttons | Owner confirms working |
| USB ADB, MTP, USB tethering and OTG | Working; owner confirms the latter three |
| Battery readings and USB charging | Charge, voltage and temperature readings available; charging verified |
| CPU frequency scaling | Frequency controls and approximately 730–1300 MHz table available |
| GPS / GNSS | Satellite reception confirmed, **no position fix confirmed**; deferred |
| Vibration | Android and direct-driver commands accepted, **no physical vibration felt**; unresolved |
| Wired headset / microphone / buttons | Driver integrated; physical test deferred |
| microSD | Driver integrated; no card available for a physical test |
| Deep sleep / idle drain | Not measured; owner reports apparently better overall endurance than 3.10 |
| Thermal management | Battery temperature available; CPU thermal protection/throttling not validated; `/sys/class/thermal` absent in the inspected boot |
| SELinux | Tested setup is **Permissive**; Enforcing not validated |

See [STATUS](docs/STATUS.md) for evidence, limitations and the kernel/userspace boundary.

## Build

The tested development workflow uses the AOSP ARM GCC 4.9 toolchain and **GNU ld.bfd**
(the kernel linker script uses `OVERLAY`, which gold rejects).

```sh
export CROSS_COMPILE=/path/to/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
JOBS=4 ./port/build.sh
# Alternate board source; not a declaration of SM-T561 hardware validation:
BOARD=gtel3g JOBS=4 ./port/build.sh
```

`port/build.sh` selects `sc7730se_defconfig`, builds in `BUILD_DIR`
(default `/tmp/k49-build`), and writes `out/zImage`, both board DTBs and the selected
`out/zImage-dtb`. Set `BUILD_DIR` and `OUT` to use different output directories.
It prefers `ld.bfd` when available. Other compilers require their own validation;
GCC 10+ also needs compatibility adjustments handled by the script.

The [devcontainer](.devcontainer/devcontainer.json) and
[CI template](port/ci-build.yml) are build helpers, not substitutes for device testing.

## Boot image and installation

Use a known-working boot image for the **same device and installed ROM** as the
repack base. Preserve its ramdisk, Samsung device-tree area and header fields;
do not reconstruct them from generic offsets or use a different model's image.

```sh
STOCK=/path/to/known-working-boot.img ./port/mkboot.sh
```

Review the script and its outputs before installation. Keep a boot backup and a
working recovery/download path. Install the intended image to Boot/KERNEL using
TWRP or an appropriate Samsung flashing tool. This documentation update does not
flash a device. Historical test images are kept on the repository's
[`boot-images` branch](https://github.com/pedrocarvs26-ops/android_kernel_samsung_sc7730se-4.9/tree/boot-images).
The latest kernel test image in this work is **v42**; it is not a complete ROM.

Current success means booting Android and exercising hardware, not stopping at a
root-filesystem panic. Single-core boot and framebuffer-console experiments are
fallback diagnostics, not the normal installation workflow.

## Documentation

- [Current status and remaining validation](docs/STATUS.md)
- [Porting decisions and recent fixes](docs/PORTING.md)
- [Hardware references](docs/HARDWARE.md)
- [ADB and recovery debugging](docs/DEBUG-TWRP.md)

This repository contains the complete Linux source tree plus the port. Original
Linux subsystem documentation remains applicable to its own subject; it is not a
hardware-support checklist. Local `.k49tools` reference/work directories are not
release documentation.

## Credits and license

Linux 4.9.337 upstream; Samsung/Spreadtrum 3.10 vendor kernel and Android device
sources; contributors to the SC7730SE bring-up and physical-device testing.
Kernel code is GPL-2.0; retain the licenses of individual files.
