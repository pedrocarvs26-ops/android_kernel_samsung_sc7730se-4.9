# SC7730SE hardware notes

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
| Vendor virtual io base | `SCI_IOMAP_BASE 0xF5000000` (not used by this port) |

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
| `+0x004C` | holding pen (secondary CPUs spin on this) |
| `+0x0050` | CPU jump address table, `+0x04 * cpu` |

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
2. write the physical address of `secondary_startup` into `AP_AHB + 0x50 + 4 * cpu`,
3. write the CPU mask into the holding pen at `AP_AHB + 0x4C`,
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

* Debug UART: **UART1 at `0x70100000`**, GIC SPI 3, 26 MHz source clock.
* The upstream `sprd_serial` driver matches `sprd,sc9836-uart` and provides
  `OF_EARLYCON_DECLARE(sprd_serial, ...)`, so `earlycon=sprd_serial,0x70100000` works before
  clocks and pinctrl are up.
* Physical access to the UART is through the headphone jack with a **619 kOhm** resistor jig.
  That jig is **optional** for this port. The primary log channel is the persistent RAM
  console (`ramoops`, 384 KiB at `0x86b80000`, inside the window the bootloader reserves for
  the stock RAM console), dumped from TWRP with [`tools/memdump.c`](../tools/memdump.c) after
  a warm reboot — see [`DEBUG-TWRP.md`](DEBUG-TWRP.md). `dd` cannot do that read: every DRAM
  address here is above 2 GiB and overflows a 32-bit `off_t`. The vendor 3.10 kernel that
  TWRP runs is built with `CONFIG_STRICT_DEVMEM` off and `CONFIG_DEVKMEM=y`, which is what
  makes reading raw physical RAM from recovery possible at all.
* **The bootloader builds its own kernel command line and ignores the one in the boot.img
  header.** The line recovered from a running device is
  `mem=1536M init=/init ... console=null loglevel=0 sec_log=0xffe00@0x86b00000
  androidboot.bootloader=T560XXU0APL1 ...`. Two consequences: the only command line we
  control is `/chosen/bootargs` in the **appended DTB**, and `CONFIG_ARM_ATAG_DTB_COMPAT`
  must stay **off**, or the decompressor folds that `console=null loglevel=0` into the DTB
  and overwrites ours.
* The bootloader expects a Samsung `boot.img` containing `zImage` with an **appended DTB**
  (`CONFIG_ARM_APPENDED_DTB`, without `ATAG_DTB_COMPAT`), flashed with Odin or heimdall.
* Vendor kernel command line: `androidboot.hardware=sc8830`; vendor `CONFIG_HZ=100`,
  `CONFIG_PAGE_OFFSET=0xC0000000`, `CONFIG_NR_CPUS=4`.
