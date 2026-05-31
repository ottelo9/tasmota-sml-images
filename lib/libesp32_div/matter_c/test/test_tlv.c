// test_tlv.c — Phase-1 self-test for matter_c TLV codec.
//
// Two kinds of checks:
//  (A) Encode canonical Matter TLV examples and compare the bytes against
//      values derived from the Matter Core Spec (Appendix A).
//  (B) Round-trip: encode a nested structure, decode it verifying every
//      value, then re-encode and assert byte-identical output.
//
// Build/run host-side: see test/build_tlv.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_tlv.h"

static int g_ok = 1;

static void tohex(const uint8_t *b, size_t n, char *out) {
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2*i] = H[b[i]>>4]; out[2*i+1] = H[b[i]&15]; }
  out[2*n] = 0;
}

static void expect_hex(const char *label, mtrc_tlv_writer *w, const char *exp) {
  char got[256];
  int ok = mtrc_tlv_writer_ok(w);
  tohex(w->buf, w->len, got);
  ok = ok && (strcmp(got, exp) == 0);
  printf("  [%s] %-28s %s\n", ok ? "PASS" : "FAIL", label, ok ? "" : got);
  if (!ok) { printf("        expected: %s\n        got:      %s\n", exp, got); }
  g_ok &= ok;
}

#define WBUF static uint8_t buf[128]; mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, buf, sizeof(buf))

static void test_encode_vectors(void) {
  printf("(A) canonical spec encode vectors\n");
  { WBUF; mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 42);          expect_hex("uint 42",        &w, "042a"); }
  { WBUF; mtrc_tlv_put_int (&w, mtrc_tlv_anon(), -1);          expect_hex("int -1",         &w, "00ff"); }
  { WBUF; mtrc_tlv_put_int (&w, mtrc_tlv_anon(), 42);          expect_hex("int 42",         &w, "002a"); }
  { WBUF; mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 0x1234);      expect_hex("uint 0x1234",    &w, "053412"); }
  { WBUF; mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 0x0102030405060708ULL);
                                                               expect_hex("uint 8-byte",    &w, "070807060504030201"); }
  { WBUF; mtrc_tlv_put_bool(&w, mtrc_tlv_anon(), false);       expect_hex("bool false",     &w, "08"); }
  { WBUF; mtrc_tlv_put_bool(&w, mtrc_tlv_anon(), true);        expect_hex("bool true",      &w, "09"); }
  { WBUF; mtrc_tlv_put_null(&w, mtrc_tlv_anon());              expect_hex("null",           &w, "14"); }
  { WBUF; mtrc_tlv_put_float(&w, mtrc_tlv_anon(), 17.9f);      expect_hex("float 17.9",     &w, "0a33338f41"); }
  { WBUF; mtrc_tlv_put_utf8(&w, mtrc_tlv_anon(), "Hello!", 6); expect_hex("utf8 Hello!",    &w, "0c0648656c6c6f21"); }
  { WBUF; const uint8_t b[] = {0,1,2,3,4};
          mtrc_tlv_put_bytes(&w, mtrc_tlv_anon(), b, 5);       expect_hex("bytes 00..04",   &w, "10050001020304"); }
  { WBUF; mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
          mtrc_tlv_end_container(&w);                          expect_hex("empty struct",   &w, "1518"); }
  { WBUF; mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
          mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), 42);
          mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(1), true);
          mtrc_tlv_end_container(&w);                          expect_hex("struct{0=42,1=T}", &w, "1524002a290118"); }
  { WBUF; mtrc_tlv_start_array(&w, mtrc_tlv_anon());
          mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 1);
          mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 2);
          mtrc_tlv_put_uint(&w, mtrc_tlv_anon(), 3);
          mtrc_tlv_end_container(&w);                          expect_hex("array[1,2,3]",   &w, "1604010402040318"); }
}

static void chk(const char *label, int cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", label);
  g_ok &= cond;
}

