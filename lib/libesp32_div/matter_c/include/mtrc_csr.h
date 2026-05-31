// mtrc_csr.h — PKCS#10 Certificate Signing Request (DER) for matter_c.
//
// Phase A3: the device's CSRRequest response carries a standard PKCS#10 CSR
// for its freshly generated *operational* keypair (Matter Core Spec §6.4.6 /
// cluster 0x003E CSRRequest). The commissioner's CA then issues the NOC over
// the public key in this CSR. The CSR is self-signed with the operational
// private key (proof of possession).
//
// This module builds the DER (X.690) by hand — no external ASN.1 library —
// using BearSSL only for SHA-256 + ECDSA (via mtrc_crypto). P-256 keys only
// (prime256v1 / id-ecPublicKey, ecdsa-with-SHA256). Host-testable: the output
// is verified by `openssl`/`cryptography` in test/build_csr.sh.
//
// GPLv3. Implemented from RFC 2986 (PKCS#10) + RFC 5480 (EC SPKI).

#ifndef MTRC_CSR_H
#define MTRC_CSR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Build a PKCS#10 CSR (DER) for the operational keypair into out[cap].
// op_pub is the uncompressed P-256 public key (0x04||X||Y, 65 bytes) matching
// op_priv (32-byte big-endian scalar). Returns the DER length, or -1 on error
// (signing failed / output too small).
int mtrc_csr_build(uint8_t *out, size_t cap,
                   const uint8_t op_priv[32], const uint8_t op_pub[65]);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CSR_H
