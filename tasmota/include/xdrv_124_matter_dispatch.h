/*
  xdrv_124_matter_dispatch.h — TinyC Matter: built-in lib or the MATTERF BinPlugin

  Firmware with USE_MATTER_C and USE_BINPLUGINS carries both: the matter_c lib
  linked in, and the option to load the same code as the MATTERF BLIB
  (tasmota/Plugins/xblib_03_matter_full.cpp). Every matter_* call of the TinyC
  glue (xdrv_124_tinyc.ino) and of the VM's mtr* syscalls goes through the table
  below; the macros at the end redirect the existing calls, so the call sites
  stay as they are.

  The choice is made ONCE, at the first matter_init() (mtrc_ensure_inited):
  a MATTERF module in the plugin partition is initialized if nobody did yet;
  if it then exports the whole API, it is used and gets the
  firmware's BearSSL primitives (mtrc_crypto_bind); otherwise the built-in lib.
  After that it is fixed — the Matter state lives in whichever one was chosen.
  Before the choice every call goes to the built-in lib, as without plugins.
  If the chosen plugin is unloaded later, all calls go to stubs that do
  nothing and report MATTER_ERR_NOT_INIT (a reboot starts over).

  Included by xdrv_124_tinyc.ino right after its first #include "matter_c.h".
*/
#ifndef XDRV_124_MATTER_DISPATCH_H
#define XDRV_124_MATTER_DISPATCH_H

#include "../Plugins/matter/include/mtrc_crypto_ops.h"   // firmware -> plugin crypto seam

extern "C" TC_BLIB_REG_ENTRY *tc_blib_lookup(const char *name);

// R(ret, name, params, args, dead_value) for functions with a result,
// V(name, params, args) for void ones. name = the part after "matter_".
#define MTRC_API_LIST(R, V) \
  R(matter_err_t, init, (const matter_port_t *p, const matter_config_t *c), (p, c), MATTER_ERR_NOT_INIT) \
  V(udp_rx, (const uint8_t *ip6, uint16_t port, const void *buf, size_t len), (ip6, port, buf, len)) \
  R(matter_err_t, start, (void), (), MATTER_ERR_NOT_INIT) \
  V(loop, (void), ()) \
  R(const char *, qr_uri, (void), (), "") \
  R(bool, qr_dark, (int x, int y), (x, y), false) \
  R(int, qr_size, (void), (), 0) \
  R(matter_err_t, factory_reset, (void), (), MATTER_ERR_NOT_INIT) \
  V(set_commissionable, (int on), (on)) \
  R(matter_err_t, open_commissioning_window, (void), (), MATTER_ERR_NOT_INIT) \
  R(const char *, manual_code, (void), (), "") \
  R(matter_err_t, set_label, (uint16_t ep, const char *name), (ep, name), MATTER_ERR_NOT_INIT) \
  R(matter_err_t, set_attr_uint, (uint16_t ep, uint32_t cl, uint32_t at, uint64_t v), (ep, cl, at, v), MATTER_ERR_NOT_INIT) \
  R(matter_err_t, set_attr_scaled, (uint16_t ep, uint32_t cl, uint32_t at, float f, int32_t sc), (ep, cl, at, f, sc), MATTER_ERR_NOT_INIT) \
  V(reset_model, (void), ()) \
  R(matter_err_t, queue_event, (uint16_t ep, uint32_t cl, uint32_t ev, int32_t a, int32_t b), (ep, cl, ev, a, b), MATTER_ERR_NOT_INIT) \
  R(int, get_attr_uint, (uint16_t ep, uint32_t cl, uint32_t at, uint64_t *out), (ep, cl, at, out), 0) \
  R(int, add_endpoint, (uint32_t dt), (dt), -1) \
  R(matter_err_t, add_cluster, (uint16_t ep, uint32_t cl), (ep, cl), MATTER_ERR_NOT_INIT) \
  R(matter_err_t, add_attr, (uint16_t ep, uint32_t cl, uint32_t at, int type, int wr), (ep, cl, at, type, wr), MATTER_ERR_NOT_INIT)