static void test_roundtrip(void) {
  printf("(B) round-trip nested structure\n");
  static uint8_t buf[256];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, buf, sizeof(buf));
  // struct { ctx0 = uint 7, ctx1 = "Tasmota", ctx2 = bytes[aa,bb], ctx3 = int -1000,
  //          ctx4 = bool true, ctx5 = array[ uint 10, uint 20 ],
  //          ctx6 = float 3.5, ctx7 = null }
  const uint8_t raw[] = { 0xaa, 0xbb };
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(0), 7);
  mtrc_tlv_put_utf8 (&w, mtrc_tlv_ctx(1), "Tasmota", 7);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), raw, 2);
  mtrc_tlv_put_int  (&w, mtrc_tlv_ctx(3), -1000);
  mtrc_tlv_put_bool (&w, mtrc_tlv_ctx(4), true);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(5));
  mtrc_tlv_put_uint (&w, mtrc_tlv_anon(), 10);
  mtrc_tlv_put_uint (&w, mtrc_tlv_anon(), 20);
  mtrc_tlv_end_container(&w);
  mtrc_tlv_put_float(&w, mtrc_tlv_ctx(6), 3.5f);
  mtrc_tlv_put_null (&w, mtrc_tlv_ctx(7));
  mtrc_tlv_end_container(&w);
  chk("encode ok", mtrc_tlv_writer_ok(&w));
  size_t enc_len = mtrc_tlv_writer_len(&w);

  // decode + verify
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, enc_len);
  mtrc_tlv_elem e;
  int step = 0, all = 1;
  #define NEXT (mtrc_tlv_read(&r, &e))
  all &= NEXT && e.type == MTRC_TLV_STRUCT;                                   step++;
  all &= NEXT && e.type == MTRC_TLV_UINT  && e.tag.number==0 && e.u==7;       step++;
  all &= NEXT && e.type == MTRC_TLV_UTF8  && e.tag.number==1 && e.bytes_len==7
              && memcmp(e.bytes,"Tasmota",7)==0;                             step++;
  all &= NEXT && e.type == MTRC_TLV_BYTES && e.tag.number==2 && e.bytes_len==2
              && e.bytes[0]==0xaa && e.bytes[1]==0xbb;                        step++;
  all &= NEXT && e.type == MTRC_TLV_SINT  && e.tag.number==3 && e.i==-1000;   step++;
  all &= NEXT && e.type == MTRC_TLV_BOOL  && e.tag.number==4 && e.u==1;       step++;
  all &= NEXT && e.type == MTRC_TLV_ARRAY && e.tag.number==5;                 step++;
  all &= NEXT && e.type == MTRC_TLV_UINT  && e.u==10;                         step++;
  all &= NEXT && e.type == MTRC_TLV_UINT  && e.u==20;                         step++;
  all &= NEXT && e.type == MTRC_TLV_END;                                      step++;
  all &= NEXT && e.type == MTRC_TLV_FLOAT && e.tag.number==6
              && e.d > 3.49 && e.d < 3.51;                                    step++;
  all &= NEXT && e.type == MTRC_TLV_NULL  && e.tag.number==7;                 step++;
  all &= NEXT && e.type == MTRC_TLV_END;                                      step++;
  all &= !NEXT && !r.err;   // clean end of buffer
  #undef NEXT
  chk("decode all elements", all);
  printf("        (decoded %d elements, %zu bytes)\n", step, enc_len);

  // re-encode from decoded values -> must equal original bytes
  static uint8_t buf2[256];
  mtrc_tlv_writer w2; mtrc_tlv_writer_init(&w2, buf2, sizeof(buf2));
  mtrc_tlv_start_struct(&w2, mtrc_tlv_anon());
  mtrc_tlv_put_uint (&w2, mtrc_tlv_ctx(0), 7);
  mtrc_tlv_put_utf8 (&w2, mtrc_tlv_ctx(1), "Tasmota", 7);
  mtrc_tlv_put_bytes(&w2, mtrc_tlv_ctx(2), raw, 2);
  mtrc_tlv_put_int  (&w2, mtrc_tlv_ctx(3), -1000);
  mtrc_tlv_put_bool (&w2, mtrc_tlv_ctx(4), true);
  mtrc_tlv_start_array(&w2, mtrc_tlv_ctx(5));
  mtrc_tlv_put_uint (&w2, mtrc_tlv_anon(), 10);
  mtrc_tlv_put_uint (&w2, mtrc_tlv_anon(), 20);
  mtrc_tlv_end_container(&w2);
  mtrc_tlv_put_float(&w2, mtrc_tlv_ctx(6), 3.5f);
  mtrc_tlv_put_null (&w2, mtrc_tlv_ctx(7));
  mtrc_tlv_end_container(&w2);
  chk("re-encode byte-identical",
      mtrc_tlv_writer_len(&w2) == enc_len && memcmp(buf, buf2, enc_len) == 0);
}

static void test_fully_qualified_tag(void) {
  printf("(C) fully-qualified tag round-trip\n");
  static uint8_t buf[64];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, buf, sizeof(buf));
  mtrc_tlv_put_uint(&w, mtrc_tlv_full(0xFFF1, 0xDEAD, 0x12345678u), 99);
  chk("encode ok", mtrc_tlv_writer_ok(&w));
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, mtrc_tlv_writer_len(&w));
  mtrc_tlv_elem e;
  int ok = mtrc_tlv_read(&r, &e) && e.type==MTRC_TLV_UINT && e.u==99
        && e.tag.ctrl==MTRC_TLV_TAG_FULL8 && e.tag.vendor_id==0xFFF1
        && e.tag.profile_num==0xDEAD && e.tag.number==0x12345678u;
  chk("decode vid/prof/tag", ok);
}

int main(void) {
  printf("matter_c TLV codec self-test\n");
  test_encode_vectors();
  test_roundtrip();
  test_fully_qualified_tag();
  printf("\n==> TLV codec %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
