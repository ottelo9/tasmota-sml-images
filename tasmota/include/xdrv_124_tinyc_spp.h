/*
  xdrv_124_tinyc_spp.h — Bluetooth Classic (RFCOMM/SPP) as a TinyC primitive.

  Provides a SERIAL link to any Bluetooth Classic device. The protocol on top is
  written in the TinyC script, not in the firmware, so the same primitive serves SMA
  inverters, OBD adapters, scales, receipt printers and anything else that speaks SPP
  -- and it can be changed without reflashing.

  ---------------------------------------------------------------------------------------
  ORIGINAL ESP32 ONLY.
  ---------------------------------------------------------------------------------------
  Bluetooth Classic (BR/EDR) exists only on the original ESP32 (D0WD and friends).
  S3, C3, C6 and P4 are BLE-only, so this whole section is compiled out there.

  Tasmota also ships an Arduino framework precompiled with NimBLE, which contains NO
  Classic headers at all. An environment using USE_TINYC_SPP must therefore have the
  framework rebuilt with Bluedroid (roughly 8 minutes) and needs the include paths
  added by hand:

      custom_sdkconfig = CONFIG_BT_ENABLED=y
                         CONFIG_BT_CONTROLLER_ENABLED=y
                         CONFIG_BT_BLUEDROID_ENABLED=y
                         '# CONFIG_BT_NIMBLE_ENABLED is not set'
                         CONFIG_BT_CLASSIC_ENABLED=y
                         CONFIG_BT_SPP_ENABLED=y
                         CONFIG_BTDM_CTRL_MODE_BR_EDR_ONLY=y
                         '# CONFIG_BTDM_CTRL_MODE_BLE_ONLY is not set'
                         '# CONFIG_BTDM_CTRL_MODE_BTDM is not set'
      build_flags      = ... -DUSE_TINYC_SPP
                         -I<packages>/framework-espidf/components/bt/host/bluedroid/api/include/api
                         -I<packages>/framework-espidf/components/bt/common/api/include/api
                         -I<packages>/framework-espidf/components/bt/include/esp32/include

  The rebuild refreshes the LIBRARIES but NOT the shipped include list
  (tools/esp32-arduino-libs/esp32/flags/includes stays behind and still names NimBLE).
  Without those three -I flags compilation dies on "esp_bt_main.h: No such file or
  directory" even though Bluedroid was built long ago. Costs about 510 KB of flash,
  and such a build has no NimBLE -- the two host stacks are mutually exclusive.

  The rebuild itself only happens when the hash in line 1 of <project>/sdkconfig.defaults
  ("# TASMOTA__<md5>") changes. If it stays the same, edited settings are ignored in
  SILENCE -- no "Replace:" lines in the build log. Force it by deleting that file and
  tools/esp32-arduino-libs/esp32/sdkconfig.

  ---------------------------------------------------------------------------------------
  READS DO NOT BLOCK.
  ---------------------------------------------------------------------------------------
  sppRead() returns whatever has arrived and comes back immediately. The script does
  its own waiting and stays in control. If the firmware waited instead, the VM would
  hang on the peer's timeouts -- exactly the failure that once wedged the main loop
  through httpGet/sendMail. Protocols with long waits belong on a worker (spawnTask),
  not in the main loop.

  The one exception is sppScan(), which waits out the requested inquiry time. Scanning
  is a rare, deliberate act, so simplicity wins there.

  ---------------------------------------------------------------------------------------
  EVERY STACK EVENT IS LOGGED, WITH ITS STATUS.
  ---------------------------------------------------------------------------------------
  A connect that never completes says nothing by itself -- out of range, wrong address,
  peer busy, authentication refused and "no such RFCOMM channel" all look identical from
  the script. So both callbacks name every event they see and print its status code.

  Read the log from the bottom up; the layers answer in this order:

      GAP ACL_CONN_CMPL   stat=0   the BASEBAND link stands -> radio, address and range
                                   are fine, and the peer accepted the page
      SPP CL_INIT         status=0 the RFCOMM connect request went out
      GAP AUTH_CMPL       stat=0   pairing done (only if the peer demands it)
      SPP OPEN            status=0 the channel is up -- only now does sppState() say 3

  Where the chain stops is the diagnosis. No ACL_CONN_CMPL at all means the peer never
  answered the page: too far away, switched off, or holding its single slot for someone
  else. ACL up but no SPP OPEN points at the RFCOMM channel number or at authentication.

  This build of Bluedroid has NO Secure Simple Pairing (CONFIG_BT_SSP_ENABLED is absent
  from the generated sdkconfig), which suits a BR/EDR 2.0 peer: pairing, if it happens at
  all, is legacy PIN. A fixed PIN is registered at bring-up so a PIN request is answered
  instead of silently timing out.
*/

