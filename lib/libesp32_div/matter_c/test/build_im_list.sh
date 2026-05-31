#!/usr/bin/env bash
# Host build+run: Descriptor list report builders (ServerList / ClientList /
# DeviceTypeList) — build then TLV-decode round-trip. Pure C (no crypto).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; LIB="$HERE/.."
OUT=/tmp/mtrc_tests; mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_im.c" "$LIB/src/mtrc_tlv.c" "$HERE/test_im_list.c" -o "$OUT/imlist"
"$OUT/imlist"
