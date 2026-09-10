#!/usr/bin/env python3
"""Repack a stock Samsung/Spreadtrum boot.img with a freshly built kernel.

Why not plain mkbootimg: the stock SM-T560/SM-T561 boot image is a four part
image - header page, kernel, ramdisk and a ~650 KiB vendor device tree area
whose size lives in the header field right after page_size - and its load
addresses are stored relative to zero (kernel_addr 0x00008000, ramdisk_addr
0x01000000, tags_addr 0x00000100), not relative to 0x80000000. Rebuilding it
with hand written offsets silently drops the dt area and changes the load
addresses, which the bootloader may or may not survive.

This script instead copies the stock header page verbatim and swaps only the
kernel, so every vendor specific field is preserved bit for bit. Only
kernel_size, the command line and the SHA1 id are rewritten. It needs nothing
but python3.

    ./port/repackboot.py --stock stock_boot.img --kernel out/zImage-dtb \\
                         --out out/boot.img

Useful options:
    --append "..."   extra words for the boot image command line
    --nosmp          also rewrite /chosen/bootargs inside the appended DTB to
                     boot single core (first boot recommendation)
    --ramdisk FILE   use a different ramdisk instead of the stock one

The stock image can be pulled from the device with TWRP (Backup -> Boot, the
backup file is the raw partition) or with `dd if=/dev/block/... of=/sdcard`.
"""

import argparse
import hashlib
import os
import struct
import sys

ARM_ZIMAGE_MAGIC = b'\x18\x28\x6f\x01'
FDT_MAGIC = b'\xd0\x0d\xfe\xed'

# /chosen/bootargs as written in the board DTS, and the single core variant.
# The replacement is padded with NULs to the exact same length so the FDT
# property length stays valid; the kernel reads it as a C string.
DTS_BOOTARGS = (b'console=ttyS1,115200n8 earlycon=sprd_serial,0x70100000 '
                b'no_console_suspend rw')
DTS_BOOTARGS_NOSMP = (b'console=ttyS1,115200n8 earlycon=sprd_serial,0x70100000 '
                      b'nosmp maxcpus=1 rw')


def boot_id(kernel, ramdisk, second, dt):
    """mkbootimg id: sha1 over each blob followed by its little endian size."""
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        h.update(blob)
        h.update(struct.pack('<I', len(blob)))
    if dt:
        h.update(dt)
        h.update(struct.pack('<I', len(dt)))
    return h.digest()


