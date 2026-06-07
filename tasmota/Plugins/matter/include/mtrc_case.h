// mtrc_case.h — Matter CASE key schedule for matter_c.
//
// CASE (Certificate Authenticated Session Establishment, Core Spec
// §4.13.2) establishes the OPERATIONAL secure session between two
// commissioned nodes using their node operational certificates (NOC).
// It is a SIGMA-I exchange (Sigma1/2/3) over P-256 ECDH, authenticated
// by ECDSA over the NOC keys, with the operational Identity Protection
// Key (IPK) mixed into every derivation.
//
// This module implements the *key schedule* — the pure-crypto assembly,
// validated independently of the message/cert plumbing:
//   destinationId = HMAC(IPK, initRandom || rootPubKey || feLE(fabricId)
//                         || feLE(nodeId))
//   shared        = ECDH(initEph, respEph)                    [mtrc_ecdh]
//   S2K = HKDF(shared, salt=IPK||respRandom||respEphPub||SHA256(Σ1),
//             "Sigma2", 16)
//   S3K = HKDF(shared, salt=IPK||SHA256(Σ1||Σ2), "Sigma3", 16)
//   I2R||R2I||Att = HKDF(shared, salt=IPK||SHA256(Σ1||Σ2||Σ3),
//                        "SessionKeys", 48)
// TBE encryption nonces are the fixed strings NCASE_Sigma2N / NCASE_Sigma3N.
//
// The Sigma message TLV + operational-cert (compact-TLV NOC) chain
// handling come with the live integration step. Constants are taken
// verbatim from connectedhomeip. GPLv3.

#ifndef MTRC_CASE_H
#define MTRC_CASE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MTRC_IPK_LEN 16

// Fixed AES-CCM nonces for the Sigma2/Sigma3 TBE blobs (13 bytes each).
extern const uint8_t MTRC_CASE_NONCE_SIGMA2[13];   // "NCASE_Sigma2N"
extern const uint8_t MTRC_CASE_NONCE_SIGMA3[13];   // "NCASE_Sigma3N"

// destinationId (32 bytes) = HMAC-SHA256(IPK, initRandom || rootPubKey ||
// LE64(fabricId) || LE64(nodeId)). rootPubKey is 65-byte uncompressed.
void mtrc_case_destination_id(const uint8_t ipk[16],
                              const uint8_t init_random[32],
                              const uint8_t root_pub[65],
                              uint64_t fabric_id, uint64_t node_id,
                              uint8_t out[32]);

// S2K (16 bytes) from the ECDH shared secret + Sigma1 transcript hash.
int mtrc_case_s2k(const uint8_t shared[32], const uint8_t ipk[16],
                  const uint8_t resp_random[32], const uint8_t resp_eph_pub[65],
                  const uint8_t hash_sigma1[32], uint8_t s2k[16]);

// S3K (16 bytes) from the ECDH shared secret + Sigma1||Sigma2 transcript hash.
int mtrc_case_s3k(const uint8_t shared[32], const uint8_t ipk[16],
                  const uint8_t hash_sigma12[32], uint8_t s3k[16]);

// Operational session keys from the full Sigma1||Sigma2||Sigma3 transcript.
int mtrc_case_session_keys(const uint8_t shared[32], const uint8_t ipk[16],
                           const uint8_t hash_all[32],
                           uint8_t i2r[16], uint8_t r2i[16], uint8_t att[16]);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CASE_H
