// matter_c.c — lifecycle skeleton + stubs for the pure-C Matter library.
//
// This file compiles and links today so the host integration (gate,
// xdrv wiring, web page) can be built and exercised before the protocol
// layers exist. Protocol entry points return MATTER_ERR_NOT_IMPLEMENTED.
// Each PLAN.md phase replaces a stub with a real module:
//   Phase 1 -> mtrc_tlv      Phase 3 -> mtrc_spake2p/case/btp/...
//   Phase 4 -> mtrc_im       Phase 5 -> mtrc_ep_* (clusters)
//
// GPLv3. Inspired by Tasmota Berry Matter; implemented from the CSA spec.

#include "matter_c.h"
#include "mtrc_frame.h"
#include "mtrc_pase.h"
#include "mtrc_spake2p.h"
#include "mtrc_crypto.h"
#include "mtrc_sec.h"
#include "mtrc_im.h"
#include "mtrc_dm.h"
#include "mtrc_tlv.h"
#include "mtrc_store.h"
#include "mtrc_case.h"
#include "mtrc_case_msg.h"
#include "mtrc_cert.h"
#include "mtrc_csr.h"
#include "qrcodegen.h"
#ifdef MTRC_ATTEST_TEST_CREDS
#include "mtrc_attest_creds.h"   // generated dev DAC/PAI/CD (gated; never ship)
#endif
#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // calloc/free for the heap-allocated context
#ifdef ARDUINO_ARCH_ESP32
// Tasmota helper: heap_caps_malloc(MALLOC_CAP_SPIRAM) if PSRAM is available,
// plain malloc() otherwise. Routes through a C-linkage shim in xdrv_124_tinyc.ino
// because the underlying special_malloc() lives in a .ino (C++) TU.
extern void *matter_special_malloc(size_t n);
#endif

// ---- module state ------------------------------------------------------
// One established operational CASE session. Apple Home always opens >=2
// operational sessions (the iPhone AND each home hub); holding only one and
// letting the second clobber the first makes the first controller's traffic
// fail to decrypt -> both accessories show "no response". We keep a table of
// established sessions keyed by our (responder) session id.
//
// Apple Home runs the iPhone AND every HomePod / Apple TV as a SEPARATE admin
// (controller) node, each holding its own operational CASE session + wildcard
// subscription — and Apple may commission the device into TWO fabrics. So a
// single home easily needs 5-6 concurrent sessions. Too few slots -> the hub
// responsible for the device keeps getting evicted (slot thrash) -> Home shows
// "Keine Antwort" even though commissioning succeeded. ~112 B per slot.
#define MTRC_MAX_CASE_SESS 16
typedef struct {
  bool     in_use;
  uint16_t my_sid;        // our responder/operational session id
  uint16_t peer_sid;      // controller (initiator) session id
  uint8_t  fabric_index;
  uint64_t peer_node_id;  // controller operational node id (RX/TX nonce source)
  uint8_t  i2r[16], r2i[16], att[16];
  uint32_t tx_counter;    // per-session secured TX message counter
  // Operational subscription (for proactive/live reports from matter_loop).
  bool     sub_active;    // this controller has a live subscription
  uint32_t sub_id;        // SubscriptionId we assigned
  uint16_t sub_max_s;     // max report interval (seconds)
  uint16_t sub_exch;      // subscription exchange id (reports reuse it)
  uint32_t sub_last_ms;   // last report time
  uint32_t sub_last_gen;  // app-data generation at last report (change detection)
  uint8_t  cli_ip6[16];   // controller's IPv6 (so a periodic report reaches it)
  uint16_t cli_port;
  uint32_t rx_last_ctr;   // last processed inbound counter (secured MRP dedup)
  bool     have_rx_ctr;
} mtrc_case_sess;

// One inbound datagram queued for processing. The old single-buffer design
// dropped every packet that arrived while one was pending; with several
// controllers (Apple opens many sessions) that caused constant MRP retransmit
// storms (seconds of control lag). A ring lets bursts queue instead of drop.
// Each entry remembers its SOURCE so the reply goes back to the right
// controller (not a global "last peer").
#define MTRC_RX_QUEUE 8
typedef struct {
  uint8_t  ip6[16];
  uint16_t port;
  uint16_t len;
  uint8_t  buf[1280];
} mtrc_rx_pkt;

// The whole context is heap-allocated on matter_init() (and freed on factory
// reset) so a firmware with Matter compiled-in but unused costs ~0 RAM — it is
// only allocated once a host/script actually starts Matter.
typedef struct {
  bool             inited;
  bool             started;
  bool             commissionable;    // PASE accepted only while a Bind window is open
  matter_port_t    port;
  matter_config_t  cfg;
  char             qr[32];     // "MT:..." (built once config is known)
  char             manual[16]; // "1234-567-8901"

  // Deferred inbound datagrams (matter_udp_rx runs in a network task; the
  // crypto-heavy processing runs in matter_loop / main loop). SPSC ring:
  // producer = network task, consumer = main loop. Single-core C6 -> atomic
  // index updates.
  mtrc_rx_pkt      rx_q[MTRC_RX_QUEUE];
  volatile uint8_t rx_head, rx_tail;
  uint8_t          reply_ip6[16];   // source of the packet currently dispatched
  uint16_t         reply_port;      // -> replies target the right controller

  // PASE responder session state
  uint8_t          pase_phase;        // 0 idle, 1 sent-resp, 2 sent-pake2, 3 done
  uint16_t         peer_session_id;
  uint64_t         peer_node_id;       // initiator's ephemeral source node id (unsecured)
  bool             peer_has_node;      // whether the initiator sent a source node id
  uint32_t         peer_last_ctr;      // last processed peer counter (unsecured MRP dedup)
  bool             have_peer_ctr;
  uint8_t          last_tx_buf[1536];  // last unsecured frame sent (re-sent on retransmit; Sigma2 w/ ICAC)
  size_t           last_tx_len;
  uint16_t         my_session_id;
  uint16_t         exchange_id;
  uint32_t         tx_counter;        // our unsecured-session message counter
  uint8_t          init_random[32];
  uint8_t          resp_random[32];
  uint8_t          salt[16];
  uint32_t         iterations;
  uint8_t          context[32];       // SHA256(prefix || req || resp)
  uint8_t          cA_expected[32];   // prover confirmation we expect in Pake3
  uint8_t          i2r[16], r2i[16], att[16];   // PASE session keys (on success)
  bool             pase_secure;       // true once cA verified
  uint32_t         sec_tx_counter;    // our secured-session message counter
  uint32_t         pase_rx_last_ctr;  // last processed inbound counter on the PASE secure session (MRP dedup)
  bool             pase_have_rx_ctr;
  // Last encrypted reply sent on ANY secured session, cached so an MRP
  // retransmit (same inbound counter) is answered by re-sending these exact
  // bytes instead of re-running the handler. Without this a retransmitted
  // AddNOC over the PASE secure session was re-invoked -> a SECOND fabric was
  // persisted for the same node (idx=1 AND idx=2), burning 2 of 5 slots per
  // commission (Hans/Google Nest, #85 comment-17101003). Single global cache
  // mirrors the unsecured path (last_tx_buf): safe because during commissioning
  // the device sends no unsolicited secured traffic between a request and its
  // retransmit.
  uint8_t          sec_last_tx_buf[1280];
  size_t           sec_last_tx_len;
  bool             onoff;             // OnOff attribute (endpoint relay state)
  uint64_t         breadcrumb;        // GeneralCommissioning Breadcrumb
  uint16_t         next_ep;           // next endpoint id handed out by add_endpoint

  // Bridge / per-endpoint naming. A controller (Apple Home) can only display a
  // manufacturer-supplied name per endpoint when the node is a *bridge*: an
  // Aggregator (0x000E) endpoint groups child endpoints that each advertise the
  // Bridged Node device type (0x0013) and a Bridged Device Basic Information
  // cluster (0x0039) whose NodeLabel carries the name. matterName() opts an
  // endpoint into this (lazily creating the aggregator on first use). Malloc-free
  // parallel table — names live here, not in the u64-only data-model registry.
  uint16_t         aggregator_ep;     // 0 = no bridge; else the Aggregator endpoint id
  uint8_t          label_count;
  struct { uint16_t ep; char name[33]; } labels[MTRC_DM_MAX_ENDPOINTS];

  // CASE responder state (operational session establishment)
  uint8_t          case_phase;        // 0 idle, 1 sent-sigma2, 2 secure
  uint8_t          case_fabric_index;
  uint16_t         case_peer_sid;     // initiator (controller) session id
  uint16_t         case_my_sid;       // our responder/operational session id
  uint64_t         case_peer_node_id; // controller's operational node id (Sigma3 NOC) — RX nonce
  uint8_t          case_shared[32];   // ECDH shared secret
  uint8_t          case_re_priv[32], case_re_pub[65];  // responder ephemeral
  uint8_t          case_init_eph[65]; // initiator ephemeral pub (from Sigma1)
  uint8_t          case_resp_random[32];
  uint8_t          case_tt[2560];     // transcript: Sigma1[||Sigma2[||Sigma3]] (NOC+ICAC fit)
  size_t           case_tt_len;       // total bytes in case_tt
  size_t           case_len1;         // Sigma1 length (prefix for h1)
  uint8_t          case_i2r[16], case_r2i[16], case_att[16];
  bool             case_secure;
  uint32_t         case_sec_tx_counter;

  // Established operational sessions. The case_* fields above are the "working
  // set" for the session currently being serviced; on RX we load the slot whose
  // my_sid matches the inbound session id into the working set, dispatch, then
  // save it back (so the per-session TX counter advances). Handshake scratch
  // (case_phase/case_shared/case_re_*/case_tt) stays single — Apple runs the two
  // CASE handshakes sequentially.
  mtrc_case_sess   case_sess[MTRC_MAX_CASE_SESS];
  // In-flight CASE handshake scratch — kept separate from the working set so an
  // inbound message on an already-established session (controller 1) can't
  // corrupt a handshake running concurrently (controller 2). Apple interleaves
  // the two, so this decoupling is required.
  uint16_t         case_hs_my_sid;
  uint16_t         case_hs_peer_sid;
  uint8_t          case_hs_fabric_index;
  uint64_t         case_hs_peer_node_id;

  // Commissioning: operational keypair generated by CSRRequest, certified by
  // AddNOC (A3). Pending until AddNOC installs the fabric.
  uint8_t          pending_op_priv[32], pending_op_pub[65];
  bool             have_pending_op;
  uint8_t          pending_root_pub[65];      // from AddTrustedRootCertificate
  bool             have_pending_root;

  // Fail-Safe context (Core Spec §11.10). ArmFailSafe starts a timer; AddNOC
  // installs a fabric that is TENTATIVE until CommissioningComplete commits it.
  // If the timer expires (or a fresh commissioning starts) first, the tentative
  // fabric is rolled back. Without this every failed/abandoned commissioning
  // attempt that reached AddNOC leaked a *persisted* fabric — after
  // MTRC_MAX_FABRICS such attempts the table is full -> AddNOC=TableFull -> no
  // operational mDNS -> the controller never starts CASE (Hans/Google Nest,
  // discussion #85: "kann nicht hinzugefügt").
  bool             fs_armed;            // fail-safe currently armed
  uint32_t         fs_expiry_ms;        // millis() deadline while armed
  uint8_t          fs_added_fabric;     // fabric_index added by AddNOC this context (0 = none/committed)

  // AdministratorCommissioning (cluster 0x003C) "enhanced" commissioning window
  // opened by an already-commissioned admin (e.g. Google Nest hub's multi-admin
  // handoff): OpenCommissioningWindow supplies an EXTERNAL SPAKE2+ verifier
  // (W0 32B || L 65B) + salt + iterations + discriminator, so a SECOND admin can
  // PASE-pair WITHOUT the on-device passcode. While ocw_active the PASE responder
  // uses these supplied parameters instead of deriving w0/L from g.cfg.passcode.
  // All zero (cleared context) -> normal Apple/Bind commissioning is byte-identical.
  bool             ocw_active;          // external-verifier PASE window open
  uint8_t          ocw_w0[32];          // supplied SPAKE2+ w0 (BE scalar)
  uint8_t          ocw_L[65];           // supplied SPAKE2+ L = w1*G (uncompressed point)
  uint8_t          ocw_salt[32];        // PBKDF salt the admin used to derive w0
  uint8_t          ocw_salt_len;        // 16..32
  uint32_t         ocw_iterations;      // PBKDF iteration count to echo
  uint16_t         ocw_disc;            // 12-bit discriminator advertised over mDNS
  uint32_t         ocw_expiry_ms;       // millis() deadline; 0 = no OCW window armed

  // single attribute subscription (the report engine)
  bool             sub_active;
  uint32_t         sub_id;
  uint16_t         sub_ep; uint32_t sub_cl, sub_attr;
  uint16_t         sub_max_s;         // max report interval (seconds)
  uint16_t         sub_exch;
  uint32_t         sub_last_ms;       // last report time
  uint64_t         sub_last_val;      // last reported value (change detection)

  // Chunked ReportData engine: a wildcard read/subscribe response that exceeds
  // one UDP datagram is split into multiple ReportData messages with
  // MoreChunkedMessages set; each non-final chunk is flow-controlled by the
  // controller's StatusResponse (Core Spec §8.7 / §4.4.4).
  bool             rpt_active;        // a chunked report is in progress
  bool             rpt_is_sub;        // subscribe (send SubscribeResponse at end) vs read
  uint8_t          rpt_phase;         // 0=sending chunks, 1=(sub) awaiting StatusResponse -> SubscribeResponse
  uint32_t         rpt_sub_id;        // subscription id (subscribe only)
  uint16_t         rpt_sub_max_s;     // subscription max interval (subscribe only)
  uint16_t         rpt_exch;          // exchange id of the read/subscribe
  int              rpt_cursor;        // next path index to emit
  int              rpt_npaths;        // total paths
  struct { uint16_t ep; uint32_t cl; uint32_t attr; } rpt_paths[1024];  // wildcard-read path buffer; sized for ~32 endpoints (see MTRC_DM_MAX_ENDPOINTS)
  // Concrete read paths that matched nothing -> a StatusIB must be returned
  // (Matter spec; an empty report makes a strict controller, e.g. Alexa, retry
  // the read and restart its whole interrogation -> RN002 "getting device ready").
  struct { uint16_t ep; uint32_t cl; uint32_t attr; uint8_t st; } rpt_status[16];
  int              rpt_nstatus;

  // App-data generation: bumped whenever an app-endpoint attribute is written
  // (sensor/light values). Per-session subscriptions compare it to detect a
  // change and push a live ReportData (matter_loop), not just at max interval.
  uint32_t app_gen;

  // Matter Events (Generic Switch button presses). matterEvent() enqueues here
  // (may run in the VM task); matter_loop drains and sends EventReports to
  // subscribers in the main loop (no race on the working set / reply target).
  uint64_t event_number;     // monotonic EventNumber
#define MTRC_EV_QUEUE 8
  struct { uint16_t ep; uint32_t cl; uint32_t ev; int32_t a; int32_t b; } ev_q[MTRC_EV_QUEUE];
  volatile uint8_t ev_head, ev_tail;
} matter_ctx_t;

static matter_ctx_t *g_ptr = NULL;   // NULL until matter_init() — zero RAM when unused
#define g (*g_ptr)                    // every g.field below dereferences the heap context

static void mlog(matter_log_level_t lvl, const char *msg) {
  if (g.port.log) g.port.log(g.port.ctx, lvl, msg);
}

// Fail-Safe disarm/rollback (defined after the fabric-store helpers it uses);
// forward-declared so the PASE handler can abandon a prior tentative fabric.
static void matter_failsafe_disarm(bool committed);

// ---- onboarding payload (Core Spec §5.1) -------------------------------
// Verhoeff check digit over the decimal string s (manual pairing code).
static uint8_t mtrc_verhoeff(const char *s) {
  static const uint8_t d[10][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,2,3,4,0,6,7,8,9,5},{2,3,4,0,1,7,8,9,5,6},
    {3,4,0,1,2,8,9,5,6,7},{4,0,1,2,3,9,5,6,7,8},{5,9,8,7,6,0,4,3,2,1},
    {6,5,9,8,7,1,0,4,3,2},{7,6,5,9,8,2,1,0,4,3},{8,7,6,5,9,3,2,1,0,4},
    {9,8,7,6,5,4,3,2,1,0}};
  static const uint8_t p[8][10] = {
    {0,1,2,3,4,5,6,7,8,9},{1,5,7,6,2,8,3,0,9,4},{5,8,0,3,7,9,6,1,4,2},
    {8,9,1,6,0,4,3,5,2,7},{9,4,5,3,1,2,6,8,7,0},{4,2,8,6,5,7,3,9,0,1},
    {2,7,9,3,8,0,6,4,1,5},{7,0,4,6,9,1,3,2,5,8}};
  static const uint8_t inv[10] = {0,4,3,2,1,5,6,7,8,9};
  int c = 0, len = (int)strlen(s);
  for (int i = 0; i < len; i++) {
    int dig = s[len - 1 - i] - '0';
    c = d[c][p[(i + 1) % 8][dig]];
  }
  return inv[c];
}

// QR module matrix for the onboarding payload, so the host can draw the code
// itself (no external/CDN QR library). Encoded once from g.qr.
static uint8_t g_qrbuf[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
static int     g_qr_ok = 0;

// Build the manual pairing code (11 digits) and the "MT:" QR string into g.
static void mtrc_build_onboarding(void) {
  uint16_t disc = g.cfg.discriminator & 0x0FFF;
  uint32_t pass = g.cfg.passcode & 0x07FFFFFF;

  // Manual code (short, no VID/PID): d1 | chunk2(5) | chunk3(4) | Verhoeff.
  unsigned d1 = (0u << 2) | (unsigned)(disc >> 10);
  unsigned c2 = ((unsigned)(disc & 0x300) << 6) | (unsigned)(pass & 0x3FFF);
  unsigned c3 = (unsigned)(pass >> 14);
  char body[12];
  snprintf(body, sizeof(body), "%01u%05u%04u", d1, c2, c3);
  snprintf(g.manual, sizeof(g.manual), "%s%u", body, (unsigned)mtrc_verhoeff(body));

  // QR: bit-pack version(3)=0, VID(16), PID(16), flow(2)=0, discovery(8),
  // discriminator(12), passcode(27), pad(4) = 88 bits, then Base38 + "MT:".
  uint8_t buf[11]; memset(buf, 0, sizeof(buf));
  int pos = 0;
  #define MTRC_PUTBITS(val, ln) do { \
      for (int _i = 0; _i < (ln); _i++) { \
        if ((uint32_t)(val) & (1u << _i)) buf[pos >> 3] |= (1 << (pos & 7)); \
        pos++; } } while (0)
  MTRC_PUTBITS(0, 3);
  MTRC_PUTBITS(g.cfg.vendor_id, 16);
  MTRC_PUTBITS(g.cfg.product_id, 16);
  MTRC_PUTBITS(0, 2);                 // commissioning flow = standard
  MTRC_PUTBITS(0x04, 8);             // discovery capabilities = on-network/IP
  MTRC_PUTBITS(disc, 12);
  MTRC_PUTBITS(pass, 27);
  MTRC_PUTBITS(0, 4);               // padding
  #undef MTRC_PUTBITS
  static const char B38[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.";
  char *o = g.qr; *o++ = 'M'; *o++ = 'T'; *o++ = ':';
  for (int i = 0; i < 11; i += 3) {
    int cl = (11 - i >= 3) ? 3 : (11 - i);
    uint32_t v = 0;
    for (int j = 0; j < cl; j++) v |= (uint32_t)buf[i + j] << (8 * j);
    int nch = (cl == 3) ? 5 : (cl == 2) ? 4 : 2;
    for (int k = 0; k < nch; k++) { *o++ = B38[v % 38]; v /= 38; }
  }
  *o = '\0';

  // Pre-render the QR module matrix so the host can draw it without a CDN.
  static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  g_qr_ok = qrcodegen_encodeText(g.qr, tmp, g_qrbuf, qrcodegen_Ecc_MEDIUM,
                                 qrcodegen_VERSION_MIN, 6, qrcodegen_Mask_AUTO, true);
}

int matter_qr_size(void) { return g_qr_ok ? qrcodegen_getSize(g_qrbuf) : 0; }
bool matter_qr_dark(int x, int y) {
  return g_qr_ok ? qrcodegen_getModule(g_qrbuf, x, y) : false;
}

// Cluster ids used by the data model.
#define MTRC_CL_ONOFF       0x0006
#define MTRC_CL_LEVEL       0x0008
#define MTRC_CL_DESCRIPTOR  0x001D
#define MTRC_CL_BASIC_INFO  0x0028
#define MTRC_CL_POWERSOURCE 0x002F
#define MTRC_CL_TEMP_MEAS   0x0402
#define MTRC_CL_HUM_MEAS    0x0405

// Attach a device-type's default clusters/attributes to an endpoint. Keeps the
// per-device-type mandatory set in one place so matter_add_endpoint and the
// matter_init seed agree. (Subset; extended as Phase D clusters land.)
// The Identify cluster (0x0003) is MANDATORY on nearly every application device
// type (plug, lights, sensors, fan, window covering, generic switch, ...). Apple
// drops an endpoint that lacks it (that's why a bare On/Off Plug-in Unit never
// appeared in Home). It is therefore added to EVERY app endpoint in
// matter_add_endpoint, not per-device-type here. IdentifyTime (0x0000 u16) +
// IdentifyType (0x0001 enum8, 0 = None).
static void dm_add_identify(uint16_t ep) {
  mtrc_dm_add_attr(ep, 0x0003, 0x0000, MTRC_DM_T_U16, MTRC_DM_F_WRITABLE, 0);
  mtrc_dm_add_attr(ep, 0x0003, 0x0001, MTRC_DM_T_U8,  0, 0);
}

// Groups (0x0004) is MANDATORY on the On/Off Plug-in Unit and every Lighting
// device type (Matter Device Library). Apple/Google tolerate its absence, but
// Alexa enforces the device type's mandatory-cluster list and rejects the whole
// device ("Alexa is getting your device ready" -> GS014 / RN002) when it is
// missing. NameSupport (0x0000, bitmap8) is its sole mandatory attribute;
// bit7 = GroupNames feature (0 = not supported, matching FeatureMap 0).
static void dm_add_groups(uint16_t ep) {
  mtrc_dm_add_attr(ep, 0x0004, 0x0000, MTRC_DM_T_U8, 0, 0);   // Groups.NameSupport = 0
}

// OnOff cluster's Lighting (LT) feature attrs — MANDATORY under LT per Matter 1.4
// OnOff cluster spec table. Added on every Light device type but NOT on Plug
// (plug spec does not include LT). Without these, an Alexa interrogation of a
// mixed actuator+sensor node treats the OnOff cluster as non-spec → GS014
// (single-light pairs but multi-class node rejects). Per-EP FeatureMap dispatch
// in emit_one_path matches: only Light EPs claim FeatureMap bit 0 (LT).
static void dm_add_onoff_lt(uint16_t ep) {
  mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x4000, MTRC_DM_T_BOOL,  0, 1); // GlobalSceneControl (default TRUE)
  mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x4001, MTRC_DM_T_U16,   0, 0); // OnTime
  mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x4002, MTRC_DM_T_U16,   0, 0); // OffWaitTime
  mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x4003, MTRC_DM_T_ENUM8, 0, 0); // StartUpOnOff (0 = Off)
}