#ifndef _XDRV_124_TINYC_SPP_H_
#define _XDRV_124_TINYC_SPP_H_

#ifdef USE_TINYC_SPP

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"

// ---------------------------------------------------------------------------------------
// KEEP THE BT CONTROLLER MEMORY. Without this the whole driver is dead on arrival.
//
// The Arduino core frees the entire BT controller memory during startup:
//
//     if (!btInUse()) { esp_bt_controller_mem_release(ESP_BT_MODE_BTDM); }   (esp32-hal-misc.c)
//
// btInUse() is weak and only reports true when Arduino's own BluetoothSerial/BLE
// libraries are linked. We talk to the IDF API directly, so it returned false, ~36 KB
// of controller memory were released, and every later esp_bt_controller_init() failed
// with ESP_ERR_INVALID_STATE (0x103) -- while esp_bt_controller_get_status() still
// cheerfully reported IDLE. That combination (status IDLE + init INVALID_STATE) is the
// fingerprint of released BT memory; nothing in the log said so.
//
// esp32-hal-bt.c names the sanctioned remedy: "Users can also provide their own strong
// btInUse() implementation." So here it is.
extern "C" bool btInUse(void) { return true; }

#define TC_SPP_RX_BUF     2048     // receive ring; DRAM is tight, do not raise without
                                   // measuring (Bluedroid Classic is a heavy eater)
#define TC_SPP_SCAN_MAX   16       // remembered inquiry results

// Legacy PIN answered on a PIN request. This Bluedroid build has no Secure Simple
// Pairing (CONFIG_BT_SSP_ENABLED is absent from the generated sdkconfig), so a peer that
// wants authentication gets the old 4-digit exchange. Without a registered PIN the
// request is simply never answered and the connect dies in a timeout with nothing in the
// log to say why.
#ifndef TC_SPP_PIN
#define TC_SPP_PIN        "0000"
#endif

// ⭐ PAGE TIMEOUT — how long the controller keeps calling a peer before giving up, in
// units of 0.625 ms. Bluedroid's default is 0x2000 = 5.12 s, and that is TOO SHORT for
// old BR/EDR 2.0 hardware.
//
// When you page a device you have never talked to, the controller knows neither its clock
// offset nor its page-scan repetition mode. A peer in mode R2 scans as rarely as every
// 2.56 s, and the specification then wants the page train repeated -- which alone eats
// the whole 5.12 s. The result is a connect that fails after EXACTLY 5.1 s, every single
// time, with HCI 0x04 PAGE TIMEOUT and no hint that the budget was the problem. Measured
// on the SMA inverter (2026-08-03): five attempts, five times 5.1 s to the millisecond.
//
// 0x7D00 = 32000 * 0.625 ms = 20 s. Valid range is 0x0016 ~ 0xFFFF (max ~41 s), and the
// IDF documentation explicitly suggests setting this before initiating a connection.
// ⚠️ A script's own connect timeout must exceed this, or it gives up before the
// controller does.
#ifndef TC_SPP_PAGE_TIMEOUT
#define TC_SPP_PAGE_TIMEOUT  0x7D00
#endif

enum TcSppState : uint8_t {
  TC_SPP_AUS = 0, TC_SPP_BEREIT = 1, TC_SPP_VERBINDET = 2, TC_SPP_OFFEN = 3
};

struct TcSppFund { uint8_t addr[6]; char name[24]; };

struct {
  volatile uint8_t  state = TC_SPP_AUS;
  volatile uint32_t handle = 0;
  volatile bool     verbinde_fehlgeschlagen = false;

  // Peer of the current connect attempt. Kept so ESP_SPP_DISCOVERY_COMP_EVT can issue
  // the actual connect once SDP has named the channel (the channel-0 path).
  uint8_t  ziel[6];
  volatile bool     ziel_gesetzt = false;

  // Ring buffer with ONE writer (the Bluedroid callback) and ONE reader (the VM).
  // Lock-free is safe as long as head and tail are each advanced by one side only.
  uint8_t  rx[TC_SPP_RX_BUF];
  volatile uint16_t kopf = 0, fuss = 0;
  volatile uint32_t verworfen = 0;   // bytes dropped because the buffer was full

