# Linux 4.9 for Spreadtrum SC7730SE (Samsung Galaxy Tab E 9.6)

A from-scratch mainline-style port of **Linux 4.9.337** (final 4.9 LTS release) to the
Spreadtrum/Unisoc **SC7730SE** SoC, targeting:

| Model | Codename | Variant |
|---|---|---|
| SM-T560 | `gtelwifi` | Wi-Fi only |
| SM-T561 | `gtel3g` | 3G |

* SoC: SC7730SE (family `scx35`, chip `x30g` / "tshark2"), 4x Cortex-A7 @ 1.3 GHz, Mali-400 MP2
* RAM: 1.5 GB, DDR base `0x80000000`
* Stock software: Android 4.4.4 with Linux **3.10.17**

> **Status: work in progress. Not yet validated on hardware.**
> This tree builds a `zImage` + appended DTB. It has *not* been proven to boot on a
> real device, and no display, storage, USB, PMIC, modem or GPU support is wired up yet.
> Treat every boot attempt as a potential brick risk and keep a working stock firmware
> plus Odin at hand.

## Why 4.9 and not mainline

Upstream Spreadtrum support (`arch/arm64/boot/dts/sprd`, `drivers/clk/sprd`, `sprd_serial`,
SC27xx PMIC, `sdhci-sprd`) exists only for **arm64** SoCs (SC9860/SC9863A and newer).
There is nothing upstream for the 32-bit `scx35` generation, and the SoC has:

* **no PSCI / no secure monitor firmware** usable for CPU bring-up (custom `smp_ops` required),
* **no ARM architected timer** (timekeeping must use the AON general purpose timers),
* a completely vendor-specific clock, regulator and pinctrl model.

4.9 is the sweet spot: it is the newest LTS whose driver APIs are still close enough to the
3.10 vendor code that hardware drivers can be forward-ported one by one, while `sprd_serial`
(the only usable upstream sprd driver for this era) is already present, so a serial console
can be brought up with no vendor code at all.

## What is implemented

* `arch/arm/mach-sprd/` — new multiplatform machine (`ARCH_SPRD`, `ARCH_SC7730SE`)
  * `sprd.c` — `DT_MACHINE_START`, compatible with `sprd,sc7730se` / `sc7730` / `sc8830` / `scx35`
  * `platsmp.c` — DT-driven SMP bring-up (AP_AHB holding pen + jump address, PMU_APB core
    power domains), CPU hotplug via SCU + `v7_exit_coherency_flush`
* `drivers/clocksource/timer-sprd.c` — new driver for the `scx35` AON general purpose timer
  (clockevent, periodic + oneshot) and the system counter (clocksource + `sched_clock`),
  including the AON_APB clock gating the bootloader may not have enabled
* `arch/arm/boot/dts/sc7730se.dtsi` + `sc7730se-gtelwifi.dts` + `sc7730se-gtel3g.dts`
  — CPUs, GIC, SCU, PMU, syscon regions, timers, 4x UART, memory and vendor carve-outs
* `arch/arm/configs/sc7730se_defconfig` — small bring-up config (serial console first)

Secondary CPUs currently rely on tick broadcast (single global clockevent + `dummy_timer`),
which ARM enables automatically through `ARCH_HAS_TICK_BROADCAST` on SMP.

## Hardware documentation

The physical address map, register layouts and bit definitions were reverse engineered from
the Samsung/Spreadtrum 3.10 vendor kernel and are documented in
[`docs/HARDWARE.md`](docs/HARDWARE.md). The porting methodology, design decisions and the
remaining work are in [`docs/PORTING.md`](docs/PORTING.md), and the current state of every
subsystem plus the failures to expect on a first build/boot are in
[`docs/STATUS.md`](docs/STATUS.md).

## Repository layout

This is a **full Linux 4.9.337 tree**, not a patch set: the first commit is the pristine
kernel.org release and the port is committed on top of it, so `git clone` is all you need.

```
arch/arm/mach-sprd/                     new machine (sprd.c, platsmp.c, core.h, Kconfig)
drivers/clocksource/timer-sprd.c        new timer + system counter driver
arch/arm/boot/dts/sc7730se*.dts*        SoC dtsi + SM-T560 and SM-T561 board files
arch/arm/configs/sc7730se_defconfig     bring-up config
port/build.sh                           one command cross build
port/mkboot.sh                          boot.img + Odin tar.md5 packaging
port/repackboot.py                      swaps the new kernel into a stock boot.img
port/ci-build.yml                       GitHub Actions workflow template
.devcontainer/devcontainer.json         Codespaces environment (Ubuntu 20.04 + GCC 9)
docs/DEBUG-TWRP.md                      reading the kernel log without a UART jig
docs/                                   hardware map, porting notes, status
```

## Building

### In GitHub Codespaces (recommended)

Open the repository in a Codespace. The included
[`.devcontainer/devcontainer.json`](.devcontainer/devcontainer.json) provisions Ubuntu 20.04
with the `arm-linux-gnueabihf` cross toolchain, `dtc`, `ccache` and `abootimg`, and exports
`ARCH=arm` and `CROSS_COMPILE=arm-linux-gnueabihf-` for you. Then:

```sh
./port/build.sh                 # SM-T560 (gtelwifi)
BOARD=gtel3g ./port/build.sh    # SM-T561
```

The script configures the tree, builds `zImage` and both DTBs, and writes the appended-DTB
image the Samsung bootloader expects:

