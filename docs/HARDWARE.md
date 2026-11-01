# SC7730SE hardware notes

Updated 2026-10-31. This is a register/reference document, not a functional
checklist. SM-T560 boots Android 11; see [STATUS](STATUS.md) for current validation.
SM-T561 board-source availability does not imply physical validation.

## Validated setup and peripheral boundaries

- SM-T560 / gtelwifi, SC7730SE, four Cortex-A7 cores, Mali-400 MP2,
  1.5 GB RAM and 800 x 1280 display; Linux 4.9.337 with LineageOS 18.1.
- Internal storage, display/touch, Wi-Fi, Bluetooth, cameras, audio, buttons and
  USB ADB/MTP/tethering/OTG work in the tested setup.
- K2HH acceleration reaches Android and screen rotation works after the v41
  evdev compatibility fix. SX9500 appearing in the HAL does not prove that
  a physical grip/SAR sensor is functional on this model.
- BCM47520 GPS uses UART3 at `0x70300000`, `/dev/ttyS3`, 921600 baud, and
  `clk_uart3` with a 26 MHz parent. GPIO 168 supplies the exported control at
  `/sys/class/sec/gps/GPS_PWR_EN/value`. Chip response and satellite reception
  are observed; a position fix is not. Android userspace fixes are also required.
- SC2723 vibration is exposed through `/sys/class/leds/vibrator/` with transient
  trigger `state`, `duration` and `activate`. Android and direct requests switch
  driver state, but neither produced vibration felt by the owner. Do not equate
  a driver or register write with an operational motor.
- Wired-headset physical tests are deferred; microSD has no card available.
  Battery readings/USB charging work; CPU thermal protection and deep sleep
  remain unvalidated. SELinux is Permissive.

The tables below retain vendor-reference information. The implemented source
is authoritative where current SoC-specific handling differs from an older map.

Everything below was recovered from the Samsung/Spreadtrum **3.10 vendor kernel** for
`gtelwifi` / `gtel3g` (`arch/arm/mach-sc/`, `arch/arm/mach-sc/include/mach/chip_x30g/__regs_*.h`,
`arch/arm/boot/dts/sprd-scx35*.dtsi`) and cross-checked against the vendor device tree.
It is the reference used to write `arch/arm/mach-sprd/`, `drivers/clocksource/timer-sprd.c`
and `arch/arm/boot/dts/sc7730se*.dts*` in this tree.

## 1. Identification

| Item | Value |
|---|---|
| SoC family | `scx35` |
| Chip directory in vendor tree | `chip_x30g` (a.k.a. "tshark2") |
| Vendor SoC id | `sprd,sc-id = <8830 11 0x20000>` |
| Vendor board string | `Spreadtrum SP7730G board` |
| CPUs | 4x Cortex-A7, DT `reg = <0xf00>` .. `<0xf03>` |
| PMU (perf) interrupts | GIC SPI 92, 93, 94, 95 (one per core) |
| Architected timer | **absent** (must use AON GP timers) |
| PSCI / secure firmware | **absent** (custom `smp_ops` required) |
| Vendor virtual io base | `SCI_IOMAP_BASE 0xF5000000` (used by ported vendor-compatible mappings) |

## 2. Physical address map

| Block | Base | Size | Notes |
|---|---|---|---|
| IRAM0 | `0x00000000` | | boot ROM/SRAM window |
| CORE / SCU | `0x12000000` | `0x100` | `arm,cortex-a7-scu` |
| GIC distributor | `0x12001000` | `0x1000` | |
| GIC CPU interface | `0x12002000` | `0x2000` | |
| DMA0 | `0x20100000` | | |
| USB (musb/dwc) | `0x20200000` | | |
| SDIO0 / SDIO1 / SDIO2 | `0x20300000` / `0x20400000` / `0x20500000` | | |
| EMMC | `0x20600000` | | |
| LCDC | `0x20800000` | | |
| HWLOCK0 | `0x20C00000` | | |
| AP_AHB syscon | `0x20D00000` | `0x10000` | SMP holding pen lives here |
| DSI | `0x21800000` | | |
| ANA_REGS_GLB (ADI) | `0x40038800` | | SC2723 PMIC transport |
| GP timer (AON) | `0x40050000` | `0x20` per timer | clockevent used by this port |
| EIC | `0x40210000` | | |
| AP timer0 | `0x40220000` | | timer0 + timer1 (broadcast) |
| SYSCNT | `0x40230000` | `0x1000` | free running counter |
| GPIO | `0x40280000` | | |
| AP timer1 | `0x40330000` | | |
| AP timer2 | `0x40340000` | | |
| PMU_APB syscon | `0x402B0000` | `0x10000` | CPU power domains |
| AON_APB syscon | `0x402E0000` | `0x10000` (SZ_64K) | always-on clock gates |
| UART0 | `0x70000000` | | source clock 48 MHz |
| UART1 | `0x70100000` | | source clock 26 MHz, debug console |
| UART2 | `0x70200000` | | |
| UART3 | `0x70300000` | | |
| APBCKG | `0x71200000` | | AP APB clock gating |
| APBREG (AP_APB syscon) | `0x71300000` | `0x10000` | |
| INTC0..INTC3 | `0x71400000` / `0x71500000` / `0x71600000` / `0x71700000` | | legacy vendor intc |
| DDR | `0x80000000` | `0x60000000` | 1.5 GB |