  // Inquiry
  volatile bool     sucht = false;
  volatile bool     name_offen = false;   // a remote-name request is outstanding
  volatile uint8_t  fund_n = 0;
  TcSppFund fund[TC_SPP_SCAN_MAX];
} TcSpp;

/*********************************************************************************************\
 * Ring buffer
\*********************************************************************************************/

static inline uint16_t TcSppAvailable(void) {
  int32_t n = (int32_t)TcSpp.kopf - (int32_t)TcSpp.fuss;
  if (n < 0) { n += TC_SPP_RX_BUF; }
  return (uint16_t)n;
}

static void TcSppPush(const uint8_t *d, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    uint16_t nx = (TcSpp.kopf + 1) % TC_SPP_RX_BUF;
    if (nx == TcSpp.fuss) { TcSpp.verworfen += (len - i); return; }  // drop, never overwrite
    TcSpp.rx[TcSpp.kopf] = d[i];
    TcSpp.kopf = nx;
  }
}

static uint16_t TcSppPull(uint8_t *ziel, uint16_t max) {
  uint16_t n = 0;
  while (n < max && TcSpp.fuss != TcSpp.kopf) {
    ziel[n++] = TcSpp.rx[TcSpp.fuss];
    TcSpp.fuss = (TcSpp.fuss + 1) % TC_SPP_RX_BUF;
  }
  return n;
}

/*********************************************************************************************\
 * Callbacks -- these run in the Bluedroid task. Buffer only, compute nothing.
 *
 * Both callbacks NAME every event they see and print its status. A connect that never
 * completes is otherwise indistinguishable from out-of-range, wrong address, peer busy or
 * refused authentication -- the script only ever sees "state never reached 3". The layer
 * where the log stops IS the diagnosis; see the chain in the file header.
\*********************************************************************************************/

static const char *TcSppEvName(esp_spp_cb_event_t e) {
  switch (e) {
    case ESP_SPP_INIT_EVT:           return "INIT";
    case ESP_SPP_UNINIT_EVT:         return "UNINIT";
    case ESP_SPP_DISCOVERY_COMP_EVT: return "DISCOVERY_COMP";
    case ESP_SPP_OPEN_EVT:           return "OPEN";
    case ESP_SPP_CLOSE_EVT:          return "CLOSE";
    case ESP_SPP_START_EVT:          return "START";
    case ESP_SPP_CL_INIT_EVT:        return "CL_INIT";
    case ESP_SPP_DATA_IND_EVT:       return "DATA_IND";
    case ESP_SPP_CONG_EVT:           return "CONG";
    case ESP_SPP_WRITE_EVT:          return "WRITE";
    case ESP_SPP_SRV_OPEN_EVT:       return "SRV_OPEN";
    case ESP_SPP_SRV_STOP_EVT:       return "SRV_STOP";
    default:                         return "?";
  }
}

static const char *TcSppGapEvName(esp_bt_gap_cb_event_t e) {
  switch (e) {
    case ESP_BT_GAP_DISC_RES_EVT:             return "DISC_RES";
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:   return "DISC_STATE";
    case ESP_BT_GAP_RMT_SRVCS_EVT:            return "RMT_SRVCS";
    case ESP_BT_GAP_RMT_SRVC_REC_EVT:         return "RMT_SRVC_REC";
    case ESP_BT_GAP_AUTH_CMPL_EVT:            return "AUTH_CMPL";
    case ESP_BT_GAP_PIN_REQ_EVT:              return "PIN_REQ";
    case ESP_BT_GAP_CFM_REQ_EVT:              return "CFM_REQ";
    case ESP_BT_GAP_KEY_NOTIF_EVT:            return "KEY_NOTIF";
    case ESP_BT_GAP_KEY_REQ_EVT:              return "KEY_REQ";
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:     return "READ_REMOTE_NAME";
    case ESP_BT_GAP_MODE_CHG_EVT:             return "MODE_CHG";
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:   return "ACL_CONN_CMPL";
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:return "ACL_DISCONN_CMPL";
    default:                                  return "?";
  }
}

