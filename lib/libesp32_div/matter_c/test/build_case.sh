#!/usr/bin/env bash
# Host build+run: CASE key-schedule self-test.
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build case "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_case.c" "$HERE/test_case.c"
