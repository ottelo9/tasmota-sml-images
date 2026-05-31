// test_msg.c — Phase-2 self-test: Matter frame codec + MRP reliability.
//
//  (A) frame: canonical byte vector + header round-trip.
//  (B) MRP: simulated A<->B exchange — reliable send is acked and clears
//      the pending tx; a dropped ack triggers backoff retransmits up to
//      the transmission limit then TIMEOUT; duplicate counters detected.
//
// Build/run host-side: see test/build_msg.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_frame.h"
#include "mtrc_mrp.h"

static int g_ok = 1;
static void chk(const char *label, int cond) {
  printf("  [%s] %s\n", cond ? "PASS" : "FAIL", label);
  g_ok &= cond;
}
static void tohex(const uint8_t *b, size_t n, char *out) {
  static const char *H = "0123456789abcdef";
  for (size_t i = 0; i < n; i++){ out[2*i]=H[b[i]>>4]; out[2*i+1]=H[b[i]&15]; } out[2*n]=0;
}

static void test_frame(void) {
  printf("(A) frame codec\n");
  // Unsecured PASE Pake1-style message: I+R, SecureChannel, exch 1, ctr 1,
  // payload "hi". Expected bytes derived from the Matter §4.4 layout.
  mtrc_msg_header mh; memset(&mh, 0, sizeof(mh));
  mh.session_id = 0; mh.session_type = 0; mh.msg_counter = 1; mh.dsiz = MTRC_DSIZ_NONE;
  mtrc_proto_header ph; memset(&ph, 0, sizeof(ph));
  ph.initiator = true; ph.reliability = true; ph.opcode = MTRC_SC_PASE_PAKE1;
  ph.exchange_id = 1; ph.protocol_id = MTRC_PROTO_SECURE_CHANNEL;
  const uint8_t pl[] = { 'h','i' };
  uint8_t buf[64]; char hex[160];
  int n = mtrc_frame_encode(buf, sizeof(buf), &mh, &ph, pl, 2);
  tohex(buf, (size_t)n, hex);
  chk("encode canonical bytes", n == 16 &&
      strcmp(hex, "00000000010000000522010000006869") == 0);
  if (strcmp(hex, "00000000010000000522010000006869"))
    printf("        got: %s\n", hex);

  // round-trip with src + 64-bit dest + vendor + ack
  mtrc_msg_header m2; memset(&m2, 0, sizeof(m2));
  m2.session_id = 0x1234; m2.session_type = 0; m2.msg_counter = 0xAABBCCDD;
  m2.has_src = true; m2.src_node_id = 0x1122334455667788ULL;
  m2.dsiz = MTRC_DSIZ_NODE; m2.dest_node_id = 0x99AABBCCDDEEFF00ULL;
  mtrc_proto_header p2; memset(&p2, 0, sizeof(p2));
  p2.initiator = false; p2.ack = true; p2.reliability = true; p2.has_vendor = true;
  p2.opcode = 0x05; p2.exchange_id = 0x9ABC; p2.protocol_id = MTRC_PROTO_IM;
  p2.vendor_id = 0xFFF1; p2.ack_counter = 0x01020304;
  const uint8_t pl2[] = { 1,2,3,4,5 };
  uint8_t b2[128]; int n2 = mtrc_frame_encode(b2, sizeof(b2), &m2, &p2, pl2, 5);
  mtrc_msg_header dm; mtrc_proto_header dp; const uint8_t *dpl; size_t dpl_len;
  int dn = mtrc_frame_decode(b2, (size_t)n2, &dm, &dp, &dpl, &dpl_len);
  int ok = dn == n2
    && dm.session_id==0x1234 && dm.msg_counter==0xAABBCCDD
    && dm.has_src && dm.src_node_id==0x1122334455667788ULL
    && dm.dsiz==MTRC_DSIZ_NODE && dm.dest_node_id==0x99AABBCCDDEEFF00ULL
    && !dp.initiator && dp.ack && dp.reliability && dp.has_vendor
    && dp.opcode==0x05 && dp.exchange_id==0x9ABC && dp.protocol_id==MTRC_PROTO_IM
    && dp.vendor_id==0xFFF1 && dp.ack_counter==0x01020304
    && dpl_len==5 && dpl[0]==1 && dpl[4]==5;
  chk("round-trip full header", ok);
}

