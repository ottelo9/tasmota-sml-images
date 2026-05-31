// mtrc_frame.c — Matter message/protocol header codec. See mtrc_frame.h.
// GPLv3.

#include "mtrc_frame.h"
#include <string.h>

// message-flags bit positions (CSA Matter Core §4.4.1.1):
//   bits 7..4 = Version, bit 2 = S (Source Node ID present),
//   bits 1..0 = DSIZ (0=none, 1=dest node 8B, 2=dest group 2B).
#define MF_VER_SHIFT   4
#define MF_S_FLAG      0x04
#define MF_DSIZ_MASK   0x03
// security-flags bit positions
#define SF_P           0x80
#define SF_C           0x40
#define SF_MX          0x20
#define SF_STYPE_MASK  0x03
// exchange-flags bit positions
#define XF_I           0x01
#define XF_A           0x02
#define XF_R           0x04
#define XF_SX          0x08
#define XF_V           0x10

typedef struct { uint8_t *p; size_t cap, len; int err; } wbuf;
static void wb_u8 (wbuf *w, uint8_t v){ if(w->err||w->len+1>w->cap){w->err=1;return;} w->p[w->len++]=v; }
static void wb_le (wbuf *w, uint64_t v, int n){ for(int i=0;i<n;i++) wb_u8(w,(uint8_t)(v>>(8*i))); }
static void wb_raw(wbuf *w, const uint8_t *d, size_t n){
  if(w->err||w->len+n>w->cap){w->err=1;return;} if(n){memcpy(w->p+w->len,d,n); w->len+=n;}
}

uint8_t mtrc_frame_security_flags(const mtrc_msg_header *mh) {
  uint8_t sf = (uint8_t)(mh->session_type & SF_STYPE_MASK);
  if (mh->control) sf |= SF_C;
  return sf;
}

int mtrc_frame_encode_msg_header(uint8_t *out, size_t cap, const mtrc_msg_header *mh) {
  if (!out || !mh) return -1;
  wbuf w = { out, cap, 0, 0 };
  uint8_t mf = (uint8_t)((mh->version & 0x07) << MF_VER_SHIFT);
  if (mh->has_src) mf |= MF_S_FLAG;
  mf |= (uint8_t)(mh->dsiz & MF_DSIZ_MASK);
  wb_u8(&w, mf);
  wb_le(&w, mh->session_id, 2);
  wb_u8(&w, mtrc_frame_security_flags(mh));
  wb_le(&w, mh->msg_counter, 4);
  if (mh->has_src)                      wb_le(&w, mh->src_node_id, 8);
  if (mh->dsiz == MTRC_DSIZ_NODE)       wb_le(&w, mh->dest_node_id, 8);
  else if (mh->dsiz == MTRC_DSIZ_GROUP) wb_le(&w, mh->dest_group_id, 2);
  return w.err ? -1 : (int)w.len;
}

int mtrc_frame_encode_proto(uint8_t *out, size_t cap, const mtrc_proto_header *ph,
                            const uint8_t *payload, size_t payload_len) {
  if (!out || !ph) return -1;
  wbuf w = { out, cap, 0, 0 };
  uint8_t xf = 0;
  if (ph->initiator)   xf |= XF_I;
  if (ph->ack)         xf |= XF_A;
  if (ph->reliability) xf |= XF_R;
  if (ph->has_vendor)  xf |= XF_V;
  wb_u8(&w, xf);
  wb_u8(&w, ph->opcode);
  wb_le(&w, ph->exchange_id, 2);
  wb_le(&w, ph->protocol_id, 2);
  if (ph->has_vendor) wb_le(&w, ph->vendor_id, 2);
  if (ph->ack)        wb_le(&w, ph->ack_counter, 4);
  if (payload_len) wb_raw(&w, payload, payload_len);
  return w.err ? -1 : (int)w.len;
}