static void dm_attach_device_type(uint16_t ep, uint32_t dt) {
  switch (dt) {
    case MATTER_DEVTYPE_ON_OFF_PLUGIN:
      // Plug: OnOff base only — LT does NOT apply to OnOff Plug-in Unit per spec.
      mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x0000, MTRC_DM_T_BOOL,
                       MTRC_DM_F_WRITABLE | MTRC_DM_F_LIVE, 0);
      dm_add_groups(ep);
      break;
    case MATTER_DEVTYPE_ON_OFF_LIGHT:
      mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x0000, MTRC_DM_T_BOOL,
                       MTRC_DM_F_WRITABLE | MTRC_DM_F_LIVE, 0);
      dm_add_onoff_lt(ep);
      dm_add_groups(ep);
      break;
    case MATTER_DEVTYPE_DIMMABLE_LIGHT:
    case 0x010C:   // Color Temperature Light
    case 0x010D:   // Extended Color Light (script also declares ColorControl)
      mtrc_dm_add_attr(ep, MTRC_CL_ONOFF, 0x0000, MTRC_DM_T_BOOL,
                       MTRC_DM_F_WRITABLE | MTRC_DM_F_LIVE, 0);
      dm_add_onoff_lt(ep);
      mtrc_dm_add_attr(ep, MTRC_CL_LEVEL, 0x0000, MTRC_DM_T_U8,
                       MTRC_DM_F_WRITABLE, 0);   // CurrentLevel
      // LevelControl mandatory/Berry-parity attrs — Alexa rejects a Dimmable/
      // Color Light whose Level cluster serves only CurrentLevel (GS014).
      mtrc_dm_add_attr(ep, MTRC_CL_LEVEL, 0x0002, MTRC_DM_T_U8, 0, 1);    // MinLevel
      mtrc_dm_add_attr(ep, MTRC_CL_LEVEL, 0x0003, MTRC_DM_T_U8, 0, 254);  // MaxLevel
      mtrc_dm_add_attr(ep, MTRC_CL_LEVEL, 0x000F, MTRC_DM_T_U8, 0, 0);    // Options
      mtrc_dm_add_attr(ep, MTRC_CL_LEVEL, 0x0011, MTRC_DM_T_U8, 0, 254);  // OnLevel
      dm_add_groups(ep);
      if (dt == 0x010C || dt == 0x010D) {
        // ColorControl attrs that are mandatory regardless of feature (HS/XY/CT).
        // The script declares CurrentHue/CurrentSaturation; these complete the
        // mandatory set so Alexa accepts the Extended/Color-Temp Light.
        mtrc_dm_add_attr(ep, 0x0300, 0x0008, MTRC_DM_T_U8,  0, 0);  // ColorMode (0=CurrentHue&Sat)
        mtrc_dm_add_attr(ep, 0x0300, 0x000F, MTRC_DM_T_U8,  0, 0);  // Options
        mtrc_dm_add_attr(ep, 0x0300, 0x4001, MTRC_DM_T_U8,  0, 0);  // EnhancedColorMode (0)
        mtrc_dm_add_attr(ep, 0x0300, 0x400A, MTRC_DM_T_U16, 0, 1);  // ColorCapabilities (bit0=HS)
      }
      break;
    case MATTER_DEVTYPE_TEMP_SENSOR:
      mtrc_dm_add_attr(ep, MTRC_CL_TEMP_MEAS, 0x0000, MTRC_DM_T_U16, 0, 0);
      break;
    case MATTER_DEVTYPE_HUMIDITY_SENSOR:
      mtrc_dm_add_attr(ep, MTRC_CL_HUM_MEAS, 0x0000, MTRC_DM_T_U16, 0, 0);
      break;
    default: break;
  }
}

// Per-endpoint bridge label lookup (NULL if the endpoint was never named). An
// endpoint that has a label is a "Bridged Node": its Descriptor DeviceTypeList
// gains 0x0013 and it carries a Bridged Device Basic Information cluster.
static const char *dm_label_for(uint16_t ep) {
  for (int i = 0; i < g.label_count; i++)
    if (g.labels[i].ep == ep) return g.labels[i].name;
  return NULL;
}
static int ep_is_bridged(uint16_t ep) { return dm_label_for(ep) != NULL; }

// NON_BRIDGE_VENDOR (Berry-matter parity): Amazon Alexa (vendor 0x1217) and
// Amazon (0x1381) do not accept the Bridged Node device type (0x0013) in an
// endpoint's DeviceTypeList — a bridged ACTUATOR makes Alexa loop during "getting
// device ready" and fail (GS014/RN002), while bridged sensors squeak by. Apple
// Home / Google Home require 0x0013 to read the per-endpoint NodeLabel (0x0039),
// so we keep it for them. The fix is PER-FABRIC: for an Alexa/Amazon fabric we
// present the SAME endpoints WITHOUT 0x0013 (a plain composed node, which Alexa
// accepts), keyed on the reading CASE session's fabric admin vendor id (captured
// at AddNOC field 4). The Aggregator (0x000E) itself is tolerated by Alexa, so it
// stays. Returns 1 when the current session's fabric is a non-bridge vendor.
static int fabric_is_non_bridge(void) {
  mtrc_fabric *f = mtrc_store_by_index(g.case_fabric_index);
  if (!f) return 0;
  return f->admin_vendor_id == 0x1217 || f->admin_vendor_id == 0x1381;
}

#ifdef MTRC_CASE_TEST_FABRIC
// Pre-provision a fixed TEST fabric so the CASE responder can establish an
// operational session before the real commissioning flow (A2/A3) exists. The
// matching credentials live in the Python prover. Gated OFF by default — never
// ship test keys; enable via -DMTRC_CASE_TEST_FABRIC for the device test only.
static void case_seed_test_fabric(void) {
  if (mtrc_store_count() > 0) return;
  mtrc_fabric *f = mtrc_store_alloc();
  if (!f) return;
  f->fabric_id = 0x0000FAB000000001ULL;
  f->node_id   = 0x1122334455667788ULL;
  f->admin_vendor_id = 0xFFF1;
  memset(f->ipk, 0xC5, 16);
  uint8_t root_priv[32]; memset(root_priv, 0x07, 32);
  mtrc_ec_pub_from_priv(f->root_pub, root_priv);
  memset(f->op_priv, 0x11, 32);
  mtrc_ec_pub_from_priv(f->op_pub, f->op_priv);
  for (int i = 0; i < 48; i++) f->noc[i] = (uint8_t)(0x40 + i);
  f->noc_len = 48;
  mlog(MATTER_LOG_INFO, "CASE: seeded TEST fabric (do not ship)");
}
#endif

// Seed endpoint 0 (root node): Descriptor + Basic Information VID/PID.
static void dm_seed_root(void) {
  mtrc_dm_add_endpoint(0, 0x0016);   // Root Node device type
  mtrc_dm_add_cluster(0, MTRC_CL_DESCRIPTOR);   // mandatory on every endpoint
  mtrc_dm_add_attr(0, MTRC_CL_BASIC_INFO, 0x0002, MTRC_DM_T_U16, 0, g.cfg.vendor_id);
  mtrc_dm_add_attr(0, MTRC_CL_BASIC_INFO, 0x0004, MTRC_DM_T_U16, 0, g.cfg.product_id);
  // Mandatory root-node clusters (read attributes served by emit_root_attr).
  // Registering them puts them in the Descriptor ServerList and the wildcard
  // enumeration; their values are synthetic / fabric-scoped (not registry attrs).
  mtrc_dm_add_cluster(0, 0x0030);   // General Commissioning
  mtrc_dm_add_cluster(0, 0x0031);   // Network Commissioning
  mtrc_dm_add_cluster(0, 0x0033);   // General Diagnostics
  mtrc_dm_add_cluster(0, 0x003C);   // Administrator Commissioning
  mtrc_dm_add_cluster(0, 0x003E);   // Operational Credentials
  mtrc_dm_add_cluster(0, 0x003F);   // Group Key Management
  mtrc_dm_add_cluster(0, 0x001F);   // Access Control
}

// ---- fabric persistence (kv) -------------------------------------------
// The whole fabric table is serialized to one kv blob ("fab" -> UFS file on
// Tasmota). Saved on AddNOC, restored on boot, so a commissioned node survives
// reboots and stays reachable to its controllers (Apple/chip-tool).
#define MTRC_KV_FABRICS "fab"
#define MTRC_KV_BLOB_MAX  5632      // max serialized fabric table (5 fabrics * ~1 KB worst case w/ ICAC); transient malloc
static void publish_operational_mdns(const mtrc_fabric *f);   // fwd (defined below)
static void unpublish_operational_mdns(const mtrc_fabric *f); // fwd (defined below)

static void mtrc_persist_fabrics(void) {
  if (!g.port.kv_set) return;
  uint8_t *blob = (uint8_t *)malloc(MTRC_KV_BLOB_MAX);   // transient — not static BSS
  if (!blob) return;
  int n = mtrc_store_serialize(blob, MTRC_KV_BLOB_MAX);
  if (n <= 0) { mlog(MATTER_LOG_ERROR, "persist: serialize failed"); free(blob); return; }
  matter_err_t e = g.port.kv_set(g.port.ctx, MTRC_KV_FABRICS, blob, (size_t)n);
  free(blob);
  char m[56];
  snprintf(m, sizeof(m), "persist: %d B / %d fabric(s) rc=%d", n, mtrc_store_count(), (int)e);
  mlog(MATTER_LOG_INFO, m);
}

static void mtrc_load_fabrics(void) {
  if (!g.port.kv_get) return;
  uint8_t *blob = (uint8_t *)malloc(MTRC_KV_BLOB_MAX);
  if (!blob) return;
  size_t len = MTRC_KV_BLOB_MAX;
  if (g.port.kv_get(g.port.ctx, MTRC_KV_FABRICS, blob, &len) == MATTER_OK && len > 0 &&
      mtrc_store_deserialize(blob, len)) {
    char m[48];
    snprintf(m, sizeof(m), "loaded %d fabric(s) from kv", mtrc_store_count());
    mlog(MATTER_LOG_INFO, m);
  }
  free(blob);
}

// ---- lifecycle ---------------------------------------------------------
matter_err_t matter_init(const matter_port_t *port, const matter_config_t *cfg) {
  if (!port || !cfg) return MATTER_ERR_INVALID_ARG;
  // Minimum viable port: persistence + time + entropy must be present.
  if (!port->kv_get || !port->kv_set || !port->millis || !port->random_bytes)
    return MATTER_ERR_INVALID_ARG;

  // Lazily allocate the (large) context the first time Matter is started.
  // Prefer PSRAM on boards that have it — the context is ~22 KB and would
  // otherwise consume a sizeable chunk of internal DRAM on a 4 MB-PSRAM
  // classic ESP32 (where DRAM is the limiting resource, not PSRAM).
  // matter_special_malloc is a thin C-linkage shim in xdrv_124_tinyc.ino
  // that forwards to Tasmota's special_malloc(). On non-PSRAM boards it
  // returns plain malloc; on the host test harness (no ARDUINO_ARCH_ESP32)
  // we fall through to libc calloc.
  if (!g_ptr) {
#ifdef ARDUINO_ARCH_ESP32
    g_ptr = (matter_ctx_t *)matter_special_malloc(sizeof(matter_ctx_t));
#else
    g_ptr = (matter_ctx_t *)calloc(1, sizeof(matter_ctx_t));
#endif
    if (!g_ptr) return MATTER_ERR_NO_MEM;
  }
  memset(&g, 0, sizeof(g));   // zero whether we came from PSRAM or DRAM
  g.port = *port;
  g.cfg  = *cfg;
  g.inited = true;
  g.next_ep = 1;            // endpoint 0 is the root node

  // Seed the data-model registry: root node + a default OnOff endpoint so the
  // current relay device works out of the box. A TinyC script (Phase C) can
  // matter_factory_reset() and rebuild a different model.
  mtrc_dm_reset();
  dm_seed_root();
  matter_add_endpoint(MATTER_DEVTYPE_ON_OFF_PLUGIN);   // -> endpoint 1

  // Fabric table (operational credentials). Restore any commissioned fabrics
  // from persistent storage so the node stays in its fabrics across reboots.
  mtrc_store_reset();
  mtrc_load_fabrics();
#ifdef MTRC_CASE_TEST_FABRIC
  if (mtrc_store_count() == 0) case_seed_test_fabric();   // dev fallback only
#endif

  // Onboarding payload: QR ("MT:...") + 11-digit manual pairing code.
  mtrc_build_onboarding();

  mlog(MATTER_LOG_INFO, "matter_c init (data model seeded)");
  return MATTER_OK;
}

#define MTRC_COMMISSION_PORT 5540   // Matter operational/commissioning UDP port

matter_err_t matter_start(void) {
  if (!g.inited)   return MATTER_ERR_NOT_INIT;
  if (g.started)   return MATTER_OK;

  // Operational only: re-publish the operational service (_matter._udp) for each
  // restored fabric so commissioned controllers re-discover us after a reboot.
  // The COMMISSIONABLE advert (_matterc._udp) + QR are published on demand by
  // matter_open_commissioning_window() — the host's Bind action. A started but
  // un-bound node is therefore NOT openly pairable until the user opens a window.
  for (int i = 0; i < mtrc_store_count(); i++) {
    mtrc_fabric *f = mtrc_store_at(i);
    if (f) publish_operational_mdns(f);
  }
#ifdef MTRC_DIAG
  // Debug: dump the data-model endpoints (device type + cluster count) so the
  // script's declared model can be inspected. Build with -DMTRC_DIAG to enable.
  for (int i = 0; i < mtrc_dm_endpoint_count(); i++) {
    const mtrc_dm_endpoint_t *e = mtrc_dm_endpoint_at(i);
    if (!e) continue;
    int nc = 0;
    for (int j = 0; j < mtrc_dm_cluster_count(); j++) {
      const mtrc_dm_cluster_t *c = mtrc_dm_cluster_at(j);
      if (c && c->endpoint == e->endpoint) nc++;
    }
    char m[64];
    snprintf(m, sizeof(m), "DIAG endpoint %u devtype 0x%04X clusters=%d",
             (unsigned)e->endpoint, (unsigned)e->device_type, nc);
    mlog(MATTER_LOG_INFO, m);
  }
#endif
  g.started = true;
  return MATTER_OK;
}

// Open the commissioning window (the Bind action): advertise the commissionable
// node over mDNS (_matterc._udp, TXT D/CM/VP per Core Spec §4.3.1) and accept
// PASE. The host times the window and closes it (remove advert + set !commissionable)
// on the Bind-window timeout or on Unbind.
// Publish the commissionable-node advert (_matterc._udp, TXT D/CM/VP per Core
// Spec §4.3.1). cm = 1 (standard, on-device passcode) or 2 (enhanced window
// opened by AdministratorCommissioning, external verifier). disc is the 12-bit
// discriminator the controller will browse for (host derives _L<disc>/_S<disc>
// subtypes from the D= TXT value). Marks g.commissionable so PASE is accepted.
static matter_err_t mtrc_publish_commissionable(uint16_t disc, int cm) {
  g.commissionable = true;
  if (!g.port.mdns_publish) return MATTER_OK;
  static char txt_d[16], txt_cm[8], txt_vp[24];
  snprintf(txt_d,  sizeof(txt_d),  "D=%u", (unsigned)disc);
  snprintf(txt_cm, sizeof(txt_cm), "CM=%d", cm);   // 1 = standard, 2 = enhanced
  snprintf(txt_vp, sizeof(txt_vp), "VP=%u+%u", (unsigned)g.cfg.vendor_id,
           (unsigned)g.cfg.product_id);
  const char *txt[] = { txt_d, txt_cm, txt_vp };
  uint8_t inst[8] = {0};
  if (g.port.random_bytes) g.port.random_bytes(g.port.ctx, inst, 8);
  char instance[17];
  for (int i = 0; i < 8; i++) snprintf(instance + 2*i, 3, "%02X", inst[i]);
  matter_err_t e = g.port.mdns_publish(g.port.ctx, "matterc", instance,
                                       MTRC_COMMISSION_PORT, txt, 3);
  mlog(e == MATTER_OK ? MATTER_LOG_INFO : MATTER_LOG_ERROR,
       e == MATTER_OK ? "matter_c: commissioning window open (_matterc._udp)"
                      : "matter_c: commissionable mDNS publish failed");
  return e;
}

matter_err_t matter_open_commissioning_window(void) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  g.ocw_active = false; g.ocw_expiry_ms = 0;   // standard (on-device passcode) window
  return mtrc_publish_commissionable(g.cfg.discriminator, 1);
}

// Toggle the commissionable flag (host closes the window on timeout/Unbind).
// While closed the PASE responder ignores new commissioning; existing fabrics
// (CASE/operational) are unaffected.
void matter_set_commissionable(int on) {
  if (!g_ptr) return;
  g.commissionable = on ? true : false;
  if (!on) { g.ocw_active = false; g.ocw_expiry_ms = 0; }   // closing the window ends any enhanced OCW
}
int matter_is_commissionable(void) { return (g_ptr && g.commissionable) ? 1 : 0; }

matter_err_t matter_stop(void) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  g.started = false;
  mlog(MATTER_LOG_INFO, "matter_c stop (stub)");
  return MATTER_OK;
}

matter_err_t matter_factory_reset(void) {
  if (!g_ptr || !g.inited) return MATTER_ERR_NOT_INIT;
  for (int i = 0; i < mtrc_store_count(); i++)           // withdraw operational mDNS
    unpublish_operational_mdns(mtrc_store_at(i));        // before the table is wiped
  mtrc_store_reset();                                    // wipe in-RAM fabric table
  if (g.port.kv_del) g.port.kv_del(g.port.ctx, MTRC_KV_FABRICS);   // wipe persisted blob
  g.pase_secure = false; g.case_secure = false;
  memset(g.case_sess, 0, sizeof(g.case_sess));   // drop all operational sessions
  g.fs_armed = false; g.fs_added_fabric = 0;     // no tentative fabric survives a reset
  g.ocw_active = false; g.ocw_expiry_ms = 0;     // drop any enhanced commissioning window
  mlog(MATTER_LOG_INFO, "matter_c factory reset (fabrics wiped)");
  return MATTER_OK;
}

// ---- PASE responder ----------------------------------------------------
// Send a Secure Channel message on the unsecured session (id 0). The
// response always reflects the initiator's exchange and (optionally) acks.
static void pase_send(uint8_t opcode, const uint8_t *payload, size_t plen,
                      bool has_ack, uint32_t ack_counter, bool reliable) {
  mtrc_msg_header mh; memset(&mh, 0, sizeof(mh));
  mh.session_id = 0; mh.session_type = 0; mh.msg_counter = ++g.tx_counter;
  // Responder echoes the initiator's ephemeral node id as Destination (and
  // carries no Source) so the controller can match its unauthenticated session.
  if (g.peer_has_node) { mh.dsiz = MTRC_DSIZ_NODE; mh.dest_node_id = g.peer_node_id; }
  mtrc_proto_header ph; memset(&ph, 0, sizeof(ph));
  ph.initiator   = false;          // we are the responder
  ph.ack         = has_ack;
  ph.ack_counter = ack_counter;
  ph.reliability = reliable;
  ph.opcode      = opcode;
  ph.exchange_id = g.exchange_id;
  ph.protocol_id = MTRC_PROTO_SECURE_CHANNEL;
  uint8_t out[1280];
  int n = mtrc_frame_encode(out, sizeof(out), &mh, &ph, payload, plen);
  if (n > 0 && g.port.udp_send) {
    // Cache as the "last reply" so an MRP retransmit (same peer counter) can be
    // answered by re-sending these exact bytes instead of re-running crypto.
    if ((size_t)n <= sizeof(g.last_tx_buf)) {
      memcpy(g.last_tx_buf, out, (size_t)n); g.last_tx_len = (size_t)n;
    } else { g.last_tx_len = 0; }
    g.port.udp_send(g.port.ctx, g.reply_ip6, g.reply_port, out, (size_t)n);  // reply to this packet's source
  }
}

// PBKDFParamRequest -> PBKDFParamResponse (we choose salt + iterations).
static void pase_handle_param_req(const uint8_t *payload, size_t plen,
                                  const mtrc_msg_header *mh) {
  if (!g.commissionable) {            // commissioning window closed -> not pairable
    mlog(MATTER_LOG_INFO, "PASE: ignored (commissioning window closed)");
    return;
  }
  mtrc_pase_param_req req;
  if (!mtrc_pase_decode_param_req(payload, plen, &req)) return;
  g.peer_session_id = req.initiator_session_id;
  memcpy(g.init_random, req.initiator_random, 32);

  // A fresh PASE handshake begins a new commissioning: discard any pending
  // operational keypair / trusted root from a prior (aborted) attempt so the
  // CSRRequest mints a new key bound to THIS commissioning's NOC.
  g.have_pending_op = false;
  g.have_pending_root = false;
  // Fresh PASE secure session ahead: forget the prior session's last inbound
  // counter (and drop the cached reply) so a low starting counter can't be
  // mistaken for a duplicate of the previous commissioning.
  g.pase_have_rx_ctr = false; g.sec_last_tx_len = 0;
  // …and abandon a fabric left tentative by a prior AddNOC that never reached
  // CommissioningComplete — else a controller retrying within the fail-safe
  // window leaks one fabric per attempt (the AddNOC=TableFull root cause).
  if (g.fs_added_fabric) matter_failsafe_disarm(false);

  g.port.random_bytes(g.port.ctx, g.resp_random, 32);
  g.port.random_bytes(g.port.ctx, g.salt, 16);
  uint16_t sid = 0;
  g.port.random_bytes(g.port.ctx, (uint8_t *)&sid, 2);
  g.my_session_id = sid ? sid : 1;
  g.iterations = 1000;

  mtrc_pase_param_resp resp; memset(&resp, 0, sizeof(resp));
  memcpy(resp.initiator_random, req.initiator_random, 32);
  memcpy(resp.responder_random, g.resp_random, 32);
  resp.responder_session_id = g.my_session_id;
  resp.iterations = g.iterations;
  memcpy(resp.salt, g.salt, 16); resp.salt_len = 16;

  // Enhanced commissioning window (AdministratorCommissioning OpenCommissioningWindow):
  // the second admin derived its w0 from the passcode using ITS salt+iterations, so
  // we must echo exactly those (and pake1 will use the supplied external w0/L) — the
  // device's own passcode is NOT involved in this PASE.
  if (g.ocw_active) {
    g.iterations = g.ocw_iterations;
    resp.iterations = g.ocw_iterations;
    memcpy(resp.salt, g.ocw_salt, g.ocw_salt_len); resp.salt_len = g.ocw_salt_len;
  }

  uint8_t rp[160];
  int rn = mtrc_pase_encode_param_resp(rp, sizeof(rp), &resp);
  if (rn < 0) return;

  // SPAKE2+ transcript context = SHA256(prefix || req || resp), saved for Pake.
  mtrc_pase_context(payload, plen, rp, (size_t)rn, g.context);

  pase_send(MTRC_SC_PBKDF_PARAM_RSP, rp, (size_t)rn, true, mh->msg_counter, true);
  g.pase_phase = 1;
  mlog(MATTER_LOG_INFO, "PASE: PBKDFParamResponse sent");
}