static void test_mrp_ack(void) {
  printf("(B1) MRP reliable send -> ack clears pending\n");
  mtrc_mrp_ctx A, B; mtrc_mrp_init(&A, 0); mtrc_mrp_init(&B, 0);

  // A sends reliable msg counter=1 to B
  mtrc_msg_header mh; memset(&mh,0,sizeof(mh));
  mh.msg_counter=1; mh.dsiz=MTRC_DSIZ_NONE;
  mtrc_proto_header ph; memset(&ph,0,sizeof(ph));
  ph.initiator=true; ph.reliability=true; ph.opcode=MTRC_SC_PASE_PAKE1;
  ph.exchange_id=7; ph.protocol_id=MTRC_PROTO_SECURE_CHANNEL;
  uint8_t frame[64]; int fl = mtrc_frame_encode(frame,sizeof(frame),&mh,&ph,NULL,0);
  mtrc_mrp_on_send(&A, frame, (size_t)fl, 1, true, 0);
  chk("A has pending tx", A.tx_pending);

  // B receives -> not duplicate, owes an ack (R flag)
  mtrc_msg_header rmh; mtrc_proto_header rph; const uint8_t *rp; size_t rl;
  mtrc_frame_decode(frame, (size_t)fl, &rmh, &rph, &rp, &rl);
  chk("B sees fresh counter", !mtrc_mrp_is_duplicate(&B, rmh.msg_counter));
  chk("B must ack (R flag)", rph.reliability);

  // B builds standalone ack (its own counter=100), A receives it
  uint8_t ack[64];
  int al = mtrc_mrp_build_ack(ack, sizeof(ack), rmh.session_id, 100,
                              rph.exchange_id, false, rmh.msg_counter);
  mtrc_msg_header amh; mtrc_proto_header aph; const uint8_t *ap; size_t alp;
  mtrc_frame_decode(ack, (size_t)al, &amh, &aph, &ap, &alp);
  chk("ack is standalone-ack opcode", aph.opcode==MTRC_SC_STANDALONE_ACK
       && aph.ack && aph.ack_counter==1 && aph.protocol_id==MTRC_PROTO_SECURE_CHANNEL);
  chk("A ack clears pending", mtrc_mrp_on_ack(&A, aph.ack_counter) && !A.tx_pending);
}

static void test_mrp_retransmit(void) {
  printf("(B2) MRP dropped ack -> backoff retransmit -> timeout\n");
  mtrc_mrp_ctx A; mtrc_mrp_init(&A, 300);
  uint8_t frame[32]; memset(frame, 0x5A, sizeof(frame));
  mtrc_mrp_on_send(&A, frame, sizeof(frame), 2, true, 0);

  // before deadline: idle
  uint8_t out[64]; size_t ol;
  chk("idle before deadline", mtrc_mrp_tick(&A, 100, out, sizeof(out), &ol)==MTRC_MRP_IDLE);

  // advance well past each deadline; count retransmits until timeout
  int retx = 0; uint32_t now = 0; int timed_out = 0;
  for (int i = 0; i < 10; i++) {
    now += 100000;  // jump far ahead so the next retransmit is always due
    mtrc_mrp_tick_result r = mtrc_mrp_tick(&A, now, out, sizeof(out), &ol);
    if (r == MTRC_MRP_RETRANSMIT) {
      retx++;
      if (memcmp(out, frame, sizeof(frame)) != 0 || ol != sizeof(frame)) {
        chk("retransmit frame identical", 0); return;
      }
    } else if (r == MTRC_MRP_TIMEOUT) { timed_out = 1; break; }
  }
  chk("4 retransmits then timeout", retx == 4 && timed_out);
  chk("pending cleared after timeout", !A.tx_pending);
}

static void test_mrp_dedup(void) {
  printf("(B3) MRP duplicate / replay detection\n");
  mtrc_mrp_ctx B; mtrc_mrp_init(&B, 0);
  chk("ctr 2 fresh",        !mtrc_mrp_is_duplicate(&B, 2));
  chk("ctr 2 duplicate",     mtrc_mrp_is_duplicate(&B, 2));
  chk("ctr 5 fresh",        !mtrc_mrp_is_duplicate(&B, 5));
  chk("ctr 3 (in window)",  !mtrc_mrp_is_duplicate(&B, 3));
  chk("ctr 3 duplicate",     mtrc_mrp_is_duplicate(&B, 3));
  chk("ctr 5 duplicate",     mtrc_mrp_is_duplicate(&B, 5));
  chk("ctr 6 fresh",        !mtrc_mrp_is_duplicate(&B, 6));
}

int main(void) {
  printf("matter_c message layer self-test (frame + MRP)\n");
  test_frame();
  test_mrp_ack();
  test_mrp_retransmit();
  test_mrp_dedup();
  printf("\n==> message layer %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