// GAP status codes above 0x100 are HCI errors with ESP_BT_STATUS_BASE_FOR_HCI_ERR added
// (esp_bt_defs.h). Nothing in the log says so, and the raw number tells you nothing:
// "stat=260" is 0x104 is HCI 0x04 is PAGE TIMEOUT -- the peer never answered our page.
// That one code is the whole difference between "out of range / slot taken" and any
// problem of ours, so it gets spelled out.
static const char *TcSppStatText(int stat) {
  if (stat == 0) { return "ok"; }
  if (stat < 0x100) { return "(bluedroid status)"; }
  switch (stat & 0xFF) {
    case 0x04: return "HCI PAGE TIMEOUT - peer did not answer: out of range, off, "
                      "or its only connection slot is taken";
    case 0x05: return "HCI authentication failure";
    case 0x06: return "HCI PIN or key missing";
    case 0x08: return "HCI connection timeout";
    case 0x0B: return "HCI connection already exists";
    case 0x0C: return "HCI command disallowed";
    case 0x0D: return "HCI connection rejected - limited resources";
    case 0x0E: return "HCI connection rejected - security";
    case 0x0F: return "HCI connection rejected - unacceptable address";
    case 0x13: return "HCI peer ended the connection";
    case 0x16: return "HCI connection terminated locally";
    case 0x1F: return "HCI unspecified error";
    case 0x22: return "HCI LMP response timeout";
    default:   return "HCI error";
  }
}

static void TcSppCb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param) {
  switch (event) {
    case ESP_SPP_INIT_EVT:
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: ev INIT status=%d"), (int)param->init.status);
      TcSpp.state = TC_SPP_BEREIT;
      break;

    case ESP_SPP_DISCOVERY_COMP_EVT:
      // The channel-0 path: SDP has looked up which RFCOMM channel carries SPP on the
      // peer, and only NOW can the connect go out. Without this branch sppConnect(addr, 0)
      // started a discovery and then waited forever for an OPEN that nobody had asked for.
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: ev DISCOVERY_COMP status=%d channels=%d"),
             (int)param->disc_comp.status, (int)param->disc_comp.scn_num);
      if (ESP_SPP_SUCCESS == param->disc_comp.status && param->disc_comp.scn_num > 0 &&
          TcSpp.ziel_gesetzt) {
        esp_bd_addr_t ziel; memcpy(ziel, (const void*)TcSpp.ziel, 6);
        AddLog(LOG_LEVEL_INFO, PSTR("SPP: SDP found channel %d, connecting"),
               (int)param->disc_comp.scn[0]);
        esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, param->disc_comp.scn[0], ziel);
      } else {
        TcSpp.state = TC_SPP_BEREIT;
        TcSpp.verbinde_fehlgeschlagen = true;
      }
      break;

    case ESP_SPP_CL_INIT_EVT:
      // The RFCOMM connect request went out. Success here says nothing about the peer --
      // a refusal arrives later as CLOSE (or as OPEN with a non-zero status).
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: ev CL_INIT status=%d handle=%u"),
             (int)param->cl_init.status, (unsigned)param->cl_init.handle);
      if (ESP_SPP_SUCCESS != param->cl_init.status) {
        TcSpp.state = TC_SPP_BEREIT;
        TcSpp.verbinde_fehlgeschlagen = true;
      }
      break;

    case ESP_SPP_OPEN_EVT:
      // ⚠️ The status MUST be checked. A failed connect can arrive as OPEN with a
      // non-zero status; an earlier version set the state to OPEN regardless and the
      // script then wrote into a channel that was never there.
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: ev OPEN status=%d handle=%u"),
             (int)param->open.status, (unsigned)param->open.handle);
      if (ESP_SPP_SUCCESS == param->open.status) {
        TcSpp.handle = param->open.handle;
        TcSpp.state = TC_SPP_OFFEN;
      } else {
        TcSpp.handle = 0;
        TcSpp.state = TC_SPP_BEREIT;
        TcSpp.verbinde_fehlgeschlagen = true;
      }
      break;

    case ESP_SPP_CLOSE_EVT:
      // async == false means WE closed it. A CLOSE while still connecting is the peer
      // refusing -- that is the single most common outcome and it used to be invisible.
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: ev CLOSE status=%d port=%u async=%d (was state %d)"),
             (int)param->close.status, (unsigned)param->close.port_status,
             (int)param->close.async, (int)TcSpp.state);
      if (TC_SPP_VERBINDET == TcSpp.state) { TcSpp.verbinde_fehlgeschlagen = true; }
      TcSpp.handle = 0;
      TcSpp.state = TC_SPP_BEREIT;
      break;

    case ESP_SPP_DATA_IND_EVT:
      TcSppPush(param->data_ind.data, param->data_ind.len);
      break;

    case ESP_SPP_CONG_EVT:
    case ESP_SPP_WRITE_EVT:
      break;                                  // too chatty to log per packet

    default:
      AddLog(LOG_LEVEL_DEBUG, PSTR("SPP: ev %s (%d)"), TcSppEvName(event), (int)event);
      break;
  }
}