// Pake1 (pA) -> Pake2 (pB, cB). Verifier-side SPAKE2+ (heavy: PBKDF2 + EC).
static void pase_handle_pake1(const uint8_t *payload, size_t plen,
                              const mtrc_msg_header *mh) {
  uint8_t pA[65];
  if (!mtrc_pase_decode_pake1(payload, plen, pA)) return;

  uint8_t w0[32], w1[32], L[65], y[32], pB[65], Z[65], V[65];
  if (g.ocw_active) {
    // Enhanced window: the admin supplied the SPAKE2+ verifier directly
    // (w0 || L). The device passcode / w1 are NOT used — skip the PBKDF + EC
    // derivation and install the supplied verifier.
    memcpy(w0, g.ocw_w0, 32);
    memcpy(L,  g.ocw_L,  65);
  } else {
    if (!mtrc_pase_derive_w0w1(g.cfg.passcode, g.salt, 16, g.iterations, w0, w1)) return;
    if (!mtrc_ec_mulgen(L, w1, 32)) return;                    // L = w1*G
  }
  g.port.random_bytes(g.port.ctx, y, 32);
  if (!mtrc_spake2p_verifier_Y(w0, y, pB)) return;             // pB = y*G + w0*N
  if (!mtrc_spake2p_verifier_ZV(w0, y, pA, L, Z, V)) return;   // Z,V from pA,L

  mtrc_pase_keys_t k;
  if (!mtrc_pase_keys(g.context, pA, pB, Z, V, w0, &k)) return;
  memcpy(g.cA_expected, k.cA, 32);                             // expect this in Pake3
  memcpy(g.i2r, k.i2r, 16); memcpy(g.r2i, k.r2i, 16); memcpy(g.att, k.att, 16);

  uint8_t out[160];
  int n = mtrc_pase_encode_pake2(out, sizeof(out), pB, k.cB);  // send pB + cB
  if (n < 0) return;
  pase_send(MTRC_SC_PASE_PAKE2, out, (size_t)n, true, mh->msg_counter, true);
  g.pase_phase = 2;
  mlog(MATTER_LOG_INFO, "PASE: Pake2 sent (SPAKE2+ verifier)");
}

// Pake3 (cA) -> verify, then StatusReport. On success the PASE session keys
// are live (g.i2r / g.r2i / g.att).
static void pase_handle_pake3(const uint8_t *payload, size_t plen,
                              const mtrc_msg_header *mh) {
  uint8_t cA[32];
  if (!mtrc_pase_decode_pake3(payload, plen, cA)) return;
  uint8_t diff = 0;
  for (int i = 0; i < 32; i++) diff |= (uint8_t)(cA[i] ^ g.cA_expected[i]);

  // Secure Channel StatusReport: GeneralCode(2) | ProtocolId(4) | Code(2), LE.
  uint8_t sr[8]; memset(sr, 0, 8);
  if (diff == 0) {
    // GeneralCode=0 (Success), ProtocolId=0 (SecureChannel),
    // Code=0 (SessionEstablishmentSuccess)
    g.pase_secure = true; g.pase_phase = 3;
    pase_send(MTRC_SC_STATUS_REPORT, sr, 8, true, mh->msg_counter, true);
    mlog(MATTER_LOG_INFO, "PASE: cA verified -> SESSION ESTABLISHED (StatusReport success)");
  } else {
    sr[0] = 0x01;   // GeneralCode = 1 (Failure)
    pase_send(MTRC_SC_STATUS_REPORT, sr, 8, true, mh->msg_counter, true);
    g.pase_phase = 0;
    mlog(MATTER_LOG_ERROR, "PASE: cA MISMATCH -> StatusReport failure");
  }
}

// ---- CASE responder (operational session establishment) ----------------
// Raw AES-CCM seal/open for the CASE TBE blobs: blob = ciphertext || tag,
// AAD empty (Core Spec §4.13.2). Returns 1 on success.
static int case_seal(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *pt, size_t pt_len, uint8_t *out) {
  memcpy(out, pt, pt_len);
  return mtrc_aes_ccm_encrypt(key, nonce, 13, NULL, 0, out, pt_len, out + pt_len, 16);
}
static int case_open(const uint8_t key[16], const uint8_t nonce[13],
                     const uint8_t *blob, size_t blob_len, uint8_t *out) {
  if (blob_len < 16) return 0;
  size_t ct = blob_len - 16;
  memcpy(out, blob, ct);
  return mtrc_aes_ccm_decrypt(key, nonce, 13, NULL, 0, out, ct, blob + ct, 16);
}

// Operational IPK for CASE = Crypto_KDF(epochIPK, salt=compressedFabricId,
// info="GroupKey v1.0", 16). ALL CASE crypto (destinationId, Sigma2/3 keys and
// the operational session keys) uses this derived key, NOT the raw IPK epoch
// key stored from AddNOC (Core Spec §4.15.3 "Operational Group Key Derivation"
// + §4.14.2 destinationId). Berry: Matter_Fabric.get_ipk_group_key(). Using the
// raw epoch IPK makes the responder's destinationId never match a real
// controller (Apple) -> "no fabric matches destinationId" and CASE never starts.
static void fabric_op_ipk(const mtrc_fabric *f, uint8_t op_ipk[16]) {
  uint8_t salt[8];
  for (int i = 0; i < 8; i++) salt[i] = (uint8_t)(f->fabric_id >> (8 * (7 - i)));
  uint8_t cfid[8];
  if (mtrc_hkdf_sha256(salt, sizeof(salt), f->root_pub + 1, 64,
                       (const uint8_t *)"CompressedFabric", 16, cfid, sizeof(cfid)) &&
      mtrc_hkdf_sha256(cfid, sizeof(cfid), f->ipk, 16,
                       (const uint8_t *)"GroupKey v1.0", 13, op_ipk, 16))
    return;
  memcpy(op_ipk, f->ipk, 16);   // fallback (KDF cannot realistically fail)
}

// ---- CASE session table (concurrent operational sessions) --------------
// Copy the current working set (g.case_*) into a session slot, and back.
static void case_session_save(mtrc_case_sess *s) {
  s->in_use       = true;
  s->my_sid       = g.case_my_sid;
  s->peer_sid     = g.case_peer_sid;
  s->fabric_index = g.case_fabric_index;
  s->peer_node_id = g.case_peer_node_id;
  s->tx_counter   = g.case_sec_tx_counter;
  memcpy(s->i2r, g.case_i2r, 16);
  memcpy(s->r2i, g.case_r2i, 16);
  memcpy(s->att, g.case_att, 16);
}
static void case_session_load(const mtrc_case_sess *s) {
  g.case_my_sid         = s->my_sid;
  g.case_peer_sid       = s->peer_sid;
  g.case_fabric_index   = s->fabric_index;
  g.case_peer_node_id   = s->peer_node_id;
  g.case_sec_tx_counter = s->tx_counter;
  memcpy(g.case_i2r, s->i2r, 16);
  memcpy(g.case_r2i, s->r2i, 16);
  memcpy(g.case_att, s->att, 16);
  g.case_secure = true;
}
static mtrc_case_sess *case_session_find(uint16_t my_sid) {
  for (int i = 0; i < MTRC_MAX_CASE_SESS; i++)
    if (g.case_sess[i].in_use && g.case_sess[i].my_sid == my_sid) return &g.case_sess[i];
  return NULL;
}
// Reuse an existing slot for this sid, else a free slot, else evict the
// least-recently-active session (smallest sub_last_ms; never-subscribed slots
// have sub_last_ms==0 and are reclaimed first). Evicting slot 0 unconditionally
// would tend to kill a live controller; LRU keeps the busy ones alive.
static mtrc_case_sess *case_session_alloc(uint16_t my_sid) {
  mtrc_case_sess *s = case_session_find(my_sid);
  if (s) return s;
  for (int i = 0; i < MTRC_MAX_CASE_SESS; i++)
    if (!g.case_sess[i].in_use) return &g.case_sess[i];
  int victim = 0; uint32_t oldest = g.case_sess[0].sub_last_ms;
  for (int i = 1; i < MTRC_MAX_CASE_SESS; i++) {
    if (g.case_sess[i].sub_last_ms < oldest) { oldest = g.case_sess[i].sub_last_ms; victim = i; }
  }
  return &g.case_sess[victim];
}

// Sigma1 -> Sigma2. Match destinationId to a stored fabric, do ECDH, seal our
// NOC + a signature over TBSData2 with the operational key, send Sigma2.
static void case_handle_sigma1(const uint8_t *pl, size_t pll,
                               const mtrc_msg_header *mh) {
  mtrc_sigma1 s1;
  if (!mtrc_sigma1_decode(pl, pll, &s1)) return;

  mtrc_fabric *f = NULL; uint8_t cand[32]; uint8_t op_ipk[16];
  for (int i = 0; i < mtrc_store_count(); i++) {
    mtrc_fabric *cf = mtrc_store_at(i);
    uint8_t cf_ipk[16]; fabric_op_ipk(cf, cf_ipk);
    mtrc_case_destination_id(cf_ipk, s1.initiator_random, cf->root_pub,
                             cf->fabric_id, cf->node_id, cand);
    if (memcmp(cand, s1.destination_id, 32) == 0) {
      f = cf; memcpy(op_ipk, cf_ipk, 16); break;
    }
  }
  if (!f) { mlog(MATTER_LOG_ERROR, "CASE: no fabric matches destinationId"); return; }

  g.case_hs_fabric_index = f->fabric_index;
  g.case_hs_peer_sid = s1.initiator_session_id;
  memcpy(g.case_init_eph, s1.initiator_eph_pub, 65);   // needed for Sigma3 verify
  g.port.random_bytes(g.port.ctx, g.case_re_priv, 32);
  if (!mtrc_ec_pub_from_priv(g.case_re_pub, g.case_re_priv)) return;
  g.port.random_bytes(g.port.ctx, g.case_resp_random, 32);
  uint16_t sid = 0; g.port.random_bytes(g.port.ctx, (uint8_t *)&sid, 2);
  g.case_hs_my_sid = sid ? sid : 2;
  if (!mtrc_ecdh(g.case_shared, s1.initiator_eph_pub, g.case_re_priv)) return;

  if (pll > sizeof(g.case_tt)) return;
  memcpy(g.case_tt, pl, pll); g.case_tt_len = pll; g.case_len1 = pll;
  uint8_t h1[32]; mtrc_sha256(g.case_tt, g.case_len1, h1);

  uint8_t s2k[16];
  if (!mtrc_case_s2k(g.case_shared, op_ipk, g.case_resp_random, g.case_re_pub, h1, s2k))
    return;

  static uint8_t tmp[1100];
  mtrc_case_tbs tbs; memset(&tbs, 0, sizeof(tbs));
  tbs.noc = f->noc; tbs.noc_len = f->noc_len;
  tbs.icac = f->icac; tbs.icac_len = f->icac_len;     // include ICAC if fabric uses one
  memcpy(tbs.sender_pub, g.case_re_pub, 65);
  memcpy(tbs.receiver_pub, s1.initiator_eph_pub, 65);
  int nt = mtrc_case_tbs_encode(tmp, sizeof(tmp), &tbs);
  if (nt < 0) return;
  uint8_t ht[32]; mtrc_sha256(tmp, (size_t)nt, ht);

  mtrc_case_tbe tbe; memset(&tbe, 0, sizeof(tbe));
  tbe.noc = f->noc; tbe.noc_len = f->noc_len;
  tbe.icac = f->icac; tbe.icac_len = f->icac_len;     // ICAC into encrypted TBEData2 too
  // TBEData2 tag 4 = resumptionID. The responder always supplies a fresh 16-byte
  // resumptionID in Sigma2 (Core Spec §4.14.2.5.3); controllers (Apple) expect
  // it and reject Sigma2 as InvalidParameter if it is missing. We don't support
  // session resumption yet, so a fresh random value per CASE is fine.
  uint8_t resumption_id[16];
  g.port.random_bytes(g.port.ctx, resumption_id, 16);
  tbe.resumption_id = resumption_id; tbe.resumption_id_len = 16;
  if (!mtrc_ecdsa_sign(tbe.signature, ht, f->op_priv)) return;
  int ne = mtrc_case_tbe_encode(tmp, sizeof(tmp), &tbe);
  if (ne < 0) return;

  static uint8_t enc2[1100];
  if (!case_seal(s2k, MTRC_CASE_NONCE_SIGMA2, tmp, (size_t)ne, enc2)) return;

  mtrc_sigma2 s2; memset(&s2, 0, sizeof(s2));
  memcpy(s2.responder_random, g.case_resp_random, 32);
  s2.responder_session_id = g.case_hs_my_sid;
  memcpy(s2.responder_eph_pub, g.case_re_pub, 65);
  s2.encrypted2 = enc2; s2.encrypted2_len = (size_t)ne + 16;
  static uint8_t s2buf[1280];
  int n2 = mtrc_sigma2_encode(s2buf, sizeof(s2buf), &s2);
  if (n2 < 0) return;
  if (g.case_tt_len + (size_t)n2 <= sizeof(g.case_tt)) {
    memcpy(g.case_tt + g.case_tt_len, s2buf, n2); g.case_tt_len += (size_t)n2;
  }
  pase_send(MTRC_SC_CASE_SIGMA2, s2buf, (size_t)n2, true, mh->msg_counter, true);
  g.case_phase = 1;
  mlog(MATTER_LOG_INFO, "CASE: Sigma2 sent (responder authenticated)");
}

// Sigma3 -> operational session. Decrypt TBEData3, verify the initiator's
// signature with the public key from its NOC, derive the session keys.
static void case_handle_sigma3(const uint8_t *pl, size_t pll,
                               const mtrc_msg_header *mh) {
  if (g.case_phase != 1) return;
  mtrc_fabric *f = mtrc_store_by_index(g.case_hs_fabric_index);
  if (!f) return;
  mtrc_sigma3 s3;
  if (!mtrc_sigma3_decode(pl, pll, &s3)) return;

  uint8_t op_ipk[16]; fabric_op_ipk(f, op_ipk);   // derived operational IPK (not raw epoch)

  // h12 = SHA256(Sigma1||Sigma2): case_tt currently holds exactly those two.
  uint8_t h12[32]; mtrc_sha256(g.case_tt, g.case_tt_len, h12);
  uint8_t s3k[16];
  if (!mtrc_case_s3k(g.case_shared, op_ipk, h12, s3k)) return;

  static uint8_t tbe3[1024];
  // SECURITY: encrypted3_len is straight off the wire; case_open memcpy's
  // (encrypted3_len - 16) into tbe3 BEFORE tag verification. Bound it or an
  // oversized Sigma3 overflows tbe3 (BSS corruption) regardless of key validity.
  if (s3.encrypted3_len < 16 || s3.encrypted3_len - 16 > sizeof(tbe3)) {
    mlog(MATTER_LOG_ERROR, "CASE: Sigma3 TBE oversize"); return;
  }
  if (!case_open(s3k, MTRC_CASE_NONCE_SIGMA3, s3.encrypted3, s3.encrypted3_len, tbe3)) {
    mlog(MATTER_LOG_ERROR, "CASE: Sigma3 TBE decrypt failed"); return;
  }
  mtrc_case_tbe t3;
  if (!mtrc_case_tbe_decode(tbe3, s3.encrypted3_len - 16, &t3)) return;

  // Verify the initiator's signature over TBSData3 with its NOC public key.
  // Relaxed for first interop: full cert-chain verify (A1b) is a later step;
  // here we parse the NOC (A1a) for its pubkey and check the Sigma3 signature.
  // TBSData3 MUST include the initiator's ICAC when present (Apple sends one),
  // else the reconstructed hash won't match the signature.
  mtrc_cert nc; int verified = 0;
  if (mtrc_cert_parse(t3.noc, t3.noc_len, &nc) && nc.have_pubkey) {
    // Remember the controller's operational node id — the operational-session
    // decrypt nonce uses the SENDER's node id, and operational messages omit it
    // from the header (implied by the session).
    if (nc.have_node_id) g.case_hs_peer_node_id = nc.subject_node_id;
    mtrc_case_tbs tbs; memset(&tbs, 0, sizeof(tbs));
    tbs.noc = t3.noc; tbs.noc_len = t3.noc_len;
    tbs.icac = t3.icac; tbs.icac_len = t3.icac_len;
    memcpy(tbs.sender_pub, g.case_init_eph, 65);   // initiator was sender
    memcpy(tbs.receiver_pub, g.case_re_pub, 65);
    static uint8_t tmp[1100];
    int nt = mtrc_case_tbs_encode(tmp, sizeof(tmp), &tbs);
    if (nt > 0) {
      uint8_t ht[32]; mtrc_sha256(tmp, (size_t)nt, ht);
      verified = mtrc_ecdsa_verify(t3.signature, ht, nc.pubkey);
    }
  }
  if (!verified) {
    mlog(MATTER_LOG_ERROR, "CASE: initiator NOC signature INVALID");
    uint8_t sr[8]; memset(sr, 0, 8); sr[0] = 0x01;   // GeneralCode = Failure
    pase_send(MTRC_SC_STATUS_REPORT, sr, 8, true, mh->msg_counter, true);
    g.case_phase = 0;
    return;
  }

  // A1b (structural): the initiator NOC[/ICAC] chain must be internally
  // consistent and bound to the fabric matched in Sigma1. (Full X.509-DER
  // signature-chain verification is the remaining A1b step; the fabric ROOT is
  // already bound by Sigma1's destinationId = HMAC over RootPubKey + IPK.)
  {
    mtrc_cert icac_c;
    int have_icac = (t3.icac && t3.icac_len > 0 &&
                     mtrc_cert_parse(t3.icac, t3.icac_len, &icac_c));
    if (!mtrc_cert_chain_check(&nc, have_icac ? &icac_c : NULL,
                               f ? f->fabric_id : 0, 0)) {
      mlog(MATTER_LOG_ERROR, "CASE: initiator NOC chain check FAILED");
      uint8_t sr[8]; memset(sr, 0, 8); sr[0] = 0x01;   // GeneralCode = Failure
      pase_send(MTRC_SC_STATUS_REPORT, sr, 8, true, mh->msg_counter, true);
      g.case_phase = 0;
      return;
    }
  }

  // Append Sigma3 to the transcript, derive the operational session keys.
  if (g.case_tt_len + pll <= sizeof(g.case_tt)) {
    memcpy(g.case_tt + g.case_tt_len, pl, pll); g.case_tt_len += pll;
  }
  uint8_t hall[32]; mtrc_sha256(g.case_tt, g.case_tt_len, hall);
  uint8_t k_i2r[16], k_r2i[16], k_att[16];
  if (!mtrc_case_session_keys(g.case_shared, op_ipk, hall, k_i2r, k_r2i, k_att)) return;
  g.case_phase = 2;
  // Operational message counter MUST start at a random value (Core Spec §4.5.1.1);
  // a fresh session starting at 1 can be rejected by strict receivers (Apple).
  // secured_send pre-increments, so the first sent counter = this value + 1.
  uint32_t c0 = 0; g.port.random_bytes(g.port.ctx, (uint8_t *)&c0, 4);

  // Store the established session in a slot (built from handshake scratch, never
  // from the working set — so a concurrent session's traffic can't corrupt it),
  // then load it as the active working set for any immediate request on it.
  mtrc_case_sess *ss = case_session_alloc(g.case_hs_my_sid);
  ss->in_use       = true;
  ss->my_sid       = g.case_hs_my_sid;
  ss->peer_sid     = g.case_hs_peer_sid;
  ss->fabric_index = g.case_hs_fabric_index;
  ss->peer_node_id = g.case_hs_peer_node_id;
  // Drop STALE prior sessions for the same fabric+node so the table can't fill
  // with duplicates. Previous version evicted EVERY matching session here, which
  // turned out to be too aggressive: Apple Home opens a fresh CASE handshake
  // every ~45-60 s in some configurations (.122 captured 72 Sigma1s in 40 min)
  // and each Sigma3 would nuke Apple's still-active subscription session ->
  // Apple loses the session -> opens a NEW one immediately -> repeat forever.
  // After enough churn the Sigma2 builder eventually wedged on a deterministic
  // state-corruption bug and the device froze.
  //
  // New policy: only evict a matching session if it has been quiet for >= 60 s.
  // - sub_last_ms == 0 (never subscribed): keep — could be a fresh just-opened
  //   session or a read-only client; LRU at table-full handles real pressure.
  // - sub_last_ms within 60 s of now: still being used, keep.
  // - sub_last_ms > 60 s old: actually stale, safe to drop.
  uint32_t now_ms = g.port.millis(g.port.ctx);
  for (int i = 0; i < MTRC_MAX_CASE_SESS; i++) {
    mtrc_case_sess *o = &g.case_sess[i];
    if (o == ss || !o->in_use) continue;
    if (o->fabric_index != ss->fabric_index) continue;
    if (o->peer_node_id != ss->peer_node_id) continue;
    if (o->sub_last_ms == 0) continue;                 // fresh / no-sub: keep
    if ((uint32_t)(now_ms - o->sub_last_ms) < 60000u)  // active within 60s: keep
      continue;
    memset(o, 0, sizeof(*o));
  }
  ss->tx_counter   = (c0 & 0x0FFFFFFF) | 1;   // random, MSB clear, non-zero
  memcpy(ss->i2r, k_i2r, 16);
  memcpy(ss->r2i, k_r2i, 16);
  memcpy(ss->att, k_att, 16);
  ss->sub_active = false;                      // no subscription on a fresh session
  ss->have_rx_ctr = false;                     // fresh inbound counter space (slot may be reused)
  case_session_load(ss);

  uint8_t sr[8]; memset(sr, 0, 8);   // GeneralCode = Success
  pase_send(MTRC_SC_STATUS_REPORT, sr, 8, true, mh->msg_counter, true);
  { int nact = 0;
    for (int i = 0; i < MTRC_MAX_CASE_SESS; i++) if (g.case_sess[i].in_use) nact++;
    char m[72]; snprintf(m, sizeof(m),
      "CASE: Sigma3 verified -> OPERATIONAL SESSION sid=%u (%d active)",
      (unsigned)ss->my_sid, nact);
    mlog(MATTER_LOG_INFO, m); }
}

// Active secure-session TX context (PASE or CASE), selected before each
// dispatch / report so secured_send addresses the right session id, response
// key (R2I) and message counter.
static struct { const uint8_t *key; uint16_t sid; uint32_t *ctr; uint64_t src; uint64_t dst; } g_tx =
  { NULL, 0, NULL, 0, 0 };
static void tx_use_pase(void) {
  g_tx.key = g.r2i; g_tx.sid = g.peer_session_id; g_tx.ctr = &g.sec_tx_counter;
  g_tx.src = 0;   // commissioning peer has no operational node id -> nonce src 0
  g_tx.dst = 0;
}
static void tx_use_case(void) {
  g_tx.key = g.case_r2i; g_tx.sid = g.case_peer_sid; g_tx.ctr = &g.case_sec_tx_counter;
  // Operational responses: the NONCE source is OUR node id (not carried in the
  // header), and the header carries the DESTINATION = the controller's node id
  // (Core Spec §4.4 / Berry build_response). The device's own id is implied by
  // the session, so we do NOT set a Source Node ID (S flag) on our messages.
  mtrc_fabric *cf = mtrc_store_by_index(g.case_fabric_index);
  g_tx.src = cf ? cf->node_id : 0;
  g_tx.dst = g.case_peer_node_id;
}

