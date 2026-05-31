#!/usr/bin/env bash
# Host-side build+run of the TLV codec self-test. Pure C, no BearSSL.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/.."
OUT=/tmp/mtrc_tlv_test
mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_tlv.c" "$HERE/test_tlv.c" -o "$OUT/tlv"
"$OUT/tlv"
