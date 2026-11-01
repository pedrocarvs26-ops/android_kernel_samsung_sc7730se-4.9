# Debugging on Android and in TWRP

Updated 2026-10-31. The SM-T560 now boots Android 11 with this kernel.
ADB is the normal diagnostic path; TWRP is the fallback for boot failures.
See [STATUS](STATUS.md) before interpreting a registered device as working hardware.

## Running Android

```sh
adb devices -l
adb shell uname -a
adb shell cat /proc/cmdline
adb logcat -b all -d -v threadtime > logcat.txt
adb shell dmesg > dmesg.txt
adb shell dumpsys battery > battery.txt
adb shell dumpsys sensorservice > sensors.txt
adb shell dumpsys location > location.txt
```

Some files require root/debuggable adbd. Logs and screenshots can contain personal
information; review before publishing. Keep complete logs before filtering them.
A disconnect after reboot is not sufficient evidence that Android failed to boot;
check the device and USB connection before repeating a flash.

Useful targeted checks:

- Sensors: changing XYZ events reaching Android, not only a sensor name in the HAL.
- GNSS: `/dev/ttyS3`, `/proc/tty/driver/sprd_serial`, GPS power state, process
  stability and actual satellites/fix. Do not consume the serial port while gpsd
  owns it. A running daemon alone is not a successful fix.
- Vibration: Android accepts a request and the LED transient driver switches on/off,
  but the owner felt no vibration in either test. Do not infer a working motor.
- Power: `/sys/kernel/debug/suspend_stats`, `sleep_time` and `wakeup_sources` can
  help when accessible. Screen-off state, deep sleep and unplugged idle drain are
  different measurements; USB charging can affect the experiment.
- Thermal: battery temperature is available, but `/sys/class/thermal` was absent.
  Do not use that battery reading as proof of CPU thermal protection.

## Recovery after a failed boot

Keep a known-working boot backup for the same tablet and ROM. Recovery is a
separate partition; restore Boot/KERNEL rather than overwriting unrelated partitions.
For a fresh failure, collect existing recovery interfaces first:

```sh
adb shell cat /proc/last_kmsg > last_kmsg.txt
adb shell ls -l /sys/fs/pstore
adb shell cat /proc/cmdline > recovery-cmdline.txt
adb shell cat /proc/iomem > recovery-iomem.txt
```

Their availability and contents depend on the running and previous kernels.
The current port implements Samsung sec_log support; the old claim that this
kernel cannot produce a vendor-format previous-boot log is obsolete.
A recovery reboot may replace some logs, so identify which boot produced them.

## Early-console experiments and physical-memory dumps

Earlier bring-up used simplefb/fbcon and a 1 MiB ramoops region at `0x89b00000`.
These are historical diagnostic configurations, not a guarantee that a normal
Android image will show an on-screen kernel console. Enabling fbcon previously
conflicted with Android's display ownership. Inspect the actual DTS, config and
boot arguments before relying on a console or reserved-memory layout.

The historical `tools/memdump.c` helper produced a raw dump plus printable text.
Before using it, confirm the reservation and recovery mappings in the exact image
being tested. Do not copy old addresses into a different layout unconditionally.

Observed hazards from early testing:

- Blind DRAM scans reset the tablet when they touched protected/unmapped regions.
- Raw reads of the `sec_log` window at `0x86b00000` reset the tested recovery.
  Prefer `/proc/last_kmsg` instead of raw access to that window.
- A read within the reserved modem window `0x88000000..0x89c00000` completed in
  earlier tests; that is not permission to scan the rest of physical memory.
- A cold power-off loses DRAM logs; bootloader/recovery activity may also overwrite
  buffers. An empty buffer alone does not identify the failed initialization step.
- Some recovery `dd` implementations cannot represent high physical-memory offsets.

Check `tools/memdump.c`, the current board DTS and `/proc/iomem` together before
any deliberate raw-memory investigation. Direct memory writes/marker experiments
are not part of the normal boot or peripheral-validation workflow.
