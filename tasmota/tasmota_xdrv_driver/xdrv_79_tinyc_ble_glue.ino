/*
  xdrv_79_tinyc_ble_glue.ino - bridge TinyC's BLE syscalls to the xdrv_79 common-BLE driver

  Why this file exists, separate from the TinyC VM:
  Tasmota concatenates all xdrv_*.ino into one translation unit in LEXICOGRAPHIC filename
  order. "xdrv_124_tinyc..." sorts BEFORE "xdrv_79_esp32_ble..." ('1' < '7'), so the TinyC VM
  (xdrv_124) is compiled before the BLE_ESP32 namespace is declared and therefore cannot
  reference BLE_ESP32 types at all (and a forward declaration would clash with xdrv_79's later
  definition in the same TU). This glue file is named to sort AFTER xdrv_79_esp32_ble.ino
  ("esp32" < "tinyc"), so BLE_ESP32 IS visible here. It touches the BLE_ESP32 advert struct and
  GATT operation struct, and exposes only plain-C functions (tc_ble_*) to xdrv_124 (declared in
  xdrv_124_tinyc_vm.h, earlier in the same TU). No BLE_ESP32 type ever crosses into xdrv_124.

  Threading: the advert callback runs on the BLE/NimBLE task -> tc_ble_push() (ring, never the VM).
  The GATT op completion callback runs on the MAIN task -> fills a result buffer and clears the
  busy flag LAST (publish-after-fill). The VM polls bleDone()/bleResult() on its own task. So no
  BLE callback ever blocks on vm_mutex (honors the spawnTask/vm_mutex + httpGet concurrency lessons).
*/

#if defined(ESP32) && defined(USE_TINYC_BLE)

// ⚠️⚠️ A column-0 function here whose parameters name an UNQUALIFIED NimBLE type needs
// a hand-written forward declaration, or it breaks the build of EVERY environment.
//
// (Deliberately line comments, not a block comment: this text has to talk about
// pointer types, and a stray "* /" written without the space would close a block
// comment early. That cost a build round on 2026-08-05.)
//
// PlatformIO concatenates all .ino files into one .cpp and auto-generates prototypes
// for the functions it recognises, hoisting them ALL to the position of the very first
// function definition in the combined file — inside tasmota.ino, thousands of lines
// ABOVE any NimBLE #include. A generated prototype naming a NimBLE class there cannot
// compile:
//
//     tasmota.ino:4363: error: variable or field 'tc_spp_notify_cb' declared void
//     tasmota.ino:4363: error: 'NimBLERemoteCharacteristic' was not declared in this scope
//
// The scanner (platformio/builder/tools/pioino.py, PROTOTYPE_RE) runs on RAW TEXT and
// ignores #ifdef — so this fires even in builds that never define USE_TINYC_BLE.
// Three things make it skip a function; all three are in use in this tree:
//
//  1. An explicit forward declaration of the same text somewhere in the file. pioino
//     drops any prototype whose text it already saw ending in ';'. This is Tasmota's
//     normal idiom — xdrv_79_esp32_ble.ino:382 hand-declares BLEGenNotifyCB(
//     NimBLERemoteCharacteristic*, ...) for exactly this reason. ⚠️ The match is on
//     the declaration TEXT, so a renamed parameter or different spacing silently
//     re-enables the hoist.
//  2. A ':' anywhere in the parameter list — the argument pattern has no colon in its
//     character class. That is the only reason tc_ble_adv_cb(
//     BLE_ESP32::ble_advertisment_t*) and tc_ble_gatt_done_cb(BLE_ESP32::
//     generic_sensor_t*) below have always been safe. Luck, not design.
//  3. Not being at column 0 — PROTOTYPE_RE is anchored ^ with re.M, so class members
//     and lambdas are never scanned.
//
// The BLE-SPP code below takes route 3, because it is the only one that cannot be
// broken later by an innocent-looking edit: the NimBLE-typed notify callback is a
// LAMBDA at its call site, delegating to tc_spp_push(), whose parameters are
// primitive and whose hoisted prototype is therefore harmless. Every other free
// function here takes only primitive parameter types for the same reason.

