// matter_c.h — public API for the minimal pure-C Matter device library.
//
// Design goal: the Matter CORE is firmware-agnostic. It contains no
// Tasmota (or any other firmware) globals. Every host coupling —
// key/value persistence, monotonic time, cryptographic randomness,
// logging, and the two network transports (operational UDP +
// commissioning BLE) plus mDNS discovery — is injected through the
// matter_port_t struct below. Tasmota's xdrv_124 fills it with
// UFS / lwIP-UDP / NimBLE / esp-mDNS; another firmware fills it with
// its own equivalents. That is what makes this library reusable.
//
// Crypto is NOT in the port: the library calls BearSSL's br_* API
// directly (low-RAM i15 P-256, AES-CCM, SHA-256, HKDF) — see PLAN.md.
//
// Inspired by the device-subset architecture of Tasmota Berry Matter
// (S. Hadinger). Implemented from the CSA Matter 1.4.1 spec, not
// converted from the Berry source. GPLv3.

#ifndef MATTER_C_H
#define MATTER_C_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MATTER_C_VERSION_MAJOR 0
#define MATTER_C_VERSION_MINOR 1
#define MATTER_C_VERSION_PATCH 0
#define MATTER_C_VERSION_STR  "0.1.0"

// ---- Status codes ------------------------------------------------------
typedef enum {
  MATTER_OK                  =  0,
  MATTER_ERR_NOT_IMPLEMENTED = -1,   // stub — not built yet
  MATTER_ERR_INVALID_ARG     = -2,
  MATTER_ERR_NO_MEM          = -3,
  MATTER_ERR_NOT_INIT        = -4,
  MATTER_ERR_STATE           = -5,   // wrong lifecycle state
  MATTER_ERR_CRYPTO          = -6,
  MATTER_ERR_STORE           = -7,   // persistence read/write/absent
  MATTER_ERR_TRANSPORT       = -8,
} matter_err_t;

typedef enum {
  MATTER_LOG_ERROR = 0,
  MATTER_LOG_INFO  = 1,
  MATTER_LOG_DEBUG = 2,
} matter_log_level_t;

