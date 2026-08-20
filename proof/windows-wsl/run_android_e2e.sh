#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROOF_ROOT="$(cd "$HERE/.." && pwd)"
OUT=${1:-"$HERE/out/runtime-$(date -u +%Y%m%dT%H%M%SZ)"}
DOCKER_SOCKET=/run/rexplayer-docker.sock
CONTAINER=rexplayer-redroid-wsl-proof
SAFE_BASH=(/usr/bin/env -u BASH_ENV -u ENV /bin/bash --noprofile --norc)

TMP=''

if [ "$(id -u)" -ne 0 ]; then
  printf 'run as root inside the dedicated WSL lab distribution\n' >&2
  exit 77
fi
if [ -L "$OUT" ]; then
  printf 'refusing symlinked output directory: %s\n' "$OUT" >&2
  exit 3
fi
mkdir -p "$OUT"
shopt -s dotglob nullglob
out_entries=("$OUT"/*)
shopt -u dotglob nullglob
if [ "${#out_entries[@]}" -ne 0 ]; then
  printf 'output directory must be empty: %s\n' "$OUT" >&2
  exit 3
fi
mkdir -p "$OUT/input" "$OUT/stealth"

cleanup() {
  status=$?
  cleanup_status=0
  trap - EXIT
  set +e
  if [ -n "$TMP" ]; then
    rm -rf "$TMP"
  fi
  if test -S "$DOCKER_SOCKET"; then
    docker -H "unix://$DOCKER_SOCKET" logs --tail 300 "$CONTAINER" \
      >"$OUT/redroid-tail.log" 2>&1 || true
  fi
  "${SAFE_BASH[@]}" "$HERE/cleanup_android_host.sh" || cleanup_status=$?
  if [ "$status" -ne 0 ]; then
    exit "$status"
  fi
  if [ "$cleanup_status" -ne 0 ]; then
    exit "$cleanup_status"
  fi
  printf 'WINDOWS_WSL_ANDROID_INPUT=PASS\n'
  printf 'WINDOWS_WSL_ANDROID_MATRIX=CAPTURED\n'
  printf 'WINDOWS_WSL_ANDROID_E2E=PASS\n'
  exit 0
}
trap cleanup EXIT

"${SAFE_BASH[@]}" "$HERE/prepare_android_host.sh"
"${SAFE_BASH[@]}" "$HERE/run_redroid.sh"

REX_DOCKER_HOST="unix://$DOCKER_SOCKET" \
REX_PRE_EMIT_MS=5000 REX_HOLD_MS=8000 \
  "${SAFE_BASH[@]}" "$PROOF_ROOT/input/run_android_container_probe.sh" \
  "$CONTAINER" "$OUT/input" "$PROOF_ROOT/input/rex_uinput_mt.c"

TMP="$(mktemp -d)"
cp "$PROOF_ROOT/stealth/run_matrix.sh" "$TMP/run_matrix.sh"
cp "$PROOF_ROOT/stealth/android_detection_matrix.sh" "$TMP/android_detection_matrix.sh"
set +e
"${SAFE_BASH[@]}" "$TMP/run_matrix.sh" 127.0.0.1:5555
matrix_rc=$?
set -e
for path in "$TMP"/result.tsv "$TMP"/getprop.txt "$TMP"/dumpsys-*.txt; do
  [ -f "$path" ] && cp "$path" "$OUT/stealth/"
done
printf 'MATRIX_EXIT=%s\n' "$matrix_rc" | tee "$OUT/stealth/exit.txt"
case "$matrix_rc" in
  0|3|4) ;;
  *) printf 'unexpected matrix exit=%s\n' "$matrix_rc" >&2; exit "$matrix_rc" ;;
esac
