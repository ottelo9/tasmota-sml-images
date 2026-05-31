#!/usr/bin/env bash
# Host build+run: CASE Sigma message codec round-trips. Pure C (TLV only).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/.."
OUT=/tmp/mtrc_tests; mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_tlv.c" "$LIB/src/mtrc_case_msg.c" "$HERE/test_case_msg.c" -o "$OUT/case_msg"
"$OUT/case_msg"