static void TcSppGapCb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    // ── The baseband link. This is the FIRST thing that must succeed: it says the peer
    // answered our page, so radio, address and range are all fine. No ACL_CONN_CMPL at
    // all means the peer never answered -- too far, switched off, or its single
    // connection slot is taken (the Sunny Beam problem).
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: gap ACL_CONN_CMPL stat=%d %02X:%02X:%02X:%02X:%02X:%02X — %s"),
             (int)param->acl_conn_cmpl_stat.stat,
             param->acl_conn_cmpl_stat.bda[0], param->acl_conn_cmpl_stat.bda[1],
             param->acl_conn_cmpl_stat.bda[2], param->acl_conn_cmpl_stat.bda[3],
             param->acl_conn_cmpl_stat.bda[4], param->acl_conn_cmpl_stat.bda[5],
             TcSppStatText((int)param->acl_conn_cmpl_stat.stat));
      break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: gap ACL_DISCONN reason=%d"),
             (int)param->acl_disconn_cmpl_stat.reason);
      break;

    // ── Legacy pairing. Answer it; an unanswered request just times out silently.
    case ESP_BT_GAP_PIN_REQ_EVT: {
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: gap PIN_REQ min16=%d — replying \"%s\""),
             (int)param->pin_req.min_16_digit, TC_SPP_PIN);
      esp_bt_pin_code_t pin = { 0 };
      uint8_t len = (uint8_t)strlen(TC_SPP_PIN);
      if (len > ESP_BT_PIN_CODE_LEN) { len = ESP_BT_PIN_CODE_LEN; }
      memcpy(pin, TC_SPP_PIN, len);
      esp_bt_gap_pin_reply(param->pin_req.bda, true, len, pin);
      break;
    }
    case ESP_BT_GAP_AUTH_CMPL_EVT:
      AddLog(LOG_LEVEL_INFO, PSTR("SPP: gap AUTH_CMPL stat=%d name='%s' — %s"),
             (int)param->auth_cmpl.stat, (const char *)param->auth_cmpl.device_name,
             TcSppStatText((int)param->auth_cmpl.stat));
      break;

    case ESP_BT_GAP_MODE_CHG_EVT:
      AddLog(LOG_LEVEL_DEBUG, PSTR("SPP: gap MODE_CHG mode=%d"), (int)param->mode_chg.mode);
      break;

    case ESP_BT_GAP_DISC_RES_EVT: {
      if (TcSpp.fund_n >= TC_SPP_SCAN_MAX) { break; }
      // Already known? An inquiry reports the same device several times.
      for (uint8_t i = 0; i < TcSpp.fund_n; i++) {
        if (0 == memcmp(TcSpp.fund[i].addr, param->disc_res.bda, 6)) { return; }
      }
      TcSppFund *f = &TcSpp.fund[TcSpp.fund_n];
      memcpy(f->addr, param->disc_res.bda, 6);
      f->name[0] = '\0';
      for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = param->disc_res.prop + i;
        if (ESP_BT_GAP_DEV_PROP_BDNAME == p->type) {
          int l = p->len; if (l > (int)sizeof(f->name) - 1) { l = sizeof(f->name) - 1; }
          memcpy(f->name, p->val, l); f->name[l] = '\0';
        }
      }
      TcSpp.fund_n++;
      break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
      TcSpp.sucht = (ESP_BT_GAP_DISCOVERY_STARTED == param->disc_st_chg.state);
      break;

    // An inquiry only reports a name if the peer puts one in its EIR, and many do not.
    // TcSppScan asks for the missing ones separately once the inquiry has finished; the
    // answers land here and fill in the entry we already have.
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
      if (ESP_BT_STATUS_SUCCESS != param->read_rmt_name.stat) { break; }
      for (uint8_t i = 0; i < TcSpp.fund_n; i++) {
        if (0 == memcmp(TcSpp.fund[i].addr, param->read_rmt_name.bda, 6)) {
          strlcpy(TcSpp.fund[i].name, (const char *)param->read_rmt_name.rmt_name,
                  sizeof(TcSpp.fund[i].name));
          break;
        }
      }
      TcSpp.name_offen = false;
      break;
    }

    default:
      AddLog(LOG_LEVEL_DEBUG, PSTR("SPP: gap ev %s (%d)"), TcSppGapEvName(event), (int)event);
      break;
  }
}

