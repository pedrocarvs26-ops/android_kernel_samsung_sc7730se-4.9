# Debugging this port from TWRP

The SM-T560/T561 has no exposed UART header. The usual trick is a 619 kOhm
resistor jig in the headphone jack, but this port is being developed without
one, so **TWRP is the only diagnostic channel**. Everything below is built
around that constraint.

## What the device has already told us

These are measurements, not assumptions. Each one changed the design.

| Fact | How we know | Consequence |
| --- | --- | --- |
| The bootloader passes `sec_log=0xffe00@0x86b00000` | recovered from DRAM in a dump taken from TWRP | that megabyte is reserved on **every** boot, recovery included: the safest place for a persistent log |
| The bootloader ignores the command line in the boot.img header | the recovered line contains none of the words we wrote there | only the **appended DTB** `/chosen/bootargs` configures the kernel |
| The bootloader's own line contains `console=null loglevel=0` | same dump | `CONFIG_ARM_ATAG_DTB_COMPAT` must stay **off**, or that line replaces ours |
| `0x89b00000` holds live modem data | a dump of it came back full of SIPC ring names (`sbuf_0_*`, `sblock_0_*`, `BML Downloaded NV Partition`) | the first `ramoops` address was inside the CP window and unusable |
| `dd if=/dev/mem` cannot reach DRAM | `dd: /dev/mem: Bad address` | every DRAM address here is above 2 GiB and overflows a 32-bit `off_t`; use `memdump` |
| A blind scan of `0x80000000..0xa0000000` resets the tablet | `memdump --scan` killed the TWRP session instantly | that range contains TrustZone and modem carve-outs; a non-secure read aborts on the bus and the SoC reboots. `--scan` was removed |

The full recovered command line, for reference:

```
mem=1536M init=/init ram=1536M lcd_id=ID000003 lcd_base=9ea44000
initrd=0x85500000,0x7793fa wfixnv=0x88240000,0x40000 wruntimenv=0x88280000,0x60000
bootmode=2 sec_debug.reset_reason=0x1A2B3C11 hw_revision=11 muic_rustproof=0
sec_debug.level=0 androidboot.debug_level=0x4f4c console=null loglevel=0
sec_log=0xffe00@0x86b00000 androidboot.bootloader=T560XXU0APL1
androidboot.emmc_checksum=0 mem_cs=2, mem_cs0_sz=30000000 ... calmode=0
```

In recovery you can read the same thing directly with `cat /proc/cmdline`.

## Memory map of the log window

```
0x86b00000  +---------------------------+  bootloader reserves 1 MiB here
            | struct sec_log_buffer     |  24 B: sig, start, size, from, to
            | recovery kernel's sec_log |  TWRP writes from here upwards,
            | (a few tens of KiB)       |  tens of KiB per boot
0x86b80000  +---------------------------+  <- our ramoops zone starts
            | ramoops console, 384 KiB  |  'DBGC' header + plain text
0x86be0000  +---------------------------+
            | slack                     |
0x86bffe18  | sec_log_ptr (4 B)         |
0x86bffe1c  | sec_log_mag (4 B, 'LOGM') |
0x86c00000  +---------------------------+
```

The 512 KiB below our zone is deliberate: the recovery kernel re-initialises
`sec_log` at the base of the window on every boot, so anything we put there
would be overwritten by TWRP itself before we could read it.

The board `.dts` files declare the zone as:

```
ramoops@86b80000 {
	compatible = "ramoops";
	reg = <0x86b80000 0x60000>;
	console-size = <0x60000>;
	record-size = <0>;
	ecc-size = <0>;
};
```

`record-size = <0>` leaves the console zone as the only zone. That matters:
Linux 4.9 zlib-compresses dmesg records, but never the console zone, so the
buffer stays readable with nothing but `strings`.

## Procedure after a failed boot

1. Flash the image: TWRP -> `Install` -> `Install Image` -> pick the `.img` ->
   partition **Boot**.
2. Copy `memdump` to `/sdcard` **before** rebooting. It ships in the same CI
   artifact as the kernel.
3. Reboot and let the tablet sit on the black screen for ~40 s.
4. Warm reboot into recovery: **Volume Up + Home + Power**. Never pull the
   battery or the charger: the log lives in DRAM and only survives a warm
   reset.
5. In the TWRP terminal or `adb shell`:

