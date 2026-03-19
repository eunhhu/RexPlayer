#!/bin/bash
# gen_android_dtb.sh — Generate DTB with Android firmware/android partition names
# Usage: ./scripts/gen_android_dtb.sh [output.dtb]
#
# Patches QEMU virt machine DTB to add Android-required firmware/android node
# with fstab entries pointing virtio-mmio devices to named partitions.

set -e

OUTPUT="${1:-/tmp/rex-android.dtb}"
DTS="/tmp/rex-android-overlay.dts"

# Step 1: Dump base DTB from QEMU
qemu-system-aarch64 \
  -machine virt,gic-version=3,dumpdtb=/tmp/rex-base.dtb \
  -cpu host -accel hvf \
  -smp 4 -m 4096 \
  2>/dev/null

# Step 2: Decompile to DTS
dtc -I dtb -O dts -o /tmp/rex-base.dts /tmp/rex-base.dtb 2>/dev/null

# Step 3: Inject firmware/android node with fstab
# virtio-mmio devices are at a003e00, a003c00, a003a00, a003800 (last 4)
# We use them in reverse order for system, vendor, userdata, metadata
# The -global virtio-mmio.force-transports=0 ties drives to mmio slots
cat >> /tmp/rex-base.dts << 'FWEOF'

	firmware {
		android {
			compatible = "android,firmware";
			fstab {
				compatible = "android,fstab";
				system {
					compatible = "android,system";
					dev = "/dev/block/vda";
					type = "ext4";
					mnt_flags = "ro,barrier=1";
					fsmgr_flags = "wait,first_stage_mount";
				};
				vendor {
					compatible = "android,vendor";
					dev = "/dev/block/vdb";
					type = "ext4";
					mnt_flags = "ro,barrier=1";
					fsmgr_flags = "wait,first_stage_mount";
				};
				metadata {
					compatible = "android,metadata";
					dev = "/dev/block/vdd";
					type = "ext4";
					mnt_flags = "noatime,nosuid,nodev";
					fsmgr_flags = "wait,formattable,first_stage_mount";
				};
			};
		};
	};
FWEOF

# Step 4: Close the root node properly — the DTS already has closing brace
# We need to insert before the final };
# Actually the append above goes after the final }; which is wrong.
# Fix: insert before the last line

# Redo: use sed to insert before the final };
# Remove the appended text first
head -n -17 /tmp/rex-base.dts > /tmp/rex-patched.dts

cat >> /tmp/rex-patched.dts << 'FWEOF'

	firmware {
		android {
			compatible = "android,firmware";
			fstab {
				compatible = "android,fstab";
				system {
					compatible = "android,system";
					dev = "/dev/block/vda";
					type = "ext4";
					mnt_flags = "ro,barrier=1";
					fsmgr_flags = "wait,first_stage_mount";
				};
				vendor {
					compatible = "android,vendor";
					dev = "/dev/block/vdb";
					type = "ext4";
					mnt_flags = "ro,barrier=1";
					fsmgr_flags = "wait,first_stage_mount";
				};
				metadata {
					compatible = "android,metadata";
					dev = "/dev/block/vdd";
					type = "ext4";
					mnt_flags = "noatime,nosuid,nodev";
					fsmgr_flags = "wait,formattable,first_stage_mount";
				};
			};
		};
	};
};
FWEOF

# Step 5: Compile back to DTB
dtc -I dts -O dtb -o "$OUTPUT" /tmp/rex-patched.dts 2>/dev/null

echo "Generated: $OUTPUT ($(du -h "$OUTPUT" | cut -f1))"
echo "Partitions: vda=system, vdb=vendor, vdc=userdata, vdd=metadata"
