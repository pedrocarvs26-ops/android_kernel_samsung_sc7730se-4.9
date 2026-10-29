#!/bin/bash
# bpftest: ARM (tablet) + x86_64 (qemu) builds, no libc.
cd "$(dirname "$0")" || exit 1
TC=/home/azureuser/los17/prebuilts/gcc/linux-x86/arm/arm-linux-androideabi-4.9/bin/arm-linux-androideabi-
COMMON="-O2 -static -nostdlib -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -fomit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Wl,-e,_start"
${TC}gcc -marm -march=armv7-a -mfloat-abi=soft $COMMON -I/tmp/k49-hdr/include \
	-o bpftest bpftest.c -lgcc || exit 1
gcc -fno-stack-protector -fno-pie -no-pie -fcf-protection=none $COMMON \
	-I/tmp/k49-hdr-x86/include -o bpftest-x86 bpftest.c -lgcc || exit 1
ls -la bpftest bpftest-x86
