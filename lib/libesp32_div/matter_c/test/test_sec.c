// test_sec.c — Phase-3b self-test: AES-CCM + Matter secured message layer.
//
//  (A) AES-128-CCM (13-byte nonce, 16-byte tag) vs an independent
//      reference (Python cryptography AESCCM): exact ct+tag, decrypt,
//      and tamper-detect.
//  (B) Secured message round-trip via mtrc_sec_encode/decode: recovers
//      header + protocol header + payload; tampering the ciphertext, the
//      header (AAD), or using the wrong key all fail the MIC.
//
// Build/run host-side: see test/build_sec.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"
#include "mtrc_sec.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }
static int hex2bin(const char *h, uint8_t *o, size_t m){ size_t n=strlen(h)/2; if(n>m)return -1;
  for(size_t i=0;i<n;i++){unsigned v;sscanf(h+2*i,"%2x",&v);o[i]=(uint8_t)v;} return (int)n; }
static int eqhex(const uint8_t *b, size_t n, const char *h){ uint8_t e[128]; int m=hex2bin(h,e,sizeof(e));
  return m==(int)n && memcmp(b,e,n)==0; }
static int contains(const uint8_t *hay, size_t hn, const uint8_t *need, size_t nn){
  if (nn>hn) return 0;
  for (size_t i=0;i+nn<=hn;i++) if (memcmp(hay+i,need,nn)==0) return 1;
  return 0; }

static void test_ccm(void) {
  printf("(A) AES-128-CCM vs reference\n");
  uint8_t key[16], nonce[13], aad[10], pt[16], data[16], tag[16];
  for(int i=0;i<16;i++) key[i]=(uint8_t)i;
  for(int i=0;i<13;i++) nonce[i]=(uint8_t)(0x20+i);
  for(int i=0;i<10;i++) aad[i]=(uint8_t)(0x40+i);
  for(int i=0;i<16;i++) pt[i]=(uint8_t)(0x80+i);

  memcpy(data, pt, 16);
  chk("encrypt ok", mtrc_aes_ccm_encrypt(key,nonce,13,aad,10,data,16,tag,16));
  chk("ciphertext", eqhex(data,16,"ea341045bff728e6ba3c56fdd8aa94ec"));
  chk("tag",        eqhex(tag, 16,"50773d7114098a741338daa7d8d3b4a4"));

  chk("decrypt ok + authentic", mtrc_aes_ccm_decrypt(key,nonce,13,aad,10,data,16,tag,16));
  chk("decrypt recovers pt", memcmp(data,pt,16)==0);

  // tamper detection
  memcpy(data, pt, 16);
  mtrc_aes_ccm_encrypt(key,nonce,13,aad,10,data,16,tag,16);
  data[0] ^= 0x01;
  chk("tampered ciphertext rejected", !mtrc_aes_ccm_decrypt(key,nonce,13,aad,10,data,16,tag,16));
}

static void test_secured_message(void) {
  printf("(B) secured message round-trip\n");
  uint8_t key[16]; for(int i=0;i<16;i++) key[i]=(uint8_t)(0xC0+i);

  mtrc_msg_header mh; memset(&mh,0,sizeof(mh));
  mh.session_id=0x1A2B; mh.session_type=0; mh.msg_counter=0x01020304;
  mh.dsiz=MTRC_DSIZ_NONE;             // PASE secure session: no node ids
  mtrc_proto_header ph; memset(&ph,0,sizeof(ph));
  ph.initiator=true; ph.reliability=true; ph.opcode=0x05; ph.exchange_id=0x77;
  ph.protocol_id=MTRC_PROTO_IM;
  const uint8_t payload[] = {0xDE,0xAD,0xBE,0xEF,0x11,0x22};

  uint8_t msg[256];
  int n = mtrc_sec_encode(msg,sizeof(msg),&mh,&ph,payload,sizeof(payload),key);
  chk("encode ok", n>0);

  mtrc_msg_header dm; mtrc_proto_header dp; const uint8_t *dpl; size_t dpl_len;
  uint8_t pt[256];
  int ok = mtrc_sec_decode(msg,(size_t)n,key,&dm,&dp,pt,sizeof(pt),&dpl,&dpl_len);
  chk("decode + MIC ok", ok);
  chk("header recovered", dm.session_id==0x1A2B && dm.msg_counter==0x01020304);
  chk("proto recovered",  dp.initiator && dp.reliability && dp.opcode==0x05
                          && dp.exchange_id==0x77 && dp.protocol_id==MTRC_PROTO_IM);
  chk("payload recovered", dpl_len==sizeof(payload) && memcmp(dpl,payload,sizeof(payload))==0);

  // ciphertext must not equal plaintext (i.e. it really encrypted)
  chk("payload was encrypted on the wire",
      !contains(msg,(size_t)n,payload,sizeof(payload)));

  // tamper a ciphertext byte (after the header) -> MIC fail
  uint8_t t1[256]; memcpy(t1,msg,(size_t)n); t1[n-20] ^= 0x01;
  chk("tampered ciphertext rejected",
      !mtrc_sec_decode(t1,(size_t)n,key,&dm,&dp,pt,sizeof(pt),&dpl,&dpl_len));

  // tamper a header (AAD) byte -> MIC fail
  uint8_t t2[256]; memcpy(t2,msg,(size_t)n); t2[1] ^= 0x01;   // session id byte
  chk("tampered header(AAD) rejected",
      !mtrc_sec_decode(t2,(size_t)n,key,&dm,&dp,pt,sizeof(pt),&dpl,&dpl_len));

  // wrong key -> MIC fail
  uint8_t wrong[16]; memcpy(wrong,key,16); wrong[0]^=0xFF;
  chk("wrong key rejected",
      !mtrc_sec_decode(msg,(size_t)n,wrong,&dm,&dp,pt,sizeof(pt),&dpl,&dpl_len));
}

int main(void) {
  printf("matter_c secured message self-test (AES-CCM)\n");
  test_ccm();
  test_secured_message();
  printf("\n==> secured message %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
