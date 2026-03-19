#!/bin/bash
# fetch_android.sh — Download Android system images for RexPlayer
# Usage: ./scripts/fetch_android.sh [output_dir]
#
# Downloads:
#   - ARM64 kernel (Image) from Alpine Linux (compatible with Android)
#   - Creates empty userdata.img and cache.img
#
# For a full Android system, you need to provide:
#   - system.img from AOSP GSI or Cuttlefish build
#   - vendor.img from your AOSP build
#
# Quick start with GSI:
#   1. Go to https://developer.android.com/topic/generic-system-image/releases
#   2. Download "arm64" GSI for your target Android version
#   3. Extract system.img
#   4. Run: ./build/rexplayer --kernel Image --system-image system.img --ram 4096

set -e

OUT_DIR="${1:-$HOME/.rexplayer/images}"
mkdir -p "$OUT_DIR"

echo "=== RexPlayer Android Image Setup ==="
echo "Output directory: $OUT_DIR"
echo ""

# 1. Download ARM64 kernel if not present
KERNEL="$OUT_DIR/vmlinuz-virt"
if [ ! -f "$KERNEL" ]; then
    echo "Downloading ARM64 Linux kernel..."
    curl -L -o "$KERNEL" \
        "https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/netboot/vmlinuz-virt"
    echo "  -> $KERNEL ($(du -h "$KERNEL" | cut -f1))"
else
    echo "Kernel already exists: $KERNEL"
fi

# 2. Download initramfs if not present
INITRD="$OUT_DIR/initramfs-virt"
if [ ! -f "$INITRD" ]; then
    echo "Downloading ARM64 initramfs..."
    curl -L -o "$INITRD" \
        "https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/netboot/initramfs-virt"
    echo "  -> $INITRD ($(du -h "$INITRD" | cut -f1))"
else
    echo "Initramfs already exists: $INITRD"
fi

# 3. Create empty userdata.img if not present
USERDATA="$OUT_DIR/userdata.img"
if [ ! -f "$USERDATA" ]; then
    echo "Creating empty userdata.img (2GB)..."
    qemu-img create -f qcow2 "$USERDATA" 2G 2>/dev/null
    echo "  -> $USERDATA"
else
    echo "Userdata already exists: $USERDATA"
fi

# 4. Create empty cache.img if not present
CACHE="$OUT_DIR/cache.img"
if [ ! -f "$CACHE" ]; then
    echo "Creating empty cache.img (512MB)..."
    qemu-img create -f qcow2 "$CACHE" 512M 2>/dev/null
    echo "  -> $CACHE"
else
    echo "Cache already exists: $CACHE"
fi

echo ""
echo "=== Setup Complete ==="
echo ""
echo "To boot Linux (test):"
echo "  ./build/rexplayer --kernel $KERNEL --initrd $INITRD --ram 1024 --cpus 2"
echo ""
echo "To boot Android (requires system.img):"
echo "  1. Download GSI from: https://developer.android.com/topic/generic-system-image/releases"
echo "  2. Extract system.img"
echo "  3. Run:"
echo "     ./build/rexplayer \\"
echo "       --kernel $KERNEL \\"
echo "       --system-image /path/to/system.img \\"
echo "       --userdata $USERDATA \\"
echo "       --cache $CACHE \\"
echo "       --ram 4096 --cpus 4"
echo ""
echo "ADB: adb connect localhost:5555"
echo "Frida: frida-ps -H localhost:27042"