// Send an encrypted message on the active secured session (PASE or CASE):
// the R2I key, our session-id assigned to the peer, our secured counter,
// acking the inbound message.
static void secured_send(uint8_t opcode, uint16_t protocol_id,
                         const uint8_t *payload, size_t plen,
                         uint16_t exch, bool has_ack, uint32_t ack_counter,
                         bool reliable) {
  if (!g_tx.key || !g_tx.ctr) return;
  mtrc_msg_header mh; memset(&mh, 0, sizeof(mh));
  mh.session_id = g_tx.sid; mh.session_type = 0;
  mh.msg_counter = ++(*g_tx.ctr);
  mh.src_node_id = g_tx.src;          // nonce source = our node id (CASE) / 0 (PASE); NOT in header
  mh.has_src = false;                 // device messages carry no Source Node ID (implied by session)
  if (g_tx.dst) {                     // operational: address the response TO the controller
    mh.dsiz = MTRC_DSIZ_NODE; mh.dest_node_id = g_tx.dst;
  }
  { char dm[88]; snprintf(dm, sizeof(dm), "TX op=0x%02X sid=%u src=0x%08lX dst=0x%08lX ctr=%u",
      (unsigned)opcode, (unsigned)g_tx.sid, (unsigned long)g_tx.src,
      (unsigned long)g_tx.dst, (unsigned)mh.msg_counter); mlog(MATTER_LOG_DEBUG, dm); }
  mtrc_proto_header ph; memset(&ph, 0, sizeof(ph));
  ph.initiator = false; ph.ack = has_ack; ph.ack_counter = ack_counter;
  ph.reliability = reliable; ph.opcode = opcode; ph.exchange_id = exch;
  ph.protocol_id = protocol_id;
#ifdef MTRC_DIAG_HANS
  // TX twin of the "DIAG secured rx" line — shows what the device sends (e.g. the
  // StatusResponse op=0x01 answering a TimedRequest, and InvokeResponses). -DMTRC_DIAG_HANS.
  { char dt[96]; snprintf(dt, sizeof(dt),
      "DIAG secured tx proto=0x%04X op=0x%02X exch=0x%04X plen=%u%s%s",
      (unsigned)protocol_id, (unsigned)opcode, (unsigned)exch, (unsigned)plen,
      reliable ? " R" : "", has_ack ? " ack" : "");
    mlog(MATTER_LOG_INFO, dt); }
#endif
  static uint8_t out[1280];
  int n = mtrc_sec_encode(out, sizeof(out), &mh, &ph, payload, plen, g_tx.key);
  if (n > 0 && g.port.udp_send) {
    // Cache as the "last secured reply" so an MRP retransmit (same inbound
    // counter on this session) can be answered by re-sending these exact bytes
    // rather than re-running the handler (which double-added a fabric on a
    // retransmitted AddNOC). See sec_last_tx_buf doc.
    if ((size_t)n <= sizeof(g.sec_last_tx_buf)) {
      memcpy(g.sec_last_tx_buf, out, (size_t)n); g.sec_last_tx_len = (size_t)n;
    } else { g.sec_last_tx_len = 0; }
    g.port.udp_send(g.port.ctx, g.reply_ip6, g.reply_port, out, (size_t)n);  // reply to this packet's source
  }
}

// Live value of an attribute. Resolution order:
//   1. host's on_attr_read (the firmware owns it, e.g. the real relay state),
//   2. the data-model registry (script/host-pushed cache),
//   3. legacy fallbacks (kept until every attribute is registered).
static uint64_t attr_value(uint16_t ep, uint32_t cl, uint32_t attr) {
  uint64_t v = 0;
  if (g.port.on_attr_read &&
      g.port.on_attr_read(g.port.ctx, ep, cl, attr, &v) == MATTER_OK) return v;
  if (mtrc_dm_get(ep, cl, attr, &v)) return v;
  if      (cl == 0x0006 && attr == 0x0000) v = g.onoff ? 1 : 0;
  else if (cl == 0x0028 && attr == 0x0002) v = g.cfg.vendor_id;
  else if (cl == 0x0028 && attr == 0x0004) v = g.cfg.product_id;
  return v;
}

// Extract a context-tagged field from the first command's CommandFields in an
// InvokeRequest (msg struct -> ctx2 array -> CommandDataIB -> ctx1 fields).
// want_bytes: 1 -> fill *bp/*blen (BYTES); 0 -> fill *uv (UINT). Returns 1/0.
static int inv_field(const uint8_t *buf, size_t len, uint32_t tag, int want_bytes,
                     const uint8_t **bp, size_t *blen, uint64_t *uv) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, buf, len);
  mtrc_tlv_elem e;
  if (!mtrc_tlv_read(&r, &e) || e.type != MTRC_TLV_STRUCT) return 0;  // message
  int depth = 1;
  while (depth >= 1 && mtrc_tlv_read(&r, &e)) {
    if (e.type == MTRC_TLV_END) { depth--; continue; }
    if (depth == 1 && e.tag.ctrl == MTRC_TLV_TAG_CONTEXT && e.tag.number == 2 &&
        e.type == MTRC_TLV_ARRAY) {                       // InvokeRequests
      mtrc_tlv_elem c;
      if (!mtrc_tlv_read(&r, &c) || c.type != MTRC_TLV_STRUCT) return 0;  // CommandDataIB
      int d2 = 1;
      while (d2 >= 1 && mtrc_tlv_read(&r, &c)) {
        if (c.type == MTRC_TLV_END) { d2--; continue; }
        if (d2 == 1 && c.tag.ctrl == MTRC_TLV_TAG_CONTEXT && c.tag.number == 1 &&
            c.type == MTRC_TLV_STRUCT) {                  // CommandFields
          mtrc_tlv_elem f;
          while (mtrc_tlv_read(&r, &f) && f.type != MTRC_TLV_END) {
            if (f.tag.ctrl == MTRC_TLV_TAG_CONTEXT && f.tag.number == tag) {
              // UTF8 and octet strings share the bytes/bytes_len fields; a string
              // field (e.g. UpdateFabricLabel's Label) reads through want_bytes too.
              if (want_bytes && (f.type == MTRC_TLV_BYTES || f.type == MTRC_TLV_UTF8)) { *bp = f.bytes; *blen = f.bytes_len; return 1; }
              if (!want_bytes && f.type == MTRC_TLV_UINT)  { *uv = f.u; return 1; }
            }
          }
          return 0;
        }
        if (c.type==MTRC_TLV_STRUCT||c.type==MTRC_TLV_ARRAY||c.type==MTRC_TLV_LIST) d2++;
      }
      return 0;
    }
    if (e.type==MTRC_TLV_STRUCT||e.type==MTRC_TLV_ARRAY||e.type==MTRC_TLV_LIST) depth++;
  }
  return 0;
}

// Build a CSRResponse (NOC cluster cmd 0x05): generate a fresh operational
// keypair, a PKCS#10 CSR for it, and the NOCSRElements{1:csr,2:CSRNonce} plus
// an attestationSignature over (NOCSRElements || attestationChallenge). The
// DAC key is a placeholder until A2 supplies the real device DAC. Returns the
// InvokeResponse length, or -1.
static int build_csr_response(uint8_t *out, size_t cap, uint16_t ep, uint32_t cl,
                              const uint8_t *nonce, size_t nlen) {
  // Generate the operational keypair ONCE per commissioning. MRP retransmits
  // the CSRRequest if our first CSRResponse is slow/lost; re-minting a key here
  // would leave the device holding a keypair the AddNOC-certified NOC does NOT
  // contain -> CASE Sigma2 is signed with the wrong key -> "Invalid signature"
  // and pairing fails (Apple/chip-tool/HA all reject it). Reuse the pending key
  // on a retransmit; the CSR pubkey then matches the NOC the controller builds.
  // have_pending_op is reset at the start of each new PASE handshake.
  if (!g.have_pending_op) {
    g.port.random_bytes(g.port.ctx, g.pending_op_priv, 32);
    if (!mtrc_ec_pub_from_priv(g.pending_op_pub, g.pending_op_priv)) return -1;
    g.have_pending_op = true;
  }

  static uint8_t csr[400];
  int csrlen = mtrc_csr_build(csr, sizeof(csr), g.pending_op_priv, g.pending_op_pub);
  if (csrlen < 0) return -1;

  static uint8_t nocsr[480];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, nocsr, sizeof(nocsr));
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), csr, (size_t)csrlen);
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), nonce, nlen);
  mtrc_tlv_end_container(&w);
  if (!mtrc_tlv_writer_ok(&w)) return -1;
  size_t nocsr_len = mtrc_tlv_writer_len(&w);

  // attestationSignature = ECDSA(DAC, SHA256(NOCSRElements || attChallenge)).
  // attChallenge is the PASE/CASE Att key; the controller verifies this against
  // the DAC public key it already received, so it MUST be the real DAC key.
#ifdef MTRC_ATTEST_TEST_CREDS
  const uint8_t *dac = MTRC_DAC_PRIV;
#else
  uint8_t dacbuf[32]; memset(dacbuf, 0x55, 32);
  const uint8_t *dac = dacbuf;
#endif
  static uint8_t hin[480 + 16];
  memcpy(hin, nocsr, nocsr_len); memcpy(hin + nocsr_len, g.att, 16);
  uint8_t h[32]; mtrc_sha256(hin, nocsr_len + 16, h);
  uint8_t attsig[64]; if (!mtrc_ecdsa_sign(attsig, h, dac)) return -1;

  mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // InvokeResponseMessage
  mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(0), false);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // InvokeResponses
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  InvokeResponseIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(0));          //   command CommandDataIB
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));            //    CommandPath
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), ep);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), cl);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), 0x05);        //    CSRResponse
  mtrc_tlv_end_container(&w);
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //    CommandFields
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(0), nocsr, nocsr_len);   // NOCSRElements
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), attsig, 64);         // attestationSignature
  mtrc_tlv_end_container(&w);
  mtrc_tlv_end_container(&w);                          //   end CommandDataIB
  mtrc_tlv_end_container(&w);                          //  end InvokeResponseIB
  mtrc_tlv_end_container(&w);                          // end InvokeResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

#ifdef MTRC_ATTEST_TEST_CREDS
// Build an InvokeResponse carrying a command (resp_cmd) whose CommandFields are
// one or two byte strings: {0:f0} and (if f1) {1:f1}. Covers CertificateChain
// Response, AttestationResponse, and similar. Returns length, or -1.
static int build_cmd_resp_bytes(uint8_t *out, size_t cap, uint16_t ep, uint32_t cl,
                                uint32_t resp_cmd,
                                const uint8_t *f0, size_t f0len,
                                const uint8_t *f1, size_t f1len) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // InvokeResponseMessage
  mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(0), false);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // InvokeResponses
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  InvokeResponseIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(0));          //   command CommandDataIB
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));            //    CommandPath
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), ep);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), cl);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), resp_cmd);
  mtrc_tlv_end_container(&w);
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //    CommandFields
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(0), f0, f0len);
  if (f1) mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), f1, f1len);
  mtrc_tlv_end_container(&w);
  mtrc_tlv_end_container(&w);                          //   end CommandDataIB
  mtrc_tlv_end_container(&w);                          //  end InvokeResponseIB
  mtrc_tlv_end_container(&w);                          // end InvokeResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);
  mtrc_tlv_end_container(&w);
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}
#endif  // MTRC_ATTEST_TEST_CREDS

// AttestationRequest -> AttestationResponse (cmd 0x01): attestationElements =
// {1:CD, 2:nonce, 3:timestamp}; attestationSignature = ECDSA(DAC, SHA256(
// attestationElements || attestationChallenge)). The challenge is the PASE
// Att key (g.att). DAC/CD come from the gated dev cred set (A2).
static int build_attestation_response(uint8_t *out, size_t cap, uint16_t ep,
                                      uint32_t cl, const uint8_t *nonce, size_t nlen) {
#ifdef MTRC_ATTEST_TEST_CREDS
  static uint8_t ae[768];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, ae, sizeof(ae));
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(1), MTRC_CD, (size_t)MTRC_CD_LEN);  // certificationDeclaration
  mtrc_tlv_put_bytes(&w, mtrc_tlv_ctx(2), nonce, nlen);                   // attestationNonce
  mtrc_tlv_put_uint (&w, mtrc_tlv_ctx(3), 0);                             // timestamp
  mtrc_tlv_end_container(&w);
  if (!mtrc_tlv_writer_ok(&w)) return -1;
  size_t ael = mtrc_tlv_writer_len(&w);
  static uint8_t hin[768 + 16];
  memcpy(hin, ae, ael); memcpy(hin + ael, g.att, 16);
  uint8_t h[32]; mtrc_sha256(hin, ael + 16, h);
  uint8_t sig[64]; if (!mtrc_ecdsa_sign(sig, h, MTRC_DAC_PRIV)) return -1;
  return build_cmd_resp_bytes(out, cap, ep, cl, 0x01, ae, ael, sig, 64);
#else
  (void)out; (void)cap; (void)ep; (void)cl; (void)nonce; (void)nlen; return -1;
#endif
}

// Build a NOCResponse (NOC cluster cmd 0x08): {0:statusCode, 1:fabricIndex,
// 2:debugText}. statusCode 0 = OK (NodeOperationalCertStatusEnum).
static int build_noc_response(uint8_t *out, size_t cap, uint16_t ep, uint32_t cl,
                              uint8_t status, uint8_t fabric_index) {
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, out, cap);
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // InvokeResponseMessage
  mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(0), false);
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));           // InvokeResponses
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          //  InvokeResponseIB
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(0));          //   command CommandDataIB
  mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));            //    CommandPath
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), ep);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), cl);
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), 0x08);        //    NOCResponse
  mtrc_tlv_end_container(&w);
  mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));          //    CommandFields
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), status);      //     statusCode
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), fabric_index);//     fabricIndex
  mtrc_tlv_put_utf8(&w, mtrc_tlv_ctx(2), "", 0);       //     debugText ""
  mtrc_tlv_end_container(&w);                          //    end CommandFields
  mtrc_tlv_end_container(&w);                          //   end CommandDataIB
  mtrc_tlv_end_container(&w);                          //  end InvokeResponseIB
  mtrc_tlv_end_container(&w);                          // end InvokeResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);
  mtrc_tlv_end_container(&w);                          // end message
  return mtrc_tlv_writer_ok(&w) ? (int)mtrc_tlv_writer_len(&w) : -1;
}

// AddNOC: parse the NOC + IPK + admin args, install the fabric (CSR-generated
// operational key + the AddTrustedRootCertificate root) into mtrc_store, and
// build the NOCResponse. NOC chain verify is relaxed for now (A1b).
// Publish the operational mDNS service (_matter._udp) for a commissioned fabric
// so controllers can re-discover this node operationally (Core Spec §4.3.1/4.3.4).
// Instance = <compressedFabricId(16 hex)>-<nodeId(16 hex)>; the host adds the
// _I<compressedFabricId> browse subtype. CFID = Crypto_KDF(rootPubKey[1..64],
// salt=fabricId(BE64), info="CompressedFabric", 64 bits).
// Compute a fabric's operational DNS-SD instance name: <CFID(16hex)>-<nodeId(16hex)>.
static int fabric_op_instance(const mtrc_fabric *f, char *instance, size_t cap) {
  if (!f) return 0;
  uint8_t salt[8];
  for (int i = 0; i < 8; i++) salt[i] = (uint8_t)(f->fabric_id >> (8 * (7 - i)));
  uint8_t cfid[8];
  if (!mtrc_hkdf_sha256(salt, sizeof(salt), f->root_pub + 1, 64,
                        (const uint8_t *)"CompressedFabric", 16, cfid, sizeof(cfid)))
    return 0;
  int p = 0;
  for (int i = 0; i < 8; i++) p += snprintf(instance + p, cap - p, "%02X", cfid[i]);
  // Node id as 16 hex, big-endian, formatted byte-by-byte. Do NOT use "%016llX":
  // ESP-IDF's newlib-nano printf (default on several Tasmota envs) ignores the
  // 'll' length modifier and emits garbage ("...lX") for a 64-bit value, which
  // corrupts the operational DNS-SD instance name so a controller can never
  // resolve the node for CASE (commissioning ends at "connecting"/"no response").
  if (p < (int)cap - 1) instance[p++] = '-';
  for (int i = 7; i >= 0; i--)
    p += snprintf(instance + p, cap - p, "%02X",
                  (unsigned)((f->node_id >> (i * 8)) & 0xFF));
  return 1;
}

// Withdraw a fabric's operational _matter._tcp record (RemoveFabric / Unbind),
// so a removed controller's stale instance does not linger until reboot.
static void unpublish_operational_mdns(const mtrc_fabric *f) {
  if (!g.port.mdns_remove || !f) return;
  char instance[40];
  if (!fabric_op_instance(f, instance, sizeof(instance))) return;
  g.port.mdns_remove(g.port.ctx, "matter", instance);
  char m[64]; snprintf(m, sizeof(m), "operational mDNS removed %s", instance);
  mlog(MATTER_LOG_INFO, m);
}

static void publish_operational_mdns(const mtrc_fabric *f) {
  if (!g.port.mdns_publish || !f) return;
  char instance[40];
  if (!fabric_op_instance(f, instance, sizeof(instance))) return;
  // Matter Core Spec §4.3.1.6 — operational mDNS TXT records:
  //   SII (Session Idle Interval, ms)        MANDATORY
  //   SAI (Session Active Interval, ms)      MANDATORY
  //   SAT (Session Active Threshold, ms)     MANDATORY (added 1.3)
  //   T   (TCP Support, bool: 0=UDP-only)    MANDATORY
  // Apple Home tolerates SAT missing; Google Nest rejects ("kann nicht
  // verbinden" right after CASE completes — Hans report 2026-05-27).
  // Default SAT per spec is 4000 ms.
  static const char *txt[] = { "SII=5000", "SAI=300", "SAT=4000", "T=0" };
  g.port.mdns_publish(g.port.ctx, "matter", instance, MTRC_COMMISSION_PORT, txt, 4);
  // Operational discovery is published by the host under _matter._TCP per Core
  // Spec §4.3.1 (transport is still UDP). The host port chooses the proto; this
  // log just reflects the spec'd service type.
  char m[96]; snprintf(m, sizeof(m), "operational mDNS: _matter._tcp %s TXT=SII/SAI/SAT/T", instance);
  mlog(MATTER_LOG_INFO, m);
}

static int build_addnoc(uint8_t *out, size_t cap, uint16_t ep, uint32_t cl,
                        const uint8_t *payload, size_t plen) {
  const uint8_t *noc = NULL, *ipk = NULL; size_t noclen = 0, ipklen = 0;
  const uint8_t *icac = NULL; size_t icaclen = 0;
  uint64_t admin_subj = 0, admin_vid = 0;
  if (!inv_field(payload, plen, 0, 1, &noc, &noclen, NULL)) {
    mlog(MATTER_LOG_INFO, "NOC: AddNOC FAIL InvalidNOC (no NOCValue field)");
    return build_noc_response(out, cap, ep, cl, 0x02, 0);   // InvalidNOC
  }
  // ICACValue (field 1) is OPTIONAL: present when the fabric issues NOCs via an
  // intermediate CA (NOC <- ICAC <- RCAC). Apple Home uses an ICAC; chip-tool by
  // default issues NOCs straight from the root. We MUST store it and echo it in
  // Sigma2, else the controller can't build the cert chain and rejects CASE.
  inv_field(payload, plen, 1, 1, &icac, &icaclen, NULL);
  inv_field(payload, plen, 2, 1, &ipk, &ipklen, NULL);
  inv_field(payload, plen, 3, 0, NULL, NULL, &admin_subj);
  inv_field(payload, plen, 4, 0, NULL, NULL, &admin_vid);

  mtrc_cert nc;
  if (!mtrc_cert_parse(noc, noclen, &nc) || !nc.have_pubkey ||
      !g.have_pending_op || !g.have_pending_root) {
    mlog(MATTER_LOG_INFO, "NOC: AddNOC FAIL InvalidNOC (cert parse / no pending op|root)");
    return build_noc_response(out, cap, ep, cl, 0x02, 0);   // InvalidNOC
  }

  mtrc_fabric *f = mtrc_store_alloc();
  if (!f) {
    char mf[72];
    snprintf(mf, sizeof(mf), "NOC: AddNOC FAIL TableFull (%d/%d fabrics) — matterReset to clear",
             mtrc_store_count(), MTRC_MAX_FABRICS);
    mlog(MATTER_LOG_INFO, mf);
    return build_noc_response(out, cap, ep, cl, 0x05, 0);   // TableFull
  }
  f->fabric_id = nc.have_fabric_id ? nc.subject_fabric_id : 0;
  f->node_id   = nc.have_node_id   ? nc.subject_node_id   : 0;
  f->admin_vendor_id = (uint16_t)admin_vid;
  memcpy(f->root_pub, g.pending_root_pub, 65);
  memset(f->ipk, 0, 16);
  if (ipk && ipklen <= 16) memcpy(f->ipk, ipk, ipklen);
  memcpy(f->op_priv, g.pending_op_priv, 32);
  memcpy(f->op_pub,  g.pending_op_pub, 65);
  if (noclen <= MTRC_NOC_MAX) { memcpy(f->noc, noc, noclen); f->noc_len = (uint16_t)noclen; }
  f->icac_len = 0;
  if (icac && icaclen && icaclen <= MTRC_NOC_MAX) {
    memcpy(f->icac, icac, icaclen); f->icac_len = (uint16_t)icaclen;
  }
  (void)admin_subj;

  char m[110];
  snprintf(m, sizeof(m), "NOC: fabric idx=%u node=0x%08lX noc=%uB icac=%uB ipk=%uB",
           (unsigned)f->fabric_index, (unsigned long)f->node_id,
           (unsigned)f->noc_len, (unsigned)f->icac_len, (unsigned)ipklen);
  mlog(MATTER_LOG_INFO, m);
  mtrc_persist_fabrics();          // survive reboots
  publish_operational_mdns(f);     // become discoverable on the operational fabric
  // Tentative until CommissioningComplete; rolled back if the fail-safe expires
  // or a new commissioning starts first (see matter_failsafe_disarm).
  g.fs_added_fabric = f->fabric_index;
  return build_noc_response(out, cap, ep, cl, 0x00, f->fabric_index);   // OK
}

// Disarm the Fail-Safe (Core Spec §11.10). committed=true => Commissioning
// Complete arrived, the AddNOC'd fabric stays. committed=false => the fail-safe
// expired / was disarmed / a new commissioning started before
// CommissioningComplete => roll back the tentative fabric so a failed attempt
// does not leak a persisted fabric (the AddNOC=TableFull root cause).
static void matter_failsafe_disarm(bool committed) {
  uint8_t idx = g.fs_added_fabric;
  if (!committed && idx) {
    mtrc_fabric *rf = mtrc_store_by_index(idx);
    if (rf) {
      unpublish_operational_mdns(rf);                  // withdraw its _matter._tcp
      for (int i = 0; i < MTRC_MAX_CASE_SESS; i++)     // drop any session on it
        if (g.case_sess[i].in_use && g.case_sess[i].fabric_index == idx)
          memset(&g.case_sess[i], 0, sizeof(g.case_sess[i]));
      mtrc_store_remove(idx);
      mtrc_persist_fabrics();
      char m[80];
      snprintf(m, sizeof(m), "fail-safe rollback: removed tentative fabric idx=%u (%d left)",
               (unsigned)idx, mtrc_store_count());
      mlog(MATTER_LOG_INFO, m);
    }
  }
  g.fs_armed = false;
  g.fs_added_fabric = 0;
}