## 3. Interrupts (GIC SPI numbers)

| Source | SPI |
|---|---|
| UART0 / UART1 / UART2 / UART3 | 2 / 3 / 4 / 5 |
| GP timer0 (`0x40050000`) | 28 |
| AP timer0 timer0 (`0x40220000`) | 29 |
| AP timer0 timer1 (broadcast timer) | 118 |
| AP timer1 (`0x40330000`) | 119 |
| AP timer2 (`0x40340000`) | 121 |
| Cortex-A7 PMU, cores 0..3 | 92 / 93 / 94 / 95 |

Vendor DT listed the timer node regs in the order
`SYSCNT, GPTIMER0, APTIMER0, APTIMER1, APTIMER2` with `interrupts = <118, 28, 29, 119, 121>`.

## 4. General purpose timer registers

Each timer instance is `0x20` bytes apart inside its block:

| Offset | Register |
|---|---|
| `+0x00` | `LOAD` |
| `+0x04` | `VALUE` |
| `+0x08` | `CTL` |
| `+0x0C` | `INT` |
| `+0x10` | `CNT_RD` |

`CTL` bits:

| Bit | Meaning |
|---|---|
| `BIT(6)` | `1` = periodic mode, `0` = one shot |
| `BIT(7)` | timer enable |
| `BIT(8)` | "new" (extended) mode |

`INT` bits:

| Bit | Meaning |
|---|---|
| `BIT(0)` | interrupt enable |
| `BIT(1)` | raw status |
| `BIT(2)` | masked status |
| `BIT(3)` | interrupt clear (write 1) |
| `BIT(4)` | busy (a load is still being latched) |

System counter (`0x40230000`):

| Offset | Register |
|---|---|
| `+0x04` | `COUNT` |
| `+0x08` | `CTL` |
| `+0x0C` | `SHADOW_CNT` (safe to read, 32 bit) |

All of these run from the 32.768 kHz always-on clock. The vendor used
`clocksource_mmio_readw_up` on `SHADOW_CNT`, which truncates the counter to 16 bits; this
port uses `clocksource_mmio_readl_up` with a 32 bit mask.

## 5. AON_APB clock gates (base `0x402E0000`)

| Register | Offset |
|---|---|
| `APB_EB0` | `+0x0000` |
| `APB_EB1` | `+0x0004` |

| Gate | Register | Bit |
|---|---|---|
| `AP_SYST_EB` (system counter) | `EB0` | `BIT(10)` |
| `AON_TMR_EB` (AON GP timer) | `EB0` | `BIT(11)` |
| `AP_TMR0_EB` | `EB0` | `BIT(12)` |
| `AP_TMR1_EB` | `EB1` | `BIT(9)` |
| `AP_TMR2_EB` | `EB1` | `BIT(10)` |

The vendor `sci_enable_timer_early()` sets `AON_TMR | AP_SYST | AP_TMR0` in `EB0` and
`AP_TMR1 | AP_TMR2` in `EB1` before touching any timer register. This port does the same for
the gates it needs, from `timer-sprd.c`, because the bootloader is not guaranteed to.

## 6. SMP bring-up and CPU power domains

AP_AHB (`0x20D00000`) offsets:

