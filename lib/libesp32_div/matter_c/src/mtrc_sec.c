// mtrc_sec.c — Matter secured message layer (AES-CCM). See mtrc_sec.h.
// GPLv3.

#include "mtrc_sec.h"
#include "mtrc_crypto.h"
#include <string.h>

#ifndef MTRC_SEC_MAX_BODY
#define MTRC_SEC_MAX_BODY 1280
#endif

void mtrc_sec_nonce(uint8_t nonce[13], uint8_t security_flags,
                    uint32_t msg_counter, uint64_t src_node_id) {
  nonce[0] = security_flags;
  for (int i = 0; i < 4; i++) nonce[1 + i] = (uint8_t)(msg_counter >> (8 * i));
  for (int i = 0; i < 8; i++) nonce[5 + i] = (uint8_t)(src_node_id >> (8 * i));
}

int mtrc_sec_encode(uint8_t *out, size_t cap,
                    const mtrc_msg_header *mh, const mtrc_proto_header *ph,
                    const uint8_t *payload, size_t payload_len,
                    const uint8_t key[16]) {
  if (!out || !mh || !ph || !key) return -1;

  // message header -> AAD, written first into the output
  int hl = mtrc_frame_encode_msg_header(out, cap, mh);
  if (hl < 0) return -1;

  // protocol header + payload -> the plaintext to encrypt
  uint8_t body[MTRC_SEC_MAX_BODY];
  int bl = mtrc_frame_encode_proto(body, sizeof(body), ph, payload, payload_len);
  if (bl < 0) return -1;

  if ((size_t)hl + (size_t)bl + MTRC_CCM_TAG_LEN > cap) return -1;
  memcpy(out + hl, body, (size_t)bl);            // encrypt in place

  uint8_t nonce[MTRC_CCM_NONCE_LEN];
  mtrc_sec_nonce(nonce, mtrc_frame_security_flags(mh), mh->msg_counter, mh->src_node_id);

  uint8_t tag[MTRC_CCM_TAG_LEN];
  if (!mtrc_aes_ccm_encrypt(key, nonce, MTRC_CCM_NONCE_LEN,
                            out, (size_t)hl,            // AAD = message header
                            out + hl, (size_t)bl,       // in-place ciphertext
                            tag, MTRC_CCM_TAG_LEN)) return -1;
  memcpy(out + hl + bl, tag, MTRC_CCM_TAG_LEN);
  return hl + bl + MTRC_CCM_TAG_LEN;
}

int mtrc_sec_decode(const uint8_t *in, size_t len, const uint8_t key[16],
                    uint64_t peer_node_id,
                    mtrc_msg_header *mh, mtrc_proto_header *ph,
                    uint8_t *pt_buf, size_t pt_cap,
                    const uint8_t **payload, size_t *payload_len) {
  if (!in || !mh || !ph || !key || !pt_buf) return 0;

  int hl = mtrc_frame_decode_msg_header(in, len, mh);
  if (hl < 0) return 0;
  if (len < (size_t)hl + MTRC_CCM_TAG_LEN) return 0;

  size_t ct_len = len - (size_t)hl - MTRC_CCM_TAG_LEN;
  if (ct_len > pt_cap) return 0;
  memcpy(pt_buf, in + hl, ct_len);                 // ciphertext -> decrypt in place
  const uint8_t *tag = in + hl + ct_len;

  // Nonce source node id = the sender's node id. When the header carries it use
  // that; otherwise (operational CASE omits it) fall back to the session peer.
  uint64_t nonce_src = mh->has_src ? mh->src_node_id : peer_node_id;
  uint8_t nonce[MTRC_CCM_NONCE_LEN];
  mtrc_sec_nonce(nonce, mtrc_frame_security_flags(mh), mh->msg_counter, nonce_src);

  if (!mtrc_aes_ccm_decrypt(key, nonce, MTRC_CCM_NONCE_LEN,
                            in, (size_t)hl,             // AAD = message header
                            pt_buf, ct_len,
                            tag, MTRC_CCM_TAG_LEN)) return 0;   // MIC failed

  return mtrc_frame_decode_proto(pt_buf, ct_len, ph, payload, payload_len);
}
