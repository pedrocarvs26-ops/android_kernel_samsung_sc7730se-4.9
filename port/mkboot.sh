#!/usr/bin/env bash
#
# Package out/zImage-dtb into a flashable boot.img (plus an Odin tar.md5).
#
# A stock SM-T560 boot image was dumped from a device and inspected. It is a
# four part image, and its load addresses are stored relative to zero rather
# than to the DDR base at 0x80000000:
#
#     page_size     2048
#     kernel_addr   0x00008000        (not 0x80008000)
#     ramdisk_addr  0x01000000        (not 0x80800000)
#     second_addr   0x00f00000
#     tags_addr     0x00000100
#     name          "sc8830"
#     cmdline       "buildvariant=userdebug"
#     dt area       665600 bytes after the ramdisk, its size stored in the
#                   header field right after page_size
#
# A plain mkbootimg run cannot reproduce that: most builds have no --dt, and the
# guessed offsets would move the ramdisk. So the image is rebuilt *from the
# stock one*: port/repackboot.py copies the stock 2048 byte header page verbatim,
# keeps the stock ramdisk and dt area byte for byte, swaps only the kernel and
# then fixes up kernel_size, the command line and the SHA1 id.
#
#     STOCK=stock_boot.img ./port/mkboot.sh
#
# Get stock_boot.img with TWRP -> Backup -> Boot (that backup is the raw
# partition), or on the device with
#     dd if=/dev/block/platform/sprd-sdhci.3/by-name/boot of=/sdcard/stock_boot.img
#
set -euo pipefail

cd "$(dirname "$0")/.."

OUT="${OUT:-out}"
BOARD="${BOARD:-gtelwifi}"
KERNEL="${KERNEL:-$OUT/zImage-dtb}"
STOCK="${STOCK:-}"
RAMDISK="${RAMDISK:-}"
# Without a UART jig the persistent RAM console is the only log we get, so make
# it as informative as possible: initcall_debug names every initcall as it runs
# (the last one printed is the one that hung), ignore_loglevel forces everything
# out, and panic=0 halts instead of rebooting so nothing overwrites the buffer.
# The buffer itself is declared in the board DTS; see docs/DEBUG-TWRP.md.
APPEND="${APPEND:-initcall_debug ignore_loglevel panic=0}"

if [ ! -f "$KERNEL" ]; then
	echo "error: $KERNEL not found, run ./port/build.sh first" >&2
	exit 1
fi

if [ -z "$STOCK" ] || [ ! -f "$STOCK" ]; then
	cat >&2 <<-EOF
		error: no stock boot image given.

		The new image is built from the one that is on the tablet right now, so
		that the ramdisk, the vendor dt area and every header field survive:
		    TWRP -> Backup -> Boot, then copy the backup off the device
		    STOCK=stock_boot.img ./port/mkboot.sh
	EOF
	exit 1
fi

mkdir -p "$OUT"

ARGS=(--stock "$STOCK" --kernel "$KERNEL" --append "$APPEND")
if [ -n "$RAMDISK" ]; then
	ARGS+=(--ramdisk "$RAMDISK")
fi

echo "== building $OUT/boot.img"
python3 port/repackboot.py "${ARGS[@]}" --out "$OUT/boot.img"

echo
echo "== building $OUT/boot-nosmp.img (single core, use this for the first boot)"
python3 port/repackboot.py "${ARGS[@]}" --nosmp --out "$OUT/boot-nosmp.img"

echo
echo "== building $OUT/boot_${BOARD}.tar.md5 for Odin"
(
	cd "$OUT"
	tar -H ustar -cf "boot_${BOARD}.tar" boot.img
	md5sum -t "boot_${BOARD}.tar" >> "boot_${BOARD}.tar"
	mv "boot_${BOARD}.tar" "boot_${BOARD}.tar.md5"
)

echo
ls -l "$OUT/boot.img" "$OUT/boot-nosmp.img" "$OUT/boot_${BOARD}.tar.md5"
cat <<EOF

== flashing from TWRP (no PC, no UART jig)
1. TWRP -> Backup -> Boot, before anything else. Restoring that backup is how
   you undo a kernel that does not boot; recovery is not touched.
2. Copy $OUT/boot-nosmp.img to the tablet, then TWRP -> Install -> Install
   Image -> boot-nosmp.img -> Boot partition. SMP bring-up is the least tested
   part of this port, so start on one core.
3. Reboot -> System, wait ~30 s, then hold Volume Up + Home + Power to warm
   reboot back into TWRP. Do not cut the power: the log lives in DRAM and only
   survives a warm reset.
4. In the TWRP terminal (or adb shell), dump the RAM console:
     dd if=/dev/mem bs=4096 skip=563968 count=256 of=/sdcard/ramoops.bin
     strings /sdcard/ramoops.bin | tail -n 200
   563968 is 0x89b00000 / 4096 and 256 pages is the 1 MiB region.
   How to read the result: docs/DEBUG-TWRP.md
5. Once it gets past SMP bring-up, flash $OUT/boot.img for all four cores.

== flashing with Odin (alternative)
Power off, hold Volume Down + Home + Power for download mode, then
Odin -> AP -> boot_${BOARD}.tar.md5, uncheck Auto Reboot, F. Reset Time.
EOF
