// mtrc_store.h — Matter fabric table (operational credentials store).
//
// Phase A4 of ROADMAP_1.4. A commissioned Matter node belongs to one or more
// fabrics; for each it holds: the trusted root public key (from the fabric's
// RCAC), the fabric/node ids, the operational Identity Protection Key (IPK),
// our operational keypair, and our NOC/ICAC chain. CASE (A5) reads this table
// to authenticate operational sessions; AddNOC/RemoveFabric (A3) write it.
//
// Storage is fixed-capacity and static (no malloc). The table is pure
// in-memory + a flat serializer so the host (matter_c) can persist the whole
// blob through the port kv (UFS on Tasmota) — keeping this module free of any
// firmware coupling and host-testable (test/build_store.sh).
//
// NOTE: op_priv is the node's operational private key; the serialized blob is
// sensitive and must live on trusted storage (UFS), never broadcast.
//
// GPLv3. Implemented from the Matter 1.4.1 spec (Core §6 Operational Creds).

#ifndef MTRC_STORE_H
#define MTRC_STORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Matter requires supporting >= 5 fabrics; Apple Home alone may use 2 (and a
// user with two homes, or adding Google/Alexa later, needs more). ~1 KB each.
#define MTRC_MAX_FABRICS  5
#define MTRC_NOC_MAX    400   // max NOC / ICAC compact-TLV bytes
#define MTRC_LABEL_MAX   32   // FabricDescriptor Label (UTF-8), spec max length

typedef struct {
  uint8_t  in_use;
  uint8_t  fabric_index;          // 1..254 (0 = invalid), assigned by the node
  uint64_t fabric_id;
  uint64_t node_id;               // our operational node id on this fabric
  uint16_t admin_vendor_id;
  uint8_t  root_pub[65];          // trusted root public key (from RCAC)
  uint8_t  ipk[16];               // operational IPK (group epoch key 0)
  uint8_t  op_priv[32];           // our operational private key
  uint8_t  op_pub[65];            // our operational public key
  uint8_t  noc[MTRC_NOC_MAX];     uint16_t noc_len;
  uint8_t  icac[MTRC_NOC_MAX];    uint16_t icac_len;
  char     label[MTRC_LABEL_MAX + 1]; // controller-set FabricLabel (UpdateFabricLabel), NUL-terminated
} mtrc_fabric;

// Clear the whole table.
void mtrc_store_reset(void);

// Claim a free slot, assign the lowest unused fabric_index, mark it in_use.
// Returns the (zeroed except index/in_use) record, or NULL if full.
mtrc_fabric *mtrc_store_alloc(void);

// Lookups (NULL if absent).
mtrc_fabric *mtrc_store_by_index(uint8_t fabric_index);
mtrc_fabric *mtrc_store_by_fabric_id(uint64_t fabric_id);

// Enumeration over in-use fabrics.
int          mtrc_store_count(void);
mtrc_fabric *mtrc_store_at(int i);             // i-th in-use fabric

// Remove a fabric by index. Returns 1 if removed, 0 if absent.
int mtrc_store_remove(uint8_t fabric_index);

// Serialize all in-use fabrics to a flat blob for kv persistence. Returns the
// byte length written, or -1 if it doesn't fit. Deserialize replaces the
// table; returns 1 on success, 0 on a malformed/oversized blob.
int mtrc_store_serialize(uint8_t *buf, size_t cap);
int mtrc_store_deserialize(const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif // MTRC_STORE_H
