// mtrc_tlv.c — Matter TLV codec. See mtrc_tlv.h. GPLv3.

#include "mtrc_tlv.h"
#include <string.h>

// ---- writer low-level --------------------------------------------------
static int w_raw(mtrc_tlv_writer *w, const uint8_t *p, size_t n) {
  if (w->err) return 0;
  if (w->len + n > w->cap) { w->err = 1; return 0; }
  memcpy(w->buf + w->len, p, n);
  w->len += n;
  return 1;
}
static int w_byte(mtrc_tlv_writer *w, uint8_t b) { return w_raw(w, &b, 1); }

// little-endian write of n bytes from a 64-bit value
static int w_le(mtrc_tlv_writer *w, uint64_t v, int n) {
  uint8_t b[8];
  for (int i = 0; i < n; i++) b[i] = (uint8_t)(v >> (8 * i));
  return w_raw(w, b, (size_t)n);
}

// write the tag bytes for `tag` (control bits go into the control byte).
static int w_tag(mtrc_tlv_writer *w, mtrc_tlv_tag tag) {
  switch (tag.ctrl) {
    case MTRC_TLV_TAG_ANON:    return 1;
    case MTRC_TLV_TAG_CONTEXT: return w_byte(w, (uint8_t)tag.number);
    case MTRC_TLV_TAG_COMMON2:
    case MTRC_TLV_TAG_IMPL2:   return w_le(w, tag.number, 2);
    case MTRC_TLV_TAG_COMMON4:
    case MTRC_TLV_TAG_IMPL4:   return w_le(w, tag.number, 4);
    case MTRC_TLV_TAG_FULL6:
      return w_le(w, tag.vendor_id, 2) && w_le(w, tag.profile_num, 2)
          && w_le(w, tag.number, 2);
    case MTRC_TLV_TAG_FULL8:
      return w_le(w, tag.vendor_id, 2) && w_le(w, tag.profile_num, 2)
          && w_le(w, tag.number, 4);
  }
  w->err = 1; return 0;
}

// emit control byte (tag_control<<5 | element_type) then the tag bytes.
static int w_head(mtrc_tlv_writer *w, mtrc_tlv_tag tag, uint8_t elem_type) {
  if (!w_byte(w, (uint8_t)(((uint8_t)tag.ctrl << 5) | elem_type))) return 0;
  return w_tag(w, tag);
}

void mtrc_tlv_writer_init(mtrc_tlv_writer *w, uint8_t *buf, size_t cap) {
  w->buf = buf; w->cap = cap; w->len = 0; w->depth = 0; w->err = 0;
}
size_t mtrc_tlv_writer_len(const mtrc_tlv_writer *w) { return w->len; }
int    mtrc_tlv_writer_ok (const mtrc_tlv_writer *w) { return !w->err && w->depth == 0; }
// Append already-encoded TLV bytes verbatim (e.g. a pre-built AttributeReportIB
// fragment into a ReportData array). Does not touch container depth.
int mtrc_tlv_put_raw(mtrc_tlv_writer *w, const uint8_t *p, size_t n) { return w_raw(w, p, n); }

int mtrc_tlv_put_uint(mtrc_tlv_writer *w, mtrc_tlv_tag tag, uint64_t v) {
  uint8_t t; int n;
  if      (v <= 0xFFu)        { t = MTRC_TLV_UINT + 0; n = 1; }
  else if (v <= 0xFFFFu)      { t = MTRC_TLV_UINT + 1; n = 2; }
  else if (v <= 0xFFFFFFFFu)  { t = MTRC_TLV_UINT + 2; n = 4; }
  else                        { t = MTRC_TLV_UINT + 3; n = 8; }
  return w_head(w, tag, t) && w_le(w, v, n);
}

int mtrc_tlv_put_int(mtrc_tlv_writer *w, mtrc_tlv_tag tag, int64_t v) {
  uint8_t t; int n;
  if      (v >= -128 && v <= 127)             { t = MTRC_TLV_SINT + 0; n = 1; }
  else if (v >= -32768 && v <= 32767)         { t = MTRC_TLV_SINT + 1; n = 2; }
  else if (v >= -2147483647LL - 1 && v <= 2147483647LL) { t = MTRC_TLV_SINT + 2; n = 4; }
  else                                        { t = MTRC_TLV_SINT + 3; n = 8; }
  return w_head(w, tag, t) && w_le(w, (uint64_t)v, n);
}