// ── Scan: advert sink (BLE task) ──────────────────────────────────────────────
static int tc_ble_adv_cb(BLE_ESP32::ble_advertisment_t *p) {
  if (!p) { return 0; }
  uint8_t mfg[TC_BLE_MFGLEN];
  int mfglen = 0;
  if (p->advertisedDevice && p->advertisedDevice->haveManufacturerData()) {
    std::string m = p->advertisedDevice->getManufacturerData();
    mfglen = (m.size() > (size_t)TC_BLE_MFGLEN) ? TC_BLE_MFGLEN : (int)m.size();
    memcpy(mfg, m.data(), mfglen);
  }
  tc_ble_push(p->addr, p->addrtype, p->RSSI, p->name, mfg, mfglen);
  return 0;  // let other consumers (MI32/iBeacon) see it too
}

void tc_ble_glue_register(void) {
  BLE_ESP32::BLEEnableUnsaved = 1;  // enable BLE at runtime so scripts work without SetOption115
  BLE_ESP32::registerForAdvertismentCallbacks("TinyC", tc_ble_adv_cb);
}

// ── GATT client: one in-flight transaction ───────────────────────────────────
static struct {
  volatile uint8_t busy;                  // 1 = op queued/running
  volatile int8_t  state;                 // 0 pending, 1 done, <0 = GEN_STATE_FAILED_*
  uint8_t          len;
  uint8_t          data[MAX_BLE_DATA_LEN_TC];
} tc_gatt_res = {};

// Runs on the MAIN task (BLE_ESP32 delivers completions there). Copy out, then clear busy LAST.
static int tc_ble_gatt_done_cb(BLE_ESP32::generic_sensor_t *op) {
  int n = 0;
  if (op->notifylen) {
    n = (op->notifylen > MAX_BLE_DATA_LEN_TC) ? MAX_BLE_DATA_LEN_TC : op->notifylen;
    memcpy(tc_gatt_res.data, op->dataNotify, n);
  } else if (op->readlen) {
    n = (op->readlen > MAX_BLE_DATA_LEN_TC) ? MAX_BLE_DATA_LEN_TC : op->readlen;
    memcpy(tc_gatt_res.data, op->dataRead, n);
  }
  tc_gatt_res.len   = (uint8_t)n;
  tc_gatt_res.state = (op->state < 0) ? (int8_t)op->state : 1;  // <0 fail, else success
  tc_gatt_res.busy  = 0;                                        // publish-last
  return 1;  // consume — don't auto-post to MQTT
}

int tc_ble_gatt_start(const uint8_t *mac, int addrtype, int svc, int chr, int notify,
                      const uint8_t *wbuf, int wlen) {
  if (tc_gatt_res.busy) { return -1; }                          // one transaction at a time
  BLE_ESP32::generic_sensor_t *op = nullptr;
  if (!BLE_ESP32::newOperation(&op)) { return -2; }
  op->addr = NimBLEAddress((uint8_t *)mac, (uint8_t)addrtype);
  if (svc)    { op->serviceUUID = NimBLEUUID((uint16_t)svc); }
  if (chr)    { op->characteristicUUID = NimBLEUUID((uint16_t)chr); }
  if (notify) { op->notificationCharacteristicUUID = NimBLEUUID((uint16_t)notify); }
  if (wbuf && wlen > 0) {
    if (wlen > MAX_BLE_DATA_LEN) { wlen = MAX_BLE_DATA_LEN; }
    op->writelen = (uint8_t)wlen;
    memcpy(op->dataToWrite, wbuf, wlen);
  }
  op->completecallback = (void *)tc_ble_gatt_done_cb;
  op->context = (void *)0;
  tc_gatt_res.len = 0; tc_gatt_res.state = 0; tc_gatt_res.busy = 1;  // prime before queueing
  if (!BLE_ESP32::extQueueOperation(&op)) {
    BLE_ESP32::freeOperation(&op);
    tc_gatt_res.busy = 0;
    return -3;
  }
  return 1;
}

int tc_ble_gatt_poll(void) {
  if (tc_gatt_res.busy)        { return 0; }                    // still running
  if (tc_gatt_res.state < 0)   { return tc_gatt_res.state; }    // failed (GEN_STATE_*)
  if (tc_gatt_res.state == 1)  { return tc_gatt_res.len ? tc_gatt_res.len : 1; }  // done
  return 0;
}

int tc_ble_gatt_copy(uint8_t *out, int max) {
  int n = tc_gatt_res.len;
  if (n > max) { n = max; }
  if (n > 0) { memcpy(out, tc_gatt_res.data, n); }
  return n;
}

