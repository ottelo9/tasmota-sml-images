#!/usr/bin/env bash
# Host build+run: PASE key schedule + commissioning messages.
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build pase "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_spake2p.c" \
               "$LIB/src/mtrc_tlv.c" "$LIB/src/mtrc_pase.c" "$HERE/test_pase.c"
