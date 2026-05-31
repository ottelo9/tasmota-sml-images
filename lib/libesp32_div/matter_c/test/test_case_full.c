// test_case_full.c — full CASE (Sigma1/2/3) handshake, end-to-end in C.
//
// Runs an initiator AND a responder against each other using the matter_c
// CASE building blocks (mtrc_case schedule, mtrc_case_msg TLV codecs,
// mtrc_crypto EC/CCM). Proves the whole operational-session establishment
// before it is wired into the device + the Python prover:
//   destinationId match -> ECDH -> S2K -> Sigma2 (TBE seal + TBS sign)
//   -> S3K -> Sigma3 -> mutual ECDSA verify -> identical session keys.
//
// NOCs are opaque octet blobs here (the CASE flow treats them as opaque;
// compact-TLV cert chain verification is A1b, a separate step). The TBS
// signatures are verified with the peers' operational public keys.
//
// Build/run host-side: see test/build_case_full.sh. GPLv3.

#include <stdio.h>
#include <string.h>
#include "mtrc_crypto.h"
#include "mtrc_case.h"
#include "mtrc_case_msg.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }

// Seal a TBE plaintext: CCM(key, nonce, aad="") -> ct||tag into out.
static int seal(const uint8_t key[16], const uint8_t nonce[13],
                const uint8_t *pt, size_t pt_len, uint8_t *out) {
  memcpy(out, pt, pt_len);
  return mtrc_aes_ccm_encrypt(key, nonce, 13, NULL, 0, out, pt_len, out + pt_len, 16);
}
// Open a sealed blob (ct||tag) in place -> plaintext in out (len = blob-16).
static int open_blob(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *blob, size_t blob_len, uint8_t *out) {
  size_t ct = blob_len - 16;
  memcpy(out, blob, ct);
  return mtrc_aes_ccm_decrypt(key, nonce, 13, NULL, 0, out, ct, blob + ct, 16);
}