// Handle a decrypted IM InvokeRequest. P3b.1 answers the General
// Commissioning commands ({errorCode, debugText}); anything else gets an
// UNSUPPORTED_COMMAND status.
static void im_handle_invoke(const uint8_t *payload, size_t plen,
                             uint16_t exch, uint32_t ack) {
  uint16_t ep; uint32_t cl, cmd;
  if (!mtrc_im_parse_first_command(payload, plen, &ep, &cl, &cmd)) return;
  char m[80];
  snprintf(m, sizeof(m), "IM Invoke ep=%u cluster=0x%04X cmd=0x%02X",
           (unsigned)ep, (unsigned)cl, (unsigned)cmd);
  mlog(MATTER_LOG_DEBUG, m);

  static uint8_t resp[1024];          // CSA DAC/PAI (~500B) + CD (~540B) responses
  int n = -1;
  if (cl == 0x003E && cmd == 0x02) {  // CertificateChainRequest -> Response(0x03)
#ifdef MTRC_ATTEST_TEST_CREDS
    uint64_t ct = 0;
    if (inv_field(payload, plen, 0, 0, NULL, NULL, &ct)) {
      if (ct == 1)
        n = build_cmd_resp_bytes(resp, sizeof(resp), ep, cl, 0x03, MTRC_DAC_DER, MTRC_DAC_DER_LEN, NULL, 0);
      else if (ct == 2)
        n = build_cmd_resp_bytes(resp, sizeof(resp), ep, cl, 0x03, MTRC_PAI_DER, MTRC_PAI_DER_LEN, NULL, 0);
    }
    if (n > 0) mlog(MATTER_LOG_INFO, "NOC: CertificateChainResponse sent");
#endif
  } else if (cl == 0x003E && cmd == 0x00) {   // AttestationRequest -> Response(0x01)
    const uint8_t *nonce = NULL; size_t nlen = 0;
    if (inv_field(payload, plen, 0, 1, &nonce, &nlen, NULL))
      n = build_attestation_response(resp, sizeof(resp), ep, cl, nonce, nlen);
    if (n > 0) mlog(MATTER_LOG_INFO, "NOC: AttestationResponse sent (DAC-signed)");
  } else if (cl == 0x003E && cmd == 0x04) {  // NOC: CSRRequest -> CSRResponse
    const uint8_t *nonce = NULL; size_t nlen = 0;
    if (inv_field(payload, plen, 0, 1, &nonce, &nlen, NULL))
      n = build_csr_response(resp, sizeof(resp), ep, cl, nonce, nlen);
    if (n > 0) mlog(MATTER_LOG_INFO, "NOC: CSRResponse sent (operational keypair + CSR)");
  } else if (cl == 0x003E && cmd == 0x0B) {   // AddTrustedRootCertificate
    const uint8_t *rc = NULL; size_t rcl = 0; mtrc_cert root;
    if (inv_field(payload, plen, 0, 1, &rc, &rcl, NULL) &&
        mtrc_cert_parse(rc, rcl, &root) && root.have_pubkey) {
      memcpy(g.pending_root_pub, root.pubkey, 65);
      g.have_pending_root = true;
      n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);   // SUCCESS
      mlog(MATTER_LOG_INFO, "NOC: trusted root stored");
    }
  } else if (cl == 0x003E && cmd == 0x06) {   // AddNOC -> NOCResponse
    n = build_addnoc(resp, sizeof(resp), ep, cl, payload, plen);
  } else if (cl == 0x003E && cmd == 0x0A) {   // RemoveFabric -> NOCResponse
    // When a controller removes this node from its fabric (Apple Home "Remove
    // Accessory"), forget that fabric: withdraw its operational mDNS, drop its
    // CASE sessions, delete it from the store and re-persist. Without this the
    // device keeps an orphaned fabric and a stale _matter._tcp record forever.
    uint64_t idx = 0;
    inv_field(payload, plen, 0, 0, NULL, NULL, &idx);        // field 0 = FabricIndex
    mtrc_fabric *rf = mtrc_store_by_index((uint8_t)idx);
    if (rf) {
      unpublish_operational_mdns(rf);
      for (int i = 0; i < MTRC_MAX_CASE_SESS; i++)
        if (g.case_sess[i].in_use && g.case_sess[i].fabric_index == (uint8_t)idx)
          memset(&g.case_sess[i], 0, sizeof(g.case_sess[i]));   // drop sessions on this fabric
      mtrc_store_remove((uint8_t)idx);
      mtrc_persist_fabrics();
      char m[48]; snprintf(m, sizeof(m), "NOC: RemoveFabric idx=%u (%d left)",
                           (unsigned)idx, mtrc_store_count());
      mlog(MATTER_LOG_INFO, m);
      n = build_noc_response(resp, sizeof(resp), ep, cl, 0x00, (uint8_t)idx);   // OK
    } else {
      n = build_noc_response(resp, sizeof(resp), ep, cl, 0x0B, (uint8_t)idx);   // InvalidFabricIndex
    }
  } else if (cl == 0x003E && cmd == 0x09) {   // UpdateFabricLabel -> NOCResponse
    // Mandatory OperationalCredentials command. We previously fell through to
    // UNSUPPORTED_COMMAND here: Apple/Google tolerated it, but Alexa treats the
    // rejection as fatal and ends commissioning with "unbekannter Fehler".
    // Apply the label to the ACCESSING fabric (the one this CASE session
    // belongs to) and persist it. Spec NOCResponse statuses: OK(0x00),
    // LabelConflict(0x0D) if another fabric already uses a non-empty label,
    // InvalidFabricIndex(0x0B) if there is no accessing fabric.
    const uint8_t *lbl = NULL; size_t lbllen = 0;
    inv_field(payload, plen, 0, 1, &lbl, &lbllen, NULL);     // field 0 = Label (UTF-8)
    if (lbllen > MTRC_LABEL_MAX) lbllen = MTRC_LABEL_MAX;
    mtrc_fabric *cf = mtrc_store_by_index(g.case_fabric_index);
    if (!cf) {
      n = build_noc_response(resp, sizeof(resp), ep, cl, 0x0B, g.case_fabric_index); // InvalidFabricIndex
    } else {
      int conflict = 0;
      if (lbllen) for (int i = 0; i < mtrc_store_count(); i++) {
        mtrc_fabric *o = mtrc_store_at(i);
        if (o && o != cf && (size_t)strlen(o->label) == lbllen &&
            memcmp(o->label, lbl, lbllen) == 0) { conflict = 1; break; }
      }
      if (conflict) {
        n = build_noc_response(resp, sizeof(resp), ep, cl, 0x0D, cf->fabric_index); // LabelConflict
      } else {
        if (lbllen) memcpy(cf->label, lbl, lbllen);
        cf->label[lbllen] = '\0';
        mtrc_persist_fabrics();
        char fm[64]; snprintf(fm, sizeof(fm), "NOC: UpdateFabricLabel idx=%u \"%s\"",
                              (unsigned)cf->fabric_index, cf->label);
        mlog(MATTER_LOG_INFO, fm);
        n = build_noc_response(resp, sizeof(resp), ep, cl, 0x00, cf->fabric_index); // OK
      }
    }
  } else if (cl == 0x0030) {          // General Commissioning (handled in-core)
    uint32_t rc = 0xFFFFFFFF;
    if (cmd == 0x00) {                // ArmFailSafe -> ArmFailSafeResponse
      rc = 0x01;
      uint64_t expiry = 0;            // field 0 = ExpiryLengthSeconds (uint16)
      inv_field(payload, plen, 0, 0, NULL, NULL, &expiry);
      if (expiry == 0) {
        // Disarm: roll back anything not yet committed (Core Spec §11.10.6.2).
        matter_failsafe_disarm(false);
      } else {
        // Arm / re-arm: (re-)start the timer. A re-arm during the same
        // commissioning keeps an already-added tentative fabric.
        g.fs_armed = true;
        g.fs_expiry_ms = g.port.millis(g.port.ctx) + (uint32_t)expiry * 1000u;
        char fm[48];
        snprintf(fm, sizeof(fm), "fail-safe armed %us", (unsigned)expiry);
        mlog(MATTER_LOG_DEBUG, fm);
      }
    }
    else if (cmd == 0x02) rc = 0x03;  // SetRegulatoryConfig -> Response
    else if (cmd == 0x04) {           // CommissioningComplete -> Response
      rc = 0x05;
      matter_failsafe_disarm(true);   // commit: the AddNOC'd fabric is now permanent
    }
    if (rc != 0xFFFFFFFF)
      n = mtrc_im_build_cmd_response_u8(resp, sizeof(resp), ep, cl, rc, 0); // errorCode OK
  } else if (cl == 0x0003 && (cmd == 0x00 || cmd == 0x40)) {  // Identify / TriggerEffect
    // We advertise these in the AcceptedCommandList, so we must accept them.
    // No physical identify indicator -> store IdentifyTime and reply SUCCESS.
    if (cmd == 0x00) { uint64_t t = 0; inv_field(payload, plen, 0, 0, NULL, NULL, &t);
                       mtrc_dm_set(ep, 0x0003, 0x0000, t & 0xFFFF); }
    n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);   // SUCCESS
  } else if (cl == 0x0006 && cmd <= 0x02) {   // OnOff: Off(0)/On(1)/Toggle(2)
    // Route to the application (script's MatterInvoke -> relay, or the built-in
    // relay default). on_command owns the action; on_attr_write is the legacy
    // fallback for hosts that don't provide on_command.
    int handled = 0;
    if (g.port.on_command)
      handled = g.port.on_command(g.port.ctx, ep, cl, cmd, (int32_t)cmd);
    else if (g.port.on_attr_write) {
      uint8_t action = (uint8_t)cmd;          // maps 1:1 to Tasmota POWER_*
      g.port.on_attr_write(g.port.ctx, ep, 0x0006, 0x0000, &action, 1);
      handled = 1;
    }
    uint64_t cur = 0; mtrc_dm_get(ep, 0x0006, 0x0000, &cur);   // per-endpoint state
    int newv = (cmd == 0x02) ? (cur ? 0 : 1) : (cmd == 0x01 ? 1 : 0);
    mtrc_dm_set(ep, 0x0006, 0x0000, newv);              // keep registry in sync
    g.onoff = newv;                                      // mirror (legacy relay fallback)
    if (handled)
      n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);  // SUCCESS
  } else if (cl == 0x0008 && (cmd == 0x00 || cmd == 0x04)) {  // LevelControl MoveToLevel[WithOnOff]
    uint64_t lvl = 0; inv_field(payload, plen, 0, 0, NULL, NULL, &lvl);   // field 0 = Level
    mtrc_dm_set(ep, 0x0008, 0x0000, lvl & 0xFF);                          // CurrentLevel
    if (cmd == 0x04 && (lvl & 0xFF)) mtrc_dm_set(ep, 0x0006, 0x0000, 1);  // WithOnOff -> On
    if (g.port.on_command) g.port.on_command(g.port.ctx, ep, cl, cmd, (int32_t)(lvl & 0xFF));
    n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);
  } else if (cl == 0x0300) {                                  // ColorControl
    uint64_t a = 0, b = 0;
    inv_field(payload, plen, 0, 0, NULL, NULL, &a);            // hue / sat / X / mireds
    inv_field(payload, plen, 1, 0, NULL, NULL, &b);            // sat (for HueAndSat) / Y
    if      (cmd == 0x00) mtrc_dm_set(ep, 0x0300, 0x0000, a & 0xFF);                 // MoveToHue
    else if (cmd == 0x03) mtrc_dm_set(ep, 0x0300, 0x0001, a & 0xFF);                 // MoveToSaturation
    else if (cmd == 0x06) { mtrc_dm_set(ep, 0x0300, 0x0000, a & 0xFF);              // MoveToHueAndSaturation
                            mtrc_dm_set(ep, 0x0300, 0x0001, b & 0xFF); }
    else if (cmd == 0x07) { mtrc_dm_set(ep, 0x0300, 0x0003, a);                     // MoveToColor (CurrentX/Y)
                            mtrc_dm_set(ep, 0x0300, 0x0004, b); }
    else if (cmd == 0x0A) mtrc_dm_set(ep, 0x0300, 0x0007, a);                        // MoveToColorTemperature
    if (g.port.on_command) g.port.on_command(g.port.ctx, ep, cl, cmd, (int32_t)(a & 0xFFFF));
    n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);
  } else if (cl == 0x003C) {          // AdministratorCommissioning (multi-admin handoff)
    // A second administrator (e.g. a Google Nest hub finishing its "add to Google
    // Home") closes the standard window and re-opens an ENHANCED one carrying its
    // own SPAKE2+ verifier, so it can PASE-pair without the on-device passcode.
    // Hans's C3 aborted here: we advertised cmds 0/1/2 but had no handler, so both
    // returned UNSUPPORTED_COMMAND and the handoff failed.
    if (cmd == 0x00) {                // OpenCommissioningWindow (external verifier)
      uint64_t timeout = 0, disc = 0, iters = 0;
      const uint8_t *verif = NULL, *salt = NULL; size_t verlen = 0, saltlen = 0;
      inv_field(payload, plen, 0, 0, NULL, NULL, &timeout);   // CommissioningTimeout (s)
      inv_field(payload, plen, 1, 1, &verif, &verlen, NULL);  // PAKEPasscodeVerifier = W0(32)||L(65)
      inv_field(payload, plen, 2, 0, NULL, NULL, &disc);      // Discriminator (12-bit)
      inv_field(payload, plen, 3, 0, NULL, NULL, &iters);     // PBKDF iterations
      inv_field(payload, plen, 4, 1, &salt, &saltlen, NULL);  // PBKDF salt
      if (verif && verlen == 97 && salt && saltlen >= 16 && saltlen <= 32 &&
          iters >= 1000 && iters <= 100000) {
        memcpy(g.ocw_w0, verif, 32); memcpy(g.ocw_L, verif + 32, 65);
        memcpy(g.ocw_salt, salt, saltlen); g.ocw_salt_len = (uint8_t)saltlen;
        g.ocw_iterations = (uint32_t)iters;
        g.ocw_disc = (uint16_t)(disc & 0x0FFF);
        g.ocw_active = true;
        g.ocw_expiry_ms = g.port.millis(g.port.ctx) +
                          (uint32_t)(timeout ? timeout : 180) * 1000u;
        mtrc_publish_commissionable(g.ocw_disc, 2);            // CM=2 (enhanced window)
        n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);  // SUCCESS
        mlog(MATTER_LOG_INFO, "AdminComm: OpenCommissioningWindow (enhanced, external verifier)");
      } else {
        g.ocw_active = false;
        n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x87);  // CONSTRAINT_ERROR
        mlog(MATTER_LOG_ERROR, "AdminComm: OpenCommissioningWindow rejected (bad params)");
      }
    } else if (cmd == 0x01) {         // OpenBasicCommissioningWindow (device passcode)
      uint64_t timeout = 0;
      inv_field(payload, plen, 0, 0, NULL, NULL, &timeout);
      matter_open_commissioning_window();                      // standard CM=1 window (clears ocw_active)
      g.ocw_expiry_ms = g.port.millis(g.port.ctx) +
                        (uint32_t)(timeout ? timeout : 180) * 1000u;
      n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);
      mlog(MATTER_LOG_INFO, "AdminComm: OpenBasicCommissioningWindow");
    } else if (cmd == 0x02) {         // RevokeCommissioning (close the window)
      matter_set_commissionable(0);                            // stop PASE; clears ocw_active/expiry
      n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);
      mlog(MATTER_LOG_INFO, "AdminComm: RevokeCommissioning (window closed)");
    }
  } else if (g.port.on_command) {     // any other app cluster -> let a script try
    if (g.port.on_command(g.port.ctx, ep, cl, cmd, (int32_t)cmd))
      n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x00);  // SUCCESS
  }
  if (n < 0)
    n = mtrc_im_build_status(resp, sizeof(resp), ep, cl, cmd, 0x81); // UNSUPPORTED_COMMAND

  // OnOff/Level/Color changed the data model -> let live subscriptions push it.
  if (cl == 0x0006 || cl == 0x0008 || cl == 0x0300) g.app_gen++;

  if (n > 0) {
    secured_send(MTRC_IM_INVOKE_RESPONSE, MTRC_PROTO_IM, resp, (size_t)n, exch, true, ack, true);
    mlog(MATTER_LOG_DEBUG, "IM InvokeResponse sent");
  }
}

// Handle a decrypted IM ReadRequest: report the requested attribute. Descriptor
// (0x001D) lists come from the registry; OnOff (0x0006/0x0000 -> relay state)
// and Basic Information are scalars; everything else reports 0.
// Is (ep,cl,attr) an attribute we can serve a value for? Mirrors the coverage
// of emit_attr_value_field below.
static int attr_known(uint16_t ep, uint32_t cl, uint32_t attr) {
  if (cl == 0x0030 && (attr <= 0x0005 || attr == 0xFFFC || attr == 0xFFFD)) return 1; // GeneralCommissioning
  if (cl == 0x0028 && (attr == 0x0000 || attr == 0x0002 || attr == 0x0004 ||
                       attr == 0xFFFC || attr == 0xFFFD)) return 1;                   // Basic Information (VID=2,PID=4)
  if (mtrc_dm_find(ep, cl, attr)) return 1;                                          // registry (OnOff, etc.)
  return 0;
}

// Write the Data field (context tag 2) for a known attribute, with the correct
// TLV type (controllers reject e.g. a bool returned as uint).
static void emit_attr_value_field(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl, uint32_t attr) {
  if (cl == 0x0030) {                                  // General Commissioning
    switch (attr) {
      case 0x0000: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), g.breadcrumb); return;     // Breadcrumb (u64)
      case 0x0001:                                                                  // BasicCommissioningInfo
        mtrc_tlv_start_struct(w, mtrc_tlv_ctx(2));
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 60);     // FailSafeExpiryLengthSeconds
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(1), 900);    // MaxCumulativeFailsafeSeconds
        mtrc_tlv_end_container(w); return;
      case 0x0002: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 0);    return;             // RegulatoryConfig (enum8)
      case 0x0003: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 0);    return;             // LocationCapability (enum8)
      case 0x0004: mtrc_tlv_put_bool(w, mtrc_tlv_ctx(2), true); return;             // SupportsConcurrentConnection
      case 0x0005: mtrc_tlv_put_bool(w, mtrc_tlv_ctx(2), false);return;             // IsCommissioningWithoutPower
      case 0xFFFC: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 0);    return;             // FeatureMap
      case 0xFFFD: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 2);    return;             // ClusterRevision
    }
  }
  if (cl == 0x0028) {                                  // Basic Information
    switch (attr) {                                    // NB: VendorID=0x0002, ProductID=0x0004
      case 0x0000: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 18);               return; // DataModelRevision
      case 0x0002: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), g.cfg.vendor_id);  return; // VendorID
      case 0x0004: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), g.cfg.product_id); return; // ProductID
      case 0xFFFC: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 0);                return; // FeatureMap
      case 0xFFFD: mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), 3);                return; // ClusterRevision
    }
  }
  // ElectricalEnergyMeasurement (0x0091): the Cumulative/Periodic Energy
  // attributes (0x0001..0x0004) are an EnergyMeasurementStruct, not a scalar.
  // Emit { 0: Energy(int64, mWh) } from the stored value (optional timestamps
  // omitted). The script stores mWh, e.g. matterSetFloat(ep,CLUSTER_ENERGY,
  // attr, kWh, 1000000).
  if (cl == 0x0091 && attr >= 0x0001 && attr <= 0x0004) {
    mtrc_tlv_start_struct(w, mtrc_tlv_ctx(2));                        // EnergyMeasurementStruct
    mtrc_tlv_put_int(w, mtrc_tlv_ctx(0), (int64_t)attr_value(ep, cl, attr));  // Energy (mWh)
    mtrc_tlv_end_container(w);
    return;
  }
  // Registry attribute: emit with the declared type. Signed sensor values
  // (TemperatureMeasurement / PressureMeasurement MeasuredValue are int16 and
  // may be negative) MUST be a TLV signed int, or controllers read a huge
  // positive number for sub-zero readings.
  uint64_t v = attr_value(ep, cl, attr);
  mtrc_dm_attr_t *a = mtrc_dm_find(ep, cl, attr);
  if (a && a->type == MTRC_DM_T_S16) {
    mtrc_tlv_put_int(w, mtrc_tlv_ctx(2), (int64_t)(int16_t)(uint16_t)v); return;
  }
  if (a && a->type == MTRC_DM_T_S32) {
    mtrc_tlv_put_int(w, mtrc_tlv_ctx(2), (int64_t)(int32_t)(uint32_t)v); return;
  }
  if (a && a->type == MTRC_DM_T_S64) {
    mtrc_tlv_put_int(w, mtrc_tlv_ctx(2), (int64_t)v); return;                       // ElectricalPower int64
  }
  if (a && a->type == MTRC_DM_T_FLOAT) {
    uint32_t bits = (uint32_t)v; float f; memcpy(&f, &bits, 4);                     // ConcentrationMeasurement single
    mtrc_tlv_put_float(w, mtrc_tlv_ctx(2), f); return;
  }
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), v);                                         // registry/uint
}

// One AttributeReportIB carrying AttributeData (DataVersion + path + value).
static void emit_attr_report(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl, uint32_t attr) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());           // AttributeReportIB
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));           //  AttributeDataIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);            //   DataVersion
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));             //   AttributePathIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cl);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attr);
  mtrc_tlv_end_container(w);                           //   end path
  emit_attr_value_field(w, ep, cl, attr);             //   Data
  mtrc_tlv_end_container(w);                           //  end AttributeDataIB
  mtrc_tlv_end_container(w);                           // end AttributeReportIB
}

// ---- wildcard attribute enumeration (Apple/Google subscribe to '*') --------
// Emit one AttributeReportIB fragment whose Data is an array of uints (Descriptor
// ServerList/ClientList/PartsList, AttributeList, etc.) into a shared writer.
static void emit_report_list(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl,
                             uint32_t attr, const uint32_t *vals, int count) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cl);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attr);
  mtrc_tlv_end_container(w);
  mtrc_tlv_start_array(w, mtrc_tlv_ctx(2));
  for (int i = 0; i < count; i++) mtrc_tlv_put_uint(w, mtrc_tlv_anon(), vals ? vals[i] : 0);
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
}

// Emit the Descriptor (0x001D) DeviceTypeList fragment (array of {0:type,1:rev}).
static void emit_report_devtypelist(mtrc_tlv_writer *w, uint16_t ep, uint32_t dt) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), 0x001D);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), 0x0000);
  mtrc_tlv_end_container(w);
  mtrc_tlv_start_array(w, mtrc_tlv_ctx(2));
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), dt);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(1), 1);
  mtrc_tlv_end_container(w);
  if (ep_is_bridged(ep) && !fabric_is_non_bridge()) {   // Bridged Node 0x0013 — NOT for Alexa/Amazon
    mtrc_tlv_start_struct(w, mtrc_tlv_anon());
    mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 0x0013);
    mtrc_tlv_put_uint(w, mtrc_tlv_ctx(1), 1);
    mtrc_tlv_end_container(w);
  }
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
}

// Emit one AttributeReportIB fragment carrying a scalar uint Data value
// (FeatureMap, ClusterRevision, and other scalar globals).
static void emit_attr_report_uint(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl,
                                  uint32_t attr, uint64_t val) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cl);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attr);
  mtrc_tlv_end_container(w);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), val);
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
}

