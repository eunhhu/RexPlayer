#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/rex_uinput_mt"
gcc -O2 -Wall -Wextra -Werror -std=c11 "$HERE/rex_uinput_mt.c" -o "$OUT"
sudo -n "$OUT"