// ── BLE "SPP": a PERSISTENT GATT client for continuous serial-style traffic ─────
// The client above connects, does ONE op, and disconnects (BLETaskRunTaskDoneOperation
// in xdrv_79_esp32_ble.ino calls pClient->disconnect() unconditionally after every
// operation) — right for a device that wakes, reports, and sleeps, wrong for a stream:
// a BlueRadios/Nordic-UART-style peripheral streaming continuously would lose the link
// before a second notify could ever arrive. So this owns a SEPARATE NimBLEClient with
// its own connect/subscribe/write/close and its own notify ring buffer — it never
// touches BLE_ESP32's op queue (currentOperations/queuedOperations) above, so the
// existing scan / one-shot client / MI32 / EQ3 consumers are unaffected.
//
// UUIDs are STRING literals here (16-bit "180a" or full 128-bit), unlike the one-shot
// client's int16-only svc/chr above — a proprietary UART-style service is essentially
// always a 128-bit UUID, which int16 cannot address. bleGattDump() (further below) is
// the one-shot companion for finding out what those UUIDs even are on an unknown device.
//
// ⭐ VERIFIED on real hardware 2026-08-05 on .39 (ESP32-S3) against gemu's ECG device with a BlueRadios dual module (MAC EC:FE:7E:10:E1:EF, BRSP profile): connect, subscribe BRSP_TX, set data mode, write "VS\r" -- the reply came back as 48 bytes in four notification chunks ("0252-56-2097151:AA000000:def0.def:...") and bleSppState() still reported 1 AFTERWARDS. That is exactly what the one-shot client cannot do: it would have disconnected after the reply.
// Still probe an UNKNOWN device with bleGattDump() first — a proprietary UUID has no
// datasheet lookup, and connecting needs a far better link than passive advert
// reception: a peer at -88..-94 dBm refused every connect while the one at -63 dBm
// worked first try.
// Runs a SECOND simultaneous NimBLE connection alongside whatever BLE_ESP32 itself is
// doing (background scan, a one-shot op) — if that ever fails with a connection-slot
// error, raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS (nimconfig.h; commonly defaults to 3).
static NimBLEClient         *tc_spp_client  = nullptr;
static NimBLERemoteService  *tc_spp_service = nullptr;   // valid only while connected

#define TC_BLE_SPP_RING 512   // bytes queued between TaskLoop polls

static struct {
  uint8_t  mac[6];
  uint8_t  addrtype;
  char     svc[40];                 // service UUID, string ("180a" or full 128-bit)
  uint8_t  ring[TC_BLE_SPP_RING];
  volatile uint16_t head;           // producer: NimBLE host task (notify callback)
  volatile uint16_t tail;           // consumer: VM task (bleSppRead/bleSppAvailable)
  portMUX_TYPE mux;
} tc_spp = { .mux = portMUX_INITIALIZER_UNLOCKED };

// Runs on the NimBLE host task (reached from the lambda in tc_ble_spp_sub()). Keep it
// tiny and VM-free, same rule as tc_ble_push().
// ⚠️ PRIMITIVE PARAMETERS ON PURPOSE — see the prototype warning at the top of this
// file. Naming a NimBLE type here instead breaks every build in the project.
static void tc_spp_push(const uint8_t *data, int len) {
  if (!data || len <= 0) { return; }
  portENTER_CRITICAL(&tc_spp.mux);
  for (int i = 0; i < len; i++) {
    uint16_t nh = (tc_spp.head + 1) % TC_BLE_SPP_RING;
    if (nh == tc_spp.tail) { break; }        // ring full — drop the rest of this burst
    tc_spp.ring[tc_spp.head] = data[i];
    tc_spp.head = nh;
  }
  portEXIT_CRITICAL(&tc_spp.mux);
}

// onDisconnect runs on the NimBLE host task too — the service/characteristic pointers
// die with the link, so drop the cached service pointer immediately.
class TcSppClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient*, int) override { tc_spp_service = nullptr; }
};
static TcSppClientCB tc_spp_client_cb;

void tc_ble_spp_target(const uint8_t *mac, int addrtype, const char *svc) {
  if (!mac) { return; }
  memcpy(tc_spp.mac, mac, 6);
  tc_spp.addrtype = (uint8_t)addrtype;
  strlcpy(tc_spp.svc, svc ? svc : "", sizeof(tc_spp.svc));
}

