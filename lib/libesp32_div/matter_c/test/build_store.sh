#!/usr/bin/env bash
# Host build+run: Matter fabric table (store) test. Pure C (no crypto).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/.."
OUT=/tmp/mtrc_tests; mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_store.c" "$HERE/test_store.c" -o "$OUT/store"
"$OUT/store"
