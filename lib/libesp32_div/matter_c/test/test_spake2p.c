// test_spake2p.c — Phase-0 spike self-test for matter_c SPAKE2+.
//
// Validates the BearSSL EC + SHA/HKDF/HMAC stack and our SPAKE2+ math
// against the RFC 9383 P256-SHA256-HKDF-HMAC test vector. This is the
// project go/no-go gate: if X, Y, Z, V and K_main match, the hard crypto
// foundation is proven on the exact library the firmware will use.
//
// Build/run host-side: see test/build_host.sh. Not compiled into firmware.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"
#include "mtrc_spake2p.h"

static int hex2bin(const char *h, uint8_t *out, size_t outmax) {
  size_t n = strlen(h) / 2;
  if (n > outmax) return -1;
  for (size_t i = 0; i < n; i++) {
    unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (uint8_t)v;
  }
  return (int)n;
}

static int check(const char *label, const uint8_t *got, const char *exphex) {
  uint8_t exp[96]; int n = hex2bin(exphex, exp, sizeof(exp));
  int ok = (n > 0) && (memcmp(got, exp, (size_t)n) == 0);
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", label);
  if (!ok) {
    printf("    expected: %s\n    got:      ", exphex);
    for (int i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n");
  }
  return ok;
}

int main(void) {
  // ---- RFC 9383 P256-SHA256-HKDF-HMAC test vector ----
  const char *CONTEXT = "SPAKE2+-P256-SHA256-HKDF-SHA256-HMAC-SHA256 Test Vectors";
  const char *idProver = "client", *idVerifier = "server";
  uint8_t w0[32], w1[32], L[65], x[32], y[32];
  hex2bin("bb8e1bbcf3c48f62c08db243652ae55d3e5586053fca77102994f23ad95491b3", w0, 32);
  hex2bin("7e945f34d78785b8a3ef44d0df5a1a97d6b3b460409a345ca7830387a74b1dba", w1, 32);
  hex2bin("04eb7c9db3d9a9eb1f8adab81b5794c1f13ae3e225efbe91ea487425854c7fc00f"
          "00bfedcbd09b2400142d40a14f2064ef31dfaa903b91d1faea7093d835966efd", L, 65);
  hex2bin("d1232c8e8693d02368976c174e2088851b8365d0d79a9eee709c6a05a2fad539", x, 32);
  hex2bin("717a72348a182085109c8d3917d6c43d59b224dc6a7fc4f0483232fa6516d8b3", y, 32);

  const char *X_exp = "04ef3bd051bf78a2234ec0df197f7828060fe9856503579bb1733009042c15c0c1"
                      "de127727f418b5966afadfdd95a6e4591d171056b333dab97a79c7193e341727";
  const char *Y_exp = "04c0f65da0d11927bdf5d560c69e1d7d939a05b0e88291887d679fcadea75810fb"
                      "5cc1ca7494db39e82ff2f50665255d76173e09986ab46742c798a9a68437b048";
  const char *Z_exp = "04bbfce7dd7f277819c8da21544afb7964705569bdf12fb92aa388059408d50091"
                      "a0c5f1d3127f56813b5337f9e4e67e2ca633117a4fbd559946ab474356c41839";
  const char *V_exp = "0458bf27c6bca011c9ce1930e8984a797a3419797b936629a5a937cf2f11c8b951"
                      "4b82b993da8a46e664f23db7c01edc87faa530db01c2ee405230b18997f16b68";
  const char *KMAIN_exp     = "4c59e1ccf2cfb961aa31bd9434478a1089b56cd11542f53d3576fb6c2a438a29";
  const char *KCONFIRMP_exp = "871ae3f7b78445e34438fb284504240239031c39d80ac23eb5ab9be5ad6db58a";
  const char *KCONFIRMV_exp = "ccd53c7c1fa37b64a462b40db8be101cedcf838950162902054e644b400f1680";
  const char *CA_exp        = "926cc713504b9b4d76c9162ded04b5493e89109f6d89462cd33adc46fda27527";
  const char *CB_exp        = "9747bcc4f8fe9f63defee53ac9b07876d907d55047e6ff2def2e7529089d3e68";

  int ok = 1;
  uint8_t X[65], Y[65], Zp[65], Vp[65], Zv[65], Vv[65];

  printf("matter_c SPAKE2+ spike (RFC 9383 P256) — BearSSL br_ec_p256_m15\n");

  // 1. Prover X = x*G + w0*M   (proves muladd)
  ok &= mtrc_spake2p_prover_X(w0, x, X);    ok &= check("X = x*G + w0*M", X, X_exp);
  // 2. Verifier Y = y*G + w0*N
  ok &= mtrc_spake2p_verifier_Y(w0, y, Y);  ok &= check("Y = y*G + w0*N", Y, Y_exp);
  // 3. Prover Z,V  (proves point subtraction + scalar mul)
  ok &= mtrc_spake2p_prover_ZV(w0, w1, x, Y, Zp, Vp);
  ok &= check("Z (prover)", Zp, Z_exp);
  ok &= check("V (prover)", Vp, V_exp);
  // 4. Verifier Z,V — must equal prover's (proves both sides agree + L usage)
  ok &= mtrc_spake2p_verifier_ZV(w0, y, X, L, Zv, Vv);
  ok &= check("Z (verifier)", Zv, Z_exp);
  ok &= check("V (verifier)", Vv, V_exp);

  // 5. Transcript + K_main = SHA256(TT)   (proves transcript + SHA-256)
  uint8_t K_main[32];
  ok &= mtrc_spake2p_transcript(
      (const uint8_t*)CONTEXT, strlen(CONTEXT),
      (const uint8_t*)idProver, strlen(idProver),
      (const uint8_t*)idVerifier, strlen(idVerifier),
      X, Y, Zp, Vp, w0, K_main);
  ok &= check("K_main = SHA256(TT)", K_main, KMAIN_exp);

  // 6. Confirmation keys + MACs   (proves HKDF + HMAC)
  uint8_t KcP[32], KcV[32], cA[32], cB[32];
  ok &= mtrc_spake2p_confirm(K_main, X, Y, KcP, KcV, cA, cB);
  ok &= check("K_confirmP", KcP, KCONFIRMP_exp);
  ok &= check("K_confirmV", KcV, KCONFIRMV_exp);
  ok &= check("cA = HMAC(KcP, Y)", cA, CA_exp);
  ok &= check("cB = HMAC(KcV, X)", cB, CB_exp);

  printf("\n==> SPAKE2+ spike %s\n", ok ? "PASS — GO" : "FAIL");
  return ok ? 0 : 1;
}
