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

#endif  // USE_TINYC_BLE