```
out/zImage
out/sc7730se-gtelwifi.dtb
out/sc7730se-gtel3g.dtb
out/zImage-dtb            <- zImage + DTB of the selected board
```

### By hand

```sh
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-
make sc7730se_defconfig
make -j"$(nproc)" zImage dtbs

# appended DTB image expected by the Samsung bootloader
cat arch/arm/boot/zImage arch/arm/boot/dts/sc7730se-gtelwifi.dtb > zImage-dtb
```

### Toolchain

Use **GCC 8 or 9**. GCC 10 made `-fno-common` the default, which breaks Linux 4.9 with
"multiple definition" link errors; `port/build.sh` detects a newer compiler, warns and
retries with `-fcommon`, but a matching toolchain is much less painful. Ubuntu 20.04 ships
GCC 9 as `gcc-arm-linux-gnueabihf`, and the AOSP `arm-linux-androideabi-4.9` prebuilt works
too.

### In CI

[`port/ci-build.yml`](port/ci-build.yml) is a ready GitHub Actions workflow that builds both
boards inside an `ubuntu:20.04` container and uploads the images as artifacts. Copy it to
`.github/workflows/build.yml` and push to enable it (that path needs a token with the
`workflow` scope, which is why it is not enabled by default).

## Flashing (read the warnings first)

TWRP is enough for the whole loop: back up boot, flash a test kernel, read the log, put the
stock kernel back. No UART jig and no PC are required — see
[`docs/DEBUG-TWRP.md`](docs/DEBUG-TWRP.md).

1. **Back up the stock boot partition first** (TWRP → Backup → *Boot*). Restoring that
   backup is how you undo a kernel that does not boot. Recovery lives in its own partition
   and is never touched by any of this.
2. Repackage the new kernel **into the boot image that is on the tablet right now**, which
   keeps the ramdisk, the vendor device tree area and every header field:

   ```sh
   # TWRP -> Backup -> Boot hands you the raw partition; copy it off the device
   STOCK=stock_boot.img ./port/mkboot.sh
   # -> out/boot.img, out/boot-nosmp.img, out/boot_gtelwifi.tar.md5
   ```

   A stock SM-T560 image was dumped and inspected, and its header is *not* what the vendor
   kernel's `Makefile.boot` suggests: the addresses are relative to zero (`kernel_addr`
   `0x00008000`, `ramdisk_addr` `0x01000000`, `tags_addr` `0x00000100`, page size 2048, name
   `sc8830`) and a **665 600 byte device tree area follows the ramdisk**. Rebuilding the
   image from hand written `mkbootimg` offsets drops that area and moves the ramdisk, so
   [`port/repackboot.py`](port/repackboot.py) copies the stock header page verbatim, keeps
   the stock ramdisk and dt area byte for byte, and swaps only the kernel.
3. Flash it from the tablet: copy `out/boot.img` over and use TWRP → Install → Install
   Image → *Boot*. (`out/boot_*.tar.md5` is the same image packed for Odin — AP slot, Auto
   Reboot off — if you would rather use a PC.)
4. **Read the log without a UART jig.** Two channels, and the first one needs no tooling
   at all: the kernel prints its log **on the tablet's own screen** through `simplefb` +
   `fbcon`, reusing the framebuffer the bootloader leaves running, so a failed boot can
   simply be photographed. The same log also goes to a persistent RAM console (`ramoops`,
   1 MiB at `0x89b00000`, the top of the modem window — the one region that is both kept
   out of the recovery kernel's allocator and still readable from it). After a boot
   attempt, warm reboot into recovery with **Volume Up + Home + Power** and dump it:

   ```sh
   chmod +x /sdcard/memdump
   /sdcard/memdump                     # 0x89b00000 + 0x100000 -> /sdcard/ramoops.bin
   tail -n 200 /sdcard/ramoops.bin.txt
   ```

   `memdump` ([`tools/memdump.c`](tools/memdump.c)) is a small static ARM helper that CI
   builds next to the kernel and ships in the same artifact; it also writes a
   printable-only `.txt`, so the recovery does not need a `strings` binary. Do **not** use
   `dd` for this: DRAM starts at `0x80000000`, that offset does not fit in a signed 32 bit
   `off_t`, so `dd` ends up reading physical address 0 and prints
   `dd: /dev/mem: Bad address`.

   Never cut the power in between — the log lives in DRAM and only survives a warm reset.
   [`docs/DEBUG-TWRP.md`](docs/DEBUG-TWRP.md) explains how to read the output and what an
   empty buffer means. A 619 kOhm jig on the headphone jack (115200 8N1,
   `earlycon=sprd_serial,0x70100000`) still works if you ever get one, but it is optional.
5. First boot: flash `out/boot-nosmp.img` instead. It carries `nosmp maxcpus=1` both in the
   boot image command line and in `/chosen/bootargs` of the appended DTB, because SMP
   bring-up is the least tested part of this port. Move to `out/boot.img` once the log shows
   it getting past that.
6. Reaching `VFS: Unable to mount root fs` is the current success criterion: there is no
   storage driver yet.

## Credits

* Spreadtrum/Samsung 3.10 vendor sources (`gtel3g/android_kernel_samsung_gtel3g`) — register
  maps and bring-up sequences
* postmarketOS wiki for `samsung-gtelwifi`
* Mainline `timer-sprd.c` (v5.4) as a style reference (different timer generation)

Kernel sources are GPL-2.0. New files added by this port carry GPL-2.0 headers.
