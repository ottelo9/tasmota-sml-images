// mtrc_crypto.h — thin crypto layer over BearSSL for matter_c.
//
// All primitives Matter needs, mapped onto the BearSSL br_* API that
// Tasmota already links: SHA-256, HMAC-SHA256, HKDF-SHA256, and the
// P-256 (secp256r1) elliptic-curve operations via br_ec_p256_m15 —
// the low-RAM 15-bit-limb implementation (stack-only bignums, no malloc).
//
// EC points are uncompressed (0x04 || X || Y, 65 bytes). Scalars are
// big-endian, length-flexible (BearSSL accepts arbitrary scalar length).
//
// GPLv3. Crypto library: BearSSL (T. Pornin, BSD).

#ifndef MTRC_CRYPTO_H
#define MTRC_CRYPTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTRC_P256_POINT_LEN   65   // uncompressed: 0x04 || X(32) || Y(32)
#define MTRC_P256_SCALAR_LEN  32
#define MTRC_SHA256_LEN       32

// ---- hashing / MAC / KDF ----------------------------------------------
void mtrc_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

void mtrc_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len, uint8_t out[32]);

// HKDF-SHA256 (extract+expand). salt may be NULL/0 (RFC 5869 zero salt).
// Returns 1 on success.
int  mtrc_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                      const uint8_t *ikm, size_t ikm_len,
                      const uint8_t *info, size_t info_len,
                      uint8_t *out, size_t out_len);

// ---- P-256 EC ops (uncompressed points, BE scalars) -------------------
// All return 1 on success, 0 on failure (invalid point / out-of-range).

// out = k * G      (G = conventional generator)
int  mtrc_ec_mulgen(uint8_t out[65], const uint8_t *k, size_t k_len);

// point = k * point   (in place)
int  mtrc_ec_mul(uint8_t point[65], const uint8_t *k, size_t k_len);

// A = a*A + b*B    (in place into A). If B is NULL, B = generator.
//   Mirrors BearSSL muladd semantics (x*A + y*B); here a<->x, b<->y.
int  mtrc_ec_muladd(uint8_t A[65], const uint8_t *B,
                    const uint8_t *a, size_t a_len,
                    const uint8_t *b, size_t b_len);

// pub = priv * G  (uncompressed public key from a 32-byte private scalar).
int  mtrc_ec_pub_from_priv(uint8_t pub[65], const uint8_t priv[32]);

// ECDH: shared_x = X-coordinate of (priv * peer_pub). 32-byte output.
int  mtrc_ecdh(uint8_t shared_x[32], const uint8_t peer_pub[65], const uint8_t priv[32]);

// ECDSA P-256 over a precomputed 32-byte message hash. Signatures are raw
// r||s (64 bytes). Signing is deterministic (RFC 6979, via BearSSL).
int  mtrc_ecdsa_sign(uint8_t sig[64], const uint8_t hash[32], const uint8_t priv[32]);
int  mtrc_ecdsa_verify(const uint8_t sig[64], const uint8_t hash[32], const uint8_t pub[65]);

// neg = (n - s) mod n, for a 32-byte big-endian scalar s < n
// (n = P-256 subgroup order). Used to turn point subtraction into muladd.
void mtrc_ec_scalar_neg(const uint8_t s[32], uint8_t neg[32]);

// Reduce an arbitrary-length big-endian integer modulo the P-256 order n,
// producing a 32-byte big-endian scalar. Used for SPAKE2+ w0/w1 from the
// 40-byte PBKDF2 halves.
void mtrc_ec_scalar_reduce(const uint8_t *in, size_t in_len, uint8_t out[32]);

// PBKDF2-HMAC-SHA256 (RFC 8018). Returns 1 on success.
int  mtrc_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                        const uint8_t *salt, size_t salt_len,
                        uint32_t iterations, uint8_t *out, size_t out_len);

// ---- AES-128-CCM (Matter message encryption) --------------------------
#define MTRC_CCM_NONCE_LEN 13
#define MTRC_CCM_TAG_LEN   16

// Encrypt in place (data: plaintext -> ciphertext), writing the auth tag.
// Returns 1 on success.
int mtrc_aes_ccm_encrypt(const uint8_t key[16],
                         const uint8_t *nonce, size_t nonce_len,
                         const uint8_t *aad, size_t aad_len,
                         uint8_t *data, size_t data_len,
                         uint8_t *tag, size_t tag_len);

// Decrypt in place (data: ciphertext -> plaintext) and verify the tag.
// Returns 1 if authentic, 0 if the tag check fails.
int mtrc_aes_ccm_decrypt(const uint8_t key[16],
                         const uint8_t *nonce, size_t nonce_len,
                         const uint8_t *aad, size_t aad_len,
                         uint8_t *data, size_t data_len,
                         const uint8_t *tag, size_t tag_len);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CRYPTO_H
