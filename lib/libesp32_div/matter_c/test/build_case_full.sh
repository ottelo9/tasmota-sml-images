#!/usr/bin/env bash
# Host build+run: full CASE (Sigma1/2/3) handshake, initiator <-> responder.
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build case_full "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_case.c" \
   "$LIB/src/mtrc_case_msg.c" "$LIB/src/mtrc_tlv.c" "$HERE/test_case_full.c"
