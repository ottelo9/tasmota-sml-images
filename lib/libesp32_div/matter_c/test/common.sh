# Shared host-build helper for matter_c crypto tests (sourced, not run).
# mtrc_crypto.c is a single TU referencing P-256 + ECDSA + AES-CCM +
# SHA/HMAC/HKDF, so any test linking it needs that whole BearSSL set.
# Explicit allowlist (the Tasmota BearSSL ships only m15/i15 — the
# default-selector and m31/i31/hardware-AES files are absent/unneeded).

mtrc_setup() {
  HERE="$(cd "$(dirname "${BASH_SOURCE[1]}")" && pwd)"
  LIB="$HERE/.."
  REPO="$(cd "$LIB/../../.." && pwd)"
  BSSL="$REPO/lib/lib_ssl/bearssl-esp8266/src"
  OUT=/tmp/mtrc_tests
  mkdir -p "$OUT"
}

# mtrc_build <outname> <src...>   (src = matter_c lib sources + test main)
mtrc_build() {
  local out="$1"; shift
  local bssl="\
$BSSL/ec/ec_p256_m15.c $BSSL/ec/ec_secp256r1.c $BSSL/ec/ec_secp384r1.c \
$BSSL/ec/ec_secp521r1.c $BSSL/ec/ecdsa_i15_sign_raw.c \
$BSSL/ec/ecdsa_i15_vrfy_raw.c $BSSL/ec/ecdsa_i15_bits.c \
$BSSL/rand/hmac_drbg.c $BSSL/aead/ccm.c \
$BSSL/symcipher/aes_ct.c $BSSL/symcipher/aes_ct_enc.c \
$BSSL/symcipher/aes_ct_dec.c $BSSL/symcipher/aes_ct_ctrcbc.c \
$BSSL/hash/sha2small.c $BSSL/mac/hmac.c $BSSL/kdf/hkdf.c \
$(find "$BSSL"/int -name 'i15_*.c') $(find "$BSSL"/codec -name '*.c')"
  cc -std=c11 -O2 -Wall -I"$BSSL" -I"$LIB/include" $bssl "$@" -o "$OUT/$out"
  "$OUT/$out"
}