int mtrc_frame_encode(uint8_t *out, size_t cap,
                      const mtrc_msg_header *mh, const mtrc_proto_header *ph,
                      const uint8_t *payload, size_t payload_len) {
  int hn = mtrc_frame_encode_msg_header(out, cap, mh);
  if (hn < 0) return -1;
  int pn = mtrc_frame_encode_proto(out + hn, cap - (size_t)hn, ph, payload, payload_len);
  if (pn < 0) return -1;
  return hn + pn;
}

typedef struct { const uint8_t *p; size_t len, off; int err; } rbuf;
static int rb_avail(rbuf *r, size_t n){ return r->off + n <= r->len; }
static uint8_t  rb_u8(rbuf *r){ if(!rb_avail(r,1)){r->err=1;return 0;} return r->p[r->off++]; }
static uint64_t rb_le(rbuf *r, int n){
  if(!rb_avail(r,(size_t)n)){r->err=1;return 0;}
  uint64_t v=0; for(int i=0;i<n;i++) v|=(uint64_t)r->p[r->off+i]<<(8*i); r->off+=n; return v;
}

int mtrc_frame_decode_msg_header(const uint8_t *buf, size_t len, mtrc_msg_header *mh) {
  if (!buf || !mh) return -1;
  rbuf r = { buf, len, 0, 0 };
  memset(mh, 0, sizeof(*mh));
  uint8_t mf = rb_u8(&r);
  mh->version  = (uint8_t)((mf >> MF_VER_SHIFT) & 0x0F);
  mh->has_src  = (mf & MF_S_FLAG) != 0;
  mh->dsiz     = (mtrc_dsiz)(mf & MF_DSIZ_MASK);
  if (mh->dsiz == 3) return -1;                      // reserved
  mh->session_id = (uint16_t)rb_le(&r, 2);
  uint8_t sf = rb_u8(&r);
  mh->control = (sf & SF_C) != 0;
  mh->session_type = (uint8_t)(sf & SF_STYPE_MASK);
  mh->msg_counter = (uint32_t)rb_le(&r, 4);
  if (mh->has_src)                mh->src_node_id  = rb_le(&r, 8);
  if (mh->dsiz == MTRC_DSIZ_NODE) mh->dest_node_id = rb_le(&r, 8);
  else if (mh->dsiz == MTRC_DSIZ_GROUP) mh->dest_group_id = (uint16_t)rb_le(&r, 2);
  if (sf & SF_MX) return -1;                         // message extensions: not yet
  return r.err ? -1 : (int)r.off;
}

int mtrc_frame_decode_proto(const uint8_t *buf, size_t len, mtrc_proto_header *ph,
                            const uint8_t **payload, size_t *payload_len) {
  if (!buf || !ph) return -1;
  rbuf r = { buf, len, 0, 0 };
  memset(ph, 0, sizeof(*ph));
  uint8_t xf = rb_u8(&r);
  ph->initiator   = (xf & XF_I) != 0;
  ph->ack         = (xf & XF_A) != 0;
  ph->reliability = (xf & XF_R) != 0;
  ph->has_vendor  = (xf & XF_V) != 0;
  ph->opcode      = rb_u8(&r);
  ph->exchange_id = (uint16_t)rb_le(&r, 2);
  ph->protocol_id = (uint16_t)rb_le(&r, 2);
  if (ph->has_vendor) ph->vendor_id   = (uint16_t)rb_le(&r, 2);
  if (ph->ack)        ph->ack_counter = (uint32_t)rb_le(&r, 4);
  if (xf & XF_SX) return -1;                         // secured extensions: not yet
  if (r.err) return -1;
  if (payload)     *payload = buf + r.off;
  if (payload_len) *payload_len = len - r.off;
  return 1;
}

int mtrc_frame_decode(const uint8_t *buf, size_t len,
                      mtrc_msg_header *mh, mtrc_proto_header *ph,
                      const uint8_t **payload, size_t *payload_len) {
  int hn = mtrc_frame_decode_msg_header(buf, len, mh);
  if (hn < 0) return -1;
  if (!mtrc_frame_decode_proto(buf + hn, len - (size_t)hn, ph, payload, payload_len))
    return -1;
  return (int)len;
}
