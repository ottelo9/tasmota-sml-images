// test_case.c — Phase-3d(2): Matter CASE key schedule.
//
//  (A) destinationId = HMAC(IPK, ...) vs a Python reference.
//  (B) S2K / S3K / session keys vs Python references computed over a fixed
//      ECDH shared secret + transcript hashes.
//  (C) two-party ECDH: initiator and responder reach the same shared
//      secret from the ephemeral keypairs, and that secret is the one the
//      key-schedule references were built on.
//
// Build/run host-side: see test/build_case.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"
#include "mtrc_case.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }
static int hex2bin(const char *h, uint8_t *o, size_t m){ size_t n=strlen(h)/2; if(n>m)return -1;
  for(size_t i=0;i<n;i++){unsigned v;sscanf(h+2*i,"%2x",&v);o[i]=(uint8_t)v;} return (int)n; }
static int eqhex(const uint8_t *b, size_t n, const char *h){ uint8_t e[96]; int m=hex2bin(h,e,sizeof(e));
  return m==(int)n && memcmp(b,e,n)==0; }

int main(void) {
  printf("matter_c CASE key-schedule self-test\n");

  uint8_t ipk[16], initRand[32], respRand[32], rootPub[65], respEphPub[65];
  hex2bin("000102030405060708090a0b0c0d0e0f", ipk, 16);
  hex2bin("202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f", initRand, 32);
  hex2bin("505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f", respRand, 32);
  rootPub[0] = 0x04; memset(rootPub + 1, 0x11, 64);   // dummy uncompressed root pubkey
  uint64_t fabricId = 0x2906c908d115d362ULL, nodeId = 0x8fc7772401cd0696ULL;

  printf("(A) destinationId\n");
  uint8_t dest[32];
  mtrc_case_destination_id(ipk, initRand, rootPub, fabricId, nodeId, dest);
  chk("destinationId vs ref",
      eqhex(dest,32,"14b0ca5d63e0ca4b5f471a1f534966ac0df011b35aefc298fcc25cf9ee051ff5"));

  printf("(B) two-party ECDH -> shared secret\n");
  uint8_t di[32], dr[32], qi[65], qr[65], sh_i[32], sh_r[32];
  hex2bin("00112233445566778899aabbccddeeff00112233445566778899aabbccddee01", di, 32);
  hex2bin("0fedcba9876543210fedcba9876543210fedcba9876543210fedcba987654300", dr, 32);
  mtrc_ec_pub_from_priv(qi, di);
  mtrc_ec_pub_from_priv(qr, dr);
  chk("respEphPub vs ref",
      eqhex(qr,65,"04354fb7b585a83f741ad5cde0d8a2e113821d75ce23c3ba39756f7b26e25a111e"
                  "d3358ad8298ce2d0e17edad45108a9cffb084145581f88de7ba73be7d3c73cea"));
  mtrc_ecdh(sh_i, qr, di);   // initiator: di * respPub
  mtrc_ecdh(sh_r, qi, dr);   // responder: dr * initPub
  chk("ECDH both sides agree", memcmp(sh_i, sh_r, 32)==0);
  chk("shared vs ref",
      eqhex(sh_i,32,"0b2fa194f6be1f01f503b6c499bc24101625e15133fe9622d1803c1e17d20aaf"));
  memcpy(respEphPub, qr, 65);

  printf("(C) key schedule (S2K / S3K / session keys)\n");
  uint8_t h1[32], h12[32], h123[32];
  hex2bin("ee0296f3d83f79421dd6d827aa1eeb1073d46e8ce76039ad9a154045996096e4", h1, 32);
  hex2bin("fd9cddddb8ea5dbde38d625cc42e794288e9b04452450d3e620b3489a709763d", h12, 32);
  hex2bin("cb55a60d13f541e0c29786e96c884489e20a750b17dfa2a8a1a1d681eb195eb8", h123, 32);

  uint8_t s2k[16], s3k[16], i2r[16], r2i[16], att[16];
  chk("S2K vs ref", mtrc_case_s2k(sh_i, ipk, respRand, respEphPub, h1, s2k) &&
      eqhex(s2k,16,"7ff65b58aa8ea3ae4e599cd2e6fe4a57"));
  chk("S3K vs ref", mtrc_case_s3k(sh_i, ipk, h12, s3k) &&
      eqhex(s3k,16,"e5053ba5f2c13c146ead218c485408c7"));
  chk("session keys ok", mtrc_case_session_keys(sh_i, ipk, h123, i2r, r2i, att));
  chk("I2RKey vs ref",            eqhex(i2r,16,"71a63c275c2c0cbbcee5bd382f64fa78"));
  chk("R2IKey vs ref",            eqhex(r2i,16,"37a026a3239025a583191ede0ed3be45"));
  chk("AttestationChallenge vs ref", eqhex(att,16,"f88011cc87e01b3c0de7dac405866cba"));

  printf("(D) Sigma TBE nonces\n");
  chk("NCASE_Sigma2N", memcmp(MTRC_CASE_NONCE_SIGMA2,"NCASE_Sigma2N",13)==0);
  chk("NCASE_Sigma3N", memcmp(MTRC_CASE_NONCE_SIGMA3,"NCASE_Sigma3N",13)==0);

  printf("\n==> CASE key schedule %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