```sh
cp /sdcard/memdump /tmp/memdump
chmod +x /tmp/memdump
/tmp/memdump
tail -n 200 /sdcard/ramoops.bin.txt
```

`memdump` writes both `/sdcard/ramoops.bin` and a printable-strings version at
`/sdcard/ramoops.bin.txt`, so no `strings` binary is needed in the recovery.

## memdump reference

```sh
./memdump                                # 0x86b80000 + 0x60000 -> /sdcard/ramoops.bin
./memdump 0x86b80000 0x60000 /sdcard/x   # any range, any output file
./memdump --probe                        # 32 bytes at each known log address
./memdump --seclog [outfile]             # the whole bootloader sec_log window,
                                         # with the vendor header decoded
```

`--probe` is the safe replacement for the old `--scan`: it reads a handful of
specific addresses instead of walking DRAM, so it cannot wander into a
TrustZone carve-out and reset the device.

`--seclog` is useful even before flashing anything: run it from a normal TWRP
session and the recovery kernel's own boot log should appear. That proves the
window is real, that `/dev/mem` reads work, and shows how many bytes TWRP
writes (`write ptr`), which is what the 512 KiB of slack is sized against.

If `/dev/mem` is missing: `mknod /dev/mem c 1 1`.

## Reading the result

| What you see | What it means |
| --- | --- |
| `signature: 0x43474244 'DBGC'` plus kernel text | the kernel booted far enough to register the console. The last line before the truncation is where it died |
| `signature: 0x00000000`, buffer all zeros | ramoops never initialised. Either the kernel died before `arch_initcall_sync` (where the reserved-memory device is created), or it never ran at all |
| `signature: 0x00000000`, buffer full of unrelated text | wrong address: something else owns that memory. This is what happened at `0x89b00000` |
| `memdump: reading ...: Bad address` | the address is outside what the recovery kernel maps; check `/proc/cmdline` and `--probe` |

A single word can also be read without `memdump`:

```sh
busybox devmem 0x86b80000 32   # expect 0x43474244 after a successful boot
```

That goes through `mmap`, which passes a much weaker address check than
`read()`, so it works where `dd` does not.

## Other channels worth checking

```sh
ls -l /proc/last_kmsg /sys/fs/pstore
```

`/proc/last_kmsg` belongs to the recovery kernel, not ours: the stock 3.10 tree
builds with `CONFIG_SEC_LOG_LAST_KMSG=y` and copies the previous contents of
the `sec_log` buffer there **if** the trailing magic is `LOGM`. Our ramoops
header is not that format, so it will not show our log today. Making the port
write a `sec_log`-compatible buffer is a possible future step, and would give
us `/proc/last_kmsg` for free.

## If the buffer is empty

Work through this in order:

1. `cat /proc/cmdline` and confirm `sec_log=0xffe00@0x86b00000` is still what
   this bootloader passes. If the address moved, update the two `.dts` files.
2. `/tmp/memdump --seclog` from a plain TWRP boot. If even the recovery
   kernel's own log is not there, the problem is the dump path, not the port.
3. `/tmp/memdump --probe` to see whether a `DBGC` header landed somewhere else.
4. Confirm the flashed kernel actually has the plumbing: the CI refuses to
   upload a build whose DTB has no `ramoops` node or whose `.config` lost
   `CONFIG_PSTORE_RAM=y`, and every artifact carries `out/BUILD-INFO.txt` with
   the commit it was built from. Check that file before blaming the device.
5. Try the single-core image (`--nosmp`). If SMP bring-up is what hangs, it
   hangs before the console is registered, which looks identical to "nothing
   ran".
6. As a last resort, add the parameters to `/chosen/bootargs` in the `.dts`
   (not to the boot.img header, which this bootloader ignores):
   `ramoops.mem_address=0x86b80000 ramoops.mem_size=0x60000
   ramoops.console_size=0x60000 ramoops.record_size=0 ramoops.ecc=0`

## Moving the region

If a future TWRP build turns out to write further into the window, change the
`ramoops@86b80000` node in both board `.dts` files and the `DEFAULT_ADDR` /
`DEFAULT_SIZE` defines at the top of `tools/memdump.c`. Keep the zone inside
`0x86b00000..0x86c00000`: that is the only range this bootloader is known to
keep out of both kernels' allocators.