// BLOCKS on NimBLE's own connect timeout (a few seconds on failure) — call from
// TaskLoop() only, same rule as sppConnect(). Reuses one NimBLEClient object across
// reconnects instead of create/delete each time, to avoid churning NimBLE's small
// client pool.
int tc_ble_spp_connect(void) {
  if (!tc_spp.svc[0]) { return 0; }
  BLE_ESP32::BLEEnableUnsaved = 1;    // request NimBLE up, same as the other BLE entry points
  // ⚠️⚠️ CALL NOTHING IN NIMBLE WHILE THE HOST IS NOT UP. BLEEnableUnsaved only ASKS
  // for BLE to come up -- it does not wait. But every host function takes
  // ble_hs_lock(), and that mutex does NOT exist before then: the access dies in
  // ble_npl_mutex_pend with exception 28 / LoadProhibited.
  // Decoded with the matching ELF (2026-08-06) after guessing wrong three times --
  // first "wrong task", then the wrong function. All the crash addresses were in the
  // same locking logic in ble_hs.c.
  // tc_ble_srv_loop() further down has always done it right and waits exactly so.
  // The caller simply tries again; next time round the stack is up.
  if (!NimBLEDevice::isInitialized()) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("BLE: SPP connect deferred -- NimBLE not up yet"));
    return 0;
  }
  // Ask for a larger MTU -- ONLY NOW, because that takes the host lock too.
  NimBLEDevice::setMTU(247);
  if (!tc_spp_client) {
    tc_spp_client = NimBLEDevice::createClient();
    if (!tc_spp_client) { return 0; }
    tc_spp_client->setClientCallbacks(&tc_spp_client_cb, false);
  }
  if (tc_spp_client->isConnected()) {
    tc_spp_client->disconnect();
    // ⚠️ WAIT for the teardown. disconnect() is asynchronous, and as long as the old
    // connection still stands in the host, connect() refuses the new one with no
    // error code (ble_gap_conn_find_by_addr finds it). Only poll our own client
    // object -- no call into the BLE stack from this task.
    for (int i = 0; i < 40 && tc_spp_client->isConnected(); i++) {
      vTaskDelay(pdMS_TO_TICKS(25));
    }
  }
  tc_spp_service = nullptr;
  // ⚠️⚠️ STOP THE RUNNING SCAN AT THE RADIO, or the connect loses the race.
  // BLE_ESP32 scans continuously and restarts on its own. NimBLEClient::connect()
  // does try to stop it, but on failure just returns false — with no error code, so
  // it looks like the peer's fault. Observed 2026-08-07: the peer advertised at
  // -58 dBm, BLE was healthy (25 scans, 12000 adverts), and EVERY attempt failed —
  // while the same connect had succeeded first try shortly after a reboot, i.e. with
  // the scanner still idle. TinyC's bleScanStop() does NOT help: it only clears
  // tc_ble.capturing and never touches the radio.
  // ⚠️ This sits deliberately AFTER the isInitialized() check above: called before the
  // host is up it grabs a non-existent mutex and crashes (Exception 28 — decoded with
  // the matching ELF; that is exactly how the first attempt at this failed).
  ble_gap_disc_cancel();
  vTaskDelay(pdMS_TO_TICKS(60));
  NimBLEAddress addr(tc_spp.mac, tc_spp.addrtype);
  const bool verbunden = tc_spp_client->connect(addr, true);
  // ⚠️ Re-allow scanning — but only by clearing the FLAG. Calling BLETaskStartScan()
  // belongs to the BLE task and crashes from here. Without this the BLE task keeps
  // believing its scan is running and never starts a new one: bleScan() then finds
  // nothing at all until the next reboot.
  BLE_ESP32::BLERunningScan = 0;
  if (!verbunden) {
    // Same reasoning as in tc_ble_gatt_dump(): the return code is the difference
    // between "move it closer" and "the address type is wrong".
    int rc = tc_spp_client->getLastError();
    AddLog(LOG_LEVEL_INFO, PSTR("BLE: SPP connect failed rc=%d %s"), rc,
           NimBLEUtils::returnCodeToString(rc));
    return 0;
  }
  tc_spp_service = tc_spp_client->getService(tc_spp.svc);
  if (!tc_spp_service) {
    AddLog(LOG_LEVEL_ERROR, PSTR("BLE: SPP service %s not found on peer"), tc_spp.svc);
    tc_spp_client->disconnect();
    return 0;
  }
  return 1;
}

