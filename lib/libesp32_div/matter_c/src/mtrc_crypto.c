// mtrc_crypto.c — BearSSL-backed crypto primitives for matter_c.
// See mtrc_crypto.h. GPLv3; crypto via BearSSL (BSD).

#include "mtrc_crypto.h"
#include "t_bearssl.h"
#include <string.h>

#define MTRC_CURVE  BR_EC_secp256r1   // 23

// P-256 subgroup order n (big-endian).
static const uint8_t P256_N[32] = {
  0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51
};

static const br_ec_impl *EC(void) { return &br_ec_p256_m15; }

// ---- hashing / MAC / KDF ----------------------------------------------
void mtrc_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
  br_sha256_context c;
  br_sha256_init(&c);
  br_sha256_update(&c, data, len);
  br_sha256_out(&c, out);
}

void mtrc_hmac_sha256(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len, uint8_t out[32]) {
  br_hmac_key_context kc;
  br_hmac_context hc;
  br_hmac_key_init(&kc, &br_sha256_vtable, key, key_len);
  br_hmac_init(&hc, &kc, 0);                 // 0 = full-length output (32)
  br_hmac_update(&hc, data, data_len);
  br_hmac_out(&hc, out);
}

int mtrc_hkdf_sha256(const uint8_t *salt, size_t salt_len,
                     const uint8_t *ikm, size_t ikm_len,
                     const uint8_t *info, size_t info_len,
                     uint8_t *out, size_t out_len) {
  br_hkdf_context hc;
  br_hkdf_init(&hc, &br_sha256_vtable, salt, salt_len);
  br_hkdf_inject(&hc, ikm, ikm_len);
  br_hkdf_flip(&hc);
  br_hkdf_produce(&hc, info, info_len, out, out_len);
  return 1;
}

// ---- P-256 EC ops ------------------------------------------------------
int mtrc_ec_mulgen(uint8_t out[65], const uint8_t *k, size_t k_len) {
  size_t r = EC()->mulgen(out, k, k_len, MTRC_CURVE);
  return r != 0;
}

int mtrc_ec_mul(uint8_t point[65], const uint8_t *k, size_t k_len) {
  return (int)EC()->mul(point, MTRC_P256_POINT_LEN, k, k_len, MTRC_CURVE);
}

int mtrc_ec_muladd(uint8_t A[65], const uint8_t *B,
                   const uint8_t *a, size_t a_len,
                   const uint8_t *b, size_t b_len) {
  // BearSSL: muladd(A, B, len, x, xlen, y, ylen, curve) => A = x*A + y*B
  return (int)EC()->muladd(A, B, MTRC_P256_POINT_LEN,
                           a, a_len, b, b_len, MTRC_CURVE);
}

int mtrc_ec_pub_from_priv(uint8_t pub[65], const uint8_t priv[32]) {
  return mtrc_ec_mulgen(pub, priv, 32);
}

int mtrc_ecdh(uint8_t shared_x[32], const uint8_t peer_pub[65], const uint8_t priv[32]) {
  uint8_t pt[65];
  memcpy(pt, peer_pub, 65);
  if (!mtrc_ec_mul(pt, priv, 32)) return 0;   // pt = priv * peer_pub
  memcpy(shared_x, pt + 1, 32);               // X coordinate
  return 1;
}

int mtrc_ecdsa_sign(uint8_t sig[64], const uint8_t hash[32], const uint8_t priv[32]) {
  br_ec_private_key sk;
  sk.curve = MTRC_CURVE; sk.x = (unsigned char *)priv; sk.xlen = 32;
  // Deterministic (RFC 6979): hf drives the internal HMAC-DRBG.
  size_t n = br_ecdsa_i15_sign_raw(EC(), &br_sha256_vtable, hash, &sk, sig);
  return n == 64 ? 1 : 0;
}

int mtrc_ecdsa_verify(const uint8_t sig[64], const uint8_t hash[32], const uint8_t pub[65]) {
  br_ec_public_key pk;
  pk.curve = MTRC_CURVE; pk.q = (unsigned char *)pub; pk.qlen = 65;
  return br_ecdsa_i15_vrfy_raw(EC(), hash, 32, &pk, sig, 64) ? 1 : 0;
}

void mtrc_ec_scalar_neg(const uint8_t s[32], uint8_t neg[32]) {
  // neg = n - s  (256-bit big-endian subtract; assumes 0 < s < n)
  int borrow = 0;
  for (int i = 31; i >= 0; i--) {
    int d = (int)P256_N[i] - (int)s[i] - borrow;
    if (d < 0) { d += 256; borrow = 1; } else { borrow = 0; }
    neg[i] = (uint8_t)d;
  }
}

