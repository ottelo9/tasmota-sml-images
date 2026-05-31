#!/usr/bin/env bash
# Host-side build+run of the message-layer self-test (frame + MRP). Pure C.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
LIB="$HERE/.."
OUT=/tmp/mtrc_msg_test
mkdir -p "$OUT"
cc -std=c11 -O2 -Wall -Wextra -I"$LIB/include" \
   "$LIB/src/mtrc_frame.c" "$LIB/src/mtrc_mrp.c" "$HERE/test_msg.c" \
   -o "$OUT/msg"
"$OUT/msg"
