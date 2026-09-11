# Debugging this port from TWRP, with no UART jig

There is no serial jig for this tablet, so the kernel has to tell us what happened some
other way. There are two channels, and they are independent on purpose:

1. **The screen.** The bootloader leaves the panel initialised and hands over its
   framebuffer (`lcd_base=0x9ea44000`). `simple-framebuffer` + `fbcon` print the kernel
   log directly on the tablet, so a failed boot can simply be photographed. Nothing has
   to be read out of physical memory for this, which is the whole point.
2. **The RAM console.** `pstore`/`ramoops`, 1 MiB at `0x89b00000`, read back from
   recovery with [`tools/memdump.c`](../tools/memdump.c) after a warm reboot. This one
   keeps the text after the panel is gone, and survives a panic and reset.

## Two rules, both learned by resetting the tablet

**1. Never scan memory blindly.** The first version of `memdump` had a `--scan` mode that
walked `0x80000000..0xa0000000` a megabyte at a time. On the device:

```
~ # /tmp/memdump --scan
scanning 0x80000000..0xa0000000 for ramoops ('DBGC') and sec_log ('LOGM')
[pedro@archlinux ~]$
```

The shell died because the tablet reset. That range contains TrustZone carve-outs and
modem windows; a non-secure read aborts on the bus and the SoC turns the abort into an
immediate reboot. `--scan` no longer exists.

**2. Never read the bootloader's `sec_log` window at `0x86b00000`.** It is tempting: the
stock command line really does contain `sec_log=0xffe00@0x86b00000`, so the bootloader
reserves that megabyte on every boot. But reading it from recovery does this:

```
~ # /tmp/memdump 0x86b00000 0x100000 /sdcard/seclog.bin
[pedro@archlinux ~]$
```

Instant reset, before even the first line of output. The reason is that the recovery
kernel carves that window out of *its own* memory map (the vendor code reserves it and
re-maps it non-cacheable), so the `/dev/mem` `read()` path resolves it to a virtual
address that is not mapped, faults in kernel context, and the Samsung kernel is built
with `panic_on_oops`. A log we cannot read back is worth nothing, so the RAM console does
not live there.

`memdump` now refuses both of these, and refuses any address outside the window that has
actually worked on this device unless you pass `--force`.

## What is readable from recovery

| range | reserved by the stock kernel | readable from TWRP | used for |
| --- | --- | --- | --- |
| `0x86b00000 + 0x100000` (`sec_log`) | yes | **no - resets the device** | stock RAM console |
| `0x87800000 + 0x240000` (`smem`) | yes | untested, do not try | modem shared memory |
| `0x88000000..0x89c00000` (`cp-modem`) | yes | **yes**, 1 MiB `pread` completed | CP firmware; our RAM console is the top MiB |
| `0x9ea44000 + 0xbb8000` (`fb`) | yes | untested | framebuffer, TWRP draws its UI here |
| everything else | no | it is ordinary RAM that TWRP allocates from | nothing useful |

Before trying any *new* address, look at `cat /proc/iomem` in TWRP. Addresses that do not
fall inside a `System RAM` range are holes in the recovery kernel's map, and reading them
is what resets the device.

## Channel 1: the log on the screen

Nothing to install. Flash the kernel, reboot to system, and watch the panel:

* Text appears once `fbcon` takes over, which is early in `device_initcall`. `printk`
  replays everything buffered before that, so the log starts from `Booting Linux...`.
* If the kernel dies before that point the screen stays on the bootloader logo. That
  itself is information: it means we did not reach driver init.
* Photograph the last screen, especially the final `initcall_debug` line - the default
  command line carries `initcall_debug ignore_loglevel`, so the last line names the
  initcall that hung.
* If the text is sheared or the lines wrap diagonally, the bootloader set the panel up the
  other way round. Change `/chosen/framebuffer` in the board DTS to `width = <1280>`,
  `height = <800>`, `stride = <5120>` and rebuild.

## Channel 2: the RAM console

1. Flash `boot.img` from TWRP: **Install → Install Image → Boot**.
2. **Reboot → System.** Wait about 30 seconds even if the screen stays black.
3. Warm reboot into recovery: **Volume Up + Home + Power**. Never pull the power or hold
   Power alone until it is off - the log lives in DRAM and only survives a *warm* reset.
4. In the TWRP terminal, or over `adb shell`:

```sh
cp /sdcard/memdump /tmp/memdump
chmod +x /tmp/memdump
/tmp/memdump                       # 0x89b00000 + 0x100000 -> /sdcard/ramoops.bin
tail -n 200 /sdcard/ramoops.bin.txt
```