int tc_ble_spp_state(void) {
  // ⚠️ The SERVICE POINTER belongs in the condition. onDisconnect() sets it to
  // nullptr, and without it tc_ble_spp_write() returns 0 straight away. Asking only
  // isConnected() here reported the state as "CONNECTED" while every write failed --
  // exactly that contradiction sat in the log on 2026-08-06 and dragged the debugging
  // out. A state that contradicts the behaviour is worse than no state at all.
  return (tc_spp_client && tc_spp_client->isConnected() && tc_spp_service) ? 1 : 0;
}

int tc_ble_spp_sub(const char *chruuid) {
  if (!tc_spp_service || !chruuid) { return 0; }
  NimBLERemoteCharacteristic *c = tc_spp_service->getCharacteristic(chruuid);
  if (!c) { return 0; }
  // ⚠️ LAMBDA, not a named function — see the prototype warning at the top of this
  // file. Indented, so the .ino prototype scanner (anchored at column 0) cannot see
  // it, and the NimBLE type in its signature never reaches tasmota.ino.
  auto cb = [](NimBLERemoteCharacteristic*, uint8_t *d, size_t l, bool) {
    tc_spp_push(d, (int)l);
  };
  if (c->canNotify())   { return c->subscribe(true,  cb, false) ? 1 : 0; }
  if (c->canIndicate()) { return c->subscribe(false, cb, false) ? 1 : 0; }
  return 0;
}

// Writes without disconnecting — the whole point of this being a separate connection
// from the one-shot client above. "response" (acked write) only when the characteristic
// doesn't support write-without-response, mirroring how most UART-TX characteristics
// are written in practice (unacked, for throughput).
int tc_ble_spp_write(const char *chruuid, const uint8_t *buf, int len) {
  if (!tc_spp_service || !chruuid || !buf || len <= 0) { return 0; }
  NimBLERemoteCharacteristic *c = tc_spp_service->getCharacteristic(chruuid);
  if (!c || !(c->canWrite() || c->canWriteNoResponse())) { return 0; }
  const bool response = !c->canWriteNoResponse();
  // ⚠️⚠️ SPLIT AT THE MTU. One ATT write carries at most MTU-3 bytes, and the default
  // ATT MTU is 23 -- i.e. 20 bytes. A single writeValue() beyond that just returns
  // false. That is why every short command worked (VS = 3 bytes answered, %9220 = 6
  // bytes acknowledged, a bare CR made the recorder BEEP) while the 29-byte Variograf
  // start command "!WFF...." silently did nothing: the write never left the ESP.
  // It cost a long session, because the calling script ignored the return value and
  // so the failure looked like the device refusing to start streaming.
  // A serial bridge does not care about write boundaries -- the module concatenates --
  // so chunking is the correct fix and works whatever MTU was negotiated.
  int mtu = tc_spp_client ? (int)tc_spp_client->getMTU() : 23;
  int chunk = mtu - 3;
  if (chunk < 1) { chunk = 20; }
  int off = 0;
  while (off < len) {
    int n = len - off;
    if (n > chunk) { n = chunk; }
    if (!c->writeValue(buf + off, (size_t)n, response)) { return 0; }
    off += n;
  }
  return 1;
}

int tc_ble_spp_available(void) {
  portENTER_CRITICAL(&tc_spp.mux);
  int n = (tc_spp.head - tc_spp.tail + TC_BLE_SPP_RING) % TC_BLE_SPP_RING;
  portEXIT_CRITICAL(&tc_spp.mux);
  return n;
}

int tc_ble_spp_read(uint8_t *out, int max) {
  if (!out || max <= 0) { return 0; }
  int n = 0;
  portENTER_CRITICAL(&tc_spp.mux);
  while (n < max && tc_spp.tail != tc_spp.head) {
    out[n++] = tc_spp.ring[tc_spp.tail];
    tc_spp.tail = (tc_spp.tail + 1) % TC_BLE_SPP_RING;
  }
  portEXIT_CRITICAL(&tc_spp.mux);
  return n;
}

void tc_ble_spp_close(void) {
  if (tc_spp_client && tc_spp_client->isConnected()) { tc_spp_client->disconnect(); }
  tc_spp_service = nullptr;
}