/*********************************************************************************************\
 * Bring-up and connect
\*********************************************************************************************/

// Bring up the Bluedroid Classic stack. Safe to call repeatedly.
//
// EVERY step logs its return code, including the successful ones. An earlier version
// treated ESP_ERR_INVALID_STATE as harmless on the init/enable calls -- that MASKED the
// first real failure and made a later call (gap_register) look like the culprit.
// Log first, judge second.
//
// Note esp_err_to_name(0) prints "UNKNOWN ERROR" in this build: 0x0 IS success.
static bool TcSppInit(void) {
  if (TcSpp.state != TC_SPP_AUS) { return true; }
  esp_err_t r;

  // ⚠️ "largest" is deliberately MALLOC_CAP_INTERNAL. With MALLOC_CAP_DEFAULT the PSRAM
  // block counts too on a board WITH PSRAM, so this prints a megabyte while the INTERNAL
  // memory — the only pool that serves frame and DMA allocations — is nearly gone.
  // Exactly the number that then sends you looking in the wrong place.
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: status=%d (0=idle 1=inited 2=enabled) heap=%u largest_internal=%u"),
         (int)esp_bt_controller_get_status(),
         (unsigned)ESP_getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

  esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: cfg mode=%d"), (int)cfg.mode);

  r = esp_bt_controller_init(&cfg);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 1 controller_init  -> 0x%x %s (status now %d)"),
         r, esp_err_to_name(r), (int)esp_bt_controller_get_status());
  if (ESP_OK != r) { return false; }

  r = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 2 controller_enable-> 0x%x %s (status now %d)"),
         r, esp_err_to_name(r), (int)esp_bt_controller_get_status());
  if (ESP_OK != r) { return false; }

  r = esp_bluedroid_init();
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 3 bluedroid_init   -> 0x%x %s (status now %d)"),
         r, esp_err_to_name(r), (int)esp_bt_controller_get_status());
  if (ESP_OK != r) { return false; }

  r = esp_bluedroid_enable();
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 4 bluedroid_enable -> 0x%x %s (status now %d)"),
         r, esp_err_to_name(r), (int)esp_bt_controller_get_status());
  if (ESP_OK != r) { return false; }

  r = esp_bt_gap_register_callback(TcSppGapCb);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 5 gap_register     -> 0x%x %s"), r, esp_err_to_name(r));
  if (ESP_OK != r) { return false; }

  r = esp_spp_register_callback(TcSppCb);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 6 spp_register     -> 0x%x %s"), r, esp_err_to_name(r));
  if (ESP_OK != r) { return false; }

  esp_spp_cfg_t spp = { };
  spp.mode = ESP_SPP_MODE_CB;
  spp.enable_l2cap_ertm = false;
  spp.tx_buffer_size = 0;
  r = esp_spp_enhanced_init(&spp);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 7 spp_init         -> 0x%x %s"), r, esp_err_to_name(r));
  if (ESP_OK != r) { return false; }

  // We only ever connect out -- nobody should be able to connect to us.
  esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

  // Register the legacy PIN. This build has no SSP, so a peer that wants authentication
  // gets the 4-digit exchange -- and an unanswered PIN request dies in a timeout with
  // nothing in the log to explain it.
  {
    esp_bt_pin_code_t pin = { 0 };
    uint8_t plen = (uint8_t)strlen(TC_SPP_PIN);
    if (plen > ESP_BT_PIN_CODE_LEN) { plen = ESP_BT_PIN_CODE_LEN; }
    memcpy(pin, TC_SPP_PIN, plen);
    r = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, plen, pin);
    AddLog(LOG_LEVEL_INFO, PSTR("SPP: 8 gap_set_pin      -> 0x%x (\"%s\")"), r, TC_SPP_PIN);
  }

  // Give an old, slowly page-scanning peer time to notice us -- see TC_SPP_PAGE_TIMEOUT.
  r = esp_bt_gap_set_page_timeout(TC_SPP_PAGE_TIMEOUT);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 9 page_timeout     -> 0x%x (%u ms)"),
         r, (unsigned)((uint32_t)TC_SPP_PAGE_TIMEOUT * 625 / 1000));

  // Full BR/EDR transmit power. The ESP32's PCB antenna is no match for a laptop's, and
  // the same antenna is shared with WiFi; on a link at the edge of range this is free.
  r = esp_bredr_tx_power_set(ESP_PWR_LVL_N0, ESP_PWR_LVL_P9);
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: 10 tx_power        -> 0x%x (max +9 dBm)"), r);

  // The SPP callback flips the state to READY; give it up to a second.
  for (uint8_t i = 0; i < 50 && TC_SPP_AUS == TcSpp.state; i++) { delay(20); }
  if (TC_SPP_AUS == TcSpp.state) {
    AddLog(LOG_LEVEL_INFO, PSTR("SPP: all calls ok but no INIT event arrived"));
    return false;
  }
  AddLog(LOG_LEVEL_INFO, PSTR("SPP: ready"));
  return true;
}