`memdump` writes both the raw dump and a printable-only `.txt` next to it, so the recovery
does not need a `strings` binary. `dd` cannot do this read at all: DRAM starts at
`0x80000000`, the offset does not fit in a signed 32 bit `off_t`, and `dd` ends up reading
physical address 0 and printing `dd: /dev/mem: Bad address`.

If `/dev/mem` is missing: `mknod /dev/mem c 1 1`.

## memdump modes

| command | what it does |
| --- | --- |
| `./memdump` | 1 MiB at `0x89b00000` → `/sdcard/ramoops.bin` (+ `.txt`) |
| `./memdump <addr> <size> [out]` | explicit range, inside the safe window |
| `./memdump --probe` | 32 bytes at each address that could hold a log header |
| `./memdump --mark` | write a marker into the RAM console zone |
| `./memdump --check` | is the marker still there after a reboot? |
| `./memdump --force` | allow an address outside `0x88000000..0x89c00000` - expect a reset |

Every access is printed and flushed *before* it happens, so if the device does reset, the
last line on the screen says exactly which address did it.

## The one test worth running before flashing anything

The open question about `0x89b00000` is whether the bootloader reloads CP firmware into
the modem window on a *recovery* boot. If it does, it would wipe our log before we could
read it, and no kernel change could fix that. Two minutes, no flashing, no risk:

```sh
/tmp/memdump --mark                # writes 'MARK' at 0x89b00000
# TWRP -> Reboot -> Recovery
/tmp/memdump --check
```

| result | meaning |
| --- | --- |
| `marker survived` | nothing rewrites the zone across a boot: the RAM console will be readable |
| `marker gone` | the bootloader repopulates the modem window every boot; the screen console becomes the only channel, and the RAM console needs a different home |

## Reading the result

| what you see | meaning |
| --- | --- |
| `signature: 0x43474244 'DBGC'` and kernel text in the `.txt` | the RAM console worked; read the tail, that is where it died |
| `signature: 0x4b52414d 'MARK'` | only our own marker is there: the kernel never reached `pstore` init |
| all zeroes | the zone was never written: the kernel died before `pstore`, or never started |
| CP firmware strings (`sbuf_0_*`, `sblock_0_*`, `BML Downloaded NV Partition`) | the bootloader reloaded modem firmware over the zone - run `--mark`/`--check` to confirm |
| the device resets | you are reading an address outside the safe window; see the two rules above |

Also worth collecting from TWRP, all of them harmless:

```sh
cat /proc/cmdline                  # the line the bootloader really passes
cat /proc/iomem                    # which ranges the recovery kernel maps
ls -l /proc/last_kmsg /sys/fs/pstore
```

`/proc/last_kmsg` is the *vendor* mechanism: the 3.10 kernel exposes the previous boot's
`sec_log` there. It only ever contains a log written by a kernel that implements Samsung's
`sec_log` format, which ours does not, so it will show the recovery kernel's own previous
boot at best.

For a single word without any tooling, `busybox devmem` uses `mmap` and is the safest
possible probe - but the same address rules apply:

```sh
busybox devmem 0x89b00000 32
```

## If the buffer stays empty

1. Was the reboot warm? A cold power-off clears DRAM.
2. Did the bootloader reload CP firmware over the zone? `--mark` / `--check`.
3. Did the kernel get as far as `pstore`? The screen console answers that independently -
   if the panel showed nothing either, the kernel died before driver init, and the next
   step is the `nosmp` image (`port/repackboot.py --nosmp`), which patches the DTB command
   line. Patching the boot.img header command line does nothing on this device: the
   bootloader builds its own line and ignores it.
4. If both channels stay silent across `nosmp` too, the failure is before `start_kernel`
   and the next move is a decompressor-stage beacon written to the RAM console zone.

## Moving the region

The address appears in three places and they must agree:

* `/chosen/framebuffer` and `ramoops@89b00000` in
  [`sc7730se-gtelwifi.dts`](../arch/arm/boot/dts/sc7730se-gtelwifi.dts) and
  [`sc7730se-gtel3g.dts`](../arch/arm/boot/dts/sc7730se-gtel3g.dts)
* `DEFAULT_ADDR` / `DEFAULT_SIZE` and the `SAFE_LO` / `SAFE_HI` guard in
  [`tools/memdump.c`](../tools/memdump.c)
* the `cp-modem` node, which must shrink by exactly as much as the RAM console takes
