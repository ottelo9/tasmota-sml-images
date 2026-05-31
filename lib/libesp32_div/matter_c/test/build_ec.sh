#!/usr/bin/env bash
# Host build+run: ECDH + ECDSA self-test (CASE crypto).
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build ec "$LIB/src/mtrc_crypto.c" "$HERE/test_ec.c"
