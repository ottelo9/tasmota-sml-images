// test_pase.c — Phase-3a self-test: Matter PASE key schedule + messages.
//
//  (A) PBKDF2-HMAC-SHA256 + w0/w1 vs an independent reference (Python
//      hashlib.pbkdf2_hmac; passcode 20202021, salt "SaltSaltSaltSalt",
//      1000 iters).
//  (B) PASE message TLV round-trips (PBKDFParamReq/Resp, Pake1/2/3).
//  (C) Full prover<->verifier handshake: Z/V agree, and both sides derive
//      IDENTICAL confirmation MACs + session keys (I2R/R2I/AttChallenge).
//
// Build/run host-side: see test/build_pase.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"
#include "mtrc_spake2p.h"
#include "mtrc_pase.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }
static int hex2bin(const char *h, uint8_t *o, size_t m){ size_t n=strlen(h)/2; if(n>m)return -1;
  for(size_t i=0;i<n;i++){unsigned v;sscanf(h+2*i,"%2x",&v);o[i]=(uint8_t)v;} return (int)n; }
static int eqhex(const uint8_t *b, size_t n, const char *h){ uint8_t e[128]; int m=hex2bin(h,e,sizeof(e));
  return m==(int)n && memcmp(b,e,n)==0; }

static void test_pbkdf(void) {
  printf("(A) PBKDF2 + w0/w1 vs reference\n");
  uint8_t pin[4] = {0x25,0x42,0x34,0x01};  // 20202021 (0x01344225) little-endian
  const uint8_t *salt = (const uint8_t*)"SaltSaltSaltSalt";
  uint8_t ws[80];
  mtrc_pbkdf2_sha256(pin, 4, salt, 16, 1000, ws, 80);
  chk("PBKDF2(80)", eqhex(ws, 80,
    "da303cefb297875e6328f02e716ef11b55fd68bf79b51e63210d5fc0ec11cd8e"
    "d3eed3529b366de90a2bef5f3034704f8c69a55a3517e8eb8a3a323fe796512d"
    "64b542f896c8f50e0d329365c830a60a"));

  uint8_t w0[32], w1[32];
  mtrc_pase_derive_w0w1(20202021, salt, 16, 1000, w0, w1);
  chk("w0 = WS[0:40] mod n",  eqhex(w0,32,"15c0778be4a72ccd8f2d5bf1827601d1c69c8d98e5af1bd13f9b61437e780b3b"));
  chk("w1 = WS[40:80] mod n", eqhex(w1,32,"bc9e15a8fab7893d8ce4b06655721c3f0f1a48281cf9d6899522d384199439fc"));
}

static void test_messages(void) {
  printf("(B) PASE message round-trips\n");
  uint8_t buf[256];

  mtrc_pase_param_req req; memset(&req,0,sizeof(req));
  for(int i=0;i<32;i++) req.initiator_random[i]=(uint8_t)(0xA0+i);
  req.initiator_session_id=0x1234; req.passcode_id=0; req.has_pbkdf_parameters=false;
  int n = mtrc_pase_encode_param_req(buf,sizeof(buf),&req);
  mtrc_pase_param_req rq2; int ok = n>0 && mtrc_pase_decode_param_req(buf,(size_t)n,&rq2)
    && memcmp(req.initiator_random,rq2.initiator_random,32)==0
    && rq2.initiator_session_id==0x1234 && rq2.passcode_id==0 && rq2.has_pbkdf_parameters==false;
  chk("PBKDFParamRequest", ok);

  mtrc_pase_param_resp rsp; memset(&rsp,0,sizeof(rsp));
  for(int i=0;i<32;i++){ rsp.initiator_random[i]=(uint8_t)(0xA0+i); rsp.responder_random[i]=(uint8_t)(0x50+i); }
  rsp.responder_session_id=0x5678; rsp.iterations=1000;
  memcpy(rsp.salt,"SaltSaltSaltSalt",16); rsp.salt_len=16;
  n = mtrc_pase_encode_param_resp(buf,sizeof(buf),&rsp);
  mtrc_pase_param_resp rs2; ok = n>0 && mtrc_pase_decode_param_resp(buf,(size_t)n,&rs2)
    && memcmp(rsp.responder_random,rs2.responder_random,32)==0
    && rs2.responder_session_id==0x5678 && rs2.iterations==1000
    && rs2.salt_len==16 && memcmp(rs2.salt,"SaltSaltSaltSalt",16)==0;
  chk("PBKDFParamResponse", ok);

  uint8_t pA[65], pB[65], cA[32], cB[32];
  for(int i=0;i<65;i++){ pA[i]=(uint8_t)i; pB[i]=(uint8_t)(0x80+i); }
  for(int i=0;i<32;i++){ cA[i]=(uint8_t)(0x10+i); cB[i]=(uint8_t)(0x20+i); }
  uint8_t o1[80],o2[80];
  n = mtrc_pase_encode_pake1(buf,sizeof(buf),pA);
  ok = n>0 && mtrc_pase_decode_pake1(buf,(size_t)n,o1) && memcmp(pA,o1,65)==0;
  chk("Pake1 (pA)", ok);
  n = mtrc_pase_encode_pake2(buf,sizeof(buf),pB,cB);
  uint8_t pB2[65],cB2[32];
  ok = n>0 && mtrc_pase_decode_pake2(buf,(size_t)n,pB2,cB2) && memcmp(pB,pB2,65)==0 && memcmp(cB,cB2,32)==0;
  chk("Pake2 (pB,cB)", ok);
  n = mtrc_pase_encode_pake3(buf,sizeof(buf),cA);
  ok = n>0 && mtrc_pase_decode_pake3(buf,(size_t)n,o2) && memcmp(cA,o2,32)==0;
  chk("Pake3 (cA)", ok);
}

