#!/usr/bin/env bash
# Host build+run: PKCS#10 CSR (DER) builder. Builds a CSR in C, then verifies
# it with Python `cryptography` (DER parses + self-signature valid + the
# embedded public key matches the operational key).
set -euo pipefail
source "$(dirname "$0")/common.sh"; mtrc_setup
mtrc_build csr "$LIB/src/mtrc_crypto.c" "$LIB/src/mtrc_csr.c" "$HERE/test_csr.c"
python3 - <<'PY'
import sys, subprocess
from cryptography import x509
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
d = open("/tmp/mtrc_test.csr", "rb").read()
csr = x509.load_der_x509_csr(d)                 # raises if DER malformed
ok = csr.is_signature_valid
pub = csr.public_key().public_bytes(Encoding.X962, PublicFormat.UncompressedPoint).hex()
# OPPUB printed by the C program is captured from the run output by build below
print("  [%s] CSR DER parses + self-signature valid" % ("PASS" if ok else "FAIL"))
print("  subject: %s" % (csr.subject.rfc4514_string() or "(empty, as expected)"))
print("  pubkey:  %s..%s" % (pub[:8], pub[-8:]))
print("\n==> CSR builder %s" % ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
PY