// One-shot: connect with a SHORT-LIVED client (not tc_spp_client — this must not disturb
// a persistent bleSpp* session that may already be open), list every service and
// characteristic with its R/W/N/I properties as text, disconnect, free the client.
// Run this FIRST against any device whose UUIDs are unknown — which for a proprietary
// UART-style profile is the normal case, there is no datasheet lookup for it.
int tc_ble_gatt_dump(const uint8_t *mac, int addrtype, char *out, int max) {
  if (!mac || !out || max <= 0) { return 0; }
  out[0] = 0;
  BLE_ESP32::BLEEnableUnsaved = 1;
  if (!NimBLEDevice::isInitialized()) {   // same reason as in tc_ble_spp_connect()
    strlcpy(out, "BLE stack not up yet -- try again", max);
    return (int)strlen(out);
  }
  NimBLEClient *c = NimBLEDevice::createClient();
  if (!c) { strlcpy(out, "no client slot free", max); return (int)strlen(out); }
  NimBLEAddress addr(mac, (uint8_t)addrtype);
  int len = 0;
  if (c->connect(addr, true)) {
    for (auto *svc : c->getServices(true)) {
      len += snprintf(out + len, (len < max) ? (max - len) : 0, "SVC %s\n",
                       svc->getUUID().toString().c_str());
      if (len >= max) { break; }
      for (auto *ch : svc->getCharacteristics(true)) {
        len += snprintf(out + len, (len < max) ? (max - len) : 0, "  CHR %s %s%s%s%s\n",
                         ch->getUUID().toString().c_str(),
                         ch->canRead() ? "R" : "", ch->canWrite() ? "W" : "",
                         ch->canWriteNoResponse() ? "w" : "",
                         ch->canNotify() ? "N" : (ch->canIndicate() ? "I" : ""));
        if (len >= max) { break; }
      }
      if (len >= max) { break; }
    }
    c->disconnect();
  } else {
    // ⚠️ Report WHY. A bare "connect failed" is worthless — it cannot tell a weak
    // link from a busy peer from a wrong address type, and those need opposite
    // fixes. getLastError() is the NimBLE host return code; the text is empty
    // unless NIMBLE_CPP_ENABLE_RETURN_CODE_TEXT is on, so the NUMBER is what to
    // read. Common ones: 13 = BLE_HS_ETIMEOUT (peer never answered — too far, off,
    // or not accepting), 7 = BLE_HS_ENOMEM, 2 = BLE_HS_EALREADY (already
    // connected), 6 = BLE_HS_ENOTCONN, 3 = BLE_HS_EINVAL (bad address/type),
    // 14 = BLE_HS_EDONE, 0x0D-ish HCI codes appear as 0x200+n.
    int rc = c->getLastError();
    len = snprintf(out, max, "connect failed rc=%d %s", rc, NimBLEUtils::returnCodeToString(rc));
  }
  NimBLEDevice::deleteClient(c);
  if (len >= max) { len = max - 1; }
  if (len < 0) { len = 0; }
  out[len] = 0;
  return len;
}

// ── GATT server (peripheral) — device-as-peripheral so a phone (central) connects ──
// NimBLE server build + setValue/notify run on the MAIN task via tc_ble_srv_loop()
// (hooked into xdrv_79 FUNC_EVERY_50_MSECOND); the VM bridge fns only set request
// flags + copy buffers. Incoming writes land in onWrite (NimBLE task) -> per-char
// buffer under a critical section; the VM drains them by polling (same idiom as scan).
#define TC_SRV_MAX_CHR   8
#define TC_SRV_VALLEN    MAX_BLE_DATA_LEN_TC

// NimBLE host C APIs used to quiesce GAP + reset the GATT into the configuring state
// before registering our service (scanner driver leaves the GATT in the started state).
extern "C" {
  int ble_gatts_reset(void);
  int ble_gap_adv_stop(void);
  int ble_gap_disc_cancel(void);
}

static portMUX_TYPE tc_srv_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t      s_srv_built_once = 0;   // GATT built once per boot; re-runs re-attach (reboot to reconfigure)

struct tc_srv_chr_t {
  char     uuid[40];
  uint16_t props;                  // NIMBLE_PROPERTY bits (mapped from BLE_READ/WRITE/NOTIFY)
  NimBLECharacteristic *chr;
  volatile uint8_t wnew;           // 1 = central wrote since the VM last read
  uint8_t  wlen;
  uint8_t  wdata[TC_SRV_VALLEN];   // incoming (central -> device)
  volatile uint8_t set_req;        // 1 = setValue pending, 2 = setValue + notify
  uint8_t  slen;
  uint8_t  sdata[TC_SRV_VALLEN];   // outgoing (device -> central)
};