// Emit a single Descriptor (0x001D) list attribute fragment.
static void emit_descriptor_one(mtrc_tlv_writer *w, uint16_t ep, uint32_t attr) {
  if (attr == 0x0000) { uint32_t dt = 0; mtrc_dm_endpoint_device_type(ep, &dt);
    emit_report_devtypelist(w, ep, dt); return; }
  if (attr == 0x0001) {                                  // ServerList
    uint32_t list[MTRC_DM_MAX_CLUSTERS]; int c = 0;
    for (int i = 0; i < mtrc_dm_cluster_count(); i++) {
      const mtrc_dm_cluster_t *cc = mtrc_dm_cluster_at(i);
      if (cc && cc->endpoint == ep && c < (int)(sizeof(list)/sizeof(list[0]))) list[c++] = cc->cluster;
    }
    emit_report_list(w, ep, 0x001D, 0x0001, list, c); return;
  }
  if (attr == 0x0002) { emit_report_list(w, ep, 0x001D, 0x0002, NULL, 0); return; }   // ClientList
  if (attr == 0x0003) {                                  // PartsList
    uint32_t list[MTRC_DM_MAX_ENDPOINTS]; int c = 0;
    // Full-family pattern: the root (ep0) lists EVERY endpoint on the node
    // (bridged endpoints + the Aggregator). The Aggregator additionally lists
    // its bridged children (every app endpoint except itself). A controller
    // (Apple) needs the root to enumerate all endpoints — listing only the
    // Aggregator under the root makes the bridged devices disappear entirely.
    int is_agg = (g.aggregator_ep != 0 && ep == g.aggregator_ep);
    if (ep == 0 || is_agg) for (int i = 0; i < mtrc_dm_endpoint_count(); i++) {
      const mtrc_dm_endpoint_t *e = mtrc_dm_endpoint_at(i);
      if (!e || e->endpoint == 0) continue;
      if (is_agg && e->endpoint == g.aggregator_ep) continue;  // aggregator excludes itself
      if (c < (int)(sizeof(list)/sizeof(list[0]))) list[c++] = e->endpoint;
    }
    emit_report_list(w, ep, 0x001D, 0x0003, list, c); return;
  }
}

// Functional (non-global) attribute ids a cluster exposes. Synthetic clusters
// (Descriptor / GeneralCommissioning / BasicInformation) have fixed lists;
// everything else comes from the data-model registry.
static int cluster_func_attrs(uint16_t ep, uint32_t cl, uint32_t *out, int cap) {
  int n = 0;
  #define CFA_LIST(...) do { static const uint32_t a[]={__VA_ARGS__}; \
    for (unsigned i=0;i<sizeof(a)/sizeof(a[0]) && n<cap;i++) out[n++]=a[i]; return n; } while(0)
  if (cl == 0x001D) CFA_LIST(0,1,2,3);                                  // Descriptor
  if (cl == 0x0039) CFA_LIST(0x0003,0x0005,0x000A,0x000F,0x0011,0x0012);// Bridged Device Basic Information
  if (ep == 0) switch (cl) {                                            // root-node clusters
    case 0x0028: CFA_LIST(0,1,2,3,4,5,6,7,8,9,0x0A,0x0F,0x11,0x12,0x13,0x15,0x16); // Basic Information
    case 0x0030: CFA_LIST(0,1,2,3,4);                                   // General Commissioning
    case 0x0031: CFA_LIST(3,4);                                         // Network Commissioning
    case 0x0033: CFA_LIST(0,1,2,8);                                     // General Diagnostics
    case 0x003C: CFA_LIST(0,1,2);                                       // Administrator Commissioning
    case 0x003E: CFA_LIST(0,1,2,3,4,5);                                 // Operational Credentials
    case 0x003F: CFA_LIST(0,1,2,3);                                     // Group Key Management
    case 0x001F: CFA_LIST(0,2,3,4);                                     // Access Control
    default: break;
  }
  #undef CFA_LIST
  for (int i=0;i<mtrc_dm_attr_count();i++) {                            // registry (app clusters)
    const mtrc_dm_attr_t *a = mtrc_dm_attr_at(i);
    if (a && a->endpoint==ep && a->cluster==cl && n<cap) out[n++]=a->attr;
  }
  return n;
}

// AttributeReportIB header up to (not incl.) the Data field at context tag 2.
static void frag_open(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl, uint32_t attr) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());            // AttributeReportIB
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));            //  AttributeDataIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 1);            //   DataVersion
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(1));             //   AttributePathIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cl);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attr);
  mtrc_tlv_end_container(w);                            //   end path
}
static void frag_close(mtrc_tlv_writer *w) {
  mtrc_tlv_end_container(w);                            //  end AttributeDataIB
  mtrc_tlv_end_container(w);                            // end AttributeReportIB
}
static void emit_attr_report_str(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl,
                                 uint32_t attr, const char *s) {
  frag_open(w, ep, cl, attr);
  mtrc_tlv_put_utf8(w, mtrc_tlv_ctx(2), (const uint8_t *)s, s ? strlen(s) : 0);
  frag_close(w);
}
static void emit_attr_report_bool(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl,
                                  uint32_t attr, bool b) {
  frag_open(w, ep, cl, attr);
  mtrc_tlv_put_bool(w, mtrc_tlv_ctx(2), b);
  frag_close(w);
}

// Root-node (endpoint 0) cluster attribute reads that controllers (Apple Home /
// Home Assistant) require to validate the node: Basic Information, General
// Commissioning, Network Commissioning, General Diagnostics, Administrator
// Commissioning, Operational Credentials (fabric-scoped lists from mtrc_store),
// Group Key Management and Access Control. Modeled on Berry Matter_Plugin_1_Root.
static void emit_root_attr(mtrc_tlv_writer *w, uint32_t cl, uint32_t attr) {
  const uint16_t ep = 0;
  static const char *UNIQUE_ID = "TASMOTA-MATTER-C6-0001";
  if (cl == 0x0028) {                                   // Basic Information
    switch (attr) {
      case 0x0000: emit_attr_report_uint(w,ep,cl,attr,18); return;            // DataModelRevision
      case 0x0001: emit_attr_report_str (w,ep,cl,attr,"Tasmota"); return;     // VendorName
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,g.cfg.vendor_id); return;
      case 0x0003: emit_attr_report_str (w,ep,cl,attr,g.cfg.device_name?g.cfg.device_name:"ESP32-C6"); return; // ProductName
      case 0x0004: emit_attr_report_uint(w,ep,cl,attr,g.cfg.product_id); return;
      case 0x0005: emit_attr_report_str (w,ep,cl,attr,g.cfg.device_name?g.cfg.device_name:"ESP32-C6"); return; // NodeLabel
      case 0x0006: emit_attr_report_str (w,ep,cl,attr,"XX"); return;          // Location
      case 0x0007: emit_attr_report_uint(w,ep,cl,attr,0); return;             // HardwareVersion
      case 0x0008: emit_attr_report_str (w,ep,cl,attr,"ESP32-C6"); return;    // HardwareVersionString
      case 0x0009: emit_attr_report_uint(w,ep,cl,attr,1); return;             // SoftwareVersion
      case 0x000A: emit_attr_report_str (w,ep,cl,attr,"1.0"); return;         // SoftwareVersionString
      case 0x000F: emit_attr_report_str (w,ep,cl,attr,UNIQUE_ID); return;     // SerialNumber
      case 0x0011: emit_attr_report_bool(w,ep,cl,attr,true); return;          // Reachable
      case 0x0012: emit_attr_report_str (w,ep,cl,attr,UNIQUE_ID); return;     // UniqueID
      case 0x0013: frag_open(w,ep,cl,attr);                                   // CapabilityMinima
        mtrc_tlv_start_struct(w, mtrc_tlv_ctx(2));
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 3);        // CaseSessionsPerFabric
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(1), 3);        // SubscriptionsPerFabric
        mtrc_tlv_end_container(w); frag_close(w); return;
      case 0x0015: emit_attr_report_uint(w,ep,cl,attr,0x01040100); return;    // SpecificationVersion
      case 0x0016: emit_attr_report_uint(w,ep,cl,attr,1); return;             // MaxPathsPerInvoke
    }
    return;
  }
  if (cl == 0x0030) {                                   // General Commissioning
    switch (attr) {
      case 0x0000: emit_attr_report_uint(w,ep,cl,attr,g.breadcrumb); return;  // Breadcrumb
      case 0x0001: frag_open(w,ep,cl,attr);                                   // BasicCommissioningInfo
        mtrc_tlv_start_struct(w, mtrc_tlv_ctx(2));
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), 60);
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(1), 900);
        mtrc_tlv_end_container(w); frag_close(w); return;
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,0); return;             // RegulatoryConfig
      case 0x0003: emit_attr_report_uint(w,ep,cl,attr,0); return;             // LocationCapability
      case 0x0004: emit_attr_report_bool(w,ep,cl,attr,true); return;          // SupportsConcurrentConnection
    }
    return;
  }
  if (cl == 0x0031) {                                   // Network Commissioning
    switch (attr) {
      case 0x0003: emit_attr_report_uint(w,ep,cl,attr,30); return;            // ConnectMaxTimeSeconds
      case 0x0004: emit_attr_report_bool(w,ep,cl,attr,true); return;          // InterfaceEnabled
    }
    return;
  }
  if (cl == 0x0033) {                                   // General Diagnostics
    switch (attr) {
      case 0x0000: frag_open(w,ep,cl,attr);                                   // NetworkInterfaces (empty)
        mtrc_tlv_start_array(w, mtrc_tlv_ctx(2)); mtrc_tlv_end_container(w);
        frag_close(w); return;
      case 0x0001: emit_attr_report_uint(w,ep,cl,attr,1); return;             // RebootCount
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,
                     g.port.millis ? g.port.millis(g.port.ctx)/1000 : 0); return; // UpTime
      case 0x0008: emit_attr_report_bool(w,ep,cl,attr,false); return;         // TestEventTriggersEnabled
    }
    return;
  }
  if (cl == 0x003C) {                                   // Administrator Commissioning
    switch (attr) {
      case 0x0000: emit_attr_report_uint(w,ep,cl,attr, g.commissionable?1:0); return; // WindowStatus
      case 0x0001: frag_open(w,ep,cl,attr); mtrc_tlv_put_null(w,mtrc_tlv_ctx(2)); frag_close(w); return; // AdminFabricIndex
      case 0x0002: frag_open(w,ep,cl,attr); mtrc_tlv_put_null(w,mtrc_tlv_ctx(2)); frag_close(w); return; // AdminVendorId
    }
    return;
  }
  if (cl == 0x003F) {                                   // Group Key Management
    switch (attr) {
      case 0x0000: frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w,mtrc_tlv_ctx(2)); mtrc_tlv_end_container(w); frag_close(w); return; // GroupKeyMap
      case 0x0001: frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w,mtrc_tlv_ctx(2)); mtrc_tlv_end_container(w); frag_close(w); return; // GroupTable
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,1); return;             // MaxGroupsPerFabric
      case 0x0003: emit_attr_report_uint(w,ep,cl,attr,1); return;             // MaxGroupKeysPerFabric
    }
    return;
  }
  if (cl == 0x001F) {                                   // Access Control
    switch (attr) {
      case 0x0000: frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w,mtrc_tlv_ctx(2)); mtrc_tlv_end_container(w); frag_close(w); return; // ACL (managed internally)
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,4); return;             // SubjectsPerAccessControlEntry
      case 0x0003: emit_attr_report_uint(w,ep,cl,attr,3); return;             // TargetsPerAccessControlEntry
      case 0x0004: emit_attr_report_uint(w,ep,cl,attr,4); return;             // AccessControlEntriesPerFabric
    }
    return;
  }
  if (cl == 0x003E) {                                   // Operational Credentials
    if (attr == 0x0000) {                               // NOCs / list[NOCStruct]
      frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w, mtrc_tlv_ctx(2));
      for (int i=0;i<mtrc_store_count();i++){ mtrc_fabric *f=mtrc_store_at(i); if(!f)continue;
        mtrc_tlv_start_struct(w, mtrc_tlv_anon());
        mtrc_tlv_put_bytes(w, mtrc_tlv_ctx(1), f->noc, f->noc_len);
        if (f->icac_len) mtrc_tlv_put_bytes(w, mtrc_tlv_ctx(2), f->icac, f->icac_len);
        else             mtrc_tlv_put_null (w, mtrc_tlv_ctx(2));
        mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0xFE), f->fabric_index);
        mtrc_tlv_end_container(w);
      }
      mtrc_tlv_end_container(w); frag_close(w); return;
    }
    if (attr == 0x0001) {                               // Fabrics / list[FabricDescriptorStruct]
      frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w, mtrc_tlv_ctx(2));
      for (int i=0;i<mtrc_store_count();i++){ mtrc_fabric *f=mtrc_store_at(i); if(!f)continue;
        mtrc_tlv_start_struct(w, mtrc_tlv_anon());
        mtrc_tlv_put_bytes(w, mtrc_tlv_ctx(1), f->root_pub, 65);        // RootPublicKey
        mtrc_tlv_put_uint (w, mtrc_tlv_ctx(2), f->admin_vendor_id);     // VendorID
        mtrc_tlv_put_uint (w, mtrc_tlv_ctx(3), f->fabric_id);           // FabricID
        mtrc_tlv_put_uint (w, mtrc_tlv_ctx(4), f->node_id);             // NodeID
        mtrc_tlv_put_utf8 (w, mtrc_tlv_ctx(5), (const uint8_t *)f->label, strlen(f->label)); // Label (UpdateFabricLabel)
        mtrc_tlv_put_uint (w, mtrc_tlv_ctx(0xFE), f->fabric_index);     // FabricIndex
        mtrc_tlv_end_container(w);
      }
      mtrc_tlv_end_container(w); frag_close(w); return;
    }
    switch (attr) {
      case 0x0002: emit_attr_report_uint(w,ep,cl,attr,5); return;             // SupportedFabrics
      case 0x0003: emit_attr_report_uint(w,ep,cl,attr,mtrc_store_count()); return; // CommissionedFabrics
      case 0x0004: frag_open(w,ep,cl,attr); mtrc_tlv_start_array(w,mtrc_tlv_ctx(2)); mtrc_tlv_end_container(w); frag_close(w); return; // TrustedRootCertificates
      case 0x0005: { mtrc_fabric *cf = mtrc_store_by_index(g.case_fabric_index);
        emit_attr_report_uint(w,ep,cl,attr, cf ? cf->fabric_index : 0); return; } // CurrentFabricIndex
    }
    return;
  }
}

// True for root-node (ep0) clusters served by emit_root_attr.
static int is_root_cluster(uint32_t cl) {
  return cl==0x0028 || cl==0x0030 || cl==0x0031 || cl==0x0033 ||
         cl==0x003C || cl==0x003E || cl==0x003F || cl==0x001F;
}

// Bridged Device Basic Information (0x0039) on a named (bridged) endpoint. The
// per-endpoint name a controller (Apple Home) displays as the accessory title
// comes from NodeLabel (0x0005); the rest are mandatory metadata.
static void emit_bridged_basic(mtrc_tlv_writer *w, uint16_t ep, uint32_t attr) {
  const char *label = dm_label_for(ep);
  char uid[24]; snprintf(uid, sizeof uid, "TASMOTA-MTRC-EP%u", (unsigned)ep);
  switch (attr) {
    case 0x0003: emit_attr_report_str (w,ep,0x0039,attr, g.cfg.device_name?g.cfg.device_name:"Tasmota"); return; // ProductName
    case 0x0005: emit_attr_report_str (w,ep,0x0039,attr, label?label:""); return;  // NodeLabel (the name)
    case 0x000A: emit_attr_report_str (w,ep,0x0039,attr, "1.0"); return;           // SoftwareVersionString
    case 0x000F: emit_attr_report_str (w,ep,0x0039,attr, uid);  return;            // SerialNumber
    case 0x0011: emit_attr_report_bool(w,ep,0x0039,attr, true); return;            // Reachable
    case 0x0012: emit_attr_report_str (w,ep,0x0039,attr, uid);  return;            // UniqueID
  }
}

// Emit one AttributeReportIB fragment for any (ep,cl,attr), including the
// mandatory global attributes every cluster must expose (Core Spec §7.13).
static void emit_one_path(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl, uint32_t attr) {
  if (cl == 0x001D && attr <= 0x0003) { emit_descriptor_one(w, ep, attr); return; }
  switch (attr) {
    case 0xFFFD: emit_attr_report_uint(w, ep, cl, attr,
                   cl == 0x0039 ? 3 :
                   cl == 0x0004 ? 4 :    // Groups
                   cl == 0x002F ? 2 :    // Power Source (cluster revision 2)
                   cl == 0x0006 ? 6 :    // OnOff (Matter 1.4 cluster revision 6)
                   1); return;                                                  // ClusterRevision
    case 0xFFFC: {                                                              // FeatureMap
      uint32_t fm = 0;
      if (cl == 0x0006) {
        // OnOff: claim Lighting (LT, bit 0) ONLY for Light device types per spec.
        // OnOff Plug-in Unit (0x010A) keeps FeatureMap=0 (no LT). The LT-required
        // attrs 0x4000..0x4003 are added in dm_attach_device_type for matching EPs.
        uint32_t dt = 0;
        if (mtrc_dm_endpoint_device_type(ep, &dt) &&
            (dt == MATTER_DEVTYPE_ON_OFF_LIGHT || dt == MATTER_DEVTYPE_DIMMABLE_LIGHT
             || dt == 0x010C || dt == 0x010D)) fm = 0x01;
      } else {
        fm = cl == 0x0008 ? 0x01 :    // LevelControl: OnOff feature (WithOnOff cmds)
             cl == 0x0300 ? 0x01 :    // ColorControl: HueSaturation
             cl == 0x0102 ? 0x05 :    // WindowCovering: Lift + PositionAwareLift
             cl == 0x003B ? 0x2E :    // Switch: MomentarySwitch + Release + LongPress + MultiPress
             cl == 0x0091 ? 0x07 :    // ElectricalEnergyMeasurement: Imported + Exported + Cumulative
             cl == 0x002F ? 0x02 :    // PowerSource: BAT (battery) feature
             (cl >= 0x040C && cl <= 0x042F) ? 0x01 :  // ConcentrationMeasurement: NumericMeasurement
             0;
      }
      emit_attr_report_uint(w, ep, cl, attr, fm); return;
    }

    case 0xFFFB: {                                                              // AttributeList
      uint32_t fa[80]; int fn = cluster_func_attrs(ep, cl, fa, 74);
      static const uint32_t glob[]={0xFFF8,0xFFF9,0xFFFA,0xFFFB,0xFFFC,0xFFFD};
      for (unsigned i=0;i<6 && fn<80;i++) fa[fn++]=glob[i];
      emit_report_list(w, ep, cl, attr, fa, fn); return;
    }
    case 0xFFFA:                                                                // EventList
      if (cl == 0x003B) { static const uint32_t sev[]={1,2,3,4,5,6};            // Switch events
        emit_report_list(w, ep, cl, attr, sev, 6); return; }
      emit_report_list(w, ep, cl, attr, NULL, 0); return;
    case 0xFFF8:                                                               // GeneratedCommandList
      if (cl == 0x003E) {                                  // OperationalCredentials responses
        static const uint32_t g_noc[] = {0x01,0x03,0x05,0x08};                 // Attestation/CertChain/CSR/NOCResponse
        emit_report_list(w, ep, cl, attr, g_noc, 4); return;
      }
      if (cl == 0x0004) {                                  // Groups responses
        static const uint32_t g_grp[] = {0x00,0x01,0x02,0x03};                 // Add/View/GetMembership/Remove Response
        emit_report_list(w, ep, cl, attr, g_grp, 4); return;
      }
      emit_report_list(w, ep, cl, attr, NULL, 0); return;   // our app clusters generate none
    case 0xFFF9: {                                                             // AcceptedCommandList
      // Controllers validate controllability via this list. An endpoint whose
      // cluster accepts NO commands is treated as non-functional: an On/Off
      // Plug-in Unit with an empty OnOff list never appears in Apple Home, and a
      // light whose OnOff list is empty shows as dimmer-only (no on/off button).
      static const uint32_t c_ident[] = {0x00,0x40};                            // Identify, TriggerEffect
      static const uint32_t c_groups[]= {0x00,0x01,0x02,0x03,0x04,0x05};        // Add/View/GetMembership/Remove/RemoveAll/AddIfIdentifying
      static const uint32_t c_onoff[] = {0x00,0x01,0x02};                       // Off, On, Toggle
      static const uint32_t c_level[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07};
      static const uint32_t c_color[] = {0x00,0x03,0x06,0x07,0x0A};             // Hue/Sat/HueSat/Color/CT
      static const uint32_t c_wcov[]  = {0x00,0x01,0x02,0x05};                  // Up/Down/Stop/GoToLift%
      // OperationalCredentials commands we accept (responses are GeneratedCommandList,
      // not here). Advertising UpdateFabricLabel(0x09) signals Alexa we honour it.
      static const uint32_t c_noc[]   = {0x00,0x02,0x04,0x06,0x09,0x0A,0x0B};
      const uint32_t *l = NULL; int ln = 0;
      switch (cl) {
        case 0x0003: l=c_ident; ln=2; break;
        case 0x0004: l=c_groups;ln=6; break;
        case 0x0006: l=c_onoff; ln=3; break;
        case 0x0008: l=c_level; ln=8; break;
        case 0x0102: l=c_wcov;  ln=4; break;
        case 0x0300: l=c_color; ln=5; break;
        case 0x003E: l=c_noc;   ln=7; break;
        default: break;
      }
      emit_report_list(w, ep, cl, attr, l, ln); return;
    }
    default: break;
  }
  if (cl == 0x0039) { emit_bridged_basic(w, ep, attr); return; }              // Bridged Device Basic Information (name)
  if (ep == 0 && is_root_cluster(cl)) { emit_root_attr(w, cl, attr); return; } // root-node clusters
  emit_attr_report(w, ep, cl, attr);                                           // app-cluster functional scalar
}

// Append every (ep,cl,attr) path matching a (possibly wildcard) filter to
// g.rpt_paths, including each cluster's mandatory global attributes. Multiple
// query paths accumulate (a ReadRequest may carry several AttributePathIBs).
static void rpt_add_query(int has_ep, uint16_t want_ep, int has_cl, uint32_t want_cl,
                          int has_attr, uint32_t want_attr) {
  static const uint32_t globals[]={0xFFF8,0xFFF9,0xFFFA,0xFFFB,0xFFFC,0xFFFD};
  int cap = (int)(sizeof(g.rpt_paths)/sizeof(g.rpt_paths[0]));
  #define RPT_PUT(EP,CL,AT) do { uint32_t _a=(AT); \
    if ((!has_attr || _a==want_attr) && g.rpt_npaths<cap) { \
      g.rpt_paths[g.rpt_npaths].ep=(EP); g.rpt_paths[g.rpt_npaths].cl=(uint32_t)(CL); \
      g.rpt_paths[g.rpt_npaths].attr=_a; g.rpt_npaths++; } } while(0)
  #define RPT_CLUSTER(EP,CL) do { uint32_t _fa[80]; \
    int _fn=cluster_func_attrs((EP),(CL),_fa,80); \
    for (int _i=0;_i<_fn;_i++) RPT_PUT((EP),(CL),_fa[_i]); \
    for (unsigned _g=0;_g<sizeof(globals)/sizeof(globals[0]);_g++) RPT_PUT((EP),(CL),globals[_g]); } while(0)
  for (int ei=0; ei<mtrc_dm_endpoint_count(); ei++) {
    const mtrc_dm_endpoint_t *e = mtrc_dm_endpoint_at(ei);
    if (!e) continue;
    uint16_t ep = e->endpoint;
    if (has_ep && ep != want_ep) continue;
    for (int ci=0; ci<mtrc_dm_cluster_count(); ci++) {     // every registered cluster on ep
      const mtrc_dm_cluster_t *cc = mtrc_dm_cluster_at(ci);
      if (!cc || cc->endpoint != ep) continue;
      if (has_cl && cc->cluster != want_cl) continue;
      RPT_CLUSTER(ep, cc->cluster);
    }
  }
  #undef RPT_CLUSTER
  #undef RPT_PUT
}

