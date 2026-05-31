#!/usr/bin/env bash
# Host build+run: Matter operational-certificate parser test. Pure C (TLV).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/.."
OUT=/tmp/mtrc_tests; mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_tlv.c" "$LIB/src/mtrc_cert.c" "$HERE/test_cert.c" -o "$OUT/cert"
"$OUT/cert"