// ---- Host port / HAL ---------------------------------------------------
// Every function pointer is called with the opaque `ctx` registered in the
// struct. Functions returning matter_err_t return MATTER_OK on success.
typedef struct matter_port {
  void *ctx;   // opaque host handle, passed back to every callback below

  // -- key/value persistence (fabric table, session resumption store) --
  //  kv_get: copy up to *len bytes into buf, set *len to actual length.
  //          Return MATTER_ERR_STORE if the key is absent.
  matter_err_t (*kv_get)(void *ctx, const char *key, void *buf, size_t *len);
  matter_err_t (*kv_set)(void *ctx, const char *key, const void *buf, size_t len);
  matter_err_t (*kv_del)(void *ctx, const char *key);

  // -- time + entropy --
  uint32_t     (*millis)(void *ctx);                              // monotonic ms
  matter_err_t (*random_bytes)(void *ctx, void *buf, size_t len); // MUST be CSPRNG

  // -- logging --
  void (*log)(void *ctx, matter_log_level_t lvl, const char *msg);

  // -- operational transport: UDP (host owns the socket) --
  //  Core calls udp_send to emit a datagram; host calls matter_udp_rx()
  //  on every inbound Matter datagram.
  matter_err_t (*udp_send)(void *ctx, const uint8_t ip6[16], uint16_t port,
                           const void *buf, size_t len);

  // -- commissioning transport: BLE (host owns the GATT service) --
  //  Core calls ble_send to emit a BTP packet; host calls matter_ble_rx().
  //  ble_advertise(enable) toggles the commissionable BLE advert.
  matter_err_t (*ble_send)(void *ctx, const void *buf, size_t len);
  matter_err_t (*ble_advertise)(void *ctx, const uint8_t *adv, size_t len, bool enable);

  // -- discovery (mDNS) --
  matter_err_t (*mdns_publish)(void *ctx, const char *service,
                               const char *instance, uint16_t port,
                               const char *const *txt, size_t txt_count);
  // Withdraw a previously published service instance (e.g. a removed fabric's
  // operational _matter._tcp record). May be NULL (then stale records linger
  // until the next reboot). `instance` matches what was passed to mdns_publish.
  matter_err_t (*mdns_remove)(void *ctx, const char *service,
                              const char *instance);

  // -- application bridge: Matter wrote an attribute (e.g. OnOff On/Off) --
  //  Host applies it to the relay/light/etc. `tlv` is the raw TLV value.
  void (*on_attr_write)(void *ctx, uint16_t endpoint, uint32_t cluster,
                        uint32_t attr, const uint8_t *tlv, size_t tlv_len);

  // -- application bridge: Matter is READING an attribute. Host returns the
  //  live value (e.g. the real relay state) via *out + MATTER_OK; return
  //  non-OK to let the core use its cached/default value. May be NULL.
  matter_err_t (*on_attr_read)(void *ctx, uint16_t endpoint, uint32_t cluster,
                               uint32_t attr, uint64_t *out);

  // -- application bridge: Matter INVOKED a command (e.g. OnOff On/Off/Toggle).
  //  The host routes it to the application — in Tasmota/TinyC that is the
  //  script's `MatterInvoke(ep, cluster, cmd)` callback, falling back to the
  //  built-in behavior (OnOff -> relay) when no script handles it. `arg` is a
  //  primary command argument where applicable (e.g. Level), else the command
  //  id. Returns 1 if handled (core replies SUCCESS), 0 if not (UNSUPPORTED).
  //  May be NULL — then the core uses on_attr_write for OnOff (legacy path).
  int (*on_command)(void *ctx, uint16_t endpoint, uint32_t cluster,
                    uint32_t command, int32_t arg);
} matter_port_t;

// ---- Device configuration ----------------------------------------------
typedef struct matter_config {
  uint16_t    vendor_id;     // dev/test VID 0xFFF1..0xFFF4 until certified
  uint16_t    product_id;
  uint16_t    discriminator; // 12-bit commissioning discriminator
  uint32_t    passcode;      // 8-digit setup passcode (not a trivial value)
  const char *device_name;   // advertised commissionable name
} matter_config_t;

// ---- Lifecycle ---------------------------------------------------------
// Typical host sequence:
//   matter_init(&port, &cfg);
//   matter_add_endpoint(MATTER_DEVTYPE_ON_OFF_PLUGIN);
//   matter_start();                 // begin commissioning advertising
//   ... in the host main loop:  matter_loop();
matter_err_t matter_init(const matter_port_t *port, const matter_config_t *cfg);
matter_err_t matter_start(void);          // begin commissioning advertising
matter_err_t matter_stop(void);
matter_err_t matter_factory_reset(void);  // wipe all fabrics + sessions
void         matter_loop(void);           // pump MRP/timers — call frequently

// ---- Inbound transport pumps (host → core) -----------------------------
void matter_udp_rx(const uint8_t src_ip6[16], uint16_t src_port,
                   const void *buf, size_t len);
void matter_ble_rx(const void *buf, size_t len);

// ---- Endpoints / attributes -------------------------------------------
// Common Matter device-type ids (16-bit) — extend as clusters land.
#define MATTER_DEVTYPE_ON_OFF_PLUGIN   0x010A
#define MATTER_DEVTYPE_ON_OFF_LIGHT    0x0100
#define MATTER_DEVTYPE_DIMMABLE_LIGHT  0x0101
#define MATTER_DEVTYPE_TEMP_SENSOR     0x0302
#define MATTER_DEVTYPE_HUMIDITY_SENSOR 0x0307