// the table type
#define MTRC_F_R(ret, name, params, args, dv)  ret (*name) params;
#define MTRC_F_V(name, params, args)           void (*name) params;
typedef struct { MTRC_API_LIST(MTRC_F_R, MTRC_F_V) } mtrc_api_t;

// stubs: after the chosen plugin was unloaded, and as the "built-in" side of a
// plugin-only build
#define MTRC_D_R(ret, name, params, args, dv)  static ret mtrc_dead_##name params { return dv; }
#define MTRC_D_V(name, params, args)           static void mtrc_dead_##name params { }
MTRC_API_LIST(MTRC_D_R, MTRC_D_V)
#define MTRC_DT_R(ret, name, params, args, dv) mtrc_dead_##name,
#define MTRC_DT_V(name, params, args)          mtrc_dead_##name,
static const mtrc_api_t mtrc_api_dead = { MTRC_API_LIST(MTRC_DT_R, MTRC_DT_V) };

#ifdef USE_MATTER_C_PLUGIN_ONLY
// Matter only as the plugin (for boards without PSRAM, where the built-in lib's
// ~33 KB of .bss would sit next to the plugin's heap block): nothing references
// the lib, so the linker leaves its code and .bss out. Without the plugin every
// matter_* call answers like an unloaded plugin.
static const mtrc_api_t mtrc_api_builtin = { MTRC_API_LIST(MTRC_DT_R, MTRC_DT_V) };
#else
// built-in lib (the real matter_* — the redirecting macros come further down)
#define MTRC_B_R(ret, name, params, args, dv)  matter_##name,
#define MTRC_B_V(name, params, args)           matter_##name,
static const mtrc_api_t mtrc_api_builtin = { MTRC_API_LIST(MTRC_B_R, MTRC_B_V) };
#endif

static mtrc_api_t        mtrc_api_plugin;          // filled from the BLIB exports
static const mtrc_api_t *mtrc_api_sel = nullptr;   // latched at the first matter_init()

// Boards with XIP from PSRAM (ESP32-S3 with OPI PSRAM): see the "loop task only"
// block below. mtrc_api_marshal wraps mtrc_api_plugin there.
// -DMTRC_NO_MARSHAL switches it off (only for the counter-test that shows the crash)
#if defined(CONFIG_SPIRAM_XIP_FROM_PSRAM) && CONFIG_SPIRAM_XIP_FROM_PSRAM && !defined(MTRC_NO_MARSHAL)
#define MTRC_MARSHAL 1
static mtrc_api_t        mtrc_api_marshal;
#endif

// The table in use. Before the choice: the built-in lib (unchanged behaviour).
static const mtrc_api_t *mtrc_api(void) {
  bool plugin = mtrc_api_sel == &mtrc_api_plugin;
#ifdef MTRC_MARSHAL
  plugin = plugin || mtrc_api_sel == &mtrc_api_marshal;
#endif
  if (plugin && !tc_blib_lookup("matter_loop")) {
    mtrc_api_sel = &mtrc_api_dead;                 // plugin unloaded under us
    AddLog(LOG_LEVEL_ERROR, PSTR("MTR: Matter plugin unloaded - Matter off until restart"));
  }
  return mtrc_api_sel ? mtrc_api_sel : &mtrc_api_builtin;
}

