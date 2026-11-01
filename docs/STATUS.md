# Current status

Updated **2026-10-31**, following physical SM-T560 testing with Linux 4.9.337
(v42 test image) and LineageOS 18.1 / Android 11. This supersedes the early
serial-console-only and unbooted-port descriptions.

## Validation scope

Tests and owner confirmations below concern **SM-T560 / gtelwifi**. The gtel3g
board and shared drivers are in the source, but SM-T561 modem, telephony and
model-specific components have not been validated by these tests.

“Integrated” means code/driver/HAL exists. “Working” requires observed use or an
owner confirmation. A sysfs node or a running service alone is insufficient.

## Hardware checklist

| Subsystem | Evidence / state |
| --- | --- |
| Boot, platform, SMP and Android integration | Running Android 11 on the physical tablet |
| Display, backlight and touchscreen | Working interface and touch input |
| Mali GPU / GSP | Drivers integrated and interface usable; no measured current FPS guarantee |
| Internal eMMC | Applications, photos, videos and logs read/written |
| Wi-Fi | Connected network with Android INTERNET/VALIDATED capability |
| Bluetooth | Owner confirms functional |
| Both cameras | Owner confirms full operation; photo and video artifacts also recorded during development |
| Speaker / microphone | Playback, PCM recording and video with audio verified in the development history |
| Accelerometer / rotation | Owner confirms normal operation after v41; Android sensorservice receives changing XYZ data |
| Physical buttons | Owner confirms functional |
| ADB | Shell, root access, push/pull and screenshots tested |
| MTP / USB tethering / OTG | Owner confirms functional |
| PTP | Not separately confirmed |
| Battery / USB charging | Actual battery readings and charging observed; battery temperature available |
| CPUFreq | Controls and 729.6–1300 MHz frequency table present; this does not prove thermal protection or deep sleep |
| Wired headset | Detection/button driver integrated; insertion, playback, mic and media-button tests deferred |
| microSD | Host driver integrated; owner has no card to test |
| Vibration | One-second Android request and two-second direct LED-trigger request accepted; owner felt neither. Driver transitions on/off; physical motor presence/condition not established. Owner does not recall vibration on 3.10 either |
| GPS / GNSS | UART data, chip response and satellite reception observed; no position fix confirmed. Deferred by owner |
| Grip/SAR | SX9500 advertised by sensor HAL; not sufficient evidence of a functional physical sensor on this SM-T560 |
| Idle / suspend | No controlled deep-sleep or screen-off drain measurement. Owner reports apparently better overall endurance than 3.10; no battery regression demonstrated |
| CPU thermal control | `/sys/class/thermal` absent in inspected setup; throttling/protection not validated. Battery temperature alone does not establish CPU thermal control |
| SELinux | Permissive; Enforcing policies and operation remain unvalidated |

## Recent fixes and delivery boundary

- **v38 — capture audio:** repaired microphone/ADC DAPM routes and capture stream
  names. PCM recording and camera video with audio subsequently worked.
- **v41 — accelerometer:** legacy sensor HAL used ioctl `0x40044591` for Android
  suspend-block control, colliding with Linux 4.9 `EVIOCREVOKE`. The compatibility
  change prevents revoking the HAL input descriptor; rotation was confirmed by owner.
- **v42 — GPS UART:** enabled UART3 with `clk_uart3` and its 26 MHz parent, making
  `/dev/ttyS3` available to the Broadcom GPS stack.
- **Android userspace, outside this kernel repository:** GPS init/shim changes were
  needed, followed by a fix to `hardware/interfaces/gnss/1.0/default/Gnss.cpp`.
  The legacy NMEA callback supplies a length-delimited buffer. `setToExternal`
  aborted on `data[size] != '\0'`; copying with `hidl_string(nmea, length)` and
  checking invalid input removed that observed abort in the running test.

The GNSS implementation library was rebuilt and installed on the test tablet with
an original-library backup. At this status update, that separate Android-source
change is **not committed/published as part of this kernel repository or a complete
ROM image**. A v42 boot image alone must not be described as including it.

After the userspace fix, the same GPS processes remained running through satellite
reception. The app showed one satellite at a time (signal readings around 25–26)
and zero in use. This is progress, not a completed navigation implementation.

## Remaining work

1. Validate suspend/resume, deep-sleep counters and unplugged idle consumption.
2. Audit CPU thermal sensing, throttling and protection.
3. Validate SELinux Enforcing with appropriate policies.
4. Test wired-headset paths and microSD when hardware is available.
5. Resolve or establish the physical limits of vibration; do not mark it working.
6. Resume GNSS acquisition/assistance testing when requested.
7. Preserve and publish the separate Android userspace changes in a reproducible
   ROM/device-tree workflow, then revalidate after clean installation and reboot.
8. Perform separate physical validation for SM-T561 before claiming model parity.

Overall endurance, a successful boot, or a driver probe is not a substitute for
these specific measurements. No CTS/VTS, long-duration stress or complete release
qualification is claimed here.
