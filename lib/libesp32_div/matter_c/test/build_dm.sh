#!/usr/bin/env bash
# Host build+run: Matter data-model registry test. Pure C (no crypto).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/.."
OUT=/tmp/mtrc_tests; mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_dm.c" "$HERE/test_dm.c" -o "$OUT/dm"
"$OUT/dm"
