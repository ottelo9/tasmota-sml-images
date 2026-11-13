/*
  xdrv_124_tinyc_espnow.h — ESP-NOW as a TinyC primitive (espnowBegin / espnowPeer / espnowSend / espnowRecv ...).

  ESP-NOW is Espressif's connectionless 2.4 GHz link: a packet of up to 250 bytes goes from one ESP to another by MAC address, without
  router, without association, and an ESP that sends once and goes back to sleep needs about 50 ms awake. gemu's use case (11.11.2026):
  a battery wall button (ESP32-C3, deep sleep, wakes on the key) that sends ONE packet to the lamp controller, which switches the
  radio-controlled lamps. The protocol on top lives in the SCRIPT, like SPP / USB: a one-byte button id is enough.

  ---------------------------------------------------------------------------------------
  WIFI. ESP-NOW rides on the WiFi radio and on its CHANNEL.
  ---------------------------------------------------------------------------------------
  * WiFi ON and connected (a receiver like the lamp controller): the radio sits on the channel of the access point, which is the
    channel the sender has to use. espnowBegin(0) keeps it.
  * A battery NODE does not connect at all: set the Tasmota command `WiFi 0` once (it is stored), and espnowBegin(channel) then
    starts the radio in station mode WITHOUT connecting (exactly what Tasmota's own WizMote driver does) and puts it on `channel`.
    That is the channel of the receiver's access point — fix it in the router, or the packets go nowhere.
  * Only ONE ESP-NOW user per device: it must not be built together with USE_WIZMOTE (xdrv_77) or TasMesh. user_config_override.h
    switches this feature off when one of them is there.

  ---------------------------------------------------------------------------------------
  RECEIVE. The callback runs in the WiFi task, so it only copies the packet into a ring (TC_ESPNOW_QUEUE packets of up to 250 bytes,
  258 bytes each = 2 KB RAM when enabled); the script reads it with espnowAvailable() / espnowRecv() from TaskLoop() or a tick.
  A full ring drops the NEWEST packet (and counts it). Nothing of this touches the VM from another task.

  ENCRYPTION is ESP-NOW's own (CCMP): espnowKey("16 characters") sets the key for the peers added AFTER it, on BOTH sides, and both
  sides must list the other as a peer (espnowPeer) — an encrypted packet from an unknown sender is dropped by the radio. Unicast only
  (not for the broadcast address), at most 6 encrypted peers. Without a key anyone in range can send you a packet: put a counter and
  an hmacSha256 of it in your payload if that matters.

  ⚠️ Sending to an unknown MAC adds it as a peer on the fly (with the key, if one is set). A unicast send waits (up to 100 ms) for the
  radio's answer and returns 1 only when the peer ACKed it; a broadcast (FF:FF:FF:FF:FF:FF) returns 1 when it was sent.
*/

#ifndef _XDRV_124_TINYC_ESPNOW_H_
#define _XDRV_124_TINYC_ESPNOW_H_

// On by default for the ESP32 family (about 2 KB RAM for the receive ring and some flash); -DTINYC_NO_ESPNOW leaves it out. Set here, in a
// tracked file, so a build from this repository has it without a private user_config_override.h.
// NOT on the ESP32-P4: it has no WiFi radio of its own (esp_wifi_remote), the esp_now_* functions are not linked and the build stops with
// "undefined reference to esp_now_init" (found building tinyc32-p4-full for 1.6.78).
#if defined(ESP32) && !defined(CONFIG_IDF_TARGET_ESP32P4) && !defined(TINYC_NO_ESPNOW) && !defined(USE_TINYC_ESPNOW)
#define USE_TINYC_ESPNOW
#endif

// One ESP-NOW user per device: Tasmota's WizMote decoder (xdrv_77) and TasMesh own the receive callback. When one of them is built in,
// TinyC leaves ESP-NOW alone (the calls answer 0). Decided HERE, after every configuration file has been read.
#if defined(USE_TINYC_ESPNOW) && (defined(USE_WIZMOTE) || defined(USE_TASMESH))
#undef USE_TINYC_ESPNOW
#endif