// Does endpoint `ep` exist in the data model? (root ep0 always does.)
static int dm_ep_present(uint16_t ep) {
  if (ep == 0) return 1;
  uint32_t dt; return mtrc_dm_endpoint_device_type(ep, &dt);
}
// Does cluster `cl` exist on endpoint `ep`? Descriptor is synthetic on every
// endpoint; root clusters synthetic on ep0; 0x0039 on named/bridged eps; the
// rest come from the registry.
static int dm_cl_present(uint16_t ep, uint32_t cl) {
  if (cl == MTRC_CL_DESCRIPTOR) return 1;
  if (ep == 0 && is_root_cluster(cl)) return 1;
  if (cl == 0x0039 && ep_is_bridged(ep)) return 1;
  for (int i = 0; i < mtrc_dm_cluster_count(); i++) {
    const mtrc_dm_cluster_t *c = mtrc_dm_cluster_at(i);
    if (c && c->endpoint == ep && c->cluster == cl) return 1;
  }
  return 0;
}
// Matter IM status for an unmatched concrete read path (Core Spec §8.4.3.2).
static uint8_t read_status_for(uint16_t ep, uint32_t cl, uint32_t attr) {
  (void)attr;
  if (!dm_ep_present(ep))     return 0x7F;   // UNSUPPORTED_ENDPOINT
  if (!dm_cl_present(ep, cl)) return 0xC3;   // UNSUPPORTED_CLUSTER
  return 0x86;                               // UNSUPPORTED_ATTRIBUTE
}
// Emit an AttributeReportIB carrying a StatusIB (the AttributeStatusIB variant,
// tag ctx(0)) for a path the node can't serve — vs the data variant ctx(1).
static void emit_status_path(mtrc_tlv_writer *w, uint16_t ep, uint32_t cl,
                             uint32_t attr, uint8_t st) {
  mtrc_tlv_start_struct(w, mtrc_tlv_anon());            // AttributeReportIB
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(0));            // AttributeStatusIB
  mtrc_tlv_start_list(w, mtrc_tlv_ctx(0));              // AttributePathIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(2), ep);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(3), cl);
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(4), attr);
  mtrc_tlv_end_container(w);
  mtrc_tlv_start_struct(w, mtrc_tlv_ctx(1));            // StatusIB
  mtrc_tlv_put_uint(w, mtrc_tlv_ctx(0), st);            // Status
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
  mtrc_tlv_end_container(w);
}

// Build and send one ReportData chunk from g.rpt_paths[g.rpt_cursor:]. Sets
// MoreChunkedMessages while paths remain; the controller's StatusResponse pulls
// the next chunk. `ack` is the counter of the message that triggered this chunk.
static void send_report_chunk(uint32_t ack) {
  static uint8_t chunk[1280];
  static uint8_t frag[1024];   // a single fragment can be large (OpCreds NOCs = NOC+ICAC certs)
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, chunk, sizeof(chunk));
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());             // ReportDataMessage
  if (g.rpt_is_sub) mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), g.rpt_sub_id);   // SubscriptionId
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));              // AttributeReports
  const size_t MAX_PAYLOAD = 1100;                        // < 1280 IPv6 MTU after headers + MIC
  int emitted = 0;
  while (g.rpt_cursor < g.rpt_npaths) {
    mtrc_tlv_writer fw; mtrc_tlv_writer_init(&fw, frag, sizeof(frag));
    emit_one_path(&fw, g.rpt_paths[g.rpt_cursor].ep, g.rpt_paths[g.rpt_cursor].cl,
                  g.rpt_paths[g.rpt_cursor].attr);
    if (!mtrc_tlv_writer_ok(&fw)) { g.rpt_cursor++; continue; }   // skip a frag that didn't build
    size_t flen = mtrc_tlv_writer_len(&fw);
    if (emitted > 0 && mtrc_tlv_writer_len(&w) + flen > MAX_PAYLOAD) break;   // chunk full
    mtrc_tlv_put_raw(&w, frag, flen); emitted++; g.rpt_cursor++;
  }
  // On the final chunk (all data paths drained), append a StatusIB for each
  // concrete read path that matched nothing. Required by spec; without it the
  // report is empty and a strict controller (Alexa) retries -> RN002.
  if (g.rpt_cursor >= g.rpt_npaths && !g.rpt_is_sub) {
    for (int s = 0; s < g.rpt_nstatus; s++) {
      mtrc_tlv_writer fw; mtrc_tlv_writer_init(&fw, frag, sizeof(frag));
      emit_status_path(&fw, g.rpt_status[s].ep, g.rpt_status[s].cl,
                       g.rpt_status[s].attr, g.rpt_status[s].st);
      if (!mtrc_tlv_writer_ok(&fw)) continue;
      size_t flen = mtrc_tlv_writer_len(&fw);
      if (emitted > 0 && mtrc_tlv_writer_len(&w) + flen > MAX_PAYLOAD) break;
      mtrc_tlv_put_raw(&w, frag, flen); emitted++;
    }
    g.rpt_nstatus = 0;
  }
  mtrc_tlv_end_container(&w);                             // end AttributeReports
  int more = (g.rpt_cursor < g.rpt_npaths);
  if (more) mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(3), true); // MoreChunkedMessages
  else if (!g.rpt_is_sub) mtrc_tlv_put_bool(&w, mtrc_tlv_ctx(4), true);  // read final: SuppressResponse
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);          // InteractionModelRevision
  mtrc_tlv_end_container(&w);
  if (mtrc_tlv_writer_ok(&w))
    secured_send(MTRC_IM_REPORT_DATA, MTRC_PROTO_IM, chunk, mtrc_tlv_writer_len(&w),
                 g.rpt_exch, true, ack, true);
  else mlog(MATTER_LOG_ERROR, "IM report chunk overflow");
  { char m[56]; snprintf(m, sizeof(m), "IM ReportData %d/%d more=%d",
      g.rpt_cursor, g.rpt_npaths, more); mlog(MATTER_LOG_DEBUG, m); }
  if (more)              { /* stay active; next chunk on StatusResponse */ }
  else if (g.rpt_is_sub) { g.rpt_phase = 1; }            // priming done -> SubscribeResponse next
  else                   { g.rpt_active = false; }       // read complete
}

// Multi-path ReadRequest -> chunked ReportData covering every requested
// AttributePathIB plus each cluster's mandatory global attributes. Large
// responses (wildcard reads) are split across ReportData chunks driven by the
// controller's StatusResponse (see send_report_chunk / secured_dispatch).
static void im_handle_read(const uint8_t *payload, size_t plen,
                           uint16_t exch, uint32_t ack) {
  g.rpt_npaths = 0; g.rpt_nstatus = 0;
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, payload, plen);
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(&r, &e)) {
    if (e.type != MTRC_TLV_LIST) continue;             // AttributePathIB is the only list
    int have_ep = 0, have_cl = 0, have_attr = 0;
    uint16_t ep = 0; uint32_t cl = 0, attr = 0;
    mtrc_tlv_elem ie;
    while (mtrc_tlv_read(&r, &ie) && ie.type != MTRC_TLV_END) {
      if (ie.tag.ctrl != MTRC_TLV_TAG_CONTEXT) continue;
      if      (ie.tag.number == 2) { ep   = (uint16_t)ie.u; have_ep = 1; }
      else if (ie.tag.number == 3) { cl   = (uint32_t)ie.u; have_cl = 1; }
      else if (ie.tag.number == 4) { attr = (uint32_t)ie.u; have_attr = 1; }
    }
    int _before = g.rpt_npaths;
    rpt_add_query(have_ep, ep, have_cl, cl, have_attr, attr);
    // A fully-concrete path (ep+cl+attr all specified) that matched nothing must
    // get a StatusIB, not an empty report (Matter Core Spec §8.4.3.2). Queue it.
    if (have_ep && have_cl && have_attr && g.rpt_npaths == _before &&
        g.rpt_nstatus < (int)(sizeof(g.rpt_status)/sizeof(g.rpt_status[0]))) {
      g.rpt_status[g.rpt_nstatus].ep = ep;
      g.rpt_status[g.rpt_nstatus].cl = cl;
      g.rpt_status[g.rpt_nstatus].attr = attr;
      g.rpt_status[g.rpt_nstatus].st = read_status_for(ep, cl, attr);
      g.rpt_nstatus++;
    }
  }
  g.rpt_active = true; g.rpt_is_sub = false; g.rpt_phase = 0;
  g.rpt_exch = exch; g.rpt_cursor = 0;
  send_report_chunk(ack);
}

// Handle a SubscribeRequest: send a chunked priming ReportData enumerating the
// subscribed paths (wildcard -> whole node, incl. global attributes), then —
// after the controller acks the last chunk with a StatusResponse — a
// SubscribeResponse. A concrete hosted attribute is also registered for
// periodic/change reports from matter_loop. Chunk continuation + the final
// SubscribeResponse are driven from secured_dispatch's StatusResponse handler.
static void im_handle_subscribe(const uint8_t *payload, size_t plen,
                                uint16_t exch, uint32_t ack) {
  uint16_t ep = 0, maxc = 0; uint32_t cl = 0, attr = 0;
  int have = mtrc_im_parse_subscribe(payload, plen, &ep, &cl, &attr, &maxc);
  uint16_t max_s = (maxc == 0 || maxc > 60) ? 30 : maxc;   // clamp
  uint32_t sid = (g.sub_id ? g.sub_id : 1) + 1; g.sub_id = sid;
  int hosted = (have && attr_known(ep, cl, attr));

  if (hosted) {
    g.sub_active = true; g.sub_ep = ep; g.sub_cl = cl; g.sub_attr = attr;
    g.sub_max_s = max_s; g.sub_exch = exch;
    g.sub_last_val = attr_value(ep, cl, attr);
    g.sub_last_ms = g.port.millis(g.port.ctx);
  } else {
    g.sub_active = false;
  }

  g.rpt_npaths = 0; g.rpt_nstatus = 0;
  rpt_add_query(have, ep, have, cl, have, attr);          // wildcard when !have
  g.rpt_active = true; g.rpt_is_sub = true; g.rpt_phase = 0;
  g.rpt_exch = exch; g.rpt_cursor = 0;
  g.rpt_sub_id = sid; g.rpt_sub_max_s = max_s;

  // Record the subscription on the current operational session so matter_loop
  // pushes live ReportData updates (on app-value change, and at max interval).
  mtrc_case_sess *subs = case_session_find(g.case_my_sid);
  if (subs) {
    subs->sub_active = true; subs->sub_id = sid; subs->sub_max_s = max_s;
    subs->sub_exch = exch; subs->sub_last_ms = g.port.millis(g.port.ctx);
    subs->sub_last_gen = g.app_gen;
  }

  char m[80];
  snprintf(m, sizeof(m), "IM Subscribe id=%u max=%us hosted=%d paths=%d",
           (unsigned)sid,(unsigned)max_s,hosted,g.rpt_npaths);
  mlog(MATTER_LOG_INFO, m);
  send_report_chunk(ack);                                 // first priming chunk
}

// Consume the rest of a container whose opening element was just read (skips
// nested structs/arrays/lists) so the flat walker resyncs on the next element.
static void tlv_skip_container(mtrc_tlv_reader *r) {
  int depth = 1; mtrc_tlv_elem e;
  while (depth > 0 && mtrc_tlv_read(r, &e)) {
    if (e.type == MTRC_TLV_STRUCT || e.type == MTRC_TLV_ARRAY || e.type == MTRC_TLV_LIST) depth++;
    else if (e.type == MTRC_TLV_END) depth--;
  }
}

// Handle a WriteRequest -> WriteResponse. Apple Home writes the ACL (Access
// Control, cluster 0x001F) over the operational session right after
// CommissioningComplete; with no WriteResponse it retransmits and the "add"
// times out. We accept every write and report SUCCESS per AttributePathIB. A
// SCALAR write to an app endpoint is applied to the data model (so a writable
// attribute — e.g. Fan PercentSetting — actually takes effect; a script reads it
// with matterGet). Complex values (e.g. the ACL list) are accepted but skipped.
static void im_handle_write(const uint8_t *payload, size_t plen,
                            uint16_t exch, uint32_t ack) {
  static uint8_t resp[512];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, resp, sizeof(resp));
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());          // WriteResponseMessage
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(0));           // WriteResponses [AttributeStatusIB]

  int npaths = 0;
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, payload, plen);
  mtrc_tlv_elem e;
  while (mtrc_tlv_read(&r, &e)) {
    if (e.type != MTRC_TLV_LIST) continue;             // AttributePathIB is the only list
    int have_cl = 0, have_attr = 0; uint16_t ep = 0; uint32_t cl = 0, attr = 0;
    mtrc_tlv_elem ie;
    while (mtrc_tlv_read(&r, &ie) && ie.type != MTRC_TLV_END) {
      if (ie.tag.ctrl != MTRC_TLV_TAG_CONTEXT) continue;
      if      (ie.tag.number == 2) { ep   = (uint16_t)ie.u; }
      else if (ie.tag.number == 3) { cl   = (uint32_t)ie.u; have_cl = 1; }
      else if (ie.tag.number == 4) { attr = (uint32_t)ie.u; have_attr = 1; }
    }
    if (!have_cl) continue;

    // The Data field follows the path inside the AttributeDataIB. Apply a scalar
    // write to an app endpoint; consume (skip) a complex value like the ACL.
    mtrc_tlv_elem de;
    if (mtrc_tlv_read(&r, &de)) {
      if (de.type == MTRC_TLV_STRUCT || de.type == MTRC_TLV_ARRAY || de.type == MTRC_TLV_LIST) {
        tlv_skip_container(&r);
      } else if (have_attr && ep != 0 &&
                 (de.type == MTRC_TLV_UINT || de.type == MTRC_TLV_SINT || de.type == MTRC_TLV_BOOL)) {
        uint64_t val = (de.type == MTRC_TLV_SINT) ? (uint64_t)de.i : de.u;
        if (mtrc_dm_find(ep, cl, attr)) { mtrc_dm_set(ep, cl, attr, val); g.app_gen++; }
      }
    }

    mtrc_tlv_start_struct(&w, mtrc_tlv_anon());         //  AttributeStatusIB
    mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));           //   AttributePathIB
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), ep);
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(3), cl);
    if (have_attr) mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(4), attr);
    mtrc_tlv_end_container(&w);
    mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));         //   StatusIB
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), 0);          //    Status = SUCCESS
    mtrc_tlv_end_container(&w);
    mtrc_tlv_end_container(&w);                         //  end AttributeStatusIB
    npaths++;
  }

  mtrc_tlv_end_container(&w);                           // end WriteResponses
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);         // InteractionModelRevision
  mtrc_tlv_end_container(&w);                           // end WriteResponseMessage

  if (mtrc_tlv_writer_ok(&w)) {
    secured_send(MTRC_IM_WRITE_RESPONSE, MTRC_PROTO_IM, resp,
                 mtrc_tlv_writer_len(&w), exch, true, ack, true);
    char m[48]; snprintf(m, sizeof(m), "IM WriteResponse sent (%d attrs)", npaths);
    mlog(MATTER_LOG_INFO, m);
  } else {
    mlog(MATTER_LOG_ERROR, "IM write: response build overflow");
  }
}

// A message arrived on the established PASE secure session: decrypt with the
// I2R key, parse the inner protocol header, dispatch the IM.
static void secured_dispatch(const uint8_t *buf, size_t len, const uint8_t *rx_key,
                             uint64_t peer_node_id) {
  mtrc_msg_header mh; mtrc_proto_header ph;
  const uint8_t *ipl; size_t ipll;
  static uint8_t pt[1280];
  if (!mtrc_sec_decode(buf, len, rx_key, peer_node_id, &mh, &ph, pt, sizeof(pt), &ipl, &ipll)) {
    // MIC/decrypt failures usually mean wrong key (no session yet for that peer)
    // OR Google rotated keys silently. Include session id + peer node so we know
    // which session was tried (helps diagnose "kann nicht verbinden" cases).
    char em[80];
    snprintf(em, sizeof(em), "secured rx MIC/decrypt FAIL sid=%u peer=0x%016llX len=%u",
             (unsigned)mh.session_id, (unsigned long long)peer_node_id, (unsigned)len);
    mlog(MATTER_LOG_ERROR, em);
    return;
  }
#ifdef MTRC_DIAG_HANS
  // Diagnostic build (Hans / Google Nest debugging): dump every decrypted
  // message with proto/op + first 24 hex bytes of inner payload. Disable in
  // production — chatty during a subscribe burst.
  {
    char hx[64]; int hp = 0;
    size_t nh = ipll < 24 ? ipll : 24;
    for (size_t i = 0; i < nh && hp < 58; i++)
      hp += snprintf(hx + hp, sizeof(hx) - hp, "%02X", ipl[i]);
    char dm[120];
    snprintf(dm, sizeof(dm),
             "DIAG secured rx proto=0x%04X op=0x%02X exch=0x%04X plen=%u %s",
             (unsigned)ph.protocol_id, (unsigned)ph.opcode,
             (unsigned)ph.exchange_id, (unsigned)ipll, hx);
    mlog(MATTER_LOG_INFO, dm);
  }
#endif
  if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_INVOKE_REQUEST) {
    im_handle_invoke(ipl, ipll, ph.exchange_id, mh.msg_counter);
  } else if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_READ_REQUEST) {
    im_handle_read(ipl, ipll, ph.exchange_id, mh.msg_counter);
  } else if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_SUBSCRIBE_REQUEST) {
    im_handle_subscribe(ipl, ipll, ph.exchange_id, mh.msg_counter);
  } else if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_WRITE_REQUEST) {
    im_handle_write(ipl, ipll, ph.exchange_id, mh.msg_counter);
  } else if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_TIMED_REQUEST) {
    // Timed interaction (Core Spec §8.7): the controller sends TimedRequest{0:timeoutMs}
    // before a timed Invoke/Write. We MUST reply with a StatusResponse(SUCCESS); the actual
    // Invoke/Write then arrives on the SAME exchange and is handled by the branches above.
    // Google Nest uses a timed interaction in its post-CASE setup; Apple Home does not — so
    // without this the Nest stalls (no follow-up, retries the TimedRequest, then reports
    // "device can't be added"). This device has no command requiring strict timed gating, so
    // the follow-up is accepted unconditionally (timeout value not enforced).
    static const uint8_t sr[] = { 0x15, 0x24, 0x00, 0x00, 0x24, 0xFF, 0x0C, 0x18 }; // {0:SUCCESS, 0xFF:IMrev=12}
    secured_send(MTRC_IM_STATUS_RESPONSE, MTRC_PROTO_IM, sr, sizeof(sr),
                 ph.exchange_id, true, mh.msg_counter, true);
    mlog(MATTER_LOG_INFO, "IM TimedRequest -> StatusResponse SUCCESS");
  } else if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == MTRC_IM_STATUS_RESPONSE
             && g.rpt_active) {
    // Flow control for a chunked ReportData: the controller StatusResponses each
    // chunk. Pull the next chunk; for a subscribe, after the final chunk send the
    // SubscribeResponse. The reply piggybacks the MRP ack for this StatusResponse.
    if (g.rpt_phase == 1) {                 // subscribe priming done -> SubscribeResponse
      static uint8_t sr[80];
      int m2 = mtrc_im_build_subscribe_response(sr, sizeof(sr), g.rpt_sub_id, g.rpt_sub_max_s);
      if (m2 > 0) secured_send(MTRC_IM_SUBSCRIBE_RESPONSE, MTRC_PROTO_IM, sr, (size_t)m2,
                               g.rpt_exch, true, mh.msg_counter, true);
      g.rpt_active = false;
      mlog(MATTER_LOG_INFO, "IM SubscribeResponse sent");
    } else {
      send_report_chunk(mh.msg_counter);    // next data chunk
    }
  } else {
#ifdef MTRC_DIAG
    // Debug: dump a controller's IM StatusResponse (op 0x01) raw TLV — a non-zero
    // status after our ReportData means the controller rejected it. -DMTRC_DIAG.
    if (ph.protocol_id == MTRC_PROTO_IM && ph.opcode == 0x01 && ipll <= 24) {
      char hx[56]; int hp = 0;
      for (size_t i = 0; i < ipll && hp < 52; i++) hp += snprintf(hx + hp, sizeof(hx) - hp, "%02X", ipl[i]);
      char sm[80]; snprintf(sm, sizeof(sm), "DIAG IM StatusResponse raw=%s", hx);
      mlog(MATTER_LOG_INFO, sm);
    }
#endif
    char m[80];
    snprintf(m, sizeof(m), "secured rx proto=0x%04X op=0x%02X (unhandled)",
             (unsigned)ph.protocol_id, (unsigned)ph.opcode);
    mlog(MATTER_LOG_DEBUG, m);
    // MRP: a reliable message we generate no application response for (e.g. a
    // StatusResponse acking our priming report) must still be acknowledged, or
    // the peer (Apple) retransmits it forever and the add/subscribe stalls. Send
    // a non-reliable StandaloneAck for its message counter. Bare acks (op 0x10,
    // R=0) need no ack, so this never loops.
    if (ph.reliability) {
      secured_send(MTRC_SC_STANDALONE_ACK, MTRC_PROTO_SECURE_CHANNEL, NULL, 0,
                   ph.exchange_id, true, mh.msg_counter, false);
    }
  }
}

