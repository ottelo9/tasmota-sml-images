// mtrc_pase.c — Matter PASE key schedule + message TLV. See mtrc_pase.h.
// GPLv3. Matter constants verbatim from connectedhomeip.

#include "mtrc_pase.h"
#include "mtrc_crypto.h"
#include "mtrc_spake2p.h"
#include "mtrc_tlv.h"
#include <string.h>

static const char SPAKE_CTX_PREFIX[] = "CHIP PAKE V1 Commissioning";
static const uint8_t INFO_CONFIRM[]  = { 'C','o','n','f','i','r','m','a','t','i','o','n','K','e','y','s' };
static const uint8_t INFO_SESSION[]  = { 'S','e','s','s','i','o','n','K','e','y','s' };
#define SPAKE_WS 40   // kSpake2p_WS_Length = kP256_FE_Length(32) + 8

int mtrc_pase_derive_w0w1(uint32_t passcode, const uint8_t *salt, size_t salt_len,
                          uint32_t iterations, uint8_t w0[32], uint8_t w1[32]) {
  uint8_t pin[4] = { (uint8_t)passcode, (uint8_t)(passcode >> 8),
                     (uint8_t)(passcode >> 16), (uint8_t)(passcode >> 24) }; // LE
  uint8_t ws[2 * SPAKE_WS];
  if (!mtrc_pbkdf2_sha256(pin, 4, salt, salt_len, iterations, ws, sizeof(ws))) return 0;
  mtrc_ec_scalar_reduce(ws,            SPAKE_WS, w0);   // first 40 bytes mod n
  mtrc_ec_scalar_reduce(ws + SPAKE_WS, SPAKE_WS, w1);   // next 40 bytes  mod n
  return 1;
}

void mtrc_pase_context(const uint8_t *req, size_t req_len,
                       const uint8_t *resp, size_t resp_len, uint8_t ctx[32]) {
  // ctx = SHA256(prefix || req || resp). Streamed via HMAC-less SHA: build
  // one buffer (small messages) — keeps mtrc_crypto's one-shot hash API.
  static uint8_t tmp[1024];
  size_t off = 0, pfx = sizeof(SPAKE_CTX_PREFIX) - 1;
  if (pfx + req_len + resp_len > sizeof(tmp)) { memset(ctx, 0, 32); return; }
  memcpy(tmp + off, SPAKE_CTX_PREFIX, pfx); off += pfx;
  memcpy(tmp + off, req,  req_len);  off += req_len;
  memcpy(tmp + off, resp, resp_len); off += resp_len;
  mtrc_sha256(tmp, off, ctx);
}

int mtrc_pase_keys(const uint8_t ctx[32], const uint8_t pA[65], const uint8_t pB[65],
                   const uint8_t Z[65], const uint8_t V[65], const uint8_t w0[32],
                   mtrc_pase_keys_t *out) {
  uint8_t K_main[32];
  if (!mtrc_spake2p_transcript(ctx, 32, NULL, 0, NULL, 0,
                               pA, pB, Z, V, w0, K_main)) return 0;
  const uint8_t *Ka = K_main;       // [0:16]
  const uint8_t *Ke = K_main + 16;  // [16:32]

  // KcA || KcB = HKDF(salt=nil, IKM=Ka, "ConfirmationKeys", 32)
  uint8_t kcab[32];
  mtrc_hkdf_sha256(NULL, 0, Ka, 16, INFO_CONFIRM, sizeof(INFO_CONFIRM), kcab, 32);
  mtrc_hmac_sha256(kcab,      16, pB, 65, out->cA);   // cA = HMAC(KcA, pB)
  mtrc_hmac_sha256(kcab + 16, 16, pA, 65, out->cB);   // cB = HMAC(KcB, pA)

  // I2R || R2I || AttChallenge = HKDF(salt=nil, IKM=Ke, "SessionKeys", 48)
  uint8_t sek[48];
  mtrc_hkdf_sha256(NULL, 0, Ke, 16, INFO_SESSION, sizeof(INFO_SESSION), sek, 48);
  memcpy(out->i2r, sek,      16);
  memcpy(out->r2i, sek + 16, 16);
  memcpy(out->att, sek + 32, 16);
  return 1;
}

