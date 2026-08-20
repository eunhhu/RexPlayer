#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
DOCKER_SOCKET=/run/rexplayer-docker.sock
DOCKER_DATA_ROOT=/var/lib/rexplayer-docker
DOCKER_EXEC_ROOT=/run/rexplayer-docker
DOCKER_UNIT=rexplayer-dockerd.service
BINDER_MARKER=/run/rexplayer-binderfs-owned

if [ "$(id -u)" -ne 0 ]; then
  printf 'run as root inside the dedicated WSL lab distribution\n' >&2
  exit 77
fi

required=(docker dockerd findmnt gcc mount mountpoint python3 systemctl systemd-run)
for command_name in "${required[@]}"; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'missing prerequisite: %s\n' "$command_name" >&2
    exit 2
  fi
done

if mountpoint -q /dev/binderfs; then
  if [ -f "$BINDER_MARKER" ] &&
    [[ "$(<"$BINDER_MARKER")" =~ ^[0-9]+$ ]] &&
    [ "$(findmnt -n -o FSTYPE --target /dev/binderfs)" = binder ] &&
    [ "$(findmnt -n -o ID --target /dev/binderfs)" = "$(<"$BINDER_MARKER")" ]; then
    umount /dev/binderfs
    ! mountpoint -q /dev/binderfs
    rm -f "$BINDER_MARKER"
  else
    printf 'refusing to replace BinderFS without a matching proof mount ID\n' >&2
    exit 3
  fi
elif [ -f "$BINDER_MARKER" ]; then
  rm -f "$BINDER_MARKER"
fi
mkdir -p /dev/binderfs
mount -t binder binder /dev/binderfs
test "$(findmnt -n -o FSTYPE --target /dev/binderfs)" = binder
binder_mount_id="$(findmnt -n -o ID --target /dev/binderfs)"
[[ "$binder_mount_id" =~ ^[0-9]+$ ]]
umask 077
printf '%s\n' "$binder_mount_id" >"${BINDER_MARKER}.tmp"
mv -f "${BINDER_MARKER}.tmp" "$BINDER_MARKER"

gcc -O2 -Wall -Wextra -Werror -std=c11 \
  "$HERE/create_binder_devices.c" -o /tmp/rex_create_binder_devices
/tmp/rex_create_binder_devices
rm -f /tmp/rex_create_binder_devices

for device in binder hwbinder vndbinder; do
  node="/dev/binderfs/$device"
  test -c "$node"
  chmod 0666 "$node"
  if ! python3 - "$node" <<'PY'
import os
import sys

fd = os.open(sys.argv[1], os.O_RDWR | os.O_CLOEXEC)
os.close(fd)
PY
  then
    printf 'Binder node is not openable: %s\n' "$node" >&2
    exit 3
  fi
  stat -c 'BINDER_NODE=%n major=%t minor=%T mode=%a' "$node"
done

if systemctl is-active --quiet "$DOCKER_UNIT"; then
  systemctl stop "$DOCKER_UNIT"
fi
if systemctl is-active --quiet "$DOCKER_UNIT"; then
  printf 'dedicated Docker unit did not stop\n' >&2
  exit 4
fi
systemctl reset-failed "$DOCKER_UNIT" >/dev/null 2>&1 || true
if [ -S "$DOCKER_SOCKET" ]; then
  printf 'dedicated Docker socket still exists after unit stop; run cleanup first\n' >&2
  exit 4
fi
rm -f /run/rexplayer-dockerd.pid
mkdir -p "$DOCKER_DATA_ROOT" "$DOCKER_EXEC_ROOT"
systemd-run --unit="${DOCKER_UNIT%.service}" --collect \
  --property=Nice=10 --property=CPUQuota=300% \
  /usr/bin/dockerd \
  --host="unix://$DOCKER_SOCKET" \
  --data-root="$DOCKER_DATA_ROOT" \
  --exec-root="$DOCKER_EXEC_ROOT" \
  --pidfile=/run/rexplayer-dockerd.pid \
  --bridge=none --iptables=false --ip-forward=false --ip-masq=false

for _ in $(seq 1 60); do
  if docker -H "unix://$DOCKER_SOCKET" info >/dev/null 2>&1; then
    docker -H "unix://$DOCKER_SOCKET" version --format 'DOCKER_SERVER={{.Server.Version}}'
    printf 'WSL_ANDROID_HOST_PREP=PASS\n'
    exit 0
  fi
  sleep 1
done
journalctl -u "$DOCKER_UNIT" --no-pager -n 100 >&2 || true
exit 1
