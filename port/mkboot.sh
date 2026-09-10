#!/usr/bin/env bash
#
# Package out/zImage-dtb into a Samsung/Spreadtrum boot.img and an Odin
# flashable tar.md5.
#
# You need a ramdisk. The safest one is the ramdisk from the boot.img that is
# currently on the tablet, because it matches the Android userspace that is
# installed. Extract it from a stock boot.img with:
#
#     abootimg -x stock_boot.img          # gives bootimg.cfg, zImage, initrd.img
#     RAMDISK=initrd.img ./port/mkboot.sh
#
# Offsets below come from the vendor kernel's Makefile.boot and __mmap.h:
#     DDR base      0x80000000
#     zreladdr      0x80008000  -> kernel_offset 0x00008000
#     params_phys   0x80000100  -> tags_offset   0x00000100
#     initrd_phys   0x80800000  -> ramdisk_offset 0x00800000
#
set -euo pipefail

cd "$(dirname "$0")/.."

OUT="${OUT:-out}"
BOARD="${BOARD:-gtelwifi}"
KERNEL="${KERNEL:-$OUT/zImage-dtb}"
RAMDISK="${RAMDISK:-}"
PAGESIZE="${PAGESIZE:-2048}"
BASE="${BASE:-0x80000000}"
# Without a UART jig the persistent RAM console is the only log we get, so make it
# as informative as possible: initcall_debug names every initcall as it runs (the
# last one printed is the one that hung), ignore_loglevel forces everything out,
# and panic=0 halts instead of rebooting so nothing overwrites the buffer.
# The buffer itself is declared in the board DTS; see docs/DEBUG-TWRP.md.
CMDLINE="${CMDLINE:-console=ttyS1,115200n8 earlycon=sprd_serial,0x70100000 no_console_suspend initcall_debug ignore_loglevel panic=0 androidboot.hardware=sc8830}"

if [ ! -f "$KERNEL" ]; then
	echo "error: $KERNEL not found, run ./port/build.sh first" >&2
	exit 1
fi

if [ -z "$RAMDISK" ] || [ ! -f "$RAMDISK" ]; then
	cat >&2 <<-EOF
		error: no ramdisk given.

		Extract one from the boot.img that is currently on the device:
		    abootimg -x stock_boot.img
		    RAMDISK=initrd.img ./port/mkboot.sh
	EOF
	exit 1
fi

if ! command -v mkbootimg >/dev/null 2>&1; then
	cat >&2 <<-EOF
		error: mkbootimg not found.

		Install the Python implementation, which supports the offsets used here:
		    pip3 install --user mkbootimg
		or use the one from an AOSP checkout (system/tools/mkbootimg).
	EOF
	exit 1
fi

mkdir -p "$OUT"

echo "== building $OUT/boot.img"
mkbootimg \
	--kernel "$KERNEL" \
	--ramdisk "$RAMDISK" \
	--base "$BASE" \
	--kernel_offset 0x00008000 \
	--ramdisk_offset 0x00800000 \
	--second_offset 0x00f00000 \
	--tags_offset 0x00000100 \
	--pagesize "$PAGESIZE" \
	--cmdline "$CMDLINE" \
	--output "$OUT/boot.img"

echo "== building $OUT/boot_${BOARD}.tar.md5 for Odin"
(
	cd "$OUT"
	tar -H ustar -cf "boot_${BOARD}.tar" boot.img
	md5sum -t "boot_${BOARD}.tar" >> "boot_${BOARD}.tar"
	mv "boot_${BOARD}.tar" "boot_${BOARD}.tar.md5"
)

echo
ls -l "$OUT/boot.img" "$OUT/boot_${BOARD}.tar.md5"
cat <<-EOF

	== flashing from TWRP (no PC, no UART jig)
	1. TWRP -> Backup -> Boot, before anything else. Restoring that backup is
	   how you undo a kernel that does not boot; recovery is not touched.
	2. Copy $OUT/boot.img to the tablet, then TWRP -> Install -> Install Image
	   -> boot.img -> Boot partition.
	3. Reboot -> System, wait ~30 s, then hold Volume Up + Home + Power to warm
	   reboot back into TWRP. Do not cut the power: the log lives in DRAM and
	   only survives a warm reset.
	4. In the TWRP terminal (or adb shell), dump the RAM console:
	     dd if=/dev/mem bs=4096 skip=563968 count=256 of=/sdcard/ramoops.bin
	     strings /sdcard/ramoops.bin | tail -n 200
	   563968 is 0x89b00000 / 4096 and 256 pages is the 1 MiB region.
	   How to read the result: docs/DEBUG-TWRP.md
	5. First boot: add nosmp (or maxcpus=1) to the command line. SMP bring-up
	   is the least tested part of this port.

	== flashing with Odin (alternative)
	Power off, hold Volume Down + Home + Power for download mode, then
	Odin -> AP -> boot_${BOARD}.tar.md5, uncheck Auto Reboot, F. Reset Time.
EOF