| Offset | Register |
|---|---|
| `+0x0000` | `AHB_EB` |
| `+0x0008` | `CA7_RST_SET` |
| `+0x0014` | SCX30G holding pen |
| `+0x0018..0x0024` | SCX30G CPU jump address table |
| `+0x004C` | legacy holding pen (compatibility write in current implementation) |
| `+0x0050` | legacy CPU jump table, `+0x04 * cpu` (compatibility write) |

PMU_APB (`0x402B0000`) offsets:

| Offset | Register |
|---|---|
| `+0x0008` | `PD_CA7_C1_CFG` (core 1) |
| `+0x000C` | `PD_CA7_C2_CFG` (core 2) |
| `+0x0010` | `PD_CA7_C3_CFG` (core 3) |
| `+0x00BC` | `PWR_STATUS0_DBG` |

Power domain config bits:

| Field | Encoding |
|---|---|
| `FORCE_SHUTDOWN` | `BIT(25)` |
| `AUTO_SHUTDOWN_EN` | `BIT(24)` |
| `PWR_ON_DLY(x)` | `(x << 16) & GENMASK(23,16)` |
| `PWR_ON_SEQ_DLY(x)` | `(x << 8) & GENMASK(15,8)` |
| `ISO_ON_DLY(x)` | `x & GENMASK(7,0)` |

Vendor delay values: `PWR_ON_DLY = 1000`, `PWR_ON_SEQ_DLY = 500`, `ISO_ON_DLY = 60` (us units
of the AON clock).

Power status is read from `PWR_STATUS0_DBG`: core *n* is fully powered **down** when
`((status >> (4 * (n + 1))) & 0xf) == 0x7`. Per-core status/power bit groups seen in the
vendor code: core1 `BIT(8..10)`, core2 `BIT(12..14)`, core3 `BIT(16..18)`.

Bring-up sequence used by this port:

1. clear `FORCE_SHUTDOWN` and set the delay fields in the core's `PD_CA7_Cn_CFG`,
2. write the physical address of `secondary_startup` into `AP_AHB + 0x18 + 4 * cpu` (plus the legacy jump-table compatibility write),
3. write the CPU mask into the holding pen at `AP_AHB + 0x14` (plus the legacy holding-pen compatibility write),
4. deassert the core reset in `CA7_RST_SET`.

Hotplug down: `v7_exit_coherency_flush(all)`, leave SCU coherency, then
`FORCE_SHUTDOWN` and poll `PWR_STATUS0_DBG`.

## 7. Memory layout and vendor carve-outs

| Region | Base | Size |
|---|---|---|
| DDR | `0x80000000` | `0x60000000` |
| Shared memory (SMEM) | `0x87800000` | `0x240000` |
| CP / modem | `0x88000000` | `0x1C00000` |
| Framebuffer | `0x9EA44000` | `0xBB8000` |
| ION | `0x9F5FC000` | `0x7D4000` |

Boot addresses from the vendor `Makefile.boot`:

| Symbol | Value |
|---|---|
| `zreladdr` | `0x80008000` |
| `params_phys` | `0x80000100` |
| `initrd_phys` | `0x80800000` |

## 8. Console and boot chain

- Debug UART1 is at `0x70100000`, GIC SPI 3, with a 26 MHz source clock.
  UART3 at `0x70300000` is used by GPS, not the debug console.
- Early bring-up experimented with a bootloader framebuffer at `0x9ea44000`,
  simplefb/fbcon and ramoops at `0x89b00000`. These are historical configurations;
  current Android display ownership and logging must be checked in the actual image.
- The current tree implements Samsung sec_log / `/proc/last_kmsg` support.
  Prefer existing log interfaces over raw physical-memory reads. Earlier raw
  reads of `0x86b00000` and blind memory scans reset the tested recovery.
  See [DEBUG-TWRP](DEBUG-TWRP.md) for recovery evidence and limitations.
- S-Boot may supply its own command line. Appended DTB arguments and initrd
  handoff need to be consistent with the boot image. Blindly enabling
  `CONFIG_ARM_ATAG_DTB_COMPAT` previously merged `console=null loglevel=0`
  over useful diagnostic arguments.
- Preserve the known-working boot image's header, ramdisk and Samsung device-tree
  area. Vendor `Makefile.boot` physical addresses alone are not sufficient to
  reconstruct an installed ROM's boot image.
- Vendor userspace identifies hardware as `sc8830`. SM-T560 validation does not
  establish modem or model-specific hardware support on SM-T561.
