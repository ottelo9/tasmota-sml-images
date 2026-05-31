// test_csr.c — build a PKCS#10 CSR for a fixed operational keypair and write
// the DER to a file; build_csr.sh then verifies it with Python `cryptography`.

#include <stdio.h>
#include "mtrc_csr.h"
#include "mtrc_crypto.h"

int main(int argc, char **argv) {
  uint8_t op_priv[32]; for (int i = 0; i < 32; i++) op_priv[i] = (uint8_t)(0x11 + i);
  uint8_t op_pub[65];
  if (!mtrc_ec_pub_from_priv(op_pub, op_priv)) { fprintf(stderr, "pub fail\n"); return 1; }
  uint8_t csr[400];
  int n = mtrc_csr_build(csr, sizeof(csr), op_priv, op_pub);
  if (n < 0) { fprintf(stderr, "csr build fail\n"); return 1; }
  const char *path = (argc > 1) ? argv[1] : "/tmp/mtrc_test.csr";
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "open %s fail\n", path); return 1; }
  fwrite(csr, 1, (size_t)n, f); fclose(f);
  printf("built %d-byte CSR -> %s\n", n, path);
  printf("OPPUB:"); for (int i = 0; i < 65; i++) printf("%02x", op_pub[i]); printf("\n");
  return 0;
}