static void test_handshake(void) {
  printf("(C) full prover<->verifier handshake -> matching session keys\n");
  const uint8_t *salt=(const uint8_t*)"SaltSaltSaltSalt";
  uint8_t w0[32], w1[32];
  mtrc_pase_derive_w0w1(20202021, salt, 16, 1000, w0, w1);

  // ephemeral scalars (fixed for the test; both < n)
  uint8_t x[32], y[32];
  memset(x,0x11,32); memset(y,0x22,32);

  // verifier registration record L = w1*G
  uint8_t L[65]; chk("L = w1*G", mtrc_ec_mulgen(L, w1, 32));

  // round one
  uint8_t pA[65], pB[65];
  chk("prover X",   mtrc_spake2p_prover_X(w0, x, pA));
  chk("verifier Y", mtrc_spake2p_verifier_Y(w0, y, pB));

  // round two: shared Z, V from each side
  uint8_t Zp[65],Vp[65],Zv[65],Vv[65];
  chk("prover Z,V",   mtrc_spake2p_prover_ZV(w0, w1, x, pB, Zp, Vp));
  chk("verifier Z,V", mtrc_spake2p_verifier_ZV(w0, y, pA, L, Zv, Vv));
  chk("Z agrees", memcmp(Zp,Zv,65)==0);
  chk("V agrees", memcmp(Vp,Vv,65)==0);

  // shared transcript context (from the two param messages)
  uint8_t req[128], resp[256], ctx[32];
  mtrc_pase_param_req  rq; memset(&rq,0,sizeof(rq)); rq.initiator_session_id=1;
  mtrc_pase_param_resp rs; memset(&rs,0,sizeof(rs)); rs.responder_session_id=2;
  rs.iterations=1000; memcpy(rs.salt,salt,16); rs.salt_len=16;
  int rn = mtrc_pase_encode_param_req(req,sizeof(req),&rq);
  int sn = mtrc_pase_encode_param_resp(resp,sizeof(resp),&rs);
  chk("param messages encode", rn>0 && sn>0);
  mtrc_pase_context(req,(size_t)rn, resp,(size_t)sn, ctx);

  // each side derives the full key set
  mtrc_pase_keys_t kp, kv;
  chk("prover keys",   mtrc_pase_keys(ctx, pA, pB, Zp, Vp, w0, &kp));
  chk("verifier keys", mtrc_pase_keys(ctx, pA, pB, Zv, Vv, w0, &kv));

  chk("cA matches (verifier confirms prover)", memcmp(kp.cA, kv.cA, 32)==0);
  chk("cB matches (prover confirms verifier)", memcmp(kp.cB, kv.cB, 32)==0);
  chk("I2RKey matches",            memcmp(kp.i2r, kv.i2r, 16)==0);
  chk("R2IKey matches",            memcmp(kp.r2i, kv.r2i, 16)==0);
  chk("AttestationChallenge match",memcmp(kp.att, kv.att, 16)==0);
  // sanity: keys are not all-zero
  uint8_t z16[16]={0};
  chk("session keys non-trivial",  memcmp(kp.i2r,z16,16)!=0 && memcmp(kp.r2i,z16,16)!=0);
}

int main(void) {
  printf("matter_c PASE self-test (key schedule + messages)\n");
  test_pbkdf();
  test_messages();
  test_handshake();
  printf("\n==> PASE %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