// Fill the plugin table; true only if the plugin exports the complete API.
static bool mtrc_api_from_plugin(void) {
  TC_BLIB_REG_ENTRY *r;
#define MTRC_L_R(ret, name, params, args, dv) \
  if (!(r = tc_blib_lookup("matter_" #name))) return false; \
  mtrc_api_plugin.name = (ret (*) params)r->fn;
#define MTRC_L_V(name, params, args) \
  if (!(r = tc_blib_lookup("matter_" #name))) return false; \
  mtrc_api_plugin.name = (void (*) params)r->fn;
  MTRC_API_LIST(MTRC_L_R, MTRC_L_V)
  return true;
}

// The plugin gets the firmware's BearSSL primitives by pointer (Fork B, proven
// on .156 with the stage-2 self test: vtables work without EXEC_OFFSET).
static bool mtrc_bind_plugin_crypto(void) {
  TC_BLIB_REG_ENTRY *r = tc_blib_lookup("mtrc_crypto_bind");
  if (!r) return false;
  static mtrc_crypto_ops ops;
  ops.sha256_init   = br_sha256_init;     ops.sha256_update = br_sha256_update;
  ops.sha256_out    = br_sha256_out;
  ops.hmac_key_init = br_hmac_key_init;   ops.hmac_init     = br_hmac_init;
  ops.hmac_update   = br_hmac_update;     ops.hmac_out      = br_hmac_out;
  ops.hkdf_init     = br_hkdf_init;       ops.hkdf_inject   = br_hkdf_inject;
  ops.hkdf_flip     = br_hkdf_flip;       ops.hkdf_produce  = br_hkdf_produce;
  ops.ecdsa_sign_raw= br_ecdsa_i15_sign_raw; ops.ecdsa_vrfy_raw= br_ecdsa_i15_vrfy_raw;
  ops.aes_ct_ctrcbc_init = br_aes_ct_ctrcbc_init;
  ops.ccm_init      = br_ccm_init;        ops.ccm_reset     = br_ccm_reset;
  ops.ccm_aad_inject= br_ccm_aad_inject;  ops.ccm_flip      = br_ccm_flip;
  ops.ccm_run       = br_ccm_run;         ops.ccm_get_tag   = br_ccm_get_tag;
  ops.ccm_check_tag = br_ccm_check_tag;
  ops.ec_p256_m15   = &br_ec_p256_m15;    ops.sha256_vtable = &br_sha256_vtable;
  ((void (*)(const mtrc_crypto_ops *))r->fn)(&ops);
  return true;
}

// A MATTERF module in the plugin partition counts as present even if nobody
// has run `iniz` on it: plugins are not initialized at boot, and a TinyC script
// with autostart reaches matter_init() long before anyone could. Initialize it
// here, then its exports are registered.
static void mtrc_plugin_autoinit(void) {
  if (!plugins.ready) return;
  for (uint32_t i = 0; i < MAX_PLUGINS; i++) {
    if (!modules[i].mod_addr) continue;
    // header word by word: the partition is mapped on the instruction bus,
    // byte reads (strncmp on fm->name) fault on ESP32/S3
    uint32_t hdr[sizeof(FLASH_MODULE) / 4];
    const volatile uint32_t *lp = (const volatile uint32_t *)modules[i].mod_addr;
    for (uint32_t w = 0; w < sizeof(FLASH_MODULE) / 4; w++) { hdr[w] = lp[w]; }
    if (strncmp(((FLASH_MODULE *)hdr)->name, "MATTERF", 16) != 0) continue;
    if (!modules[i].flags.initialized) {
      AddLog(LOG_LEVEL_INFO, PSTR("MTR: initializing the Matter plugin (module %u)"), (unsigned)i + 1);
      Init_module(i);
    }
    return;
  }
}

// ---- Plugin code only on the loop task (boards with XIP from PSRAM) ----------
// With CONFIG_SPIRAM_XIP_FROM_PSRAM the firmware runs from a copy in PSRAM, so
// ESP-IDF keeps the cache on and the other tasks running while flash is written
// (docs: External RAM -> "Execute In Place (XiP) from PSRAM"). The plugin
// partition, however, is still executed from flash: a task that has to fetch
// plugin code from flash while another task writes it gets garbage. On .39
// (28.09.2026) the AsyncUDP task entered matter_udp_rx while the loop task saved
// the new fabric: IllegalInstruction on its first instruction, /mtr_fab left
// empty. Code already in the cache keeps running, which is why plugins that
// stay in a hot loop (audio) do not show it.
// Tasmota writes flash from the loop task, so the plugin is entered only there:
//  - datagrams from the AsyncUDP task are copied into a ring and handed over
//    in mtrc_main_pump() (FUNC_LOOP, before matter_loop);
//  - set_attr_uint / set_attr_scaled / queue_event from another task (TinyC
//    VM task, TaskLoop) are queued and applied there; they answer MATTER_OK
//    (a caller like TaskLoop holds its VM mutex, waiting could deadlock);
//  - any other call from another task waits until the loop task ran it.
#ifdef MTRC_MARSHAL
#include <functional>
extern TaskHandle_t loopTaskHandle;                // arduino-esp32 main.cpp
void *special_malloc(uint32_t size);

static inline bool mtrc_foreign(void) {
  return loopTaskHandle && xTaskGetCurrentTaskHandle() != loopTaskHandle;
}

static portMUX_TYPE mtrc_mux = portMUX_INITIALIZER_UNLOCKED;

// datagrams: one producer (AsyncUDP task), one consumer (loop task)
// 32 slots in PSRAM (~42 KB): the controller acks every chunk of a report at
// once, and those bursts arrive while the loop task is busy sending - with 8
// slots .39 dropped ~1 datagram/s at only ~3 datagrams/s (28.09.2026)
#define MTRC_HQ_N 32
// the plugin's own rx ring (MTRC_RX_QUEUE 8 in matter_c_c.h) takes 7; handing
// over more per loop pass made the plugin drop them silently
#define MTRC_HQ_PER_PASS 7
typedef struct { uint8_t ip6[16]; uint16_t port; uint16_t len; uint8_t buf[1280]; } mtrc_hq_pkt;
static mtrc_hq_pkt      *mtrc_hq = nullptr;
static volatile uint8_t  mtrc_hq_head = 0, mtrc_hq_tail = 0;
static volatile uint32_t mtrc_hq_drops = 0;

static void mtrc_mx_udp_rx(const uint8_t *ip6, uint16_t port, const void *buf, size_t len) {
  if (!mtrc_foreign()) { mtrc_api_plugin.udp_rx(ip6, port, buf, len); return; }
  if (!mtrc_hq || len == 0 || len > sizeof(mtrc_hq[0].buf)) { mtrc_hq_drops++; return; }
  uint8_t nh = (uint8_t)((mtrc_hq_head + 1) % MTRC_HQ_N);
  if (nh == mtrc_hq_tail) { mtrc_hq_drops++; return; }   // full: drop, MRP resends
  mtrc_hq_pkt *p = &mtrc_hq[mtrc_hq_head];
  if (ip6) memcpy(p->ip6, ip6, 16); else memset(p->ip6, 0, 16);
  p->port = port; p->len = (uint16_t)len;
  memcpy(p->buf, buf, len);
  __sync_synchronize();                            // fields before the index
  mtrc_hq_head = nh;
}

// attribute updates from other tasks: several producers, applied in order
enum { MTRC_DQ_UINT = 1, MTRC_DQ_SCALED, MTRC_DQ_EVENT };
typedef struct {
  uint8_t  op;
  uint16_t ep;
  uint32_t cl, at;                                 // at = event id for MTRC_DQ_EVENT
  uint64_t u;
  float    f;
  int32_t  sc, a, b;
} mtrc_dq_cmd;
#define MTRC_DQ_N 32
static mtrc_dq_cmd mtrc_dq[MTRC_DQ_N];
static uint8_t     mtrc_dq_head = 0, mtrc_dq_tail = 0;
static volatile uint32_t mtrc_dq_drops = 0;

static matter_err_t mtrc_dq_push(const mtrc_dq_cmd &c) {
  bool ok;
  portENTER_CRITICAL(&mtrc_mux);
  uint8_t nh = (uint8_t)((mtrc_dq_head + 1) % MTRC_DQ_N);
  ok = nh != mtrc_dq_tail;
  if (ok) { mtrc_dq[mtrc_dq_head] = c; mtrc_dq_head = nh; }
  portEXIT_CRITICAL(&mtrc_mux);
  if (!ok) mtrc_dq_drops++;
  return MATTER_OK;
}
static matter_err_t mtrc_mx_set_attr_uint(uint16_t ep, uint32_t cl, uint32_t at, uint64_t v) {
  if (!mtrc_foreign()) return mtrc_api_plugin.set_attr_uint(ep, cl, at, v);
  mtrc_dq_cmd c = {}; c.op = MTRC_DQ_UINT; c.ep = ep; c.cl = cl; c.at = at; c.u = v;
  return mtrc_dq_push(c);
}
static matter_err_t mtrc_mx_set_attr_scaled(uint16_t ep, uint32_t cl, uint32_t at, float f, int32_t sc) {
  if (!mtrc_foreign()) return mtrc_api_plugin.set_attr_scaled(ep, cl, at, f, sc);
  mtrc_dq_cmd c = {}; c.op = MTRC_DQ_SCALED; c.ep = ep; c.cl = cl; c.at = at; c.f = f; c.sc = sc;
  return mtrc_dq_push(c);
}
static matter_err_t mtrc_mx_queue_event(uint16_t ep, uint32_t cl, uint32_t ev, int32_t a, int32_t b) {
  if (!mtrc_foreign()) return mtrc_api_plugin.queue_event(ep, cl, ev, a, b);
  mtrc_dq_cmd c = {}; c.op = MTRC_DQ_EVENT; c.ep = ep; c.cl = cl; c.at = ev; c.a = a; c.b = b;
  return mtrc_dq_push(c);
}

// everything else from another task: run it on the loop task and wait
static const std::function<void()> *mtrc_job = nullptr;
static volatile uint8_t  mtrc_job_state = 0;       // 0 idle, 1 queued, 2 running
static SemaphoreHandle_t mtrc_job_lock = nullptr, mtrc_job_done = nullptr;
static volatile uint32_t mtrc_job_timeouts = 0;
static const char *volatile mtrc_job_timeout_name = nullptr;   // last one, for the log

static bool mtrc_on_loop(const char *name, const std::function<void()> &f) {
  if (!mtrc_job_lock) return false;
  xSemaphoreTake(mtrc_job_lock, portMAX_DELAY);    // one caller at a time
  xSemaphoreTake(mtrc_job_done, 0);                // drop a stale signal
  portENTER_CRITICAL(&mtrc_mux);
  mtrc_job = &f; mtrc_job_state = 1;
  portEXIT_CRITICAL(&mtrc_mux);
  bool ok = true;
  if (xSemaphoreTake(mtrc_job_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
    // the loop task may be blocked on something this task holds (a TinyC VM
    // mutex): withdraw the job, unless it already runs - it uses our stack
    portENTER_CRITICAL(&mtrc_mux);
    bool running = mtrc_job_state == 2;
    if (!running) { mtrc_job = nullptr; mtrc_job_state = 0; }
    portEXIT_CRITICAL(&mtrc_mux);
    if (running) { xSemaphoreTake(mtrc_job_done, portMAX_DELAY); }
    else { ok = false; mtrc_job_timeouts++; mtrc_job_timeout_name = name; }
  }
  xSemaphoreGive(mtrc_job_lock);
  return ok;
}

#define MTRC_W_R(ret, name, params, args, dv) \
  static ret mtrc_mw_##name params { \
    if (!mtrc_foreign()) return mtrc_api_plugin.name args; \
    ret r = dv; \
    if (!mtrc_on_loop(#name, [&] { r = mtrc_api_plugin.name args; })) return dv; \
    return r; \
  }
#define MTRC_W_V(name, params, args) \
  static void mtrc_mw_##name params { \
    if (!mtrc_foreign()) { mtrc_api_plugin.name args; return; } \
    mtrc_on_loop(#name, [&] { mtrc_api_plugin.name args; }); \
  }
MTRC_API_LIST(MTRC_W_R, MTRC_W_V)

static bool mtrc_marshal_sync(void) {
  if (!mtrc_job_lock) mtrc_job_lock = xSemaphoreCreateMutex();
  if (!mtrc_job_done) mtrc_job_done = xSemaphoreCreateBinary();
  return mtrc_job_lock && mtrc_job_done;
}

// the plugin was chosen: wrap it (runs on the loop task, see mtrc_select_and_init)
static bool mtrc_marshal_setup(void) {
  if (!mtrc_hq) mtrc_hq = (mtrc_hq_pkt *)special_malloc(MTRC_HQ_N * sizeof(mtrc_hq_pkt));
  if (!mtrc_hq || !mtrc_marshal_sync()) return false;
#define MTRC_WT_R(ret, name, params, args, dv) mtrc_api_marshal.name = mtrc_mw_##name;
#define MTRC_WT_V(name, params, args)          mtrc_api_marshal.name = mtrc_mw_##name;
  MTRC_API_LIST(MTRC_WT_R, MTRC_WT_V)
  mtrc_api_marshal.udp_rx          = mtrc_mx_udp_rx;
  mtrc_api_marshal.set_attr_uint   = mtrc_mx_set_attr_uint;
  mtrc_api_marshal.set_attr_scaled = mtrc_mx_set_attr_scaled;
  mtrc_api_marshal.queue_event     = mtrc_mx_queue_event;
  return true;
}

// Run a call another task is waiting for (loop task only). Also used while
// the loop task waits for a TinyC VM mutex (tc_vm_lock): the VM task holding it
// may be the one waiting here, e.g. a script's main() doing matterAdd() while
// a MatterInvoke is being delivered - without this both waited 2 s (seen at
// startup on .39: "2 calls timed out").
static void mtrc_run_job(void) {
  const std::function<void()> *j = nullptr;
  portENTER_CRITICAL(&mtrc_mux);
  if (mtrc_job_state == 1) { j = mtrc_job; mtrc_job_state = 2; }
  portEXIT_CRITICAL(&mtrc_mux);
  if (!j) return;
  (*j)();
  portENTER_CRITICAL(&mtrc_mux);
  mtrc_job = nullptr; mtrc_job_state = 0;
  portEXIT_CRITICAL(&mtrc_mux);
  xSemaphoreGive(mtrc_job_done);
}

// FUNC_LOOP, before matter_loop(): hand over what the other tasks delivered
static void mtrc_main_pump(void) {
  for (int n = 0; n < MTRC_HQ_PER_PASS && mtrc_api_sel == &mtrc_api_marshal && mtrc_hq_tail != mtrc_hq_head; n++) {
    mtrc_hq_pkt *p = &mtrc_hq[mtrc_hq_tail];
    mtrc_api_plugin.udp_rx(p->ip6, p->port, p->buf, p->len);
    __sync_synchronize();
    mtrc_hq_tail = (uint8_t)((mtrc_hq_tail + 1) % MTRC_HQ_N);
  }
  for (;;) {                                       // empty unless the plugin is in use
    mtrc_dq_cmd c;
    bool have = false;
    portENTER_CRITICAL(&mtrc_mux);
    if (mtrc_dq_tail != mtrc_dq_head) {
      c = mtrc_dq[mtrc_dq_tail]; mtrc_dq_tail = (uint8_t)((mtrc_dq_tail + 1) % MTRC_DQ_N); have = true;
    }
    portEXIT_CRITICAL(&mtrc_mux);
    if (!have || mtrc_api_sel != &mtrc_api_marshal) break;
    switch (c.op) {
      case MTRC_DQ_UINT:   mtrc_api_plugin.set_attr_uint(c.ep, c.cl, c.at, c.u); break;
      case MTRC_DQ_SCALED: mtrc_api_plugin.set_attr_scaled(c.ep, c.cl, c.at, c.f, c.sc); break;
      case MTRC_DQ_EVENT:  mtrc_api_plugin.queue_event(c.ep, c.cl, c.at, c.a, c.b); break;
    }
  }
  mtrc_run_job();                                  // also before the choice: matter_init()
  // report losses, at most every 60 s
  static uint32_t last_ms = 0, last_sum = 0;
  uint32_t sum = mtrc_hq_drops + mtrc_dq_drops + mtrc_job_timeouts;
  if (sum != last_sum && (uint32_t)(millis() - last_ms) >= 60000) {
    const char *tn = mtrc_job_timeout_name;
    AddLog(LOG_LEVEL_INFO, PSTR("MTR: plugin hand-over: %u datagrams dropped, %u updates dropped, %u calls timed out (last: %s)"),
           (unsigned)mtrc_hq_drops, (unsigned)mtrc_dq_drops, (unsigned)mtrc_job_timeouts, tn ? tn : "-");
    last_sum = sum; last_ms = millis();
  }
}
#else
static inline void mtrc_main_pump(void) {}
#endif

// matter_init() with the one-time choice in front of it.
static matter_err_t mtrc_select_and_init_here(const matter_port_t *p, const matter_config_t *c) {
  if (!mtrc_api_sel) {
    mtrc_plugin_autoinit();
    if (mtrc_api_from_plugin() && mtrc_bind_plugin_crypto()) {
      mtrc_api_sel = &mtrc_api_plugin;
#ifdef MTRC_MARSHAL
      if (mtrc_marshal_setup()) {
        mtrc_api_sel = &mtrc_api_marshal;
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("MTR: no memory for the plugin hand-over - calls from other tasks run the plugin directly"));
      }
#endif
      AddLog(LOG_LEVEL_INFO, PSTR("MTR: using the Matter plugin (MATTERF)"));
    } else {
      mtrc_api_sel = &mtrc_api_builtin;
#ifdef USE_MATTER_C_PLUGIN_ONLY
      AddLog(LOG_LEVEL_ERROR, PSTR("MTR: no Matter plugin (MATTERF) - this firmware has no built-in Matter"));
#else
      AddLog(LOG_LEVEL_INFO, PSTR("MTR: using the built-in Matter"));
#endif
    }
  }
  return mtrc_api()->init(p, c);
}

static matter_err_t mtrc_select_and_init(const matter_port_t *p, const matter_config_t *c) {
#ifdef MTRC_MARSHAL
  // the first matterXxx() of a script may come from its VM task: the choice
  // initializes the plugin, so it runs on the loop task too
  if (mtrc_foreign() && mtrc_marshal_sync()) {
    matter_err_t r = MATTER_ERR_NOT_INIT;
    mtrc_on_loop("init", [&] { r = mtrc_select_and_init_here(p, c); });
    return r;
  }
#endif
  return mtrc_select_and_init_here(p, c);
}

// Redirect every matter_* call below this point.
#define matter_init(p, c)                          mtrc_select_and_init(p, c)
#define matter_udp_rx(...)                         mtrc_api()->udp_rx(__VA_ARGS__)
#define matter_start()                             mtrc_api()->start()
#define matter_loop()                              mtrc_api()->loop()
#define matter_qr_uri()                            mtrc_api()->qr_uri()
#define matter_qr_dark(...)                        mtrc_api()->qr_dark(__VA_ARGS__)
#define matter_qr_size()                           mtrc_api()->qr_size()
#define matter_factory_reset()                     mtrc_api()->factory_reset()
#define matter_set_commissionable(...)             mtrc_api()->set_commissionable(__VA_ARGS__)
#define matter_open_commissioning_window()         mtrc_api()->open_commissioning_window()
#define matter_manual_code()                       mtrc_api()->manual_code()
#define matter_set_label(...)                      mtrc_api()->set_label(__VA_ARGS__)
#define matter_set_attr_uint(...)                  mtrc_api()->set_attr_uint(__VA_ARGS__)
#define matter_set_attr_scaled(...)                mtrc_api()->set_attr_scaled(__VA_ARGS__)
#define matter_reset_model()                       mtrc_api()->reset_model()
#define matter_queue_event(...)                    mtrc_api()->queue_event(__VA_ARGS__)
#define matter_get_attr_uint(...)                  mtrc_api()->get_attr_uint(__VA_ARGS__)
#define matter_add_endpoint(...)                   mtrc_api()->add_endpoint(__VA_ARGS__)
#define matter_add_cluster(...)                    mtrc_api()->add_cluster(__VA_ARGS__)
#define matter_add_attr(...)                       mtrc_api()->add_attr(__VA_ARGS__)

#endif // XDRV_124_MATTER_DISPATCH_H
