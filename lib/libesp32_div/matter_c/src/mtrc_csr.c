// mtrc_csr.c — PKCS#10 CSR (DER) builder. See mtrc_csr.h. GPLv3.

#include "mtrc_csr.h"
#include "mtrc_crypto.h"
#include <string.h>

// ---- minimal DER (X.690) helpers ---------------------------------------
// Write a DER length. Returns bytes written (1..3; CSRs are well under 64 KB).
static size_t der_len(uint8_t *p, size_t len) {
  if (len < 0x80)  { p[0] = (uint8_t)len; return 1; }
  if (len < 0x100) { p[0] = 0x81; p[1] = (uint8_t)len; return 2; }
  p[0] = 0x82; p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)len; return 3;
}
// Emit tag + length + content. Returns total bytes written.
static size_t der_wrap(uint8_t *out, uint8_t tag, const uint8_t *content, size_t clen) {
  size_t o = 0; out[o++] = tag; o += der_len(out + o, clen);
  memcpy(out + o, content, clen); o += clen; return o;
}
// Emit a DER INTEGER from a 32-byte big-endian scalar (minimal, sign-safe).
static size_t der_int(uint8_t *out, const uint8_t v[32]) {
  size_t i = 0; while (i < 31 && v[i] == 0) i++;       // strip leading zeros
  int pad = (v[i] & 0x80) ? 1 : 0;                      // keep it positive
  size_t clen = (32 - i) + (size_t)pad;
  size_t o = 0; out[o++] = 0x02; o += der_len(out + o, clen);
  if (pad) out[o++] = 0x00;
  memcpy(out + o, v + i, 32 - i); o += (32 - i);
  return o;
}

// AlgorithmIdentifier for an EC public key on prime256v1:
//   SEQ { OID id-ecPublicKey (1.2.840.10045.2.1), OID prime256v1 (..3.1.7) }
static const uint8_t ALG_ECPK[] = {
  0x30,0x13, 0x06,0x07,0x2A,0x86,0x48,0xCE,0x3D,0x02,0x01,
             0x06,0x08,0x2A,0x86,0x48,0xCE,0x3D,0x03,0x01,0x07 };
// AlgorithmIdentifier ecdsa-with-SHA256 (1.2.840.10045.4.3.2)
static const uint8_t ALG_ECDSA_SHA256[] = {
  0x30,0x0A, 0x06,0x08,0x2A,0x86,0x48,0xCE,0x3D,0x04,0x03,0x02 };

int mtrc_csr_build(uint8_t *out, size_t cap,
                   const uint8_t op_priv[32], const uint8_t op_pub[65]) {
  // SubjectPublicKeyInfo = SEQ { ALG_ECPK, BITSTRING(0x00 || op_pub) }
  uint8_t spki_c[128]; size_t sc = 0;
  memcpy(spki_c + sc, ALG_ECPK, sizeof(ALG_ECPK)); sc += sizeof(ALG_ECPK);
  uint8_t bits[3 + 1 + 65]; size_t bo = 0;
  bits[bo++] = 0x03; bo += der_len(bits + bo, 66); bits[bo++] = 0x00;
  memcpy(bits + bo, op_pub, 65); bo += 65;
  memcpy(spki_c + sc, bits, bo); sc += bo;
  uint8_t spki[160]; size_t sp = der_wrap(spki, 0x30, spki_c, sc);

  // CertificationRequestInfo = SEQ { INTEGER 0, Name{}, SPKI, [0]{} }
  static const uint8_t VER[]  = { 0x02,0x01,0x00 };   // version 0
  static const uint8_t SUBJ[] = { 0x30,0x00 };        // empty Name
  static const uint8_t ATTR[] = { 0xA0,0x00 };        // empty attributes [0]
  uint8_t cri_c[256]; size_t cc = 0;
  memcpy(cri_c + cc, VER, sizeof(VER));   cc += sizeof(VER);
  memcpy(cri_c + cc, SUBJ, sizeof(SUBJ)); cc += sizeof(SUBJ);
  memcpy(cri_c + cc, spki, sp);           cc += sp;
  memcpy(cri_c + cc, ATTR, sizeof(ATTR)); cc += sizeof(ATTR);
  uint8_t cri[300]; size_t cl = der_wrap(cri, 0x30, cri_c, cc);

  // signature = ECDSA(op_priv, SHA256(CertificationRequestInfo)), DER {r,s}
  uint8_t h[32]; mtrc_sha256(cri, cl, h);
  uint8_t raw[64]; if (!mtrc_ecdsa_sign(raw, h, op_priv)) return -1;
  uint8_t rint[40], sint[40];
  size_t rl = der_int(rint, raw), sl = der_int(sint, raw + 32);
  uint8_t sigseq_c[80]; memcpy(sigseq_c, rint, rl); memcpy(sigseq_c + rl, sint, sl);
  uint8_t sigseq[88]; size_t ssl = der_wrap(sigseq, 0x30, sigseq_c, rl + sl);
  uint8_t sigbits[92]; size_t sb = 0;
  sigbits[sb++] = 0x03; sb += der_len(sigbits + sb, ssl + 1); sigbits[sb++] = 0x00;
  memcpy(sigbits + sb, sigseq, ssl); sb += ssl;

  // CertificationRequest = SEQ { CRI, sigAlg, signature BITSTRING }
  uint8_t csr_c[512]; size_t xc = 0;
  memcpy(csr_c + xc, cri, cl);                          xc += cl;
  memcpy(csr_c + xc, ALG_ECDSA_SHA256, sizeof(ALG_ECDSA_SHA256));
  xc += sizeof(ALG_ECDSA_SHA256);
  memcpy(csr_c + xc, sigbits, sb);                      xc += sb;

  uint8_t full[600]; size_t total = der_wrap(full, 0x30, csr_c, xc);
  if (total > cap) return -1;
  memcpy(out, full, total);
  return (int)total;
}