int mtrc_tlv_put_bool(mtrc_tlv_writer *w, mtrc_tlv_tag tag, bool b) {
  return w_head(w, tag, (uint8_t)(MTRC_TLV_BOOL + (b ? 1 : 0)));
}
int mtrc_tlv_put_null(mtrc_tlv_writer *w, mtrc_tlv_tag tag) {
  return w_head(w, tag, MTRC_TLV_NULL);
}
int mtrc_tlv_put_float(mtrc_tlv_writer *w, mtrc_tlv_tag tag, float f) {
  uint32_t u; memcpy(&u, &f, 4);
  return w_head(w, tag, MTRC_TLV_FLOAT) && w_le(w, u, 4);
}
int mtrc_tlv_put_double(mtrc_tlv_writer *w, mtrc_tlv_tag tag, double dd) {
  uint64_t u; memcpy(&u, &dd, 8);
  return w_head(w, tag, MTRC_TLV_DOUBLE) && w_le(w, u, 8);
}

// length-prefixed value (UTF8/BYTES): pick the minimal length-field width.
static int w_lenval(mtrc_tlv_writer *w, mtrc_tlv_tag tag, uint8_t base,
                    const uint8_t *data, size_t len) {
  uint8_t t; int ln;
  if      (len <= 0xFFu)       { t = base + 0; ln = 1; }
  else if (len <= 0xFFFFu)     { t = base + 1; ln = 2; }
  else if (len <= 0xFFFFFFFFu) { t = base + 2; ln = 4; }
  else                         { t = base + 3; ln = 8; }
  if (!w_head(w, tag, t) || !w_le(w, (uint64_t)len, ln)) return 0;
  return len ? w_raw(w, data, len) : 1;
}
int mtrc_tlv_put_bytes(mtrc_tlv_writer *w, mtrc_tlv_tag tag,
                       const uint8_t *data, size_t len) {
  return w_lenval(w, tag, MTRC_TLV_BYTES, data, len);
}
int mtrc_tlv_put_utf8(mtrc_tlv_writer *w, mtrc_tlv_tag tag,
                      const char *s, size_t len) {
  return w_lenval(w, tag, MTRC_TLV_UTF8, (const uint8_t *)s, len);
}

static int w_start(mtrc_tlv_writer *w, mtrc_tlv_tag tag, uint8_t t) {
  if (!w_head(w, tag, t)) return 0;
  w->depth++;
  return 1;
}
int mtrc_tlv_start_struct(mtrc_tlv_writer *w, mtrc_tlv_tag tag) { return w_start(w, tag, MTRC_TLV_STRUCT); }
int mtrc_tlv_start_array (mtrc_tlv_writer *w, mtrc_tlv_tag tag) { return w_start(w, tag, MTRC_TLV_ARRAY); }
int mtrc_tlv_start_list  (mtrc_tlv_writer *w, mtrc_tlv_tag tag) { return w_start(w, tag, MTRC_TLV_LIST); }
int mtrc_tlv_end_container(mtrc_tlv_writer *w) {
  if (w->depth <= 0) { w->err = 1; return 0; }
  if (!w_byte(w, MTRC_TLV_END)) return 0;
  w->depth--;
  return 1;
}

// ---- reader ------------------------------------------------------------
// SECURITY: compute remaining as (len - off) — NEVER (off + n) which overflows
// in 32-bit size_t for an attacker-chosen length, wrapping below len and passing
// a bogus bounds check. The off<=len invariant holds throughout (off only advances
// after a passing check).
static int r_avail(mtrc_tlv_reader *r, size_t n) { return r->off <= r->len && n <= r->len - r->off; }

static uint64_t r_le(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= (uint64_t)p[i] << (8 * i);
  return v;
}

void mtrc_tlv_reader_init(mtrc_tlv_reader *r, const uint8_t *buf, size_t len) {
  r->buf = buf; r->len = len; r->off = 0; r->err = 0;
}

