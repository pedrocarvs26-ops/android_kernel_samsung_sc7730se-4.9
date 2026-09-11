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
| Serial console (upstream `sprd_serial`, no vendor code) | wired up, `ttyS1` assumption unverified, UART jig optional |
| On-screen console (`simplefb` + `fbcon` on the bootloader's framebuffer) | done, **primary debug channel**, needs no jig and no memory reads |
| Persistent RAM console (`pstore`/`ramoops` at `0x89b00000`) | done, read from TWRP with `tools/memdump` |
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

## How this port is debugged (no UART jig)

There is no serial jig for this device, so there are two channels.

**The screen.** The bootloader leaves the panel running and hands over its framebuffer
(`lcd_base=0x9ea44000`). `simple-framebuffer` + `fbcon` print the kernel log straight onto
the tablet, so a failed boot can be photographed. Nothing has to be read out of physical
memory, which matters a great deal here: raw `/dev/mem` reads have reset this device
twice.

**The RAM console.** `ramoops`, 1 MiB at `0x89b00000`, dumped from TWRP after a warm
reboot. Picking that address took three attempts:

| attempt | result |
| --- | --- |
| `0x89b00000`, top of the modem window | a 1 MiB read **succeeded**, and returned stale CP firmware strings |
| `0x86b80000`, inside the bootloader's `sec_log=0xffe00@0x86b00000` window | reading it from recovery **resets the tablet instantly**: the recovery kernel carves that window out of its own memory map, so the read faults in kernel context and the vendor kernel is built with `panic_on_oops` |
| back to `0x89b00000` | the only region that is both kept out of the recovery kernel's allocator and still inside its linear map |

What is still open is whether the bootloader reloads CP firmware into the modem window on
a *recovery* boot, which would wipe the log before it could be read. `memdump --mark`, a
reboot back into recovery, and `memdump --check` settle that on the device without
flashing anything.

After a boot attempt, hold Volume Up + Home + Power to get back into TWRP (never cut the
power) and dump it:

```sh
chmod +x /sdcard/memdump
/sdcard/memdump                     # 0x89b00000 + 0x100000 -> /sdcard/ramoops.bin
tail -n 200 /sdcard/ramoops.bin.txt
```

`memdump` (`tools/memdump.c`, cross-compiled statically by CI and shipped in the kernel
artifact) replaces `dd` here. DRAM starts at `0x80000000`, so the byte offset does not fit
in a signed 32 bit `off_t`; `dd` falls back to reading physical address 0, which is below
`PHYS_OFFSET`, and fails with `dd: /dev/mem: Bad address`. `memdump` uses a 64 bit
`pread()` with an `mmap()` fallback and writes a printable-only `.txt` next to the dump.

`initcall_debug` and `ignore_loglevel` are in the default command line, and
`CONFIG_PANIC_ON_OOPS` plus the hung-task and softlockup panics are enabled, so a hang turns
into a recorded panic instead of a silent black screen. Full procedure and an interpretation
table: [`DEBUG-TWRP.md`](DEBUG-TWRP.md).

## Most likely first failures

In rough order of probability, so you know where to look:

1. **Link errors from `platsmp.c`** if a symbol moved in 4.9
   (`secondary_startup`, `v7_exit_coherency_flush`, `cpu_do_idle`, `scu_enable`).
2. **`dtc` warnings** about unit addresses on `scu@12000000` / `syscnt@40230000`, or
   about the missing `#clock-cells` on nodes that reference clocks. Warnings are not
   fatal.
3. **An empty `ramoops` dump.** No text and no `DBGC` header means the kernel never reached
   `arch_initcall_sync`, so it died in the decompressor, in `head.S` or in the DTB handoff
   and nothing was ever written to RAM. That narrows it down a lot; work through the
   checklist in [`DEBUG-TWRP.md`](DEBUG-TWRP.md), starting with `nosmp` and the boot.img
   offsets. (The `ttyS1` console index is a separate assumption that only matters if a UART
   jig is ever attached; `earlycon=sprd_serial,0x70100000` never depends on it.)
4. **Hang right after `smp_prepare_cpus`.** Boot with `nosmp` or `maxcpus=1` first; the
   power-up sequence in `platsmp.c` is transcribed from the vendor kernel but its delays
   and the SCU enable were never validated.
5. **`VFS: Unable to mount root fs`.** Expected: there is no eMMC driver yet. Reaching
   this message means the port booted, which is the current milestone.

## Roadmap

1. Boot far enough to record `VFS: Unable to mount root fs` in the RAM console.
2. scx30g clock gates (`drivers/clk/sprd`) so peripherals beyond the UART can be clocked.
3. pinctrl, GPIO and EIC.
4. SC2723 PMIC and regulators (upstream `sc27xx` drivers).
5. `sdhci-sprd` for eMMC, then a real rootfs.
6. `simple-framebuffer` on the bootloader framebuffer, then DRM.
7. USB, Wi-Fi, touchscreen, sensors.
8. Mali-400: out of scope here, Lima needs kernel 5.2 or newer.
