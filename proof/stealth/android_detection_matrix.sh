#!/system/bin/sh
# RexPlayer owned-lab detection matrix. PASS means this vector did not reveal
# the Android container to this shell-level detector. FAIL includes evidence.
# SKIP means the detector could not inspect the vector; it is never a clean pass.

pass() { printf 'PASS\t%s\t%s\n' "$1" "$2"; }
fail() { printf 'FAIL\t%s\t%s\n' "$1" "$2"; }
skip() { printf 'SKIP\t%s\t%s\n' "$1" "$2"; }
info() { printf 'INFO\t%s\t%s\n' "$1" "$2"; }

prop_eq() {
  id="$1"; key="$2"; expected="$3"
  value="$(getprop "$key")"
  if [ "$value" = "$expected" ]; then pass "$id" "$key=$value"; else fail "$id" "$key=$value expected=$expected"; fi
}

prop_clean() {
  id="$1"; key="$2"
  value="$(getprop "$key")"
  if [ -z "$value" ]; then skip "$id" "$key unavailable"; return; fi
  lower="$(printf '%s' "$value" | tr '[:upper:]' '[:lower:]')"
  case "$lower" in
    *redroid*|*waydroid*|*qemu*|*ranchu*|*goldfish*|*emulator*|*generic*|*sdk_gphone*|*test-keys*)
      fail "$id" "$key=$value" ;;
    *) pass "$id" "$key=$value" ;;
  esac
}

file_clean() {
  id="$1"; path="$2"
  if [ ! -r "$path" ]; then skip "$id" "$path unreadable/absent"; return; fi
  matches="$(grep -ioE 'redroid|waydroid|qemu|ranchu|goldfish|docker|overlay|lxc|libpod|kubepods' "$path" 2>/dev/null)"
  grep_rc=$?
  case "$grep_rc" in
    0) markers="$(printf '%s\n' "$matches" | sort -u | tr '\n' ',')"; fail "$id" "markers=$markers" ;;
    1) pass "$id" "$path clean" ;;
    *) skip "$id" "$path read/search error" ;;
  esac
}

printf 'VERDICT\tCHECK\tEVIDENCE\n'
qemu_value="$(getprop ro.kernel.qemu)"
case "$qemu_value" in
  '') skip qemu_flag 'ro.kernel.qemu unavailable' ;;
  0) pass qemu_flag 'ro.kernel.qemu=0' ;;
  *) fail qemu_flag "ro.kernel.qemu=$qemu_value expected=0" ;;
esac
prop_clean hardware ro.hardware
prop_clean boot_hardware ro.boot.hardware
prop_clean product_model ro.product.model
prop_clean product_device ro.product.device
prop_clean product_name ro.product.name
prop_clean manufacturer ro.product.manufacturer
prop_clean brand ro.product.brand
prop_clean fingerprint ro.build.fingerprint
prop_eq build_type ro.build.type user
prop_eq build_tags ro.build.tags release-keys
prop_eq ro_debuggable ro.debuggable 0
prop_eq ro_secure ro.secure 1
prop_eq adb_secure ro.adb.secure 1
prop_eq verified_boot ro.boot.verifiedbootstate green
prop_eq flash_locked ro.boot.flash.locked 1
prop_eq vbmeta_state ro.boot.vbmeta.device_state locked
prop_eq selinux_enforcing ro.boot.selinux enforcing

file_clean proc_cmdline /proc/cmdline
file_clean proc_mountinfo /proc/self/mountinfo
file_clean proc_cgroup /proc/self/cgroup
file_clean proc_cpuinfo /proc/cpuinfo

if [ -e /dev/qemu_pipe ] || [ -e /dev/socket/qemud ]; then fail qemu_devices '/dev/qemu_pipe or /dev/socket/qemud exists'; else pass qemu_devices 'absent'; fi
if [ -d /sys/devices/platform/goldfish ] || [ -d /sys/devices/virtual/misc/goldfish_pipe ]; then fail goldfish_sysfs 'goldfish sysfs exists'; else pass goldfish_sysfs 'absent'; fi

su_evidence=''
for p in /system/bin/su /system/xbin/su /sbin/su /vendor/bin/su /debug_ramdisk/su; do [ -e "$p" ] && su_evidence="$su_evidence$p;"; done
command -v su >/dev/null 2>&1 && su_evidence="$su_evidence$(command -v su);"
if [ -n "$su_evidence" ]; then fail su_binary "$su_evidence"; else pass su_binary 'absent'; fi

if [ ! -x /data ]; then
  skip root_artifacts '/data not searchable'
