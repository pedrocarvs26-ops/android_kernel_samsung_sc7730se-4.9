# Debugging this port with nothing but TWRP

No UART jig is needed. The kernel writes its console into a RAM region that survives a
reboot, and TWRP can read that region back, so the whole edit → flash → read-the-log loop
runs on the tablet alone.

## Why this works

* The board device trees declare a 1 MiB `ramoops` region at **`0x89b00000`**. Linux 4.9
  already understands the `compatible = "ramoops"` reserved-memory binding
  (`drivers/of/platform.c` creates the device at `arch_initcall_sync`, very early), so no
  extra code is involved.
* That address is inside the modem (CP) firmware window. The stock 3.10 kernel that TWRP
  runs reserves the same window at boot and never loads a modem while in recovery, so those
  pages are not allocated, cleared or reused. The framebuffer (`0x9ea44000`) and ION
  (`0x9f5fc000`) regions would **not** be safe: TWRP draws its UI in one and allocates from
  the other.
* The region contains a single **console zone**, because `record-size = <0>` disables the
  dmesg records. That matters for readability: 4.9 stores dmesg records zlib-compressed,
  but never compresses the console. What ends up in RAM is plain text behind a 12-byte
  header (`sig` = `0x43474244` "DBGC", `start`, `size`) — the same layout Samsung's own
  `sec_log` uses on this SoC.
* The vendor kernel is built with `CONFIG_STRICT_DEVMEM` **off** and `CONFIG_DEVKMEM=y`
  (both verified in its `gtel3g_defconfig`), so `/dev/mem` inside TWRP reads physical RAM
  directly.

## The loop

1. **Back up boot, once.** TWRP → Backup → tick *Boot* → Swipe to Backup. This is the undo
   button for everything below. Recovery lives in its own partition and is never touched by
   these experiments, so a kernel that does not boot is not a brick.
2. **Flash the test kernel.** Copy `out/boot.img` to the tablet, then TWRP → Install →
   Install Image → pick `boot.img` → target *Boot*.
3. **Boot it.** Reboot → System, then wait ~30 s. Expect one of:
   * a black screen that stays black — the normal case, keep going;
   * an immediate bounce into download mode or recovery — the bootloader rejected the
     image, re-check the offsets in `port/mkboot.sh` against `abootimg -i stock_boot.img`;
   * nothing that looks like Android even on success — this tree has no display, storage or
     input drivers yet, so a *good* boot is also a silent one.
4. **Warm reboot into TWRP.** Hold **Volume Up + Home + Power** until the tablet resets.
   Do not hold Power alone for ten seconds and do not let the battery run out: the log
   lives in DRAM and only survives a warm reset.
5. **Dump the log with `memdump`.** TWRP → Advanced → Terminal, or `adb shell` from a PC
   (TWRP runs adbd). `memdump` is a small static ARM binary built by CI next to the
   kernel; download it from the same artifact, copy it to the tablet and run:

   ```sh
   chmod +x /sdcard/memdump
   /sdcard/memdump                       # 1 MiB at 0x89b00000 -> /sdcard/ramoops.bin
   tail -n 200 /sdcard/ramoops.bin.txt
   ```

   It writes the raw region to `ramoops.bin` *and* a printable-only version to
   `ramoops.bin.txt`, so no `strings` binary has to exist in the recovery. It also prints
   whether the `DBGC` header is there. If it is not, `/sdcard/memdump --scan` searches
   `0x80000000`–`0xa0000000` for the ramoops and sec_log signatures. If `/dev/mem` does not
   exist, create it with `mknod /dev/mem c 1 1`.

   **`dd` cannot do this job.** `dd if=/dev/mem bs=4096 skip=563968 ...` answers

   ```
   dd: /dev/mem: Bad address
   ```

   because DRAM on this SoC starts at `0x80000000`: every address of interest is past
   2 GiB and does not fit in a signed 32 bit `off_t`. The seek is rejected, `dd` falls back
   to reading from physical address 0, that address is below `PHYS_OFFSET`, and
   `read_mem()` in `drivers/char/mem.c` returns `EFAULT` — the "Bad address" above. Even
   with a correct 64 bit seek, `read()` still has to pass `valid_phys_addr_range()`
   (`arch/arm/mm/mmap.c`), which rejects carved-out regions. `memdump` uses a 64 bit
   `pread()` and falls back to `mmap()`, which only has to pass the far more permissive
   `valid_mmap_phys_addr_range()`.

   Two things worth trying first, since they need no binary at all:

   ```sh
   ls -l /proc/last_kmsg          # the recovery kernel's own RAM console, if enabled
   busybox devmem 0x89b00000 32   # single word, goes through mmap instead of read
   ```

   A `0x43474244` answer from `devmem` means the ramoops header is there and only the
   transfer was the problem.