#if defined(ESP32) && defined(USE_TINYC_ESPNOW)

#include <esp_now.h>
#include <esp_wifi.h>

#ifndef TC_ESPNOW_QUEUE
#define TC_ESPNOW_QUEUE 8
#endif
#define TC_ESPNOW_MAXLEN 250

struct TcEspNowPkt {
  uint8_t mac[6];
  uint8_t len;
  int8_t  rssi;
  uint8_t data[TC_ESPNOW_MAXLEN];
};

struct TcEspNowState {
  bool     up;
  bool     have_key;
  uint8_t  key[16];
  uint8_t  head, tail, count;          // ring: push at head, pop at tail
  uint32_t rx, dropped;
  int8_t   last_rssi;
  volatile int8_t tx_status;           // -1 = waiting for the send callback, 0 = failed, 1 = delivered
  TcEspNowPkt q[TC_ESPNOW_QUEUE];
};

static TcEspNowState TcEn;
static portMUX_TYPE TcEnMux = portMUX_INITIALIZER_UNLOCKED;

// Callbacks run in the WiFi task: copy and leave. IDF 5.5 signatures (esp_now_recv_info_t / esp_now_send_info_t).
static void tc_en_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!info || !data || len <= 0 || len > TC_ESPNOW_MAXLEN) { return; }
  portENTER_CRITICAL(&TcEnMux);
  if (TcEn.count >= TC_ESPNOW_QUEUE) {
    TcEn.dropped++;
  } else {
    TcEspNowPkt *p = &TcEn.q[TcEn.head];
    memcpy(p->mac, info->src_addr, 6);
    p->len = (uint8_t)len;
    p->rssi = info->rx_ctrl ? (int8_t)info->rx_ctrl->rssi : 0;
    memcpy(p->data, data, len);
    TcEn.head = (TcEn.head + 1) % TC_ESPNOW_QUEUE;
    TcEn.count++;
    TcEn.rx++;
  }
  portEXIT_CRITICAL(&TcEnMux);
}

static void tc_en_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status) {
  TcEn.tx_status = (ESP_NOW_SEND_SUCCESS == status) ? 1 : 0;
}

// "AA:BB:CC:DD:EE:FF", "AA-BB-CC-DD-EE-FF" or "AABBCCDDEEFF" -> 6 bytes; false when it is not a MAC
static bool tc_en_parse_mac(const char *s, uint8_t *mac) {
  if (!s) { return false; }
  int n = 0;
  int hi = -1;
  for (const char *p = s; *p; p++) {
    char c = *p;
    int v;
    if (c >= '0' && c <= '9') { v = c - '0'; }
    else if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; }
    else if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; }
    else if (c == ':' || c == '-' || c == ' ') { continue; }
    else { return false; }
    if (hi < 0) { hi = v; }
    else {
      if (n >= 6) { return false; }
      mac[n++] = (uint8_t)((hi << 4) | v);
      hi = -1;
    }
  }
  return (6 == n) && (hi < 0);
}

static bool tc_en_is_broadcast(const uint8_t *mac) {
  for (int i = 0; i < 6; i++) { if (mac[i] != 0xFF) { return false; } }
  return true;
}

