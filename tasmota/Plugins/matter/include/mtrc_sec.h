// mtrc_sec.h — Matter secured (encrypted) message layer for matter_c.
//
// Wraps a message in AES-128-CCM (Core Spec §4.7):
//   wire = MessageHeader (plaintext, used as AAD)
//        || AES-CCM-encrypt( key, nonce, aad=MessageHeader,
//                            plaintext = ProtocolHeader || AppPayload )
//        || MIC (16 bytes)
//   nonce(13) = SecurityFlags(1) || MessageCounter(4 LE) || SourceNodeID(8 LE)
//   key = I2RKey or R2IKey from the PASE/CASE session (direction-dependent)
//
// Privacy obfuscation (§4.8) is optional in Matter and not applied here;
// PASE/CASE and basic operation do not require it.
//
// GPLv3. Implemented from the Matter spec + connectedhomeip nonce/AAD
// construction, not converted from Berry.

#ifndef MTRC_SEC_H
#define MTRC_SEC_H

#include <stdint.h>
#include <stddef.h>
#include "mtrc_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

// Build the 13-byte Matter CCM nonce.
void mtrc_sec_nonce(uint8_t nonce[13], uint8_t security_flags,
                    uint32_t msg_counter, uint64_t src_node_id);

// Encode a secured message: header(AAD) || CCM-ciphertext || 16-byte MIC.
// `key` is the 16-byte session key for the sending direction. The CCM
// nonce is derived from mh (security flags, counter, source node id).
// Returns total length, or -1.
int mtrc_sec_encode(uint8_t *out, size_t cap,
                    const mtrc_msg_header *mh, const mtrc_proto_header *ph,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[16]);

// Decode + authenticate a secured message. Parses the header, decrypts the
// remainder into pt_buf, verifies the MIC, then parses the protocol header.
// On success fills mh/ph and points *payload into pt_buf. Returns 1, or 0
// on malformed input / authentication failure.
//   peer_node_id: the SENDER's operational node id, used in the decrypt nonce
//   when the message header omits the source node id (S flag clear) — operational
//   CASE messages do this. Pass 0 for PASE / commissioning sessions.
int mtrc_sec_decode(const uint8_t *in, size_t len, const uint8_t key[16],
                    uint64_t peer_node_id,
                    mtrc_msg_header *mh, mtrc_proto_header *ph,
                    uint8_t *pt_buf, size_t pt_cap,
                    const uint8_t **payload, size_t *payload_len);

#ifdef __cplusplus
}
#endif

#endif // MTRC_SEC_H
