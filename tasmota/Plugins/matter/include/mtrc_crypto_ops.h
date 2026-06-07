// mtrc_crypto_ops.h — the BearSSL crypto seam for the matter BinPlugin (Fork B).
//
// matter_c's BearSSL use is isolated to mtrc_crypto.c. Rather than bundle the
// BearSSL subset into the plugin (Fork A — a multi-day, bit-exact PROGMEM/no-
// intrinsic port of ec_p256_m15 + the i15 bignum), the plugin receives the
// primitives BY POINTER, exactly like the matter_port_t HAL: the firmware fills
// this struct with its own linked br_* and hands it over once at load via
// mtrc_crypto_bind(); mtrc_crypto.c then calls through g_cr->… instead of the
// direct br_* symbols. Signatures match exactly (the fields are typed with
// __typeof__ of the real br_*), so there is no arg-packing and no jumptable /
// tmod_ext_call involvement at all.
//
// This header is included ONLY by mtrc_crypto.c (plugin side, calls through it)
// and by the firmware (base side, fills it) — NOT by the rest of matter_c,
// which only sees the mtrc_* API in mtrc_crypto.h. Keep the two copies in sync.
//
// The two const vtables (br_ec_p256_m15, br_sha256_vtable) live in base .rodata;
// their inner method pointers are base absolute addresses, so the plugin calls
// through them as-is (no EXEC_OFFSET). GPLv3.

#ifndef MTRC_CRYPTO_OPS_H
#define MTRC_CRYPTO_OPS_H

#include "t_bearssl.h"      // hash / hmac / kdf / block / aead (br_* types)
#include "t_bearssl_ec.h"   // br_ec_impl, br_ec_private_key/public_key, br_ecdsa_i15_* (NOT pulled in by t_bearssl.h)

#ifdef __cplusplus
extern "C" {
#endif

// Each fn field is typed as __typeof__(&br_xxx) so it is, by construction,
// byte-identical to the real BearSSL prototype — a hand-typed signature here
// would be the one place a subtle ABI mismatch could slip in. The base assigns
// `field = br_xxx;` (checked by the compiler against this exact type); the
// plugin calls `g_cr->field(...)` (checked the same way).
typedef struct mtrc_crypto_ops {
  // SHA-256
  __typeof__(&br_sha256_init)      sha256_init;
  __typeof__(&br_sha256_update)    sha256_update;
  __typeof__(&br_sha256_out)       sha256_out;
  // HMAC-SHA256
  __typeof__(&br_hmac_key_init)    hmac_key_init;
  __typeof__(&br_hmac_init)        hmac_init;
  __typeof__(&br_hmac_update)      hmac_update;
  __typeof__(&br_hmac_out)         hmac_out;
  // HKDF-SHA256
  __typeof__(&br_hkdf_init)        hkdf_init;
  __typeof__(&br_hkdf_inject)      hkdf_inject;
  __typeof__(&br_hkdf_flip)        hkdf_flip;
  __typeof__(&br_hkdf_produce)     hkdf_produce;
  // ECDSA P-256 (i15), raw r||s
  __typeof__(&br_ecdsa_i15_sign_raw) ecdsa_sign_raw;
  __typeof__(&br_ecdsa_i15_vrfy_raw) ecdsa_vrfy_raw;
  // AES-128-CCM (CT key schedule + CCM mode)
  __typeof__(&br_aes_ct_ctrcbc_init) aes_ct_ctrcbc_init;
  __typeof__(&br_ccm_init)         ccm_init;
  __typeof__(&br_ccm_reset)        ccm_reset;
  __typeof__(&br_ccm_aad_inject)   ccm_aad_inject;
  __typeof__(&br_ccm_flip)         ccm_flip;
  __typeof__(&br_ccm_run)          ccm_run;
  __typeof__(&br_ccm_get_tag)      ccm_get_tag;
  __typeof__(&br_ccm_check_tag)    ccm_check_tag;
  // const vtables in base .rodata (addresses handed over; method ptrs are base-absolute)
  const br_ec_impl    *ec_p256_m15;   // P-256 m15 EC implementation
  const br_hash_class *sha256_vtable; // SHA-256 hash class
} mtrc_crypto_ops;

// Bind the firmware-provided crypto primitives. Must be called once at plugin
// load, before any matter operation that touches crypto. Passing NULL (or never
// binding) makes every crypto entry point fail safe (return 0 / no-op) rather
// than wild-jump through an unset pointer.
void mtrc_crypto_bind(const mtrc_crypto_ops *ops);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CRYPTO_OPS_H