// start ESP-NOW. channel: 0 = keep the current one (WiFi connected); 1..13 = put the radio on it when it is not connected / WiFi is off.
static int tc_en_begin(int channel) {
  // Always start from scratch: a script that restarts (or a WiFi that was switched off / on since the last call) must not keep an ESP-NOW
  // that was initialised against the old radio state. (Found 11.11.2026: after `WiFi 0` the old init stayed "up" and every send failed.)
  if (TcEn.up) {
    esp_now_deinit();
    TcEn.up = false;
  }
  if (0 == Settings->flag4.network_wifi) {            // `WiFi 0`: node mode, no connection (same steps as xdrv_77 WizMote)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    if (channel < 1 || channel > 13) { channel = 1; }
    esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
  } else {
    wifi_mode_t wm = WIFI_MODE_NULL;
    if (ESP_OK != esp_wifi_get_mode(&wm) || WIFI_MODE_NULL == wm) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: espnowBegin - WiFi is not running yet"));
      return 0;
    }
    if (channel >= 1 && channel <= 13 && !WiFi.isConnected()) {   // not on an access point: use the asked channel
      esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    }
  }
  if (ESP_OK != esp_now_init()) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: espnowBegin - esp_now_init failed"));
    return 0;
  }
  esp_now_register_recv_cb(tc_en_recv_cb);
  esp_now_register_send_cb(tc_en_send_cb);
  portENTER_CRITICAL(&TcEnMux);
  TcEn.head = TcEn.tail = TcEn.count = 0;
  portEXIT_CRITICAL(&TcEnMux);
  TcEn.up = true;
  uint8_t ch = 0; wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: ESP-NOW on channel %d"), (int)ch);
  return 1;
}

static void tc_en_end(void) {
  if (!TcEn.up) { return; }
  esp_now_deinit();
  TcEn.up = false;
  portENTER_CRITICAL(&TcEnMux);
  TcEn.head = TcEn.tail = TcEn.count = 0;
  portEXIT_CRITICAL(&TcEnMux);
}

static int tc_en_set_key(const char *k) {
  if (!k || strlen(k) < 16) { TcEn.have_key = false; return 0; }   // fewer than 16 characters = no key
  memcpy(TcEn.key, k, 16);
  TcEn.have_key = true;
  return 1;
}

// add (or modify) a peer; encrypted when a key is set (never the broadcast address)
static int tc_en_add_peer(const uint8_t *mac) {
  if (!TcEn.up) { return 0; }
  esp_now_peer_info_t p;
  memset(&p, 0, sizeof(p));
  memcpy(p.peer_addr, mac, 6);
  p.channel = 0;                                       // the channel the radio is on
  p.ifidx = WIFI_IF_STA;
  p.encrypt = TcEn.have_key && !tc_en_is_broadcast(mac);
  if (p.encrypt) { memcpy(p.lmk, TcEn.key, 16); }
  esp_err_t e = esp_now_is_peer_exist(mac) ? esp_now_mod_peer(&p) : esp_now_add_peer(&p);
  return (ESP_OK == e) ? 1 : 0;
}

// send and wait (up to wait_ms) for the radio's verdict
static int tc_en_send(const uint8_t *mac, const uint8_t *data, int len, uint32_t wait_ms) {
  if (!TcEn.up || len <= 0 || len > TC_ESPNOW_MAXLEN) { return 0; }
  if (!esp_now_is_peer_exist(mac) && !tc_en_add_peer(mac)) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: espnowSend - peer could not be added"));
    return 0;
  }
  TcEn.tx_status = -1;
  esp_err_t se = esp_now_send(mac, data, (size_t)len);
  if (ESP_OK != se) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: espnowSend failed, esp_now_send=0x%X"), (unsigned)se);
    return 0;
  }
  uint32_t t0 = millis();
  while (TcEn.tx_status < 0 && (millis() - t0) < wait_ms) { vTaskDelay(1); }
  return (1 == TcEn.tx_status) ? 1 : 0;
}

static int tc_en_available(void) { return TcEn.count; }

// pop the oldest packet; 0 = none
static int tc_en_pop(TcEspNowPkt *out) {
  int got = 0;
  portENTER_CRITICAL(&TcEnMux);
  if (TcEn.count > 0) {
    *out = TcEn.q[TcEn.tail];
    TcEn.tail = (TcEn.tail + 1) % TC_ESPNOW_QUEUE;
    TcEn.count--;
    TcEn.last_rssi = out->rssi;
    got = 1;
  }
  portEXIT_CRITICAL(&TcEnMux);
  return got;
}

#endif  // ESP32 && USE_TINYC_ESPNOW
#endif  // _XDRV_124_TINYC_ESPNOW_H_