def split(stock):
    if stock[:8] != b'ANDROID!':
        sys.exit('error: %r is not an Android boot image' % args.stock)

    def u32(off):
        return struct.unpack_from('<I', stock, off)[0]

    hdr = {
        'kernel_size': u32(8),
        'ramdisk_size': u32(16),
        'second_size': u32(24),
        'page_size': u32(36),
        'dt_size': u32(40),
        'cmdline': stock[64:64 + 512].rstrip(b'\x00').decode('latin1'),
        'name': stock[48:64].rstrip(b'\x00').decode('latin1'),
    }
    # On boot image header v1/v2 this field is header_version, not dt_size.
    if hdr['dt_size'] < 64:
        hdr['dt_size'] = 0

    ps = hdr['page_size']
    if ps not in (2048, 4096, 8192, 16384):
        sys.exit('error: implausible page size %d' % ps)

    def take(off, size):
        blob = stock[off:off + size]
        if len(blob) != size:
            sys.exit('error: truncated boot image')
        return blob, off + ((size + ps - 1) // ps) * ps

    off = ps
    kernel, off = take(off, hdr['kernel_size'])
    ramdisk, off = take(off, hdr['ramdisk_size'])
    second, off = take(off, hdr['second_size'])
    dt, off = take(off, hdr['dt_size'])
    return hdr, kernel, ramdisk, second, dt


def patch_dtb_bootargs(kernel):
    at = kernel.find(DTS_BOOTARGS)
    if at < 0:
        sys.exit('error: --nosmp needs the stock /chosen/bootargs string from '
                 'the board DTS; not found in this kernel')
    if kernel.find(DTS_BOOTARGS, at + 1) >= 0:
        sys.exit('error: bootargs string is not unique, refusing to patch')
    pad = b'\x00' * (len(DTS_BOOTARGS) - len(DTS_BOOTARGS_NOSMP))
    return kernel[:at] + DTS_BOOTARGS_NOSMP + pad + kernel[at + len(DTS_BOOTARGS):]


parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--stock', required=True, help='stock boot.img to use as the template')
parser.add_argument('--kernel', default='out/zImage-dtb', help='new kernel (default out/zImage-dtb)')
parser.add_argument('--out', default='out/boot.img', help='output image (default out/boot.img)')
parser.add_argument('--ramdisk', help='replace the stock ramdisk')
parser.add_argument('--append', default='', help='extra command line words')
parser.add_argument('--nosmp', action='store_true', help='patch the appended DTB to boot single core')
args = parser.parse_args()

stock = open(args.stock, 'rb').read()
hdr, stock_kernel, ramdisk, second, dt = split(stock)
ps = hdr['page_size']

kernel = open(args.kernel, 'rb').read()
if kernel[0x24:0x28] != ARM_ZIMAGE_MAGIC:
    print('warning: %s does not look like an ARM zImage' % args.kernel)
idx = kernel.rfind(FDT_MAGIC)
if idx <= 0:
    print('warning: no appended DTB found; CONFIG_ARM_APPENDED_DTB needs one')
else:
    dtb_len = struct.unpack('>I', kernel[idx + 4:idx + 8])[0]
    if idx + dtb_len != len(kernel):
        print('warning: appended DTB does not end at EOF (%d != %d)'
              % (idx + dtb_len, len(kernel)))
    # Guard against repacking a kernel built before the RAM console existed.
    # Without the ramoops node nothing is ever written to the region, so every
    # read of it comes back as zeros - which looks exactly like "the kernel
    # never started" and sends you debugging the wrong thing. Cheap to check
    # here, expensive to figure out on the device.
    if b'ramoops' not in kernel[idx:]:
        print()
        print("WARNING: this kernel's appended DTB has no ramoops node, so it")
        print('         sets up no RAM console and the TWRP debug path in')
        print('         docs/DEBUG-TWRP.md cannot recover anything at all.')
        print('         This usually means a stale artifact: check the commit')
        print('         in out/BUILD-INFO.txt and rebuild from a tree that has')
        print('         the reserved-memory ramoops node.')
        print()

if args.ramdisk:
    ramdisk = open(args.ramdisk, 'rb').read()
if args.nosmp:
    kernel = patch_dtb_bootargs(kernel)

cmdline = hdr['cmdline']
extra = args.append + (' nosmp maxcpus=1' if args.nosmp else '')
if extra.strip():
    cmdline = (cmdline + ' ' + extra.strip()).strip()
if len(cmdline) >= 512:
    sys.exit('error: command line too long (%d bytes)' % len(cmdline))

out = bytearray(stock[:ps])                       # verbatim vendor header page
struct.pack_into('<I', out, 8, len(kernel))
struct.pack_into('<I', out, 16, len(ramdisk))
struct.pack_into('<I', out, 24, len(second))
if hdr['dt_size']:
    struct.pack_into('<I', out, 40, len(dt))
out[64:64 + 512] = cmdline.encode() + b'\x00' * (512 - len(cmdline))
out[576:576 + 32] = boot_id(kernel, ramdisk, second, dt) + b'\x00' * 12

for blob in (kernel, ramdisk, second, dt):
    if blob:
        out += blob
        out += b'\x00' * ((-len(blob)) % ps)

if os.path.dirname(args.out):
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
open(args.out, 'wb').write(out)

print('template   : %s (%s, page size %d)' % (args.stock, hdr['name'] or '?', ps))
print('kernel     : %s, %d bytes (was %d)' % (args.kernel, len(kernel), hdr['kernel_size']))
print('ramdisk    : %d bytes%s' % (len(ramdisk), ' (replaced)' if args.ramdisk else ' (stock, untouched)'))
if dt:
    print('dt area    : %d bytes (stock, untouched)' % len(dt))
print('cmdline    : %s' % cmdline)
print('written    : %s, %d bytes (%.2f MiB)' % (args.out, len(out), len(out) / 1048576.0))
if len(out) > len(stock):
    print('warning: the new image is larger than the stock one; make sure it '
          'still fits the boot partition')
print()
print('Flash it from TWRP: Install -> Install Image -> %s -> Boot.'
      % os.path.basename(args.out))
print('After the boot attempt, warm reboot into TWRP (Volume Up + Home + Power)')
print('and dump the RAM console, see docs/DEBUG-TWRP.md:')
print('  cp /sdcard/memdump /tmp/memdump && chmod +x /tmp/memdump')
print('  /tmp/memdump')
print('  tail -n 200 /sdcard/ramoops.bin.txt')
print()
print('dd cannot do that read: every DRAM address on this SoC is above 2 GiB,')
print('which does not fit a 32 bit off_t, so dd stops with "Bad address".')
print('memdump ships in the same CI artifact as the kernel.')