int main(void) {
  printf("matter_c full CASE handshake (initiator <-> responder)\n");

  // ---- shared fabric parameters ----
  uint8_t  ipk[16];          memset(ipk, 0xC5, 16);
  uint64_t fabric_id = 0x0000FAB000000001ULL;
  uint64_t dev_node  = 0x1122334455667788ULL;   // responder (device) node id
  uint8_t  root_priv[32];    memset(root_priv, 0x07, 32);
  uint8_t  root_pub[65];     chk("root pub", mtrc_ec_pub_from_priv(root_pub, root_priv));

  // operational keypairs
  uint8_t dev_priv[32]; memset(dev_priv, 0x11, 32); uint8_t dev_pub[65];
  uint8_t ctl_priv[32]; memset(ctl_priv, 0x22, 32); uint8_t ctl_pub[65];
  chk("dev op pub", mtrc_ec_pub_from_priv(dev_pub, dev_priv));
  chk("ctl op pub", mtrc_ec_pub_from_priv(ctl_pub, ctl_priv));

  // opaque NOC blobs (cert verify is A1b; here they are just signed-over bytes)
  uint8_t dev_noc[48]; for (int i=0;i<48;i++) dev_noc[i]=(uint8_t)(0x40+i);
  uint8_t ctl_noc[48]; for (int i=0;i<48;i++) ctl_noc[i]=(uint8_t)(0x80+i);

  uint8_t buf1[512], buf2[512], buf3[512], tmp[512];

  // ---- Sigma1 (initiator) ----
  uint8_t ie_priv[32]; memset(ie_priv, 0x33, 32); uint8_t ie_pub[65];
  mtrc_ec_pub_from_priv(ie_pub, ie_priv);
  mtrc_sigma1 s1; memset(&s1, 0, sizeof(s1));
  memset(s1.initiator_random, 0xA1, 32);
  s1.initiator_session_id = 0x1111;
  memcpy(s1.initiator_eph_pub, ie_pub, 65);
  mtrc_case_destination_id(ipk, s1.initiator_random, root_pub, fabric_id, dev_node,
                           s1.destination_id);
  int n1 = mtrc_sigma1_encode(buf1, sizeof(buf1), &s1);
  chk("sigma1 encode", n1 > 0);
  uint8_t h1[32]; mtrc_sha256(buf1, (size_t)n1, h1);

  // ---- responder processes Sigma1 ----
  mtrc_sigma1 r1; chk("sigma1 decode", mtrc_sigma1_decode(buf1, (size_t)n1, &r1));
  uint8_t cand[32];
  mtrc_case_destination_id(ipk, r1.initiator_random, root_pub, fabric_id, dev_node, cand);
  chk("destinationId matches fabric", memcmp(cand, r1.destination_id, 32) == 0);

  uint8_t re_priv[32]; memset(re_priv, 0x44, 32); uint8_t re_pub[65];
  mtrc_ec_pub_from_priv(re_pub, re_priv);
  uint8_t resp_random[32]; memset(resp_random, 0xB2, 32);
  uint8_t shared_r[32]; chk("resp ECDH", mtrc_ecdh(shared_r, r1.initiator_eph_pub, re_priv));
  uint8_t s2k_r[16];
  chk("resp S2K", mtrc_case_s2k(shared_r, ipk, resp_random, re_pub, h1, s2k_r));

  // TBSData2 = {noc, sender=respEph, receiver=initEph} -> sign with dev_priv
  mtrc_case_tbs tbs2; memset(&tbs2, 0, sizeof(tbs2));
  tbs2.noc = dev_noc; tbs2.noc_len = sizeof(dev_noc);
  memcpy(tbs2.sender_pub, re_pub, 65); memcpy(tbs2.receiver_pub, r1.initiator_eph_pub, 65);
  int nt2 = mtrc_case_tbs_encode(tmp, sizeof(tmp), &tbs2);
  uint8_t ht2[32]; mtrc_sha256(tmp, (size_t)nt2, ht2);
  mtrc_case_tbe tbe2; memset(&tbe2, 0, sizeof(tbe2));
  tbe2.noc = dev_noc; tbe2.noc_len = sizeof(dev_noc);
  chk("resp sign TBS2", mtrc_ecdsa_sign(tbe2.signature, ht2, dev_priv));
  int ne2 = mtrc_case_tbe_encode(tmp, sizeof(tmp), &tbe2);
  uint8_t enc2[512];
  chk("seal TBE2", seal(s2k_r, MTRC_CASE_NONCE_SIGMA2, tmp, (size_t)ne2, enc2));
  mtrc_sigma2 s2; memset(&s2, 0, sizeof(s2));
  memcpy(s2.responder_random, resp_random, 32);
  s2.responder_session_id = 0x2222;
  memcpy(s2.responder_eph_pub, re_pub, 65);
  s2.encrypted2 = enc2; s2.encrypted2_len = (size_t)ne2 + 16;
  int n2 = mtrc_sigma2_encode(buf2, sizeof(buf2), &s2);
  chk("sigma2 encode", n2 > 0);

  // transcript hash Sigma1||Sigma2
  uint8_t cat12[1024]; memcpy(cat12, buf1, n1); memcpy(cat12+n1, buf2, n2);
  uint8_t h12[32]; mtrc_sha256(cat12, (size_t)(n1+n2), h12);

  // ---- initiator processes Sigma2 ----
  mtrc_sigma2 i2; chk("sigma2 decode", mtrc_sigma2_decode(buf2, (size_t)n2, &i2));
  uint8_t shared_i[32]; mtrc_ecdh(shared_i, i2.responder_eph_pub, ie_priv);
  chk("ECDH agree", memcmp(shared_i, shared_r, 32) == 0);
  uint8_t s2k_i[16];
  mtrc_case_s2k(shared_i, ipk, i2.responder_random, i2.responder_eph_pub, h1, s2k_i);
  chk("S2K agree", memcmp(s2k_i, s2k_r, 16) == 0);
  uint8_t op2[512];
  chk("open TBE2", open_blob(s2k_i, MTRC_CASE_NONCE_SIGMA2, i2.encrypted2,
                             i2.encrypted2_len, op2));
  mtrc_case_tbe gt2; chk("TBE2 decode", mtrc_case_tbe_decode(op2, i2.encrypted2_len-16, &gt2));
  // verify responder signature over reconstructed TBS2
  mtrc_case_tbs vtbs2; memset(&vtbs2, 0, sizeof(vtbs2));
  vtbs2.noc = gt2.noc; vtbs2.noc_len = gt2.noc_len;
  memcpy(vtbs2.sender_pub, i2.responder_eph_pub, 65);
  memcpy(vtbs2.receiver_pub, ie_pub, 65);
  int nv2 = mtrc_case_tbs_encode(tmp, sizeof(tmp), &vtbs2);
  uint8_t hv2[32]; mtrc_sha256(tmp, (size_t)nv2, hv2);
  chk("verify responder NOC sig", mtrc_ecdsa_verify(gt2.signature, hv2, dev_pub));

  // ---- initiator builds Sigma3 ----
  mtrc_case_tbs tbs3; memset(&tbs3, 0, sizeof(tbs3));
  tbs3.noc = ctl_noc; tbs3.noc_len = sizeof(ctl_noc);
  memcpy(tbs3.sender_pub, ie_pub, 65); memcpy(tbs3.receiver_pub, i2.responder_eph_pub, 65);
  int nt3 = mtrc_case_tbs_encode(tmp, sizeof(tmp), &tbs3);
  uint8_t ht3[32]; mtrc_sha256(tmp, (size_t)nt3, ht3);
  mtrc_case_tbe tbe3; memset(&tbe3, 0, sizeof(tbe3));
  tbe3.noc = ctl_noc; tbe3.noc_len = sizeof(ctl_noc);
  mtrc_ecdsa_sign(tbe3.signature, ht3, ctl_priv);
  int ne3 = mtrc_case_tbe_encode(tmp, sizeof(tmp), &tbe3);
  uint8_t s3k_i[16]; mtrc_case_s3k(shared_i, ipk, h12, s3k_i);
  uint8_t enc3[512];
  seal(s3k_i, MTRC_CASE_NONCE_SIGMA3, tmp, (size_t)ne3, enc3);
  mtrc_sigma3 s3; memset(&s3, 0, sizeof(s3));
  s3.encrypted3 = enc3; s3.encrypted3_len = (size_t)ne3 + 16;
  int n3 = mtrc_sigma3_encode(buf3, sizeof(buf3), &s3);
  chk("sigma3 encode", n3 > 0);
  uint8_t cat123[1536]; memcpy(cat123, cat12, n1+n2); memcpy(cat123+n1+n2, buf3, n3);
  uint8_t hall[32]; mtrc_sha256(cat123, (size_t)(n1+n2+n3), hall);
  uint8_t i2r_i[16], r2i_i[16], att_i[16];
  mtrc_case_session_keys(shared_i, ipk, hall, i2r_i, r2i_i, att_i);

  // ---- responder processes Sigma3 ----
  mtrc_sigma3 r3; chk("sigma3 decode", mtrc_sigma3_decode(buf3, (size_t)n3, &r3));
  uint8_t s3k_r[16]; mtrc_case_s3k(shared_r, ipk, h12, s3k_r);
  uint8_t op3[512];
  chk("open TBE3", open_blob(s3k_r, MTRC_CASE_NONCE_SIGMA3, r3.encrypted3,
                             r3.encrypted3_len, op3));
  mtrc_case_tbe gt3; chk("TBE3 decode", mtrc_case_tbe_decode(op3, r3.encrypted3_len-16, &gt3));
  mtrc_case_tbs vtbs3; memset(&vtbs3, 0, sizeof(vtbs3));
  vtbs3.noc = gt3.noc; vtbs3.noc_len = gt3.noc_len;
  memcpy(vtbs3.sender_pub, ie_pub, 65);          // initiator was sender in Sigma3
  memcpy(vtbs3.receiver_pub, re_pub, 65);
  int nv3 = mtrc_case_tbs_encode(tmp, sizeof(tmp), &vtbs3);
  uint8_t hv3[32]; mtrc_sha256(tmp, (size_t)nv3, hv3);
  chk("verify initiator NOC sig", mtrc_ecdsa_verify(gt3.signature, hv3, ctl_pub));
  uint8_t i2r_r[16], r2i_r[16], att_r[16];
  mtrc_case_session_keys(shared_r, ipk, hall, i2r_r, r2i_r, att_r);

  // ---- both sides agree on the operational session keys ----
  chk("I2R agree", memcmp(i2r_i, i2r_r, 16) == 0);
  chk("R2I agree", memcmp(r2i_i, r2i_r, 16) == 0);
  chk("Att agree", memcmp(att_i, att_r, 16) == 0);

  // ---- sanity: a message sealed by the responder opens on the initiator ----
  const char *msg = "operational!";
  uint8_t nonce[13]; memset(nonce, 0, 13); nonce[0]=1;
  uint8_t ce[64]; size_t ml = strlen(msg);
  memcpy(ce, msg, ml);
  mtrc_aes_ccm_encrypt(r2i_r, nonce, 13, NULL, 0, ce, ml, ce+ml, 16);
  uint8_t cd[64]; memcpy(cd, ce, ml);
  int dec = mtrc_aes_ccm_decrypt(r2i_i, nonce, 13, NULL, 0, cd, ml, ce+ml, 16);
  chk("R2I app message round-trips", dec && memcmp(cd, msg, ml)==0);

  printf(g_ok ? "\n==> full CASE handshake PASS\n" : "\n==> full CASE handshake FAIL\n");
  return g_ok ? 0 : 1;
}
