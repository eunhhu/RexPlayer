#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
CONTAINER="${1:-rexplayer-redroid-proof}"
OUT="${2:-$HERE/android-proof}"
SOURCE="${3:-$HERE/rex_uinput_mt.c}"
PRE_EMIT_MS="${REX_PRE_EMIT_MS:-5000}"
HOLD_MS="${REX_HOLD_MS:-8000}"
REMOTE_BIN=/data/local/tmp/rex_uinput_mt.static
PRODUCER_PIDFILE=/data/local/tmp/rex_uinput_producer.pid
READER_PIDFILE=/data/local/tmp/rex_getevent.pid

validate_ms() {
  local name="$1" value="$2" min="$3"
  if [[ ! "$value" =~ ^[0-9]+$ ]] || [ "${#value}" -gt 5 ]; then
    printf '%s must be an integer between %s and 60000\n' "$name" "$min" >&2
    exit 64
  fi
  local numeric=$((10#$value))
  if [ "$numeric" -lt "$min" ] || [ "$numeric" -gt 60000 ]; then
    printf '%s must be between %s and 60000\n' "$name" "$min" >&2
    exit 64
  fi
}
validate_ms REX_PRE_EMIT_MS "$PRE_EMIT_MS" 1000
validate_ms REX_HOLD_MS "$HOLD_MS" 0

docker_host="${REX_DOCKER_HOST:-${DOCKER_HOST:-}}"
docker_cmd=(sudo -n)
if [ -n "$docker_host" ]; then
  docker_cmd+=(env "DOCKER_HOST=$docker_host")
fi
docker_cmd+=(docker)

mkdir -p "$OUT"
rm -f "$OUT/producer.log" "$OUT/getevent.log" \
  "$OUT/dumpsys-input-live.txt" "$OUT/summary.txt"

work="$(mktemp -d)"
producer=""
reader=""

kill_container_pidfile() {
  local pidfile="$1" expected_comm="$2" pid='' comm=''
  pid="$("${docker_cmd[@]}" exec "$CONTAINER" cat "$pidfile" 2>/dev/null || true)"
  if [[ "$pid" =~ ^[0-9]+$ ]]; then
    comm="$("${docker_cmd[@]}" exec "$CONTAINER" cat "/proc/$pid/comm" 2>/dev/null || true)"
    if [ "$comm" = "$expected_comm" ]; then
      "${docker_cmd[@]}" exec "$CONTAINER" kill "$pid" >/dev/null 2>&1 || true
    fi
  fi
  "${docker_cmd[@]}" exec "$CONTAINER" rm -f "$pidfile" >/dev/null 2>&1 || true
}

cleanup() {
  set +e
  if [ -n "$reader" ]; then
    kill_container_pidfile "$READER_PIDFILE" getevent
    kill "$reader" >/dev/null 2>&1 || true
  fi
  if [ -n "$producer" ]; then
    kill_container_pidfile "$PRODUCER_PIDFILE" rex_uinput_mt.s
    kill "$producer" >/dev/null 2>&1 || true
  fi
  "${docker_cmd[@]}" exec "$CONTAINER" rm -f \
    "$REMOTE_BIN" "$PRODUCER_PIDFILE" "$READER_PIDFILE" >/dev/null 2>&1 || true
  rm -rf "$work"
}
trap cleanup EXIT

cc -std=c11 -O2 -Wall -Wextra -Werror -static \
  -o "$work/rex_uinput_mt.static" "$SOURCE"
"${docker_cmd[@]}" inspect "$CONTAINER" >/dev/null
"${docker_cmd[@]}" exec "$CONTAINER" mkdir -p /data/local/tmp
"${docker_cmd[@]}" exec "$CONTAINER" rm -f \
  "$REMOTE_BIN" "$PRODUCER_PIDFILE" "$READER_PIDFILE"
"${docker_cmd[@]}" cp "$work/rex_uinput_mt.static" "$CONTAINER:$REMOTE_BIN" >/dev/null
"${docker_cmd[@]}" exec "$CONTAINER" chmod 755 "$REMOTE_BIN"

"${docker_cmd[@]}" exec \
  -e REX_CREATE_EVENT_NODE=1 \
  -e "REX_PRE_EMIT_MS=$PRE_EMIT_MS" \
  -e "REX_HOLD_MS=$HOLD_MS" \
  "$CONTAINER" sh -c \
  'echo $$ > /data/local/tmp/rex_uinput_producer.pid; exec /data/local/tmp/rex_uinput_mt.static' \
  >"$OUT/producer.log" 2>&1 &
producer=$!

ready=0
for _ in $(seq 1 50); do
  if grep -q '^DEVICE ' "$OUT/producer.log" 2>/dev/null; then
    ready=1
    break
  fi
  kill -0 "$producer" 2>/dev/null || break
  sleep 0.2
done
if [ "$ready" != 1 ]; then
  wait "$producer" || true
  producer=""
  "${docker_cmd[@]}" exec "$CONTAINER" rm -f "$PRODUCER_PIDFILE" >/dev/null 2>&1 || true
  printf 'input producer did not create a device\n' >&2
  sed -n '1,160p' "$OUT/producer.log" >&2
  exit 2
fi

event_node="$(grep -m1 -oE '/dev/input/event[0-9]+' "$OUT/producer.log")"
if [[ ! "$event_node" =~ ^/dev/input/event[0-9]+$ ]]; then
  printf 'invalid event node from producer: %s\n' "$event_node" >&2
  exit 2
fi

"${docker_cmd[@]}" exec "$CONTAINER" sh -c \
  'echo $$ > /data/local/tmp/rex_getevent.pid; exec getevent -lt "$1"' sh "$event_node" \
  >"$OUT/getevent.log" 2>&1 &
reader=$!
reader_ready=0
for _ in $(seq 1 30); do
  if "${docker_cmd[@]}" exec "$CONTAINER" test -s "$READER_PIDFILE" 2>/dev/null; then
    reader_ready=1
    break
  fi
  kill -0 "$reader" 2>/dev/null || break
  sleep 0.1
done
if [ "$reader_ready" != 1 ]; then
  printf 'Android getevent reader did not start\n' >&2
  exit 2
fi

inputreader_matches() {
  python3 - "$OUT/dumpsys-input-live.txt" <<'PY'
from pathlib import Path
import re
import sys
text = Path(sys.argv[1]).read_text(errors="replace")
reader = text.split("Input Reader State", 1)
if len(reader) != 2:
    raise SystemExit(1)
match = re.search(
    r"\n  Device -?\d+: RexPlayer Virtual Multi-Touch Proof\n(.*?)(?=\n  Device -?\d+:|\n\S|\Z)",
    reader[1],
    re.S,
)
if not match:
    raise SystemExit(1)
block = match.group(1)
if "Sources: TOUCHSCREEN" not in block or "DeviceType: TOUCH_SCREEN" not in block:
    raise SystemExit(1)
PY
}

for _ in $(seq 1 25); do
  "${docker_cmd[@]}" exec "$CONTAINER" dumpsys input >"$OUT/dumpsys-input-live.txt"
  inputreader_matches && break
  sleep 0.2
done

set +e
wait "$producer"
producer_rc=$?
producer=""
"${docker_cmd[@]}" exec "$CONTAINER" rm -f "$PRODUCER_PIDFILE" >/dev/null 2>&1 || true
kill_container_pidfile "$READER_PIDFILE" getevent
wait "$reader"
reader_rc=$?
reader=""
set -e

producer_pass=NO
grep -q '^RESULT PASS$' "$OUT/producer.log" && producer_pass=YES

reader_events="$(grep -cE 'EV_(KEY|ABS|SYN)' "$OUT/getevent.log" || true)"
getevent_sequence=NO
if python3 - "$OUT/getevent.log" <<'PY'
from pathlib import Path
import re
import sys
expected = [
    ("EV_KEY", "BTN_TOUCH", "DOWN"),
    ("EV_ABS", "ABS_MT_TRACKING_ID", "0000002a"),
    ("EV_ABS", "ABS_MT_POSITION_X", "00000064"),
    ("EV_ABS", "ABS_MT_POSITION_Y", "000000c8"),
    ("EV_ABS", "ABS_X", "00000064"),
    ("EV_ABS", "ABS_Y", "000000c8"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
    ("EV_ABS", "ABS_MT_POSITION_X", "00000190"),
    ("EV_ABS", "ABS_MT_POSITION_Y", "000001f4"),
    ("EV_ABS", "ABS_X", "00000190"),
    ("EV_ABS", "ABS_Y", "000001f4"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
    ("EV_ABS", "ABS_MT_TRACKING_ID", "ffffffff"),
    ("EV_KEY", "BTN_TOUCH", "UP"),
    ("EV_SYN", "SYN_REPORT", "00000000"),
]
pattern = re.compile(r"\]\s+(EV_[A-Z]+)\s+(\S+)\s+(\S+)\s*$")
observed = []
for line in Path(sys.argv[1]).read_text(errors="replace").splitlines():
    match = pattern.search(line)
    if match:
        observed.append(match.groups())
if observed != expected:
    print(f"expected {expected!r}\nobserved {observed!r}", file=sys.stderr)
    raise SystemExit(1)
PY
then
  getevent_sequence=YES
fi

inputreader_registered=NO
touchscreen_source=NO
touchscreen_device_type=NO
if inputreader_matches; then
  inputreader_registered=YES
  touchscreen_source=YES
  touchscreen_device_type=YES
fi

result=FAIL
if [ "$producer_rc" -eq 0 ] && [ "$producer_pass" = YES ] && \
   [ "$getevent_sequence" = YES ] && [ "$inputreader_registered" = YES ] && \
   [ "$touchscreen_source" = YES ] && [ "$touchscreen_device_type" = YES ]; then
  result=PASS
fi

{
  printf 'PRODUCER_EXIT=%s\n' "$producer_rc"
  printf 'PRODUCER_PASS=%s\n' "$producer_pass"
  printf 'GETEVENT_EXIT=%s\n' "$reader_rc"
  printf 'GETEVENT_EVENT_LINES=%s\n' "$reader_events"
  printf 'GETEVENT_SEQUENCE=%s\n' "$getevent_sequence"
  printf 'INPUTREADER_REGISTERED=%s\n' "$inputreader_registered"
  printf 'TOUCHSCREEN_SOURCE=%s\n' "$touchscreen_source"
  printf 'TOUCHSCREEN_DEVICE_TYPE=%s\n' "$touchscreen_device_type"
  printf 'EVENT_NODE=%s\n' "$event_node"
  printf 'RESULT=%s\n' "$result"
} | tee "$OUT/summary.txt"

[ "$result" = PASS ]
