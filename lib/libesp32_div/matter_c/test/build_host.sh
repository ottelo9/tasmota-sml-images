#!/usr/bin/env bash
# Host build+run: SPAKE2+ Phase-0 spike vs the RFC 9383 P256 vector.
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build spike "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_spake2p.c" "$HERE/test_spake2p.c"
