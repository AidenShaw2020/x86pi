#!/bin/sh
# Fetch the Circle revision build.sh is pinned to, and configure it for the
# Raspberry Pi 3 in 64-bit mode.  build.sh applies X86Pi's changes to it.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
circle="$root/build-rpi3-deps/circle"
rev=6177984e30fac5e65582d171d43f1563368a94ac
test -d "$circle/.git" || git clone https://github.com/rsta2/circle.git "$circle"
git -C "$circle" cat-file -e "$rev^{commit}" 2>/dev/null || git -C "$circle" fetch origin
git -C "$circle" checkout --quiet "$rev"
cp "$root/platform/circle/Config.mk.example" "$circle/Config.mk"
echo "Circle $rev ready in $circle"