else
  root_evidence=''
  for p in /data/adb /data/adb/ksu /data/adb/magisk /sbin/.magisk /debug_ramdisk/.magisk; do [ -e "$p" ] && root_evidence="$root_evidence$p;"; done
  if [ -n "$root_evidence" ]; then fail root_artifacts "$root_evidence"; else pass root_artifacts 'absent'; fi
fi

if ps_text="$(ps -A 2>/dev/null)"; then
  frida_matches="$(printf '%s\n' "$ps_text" | grep -iE 'frida|gum-js|gadget')"
  grep_rc=$?
  case "$grep_rc" in
    0) frida_proc="$(printf '%s\n' "$frida_matches" | head -n 3 | tr '\n' ';')"; fail frida_process "$frida_proc" ;;
    1) pass frida_process 'no name match' ;;
    *) skip frida_process 'ps output search error' ;;
  esac
else
  skip frida_process 'ps unavailable'
fi

frida_ports=''
readable_tables=0
failed_tables=0
for f in /proc/net/tcp /proc/net/tcp6; do
  if [ ! -r "$f" ]; then failed_tables=$((failed_tables + 1)); continue; fi
  grep -qiE ':69A2|:69A3' "$f" 2>/dev/null
  grep_rc=$?
  case "$grep_rc" in
    0) readable_tables=$((readable_tables + 1)); frida_ports="$frida_ports$f;" ;;
    1) readable_tables=$((readable_tables + 1)) ;;
    *) failed_tables=$((failed_tables + 1)) ;;
  esac
done
if [ -n "$frida_ports" ]; then
  fail frida_ports "27042/27043 signature in $frida_ports"
elif [ "$readable_tables" -eq 0 ] || [ "$failed_tables" -gt 0 ]; then
  skip frida_ports '/proc/net/tcp* unreadable/absent'
else
  pass frida_ports '27042/27043 absent'
fi

map_hit=''
readable_maps=0
failed_maps=0
for f in /proc/[0-9]*/maps; do
  if [ ! -r "$f" ]; then failed_maps=$((failed_maps + 1)); continue; fi
  hits="$(grep -ioE 'frida|gadget|gum-js' "$f" 2>/dev/null)"
  grep_rc=$?
  case "$grep_rc" in
    0) readable_maps=$((readable_maps + 1)); hit="$(printf '%s\n' "$hits" | head -n 1)"; map_hit="$f:$hit"; break ;;
    1) readable_maps=$((readable_maps + 1)) ;;
    *) failed_maps=$((failed_maps + 1)) ;;
  esac
done
if [ -n "$map_hit" ]; then
  fail frida_maps "$map_hit"
elif [ "$readable_maps" -eq 0 ] || [ "$failed_maps" -gt 0 ]; then
  skip frida_maps '/proc/*/maps unreadable/absent'
else
  pass frida_maps 'no readable map match'
fi

usb="$(getprop persist.sys.usb.config)"
case "$usb" in *adb*) fail adb_enabled "persist.sys.usb.config=$usb";; *) pass adb_enabled "persist.sys.usb.config=$usb";; esac

renderer="$(getprop ro.hardware.egl) $(getprop ro.hardware.vulkan) $(getprop debug.hwui.renderer)"
lower_renderer="$(printf '%s' "$renderer" | tr '[:upper:]' '[:lower:]')"
case "$lower_renderer" in *swiftshader*|*emulation*|*redroid*|*angle*) fail graphics_props "$renderer";; *) pass graphics_props "$renderer";; esac

sim="$(getprop gsm.sim.state)"
operator="$(getprop gsm.operator.numeric)"
if [ -n "$sim" ] && [ "$sim" != 'UNKNOWN' ] && [ -n "$operator" ]; then pass telephony "sim=$sim operator=$operator"; else fail telephony "sim=$sim operator=$operator"; fi

serial="$(getprop ro.serialno)"
if [ -n "$serial" ] && [ "$serial" != 'unknown' ]; then pass serial_number "present length=${#serial}"; else fail serial_number 'absent/unknown'; fi

id_out="$(id)"
case "$id_out" in *'uid=0('* ) fail adb_shell_privilege "$id_out";; *) pass adb_shell_privilege "$id_out";; esac

info kernel "$(uname -a)"
info page_size "$(getconf PAGE_SIZE 2>/dev/null)"
info sdk "$(getprop ro.build.version.sdk)"
info release "$(getprop ro.build.version.release)"
info security_patch "$(getprop ro.build.version.security_patch)"
info density "$(wm density 2>/dev/null | tr '\n' ';')"
info size "$(wm size 2>/dev/null | tr '\n' ';')"
