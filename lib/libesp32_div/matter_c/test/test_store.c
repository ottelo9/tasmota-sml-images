// test_store.c — Matter fabric table test (pure C). Exercises alloc/index
// assignment, lookups, enumeration, remove, and serialize/deserialize
// round-trip. Build/run host-side: see test/build_store.sh.

#include <stdio.h>
#include <string.h>
#include "mtrc_store.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }

int main(void) {
  printf("matter_c fabric store test\n");
  mtrc_store_reset();

  printf("(A) alloc + fields\n");
  mtrc_fabric *f1 = mtrc_store_alloc();
  chk("alloc f1", f1 != NULL);
  chk("f1 index==1", f1 && f1->fabric_index == 1);
  f1->fabric_id = 0x0000FAB000000001ULL;
  f1->node_id   = 0x1122334455667788ULL;
  f1->admin_vendor_id = 0xFFF1;
  memset(f1->root_pub, 0xAB, 65); f1->root_pub[0] = 0x04;
  memset(f1->ipk, 0xC5, 16);
  memset(f1->op_priv, 0x11, 32);
  memset(f1->op_pub, 0x22, 65); f1->op_pub[0] = 0x04;
  memset(f1->noc, 0x15, 130); f1->noc_len = 130;
  memset(f1->icac, 0x30, 90); f1->icac_len = 90;

  mtrc_fabric *f2 = mtrc_store_alloc();
  chk("alloc f2", f2 != NULL);
  chk("f2 index==2", f2 && f2->fabric_index == 2);
  f2->fabric_id = 0x0000FAB000000002ULL;
  f2->node_id   = 0x99AABBCCDDEEFF00ULL;

  chk("count==2", mtrc_store_count() == 2);

  printf("(B) lookups\n");
  chk("by_index(1)==f1", mtrc_store_by_index(1) == f1);
  chk("by_index(2)==f2", mtrc_store_by_index(2) == f2);
  chk("by_index(9)==NULL", mtrc_store_by_index(9) == NULL);
  chk("by_fabric_id f1", mtrc_store_by_fabric_id(0x0000FAB000000001ULL) == f1);
  chk("by_fabric_id absent", mtrc_store_by_fabric_id(0xDEAD) == NULL);

  printf("(C) remove + index reuse\n");
  chk("remove(1)", mtrc_store_remove(1) == 1);
  chk("count==1", mtrc_store_count() == 1);
  chk("by_index(1) gone", mtrc_store_by_index(1) == NULL);
  mtrc_fabric *f3 = mtrc_store_alloc();
  chk("realloc reuses index 1", f3 && f3->fabric_index == 1);  // lowest unused
  chk("count==2 again", mtrc_store_count() == 2);

  printf("(D) serialize / deserialize round-trip\n");
  // Rebuild a known state: clear, add one fully-populated fabric.
  mtrc_store_reset();
  mtrc_fabric *fa = mtrc_store_alloc();
  fa->fabric_id = 0x0102030405060708ULL;
  fa->node_id   = 0x1111222233334444ULL;
  fa->admin_vendor_id = 0x1234;
  for (int i = 0; i < 65; i++) fa->root_pub[i] = (uint8_t)(i+1);
  for (int i = 0; i < 16; i++) fa->ipk[i] = (uint8_t)(0xA0+i);
  for (int i = 0; i < 32; i++) fa->op_priv[i] = (uint8_t)(0x40+i);
  for (int i = 0; i < 65; i++) fa->op_pub[i] = (uint8_t)(0x80+i);
  fa->noc_len = 200;  for (int i = 0; i < 200; i++) fa->noc[i] = (uint8_t)(i & 0xFF);
  fa->icac_len = 150; for (int i = 0; i < 150; i++) fa->icac[i] = (uint8_t)((i*3) & 0xFF);

  static uint8_t blob[2048];
  int n = mtrc_store_serialize(blob, sizeof(blob));
  chk("serialize ok", n > 0);

  mtrc_store_reset();
  chk("table cleared", mtrc_store_count() == 0);
  chk("deserialize ok", mtrc_store_deserialize(blob, (size_t)n) == 1);
  chk("count==1 after load", mtrc_store_count() == 1);

  mtrc_fabric *fb = mtrc_store_by_index(fa->fabric_index);
  chk("fabric back", fb != NULL);
  chk("fabric_id", fb && fb->fabric_id == 0x0102030405060708ULL);
  chk("node_id",   fb && fb->node_id   == 0x1111222233334444ULL);
  chk("admin_vid", fb && fb->admin_vendor_id == 0x1234);
  chk("root_pub",  fb && fb->root_pub[0]==1 && fb->root_pub[64]==65);
  chk("ipk",       fb && fb->ipk[0]==0xA0 && fb->ipk[15]==0xAF);
  chk("op_priv",   fb && fb->op_priv[0]==0x40 && fb->op_priv[31]==0x5F);
  chk("op_pub",    fb && fb->op_pub[0]==0x80 && fb->op_pub[64]==0xC0);
  chk("noc_len",   fb && fb->noc_len==200 && fb->noc[199]==(uint8_t)199);
  chk("icac_len",  fb && fb->icac_len==150 && fb->icac[149]==(uint8_t)(149*3));

  printf("(E) reject malformed blob\n");
  chk("bad magic rejected", mtrc_store_deserialize((const uint8_t*)"XXXX\0", 5) == 0);
  chk("truncated rejected", mtrc_store_deserialize(blob, 6) == 0);

  printf("(F) FabricLabel round-trip (UpdateFabricLabel)\n");
  mtrc_store_reset();
  mtrc_fabric *fl = mtrc_store_alloc();
  fl->fabric_id = 0xAABBCCDDEEFF0011ULL;
  fl->noc_len = 10; memset(fl->noc, 0x55, 10);
  strcpy(fl->label, "Alexa Home");
  static uint8_t lblob[2048];
  int ln = mtrc_store_serialize(lblob, sizeof(lblob));
  chk("serialize w/ label ok", ln > 0);
  mtrc_store_reset();
  chk("deserialize w/ label ok", mtrc_store_deserialize(lblob, (size_t)ln) == 1);
  mtrc_fabric *fk = mtrc_store_by_index(1);
  chk("label preserved", fk && strcmp(fk->label, "Alexa Home") == 0);

  printf("(G) legacy ver-1 blob (no label) still loads\n");
  // A ver-1 blob is a ver-2 blob with an empty trailing label (label_len=0),
  // the version byte set to 1, and that one trailing zero byte dropped.
  mtrc_store_reset();
  mtrc_fabric *fv = mtrc_store_alloc();
  fv->fabric_id = 0x1212121212121212ULL;
  fv->noc_len = 4; memset(fv->noc, 0x77, 4);
  fv->label[0] = '\0';                                  // empty -> 1-byte (0x00) tail in ver 2
  static uint8_t vblob[2048];
  int vn = mtrc_store_serialize(vblob, sizeof(vblob));
  chk("serialize (empty label) ok", vn > 0);
  vblob[3] = 1;                                         // pretend it was written by ver-1 firmware
  mtrc_store_reset();
  chk("ver-1 deserialize ok", mtrc_store_deserialize(vblob, (size_t)(vn - 1)) == 1);
  mtrc_fabric *fw = mtrc_store_by_index(1);
  chk("ver-1 fabric back", fw && fw->fabric_id == 0x1212121212121212ULL);
  chk("ver-1 label empty", fw && fw->label[0] == '\0');

  printf(g_ok ? "\n==> fabric store PASS\n" : "\n==> fabric store FAIL\n");
  return g_ok ? 0 : 1;
}
