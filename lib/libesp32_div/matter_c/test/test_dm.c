// test_dm.c — Matter data-model registry test (pure C, no crypto).
// Exercises endpoint/cluster/attribute declaration, get/set + change
// detection, idempotency, capacity limits, and enumeration.
// Build/run host-side: see test/build_dm.sh.

#include <stdio.h>
#include "mtrc_dm.h"

static int g_ok = 1;
static void chk(const char *l, int c){ printf("  [%s] %s\n", c?"PASS":"FAIL", l); g_ok &= c; }

int main(void) {
  printf("matter_c data-model registry test\n");
  mtrc_dm_reset();

  printf("(A) declaration\n");
  chk("add endpoint 0",  mtrc_dm_add_endpoint(0, 0x0016) == 0);
  chk("add endpoint 1",  mtrc_dm_add_endpoint(1, 0x010A) == 1);
  chk("re-add ep1 idemp", mtrc_dm_add_endpoint(1, 0x010A) == 1);   // same index
  chk("endpoint count 2", mtrc_dm_endpoint_count() == 2);
  chk("add OnOff attr",  mtrc_dm_add_attr(1, 0x0006, 0x0000, MTRC_DM_T_BOOL,
                                          MTRC_DM_F_WRITABLE, 0) == 0);
  chk("add BasicInfo VID", mtrc_dm_add_attr(0, 0x0028, 0x0002, MTRC_DM_T_U16, 0, 0xFFF1) == 0);
  chk("auto-add cluster on attr", mtrc_dm_add_attr(1, 0x0402, 0x0000, MTRC_DM_T_U16, 0, 2500) == 0);

  printf("(B) get / find\n");
  uint64_t v = 0;
  chk("get OnOff -> 0",  mtrc_dm_get(1, 0x0006, 0x0000, &v) && v == 0);
  chk("get VID -> 0xFFF1", mtrc_dm_get(0, 0x0028, 0x0002, &v) && v == 0xFFF1);
  chk("get temp -> 2500", mtrc_dm_get(1, 0x0402, 0x0000, &v) && v == 2500);
  chk("get absent -> 0", mtrc_dm_get(1, 0x0006, 0x4000, &v) == 0);
  chk("find present",   mtrc_dm_find(1, 0x0006, 0x0000) != NULL);
  chk("find wrong ep",  mtrc_dm_find(0, 0x0006, 0x0000) == NULL);

  printf("(C) set + change detection\n");
  chk("set OnOff 1 -> changed", mtrc_dm_set(1, 0x0006, 0x0000, 1) == 1);
  chk("set OnOff 1 again -> unchanged", mtrc_dm_set(1, 0x0006, 0x0000, 1) == 0);
  chk("read back 1", mtrc_dm_get(1, 0x0006, 0x0000, &v) && v == 1);
  chk("set absent -> 0", mtrc_dm_set(1, 0x0006, 0x4000, 9) == 0);

  printf("(D) re-add updates metadata, not a new row\n");
  int before = mtrc_dm_attr_count();
  chk("re-add OnOff seeds value 0", mtrc_dm_add_attr(1, 0x0006, 0x0000, MTRC_DM_T_BOOL, 0, 0) == 0);
  chk("count unchanged", mtrc_dm_attr_count() == before);
  chk("value re-seeded to 0", mtrc_dm_get(1, 0x0006, 0x0000, &v) && v == 0);

  printf("(E) enumeration\n");
  int seen_onoff = 0, seen_vid = 0;
  for (int i = 0; i < mtrc_dm_attr_count(); i++) {
    const mtrc_dm_attr_t *a = mtrc_dm_attr_at(i);
    if (a->endpoint == 1 && a->cluster == 0x0006 && a->attr == 0x0000) seen_onoff = 1;
    if (a->endpoint == 0 && a->cluster == 0x0028 && a->attr == 0x0002) seen_vid = 1;
  }
  chk("enum found OnOff", seen_onoff);
  chk("enum found VID",   seen_vid);
  chk("endpoint_at(1) is ep1", mtrc_dm_endpoint_at(1) && mtrc_dm_endpoint_at(1)->endpoint == 1);
  chk("endpoint_at(99) NULL", mtrc_dm_endpoint_at(99) == NULL);

  printf("(F) capacity limit\n");
  mtrc_dm_reset();
  int added = 0;
  for (uint32_t i = 0; i < MTRC_DM_MAX_ATTRS + 10; i++)
    if (mtrc_dm_add_attr(0, 0x1000, i, MTRC_DM_T_U32, 0, i) == 0) added++;
  chk("stops at MAX_ATTRS", added == MTRC_DM_MAX_ATTRS);

  printf(g_ok ? "\n==> data-model registry PASS\n" : "\n==> data-model registry FAIL\n");
  return g_ok ? 0 : 1;
}