// Register an endpoint of a Matter device-type. Returns the endpoint
// number (>= 1) on success, or a negative matter_err_t on failure.
int matter_add_endpoint(uint32_t device_type_id);

// Give an endpoint a friendly name that controllers (Apple Home) display as the
// accessory title. Turns the node into a Matter bridge (Aggregator + Bridged
// Device Basic Information NodeLabel); call after matter_add_endpoint for that
// endpoint. Idempotent — re-call to rename.
matter_err_t matter_set_label(uint16_t endpoint, const char *name);

// Host pushes a new value into an attribute (e.g. a fresh sensor reading).
matter_err_t matter_set_attr(uint16_t endpoint, uint32_t cluster,
                             uint32_t attr, const uint8_t *tlv, size_t tlv_len);

// ---- TinyC / script data-model bridge (Phase C2) -----------------------
// Integer-friendly wrappers over the data-model registry, used by the TinyC
// `mtr*` syscalls (and any host that prefers plain ints over TLV). The
// endpoint is the value returned by matter_add_endpoint().

// Reset the data model to just the root node (endpoint 0 + Basic Information).
// A script calls this before declaring its own endpoints for full control.
void         matter_reset_model(void);

// Declare a cluster on an endpoint (idempotent).
matter_err_t matter_add_cluster(uint16_t endpoint, uint32_t cluster);

// Declare an attribute (auto-adds the cluster). `type` is a mtrc_dm_type_t
// (0=bool, 1=u8, 2=u16, 3=u32, 4=u64, 5=enum8); `writable` (0/1) marks it
// Write-able by a controller.
matter_err_t matter_add_attr(uint16_t endpoint, uint32_t cluster, uint32_t attr,
                             int type, int writable);

// Set an attribute's value (set-or-create). The subscription report engine
// picks up the change on the next matter_loop() pump.
matter_err_t matter_set_attr_uint(uint16_t endpoint, uint32_t cluster,
                                  uint32_t attr, uint64_t value);

// matterSetFloat backend: store a FLOAT attribute's raw value, or round(f*scale)
// as an integer for any other (scaled-int) attribute type.
matter_err_t matter_set_attr_scaled(uint16_t endpoint, uint32_t cluster,
                                    uint32_t attr, float f, int32_t scale);

// Queue a Matter Event (e.g. Generic Switch button press) for delivery as an
// EventReport to subscribed controllers (sent from matter_loop).
matter_err_t matter_queue_event(uint16_t endpoint, uint32_t cluster,
                                uint32_t event_id, int32_t a, int32_t b);

// Get an attribute's cached value. Returns 1 + fills *out if present, else 0.
int          matter_get_attr_uint(uint16_t endpoint, uint32_t cluster,
                                  uint32_t attr, uint64_t *out);

// ---- Onboarding payload ------------------------------------------------
const char *matter_qr_uri(void);       // "MT:..." string for the QR code
const char *matter_manual_code(void);  // "1234-567-8901" manual pairing code

// QR matrix (rendered from matter_qr_uri) so a host can draw the code itself
// without an external/CDN QR library. matter_qr_size() returns the modules per
// side (0 if unavailable); matter_qr_dark(x,y) is true for a dark module.
int  matter_qr_size(void);
bool matter_qr_dark(int x, int y);

// ---- Introspection -----------------------------------------------------
const char *matter_version(void);      // MATTER_C_VERSION_STR
bool        matter_is_commissioned(void);

// Commissioning window (host-driven Bind/Unbind + timeout). When closed, the
// PASE responder refuses new commissioning; operational (CASE) sessions for
// existing fabrics are unaffected. matter_start() opens it; the host closes it
// on the Bind-window timeout or on Unbind.
matter_err_t matter_open_commissioning_window(void);  // Bind: advertise + accept PASE
void matter_set_commissionable(int on);
int  matter_is_commissionable(void);

#ifdef __cplusplus
}
#endif

#endif // MATTER_C_H
