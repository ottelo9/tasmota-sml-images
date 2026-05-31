// mtrc_case.c — Matter CASE key schedule. See mtrc_case.h. GPLv3.
// Constants verbatim from connectedhomeip.

#include "mtrc_case.h"
#include "mtrc_crypto.h"
#include <string.h>

const uint8_t MTRC_CASE_NONCE_SIGMA2[13] =
  { 'N','C','A','S','E','_','S','i','g','m','a','2','N' };
const uint8_t MTRC_CASE_NONCE_SIGMA3[13] =
  { 'N','C','A','S','E','_','S','i','g','m','a','3','N' };

static const uint8_t INFO_SIGMA2[]  = { 'S','i','g','m','a','2' };
static const uint8_t INFO_SIGMA3[]  = { 'S','i','g','m','a','3' };
static const uint8_t INFO_SESSION[] = { 'S','e','s','s','i','o','n','K','e','y','s' };

static void put_le64(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

void mtrc_case_destination_id(const uint8_t ipk[16],
                              const uint8_t init_random[32],
                              const uint8_t root_pub[65],
                              uint64_t fabric_id, uint64_t node_id,
                              uint8_t out[32]) {
  // message = initRandom(32) || rootPubKey(65) || LE64(fabricId) || LE64(nodeId)
  uint8_t msg[32 + 65 + 8 + 8];
  size_t o = 0;
  memcpy(msg + o, init_random, 32); o += 32;
  memcpy(msg + o, root_pub, 65);    o += 65;
  put_le64(msg + o, fabric_id);     o += 8;
  put_le64(msg + o, node_id);       o += 8;
  mtrc_hmac_sha256(ipk, 16, msg, o, out);
}

int mtrc_case_s2k(const uint8_t shared[32], const uint8_t ipk[16],
                  const uint8_t resp_random[32], const uint8_t resp_eph_pub[65],
                  const uint8_t hash_sigma1[32], uint8_t s2k[16]) {
  // salt = IPK(16) || respRandom(32) || respEphPub(65) || SHA256(Σ1)(32)
  uint8_t salt[16 + 32 + 65 + 32]; size_t o = 0;
  memcpy(salt + o, ipk, 16);          o += 16;
  memcpy(salt + o, resp_random, 32);  o += 32;
  memcpy(salt + o, resp_eph_pub, 65); o += 65;
  memcpy(salt + o, hash_sigma1, 32);  o += 32;
  return mtrc_hkdf_sha256(salt, o, shared, 32,
                          INFO_SIGMA2, sizeof(INFO_SIGMA2), s2k, 16);
}

int mtrc_case_s3k(const uint8_t shared[32], const uint8_t ipk[16],
                  const uint8_t hash_sigma12[32], uint8_t s3k[16]) {
  // salt = IPK(16) || SHA256(Σ1||Σ2)(32)
  uint8_t salt[16 + 32];
  memcpy(salt, ipk, 16);
  memcpy(salt + 16, hash_sigma12, 32);
  return mtrc_hkdf_sha256(salt, sizeof(salt), shared, 32,
                          INFO_SIGMA3, sizeof(INFO_SIGMA3), s3k, 16);
}

int mtrc_case_session_keys(const uint8_t shared[32], const uint8_t ipk[16],
                           const uint8_t hash_all[32],
                           uint8_t i2r[16], uint8_t r2i[16], uint8_t att[16]) {
  // salt = IPK(16) || SHA256(Σ1||Σ2||Σ3)(32); info "SessionKeys"; 48 bytes
  uint8_t salt[16 + 32], sek[48];
  memcpy(salt, ipk, 16);
  memcpy(salt + 16, hash_all, 32);
  if (!mtrc_hkdf_sha256(salt, sizeof(salt), shared, 32,
                        INFO_SESSION, sizeof(INFO_SESSION), sek, 48)) return 0;
  memcpy(i2r, sek,      16);
  memcpy(r2i, sek + 16, 16);
  memcpy(att, sek + 32, 16);
  return 1;
}
