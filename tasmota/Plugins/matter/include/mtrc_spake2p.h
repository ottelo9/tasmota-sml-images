// mtrc_spake2p.h — SPAKE2+ (RFC 9383, P256-SHA256-HKDF-HMAC) for matter_c.
//
// This is the augmented-PAKE used by Matter PASE commissioning. The math
// is implemented from RFC 9383 + the Matter 1.4.1 Core spec (§3.10), on
// top of BearSSL EC (mtrc_crypto). Not converted from any Berry source.
//
// Roles:
//   Prover   (Matter Commissioner): knows passcode -> w0, w1
//   Verifier (Matter Commissionee/device): stores w0 and L = w1*G
//
// Exchange:
//   Prover   X = x*G + w0*M
//   Verifier Y = y*G + w0*N
//   Prover   Z = x*(Y - w0*N),  V = w1*(Y - w0*N)
//   Verifier Z = y*(X - w0*M),  V = y*L
//   Both     TT = transcript;  K_main = SHA256(TT)
//            K_confirmP||K_confirmV = HKDF(nil, K_main, "ConfirmationKeys")
//            cA = HMAC(K_confirmP, Y),  cB = HMAC(K_confirmV, X)
//
// GPLv3.

#ifndef MTRC_SPAKE2P_H
#define MTRC_SPAKE2P_H

#include <stdint.h>
#include <stddef.h>
#include "mtrc_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

// Prover step: compute X = x*G + w0*M.  w0,x are 32-byte BE scalars.
int mtrc_spake2p_prover_X(const uint8_t w0[32], const uint8_t x[32],
                          uint8_t X[65]);

// Verifier step: compute Y = y*G + w0*N.
int mtrc_spake2p_verifier_Y(const uint8_t w0[32], const uint8_t y[32],
                            uint8_t Y[65]);

// Prover shared values from peer Y: Z = x*(Y - w0*N), V = w1*(Y - w0*N).
int mtrc_spake2p_prover_ZV(const uint8_t w0[32], const uint8_t w1[32],
                           const uint8_t x[32], const uint8_t Y[65],
                           uint8_t Z[65], uint8_t V[65]);

// Verifier shared values from peer X: Z = y*(X - w0*M), V = y*L.
int mtrc_spake2p_verifier_ZV(const uint8_t w0[32], const uint8_t y[32],
                             const uint8_t X[65], const uint8_t L[65],
                             uint8_t Z[65], uint8_t V[65]);

// Build transcript TT and derive K_main = SHA256(TT).
// idProver/idVerifier may be NULL (length 0). Points are 65-byte uncompressed.
int mtrc_spake2p_transcript(const uint8_t *context, size_t context_len,
                            const uint8_t *idProver, size_t idProver_len,
                            const uint8_t *idVerifier, size_t idVerifier_len,
                            const uint8_t X[65], const uint8_t Y[65],
                            const uint8_t Z[65], const uint8_t V[65],
                            const uint8_t w0[32],
                            uint8_t K_main[32]);

// From K_main derive confirmation keys + MACs.
//   K_confirmP || K_confirmV = HKDF(nil, K_main, "ConfirmationKeys")
//   cA = HMAC(K_confirmP, Y),  cB = HMAC(K_confirmV, X)
int mtrc_spake2p_confirm(const uint8_t K_main[32],
                         const uint8_t X[65], const uint8_t Y[65],
                         uint8_t K_confirmP[32], uint8_t K_confirmV[32],
                         uint8_t cA[32], uint8_t cB[32]);

#ifdef __cplusplus
}
#endif

#endif // MTRC_SPAKE2P_H