static bool TcSppAdresse(const char *text, uint8_t *aus) {
  unsigned int v[6];
  if (6 != sscanf(text, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5])) {
    return false;
  }
  for (uint8_t i = 0; i < 6; i++) { aus[i] = (uint8_t)v[i]; }
  return true;
}

// channel 0 means "let SDP discovery find it" -- the connect is then issued from
// ESP_SPP_DISCOVERY_COMP_EVT once SDP has named the channel. Returns IMMEDIATELY in both
// cases; the script polls sppState() afterwards to see whether the channel came up.
//
// An inquiry MUST NOT be running while we page a peer: the radio cannot do both, and the
// connect either stalls or fails outright. Cancel it first -- cheap when nothing is
// running, and it removes an error that looks entirely like a peer problem.
static int32_t TcSppConnect(const char *adresse, int32_t kanal) {
  if (!TcSppInit()) { return -1; }
  uint8_t bda[6];
  if (!TcSppAdresse(adresse, bda)) { return -2; }
  if (TC_SPP_OFFEN == TcSpp.state) { return 0; }   // already connected

  esp_bt_gap_cancel_discovery();
  TcSpp.fuss = TcSpp.kopf = 0;                     // drop stale bytes
  TcSpp.verbinde_fehlgeschlagen = false;
  memcpy((void*)TcSpp.ziel, bda, 6);               // remembered for the SDP path
  TcSpp.ziel_gesetzt = true;
  TcSpp.state = TC_SPP_VERBINDET;

  AddLog(LOG_LEVEL_INFO, PSTR("SPP: connect %02X:%02X:%02X:%02X:%02X:%02X channel %d%s"),
         bda[0], bda[1], bda[2], bda[3], bda[4], bda[5], (int)kanal,
         (kanal > 0) ? "" : " (via SDP)");

  esp_bd_addr_t ziel; memcpy(ziel, bda, 6);
  esp_err_t r = (kanal > 0)
      ? esp_spp_connect(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_MASTER, (uint8_t)kanal, ziel)
      : esp_spp_start_discovery(ziel);
  if (ESP_OK != r) {
    AddLog(LOG_LEVEL_INFO, PSTR("SPP: connect call refused -> 0x%x %s"), r, esp_err_to_name(r));
    TcSpp.state = TC_SPP_BEREIT;
    return -3;
  }
  return 0;
}

static void TcSppClose(void) {
  if (TcSpp.handle) { esp_spp_disconnect(TcSpp.handle); }
  TcSpp.handle = 0;
  if (TcSpp.state == TC_SPP_OFFEN || TcSpp.state == TC_SPP_VERBINDET) {
    TcSpp.state = TC_SPP_BEREIT;
  }
}

