#!/usr/bin/env bash
#
# Cross build the SC7730SE (Samsung SM-T560 / SM-T561) 4.9 kernel.
#
# Usage:
#   ./port/build.sh                 # SM-T560 (gtelwifi)
#   BOARD=gtel3g ./port/build.sh    # SM-T561
#   JOBS=8 ./port/build.sh
#
# Environment:
#   BOARD          gtelwifi (default) or gtel3g
#   CROSS_COMPILE  toolchain prefix, default arm-linux-gnueabihf-
#   JOBS           parallel jobs, default nproc
#   OUT            output directory, default out
#
set -euo pipefail

BOARD="${BOARD:-gtelwifi}"
JOBS="${JOBS:-$(nproc)}"
OUT="${OUT:-out}"

export ARCH=arm
export CROSS_COMPILE="${CROSS_COMPILE:-arm-linux-gnueabihf-}"

cd "$(dirname "$0")/.."

case "$BOARD" in
gtelwifi | gtel3g) ;;
*)
	echo "error: BOARD must be gtelwifi (SM-T560) or gtel3g (SM-T561)" >&2
	exit 1
	;;
esac

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
	cat >&2 <<-EOF
		error: ${CROSS_COMPILE}gcc not found.

		On Ubuntu 20.04 (recommended, GCC 9):
		    sudo apt-get install -y gcc-arm-linux-gnueabihf

		On a newer distro, install a GCC 8/9 cross toolchain instead of the
		system one and point CROSS_COMPILE at it, for example:
		    CROSS_COMPILE=/opt/gcc-arm-9/bin/arm-none-linux-gnueabihf- ./port/build.sh
	EOF
	exit 1
fi

GCC_VERSION="$("${CROSS_COMPILE}gcc" -dumpfullversion -dumpversion 2>/dev/null | head -n 1)"
GCC_MAJOR="${GCC_VERSION%%.*}"

echo "== toolchain: $("${CROSS_COMPILE}gcc" --version | head -n 1)"

if [ "${GCC_MAJOR:-0}" -ge 10 ] 2>/dev/null; then
	cat >&2 <<-EOF

		warning: GCC ${GCC_VERSION} is newer than what Linux 4.9 expects.
		         GCC 10 defaults to -fno-common, which breaks this kernel with
		         "multiple definition" link errors. GCC 8 or 9 is recommended.
		         Trying anyway with -fcommon; expect a lot of warnings.

EOF
	EXTRA_CFLAGS="-fcommon -Wno-error"
else
	EXTRA_CFLAGS=""
fi

if command -v ccache >/dev/null 2>&1; then
	export CROSS_COMPILE="ccache ${CROSS_COMPILE}"
	echo "== ccache enabled"
fi

echo "== configuring (sc7730se_defconfig)"
make -j"$JOBS" sc7730se_defconfig

echo "== building zImage and DTBs with $JOBS jobs"
if [ -n "$EXTRA_CFLAGS" ]; then
	make -j"$JOBS" KCFLAGS="$EXTRA_CFLAGS" zImage dtbs
else
	make -j"$JOBS" zImage dtbs
fi

mkdir -p "$OUT"
cp arch/arm/boot/zImage "$OUT/zImage"
cp arch/arm/boot/dts/sc7730se-gtelwifi.dtb "$OUT/"
cp arch/arm/boot/dts/sc7730se-gtel3g.dtb "$OUT/"

# The stock Spreadtrum bootloader does not pass a device tree, so the DTB is
# appended to the zImage (CONFIG_ARM_APPENDED_DTB).
cat "$OUT/zImage" "$OUT/sc7730se-${BOARD}.dtb" > "$OUT/zImage-dtb"

echo
echo "== done"
ls -l "$OUT"
echo
echo "Next: build a flashable image with"
echo "    RAMDISK=<stock ramdisk> ./port/mkboot.sh"
