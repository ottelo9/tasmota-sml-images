// mtrc_tlv.h — Matter TLV codec for matter_c.
//
// Implements the Matter TLV encoding (Matter Core Spec, Appendix A): a
// compact, self-describing, little-endian wire format used everywhere in
// the Interaction Model and the commissioning messages.
//
// Element layout: control byte || tag bytes || [length] || value.
//   control byte = (tag_control << 5) | element_type
// All multi-byte integers and lengths are little-endian.
//
// Streaming, no heap: a writer appends into a caller-provided buffer; a
// reader walks a caller-provided buffer and points string/byte values
// directly into it (zero-copy). Container nesting is explicit
// (start_/end_container on the writer; the reader returns END elements).
//
// GPLv3. Implemented from the Matter spec, not converted from Berry.

#ifndef MTRC_TLV_H
#define MTRC_TLV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- element types (low 5 bits of the control byte) -------------------
// The reader/writer expose LOGICAL types; integer width is chosen/decoded
// automatically. These constants are the canonical (smallest-width) ids.
typedef enum {
  MTRC_TLV_SINT   = 0x00,  // signed integer   (1/2/4/8-byte on the wire)
  MTRC_TLV_UINT   = 0x04,  // unsigned integer (1/2/4/8-byte on the wire)
  MTRC_TLV_BOOL   = 0x08,  // boolean (false=0x08, true=0x09)
  MTRC_TLV_FLOAT  = 0x0A,  // 4-byte IEEE-754
  MTRC_TLV_DOUBLE = 0x0B,  // 8-byte IEEE-754
  MTRC_TLV_UTF8   = 0x0C,  // UTF-8 string  (1/2/4/8-byte length prefix)
  MTRC_TLV_BYTES  = 0x10,  // octet string  (1/2/4/8-byte length prefix)
  MTRC_TLV_NULL   = 0x14,  // null
  MTRC_TLV_STRUCT = 0x15,  // structure (container)
  MTRC_TLV_ARRAY  = 0x16,  // array     (container)
  MTRC_TLV_LIST   = 0x17,  // list      (container)
  MTRC_TLV_END    = 0x18,  // end of container
} mtrc_tlv_type;

// ---- tag control (high 3 bits of the control byte) --------------------
typedef enum {
  MTRC_TLV_TAG_ANON     = 0,  // 0 tag bytes
  MTRC_TLV_TAG_CONTEXT  = 1,  // 1 tag byte
  MTRC_TLV_TAG_COMMON2  = 2,  // 2 tag bytes (common profile)
  MTRC_TLV_TAG_COMMON4  = 3,  // 4 tag bytes (common profile)
  MTRC_TLV_TAG_IMPL2    = 4,  // 2 tag bytes (implicit profile)
  MTRC_TLV_TAG_IMPL4    = 5,  // 4 tag bytes (implicit profile)
  MTRC_TLV_TAG_FULL6    = 6,  // 6 tag bytes: vid(2) prof(2) tag(2)
  MTRC_TLV_TAG_FULL8    = 7,  // 8 tag bytes: vid(2) prof(2) tag(4)
} mtrc_tlv_tag_ctrl;

typedef struct {
  mtrc_tlv_tag_ctrl ctrl;
  uint16_t vendor_id;    // FULL6/FULL8 only
  uint16_t profile_num;  // FULL6/FULL8 only
  uint32_t number;       // tag number (CONTEXT: 0..255, else up to 32-bit)
} mtrc_tlv_tag;

static inline mtrc_tlv_tag mtrc_tlv_anon(void) {
  mtrc_tlv_tag t = { MTRC_TLV_TAG_ANON, 0, 0, 0 }; return t;
}
static inline mtrc_tlv_tag mtrc_tlv_ctx(uint8_t n) {
  mtrc_tlv_tag t = { MTRC_TLV_TAG_CONTEXT, 0, 0, n }; return t;
}
static inline mtrc_tlv_tag mtrc_tlv_full(uint16_t vid, uint16_t prof, uint32_t num) {
  mtrc_tlv_tag t = { MTRC_TLV_TAG_FULL8, vid, prof, num }; return t;
}

// ---- writer ------------------------------------------------------------
typedef struct {
  uint8_t *buf;
  size_t   cap;
  size_t   len;     // bytes written so far
  int      depth;   // open container count
  int      err;     // sticky: 1 once an overflow/misuse occurred
} mtrc_tlv_writer;

void   mtrc_tlv_writer_init(mtrc_tlv_writer *w, uint8_t *buf, size_t cap);
size_t mtrc_tlv_writer_len(const mtrc_tlv_writer *w);
int    mtrc_tlv_writer_ok(const mtrc_tlv_writer *w);  // 1 if no error + depth==0

// Append already-encoded TLV bytes verbatim (pre-built fragment into a container).
int mtrc_tlv_put_raw   (mtrc_tlv_writer *w, const uint8_t *p, size_t n);

// Scalars (minimal-width encoding chosen automatically for ints).
int mtrc_tlv_put_uint  (mtrc_tlv_writer *w, mtrc_tlv_tag tag, uint64_t v);
int mtrc_tlv_put_int   (mtrc_tlv_writer *w, mtrc_tlv_tag tag, int64_t  v);
int mtrc_tlv_put_bool  (mtrc_tlv_writer *w, mtrc_tlv_tag tag, bool b);
int mtrc_tlv_put_null  (mtrc_tlv_writer *w, mtrc_tlv_tag tag);
int mtrc_tlv_put_float (mtrc_tlv_writer *w, mtrc_tlv_tag tag, float f);
int mtrc_tlv_put_double(mtrc_tlv_writer *w, mtrc_tlv_tag tag, double d);
int mtrc_tlv_put_bytes (mtrc_tlv_writer *w, mtrc_tlv_tag tag,
                        const uint8_t *data, size_t len);
int mtrc_tlv_put_utf8  (mtrc_tlv_writer *w, mtrc_tlv_tag tag,
                        const char *s, size_t len);

// Containers.
int mtrc_tlv_start_struct(mtrc_tlv_writer *w, mtrc_tlv_tag tag);
int mtrc_tlv_start_array (mtrc_tlv_writer *w, mtrc_tlv_tag tag);
int mtrc_tlv_start_list  (mtrc_tlv_writer *w, mtrc_tlv_tag tag);
int mtrc_tlv_end_container(mtrc_tlv_writer *w);

// ---- reader ------------------------------------------------------------
typedef struct {
  const uint8_t *buf;
  size_t len;
  size_t off;
  int    err;
} mtrc_tlv_reader;

typedef struct {
  mtrc_tlv_type type;
  mtrc_tlv_tag  tag;
  int64_t       i;          // valid for SINT
  uint64_t      u;          // valid for UINT and BOOL (0/1)
  double        d;          // valid for FLOAT/DOUBLE
  const uint8_t *bytes;     // valid for UTF8/BYTES (points into source buf)
  uint32_t      bytes_len;
} mtrc_tlv_elem;

void mtrc_tlv_reader_init(mtrc_tlv_reader *r, const uint8_t *buf, size_t len);

// Read the next element header (+ scalar/string value). For containers,
// `type` is set and the reader is positioned at the first inner element;
// the matching MTRC_TLV_END is returned as its own element. Returns 1 on
// success, 0 at end-of-buffer or on malformed input (sets r->err on error).
int  mtrc_tlv_read(mtrc_tlv_reader *r, mtrc_tlv_elem *e);

#ifdef __cplusplus
}
#endif

#endif // MTRC_TLV_H