int mtrc_tlv_read(mtrc_tlv_reader *r, mtrc_tlv_elem *e) {
  if (r->err || r->off >= r->len) return 0;
  uint8_t ctrl = r->buf[r->off++];
  uint8_t tag_ctrl = (uint8_t)(ctrl >> 5);
  uint8_t et = (uint8_t)(ctrl & 0x1F);

  memset(e, 0, sizeof(*e));
  e->tag.ctrl = (mtrc_tlv_tag_ctrl)tag_ctrl;

  // tag bytes
  switch (tag_ctrl) {
    case MTRC_TLV_TAG_ANON: break;
    case MTRC_TLV_TAG_CONTEXT:
      if (!r_avail(r, 1)) goto bad; e->tag.number = r->buf[r->off]; r->off += 1; break;
    case MTRC_TLV_TAG_COMMON2: case MTRC_TLV_TAG_IMPL2:
      if (!r_avail(r, 2)) goto bad; e->tag.number = (uint32_t)r_le(r->buf + r->off, 2); r->off += 2; break;
    case MTRC_TLV_TAG_COMMON4: case MTRC_TLV_TAG_IMPL4:
      if (!r_avail(r, 4)) goto bad; e->tag.number = (uint32_t)r_le(r->buf + r->off, 4); r->off += 4; break;
    case MTRC_TLV_TAG_FULL6:
      if (!r_avail(r, 6)) goto bad;
      e->tag.vendor_id   = (uint16_t)r_le(r->buf + r->off, 2);
      e->tag.profile_num = (uint16_t)r_le(r->buf + r->off + 2, 2);
      e->tag.number      = (uint32_t)r_le(r->buf + r->off + 4, 2);
      r->off += 6; break;
    case MTRC_TLV_TAG_FULL8:
      if (!r_avail(r, 8)) goto bad;
      e->tag.vendor_id   = (uint16_t)r_le(r->buf + r->off, 2);
      e->tag.profile_num = (uint16_t)r_le(r->buf + r->off + 2, 2);
      e->tag.number      = (uint32_t)r_le(r->buf + r->off + 4, 4);
      r->off += 8; break;
    default: goto bad;
  }

  // value, by element type
  if (et <= 0x03) {                     // signed int, width 1<<et bytes
    int n = 1 << et;
    if (!r_avail(r, (size_t)n)) goto bad;
    uint64_t raw = r_le(r->buf + r->off, n); r->off += n;
    int bits = n * 8;
    // sign-extend
    if (bits < 64 && (raw & (1ull << (bits - 1)))) raw |= ~((1ull << bits) - 1);
    e->type = MTRC_TLV_SINT; e->i = (int64_t)raw;
  } else if (et <= 0x07) {              // unsigned int
    int n = 1 << (et - 0x04);
    if (!r_avail(r, (size_t)n)) goto bad;
    e->type = MTRC_TLV_UINT; e->u = r_le(r->buf + r->off, n); r->off += n;
  } else if (et == 0x08 || et == 0x09) {
    e->type = MTRC_TLV_BOOL; e->u = (et == 0x09) ? 1 : 0;
  } else if (et == 0x0A) {              // float
    if (!r_avail(r, 4)) goto bad;
    uint32_t u = (uint32_t)r_le(r->buf + r->off, 4); r->off += 4;
    float f; memcpy(&f, &u, 4); e->type = MTRC_TLV_FLOAT; e->d = (double)f;
  } else if (et == 0x0B) {              // double
    if (!r_avail(r, 8)) goto bad;
    uint64_t u = r_le(r->buf + r->off, 8); r->off += 8;
    double dd; memcpy(&dd, &u, 8); e->type = MTRC_TLV_DOUBLE; e->d = dd;
  } else if (et >= 0x0C && et <= 0x13) {   // utf8 (0x0C..) / bytes (0x10..)
    int base = (et >= 0x10) ? 0x10 : 0x0C;
    int ln = 1 << (et - base);
    if (!r_avail(r, (size_t)ln)) goto bad;
    uint64_t slen = r_le(r->buf + r->off, ln); r->off += ln;
    // SECURITY: compare the full 64-bit length against remaining BEFORE the
    // (size_t) cast — a 4/8-byte length form can exceed size_t and truncate.
    if (slen > (uint64_t)(r->len - r->off)) goto bad;
    e->type = (base == 0x10) ? MTRC_TLV_BYTES : MTRC_TLV_UTF8;
    e->bytes = r->buf + r->off; e->bytes_len = (uint32_t)slen;
    r->off += (size_t)slen;
  } else if (et == 0x14) {
    e->type = MTRC_TLV_NULL;
  } else if (et == 0x15) { e->type = MTRC_TLV_STRUCT;
  } else if (et == 0x16) { e->type = MTRC_TLV_ARRAY;
  } else if (et == 0x17) { e->type = MTRC_TLV_LIST;
  } else if (et == 0x18) { e->type = MTRC_TLV_END;
  } else { goto bad; }

  return 1;
bad:
  r->err = 1;
  return 0;
}