// Tear the whole Classic stack down and give the memory back.
//
// ⭐ WHY THIS EXISTS. Bringing Bluedroid Classic up costs about 85 KB of heap on this
// build (measured on .185: 114 KB free after boot, 29 KB with the stack running), and it
// NEVER comes back on its own. A script that reads a device once every fifteen minutes
// therefore pays for the stack all the time -- and the next slot restart fails to
// allocate. That failure surfaces as **"Stack overflow"**, because the loader's
// out-of-memory paths return TC_ERR_STACK_OVERFLOW (xdrv_124_tinyc_vm.h). Nothing points
// at Bluetooth, and the script looks like it has a runaway recursion. It does not.
//
// ⚠️ DO NOT call esp_bt_controller_mem_release() here. That frees the controller's
// memory permanently, and every later esp_bt_controller_init() then fails with
// ESP_ERR_INVALID_STATE while the status still reads IDLE -- the exact trap documented
// at btInUse() at the top of this file. Release is one-way; disable+deinit is not.
static void TcSppDeinit(void) {
  if (TC_SPP_AUS == TcSpp.state) { return; }
  TcSppClose();
  delay(100);                       // let the disconnect leave before the stack goes

  esp_spp_deinit();
  esp_bluedroid_disable();
  esp_bluedroid_deinit();
  esp_bt_controller_disable();
  esp_bt_controller_deinit();

  TcSpp.state  = TC_SPP_AUS;
  TcSpp.handle = 0;
  TcSpp.kopf = TcSpp.fuss = 0;
  TcSpp.verworfen = 0;
  TcSpp.fund_n = 0;
  TcSpp.sucht = false;
  TcSpp.name_offen = false;
  TcSpp.ziel_gesetzt = false;
  TcSpp.verbinde_fehlgeschlagen = false;

  AddLog(LOG_LEVEL_INFO, PSTR("SPP: stack down, heap=%u largest=%u"),
         (unsigned)ESP_getFreeHeap(),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static int32_t TcSppWrite(const uint8_t *d, uint16_t len) {
  if (TC_SPP_OFFEN != TcSpp.state || !TcSpp.handle) { return -1; }
  return (ESP_OK == esp_spp_write(TcSpp.handle, len, (uint8_t*)d)) ? (int32_t)len : -1;
}

// Inquire for `sekunden` seconds and write the results as text, one "AA:BB:.. Name\n"
// per line. WAITS for the inquiry to finish, so it needs a context where delay() is
// allowed -- TaskLoop() or main().
//
// ⚠️ NOT from a spawnTask worker. A worker runs on its OWN VM with its OWN heap, and the
// script's char[] buffers live in the heap-array table that only the PRIMARY VM's loader
// fills in. In a worker every one of them has size 0 and the first access dies with
// runtime error 9. That cost us a morning on 2026-08-03 and looked, wrongly, like a hang
// in this function -- hence the "sppScan hangs" note that used to stand here. It never
// hung; the two callers crashed before and after it.
static int32_t TcSppScan(char *aus, uint16_t max, int32_t sekunden) {
  if (!TcSppInit()) { return -1; }
  if (sekunden < 1) { sekunden = 5; }
  if (sekunden > 30) { sekunden = 30; }

  TcSpp.fund_n = 0;
  TcSpp.sucht = true;
  // Duration is counted in units of 1.28 s by the API.
  uint8_t dauer = (uint8_t)((sekunden * 100) / 128);
  if (dauer < 1) { dauer = 1; }
  if (ESP_OK != esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, dauer, 0)) {
    return -1;
  }
  uint32_t ende = millis() + (uint32_t)sekunden * 1000 + 1500;
  while (TcSpp.sucht && !TimeReached(ende)) { delay(50); }
  esp_bt_gap_cancel_discovery();

  // Ask for the names the inquiry did not deliver. Only AFTER the inquiry -- the radio
  // cannot page and inquire at the same time, and Bluedroid rejects the request while a
  // discovery is running. One at a time, each with a short budget; a peer that stays
  // silent costs us that budget and nothing else.
  for (uint8_t i = 0; i < TcSpp.fund_n; i++) {
    if (TcSpp.fund[i].name[0]) { continue; }
    esp_bd_addr_t bda; memcpy(bda, TcSpp.fund[i].addr, 6);
    TcSpp.name_offen = true;
    if (ESP_OK != esp_bt_gap_read_remote_name(bda)) { TcSpp.name_offen = false; continue; }
    uint32_t nende = millis() + 2500;
    while (TcSpp.name_offen && !TimeReached(nende)) { delay(50); }
    TcSpp.name_offen = false;
  }

  uint16_t o = 0;
  if (max) { aus[0] = '\0'; }
  for (uint8_t i = 0; i < TcSpp.fund_n && o + 32 < max; i++) {
    TcSppFund *f = &TcSpp.fund[i];
    o += snprintf(aus + o, max - o, "%02X:%02X:%02X:%02X:%02X:%02X %s\n",
                  f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->addr[4], f->addr[5],
                  f->name);
  }
  return TcSpp.fund_n;
}

#endif  // USE_TINYC_SPP
#endif  // _XDRV_124_TINYC_SPP_H_
