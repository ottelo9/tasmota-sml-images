// test_case_msg.c — CASE Sigma message TLV round-trips.
// Build/run host-side: see test/build_case_msg.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_case_msg.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }

int main(void) {
  printf("matter_c CASE message codec self-test\n");
  uint8_t buf[512];

  // Sigma1
  mtrc_sigma1 s1; memset(&s1,0,sizeof(s1));
  for(int i=0;i<32;i++){ s1.initiator_random[i]=(uint8_t)i; s1.destination_id[i]=(uint8_t)(0x40+i); }
  for(int i=0;i<65;i++) s1.initiator_eph_pub[i]=(uint8_t)(0x80+i);
  s1.initiator_session_id=0x1234;
  int n=mtrc_sigma1_encode(buf,sizeof(buf),&s1);
  mtrc_sigma1 d1;
  chk("Sigma1 round-trip", n>0 && mtrc_sigma1_decode(buf,(size_t)n,&d1)
      && memcmp(s1.initiator_random,d1.initiator_random,32)==0
      && d1.initiator_session_id==0x1234
      && memcmp(s1.destination_id,d1.destination_id,32)==0
      && memcmp(s1.initiator_eph_pub,d1.initiator_eph_pub,65)==0);

  // Sigma2 (opaque encrypted2)
  uint8_t enc2[120]; for(size_t i=0;i<sizeof(enc2);i++) enc2[i]=(uint8_t)(i*7);
  mtrc_sigma2 s2; memset(&s2,0,sizeof(s2));
  for(int i=0;i<32;i++) s2.responder_random[i]=(uint8_t)(0x10+i);
  for(int i=0;i<65;i++) s2.responder_eph_pub[i]=(uint8_t)(0x90+i);
  s2.responder_session_id=0x5678; s2.encrypted2=enc2; s2.encrypted2_len=sizeof(enc2);
  n=mtrc_sigma2_encode(buf,sizeof(buf),&s2);
  mtrc_sigma2 d2;
  chk("Sigma2 round-trip", n>0 && mtrc_sigma2_decode(buf,(size_t)n,&d2)
      && memcmp(s2.responder_random,d2.responder_random,32)==0
      && d2.responder_session_id==0x5678
      && memcmp(s2.responder_eph_pub,d2.responder_eph_pub,65)==0
      && d2.encrypted2_len==sizeof(enc2) && memcmp(d2.encrypted2,enc2,sizeof(enc2))==0);

  // Sigma3 (opaque encrypted3)
  uint8_t enc3[200]; for(size_t i=0;i<sizeof(enc3);i++) enc3[i]=(uint8_t)(0xFF-i);
  mtrc_sigma3 s3 = { enc3, sizeof(enc3) }, d3;
  n=mtrc_sigma3_encode(buf,sizeof(buf),&s3);
  chk("Sigma3 round-trip", n>0 && mtrc_sigma3_decode(buf,(size_t)n,&d3)
      && d3.encrypted3_len==sizeof(enc3) && memcmp(d3.encrypted3,enc3,sizeof(enc3))==0);

  // TBEData with ICAC + resumptionID
  uint8_t noc[140], icac[120], rid[16];
  for(size_t i=0;i<sizeof(noc);i++) noc[i]=(uint8_t)i;
  for(size_t i=0;i<sizeof(icac);i++) icac[i]=(uint8_t)(0x30+i);
  for(int i=0;i<16;i++) rid[i]=(uint8_t)(0xA0+i);
  mtrc_case_tbe tbe; memset(&tbe,0,sizeof(tbe));
  tbe.noc=noc; tbe.noc_len=sizeof(noc); tbe.icac=icac; tbe.icac_len=sizeof(icac);
  for(int i=0;i<64;i++) tbe.signature[i]=(uint8_t)(0x11+i);
  tbe.resumption_id=rid; tbe.resumption_id_len=16;
  n=mtrc_case_tbe_encode(buf,sizeof(buf),&tbe);
  mtrc_case_tbe dt;
  chk("TBEData round-trip (NOC+ICAC+sig+rid)", n>0 && mtrc_case_tbe_decode(buf,(size_t)n,&dt)
      && dt.noc_len==sizeof(noc) && memcmp(dt.noc,noc,sizeof(noc))==0
      && dt.icac_len==sizeof(icac) && memcmp(dt.icac,icac,sizeof(icac))==0
      && memcmp(dt.signature,tbe.signature,64)==0
      && dt.resumption_id_len==16 && memcmp(dt.resumption_id,rid,16)==0);

  // TBEData WITHOUT optional ICAC/resumptionID -> they must be absent
  mtrc_case_tbe tbe2; memset(&tbe2,0,sizeof(tbe2));
  tbe2.noc=noc; tbe2.noc_len=sizeof(noc);
  for(int i=0;i<64;i++) tbe2.signature[i]=(uint8_t)i;
  n=mtrc_case_tbe_encode(buf,sizeof(buf),&tbe2);
  mtrc_case_tbe dt2;
  chk("TBEData round-trip (no ICAC/rid)", n>0 && mtrc_case_tbe_decode(buf,(size_t)n,&dt2)
      && dt2.noc_len==sizeof(noc) && dt2.icac_len==0 && dt2.resumption_id_len==0);

  // TBSData encode is deterministic -> two encodes match
  mtrc_case_tbs tbs; memset(&tbs,0,sizeof(tbs));
  tbs.noc=noc; tbs.noc_len=sizeof(noc); tbs.icac=icac; tbs.icac_len=sizeof(icac);
  for(int i=0;i<65;i++){ tbs.sender_pub[i]=(uint8_t)i; tbs.receiver_pub[i]=(uint8_t)(0x80+i); }
  uint8_t b1[512], b2[512];
  int n1=mtrc_case_tbs_encode(b1,sizeof(b1),&tbs);
  int n2=mtrc_case_tbs_encode(b2,sizeof(b2),&tbs);
  chk("TBSData encode deterministic", n1>0 && n1==n2 && memcmp(b1,b2,(size_t)n1)==0);

  printf("\n==> CASE message codec %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