// 32-byte big-endian helpers for the mod-n reduction.
static int be32_ge(const uint8_t a[32], const uint8_t b[32]) {
  for (int i = 0; i < 32; i++) if (a[i] != b[i]) return a[i] > b[i];
  return 1;  // equal counts as >=
}
static void be32_sub_n(uint8_t a[32]) {  // a = (a - n) mod 2^256
  int borrow = 0;
  for (int i = 31; i >= 0; i--) {
    int d = (int)a[i] - (int)P256_N[i] - borrow;
    if (d < 0) { d += 256; borrow = 1; } else borrow = 0;
    a[i] = (uint8_t)d;
  }
}

void mtrc_ec_scalar_reduce(const uint8_t *in, size_t in_len, uint8_t out[32]) {
  // Bit-by-bit: acc = (acc << 1 | bit); if it reached >= n, subtract n once.
  // acc stays < n throughout (value before each subtract is < 2n).
  uint8_t acc[32] = {0};
  for (size_t i = 0; i < in_len * 8; i++) {
    int bit = (in[i >> 3] >> (7 - (i & 7))) & 1;
    int carry = bit;
    for (int j = 31; j >= 0; j--) { int v = ((int)acc[j] << 1) | carry; acc[j] = (uint8_t)v; carry = v >> 8; }
    if (carry) be32_sub_n(acc);                 // 257th bit set -> definitely >= n
    else if (be32_ge(acc, P256_N)) be32_sub_n(acc);
  }
  memcpy(out, acc, 32);
}

int mtrc_aes_ccm_encrypt(const uint8_t key[16],
                         const uint8_t *nonce, size_t nonce_len,
                         const uint8_t *aad, size_t aad_len,
                         uint8_t *data, size_t data_len,
                         uint8_t *tag, size_t tag_len) {
  br_aes_ct_ctrcbc_keys bc;
  br_aes_ct_ctrcbc_init(&bc, key, 16);
  br_ccm_context ctx;
  br_ccm_init(&ctx, &bc.vtable);
  if (!br_ccm_reset(&ctx, nonce, nonce_len, aad_len, data_len, tag_len)) return 0;
  if (aad_len) br_ccm_aad_inject(&ctx, aad, aad_len);
  br_ccm_flip(&ctx);
  if (data_len) br_ccm_run(&ctx, 1, data, data_len);
  br_ccm_get_tag(&ctx, tag);
  return 1;
}

int mtrc_aes_ccm_decrypt(const uint8_t key[16],
                         const uint8_t *nonce, size_t nonce_len,
                         const uint8_t *aad, size_t aad_len,
                         uint8_t *data, size_t data_len,
                         const uint8_t *tag, size_t tag_len) {
  br_aes_ct_ctrcbc_keys bc;
  br_aes_ct_ctrcbc_init(&bc, key, 16);
  br_ccm_context ctx;
  br_ccm_init(&ctx, &bc.vtable);
  if (!br_ccm_reset(&ctx, nonce, nonce_len, aad_len, data_len, tag_len)) return 0;
  if (aad_len) br_ccm_aad_inject(&ctx, aad, aad_len);
  br_ccm_flip(&ctx);
  if (data_len) br_ccm_run(&ctx, 0, data, data_len);
  return br_ccm_check_tag(&ctx, tag) ? 1 : 0;
}

int mtrc_pbkdf2_sha256(const uint8_t *pw, size_t pw_len,
                       const uint8_t *salt, size_t salt_len,
                       uint32_t iterations, uint8_t *out, size_t out_len) {
  if (salt_len > 64 || iterations == 0) return 0;
  uint8_t block[64 + 4];
  uint32_t i = 1; size_t done = 0;
  while (done < out_len) {
    memcpy(block, salt, salt_len);
    block[salt_len + 0] = (uint8_t)(i >> 24); block[salt_len + 1] = (uint8_t)(i >> 16);
    block[salt_len + 2] = (uint8_t)(i >> 8);  block[salt_len + 3] = (uint8_t)i;
    uint8_t Uin[32], Uout[32], T[32];
    mtrc_hmac_sha256(pw, pw_len, block, salt_len + 4, Uin); // U1
    memcpy(T, Uin, 32);
    for (uint32_t c = 1; c < iterations; c++) {
      mtrc_hmac_sha256(pw, pw_len, Uin, 32, Uout);          // U_{c+1} (no aliasing)
      memcpy(Uin, Uout, 32);
      for (int j = 0; j < 32; j++) T[j] ^= Uout[j];
    }
    size_t take = out_len - done; if (take > 32) take = 32;
    memcpy(out + done, T, take);
    done += take; i++;
  }
  return 1;
}
