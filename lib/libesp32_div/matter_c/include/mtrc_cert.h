// mtrc_cert.h — Matter operational certificate (compact-TLV) codec.
//
// Matter certs (Core Spec §6.5) are TLV, not X.509 DER. Top-level is an
// anonymous structure with elements:
//   1:serialNumber(bytes) 2:sigAlgo(u) 3:issuer(list-DN) 4:notBefore(u)
//   5:notAfter(u) 6:subject(list-DN) 7:pubKeyAlgo(u) 8:curveId(u)
//   9:ecPublicKey(bytes65) 10:extensions(list) 11:ecdsaSignature(bytes64)
// DN list attributes (context tags): commonName=1 ... matterNodeId=17,
// matterICACId=19, matterRCACId=20, matterFabricId=21, matterNOCCAT=22.
// Extensions: basicConstraints=1{ isCA=1(bool) }, keyUsage=2(u).
//
// This module PARSES a cert and extracts the fields CASE/AddNOC need
// (subject node-id/fabric-id, public key, signature, issuer ids, is-CA).
// Full X.509-DER-TBS chain-signature verification is a separate step
// (mtrc_cert_verify, A1b).
//
// GPLv3. Implemented from the Matter spec + connectedhomeip tag values.

#ifndef MTRC_CERT_H
#define MTRC_CERT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t  serial[20];  uint8_t serial_len;
  uint32_t not_before, not_after;        // Matter epoch (2000-01-01) seconds

  // subject DN matter ids
  uint64_t subject_node_id;   bool have_node_id;     // NOC
  uint64_t subject_fabric_id; bool have_fabric_id;
  uint64_t subject_rcac_id;   bool have_rcac_id;     // RCAC self-subject
  uint64_t subject_icac_id;   bool have_icac_id;     // ICAC self-subject
  // issuer DN matter ids (for chain matching)
  uint64_t issuer_rcac_id;    bool issuer_has_rcac;
  uint64_t issuer_icac_id;    bool issuer_has_icac;

  uint8_t  pubkey[65];        bool have_pubkey;       // ec-public-key (uncompressed)
  uint8_t  signature[64];     bool have_sig;          // ECDSA r||s
  bool     is_ca;                                     // basicConstraints.isCA
} mtrc_cert;

// Parse a Matter-TLV certificate. Returns 1 on success.
int mtrc_cert_parse(const uint8_t *tlv, size_t len, mtrc_cert *out);

// Structural (non-cryptographic) operational-cert chain check — partial A1b.
// Validates the relationships a valid initiator NOC[/ICAC] chain must hold and
// that the NOC is bound to the fabric the CASE handshake matched, WITHOUT the
// X.509-DER signature-chain crypto (the remaining A1b step). The fabric root is
// already bound by Sigma1's destinationId = HMAC(IPK, ...||RootPubKey||
// FabricId||NodeId), so this adds fabric/role/chain-consistency hardening on top
// (catches wrong-fabric NOCs, a non-CA ICAC, a NOC posing as a CA, an ICAC that
// contradicts the NOC's issuer id, and — when a clock is available — expiry).
// Conservative by design: it rejects only positive contradictions, so it can't
// reject a well-formed NOC.
//   noc       : parsed initiator NOC (required)
//   icac      : parsed initiator ICAC, or NULL if the chain has none
//   fabric_id : fabric the handshake matched; 0 = caller doesn't know it (skip)
//   now_epoch : current time in Matter epoch (2000-01-01) seconds, or 0 =
//               unknown -> skip the validity-window check
// Returns 1 if the chain is structurally acceptable, 0 to reject the session.
int mtrc_cert_chain_check(const mtrc_cert *noc, const mtrc_cert *icac,
                          uint64_t fabric_id, uint32_t now_epoch);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CERT_H