static struct {
  volatile uint8_t start_req;
  volatile uint8_t stop_req;
  volatile uint8_t built;
  volatile uint8_t connected;
  char     name[32];
  char     svc[40];
  int      nchr;
  tc_srv_chr_t chr[TC_SRV_MAX_CHR];
  NimBLEServer  *server;
  NimBLEService *service;
} tc_srv = {};

// onConnect / onWrite run on the NimBLE host task.
class TcSrvServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*, NimBLEConnInfo&) override { tc_srv.connected = 1; }
  void onDisconnect(NimBLEServer*, NimBLEConnInfo&, int) override {
    tc_srv.connected = 0;
    if (tc_srv.built) { NimBLEDevice::getAdvertising()->start(); }   // re-advertise for reconnect
  }
};
class TcSrvCharCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo&) override {
    for (int i = 0; i < tc_srv.nchr; i++) {
      if (tc_srv.chr[i].chr != c) { continue; }
      NimBLEAttValue v = c->getValue();
      int n = v.length(); if (n > TC_SRV_VALLEN) { n = TC_SRV_VALLEN; }
      portENTER_CRITICAL(&tc_srv_mux);
      if (n > 0) { memcpy(tc_srv.chr[i].wdata, v.data(), n); }
      tc_srv.chr[i].wlen = (uint8_t)n;
      tc_srv.chr[i].wnew = 1;
      portEXIT_CRITICAL(&tc_srv_mux);
      break;
    }
  }
};
static TcSrvServerCB tc_srv_server_cb;
static TcSrvCharCB   tc_srv_char_cb;

void tc_ble_srv_init(const char *name) {
  BLE_ESP32::BLEEnableUnsaved = 1;                       // request NimBLE up (no scan needed)
  strlcpy(tc_srv.name, name ? name : "TinyC", sizeof(tc_srv.name));
  tc_srv.svc[0] = 0;
  tc_srv.nchr = 0;
  tc_srv.built = 0;
  tc_srv.start_req = 0;
}
void tc_ble_srv_service(const char *uuid) {
  strlcpy(tc_srv.svc, uuid ? uuid : "", sizeof(tc_srv.svc));
}
int tc_ble_srv_char(const char *uuid, int props) {
  if (tc_srv.built || tc_srv.nchr >= TC_SRV_MAX_CHR || !uuid) { return -1; }
  uint16_t np = 0;                                        // BLE_READ=1 | BLE_WRITE=2 | BLE_NOTIFY=4 -> NimBLE flags
  if (props & 1) { np |= NIMBLE_PROPERTY::READ; }
  if (props & 2) { np |= NIMBLE_PROPERTY::WRITE; }
  if (props & 4) { np |= NIMBLE_PROPERTY::NOTIFY; }
  int i = tc_srv.nchr++;
  strlcpy(tc_srv.chr[i].uuid, uuid, sizeof(tc_srv.chr[i].uuid));
  tc_srv.chr[i].props = np;
  tc_srv.chr[i].chr = nullptr;
  tc_srv.chr[i].wnew = 0; tc_srv.chr[i].wlen = 0; tc_srv.chr[i].set_req = 0;
  return i;
}
void tc_ble_srv_go(void)        { tc_srv.start_req = 1; }
int  tc_ble_srv_connected(void) { return tc_srv.connected ? 1 : 0; }
int  tc_ble_srv_written(int h)  {
  if (h < 0 || h >= tc_srv.nchr) { return 0; }
  return tc_srv.chr[h].wnew ? tc_srv.chr[h].wlen : 0;
}
int tc_ble_srv_read(int h, uint8_t *out, int max) {
  if (h < 0 || h >= tc_srv.nchr) { return 0; }
  int n;
  portENTER_CRITICAL(&tc_srv_mux);
  n = tc_srv.chr[h].wlen; if (n > max) { n = max; }
  if (n > 0) { memcpy(out, tc_srv.chr[h].wdata, n); }
  tc_srv.chr[h].wnew = 0;
  portEXIT_CRITICAL(&tc_srv_mux);
  return n;
}
void tc_ble_srv_set(int h, const uint8_t *buf, int len, int notify) {
  if (h < 0 || h >= tc_srv.nchr) { return; }
  if (len < 0) { len = 0; }
  if (len > TC_SRV_VALLEN) { len = TC_SRV_VALLEN; }
  if (buf && len > 0) { memcpy(tc_srv.chr[h].sdata, buf, len); }
  tc_srv.chr[h].slen = (uint8_t)len;
  tc_srv.chr[h].set_req = notify ? 2 : 1;                // applied on the main task (publish-last)
}
void tc_ble_srv_stop(void) { tc_srv.stop_req = 1; }

// MAIN task: build the server once NimBLE is up; apply pending setValue/notify.
void tc_ble_srv_loop(void) {
  if (tc_srv.stop_req) {
    tc_srv.stop_req = 0;
    if (tc_srv.built) {
      NimBLEDevice::getAdvertising()->stop();
      tc_srv.built = 0; tc_srv.connected = 0;
    }
    return;
  }
  if (tc_srv.start_req && !tc_srv.built) {
    if (!NimBLEDevice::isInitialized()) { return; }      // wait for BLE_ESP32 to bring NimBLE up
    if (!tc_srv.svc[0] || tc_srv.nchr == 0) { tc_srv.start_req = 0; return; }
    if (s_srv_built_once && tc_srv.service) {
      // Re-run without a reboot: rebuilding would create a DUPLICATE GATT service (NimBLE never
      // dedups). Re-attach the characteristic handles to the existing service so notify/set keep
      // working, and resume advertising (e.g. after bleServerStop). Reboot to change the layout.
      for (int i = 0; i < tc_srv.nchr; i++) {
        tc_srv.chr[i].chr = tc_srv.service->getCharacteristic(NimBLEUUID(tc_srv.chr[i].uuid));
      }
      NimBLEDevice::getAdvertising()->start();
      tc_srv.built = 1; tc_srv.start_req = 0;
      AddLog(LOG_LEVEL_INFO, PSTR("BLE: TinyC GATT server re-attached (reboot to change services)"));
      return;
    }
    // BLE_ESP32 already brought NimBLE up AND is running its background scan, so the GATT is not
    // in the "configuring" state -> ble_gatts_add_svcs() (in service->start) is rejected with
    // BLE_HS_EBUSY and the characteristics never register. Quiesce GAP (esp. the scan) so the
    // reset in createServer() succeeds, then build + register the service.
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    ble_gatts_reset();
    tc_srv.server = NimBLEDevice::createServer();   // re-inits the default GAP/GATT services
    tc_srv.server->setCallbacks(&tc_srv_server_cb);
    tc_srv.service = tc_srv.server->createService(NimBLEUUID(tc_srv.svc));
    for (int i = 0; i < tc_srv.nchr; i++) {
      NimBLECharacteristic *c = tc_srv.service->createCharacteristic(NimBLEUUID(tc_srv.chr[i].uuid), tc_srv.chr[i].props);
      c->setCallbacks(&tc_srv_char_cb);
      tc_srv.chr[i].chr = c;
    }
    bool svc_ok = tc_srv.service->start();
    NimBLEDevice::getServer()->start();             // ble_gatts_start before advertising
    NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
    adv->setName(tc_srv.name);
    adv->addServiceUUID(tc_srv.service->getUUID());
    adv->enableScanResponse(true);
    adv->start();
    tc_srv.built = 1; tc_srv.start_req = 0;
    s_srv_built_once = 1;
    AddLog(svc_ok ? LOG_LEVEL_INFO : LOG_LEVEL_ERROR,
           PSTR("BLE: TinyC GATT server '%s' %s (%d chars)"),
           tc_srv.name, svc_ok ? "advertising" : "registration FAILED", tc_srv.nchr);
    return;
  }
  if (!tc_srv.built) { return; }
  for (int i = 0; i < tc_srv.nchr; i++) {
    uint8_t req = tc_srv.chr[i].set_req;
    if (!req || !tc_srv.chr[i].chr) { continue; }
    tc_srv.chr[i].chr->setValue(tc_srv.chr[i].sdata, tc_srv.chr[i].slen);
    if (req == 2 && tc_srv.connected) { tc_srv.chr[i].chr->notify(); }
    tc_srv.chr[i].set_req = 0;
  }
}

#endif  // USE_TINYC_BLE