// ---- message TLV codecs (anonymous outer struct, context tags) ---------
int mtrc_pase_encode_param_req(uint8_t *out, size_t cap, const mtrc_pase_param_req *r) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), r->initiator_random, 32);
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(2), r->initiator_session_id);
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(3), r->passcode_id);
  mtrc_tlv_put_bool (&w, mtrc_tlv_ctx(4), r->has_pbkdf_parameters);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_pase_decode_param_req(const uint8_t *in, size_t len, mtrc_pase_param_req *r) {
  mtrc_tlv_reader rd; mtrc_tlv_reader_init(&rd, in, len);
  mtrc_tlv_elem e;
  memset(r, 0, sizeof(*r));
  if (!mtrc_tlv_read(&rd, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  // Depth-tracked walk: only top-level (depth==1) fields are this struct's
  // members. Field 5 (initiatorSessionParams) is a nested struct carrying MRP
  // params — its inner tag 2 (SESSION_ACTIVE_INTERVAL, default 300) must NOT be
  // mistaken for initiatorSessionId. Skip the contents of any nested container.
  int depth = 1;
  while (depth > 0 && mtrc_tlv_read(&rd, &e)) {
    if (e.type == MTRC_TLV_END) { depth--; continue; }
    int is_container = (e.type==MTRC_TLV_STRUCT || e.type==MTRC_TLV_ARRAY || e.type==MTRC_TLV_LIST);
    if (depth == 1) {
      switch (e.tag.number) {
        case 1: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(r->initiator_random, e.bytes, 32); break;
        case 2: r->initiator_session_id = (uint16_t)e.u; break;
        case 3: r->passcode_id = (uint16_t)e.u; break;
        case 4: r->has_pbkdf_parameters = (e.u != 0); break;
        default: break;
      }
    }
    if (is_container) depth++;
  }
  return !rd.err;
}

int mtrc_pase_encode_param_resp(uint8_t *out, size_t cap, const mtrc_pase_param_resp *r) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), r->initiator_random, 32);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), r->responder_random, 32);
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(3), r->responder_session_id);
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(4));          // pbkdf_parameters
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(1), r->iterations);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), r->salt, r->salt_len);
  mtrc_tlv_end_container(&w);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_pase_decode_param_resp(const uint8_t *in, size_t len, mtrc_pase_param_resp *r) {
  mtrc_tlv_reader rd; mtrc_tlv_reader_init(&rd, in, len);
  mtrc_tlv_elem e;
  memset(r, 0, sizeof(*r));
  if (!mtrc_tlv_read(&rd, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  int depth = 1;
  while (depth > 0 && mtrc_tlv_read(&rd, &e)) {
    if (e.type == MTRC_TLV_END) { depth--; continue; }
    if (depth == 1) {
      switch (e.tag.number) {
        case 1: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(r->initiator_random, e.bytes, 32); break;
        case 2: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(r->responder_random, e.bytes, 32); break;
        case 3: r->responder_session_id = (uint16_t)e.u; break;
        case 4: if (e.type==MTRC_TLV_STRUCT) depth++; break;   // enter pbkdf_parameters
        default: break;
      }
    } else { // inside pbkdf_parameters
      switch (e.tag.number) {
        case 1: r->iterations = (uint32_t)e.u; break;
        case 2: if (e.type==MTRC_TLV_BYTES && e.bytes_len<=sizeof(r->salt)) {
                  memcpy(r->salt, e.bytes, e.bytes_len); r->salt_len = (uint8_t)e.bytes_len; } break;
        default: break;
      }
    }
  }
  return !rd.err;
}

static int enc_one_bytes(uint8_t *out, size_t cap, uint8_t tag, const uint8_t *d, size_t n) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(tag), d, n);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}
static int dec_field_bytes(const uint8_t *in, size_t len, uint8_t tag, uint8_t *d, size_t n) {
  mtrc_tlv_reader rd; mtrc_tlv_reader_init(&rd, in, len);
  mtrc_tlv_elem e;
  if (!mtrc_tlv_read(&rd, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  int found = 0;
  while (mtrc_tlv_read(&rd, &e) && e.type != MTRC_TLV_END) {
    if (e.tag.number == tag && e.type == MTRC_TLV_BYTES && e.bytes_len == n) {
      memcpy(d, e.bytes, n); found = 1;
    }
  }
  return found && !rd.err;
}

int mtrc_pase_encode_pake1(uint8_t *out, size_t cap, const uint8_t pA[65]) { return enc_one_bytes(out, cap, 1, pA, 65); }
int mtrc_pase_decode_pake1(const uint8_t *in, size_t len, uint8_t pA[65])  { return dec_field_bytes(in, len, 1, pA, 65); }
int mtrc_pase_encode_pake3(uint8_t *out, size_t cap, const uint8_t cA[32]) { return enc_one_bytes(out, cap, 1, cA, 32); }
int mtrc_pase_decode_pake3(const uint8_t *in, size_t len, uint8_t cA[32])  { return dec_field_bytes(in, len, 1, cA, 32); }

int mtrc_pase_encode_pake2(uint8_t *out, size_t cap, const uint8_t pB[65], const uint8_t cB[32]) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), pB, 65);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), cB, 32);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}
int mtrc_pase_decode_pake2(const uint8_t *in, size_t len, uint8_t pB[65], uint8_t cB[32]) {
  mtrc_tlv_reader rd; mtrc_tlv_reader_init(&rd, in, len);
  mtrc_tlv_elem e; int got = 0;
  if (!mtrc_tlv_read(&rd, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  while (mtrc_tlv_read(&rd, &e) && e.type != MTRC_TLV_END) {
    if (e.tag.number==1 && e.type==MTRC_TLV_BYTES && e.bytes_len==65) { memcpy(pB, e.bytes, 65); got |= 1; }
    if (e.tag.number==2 && e.type==MTRC_TLV_BYTES && e.bytes_len==32) { memcpy(cB, e.bytes, 32); got |= 2; }
  }
  return got == 3 && !rd.err;
}
