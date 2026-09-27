#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
circle="$root/build-rpi3-deps/circle"
expected=6177984e30fac5e65582d171d43f1563368a94ac
# Dependency revision is deliberately pinned; fetch separately.
test -f "$circle/Rules.mk" || { echo "Missing Circle checkout ($expected)" >&2; exit 1; }
actual=$(git -C "$circle" rev-parse HEAD)
test "$actual" = "$expected" || { echo "Unexpected Circle revision: $actual" >&2; exit 1; }
cmp "$root/platform/circle/Config.mk.example" "$circle/Config.mk" || {
    echo "Circle Config.mk differs: install Config.mk.example before building." >&2
    exit 1
}
# FatFS fast seek, forced on before the library is built.
#
# Without it f_lseek() walks the cluster chain from the start of the file on
# every backward seek, and a gigabyte disk image makes that cost seven times
# more than reading the data it is seeking to - long enough to empty the audio
# queue, which is heard as sound dropping out while a game loads.  src/disk.c
# builds the cluster link map that depends on this; with the option off it
# compiles to nothing and the stall comes back silently.
#
# The Circle checkout is not part of this repository, so the setting cannot be
# committed with the rest of the fix and is applied here instead.
ffconf="$circle/addon/fatfs/ffconf.h"
grep -q '^#define FF_USE_FASTSEEK[[:space:]]*1' "$ffconf" || {
    sed -i 's/^#define FF_USE_FASTSEEK.*/#define FF_USE_FASTSEEK	1/' "$ffconf"
    grep -q '^#define FF_USE_FASTSEEK[[:space:]]*1' "$ffconf" || {
        echo "Could not enable FF_USE_FASTSEEK in $ffconf" >&2; exit 1; }
    make -C "$circle/addon/fatfs" clean >/dev/null
}

# The graphics processor's memory, mapped as memory rather than as a device.
#
# Circle marks everything above the ARM's memory size as Device, and the
# framebuffer lives there.  Device memory does not let the processor merge
# stores, so writing a frame goes out one transaction at a time - which is
# why this target used to draw into a staging buffer and copy.  Mapped Normal
# but non-cacheable, the renderer writes straight into the scanout buffer and
# the copy disappears: the display fell from 1090 ms of every ten seconds to
# 478.  Peripherals, from ARM_IO_BASE up, stay Device.
#
# kernel.cpp relies on this and would be slower without it, and the Circle
# checkout is not part of this repository, so it is applied here.
tt="$circle/lib/translationtable64.cpp"
mm="$circle/lib/memory64.cpp"
th="$circle/include/circle/translationtable64.h"
grep -q ATTRINDX_NORMAL_NC "$tt" || {
    sed -i 's|^#define ATTRINDX_COHERENT\t2$|&\n#define ATTRINDX_NORMAL_NC\t3|' "$th"
    sed -i 's#| 0x00 << ATTRINDX_COHERENT\*8;\t// Device-nGnRnE#| 0x00 << ATTRINDX_COHERENT*8\t// Device-nGnRnE\n\t                | 0x44ULL << ATTRINDX_NORMAL_NC*8;\t// Normal, non-cacheable#' "$mm"
    sed -i 's|^#include <circle/sysconfig.h>$|&\n#include <circle/bcm2835.h>|' "$tt"
    sed -i 's|pDesc->AttrIndx = ATTRINDX_DEVICE;|pDesc->AttrIndx = nBaseAddress < ARM_IO_BASE ? ATTRINDX_NORMAL_NC : ATTRINDX_DEVICE;|' "$tt"
    for f in "$tt" "$mm" "$th"; do
        grep -q ATTRINDX_NORMAL_NC "$f" || {
            echo "Could not map the graphics memory as Normal in $f" >&2; exit 1; }
    done
    make -C "$circle/lib" clean >/dev/null
}

