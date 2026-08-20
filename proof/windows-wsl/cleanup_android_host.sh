#!/usr/bin/env bash
set -euo pipefail

DOCKER_SOCKET=/run/rexplayer-docker.sock
DOCKER_UNIT=rexplayer-dockerd.service
CONTAINER=rexplayer-redroid-wsl-proof
BINDER_MARKER=/run/rexplayer-binderfs-owned
CONTAINER_ID_MARKER=/run/rexplayer-redroid-container-id
PURGE_DATA=0
if [ "${1:-}" = --purge-data ]; then
  PURGE_DATA=1
elif [ "$#" -ne 0 ]; then
  printf 'usage: %s [--purge-data]\n' "$0" >&2
  exit 64
fi
if [ "$(id -u)" -ne 0 ]; then
  printf 'run as root inside the dedicated WSL lab distribution\n' >&2
  exit 77
fi
for command_name in docker findmnt mountpoint ss systemctl; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'missing prerequisite: %s\n' "$command_name" >&2
    exit 2
  fi
done

container_id=""
if [ -f "$CONTAINER_ID_MARKER" ]; then
  container_id="$(<"$CONTAINER_ID_MARKER")"
  if [[ ! "$container_id" =~ ^[0-9a-f]{64}$ ]]; then
    printf 'invalid RexPlayer container ID marker\n' >&2
    exit 5
  fi
fi

unit_active=0
if systemctl is-active --quiet "$DOCKER_UNIT"; then
  unit_active=1
fi
if [ -S "$DOCKER_SOCKET" ]; then
  if [ "$unit_active" -ne 1 ]; then
    printf 'refusing a Docker socket not owned by the active RexPlayer unit\n' >&2
    exit 5
  fi
  if ! docker -H "unix://$DOCKER_SOCKET" info >/dev/null 2>&1; then
    printf 'dedicated Docker socket exists but daemon is unreachable\n' >&2
    exit 5
  fi
  if docker -H "unix://$DOCKER_SOCKET" inspect "$CONTAINER" >/dev/null 2>&1; then
    docker -H "unix://$DOCKER_SOCKET" rm -f "$CONTAINER" >/dev/null
  fi
  if docker -H "unix://$DOCKER_SOCKET" inspect "$CONTAINER" >/dev/null 2>&1; then
    printf 'RexPlayer Android container still exists after removal\n' >&2
    exit 5
  fi
fi

if systemctl is-active --quiet "$DOCKER_UNIT"; then
  systemctl stop "$DOCKER_UNIT"
fi
if systemctl is-active --quiet "$DOCKER_UNIT"; then
  printf 'dedicated Docker unit is still active\n' >&2
  exit 5
fi
systemctl reset-failed "$DOCKER_UNIT" >/dev/null 2>&1 || true
rm -f "$DOCKER_SOCKET" /run/rexplayer-dockerd.pid
if mountpoint -q /run/rexplayer-docker/netns/default; then
  umount /run/rexplayer-docker/netns/default
fi
if mountpoint -q /run/rexplayer-docker/netns/default; then
  printf 'dedicated Docker network namespace is still mounted\n' >&2
  exit 5
fi
rm -rf /run/rexplayer-docker /opt/rexplayer-redroid-wsl-data

if [ -n "$container_id" ]; then
  for cgroup_file in /proc/[0-9]*/cgroup; do
    [ -r "$cgroup_file" ] || continue
    if [[ "$(<"$cgroup_file")" == *"$container_id"* ]]; then
      printf 'container cgroup still has a live process: %s\n' "$cgroup_file" >&2
      exit 5
    fi
  done
  rm -f "$CONTAINER_ID_MARKER"
fi

owned_binder=0
if [ -f "$BINDER_MARKER" ]; then
  owned_binder=1
  binder_mount_id="$(<"$BINDER_MARKER")"
  if [[ ! "$binder_mount_id" =~ ^[0-9]+$ ]]; then
    printf 'invalid BinderFS ownership marker\n' >&2
    exit 5
  fi
  if mountpoint -q /dev/binderfs; then
    if [ "$(findmnt -n -o FSTYPE --target /dev/binderfs)" != binder ] ||
      [ "$(findmnt -n -o ID --target /dev/binderfs)" != "$binder_mount_id" ]; then
      printf 'BinderFS mount does not match the recorded proof mount ID\n' >&2
      exit 5
    fi
    umount /dev/binderfs
    if mountpoint -q /dev/binderfs; then
      printf 'owned BinderFS mount is still active\n' >&2
      exit 5
    fi
  fi
  rm -f "$BINDER_MARKER"
  rmdir /dev/binderfs >/dev/null 2>&1 || true
fi

if [ "$PURGE_DATA" -eq 1 ]; then
  rm -rf /var/lib/rexplayer-docker
fi
if [ -S "$DOCKER_SOCKET" ] ||
  systemctl is-active --quiet "$DOCKER_UNIT" ||
  [ -f "$BINDER_MARKER" ] ||
  [ -f "$CONTAINER_ID_MARKER" ] ||
  mountpoint -q /run/rexplayer-docker/netns/default; then
  printf 'post-cleanup ownership check failed\n' >&2
  exit 5
fi
if [ "$owned_binder" -eq 1 ] && mountpoint -q /dev/binderfs; then
  printf 'owned BinderFS mount survived cleanup\n' >&2
  exit 5
fi
if [ -n "$(ss -H -ltn 'sport = :5555')" ]; then
  printf 'ADB listener on TCP 5555 survived cleanup\n' >&2
  exit 5
fi
printf 'WSL_ANDROID_CLEANUP=PASS\n'
