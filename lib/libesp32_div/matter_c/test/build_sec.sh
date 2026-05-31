#!/usr/bin/env bash
# Host build+run: AES-CCM secured message layer.
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build sec "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_frame.c" \
              "$LIB/src/mtrc_sec.c" "$HERE/test_sec.c"
