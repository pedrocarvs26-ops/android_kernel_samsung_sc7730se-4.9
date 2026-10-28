#!/usr/bin/env bash
#
# Cross build the SC7730SE (Samsung SM-T560 / SM-T561) 4.9.337 kernel.
#
# Usage:
#   ./port/build.sh                 # SM-T560 (gtelwifi)
#   BOARD=gtel3g ./port/build.sh    # SM-T561 (gtel3g)
#   JOBS=8 ./port/build.sh
#
set -euo pipefail

BOARD="${BOARD:-gtelwifi}"
JOBS="${JOBS:-$(nproc)}"
OUT="${OUT:-out}"
BUILD_DIR="${BUILD_DIR:-/tmp/k49-build}"

export ARCH=arm

# Auto-detect local Android GCC 4.9 toolchain on Devspace if CROSS_COMPILE is not set
if [ -z "${CROSS_COMPILE:-}" ]; then
	if [ -x "/home/azureuser/los17/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-gcc" ]; then
		export CROSS_COMPILE="/home/azureuser/los17/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-"
	else
		export CROSS_COMPILE="arm-linux-gnueabihf-"
	fi
fi

cd "$(dirname "$0")/.."

case "$BOARD" in
gtelwifi | gtel3g) ;;
*)
	echo "error: BOARD must be gtelwifi (SM-T560) or gtel3g (SM-T561)" >&2
	exit 1
	;;
esac

if ! command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
	echo "error: ${CROSS_COMPILE}gcc not found." >&2
	exit 1
fi

# Prefer ld.bfd when using Android prebuilt toolchains (ld.gold rejects OVERLAY in vmlinux.lds)
LD_ARGS=()
if command -v "${CROSS_COMPILE}ld.bfd" >/dev/null 2>&1; then
	LD_ARGS=("LD=${CROSS_COMPILE}ld.bfd")
fi

GCC_VERSION="$("${CROSS_COMPILE}gcc" -dumpfullversion -dumpversion 2>/dev/null | head -n 1)"
GCC_MAJOR="${GCC_VERSION%%.*}"

echo "== toolchain: $("${CROSS_COMPILE}gcc" --version | head -n 1)"

if [ "${GCC_MAJOR:-0}" -ge 10 ] 2>/dev/null; then
	EXTRA_CFLAGS="-fcommon -Wno-error"
else
	EXTRA_CFLAGS=""
fi

mkdir -p "$BUILD_DIR"

echo "== configuring (sc7730se_defconfig) in $BUILD_DIR"
make O="$BUILD_DIR" -j"$JOBS" "${LD_ARGS[@]}" sc7730se_defconfig

echo "== building zImage and DTBs with $JOBS jobs"
if [ -n "$EXTRA_CFLAGS" ]; then
	make O="$BUILD_DIR" -j"$JOBS" "${LD_ARGS[@]}" KCFLAGS="$EXTRA_CFLAGS" zImage dtbs
else
	make O="$BUILD_DIR" -j"$JOBS" "${LD_ARGS[@]}" zImage dtbs
fi

mkdir -p "$OUT"
cp "$BUILD_DIR/arch/arm/boot/zImage" "$OUT/zImage"
cp "$BUILD_DIR/arch/arm/boot/dts/sc7730se-gtelwifi.dtb" "$OUT/"
cp "$BUILD_DIR/arch/arm/boot/dts/sc7730se-gtel3g.dtb" "$OUT/"

cat "$OUT/zImage" "$OUT/sc7730se-${BOARD}.dtb" > "$OUT/zImage-dtb"
cat "$OUT/zImage" "$OUT/sc7730se-gtelwifi.dtb" > "$OUT/zImage-dtb-gtelwifi"
cat "$OUT/zImage" "$OUT/sc7730se-gtel3g.dtb" > "$OUT/zImage-dtb-gtel3g"

echo
echo "== done"
ls -lh "$OUT"
