# Android fstab for RexPlayer x86_64
# <dev>                     <mnt_point>   <type>  <mnt_flags and options>                           <fs_mgr_flags>
/dev/vda                    /system       ext4    ro,barrier=1                                       wait
/dev/vdb                    /vendor       ext4    ro,barrier=1                                       wait
/dev/vdc                    /data         ext4    noatime,nosuid,nodev,barrier=1,data=ordered        wait,check,formattable,quota,reservedsize=128M
/dev/vdd                    /cache        ext4    noatime,nosuid,nodev                               wait,check
