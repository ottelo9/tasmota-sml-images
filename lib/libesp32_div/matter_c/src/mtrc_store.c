// mtrc_store.c — Matter fabric table. See mtrc_store.h. GPLv3.

#include "mtrc_store.h"
#include <string.h>

static mtrc_fabric g_fab[MTRC_MAX_FABRICS];

void mtrc_store_reset(void) { memset(g_fab, 0, sizeof(g_fab)); }

mtrc_fabric *mtrc_store_by_index(uint8_t fabric_index) {
  if (fabric_index == 0) return NULL;
  for (int i = 0; i < MTRC_MAX_FABRICS; i++)
    if (g_fab[i].in_use && g_fab[i].fabric_index == fabric_index) return &g_fab[i];
  return NULL;
}

mtrc_fabric *mtrc_store_by_fabric_id(uint64_t fabric_id) {
  for (int i = 0; i < MTRC_MAX_FABRICS; i++)
    if (g_fab[i].in_use && g_fab[i].fabric_id == fabric_id) return &g_fab[i];
  return NULL;
}

mtrc_fabric *mtrc_store_alloc(void) {
  // Find a free slot.
  int slot = -1;
  for (int i = 0; i < MTRC_MAX_FABRICS; i++)
    if (!g_fab[i].in_use) { slot = i; break; }
  if (slot < 0) return NULL;
  // Assign the lowest unused fabric_index (1..254).
  uint8_t idx = 0;
  for (uint16_t cand = 1; cand <= 254; cand++) {
    if (!mtrc_store_by_index((uint8_t)cand)) { idx = (uint8_t)cand; break; }
  }
  if (idx == 0) return NULL;
  memset(&g_fab[slot], 0, sizeof(g_fab[slot]));
  g_fab[slot].in_use = 1;
  g_fab[slot].fabric_index = idx;
  return &g_fab[slot];
}

int mtrc_store_count(void) {
  int n = 0;
  for (int i = 0; i < MTRC_MAX_FABRICS; i++) if (g_fab[i].in_use) n++;
  return n;
}

mtrc_fabric *mtrc_store_at(int i) {
  int n = 0;
  for (int k = 0; k < MTRC_MAX_FABRICS; k++)
    if (g_fab[k].in_use) { if (n == i) return &g_fab[k]; n++; }
  return NULL;
}

int mtrc_store_remove(uint8_t fabric_index) {
  mtrc_fabric *f = mtrc_store_by_index(fabric_index);
  if (!f) return 0;
  memset(f, 0, sizeof(*f));
  return 1;
}

// ---- serialization -----------------------------------------------------
// Blob layout (little-endian): 'M''F''B'(magic3) ver(1) count(1), then per
// fabric: index(1) fabric_id(8) node_id(8) admin_vid(2) root_pub(65) ipk(16)
// op_priv(32) op_pub(65) noc_len(2) noc[noc_len] icac_len(2) icac[icac_len].
// ver 2 appends label_len(1) label[label_len] (UTF-8, <=MTRC_LABEL_MAX) per
// fabric. Deserialize still accepts ver-1 blobs (label defaults empty) so a
// device already commissioned under the old format keeps its fabrics.
#define MTRC_STORE_VER 2

static void put_u16(uint8_t *p, uint16_t v) { p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }
static void put_u64(uint8_t *p, uint64_t v) { for (int i=0;i<8;i++) p[i]=(v>>(8*i))&0xFF; }
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1]<<8)); }
static uint64_t get_u64(const uint8_t *p) {
  uint64_t v=0; for (int i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v;
}

int mtrc_store_serialize(uint8_t *buf, size_t cap) {
  size_t o = 0;
  if (cap < 5) return -1;
  buf[o++]='M'; buf[o++]='F'; buf[o++]='B';
  buf[o++]=MTRC_STORE_VER;
  buf[o++]=(uint8_t)mtrc_store_count();
  for (int i = 0; i < MTRC_MAX_FABRICS; i++) {
    mtrc_fabric *f = &g_fab[i];
    if (!f->in_use) continue;
    size_t lbl = strlen(f->label);
    if (lbl > MTRC_LABEL_MAX) lbl = MTRC_LABEL_MAX;
    size_t need = 1+8+8+2+65+16+32+65 + 2 + f->noc_len + 2 + f->icac_len + 1 + lbl;
    if (o + need > cap) return -1;
    buf[o++]=f->fabric_index;
    put_u64(buf+o, f->fabric_id); o+=8;
    put_u64(buf+o, f->node_id);   o+=8;
    put_u16(buf+o, f->admin_vendor_id); o+=2;
    memcpy(buf+o, f->root_pub, 65); o+=65;
    memcpy(buf+o, f->ipk, 16);      o+=16;
    memcpy(buf+o, f->op_priv, 32);  o+=32;
    memcpy(buf+o, f->op_pub, 65);   o+=65;
    put_u16(buf+o, f->noc_len);  o+=2; memcpy(buf+o, f->noc,  f->noc_len);  o+=f->noc_len;
    put_u16(buf+o, f->icac_len); o+=2; memcpy(buf+o, f->icac, f->icac_len); o+=f->icac_len;
    buf[o++]=(uint8_t)lbl;       memcpy(buf+o, f->label, lbl);              o+=lbl;
  }
  return (int)o;
}

int mtrc_store_deserialize(const uint8_t *buf, size_t len) {
  if (len < 5 || buf[0]!='M' || buf[1]!='F' || buf[2]!='B') return 0;
  uint8_t ver = buf[3];
  if (ver != 1 && ver != MTRC_STORE_VER) return 0;   // ver 1 = no label (legacy)
  uint8_t count = buf[4];
  if (count > MTRC_MAX_FABRICS) return 0;
  mtrc_store_reset();
  size_t o = 5;
  for (uint8_t c = 0; c < count; c++) {
    if (o + 1+8+8+2+65+16+32+65+2 > len) return 0;
    mtrc_fabric *f = &g_fab[c];
    f->in_use = 1;
    f->fabric_index = buf[o++];
    f->fabric_id = get_u64(buf+o); o+=8;
    f->node_id   = get_u64(buf+o); o+=8;
    f->admin_vendor_id = get_u16(buf+o); o+=2;
    memcpy(f->root_pub, buf+o, 65); o+=65;
    memcpy(f->ipk, buf+o, 16);      o+=16;
    memcpy(f->op_priv, buf+o, 32);  o+=32;
    memcpy(f->op_pub, buf+o, 65);   o+=65;
    f->noc_len = get_u16(buf+o); o+=2;
    if (f->noc_len > MTRC_NOC_MAX || o + f->noc_len + 2 > len) { mtrc_store_reset(); return 0; }
    memcpy(f->noc, buf+o, f->noc_len); o+=f->noc_len;
    f->icac_len = get_u16(buf+o); o+=2;
    if (f->icac_len > MTRC_NOC_MAX || o + f->icac_len > len) { mtrc_store_reset(); return 0; }
    memcpy(f->icac, buf+o, f->icac_len); o+=f->icac_len;
    if (ver >= 2) {                                  // ver 2+ : trailing label
      if (o + 1 > len) { mtrc_store_reset(); return 0; }
      uint8_t lbl = buf[o++];
      if (lbl > MTRC_LABEL_MAX || o + lbl > len) { mtrc_store_reset(); return 0; }
      memcpy(f->label, buf+o, lbl); f->label[lbl] = '\0'; o+=lbl;
    }
  }
  return 1;
}
