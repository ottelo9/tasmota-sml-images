// mtrc_crypto_selftest.h — de-risk harness for the Fork-B by-pointer crypto seam.
//
// ⚠ TEMPORARY SCAFFOLD (task #125) — delete once the real matter_c amalgamation +
// the mtrc_crypto_bind path land. Proven green on .156 2026-06-03.
//
// Validates, ON A LOADED PLUGIN, that matter_c's mtrc_crypto.c can call through
// the firmware-supplied BearSSL primitives (mtrc_crypto_ops) — INCLUDING the two
// const vtables that live in base .rodata (br_ec_p256_m15, br_sha256_vtable),
// whose method pointers are base-absolute and must work WITHOUT EXEC_OFFSET.
//
// Division of labour (deliberate, so the test isolates the SEAM and nothing else):
//   - The FIRMWARE owns every constant: the input vectors AND the expected KAT
//     outputs. Firmware .rodata has no relocation question, so the known-answer
//     comparison is trustworthy.
//   - The PLUGIN holds ZERO const data. matter_crypto_selftest() binds the ops,
//     runs each mtrc_* primitive reading inputs / writing outputs through this
//     struct, and returns. So a green result means the by-pointer seam (and the
//     two vtables) work — it is not entangled with the separate "plugin-local
//     const arrays under EXEC_OFFSET" question handled in the discipline pass.
//
// This header is included by BOTH sides: the plugin (xblib_02_matter.cpp, which
// fills the *_out fields) and the firmware (xdrv_124, which fills ops + inputs and
// checks the outputs). Keep it in sync. GPLv3.

#ifndef MTRC_CRYPTO_SELFTEST_H
#define MTRC_CRYPTO_SELFTEST_H

#include <stdint.h>
#include "mtrc_crypto_ops.h"   // the by-pointer crypto seam (pulls the br_* types)

#ifdef __cplusplus
extern "C" {
#endif

// One flat struct passed (by pointer, as the export's `buf` arg) firmware->plugin.
// All input pointers point into firmware .rodata; all *_out arrays are written by
// the plugin and read back + compared by the firmware.
typedef struct mtrc_crypto_selftest_io {
  const mtrc_crypto_ops *ops;     // firmware-filled crypto primitives (the seam)

  // ---- inputs (firmware -> plugin) ----
  const uint8_t *sha_in;          uint32_t sha_in_len;     // SHA-256 message
  const uint8_t *hmac_key;        uint32_t hmac_key_len;
  const uint8_t *hmac_in;         uint32_t hmac_in_len;
  const uint8_t *hkdf_salt;       uint32_t hkdf_salt_len;
  const uint8_t *hkdf_ikm;        uint32_t hkdf_ikm_len;
  const uint8_t *hkdf_info;       uint32_t hkdf_info_len;
  uint32_t       hkdf_out_len;                              // <= sizeof(hkdf_out)
  const uint8_t *ec_priv;         // 32-byte P-256 scalar (pub-from-priv + ECDSA)
  const uint8_t *ccm_key;         // 16 bytes (AES-128)
  const uint8_t *ccm_nonce;       uint32_t ccm_nonce_len;
  const uint8_t *ccm_aad;         uint32_t ccm_aad_len;
  const uint8_t *ccm_pt;          uint32_t ccm_pt_len;      // <= sizeof(ccm_ct)

  // ---- outputs (plugin -> firmware) ----
  uint8_t  sha_out[32];
  uint8_t  hmac_out[32];
  uint8_t  hkdf_out[64];
  uint8_t  ec_pub[65];            // 0x04 || X || Y  (validates mulgen vtable)
  uint8_t  ecdsa_sig[64];
  uint8_t  ecdsa_verify;          // 1 = plugin's own sign->verify round-trip held
  uint8_t  ccm_ct[64];            // ciphertext of ccm_pt
  uint8_t  ccm_tag[16];
  uint8_t  ccm_dec_ok;            // 1 = decrypt verified the tag
  uint8_t  ccm_dec[64];           // recovered plaintext (== ccm_pt on success)
  uint8_t  ran;                   // 0x5A marker: the export executed to the end
} mtrc_crypto_selftest_io;

// Plugin export (registered as a (buf,int)->int BLIB export): binds io->ops,
// runs all primitives through the seam, fills the *_out fields, returns io->ran.
int32_t matter_crypto_selftest(uint8_t *io_buf, int len);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CRYPTO_SELFTEST_H