# The USB floppy driver, replaced wholesale.
#
# Circle's CBI driver needs three things before a real drive is usable, and
# they are too tangled to express as substitutions - so the whole file is
# kept beside this script and copied in.  That is safe because the revision
# of the Circle checkout is pinned and checked above: if it ever moves, this
# file has to be looked at again, which is the right thing to be forced to do.
#
#   - It gives a drive four seconds to report itself ready and then leaves
#     that loop on the first refusal.  A MITSUMI USB floppy refuses for the
#     first quarter second after it is attached, so the drive was never
#     configured and the media could not be read at all.
#   - It never clears a stalled endpoint.  A CBI device reports a failed
#     command by stalling, and Reset() only sends SEND DIAGNOSTIC, so the
#     first failure poisoned the endpoint for good.
#   - It called WaitUnitReady() before every single transfer, and a failed
#     Reset() abandoned four of its five retries.
#
# The Circle checkout is not part of this repository, so this cannot be
# committed with the rest and is applied here.
# CLEAR_TT_BUFFER goes with it: a full speed device behind a high speed hub
# talks through a transaction translator, and a split transaction that stalls
# leaves a buffer in the hub that only that request frees.  Circle names it as
# a TODO in the floppy driver and has no way to send it, so the hub and the
# device gain one.  It has to name the endpoint's type as well as its number,
# which is why TEndpointType moves into usb.h - usbdevice.h needs it and
# cannot include the header it lived in.
#
# The mass storage driver goes with them.  Circle's takes only direct access
# block devices with 512 byte blocks, which is every USB stick and no CD drive
# at all: a CD answers INQUIRY as device type 5 and reads in 2048 byte blocks,
# and it is empty most of the time, which Circle's configuration sequence
# treats as a broken device rather than an open drawer.
patched=0
for f in lib/usb/usbfloppydevice.cpp include/circle/usb/usbfloppydevice.h \
         lib/usb/usbmassdevice.cpp include/circle/usb/usbmassdevice.h \
         lib/usb/usbdevicefactory.cpp \
         lib/usb/usbstandardhub.cpp include/circle/usb/usbstandardhub.h \
         lib/usb/usbdevice.cpp include/circle/usb/usbdevice.h \
         include/circle/usb/usb.h include/circle/usb/usbendpoint.h; do
    cmp -s "$root/platform/circle/patches/$f" "$circle/$f" || {
        cp "$root/platform/circle/patches/$f" "$circle/$f"
        patched=1
    }
done
test "$patched" = 0 || make -C "$circle/lib/usb" clean >/dev/null

# The other three cores.
#
# Core 1 is the sound and core 2 the display (CSynthCores in pc/kernel.cpp),
# which needs Circle's multi-core support; it is compiled out unless
# sysconfig.h defines ARM_ALLOW_MULTI_CORE.  The define changes spinlocks and
# the memory system's layout, so every library built against the header is
# rebuilt.
sc="$circle/include/circle/sysconfig.h"
grep -q '^#define ARM_ALLOW_MULTI_CORE' "$sc" || {
    # The checkout has CRLF line ends.
    sed -i 's|^//#define ARM_ALLOW_MULTI_CORE\(\r\?\)$|#define ARM_ALLOW_MULTI_CORE\1|' "$sc"
    grep -q '^#define ARM_ALLOW_MULTI_CORE' "$sc" || {
        echo "Could not enable ARM_ALLOW_MULTI_CORE in $sc" >&2; exit 1; }
    for d in lib lib/usb lib/fs lib/input lib/sound addon/SDCard addon/fatfs; do
        make -C "$circle/$d" clean >/dev/null
    done
}

# The temperature at which Circle's CPU throttle halves the clock: 80 C.
#
# Its default is 60 C, and with three cores at work the Pi 3 sits at 58 C in
# Tyrian's demo; 61 C, reached once, took the clock from 1200 to 600 MHz every
# other ten seconds.  cmdline.txt's socmaxtemp= cannot go past 78, so the
# default itself is raised.  The throttle never enforces more than the
# firmware's own maximum, which it reads from the firmware at start.
ko="$circle/lib/koptions.cpp"
grep -q 'm_nSoCMaxTemp (80)' "$ko" || {
    sed -i 's|m_nSoCMaxTemp (60)|m_nSoCMaxTemp (80)|' "$ko"
    grep -q 'm_nSoCMaxTemp (80)' "$ko" || {
        echo "Could not raise the throttle temperature in $ko" >&2; exit 1; }
}

make -C "$circle/lib" -j4 "$@"
# Named explicitly because this script patches it, and because "make -C lib"
# does not descend here - so a cleaned libusb.a would simply be missing.
make -C "$circle/lib/usb" -j4 "$@"
make -C "$circle/lib/fs" -j4 "$@"
make -C "$circle/lib/input" -j4 "$@"
make -C "$circle/lib/sound" -j4 "$@"
make -C "$circle/addon/SDCard" -j4 "$@"
make -C "$circle/addon/fatfs" -j4 "$@"
make -C "$root/platform/circle" -j4 "$@"