static void pase_dispatch(const uint8_t *buf, size_t len, uint16_t src_port) {
  (void)src_port;
  { char m[40]; snprintf(m, sizeof(m), "rx %u B (dispatch)", (unsigned)len);
    mlog(MATTER_LOG_DEBUG, m); }
  // Peek the message header to route by session id.
  mtrc_msg_header mh0;
  if (mtrc_frame_decode_msg_header(buf, len, &mh0) < 0) {
    mlog(MATTER_LOG_DEBUG, "rx: msg-header decode FAIL"); return; }

  // The initiator carries an ephemeral Source Node ID on the unsecured
  // session; our replies must echo it back as the Destination Node ID
  // (CSA Core §4.5.2 — exactly one node id present on unsecured messages).
  if (mh0.has_src) { g.peer_node_id = mh0.src_node_id; g.peer_has_node = true; }

  if (mh0.session_id != 0) {
    // Secured session traffic: PASE (commissioning IM) or CASE (operational).
    // The message counter sits in the cleartext header (it seeds the decrypt
    // nonce), so MRP duplicate suppression works BEFORE decrypt: a retransmit
    // carries the same counter on the same session. Re-send the cached secured
    // reply (it already acks that counter) instead of re-running the handler —
    // re-running a retransmitted AddNOC persisted a SECOND fabric for the same
    // node (#85). Tracked per session because PASE and each CASE session have
    // independent counter spaces.
    if (g.pase_secure && mh0.session_id == g.my_session_id) {
      if (g.pase_have_rx_ctr && mh0.msg_counter == g.pase_rx_last_ctr) {
        if (g.sec_last_tx_len && g.port.udp_send)
          g.port.udp_send(g.port.ctx, g.reply_ip6, g.reply_port,
                          g.sec_last_tx_buf, g.sec_last_tx_len);
        mlog(MATTER_LOG_DEBUG, "rx: dup PASE counter -> re-sent last secured reply");
        return;
      }
      g.pase_rx_last_ctr = mh0.msg_counter; g.pase_have_rx_ctr = true;
      tx_use_pase();
      secured_dispatch(buf, len, g.i2r, 0);   // PASE: commissioning peer node id = 0
    } else {
      // Operational: find the established session matching this session id, load
      // it into the working set, dispatch, then save it back so its TX counter
      // persists. This is what lets Apple's phone AND home hub talk at once.
      mtrc_case_sess *ss = case_session_find(mh0.session_id);
      if (ss) {
        if (ss->have_rx_ctr && mh0.msg_counter == ss->rx_last_ctr) {
          if (g.sec_last_tx_len && g.port.udp_send)
            g.port.udp_send(g.port.ctx, g.reply_ip6, g.reply_port,
                            g.sec_last_tx_buf, g.sec_last_tx_len);
          mlog(MATTER_LOG_DEBUG, "rx: dup CASE counter -> re-sent last secured reply");
          return;
        }
        ss->rx_last_ctr = mh0.msg_counter; ss->have_rx_ctr = true;
        case_session_load(ss);
        memcpy(ss->cli_ip6, g.reply_ip6, 16); ss->cli_port = g.reply_port;  // for live reports
        tx_use_case();
        secured_dispatch(buf, len, g.case_i2r, g.case_peer_node_id);
        case_session_save(ss);
      }
    }
    return;
  }

  // MRP duplicate suppression on the unsecured session: a retransmit carries
  // the same message counter. Re-send our last reply (it already acks that
  // counter) rather than re-running the SPAKE2+/CASE crypto, which would mint
  // fresh ephemeral state and break the in-flight handshake.
  if (g.have_peer_ctr && mh0.msg_counter == g.peer_last_ctr) {
    if (g.last_tx_len && g.port.udp_send)
      g.port.udp_send(g.port.ctx, g.reply_ip6, g.reply_port, g.last_tx_buf, g.last_tx_len);
    mlog(MATTER_LOG_DEBUG, "rx: duplicate counter -> re-sent last reply");
    return;
  }
  g.peer_last_ctr = mh0.msg_counter; g.have_peer_ctr = true;

  // Unsecured session (id 0): PASE over Secure Channel.
  mtrc_msg_header mh; mtrc_proto_header ph;
  const uint8_t *pl; size_t pll;
  if (mtrc_frame_decode(buf, len, &mh, &ph, &pl, &pll) <= 0) {
    mlog(MATTER_LOG_DEBUG, "rx: frame decode FAIL"); return; }
  if (ph.protocol_id != MTRC_PROTO_SECURE_CHANNEL) {
    char m[48]; snprintf(m, sizeof(m), "rx: proto 0x%04X != SecureChannel",
                         (unsigned)ph.protocol_id);
    mlog(MATTER_LOG_DEBUG, m); return; }
  { char m[48]; snprintf(m, sizeof(m), "rx: SC opcode 0x%02X", (unsigned)ph.opcode);
    mlog(MATTER_LOG_DEBUG, m); }
  g.exchange_id = ph.exchange_id;
  switch (ph.opcode) {
    case MTRC_SC_PBKDF_PARAM_REQ: pase_handle_param_req(pl, pll, &mh); break;
    case MTRC_SC_PASE_PAKE1:      pase_handle_pake1(pl, pll, &mh);     break;
    case MTRC_SC_PASE_PAKE3:      pase_handle_pake3(pl, pll, &mh);     break;
    case MTRC_SC_CASE_SIGMA1:     case_handle_sigma1(pl, pll, &mh);    break;
    case MTRC_SC_CASE_SIGMA3:     case_handle_sigma3(pl, pll, &mh);    break;
    case MTRC_SC_STATUS_REPORT: {
      // A peer-sent StatusReport (e.g. Apple rejecting our Sigma2). Decode
      // GeneralCode(LE16) || ProtocolId(LE32) || ProtocolCode(LE16) so we can
      // see WHY. SecureChannel ProtocolCode: 0x0000 success, 0x0001
      // NoSharedTrustRoots, 0x0002 InvalidParam, 0x0003 CloseSession, 0x0004 Busy.
      if (pll >= 8) {
        unsigned gc = pl[0] | (pl[1] << 8);
        unsigned long pid = (unsigned long)pl[2] | ((unsigned long)pl[3] << 8) |
                            ((unsigned long)pl[4] << 16) | ((unsigned long)pl[5] << 24);
        unsigned pc = pl[6] | (pl[7] << 8);
        char m[80];
        snprintf(m, sizeof(m), "rx StatusReport gen=%u proto=0x%08lX code=0x%04X",
                 gc, pid, pc);
        mlog(MATTER_LOG_ERROR, m);
      }
      break;
    }
    default: break;
  }
}

// Push a live subscription ReportData over the CURRENT working-set session: all
// app-endpoint attributes (the dynamic sensor/light values). Small enough for
// one datagram. The caller loads the session and points g.reply_ip6 at its
// controller first. Reports reuse the subscription's exchange id.
static void send_subscription_report(uint32_t sub_id, uint16_t exch) {
  static uint8_t buf[1100];
  mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, buf, sizeof(buf));
  mtrc_tlv_start_struct(&w, mtrc_tlv_anon());                 // ReportDataMessage
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), sub_id);            // SubscriptionId
  mtrc_tlv_start_array(&w, mtrc_tlv_ctx(1));                 // AttributeReports
  for (int i = 0; i < mtrc_dm_attr_count(); i++) {
    const mtrc_dm_attr_t *a = mtrc_dm_attr_at(i);
    if (!a || a->endpoint == 0) continue;                    // app endpoints only (dynamic)
    if (mtrc_tlv_writer_len(&w) > 1000) break;               // keep to one datagram
    emit_attr_report(&w, a->endpoint, a->cluster, a->attr);
  }
  mtrc_tlv_end_container(&w);                                // end AttributeReports
  mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);             // InteractionModelRevision
  mtrc_tlv_end_container(&w);                                // end ReportDataMessage
  if (mtrc_tlv_writer_ok(&w))
    secured_send(MTRC_IM_REPORT_DATA, MTRC_PROTO_IM, buf, mtrc_tlv_writer_len(&w),
                 exch, false, 0, true);                      // device-initiated, reliable
}

// Send one Matter Event as an EventReport (ReportData) to every subscribed
// session — used by Generic Switch button events. Runs in the main loop.
static void matter_emit_event(uint16_t ep, uint32_t cl, uint32_t event_id,
                              int32_t a, int32_t b) {
  uint64_t evno = ++g.event_number;
  uint32_t now  = g.port.millis(g.port.ctx);
  for (int i = 0; i < MTRC_MAX_CASE_SESS; i++) {
    mtrc_case_sess *s = &g.case_sess[i];
    if (!s->in_use || !s->sub_active) continue;
    static uint8_t buf[256];
    mtrc_tlv_writer w; mtrc_tlv_writer_init(&w, buf, sizeof(buf));
    mtrc_tlv_start_struct(&w, mtrc_tlv_anon());                 // ReportDataMessage
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), s->sub_id);         // SubscriptionId
    mtrc_tlv_start_array(&w, mtrc_tlv_ctx(2));                 // EventReports
    mtrc_tlv_start_struct(&w, mtrc_tlv_anon());                //  EventReportIB
    mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(1));                //   EventDataIB
    mtrc_tlv_start_list(&w, mtrc_tlv_ctx(0));                  //    EventPathIB
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), ep);               //     Endpoint
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), cl);               //     Cluster
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(3), event_id);        //     Event
    mtrc_tlv_end_container(&w);                                //    end path
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), evno);            //    EventNumber
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(2), 1);              //    PriorityLevel = Info
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(4), now);            //    SystemTimestamp (ms)
    mtrc_tlv_start_struct(&w, mtrc_tlv_ctx(7));              //    Data
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0), (uint32_t)a);    //     field 0 (New/PreviousPosition)
    if (cl == 0x003B && (event_id == 0x05 || event_id == 0x06))
      mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(1), (uint32_t)b);  //     field 1 (press count)
    mtrc_tlv_end_container(&w);                                //    end Data
    mtrc_tlv_end_container(&w);                                //   end EventDataIB
    mtrc_tlv_end_container(&w);                                //  end EventReportIB
    mtrc_tlv_end_container(&w);                                // end EventReports
    mtrc_tlv_put_uint(&w, mtrc_tlv_ctx(0xFF), 1);            // InteractionModelRevision
    mtrc_tlv_end_container(&w);                                // end ReportDataMessage
    if (mtrc_tlv_writer_ok(&w)) {
      memcpy(g.reply_ip6, s->cli_ip6, 16); g.reply_port = s->cli_port;
      case_session_load(s);
      tx_use_case();
      secured_send(MTRC_IM_REPORT_DATA, MTRC_PROTO_IM, buf, mtrc_tlv_writer_len(&w),
                   s->sub_exch, false, 0, true);
      case_session_save(s);
    }
  }
}

// matterEvent() backend: enqueue an event; the actual send happens in
// matter_loop (main loop) so it is safe to call from a VM-task callback.
matter_err_t matter_queue_event(uint16_t ep, uint32_t cl, uint32_t event_id,
                                int32_t a, int32_t b) {
  if (!g_ptr || !g.inited) return MATTER_ERR_NOT_INIT;
  uint8_t nh = (uint8_t)((g.ev_head + 1) % MTRC_EV_QUEUE);
  if (nh == g.ev_tail) return MATTER_ERR_NO_MEM;             // ring full: drop
  g.ev_q[g.ev_head].ep = ep; g.ev_q[g.ev_head].cl = cl;
  g.ev_q[g.ev_head].ev = event_id; g.ev_q[g.ev_head].a = a; g.ev_q[g.ev_head].b = b;
  g.ev_head = nh;
  return MATTER_OK;
}

void matter_loop(void) {
  if (!g_ptr || !g.inited || !g.started) return;   // off / not started -> no-op
  // Fail-safe expiry (Core Spec §11.10): if a commissioning armed the fail-safe
  // but never sent CommissioningComplete, roll back its tentative fabric so the
  // abandoned attempt does not leak a persisted fabric.
  if (g.fs_armed && (int32_t)(g.port.millis(g.port.ctx) - g.fs_expiry_ms) >= 0)
    matter_failsafe_disarm(false);
  // Enhanced commissioning window backstop: if an AdministratorCommissioning
  // OpenCommissioningWindow opened a window that nobody completed, stop accepting
  // PASE when its CommissioningTimeout elapses (the host's Bind-window timer
  // withdraws the _matterc mDNS advert separately).
  if (g.ocw_expiry_ms && (int32_t)(g.port.millis(g.port.ctx) - g.ocw_expiry_ms) >= 0) {
    g.ocw_active = false; g.ocw_expiry_ms = 0; g.commissionable = false;
    mlog(MATTER_LOG_INFO, "AdminComm: commissioning window expired");
  }
  // Drain ALL queued datagrams (a burst from several controllers must not be
  // dropped). Set the reply target from each packet's own source first.
  while (g.rx_head != g.rx_tail) {
    mtrc_rx_pkt *p = &g.rx_q[g.rx_tail];
    memcpy(g.reply_ip6, p->ip6, 16); g.reply_port = p->port;
    pase_dispatch(p->buf, p->len, p->port);            // crypto-heavy work here
    g.rx_tail = (uint8_t)((g.rx_tail + 1) % MTRC_RX_QUEUE);
  }

  // Live operational subscriptions: push a ReportData to each subscribed
  // controller when an app value changed (>=1 s apart) or the max interval
  // elapsed. Runs in the main loop, sequential with the RX drain above, so the
  // shared working set / g.reply_ip6 are not raced. Skipped while a chunked
  // priming report is still in flight.
  if (!g.rpt_active) {
    uint32_t now2 = g.port.millis(g.port.ctx);
    for (int i = 0; i < MTRC_MAX_CASE_SESS; i++) {
      mtrc_case_sess *s = &g.case_sess[i];
      if (!s->in_use || !s->sub_active) continue;
      int changed = (s->sub_last_gen != g.app_gen);
      uint32_t since = now2 - s->sub_last_ms;
      if (!((changed && since >= 1000u) || since >= (uint32_t)s->sub_max_s * 1000u)) continue;
      s->sub_last_ms = now2; s->sub_last_gen = g.app_gen;
      memcpy(g.reply_ip6, s->cli_ip6, 16); g.reply_port = s->cli_port;
      case_session_load(s);
      tx_use_case();
      send_subscription_report(s->sub_id, s->sub_exch);
      case_session_save(s);
    }
  }

  // Drain queued Matter events (matterEvent) -> EventReports to subscribers.
  while (g.ev_head != g.ev_tail) {
    uint8_t t = g.ev_tail;
    matter_emit_event(g.ev_q[t].ep, g.ev_q[t].cl, g.ev_q[t].ev, g.ev_q[t].a, g.ev_q[t].b);
    g.ev_tail = (uint8_t)((t + 1) % MTRC_EV_QUEUE);
  }

  // Legacy single-attribute subscription engine (PASE/commissioning path):
  // send a ReportData when the value changed or the max interval elapsed.
  if (g.sub_active && g.pase_secure) {
    uint32_t now = g.port.millis(g.port.ctx);
    uint64_t v = attr_value(g.sub_ep, g.sub_cl, g.sub_attr);
    if (v != g.sub_last_val || (now - g.sub_last_ms) >= (uint32_t)g.sub_max_s * 1000u) {
      g.sub_last_val = v; g.sub_last_ms = now;
      static uint8_t rep[160];
      int n = mtrc_im_build_report_uint(rep, sizeof(rep), g.sub_id,
                                        g.sub_ep, g.sub_cl, g.sub_attr, v);
      if (n > 0) {
        tx_use_pase();   // the subscription was established over PASE
        secured_send(MTRC_IM_REPORT_DATA, MTRC_PROTO_IM, rep, (size_t)n, g.sub_exch, false, 0, true);
      }
    }
  }
}

// ---- inbound transport pumps ------------------------------------------
// May run in a network task — only copy + flag; processing is in matter_loop.
void matter_udp_rx(const uint8_t src_ip6[16], uint16_t src_port,
                   const void *buf, size_t len) {
  if (!g_ptr || !g.inited) return;
  if (len == 0 || len > sizeof(g.rx_q[0].buf)) return;
  uint8_t nh = (uint8_t)((g.rx_head + 1) % MTRC_RX_QUEUE);
  if (nh == g.rx_tail) return;                        // ring full: drop (rare)
  mtrc_rx_pkt *p = &g.rx_q[g.rx_head];
  if (src_ip6) memcpy(p->ip6, src_ip6, 16); else memset(p->ip6, 0, 16);
  p->port = src_port; p->len = (uint16_t)len;
  memcpy(p->buf, buf, len);
  g.rx_head = nh;                                     // publish AFTER fields written
}

void matter_ble_rx(const void *buf, size_t len) {
  (void)buf; (void)len;
  // TODO Phase 3: BTP reassembly -> PASE/commissioning message handler.
}

// ---- endpoints / attributes -------------------------------------------
int matter_add_endpoint(uint32_t device_type_id) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  uint16_t ep = g.next_ep;
  if (mtrc_dm_add_endpoint(ep, device_type_id) < 0) return MATTER_ERR_NO_MEM;
  mtrc_dm_add_cluster(ep, MTRC_CL_DESCRIPTOR);   // mandatory on every endpoint
  dm_add_identify(ep);                           // Identify is mandatory on every app endpoint
  dm_attach_device_type(ep, device_type_id);
  g.next_ep++;
  return (int)ep;
}

// Name an endpoint so a controller (Apple Home) shows it with a friendly title
// instead of a generic "Temperature Sensor 1". Implemented as the Matter bridge
// pattern: the first call lazily creates an Aggregator (0x000E) endpoint, the
// named endpoint becomes a Bridged Node (its DeviceTypeList gains 0x0013) and
// gets a Bridged Device Basic Information cluster (0x0039) whose NodeLabel is
// the name. Idempotent — calling again just updates the label. Call AFTER the
// endpoint exists (i.e. after matterAdd); a re-declared model (matterReset)
// drops all labels and the aggregator.
matter_err_t matter_set_label(uint16_t ep, const char *name) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  if (ep == 0) return MATTER_ERR_INVALID_ARG;
  uint32_t dt; if (!mtrc_dm_endpoint_device_type(ep, &dt)) return MATTER_ERR_INVALID_ARG;

  if (g.aggregator_ep == 0) {                       // lazily stand up the bridge
    uint16_t aep = g.next_ep;
    if (mtrc_dm_add_endpoint(aep, 0x000E) < 0) return MATTER_ERR_NO_MEM;   // Aggregator
    mtrc_dm_add_cluster(aep, MTRC_CL_DESCRIPTOR);
    dm_add_identify(aep);
    g.next_ep++;
    g.aggregator_ep = aep;
  }

  int idx = -1;
  for (int i = 0; i < g.label_count; i++) if (g.labels[i].ep == ep) { idx = i; break; }
  if (idx < 0) {
    if (g.label_count >= (int)(sizeof(g.labels)/sizeof(g.labels[0]))) return MATTER_ERR_NO_MEM;
    idx = g.label_count++;
    g.labels[idx].ep = ep;
    mtrc_dm_add_cluster(ep, 0x0039);                // Bridged Device Basic Information
  }
  strncpy(g.labels[idx].name, name ? name : "", sizeof(g.labels[idx].name) - 1);
  g.labels[idx].name[sizeof(g.labels[idx].name) - 1] = 0;
#ifdef MTRC_DIAG
  { char m[80]; snprintf(m, sizeof m, "DIAG label ep=%u agg=%u '%s'",
      (unsigned)ep, (unsigned)g.aggregator_ep, g.labels[idx].name);
    mlog(MATTER_LOG_INFO, m); }
#endif
  return MATTER_OK;
}

// Decode a leading TLV scalar (uint/bool) into a u64. Returns 1 on success.
static int tlv_scalar_u64(const uint8_t *tlv, size_t len, uint64_t *out) {
  mtrc_tlv_reader r; mtrc_tlv_reader_init(&r, tlv, len);
  mtrc_tlv_elem e;
  if (!mtrc_tlv_read(&r, &e)) return 0;
  if (e.type == MTRC_TLV_UINT) { *out = e.u; return 1; }
  if (e.type == MTRC_TLV_BOOL) { *out = e.u ? 1 : 0; return 1; }
  return 0;
}

matter_err_t matter_set_attr(uint16_t endpoint, uint32_t cluster,
                             uint32_t attr, const uint8_t *tlv, size_t tlv_len) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  if (!tlv || tlv_len == 0) return MATTER_ERR_INVALID_ARG;
  uint64_t v;
  if (!tlv_scalar_u64(tlv, tlv_len, &v)) return MATTER_ERR_INVALID_ARG;
  // Auto-register the attribute if the host pushes a value before declaring it.
  if (!mtrc_dm_find(endpoint, cluster, attr))
    mtrc_dm_add_attr(endpoint, cluster, attr, MTRC_DM_T_U32, 0, v);
  else
    mtrc_dm_set(endpoint, cluster, attr, v);
  // The subscription report engine in matter_loop() picks up the change on the
  // next pump (it polls attr_value), so no immediate report is needed here.
  return MATTER_OK;
}

// ---- TinyC / script data-model bridge (Phase C2) -----------------------
void matter_reset_model(void) {
  if (!g.inited) return;
  mtrc_dm_reset();
  dm_seed_root();
  g.next_ep = 1;
  g.aggregator_ep = 0;          // bridge torn down with the model
  g.label_count = 0;
}

matter_err_t matter_add_cluster(uint16_t endpoint, uint32_t cluster) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  return mtrc_dm_add_cluster(endpoint, cluster) == 0 ? MATTER_OK : MATTER_ERR_NO_MEM;
}

matter_err_t matter_add_attr(uint16_t endpoint, uint32_t cluster, uint32_t attr,
                             int type, int writable) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  if (type < 0 || type > MTRC_DM_T_FLOAT) type = MTRC_DM_T_U32;
  uint8_t flags = writable ? MTRC_DM_F_WRITABLE : 0;
  return mtrc_dm_add_attr(endpoint, cluster, attr, (mtrc_dm_type_t)type, flags, 0) == 0
         ? MATTER_OK : MATTER_ERR_NO_MEM;
}

matter_err_t matter_set_attr_uint(uint16_t endpoint, uint32_t cluster,
                                  uint32_t attr, uint64_t value) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  int changed;
  if (!mtrc_dm_find(endpoint, cluster, attr)) {
    mtrc_dm_add_attr(endpoint, cluster, attr, MTRC_DM_T_U32, 0, value);
    changed = 1;                                  // first publication of this attr
  } else {
    changed = mtrc_dm_set(endpoint, cluster, attr, value);  // 1 only if the value differs
  }
  // Only a REAL change schedules a live subscription report. Bumping app_gen on
  // every write (even identical values) made a script that re-publishes N sensor
  // attrs each second flood the subscriber with N ReportData bursts/second —
  // which starved the Matter CASE handshake during commissioning.
  if (changed && endpoint != 0) g.app_gen++;
  return MATTER_OK;
}

// matterSetFloat backend. For a FLOAT attribute the raw single-precision value
// is stored (as its 32-bit bits); for any other type the value is scaled to an
// integer round(f*scale) — so one builtin serves both float wire attrs (air
// quality) and scaled-int wire attrs (temperature 0.01C, power mW, ...).
matter_err_t matter_set_attr_scaled(uint16_t endpoint, uint32_t cluster,
                                    uint32_t attr, float f, int32_t scale) {
  if (!g.inited) return MATTER_ERR_NOT_INIT;
  mtrc_dm_attr_t *a = mtrc_dm_find(endpoint, cluster, attr);
  uint64_t v;
  if (a && a->type == MTRC_DM_T_FLOAT) {
    uint32_t bits; memcpy(&bits, &f, 4); v = bits;           // store float bits as-is
  } else {
    double s = (double)f * (double)scale;                    // scale + round-to-nearest
    v = (uint64_t)(int64_t)(s + (s < 0 ? -0.5 : 0.5));
  }
  int changed;
  if (!a) { mtrc_dm_add_attr(endpoint, cluster, attr, MTRC_DM_T_U32, 0, v); changed = 1; }
  else    { changed = mtrc_dm_set(endpoint, cluster, attr, v); }   // 1 only if value differs
  if (changed && endpoint != 0) g.app_gen++;     // report only on real change (see matter_set_attr_uint)
  return MATTER_OK;
}

int matter_get_attr_uint(uint16_t endpoint, uint32_t cluster,
                         uint32_t attr, uint64_t *out) {
  if (!g.inited) return 0;
  return mtrc_dm_get(endpoint, cluster, attr, out);
}

// ---- onboarding + introspection ---------------------------------------
const char *matter_qr_uri(void)      { return g_ptr ? g.qr : ""; }
const char *matter_manual_code(void) { return g_ptr ? g.manual : ""; }
const char *matter_version(void)     { return MATTER_C_VERSION_STR; }
bool        matter_is_commissioned(void) { return false; } // TODO Phase 3
