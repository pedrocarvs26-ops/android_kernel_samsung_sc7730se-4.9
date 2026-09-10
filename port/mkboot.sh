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
CMDLINE="${CMDLINE:-console=ttyS1,115200n8 earlycon=sprd_serial,0x70100000 no_console_suspend androidboot.hardware=sc8830}"

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

	== flashing
	1. Back up the stock boot partition first. There is no download-mode
	   recovery for a bad kernel other than reflashing stock firmware.
	2. Power off, hold Volume Down + Home + Power to enter download mode.
	3. Odin -> AP -> boot_${BOARD}.tar.md5, uncheck Auto Reboot, F. Reset Time.
	4. Attach the UART jig (619 kOhm) and open the serial port at 115200 8N1
	   before booting, otherwise you will see nothing at all.
	5. First boot: add nosmp (or maxcpus=1) to the command line. SMP bring-up
	   is the least tested part of this port.
EOF
