// mtrc_pase.h — Matter PASE (Passcode-Authenticated Session Establishment).
//
// Wraps the proven SPAKE2+ (mtrc_spake2p) in the Matter commissioning
// key schedule + the PBKDFParamRequest/Response + Pake1/2/3 message TLV.
// All Matter-specific constants are taken verbatim from connectedhomeip:
//   - SPAKE2+ context = SHA256("CHIP PAKE V1 Commissioning" ||
//                               PBKDFParamRequest || PBKDFParamResponse)
//   - identities are empty in the SPAKE2+ transcript
//   - K_main = SHA256(TT); Ka = K_main[0:16], Ke = K_main[16:32]
//   - KcA||KcB = HKDF(salt=nil, IKM=Ka, "ConfirmationKeys", 32) (16 each)
//   - cA = HMAC(KcA, pB), cB = HMAC(KcB, pA)
//   - I2R||R2I||AttChallenge = HKDF(salt=nil, IKM=Ke, "SessionKeys", 48)
//   - w0,w1 = PBKDF2-HMAC-SHA256(LE32(passcode), salt, iters, 80),
//             each 40-byte half reduced mod n
//
// GPLv3. Implemented from the Matter spec + connectedhomeip constants,
// not converted from Berry.

#ifndef MTRC_PASE_H
#define MTRC_PASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- key material ------------------------------------------------------
// Derive w0,w1 from the setup passcode (1..99999998), salt, iteration count.
int  mtrc_pase_derive_w0w1(uint32_t passcode, const uint8_t *salt, size_t salt_len,
                           uint32_t iterations, uint8_t w0[32], uint8_t w1[32]);

// SPAKE2+ transcript context = SHA256(prefix || req || resp).
void mtrc_pase_context(const uint8_t *req, size_t req_len,
                       const uint8_t *resp, size_t resp_len, uint8_t ctx[32]);

// Full Matter key schedule from the shared SPAKE2+ values.
typedef struct {
  uint8_t cA[32], cB[32];       // confirmation MACs
  uint8_t i2r[16], r2i[16];     // session encryption keys
  uint8_t att[16];              // attestation challenge
} mtrc_pase_keys_t;

int  mtrc_pase_keys(const uint8_t ctx[32], const uint8_t pA[65], const uint8_t pB[65],
                    const uint8_t Z[65], const uint8_t V[65], const uint8_t w0[32],
                    mtrc_pase_keys_t *out);

// ---- message structures + TLV codecs -----------------------------------
typedef struct {
  uint8_t  initiator_random[32];
  uint16_t initiator_session_id;
  uint16_t passcode_id;          // 0 for the standard commissioning passcode
  bool     has_pbkdf_parameters;
} mtrc_pase_param_req;

typedef struct {
  uint8_t  initiator_random[32];
  uint8_t  responder_random[32];
  uint16_t responder_session_id;
  uint32_t iterations;
  uint8_t  salt[32];
  uint8_t  salt_len;
} mtrc_pase_param_resp;

// Each returns encoded length, or -1. Decoders return 1/0.
int mtrc_pase_encode_param_req (uint8_t *out, size_t cap, const mtrc_pase_param_req *r);
int mtrc_pase_decode_param_req (const uint8_t *in, size_t len, mtrc_pase_param_req *r);
int mtrc_pase_encode_param_resp(uint8_t *out, size_t cap, const mtrc_pase_param_resp *r);
int mtrc_pase_decode_param_resp(const uint8_t *in, size_t len, mtrc_pase_param_resp *r);

int mtrc_pase_encode_pake1(uint8_t *out, size_t cap, const uint8_t pA[65]);
int mtrc_pase_decode_pake1(const uint8_t *in, size_t len, uint8_t pA[65]);
int mtrc_pase_encode_pake2(uint8_t *out, size_t cap, const uint8_t pB[65], const uint8_t cB[32]);
int mtrc_pase_decode_pake2(const uint8_t *in, size_t len, uint8_t pB[65], uint8_t cB[32]);
int mtrc_pase_encode_pake3(uint8_t *out, size_t cap, const uint8_t cA[32]);
int mtrc_pase_decode_pake3(const uint8_t *in, size_t len, uint8_t cA[32]);

#ifdef __cplusplus
}
#endif

#endif // MTRC_PASE_H
