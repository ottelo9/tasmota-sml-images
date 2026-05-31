// mtrc_case_msg.c — CASE Sigma message TLV codecs. See mtrc_case_msg.h.
// GPLv3.

#include "mtrc_case_msg.h"
#include "mtrc_tlv.h"
#include <string.h>

// ---- Sigma1 ------------------------------------------------------------
int mtrc_sigma1_encode(uint8_t *out, size_t cap, const mtrc_sigma1 *s) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), s->initiator_random, 32);
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(2), s->initiator_session_id);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(3), s->destination_id, 32);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(4), s->initiator_eph_pub, 65);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_sigma1_decode(const uint8_t *in, size_t len, mtrc_sigma1 *s) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, in, len);
  mtrc_tlv_elem e; memset(s, 0, sizeof(*s));
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  // Sigma1 field 5 (initiatorSEDParams) is a NESTED struct whose inner context
  // tags (1=idle,2=active interval,...) collide with our top-level tags. The
  // flat reader emits the nested elements too, so we must only act on top-level
  // fields (depth 0) and skip the contents of any nested container — otherwise
  // e.g. SESSION_ACTIVE_INTERVAL (300 ms) overwrites initiatorSessionId.
  int depth = 0;
  while (mtrc_tlv_read(&r, &e)) {
    if (e.type == MTRC_TLV_END) { if (depth == 0) break; depth--; continue; }
    if (depth == 0) {
      switch (e.tag.number) {
        case 1: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(s->initiator_random, e.bytes, 32); break;
        case 2: if (e.type==MTRC_TLV_UINT) s->initiator_session_id = (uint16_t)e.u; break;
        case 3: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(s->destination_id, e.bytes, 32); break;
        case 4: if (e.type==MTRC_TLV_BYTES && e.bytes_len==65) memcpy(s->initiator_eph_pub, e.bytes, 65); break;
        default: break;   // optional 5 (SEDParams)/6/7 skipped below
      }
    }
    if (e.type==MTRC_TLV_STRUCT || e.type==MTRC_TLV_ARRAY || e.type==MTRC_TLV_LIST) depth++;
  }
  return !r.err;
}

// ---- Sigma2 ------------------------------------------------------------
int mtrc_sigma2_encode(uint8_t *out, size_t cap, const mtrc_sigma2 *s) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), s->responder_random, 32);
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(2), s->responder_session_id);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(3), s->responder_eph_pub, 65);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(4), s->encrypted2, s->encrypted2_len);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_sigma2_decode(const uint8_t *in, size_t len, mtrc_sigma2 *s) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, in, len);
  mtrc_tlv_elem e; memset(s, 0, sizeof(*s));
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  while (mtrc_tlv_read(&r, &e) && e.type != MTRC_TLV_END) {
    switch (e.tag.number) {
      case 1: if (e.type==MTRC_TLV_BYTES && e.bytes_len==32) memcpy(s->responder_random, e.bytes, 32); break;
      case 2: s->responder_session_id = (uint16_t)e.u; break;
      case 3: if (e.type==MTRC_TLV_BYTES && e.bytes_len==65) memcpy(s->responder_eph_pub, e.bytes, 65); break;
      case 4: if (e.type==MTRC_TLV_BYTES) { s->encrypted2 = e.bytes; s->encrypted2_len = e.bytes_len; } break;
      default: break;
    }
  }
  return !r.err;
}

// ---- Sigma3 ------------------------------------------------------------
int mtrc_sigma3_encode(uint8_t *out, size_t cap, const mtrc_sigma3 *s) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), s->encrypted3, s->encrypted3_len);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_sigma3_decode(const uint8_t *in, size_t len, mtrc_sigma3 *s) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, in, len);
  mtrc_tlv_elem e; memset(s, 0, sizeof(*s));
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  while (mtrc_tlv_read(&r, &e) && e.type != MTRC_TLV_END) {
    if (e.tag.number==1 && e.type==MTRC_TLV_BYTES) { s->encrypted3 = e.bytes; s->encrypted3_len = e.bytes_len; }
  }
  return !r.err;
}

// ---- TBEData (encrypted inner body) ------------------------------------
int mtrc_case_tbe_encode(uint8_t *out, size_t cap, const mtrc_case_tbe *t) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), t->noc, t->noc_len);
  if (t->icac_len) mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), t->icac, t->icac_len);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(3), t->signature, 64);
  if (t->resumption_id_len) mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(4), t->resumption_id, t->resumption_id_len);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

int mtrc_case_tbe_decode(const uint8_t *in, size_t len, mtrc_case_tbe *t) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, in, len);
  mtrc_tlv_elem e; memset(t, 0, sizeof(*t));
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;
  while (mtrc_tlv_read(&r, &e) && e.type != MTRC_TLV_END) {
    switch (e.tag.number) {
      case 1: if (e.type==MTRC_TLV_BYTES) { t->noc = e.bytes; t->noc_len = e.bytes_len; } break;
      case 2: if (e.type==MTRC_TLV_BYTES) { t->icac = e.bytes; t->icac_len = e.bytes_len; } break;
      case 3: if (e.type==MTRC_TLV_BYTES && e.bytes_len==64) memcpy(t->signature, e.bytes, 64); break;
      case 4: if (e.type==MTRC_TLV_BYTES) { t->resumption_id = e.bytes; t->resumption_id_len = e.bytes_len; } break;
      default: break;
    }
  }
  return !r.err;
}

// ---- TBSData (signed body) — encode only (re-encoded to sign/verify) ---
int mtrc_case_tbs_encode(uint8_t *out, size_t cap, const mtrc_case_tbs *t) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), t->noc, t->noc_len);
  if (t->icac_len) mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), t->icac, t->icac_len);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(3), t->sender_pub, 65);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(4), t->receiver_pub, 65);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}
