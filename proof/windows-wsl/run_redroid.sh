#!/usr/bin/env bash
set -euo pipefail

DOCKER_SOCKET=/run/rexplayer-docker.sock
export DOCKER_HOST="unix://$DOCKER_SOCKET"
IMAGE=redroid/redroid@sha256:11d58a64bfbde2253d1cce81bff409ff58174980222d1bada232d9ef59181191
CONTAINER=rexplayer-redroid-wsl-proof
DATA=/opt/rexplayer-redroid-wsl-data
CONTAINER_ID_MARKER=/run/rexplayer-redroid-container-id

for device in binder hwbinder vndbinder; do
  test -c "/dev/binderfs/$device"
done
test -S "$DOCKER_SOCKET"
if [ -n "$(ss -H -ltn 'sport = :5555')" ]; then
  printf 'port 5555 is already listening\n' >&2
  exit 2
fi

docker pull "$IMAGE"
architecture="$(docker image inspect "$IMAGE" --format '{{.Architecture}}')"
printf 'IMAGE_ARCH=%s\n' "$architecture"
test "$architecture" = amd64
if docker container inspect "$CONTAINER" >/dev/null 2>&1; then
  docker rm -f "$CONTAINER"
fi
if [ -e "$CONTAINER_ID_MARKER" ]; then
  printf 'stale RexPlayer container marker exists; run cleanup first\n' >&2
  exit 3
fi
if [ -L "$DATA" ]; then
  printf 'refusing symlinked Android data directory: %s\n' "$DATA" >&2
  exit 3
fi
rm -rf "$DATA"
mkdir -p "$DATA"

docker run -d --name "$CONTAINER" \
  --privileged --network host --ipc host \
  --mount type=bind,source=/dev/binderfs/binder,target=/dev/binder \
  --mount type=bind,source=/dev/binderfs/hwbinder,target=/dev/hwbinder \
  --mount type=bind,source=/dev/binderfs/vndbinder,target=/dev/vndbinder \
  --mount type=bind,source=/dev/binderfs,target=/dev/binderfs \
  --mount type=bind,source="$DATA",target=/data \
  "$IMAGE" \
  androidboot.redroid_gpu_mode=guest \
  androidboot.redroid_width=720 \
  androidboot.redroid_height=1280 \
  androidboot.redroid_dpi=320 >/dev/null
container_id="$(docker inspect --format '{{.Id}}' "$CONTAINER")"
[[ "$container_id" =~ ^[0-9a-f]{64}$ ]]
umask 077
printf '%s\n' "$container_id" >"${CONTAINER_ID_MARKER}.tmp"
mv -f "${CONTAINER_ID_MARKER}.tmp" "$CONTAINER_ID_MARKER"

adb kill-server >/dev/null 2>&1 || true
booted=0
for _ in $(seq 1 240); do
  state="$(docker inspect "$CONTAINER" --format '{{.State.Status}} {{.State.ExitCode}}')"
  if [ "${state%% *}" != running ]; then
    printf 'CONTAINER_STATE=%s\n' "$state" >&2
    docker logs --tail 200 "$CONTAINER" >&2 || true
    exit 3
  fi
  adb connect 127.0.0.1:5555 >/dev/null 2>&1 || true
  if [ "$(adb -s 127.0.0.1:5555 get-state 2>/dev/null || true)" = device ] && \
     [ "$(adb -s 127.0.0.1:5555 shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; then
    booted=1
    break
  fi
  sleep 1
done
if [ "$booted" -ne 1 ]; then
  printf 'ANDROID_BOOT=TIMEOUT\n' >&2
  docker logs --tail 200 "$CONTAINER" >&2 || true
  exit 4
fi

printf 'ANDROID_BOOT=PASS\n'
printf 'ADB_STATE=%s\n' "$(adb -s 127.0.0.1:5555 get-state)"
printf 'ANDROID_RELEASE=%s\n' "$(adb -s 127.0.0.1:5555 shell getprop ro.build.version.release | tr -d '\r')"
printf 'ANDROID_SDK=%s\n' "$(adb -s 127.0.0.1:5555 shell getprop ro.build.version.sdk | tr -d '\r')"
printf 'ANDROID_ABI=%s\n' "$(adb -s 127.0.0.1:5555 shell getprop ro.product.cpu.abi | tr -d '\r')"
printf 'ANDROID_CONTAINER_RESULT=PASS\n'
