#!/bin/bash
# fetch_android.sh — Automatically download and set up Android images for RexPlayer
# Usage: ./scripts/fetch_android.sh [output_dir]

set -e

OUT_DIR="${1:-$HOME/.rexplayer/images}"
mkdir -p "$OUT_DIR"

# Android 16 GSI ARM64 (AOSP, no GMS — smaller and more compatible)
GSI_URL="https://dl.google.com/developers/android/baklava/images/gsi/aosp_arm64-exp-CP11.251209.009.A1-14840729-59a822d9.zip"
GSI_ZIP="$OUT_DIR/gsi_arm64.zip"
SYSTEM_IMG="$OUT_DIR/system.img"
USERDATA_IMG="$OUT_DIR/userdata.img"
CACHE_IMG="$OUT_DIR/cache.img"
KERNEL="$OUT_DIR/vmlinuz-virt"
INITRD="$OUT_DIR/initramfs-virt"

echo "=== RexPlayer Android Image Setup ==="
echo "Directory: $OUT_DIR"
echo ""

# 1. Download kernel
if [ ! -f "$KERNEL" ]; then
    echo "[1/5] Downloading ARM64 Linux kernel..."
    curl -L --progress-bar -o "$KERNEL" \
        "https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/netboot/vmlinuz-virt"
    echo "  Done ($(du -h "$KERNEL" | cut -f1))"
else
    echo "[1/5] Kernel: already exists"
fi

# 2. Download initramfs
if [ ! -f "$INITRD" ]; then
    echo "[2/5] Downloading ARM64 initramfs..."
    curl -L --progress-bar -o "$INITRD" \
        "https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/aarch64/netboot/initramfs-virt"
    echo "  Done ($(du -h "$INITRD" | cut -f1))"
else
    echo "[2/5] Initramfs: already exists"
fi

# 3. Download GSI system image
if [ ! -f "$SYSTEM_IMG" ]; then
    echo "[3/5] Downloading Android GSI ARM64 (~1.5GB)..."
    curl -L --progress-bar -o "$GSI_ZIP" "$GSI_URL"
    echo "  Extracting system.img..."
    # GSI zip contains system.img (or sometimes *.raw.img)
    cd "$OUT_DIR"
    unzip -o "$GSI_ZIP" "*.img" 2>/dev/null || unzip -o "$GSI_ZIP" 2>/dev/null
    # Find and rename the system image
    if [ ! -f "$SYSTEM_IMG" ]; then
        # Look for any .img file that could be the system image
        SYS_CANDIDATE=$(find "$OUT_DIR" -maxdepth 1 -name "*.img" -size +500M | head -1)
        if [ -n "$SYS_CANDIDATE" ] && [ "$SYS_CANDIDATE" != "$SYSTEM_IMG" ]; then
            mv "$SYS_CANDIDATE" "$SYSTEM_IMG"
        fi
    fi
    rm -f "$GSI_ZIP"
    if [ -f "$SYSTEM_IMG" ]; then
        echo "  Done ($(du -h "$SYSTEM_IMG" | cut -f1))"
    else
        echo "  WARNING: Could not extract system.img from GSI zip"
        echo "  Please download manually from:"
        echo "  https://developer.android.com/topic/generic-system-image/releases"
    fi
else
    echo "[3/5] System image: already exists ($(du -h "$SYSTEM_IMG" | cut -f1))"
fi

# 4. Create userdata image
if [ ! -f "$USERDATA_IMG" ]; then
    echo "[4/5] Creating userdata.img (2GB)..."
    if command -v qemu-img &>/dev/null; then
        qemu-img create -f qcow2 "$USERDATA_IMG" 2G 2>/dev/null
    else
        # Fallback: create raw sparse file
        dd if=/dev/zero of="$USERDATA_IMG" bs=1 count=0 seek=2G 2>/dev/null
    fi
    echo "  Done"
else
    echo "[4/5] Userdata: already exists"
fi

# 5. Create cache image
if [ ! -f "$CACHE_IMG" ]; then
    echo "[5/5] Creating cache.img (512MB)..."
    if command -v qemu-img &>/dev/null; then
        qemu-img create -f qcow2 "$CACHE_IMG" 512M 2>/dev/null
    else
        dd if=/dev/zero of="$CACHE_IMG" bs=1 count=0 seek=512M 2>/dev/null
    fi
    echo "  Done"
else
    echo "[5/5] Cache: already exists"
fi

echo ""
echo "=== All images ready ==="
echo ""
echo "To launch RexPlayer:"
echo "  ./build/rexplayer \\"
echo "    --kernel $KERNEL \\"
echo "    --system-image $SYSTEM_IMG \\"
echo "    --userdata $USERDATA_IMG \\"
echo "    --cache $CACHE_IMG \\"
echo "    --ram 4096 --cpus 4"
echo ""
echo "After Android boots:"
echo "  adb connect localhost:5555"
echo "  frida-ps -H localhost:27042"

# Write a marker file so RexPlayer knows images are ready
cat > "$OUT_DIR/images.json" << EOJSON
{
    "kernel": "$KERNEL",
    "initrd": "$INITRD",
    "system": "$SYSTEM_IMG",
    "userdata": "$USERDATA_IMG",
    "cache": "$CACHE_IMG",
    "version": "android-16-gsi-arm64",
    "created": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOJSON
