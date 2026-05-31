// mtrc_spake2p.c — SPAKE2+ (RFC 9383, P256) implementation. See header.
// Implemented from RFC 9383 + Matter Core spec; crypto via BearSSL. GPLv3.

#include "mtrc_spake2p.h"
#include <string.h>

// SPAKE2+ fixed points M and N for P-256 (RFC 9383), uncompressed form.
static const uint8_t SPAKE_M[65] = {
  0x04,
  0x88,0x6e,0x2f,0x97,0xac,0xe4,0x6e,0x55,0xba,0x9d,0xd7,0x24,0x25,0x79,0xf2,0x99,
  0x3b,0x64,0xe1,0x6e,0xf3,0xdc,0xab,0x95,0xaf,0xd4,0x97,0x33,0x3d,0x8f,0xa1,0x2f,
  0x5f,0xf3,0x55,0x16,0x3e,0x43,0xce,0x22,0x4e,0x0b,0x0e,0x65,0xff,0x02,0xac,0x8e,
  0x5c,0x7b,0xe0,0x94,0x19,0xc7,0x85,0xe0,0xca,0x54,0x7d,0x55,0xa1,0x2e,0x2d,0x20
};
static const uint8_t SPAKE_N[65] = {
  0x04,
  0xd8,0xbb,0xd6,0xc6,0x39,0xc6,0x29,0x37,0xb0,0x4d,0x99,0x7f,0x38,0xc3,0x77,0x07,
  0x19,0xc6,0x29,0xd7,0x01,0x4d,0x49,0xa2,0x4b,0x4f,0x98,0xba,0xa1,0x29,0x2b,0x49,
  0x07,0xd6,0x0a,0xa6,0xbf,0xad,0xe4,0x50,0x08,0xa6,0x36,0x33,0x7f,0x51,0x68,0xc6,
  0x4d,0x9b,0xd3,0x60,0x34,0x80,0x8c,0xd5,0x64,0x49,0x0b,0x1e,0x65,0x6e,0xdb,0xe7
};

static const uint8_t SCALAR_ONE[1] = { 0x01 };

int mtrc_spake2p_prover_X(const uint8_t w0[32], const uint8_t x[32],
                          uint8_t X[65]) {
  // X = w0*M + x*G   (muladd: A=M, a=w0; B=NULL=generator, b=x)
  memcpy(X, SPAKE_M, 65);
  return mtrc_ec_muladd(X, NULL, w0, 32, x, 32);
}

int mtrc_spake2p_verifier_Y(const uint8_t w0[32], const uint8_t y[32],
                            uint8_t Y[65]) {
  // Y = w0*N + y*G
  memcpy(Y, SPAKE_N, 65);
  return mtrc_ec_muladd(Y, NULL, w0, 32, y, 32);
}

int mtrc_spake2p_prover_ZV(const uint8_t w0[32], const uint8_t w1[32],
                           const uint8_t x[32], const uint8_t Y[65],
                           uint8_t Z[65], uint8_t V[65]) {
  // T = Y - w0*N = 1*Y + (n-w0)*N   (muladd: A=Y, a=1; B=N, b=neg_w0)
  uint8_t neg_w0[32], T[65];
  mtrc_ec_scalar_neg(w0, neg_w0);
  memcpy(T, Y, 65);
  if (!mtrc_ec_muladd(T, SPAKE_N, SCALAR_ONE, 1, neg_w0, 32)) return 0;
  // Z = x*T,  V = w1*T
  memcpy(Z, T, 65);
  if (!mtrc_ec_mul(Z, x, 32)) return 0;
  memcpy(V, T, 65);
  if (!mtrc_ec_mul(V, w1, 32)) return 0;
  return 1;
}

int mtrc_spake2p_verifier_ZV(const uint8_t w0[32], const uint8_t y[32],
                             const uint8_t X[65], const uint8_t L[65],
                             uint8_t Z[65], uint8_t V[65]) {
  // T = X - w0*M = 1*X + (n-w0)*M
  uint8_t neg_w0[32], T[65];
  mtrc_ec_scalar_neg(w0, neg_w0);
  memcpy(T, X, 65);
  if (!mtrc_ec_muladd(T, SPAKE_M, SCALAR_ONE, 1, neg_w0, 32)) return 0;
  // Z = y*T,  V = y*L
  memcpy(Z, T, 65);
  if (!mtrc_ec_mul(Z, y, 32)) return 0;
  memcpy(V, L, 65);
  if (!mtrc_ec_mul(V, y, 32)) return 0;
  return 1;
}

// Append an 8-byte little-endian length then the value, into buf at *off.
static void tt_put(uint8_t *buf, size_t *off, const uint8_t *val, size_t len) {
  uint64_t l = (uint64_t)len;
  for (int i = 0; i < 8; i++) buf[(*off)++] = (uint8_t)(l >> (8 * i));
  if (len) { memcpy(buf + *off, val, len); *off += len; }
}

int mtrc_spake2p_transcript(const uint8_t *context, size_t context_len,
                            const uint8_t *idProver, size_t idProver_len,
                            const uint8_t *idVerifier, size_t idVerifier_len,
                            const uint8_t X[65], const uint8_t Y[65],
                            const uint8_t Z[65], const uint8_t V[65],
                            const uint8_t w0[32],
                            uint8_t K_main[32]) {
  // TT = (len||val) for: Context, idProver, idVerifier, M, N, X, Y, Z, V, w0
  // Sized for the RFC 9383 layout: 10 length prefixes + payloads.
  static uint8_t tt[8 + 256 + 8 + 64 + 8 + 64 + 8*7 + 65*6 + 32 + 64];
  size_t off = 0;
  if (context_len > 256 || idProver_len > 64 || idVerifier_len > 64) return 0;
  tt_put(tt, &off, context, context_len);
  tt_put(tt, &off, idProver, idProver_len);
  tt_put(tt, &off, idVerifier, idVerifier_len);
  tt_put(tt, &off, SPAKE_M, 65);
  tt_put(tt, &off, SPAKE_N, 65);
  tt_put(tt, &off, X, 65);
  tt_put(tt, &off, Y, 65);
  tt_put(tt, &off, Z, 65);
  tt_put(tt, &off, V, 65);
  tt_put(tt, &off, w0, 32);
  mtrc_sha256(tt, off, K_main);
  return 1;
}

int mtrc_spake2p_confirm(const uint8_t K_main[32],
                         const uint8_t X[65], const uint8_t Y[65],
                         uint8_t K_confirmP[32], uint8_t K_confirmV[32],
                         uint8_t cA[32], uint8_t cB[32]) {
  // K_confirmP || K_confirmV = HKDF(nil, K_main, "ConfirmationKeys", 64)
  static const uint8_t info[] = "ConfirmationKeys";
  uint8_t okm[64];
  if (!mtrc_hkdf_sha256(NULL, 0, K_main, 32,
                        info, sizeof(info) - 1, okm, 64)) return 0;
  memcpy(K_confirmP, okm, 32);
  memcpy(K_confirmV, okm + 32, 32);
  // cA = HMAC(K_confirmP, Y),  cB = HMAC(K_confirmV, X)
  mtrc_hmac_sha256(K_confirmP, 32, Y, 65, cA);
  mtrc_hmac_sha256(K_confirmV, 32, X, 65, cB);
  return 1;
}
