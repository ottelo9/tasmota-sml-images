// test_ec.c — Phase-3d (CASE crypto): ECDH + ECDSA on P-256.
//
//  (A) ECDH: Alice/Bob agree on the same secret; X-coord matches a
//      reference (Python cryptography).
//  (B) ECDSA: deterministic signing matches the RFC 6979 P-256/SHA-256
//      'sample' vector exactly; sign->verify round-trip; tamper rejected.
//
// Build/run host-side: see test/build_ec.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }
static int hex2bin(const char *h, uint8_t *o, size_t m){ size_t n=strlen(h)/2; if(n>m)return -1;
  for(size_t i=0;i<n;i++){unsigned v;sscanf(h+2*i,"%2x",&v);o[i]=(uint8_t)v;} return (int)n; }
static int eqhex(const uint8_t *b, size_t n, const char *h){ uint8_t e[96]; int m=hex2bin(h,e,sizeof(e));
  return m==(int)n && memcmp(b,e,n)==0; }

static void test_ecdh(void) {
  printf("(A) ECDH P-256\n");
  uint8_t da[32], db[32], qa[65], qb[65];
  hex2bin("00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", da, 32);
  hex2bin("0fedcba9876543210fedcba9876543210fedcba9876543210fedcba987654321", db, 32);
  chk("pubA = dA*G", mtrc_ec_pub_from_priv(qa, da) &&
      eqhex(qa,65,"04798953e7e8134fdf3c139f63d3fbccc252a28b6ca5059e618374a81231240f3f"
                  "c83267aec725e18b66176c3685d1257201a67033819585a22a296350159ae70b"));
  chk("pubB = dB*G", mtrc_ec_pub_from_priv(qb, db) &&
      eqhex(qb,65,"045f697b6fa1e70a2ebba3788c38c5256b1602e2197c55b6a44601b3fc66a57ea9"
                  "65a92f96a8651d1f59cc5a7db253b64ef5b36bbeaa9015008c43e88bc4b94dec"));
  uint8_t sa[32], sb[32];
  chk("ECDH A", mtrc_ecdh(sa, qb, da));
  chk("ECDH B", mtrc_ecdh(sb, qa, db));
  chk("shared secrets agree", memcmp(sa,sb,32)==0);
  chk("shared X matches ref",
      eqhex(sa,32,"dfb4d060e9e07e9b8418b437d422d9d9c4d46ade1ac2ce8477e09a666f2e62bd"));
}

static void test_ecdsa(void) {
  printf("(B) ECDSA P-256 (deterministic, RFC 6979)\n");
  uint8_t priv[32], pub[65], hash[32], sig[64];
  hex2bin("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721", priv, 32);
  hex2bin("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
          "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299", pub, 65);
  hex2bin("af2bdbe1aa9b6ec1e2ade1d694f41fc71a831d0268e9891562113d8a62add1bf", hash, 32); // SHA256("sample")

  chk("sign ok", mtrc_ecdsa_sign(sig, hash, priv));
  chk("r matches RFC 6979", eqhex(sig,      32, "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"));
  chk("s matches RFC 6979", eqhex(sig + 32, 32, "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"));

  // determinism
  uint8_t sig2[64]; mtrc_ecdsa_sign(sig2, hash, priv);
  chk("deterministic", memcmp(sig, sig2, 64)==0);

  // verify (external pubkey)
  chk("verify ok", mtrc_ecdsa_verify(sig, hash, pub));

  // tamper signature -> reject
  uint8_t bad[64]; memcpy(bad, sig, 64); bad[0] ^= 0x01;
  chk("tampered sig rejected", !mtrc_ecdsa_verify(bad, hash, pub));
  // tamper hash -> reject
  uint8_t h2[32]; memcpy(h2, hash, 32); h2[0] ^= 0x01;
  chk("wrong hash rejected", !mtrc_ecdsa_verify(sig, h2, pub));

  // sign->verify round-trip on a different message
  uint8_t h3[32]; mtrc_sha256((const uint8_t*)"matter_c", 8, h3);
  uint8_t s3[64];
  chk("round-trip sign/verify",
      mtrc_ecdsa_sign(s3, h3, priv) && mtrc_ecdsa_verify(s3, h3, pub));
}

int main(void) {
  printf("matter_c EC self-test (ECDH + ECDSA for CASE)\n");
  test_ecdh();
  test_ecdsa();
  printf("\n==> EC (ECDH+ECDSA) %s\n", g_ok ? "PASS" : "FAIL");
  return g_ok ? 0 : 1;
}
