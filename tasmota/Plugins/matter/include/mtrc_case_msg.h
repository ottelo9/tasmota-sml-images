// mtrc_case_msg.h — Matter CASE Sigma message TLV codecs for matter_c.
//
// The wire structures of the CASE handshake (Core Spec §4.13.2), with the
// context tags taken verbatim from connectedhomeip CASESession:
//
//   Sigma1{1:initRandom,2:initSessionId,3:destinationId,4:initEphPubKey}
//          (+ optional 5 sessionParams, 6 resumptionID, 7 resume1MIC — omitted)
//   Sigma2{1:respRandom,2:respSessionId,3:respEphPubKey,4:encrypted2}
//   Sigma3{1:encrypted3}
//   TBEData{1:NOC,2:ICAC?,3:signature,4:resumptionID?}   (encrypted blob body)
//   TBSData{1:NOC,2:ICAC?,3:senderPubKey,4:receiverPubKey} (the signed body)
//
// encrypted2/encrypted3 are opaque AES-CCM blobs at this layer (sealed with
// the Sigma2/Sigma3 keys + NCASE_SigmaN nonces from mtrc_case/mtrc_sec).
// NOC/ICAC are opaque octet strings here — the compact-TLV operational
// certificate format + chain verification are a separate piece.
//
// GPLv3. Implemented from the Matter spec + connectedhomeip tags.

#ifndef MTRC_CASE_MSG_H
#define MTRC_CASE_MSG_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t  initiator_random[32];
  uint16_t initiator_session_id;
  uint8_t  destination_id[32];
  uint8_t  initiator_eph_pub[65];
} mtrc_sigma1;

typedef struct {
  uint8_t        responder_random[32];
  uint16_t       responder_session_id;
  uint8_t        responder_eph_pub[65];
  const uint8_t *encrypted2;        // opaque CCM blob (TBEData2 || MIC)
  size_t         encrypted2_len;
} mtrc_sigma2;

typedef struct {
  const uint8_t *encrypted3;        // opaque CCM blob (TBEData3 || MIC)
  size_t         encrypted3_len;
} mtrc_sigma3;

// Inner encrypted body (TBEData): NOC + optional ICAC + signature + optional
// resumptionID. icac_len/resumption_id_len == 0 omits that field.
typedef struct {
  const uint8_t *noc;            size_t noc_len;
  const uint8_t *icac;           size_t icac_len;
  uint8_t        signature[64];
  const uint8_t *resumption_id;  size_t resumption_id_len;
} mtrc_case_tbe;

// Signed body (TBSData): NOC + optional ICAC + sender/receiver eph pubkeys.
typedef struct {
  const uint8_t *noc;   size_t noc_len;
  const uint8_t *icac;  size_t icac_len;
  uint8_t        sender_pub[65];
  uint8_t        receiver_pub[65];
} mtrc_case_tbs;

// Encoders return length (or -1); decoders return 1/0. Decoded variable
// fields (encrypted blobs, NOC/ICAC) point into the input buffer.
int mtrc_sigma1_encode(uint8_t *out, size_t cap, const mtrc_sigma1 *s);
int mtrc_sigma1_decode(const uint8_t *in, size_t len, mtrc_sigma1 *s);
int mtrc_sigma2_encode(uint8_t *out, size_t cap, const mtrc_sigma2 *s);
int mtrc_sigma2_decode(const uint8_t *in, size_t len, mtrc_sigma2 *s);
int mtrc_sigma3_encode(uint8_t *out, size_t cap, const mtrc_sigma3 *s);
int mtrc_sigma3_decode(const uint8_t *in, size_t len, mtrc_sigma3 *s);

int mtrc_case_tbe_encode(uint8_t *out, size_t cap, const mtrc_case_tbe *t);
int mtrc_case_tbe_decode(const uint8_t *in, size_t len, mtrc_case_tbe *t);
int mtrc_case_tbs_encode(uint8_t *out, size_t cap, const mtrc_case_tbs *t);

#ifdef __cplusplus
}
#endif

#endif // MTRC_CASE_MSG_H