6. **Keep a copy.** `adb pull /sdcard/ramoops.bin` (or MTP), then read `ramoops.bin.txt`
   on the PC. Attach it to an issue when something is unclear.

## Reading the result

| What you see | What it means |
| --- | --- |
| `Booting Linux on physical CPU 0x0`, then a trailing `calling  <name>+0x0/0x...` line with no matching `initcall <name> returned` | the kernel ran and hung inside that initcall. `initcall_debug` is in the default command line for exactly this reason. |
| a long log ending in `VFS: Unable to mount root fs` | the current success criterion — the port boots, there is simply no eMMC driver yet |
| `Unable to handle kernel paging request` / `Internal error: Oops` plus a backtrace | a real bug with a real stack trace; `CONFIG_PANIC_ON_OOPS=y` stops the machine right there so nothing scrolls it away |
| `BUG: soft lockup` or `INFO: task ... blocked for more than 60 seconds` | a hang that the watchdogs turned into a panic, with the log intact |
| the log from the *previous* attempt | this boot never got far enough to write anything new |
| all zeros, no `DBGC` header anywhere | the kernel never reached `arch_initcall_sync` — see below |

## If the buffer stays empty

An empty buffer is itself information: the failure happened before the first initcalls, so
it is in the decompressor, `head.S`, or the DTB/MMU handoff. Work through these:

1. Boot with `nosmp` (or `maxcpus=1`). SMP bring-up in `arch/arm/mach-sprd/platsmp.c` is the
   least verified code in the tree.
2. Verify the image layout: `abootimg -i out/boot.img` versus the stock boot image.
   Wrong base or kernel offset means the kernel is never even entered.
3. Verify the DTB really got appended — `out/zImage-dtb` must be bigger than `out/zImage` —
   and that `CONFIG_ARM_APPENDED_DTB=y` survived your config edits.
4. Try the other board DTB (`BOARD=gtel3g` / `BOARD=gtelwifi`); a wrong `memory` node is
   fatal this early.
5. Bypass the DTB for the region and pass it on the command line instead:
   `ramoops.mem_address=0x89b00000 ramoops.mem_size=0x100000 ramoops.console_size=0x100000
   ramoops.record_size=0 ramoops.ecc=0`. Use this only if the DTB path is suspect — with
   both in place the second one to probe just logs "already initialized".
6. Only then consider hardware: a 619 kOhm jig on the headphone jack (115200 8N1,
   `earlycon=sprd_serial,0x70100000`) is the sole way to see the decompressor talk.

## Moving the region

If some future TWRP build turns out to touch `0x89b00000`, change it in one place per board
(the `ramoops@89b00000` node in the board `.dts`), shrink or grow the neighbouring
`cp-modem` reservation so the two never overlap, and recompute the `dd` offset as
`address / 4096`. Safe neighbourhoods are the modem window `0x88000000`–`0x89c00000` and the
SMEM window `0x87800000`; avoid the framebuffer and ION.

## Bonus: TWRP is also the flashing tool

Odin and download mode are not required at any point. `port/mkboot.sh` still produces
`boot_<board>.tar.md5` for Odin, but `out/boot.img` flashed from TWRP → Install → Install
Image does the same job, and TWRP → Restore puts the stock kernel back.
