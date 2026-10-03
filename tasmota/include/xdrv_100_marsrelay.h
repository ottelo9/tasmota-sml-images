/*
  xdrv_100_marsrelay.h - Marstek cloud emulation for Tasmota (port of marsrelay)

  Runs a Marstek battery (B2500, Venus, Jupiter, ...) without the Marstek cloud.
  Port of the ESPHome project marsrelay (github.com/tomquist/marsrelay, GPL-3.0):

    - the battery joins the Range Extender AP (RgxSSID ...) of this device
    - DNS on the AP answers every A query with the AP address, and the AP's DHCP
      server hands out that address as DNS server
    - port 8883: MQTT broker over TLS; the battery's .../device/... messages go to
      the home broker via Tasmota's MQTT client, .../App/.../ctrl commands from
      the home broker (hm2mqtt) go back to the battery
    - port 443: the TLS telemetry upload of Venus firmware v150+, answered byte
      for byte like the real cloud so the battery never resets its network chip
    - port 80: the clock endpoint and friends, served by Tasmota's web server
    - UDP proxy between the AP and the home network for power meter discovery

  Everything with sockets runs in its own FreeRTOS task: an RSA handshake takes
  about a second of CPU, and the Tasmota loop must not stall for it. The task
  talks to the loop through two queues; Tasmota's MQTT client is only touched
  from the loop.

  Included from xdrv_100_marsrelay.ino. Kept in a header because the .ino
  prototype generator cannot handle struct types in function signatures.
*/

#pragma once

#pragma pack(push)
#pragma pack()                 // BearSSL / lwIP structs with default alignment
#include <errno.h>
#include <time.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <dhcpserver/dhcpserver.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <uri/UriGlob.h>
#include <t_bearssl.h>
#include "xdrv_100_marsrelay_certs.h"
#pragma pack(pop)

#define MR_CFG_FILE        "/marsrelay.cfg"
#define MR_MQTT_PORT       8883
#define MR_HTTPS_PORT      443
#define MR_DNS_PORT        53
#define MR_MAX_CONN        5            // TLS connections, MQTT + HTTPS
#define MR_MAX_CONN_NOPS   2            // without PSRAM
#define MR_MAX_MQTT        3
#define MR_MAX_SUBS        8
#define MR_TOPIC_MAX       192
#define MR_ID_MAX          100
#define MR_MAX_MAPS        6
#define MR_MAX_UDP         3
#define MR_MAX_SESS        8
#define MR_IBUF            BR_SSL_BUFSIZE_INPUT   // a peer may send full 16 KB records
#define MR_OBUF            (4096 + 85)
#define MR_MQTT_RX         8192         // largest MQTT packet accepted
#define MR_HTTPS_RX        12288        // request headers + body
#define MR_MAX_HEADER      2048
#define MR_HS_TIMEOUT      15000
#define MR_REQ_TIMEOUT     30000
#define MR_UDP_SESSION     30000
#define MR_TASK_STACK      16384
#define MR_QUEUE_LEN       16

/*********************************************************************************************\
 * State
\*********************************************************************************************/

struct MrMap {
  char device[MR_ID_MAX];
  char external[MR_ID_MAX];
  char external_enc[33];       // hm2mqtt's AES variant of a MAC, "" if external is no MAC
};

struct MrCfg {
  uint16_t udp[MR_MAX_UDP];
  char     prefix[33];
  uint8_t  https;
  uint8_t  accept_all;
  uint8_t  requests;
  uint8_t  telemetry;
  uint16_t hold_s;
  uint8_t  nmaps;
  MrMap    maps[MR_MAX_MAPS];
};

struct MrStats {
  volatile uint32_t dev_msgs, dev_last, cmds, cmds_lost, up_errors;
  volatile uint32_t http_reqs, http_last, uploads;
  volatile uint32_t udp_to_sta, udp_to_ap, udp_dropped, udp_last;
  volatile uint32_t dns, tls_errors, tls_last_err;
  volatile uint8_t  dev_seen, http_seen, udp_seen;
  volatile uint8_t  broker_up, https_up, clients;
};

enum { MR_C_MQTT = 1, MR_C_HTTPS = 2 };

struct MrConn {
  uint8_t  kind;
  int      fd;
  char     peer[16];
  br_ssl_server_context *sc;
  uint8_t *ibuf;
  uint8_t *obuf;
  uint8_t *rx;                 // decrypted application data
  uint32_t rx_len, rx_cap;
  bool     hs_done;
  uint32_t t_open, t_last;
  // MQTT
  bool     mqtt_up;
  uint16_t keepalive;
  char     client_id[64];
  uint8_t  nsubs;
  char     subs[MR_MAX_SUBS][MR_TOPIC_MAX];
  // HTTPS
  bool     answered;
  uint32_t close_at;
};

struct MrSess {
  uint32_t ip;                 // network byte order, 0 = free
  uint16_t port;
  uint32_t last;
};

struct MrUdp {
  uint16_t port;
  int      ap_fd;              // receives the battery's broadcasts
  int      sta_fd;             // forwards them, receives the meter's answers
  MrSess   sess[MR_MAX_SESS];
};

struct MrMsg {                 // queue item, topic and payload follow the struct
  char    *topic;
  uint8_t *payload;
  uint32_t len;
};

struct MrResp {
  int         code;
  const char *ctype;
  char        body[96];
  bool        kong;
};

static MrCfg    MrC;
static MrStats  MrS;
static MrConn  *MrConns[MR_MAX_CONN];
static MrUdp    MrU[MR_MAX_UDP];
static int      MrMqttFd = -1;
static int      MrHttpsFd = -1;
static int      MrDnsFd = -1;
static volatile bool MrUdpRestart = false;
static volatile bool MrHttpsRestart = false;
static QueueHandle_t     MrUpQ = nullptr;     // task -> loop: publish to the home broker
static QueueHandle_t     MrDownQ = nullptr;   // loop -> task: commands for the battery
static SemaphoreHandle_t MrLockH = nullptr;   // guards MrC
static TaskHandle_t      MrTaskH = nullptr;
static bool              MrReady = false;

static br_x509_certificate MrMqttChain[1];
static br_x509_certificate MrHttpsChain[1];
static br_rsa_private_key  MrMqttKey;
static br_rsa_private_key  MrHttpsKey;

// Offered in this order; BearSSL follows the client's preference.
static const uint16_t kMrSuites[] = {
  BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
  BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
  BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,
  BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,
  BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,
  BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,
  BR_TLS_RSA_WITH_AES_128_GCM_SHA256,
  BR_TLS_RSA_WITH_AES_256_GCM_SHA384,
  BR_TLS_RSA_WITH_AES_128_CBC_SHA256,
  BR_TLS_RSA_WITH_AES_256_CBC_SHA256,
  BR_TLS_RSA_WITH_AES_128_CBC_SHA,
  BR_TLS_RSA_WITH_AES_256_CBC_SHA,
};

static void MrLock(void)   { if (MrLockH) { xSemaphoreTake(MrLockH, portMAX_DELAY); } }
static void MrUnlock(void) { if (MrLockH) { xSemaphoreGive(MrLockH); } }

static void *MrAlloc(size_t size) {
  void *p = special_malloc(size);    // PSRAM when present
  if (p) { memset(p, 0, size); }
  return p;
}

static bool MrExpired(uint32_t since, uint32_t ms) { return (uint32_t)(millis() - since) >= ms; }

static void MrIpStr(uint32_t ip, char *out) {   // ip in network byte order
  const uint8_t *b = (const uint8_t*)&ip;
  snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/*********************************************************************************************\
 * Helpers: hex, URL decoding, JSON
\*********************************************************************************************/

static void MrRandHex(char *out, uint32_t bytes) {
  static const char hex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < bytes; i++) {
    uint8_t b = esp_random() & 0xFF;
    out[i * 2] = hex[b >> 4];
    out[i * 2 + 1] = hex[b & 0x0F];
  }
  out[bytes * 2] = 0;
}

static int MrHexVal(char c) {
  if (c >= '0' && c <= '9') { return c - '0'; }
  if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
  if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
  return -1;
}

// In place; '+' is a space, bad escapes are kept as they are.
static void MrUrlDecode(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if ('+' == *p) {
      *o++ = ' ';
    } else if (('%' == *p) && p[1] && p[2] && (MrHexVal(p[1]) >= 0) && (MrHexVal(p[2]) >= 0)) {
      *o++ = (char)((MrHexVal(p[1]) << 4) | MrHexVal(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = 0;
}

// One parameter of an "a=1&b=2" string, percent-decoded into out.
static bool MrQueryParam(const char *q, const char *name, char *out, size_t cap) {
  size_t nl = strlen(name);
  const char *p = q;
  while (*p) {
    const char *end = strchr(p, '&');
    if (!end) { end = p + strlen(p); }
    if (((size_t)(end - p) > nl) && !strncmp(p, name, nl) && ('=' == p[nl])) {
      size_t vl = end - p - nl - 1;
      if (vl >= cap) { vl = cap - 1; }
      memcpy(out, p + nl + 1, vl);
      out[vl] = 0;
      MrUrlDecode(out);
      return true;
    }
    if (!*end) { break; }
    p = end + 1;
  }
  return false;
}

// String value of a top-level key in a flat JSON object, unescaped.
static bool MrJsonStr(const char *json, const char *key, char *out, size_t cap) {
  char pat[16];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) { return false; }
  p += strlen(pat);
  while ((' ' == *p) || ('\t' == *p) || ('\r' == *p) || ('\n' == *p)) { p++; }
  if (':' != *p) { return false; }
  p++;
  while ((' ' == *p) || ('\t' == *p) || ('\r' == *p) || ('\n' == *p)) { p++; }
  if ('"' != *p) { return false; }
  p++;
  size_t o = 0;
  while (*p && ('"' != *p) && (o < cap - 1)) {
    char c = *p++;
    if (('\\' == c) && *p) {
      c = *p++;
      if ('n' == c) { c = '\n'; }
      else if ('r' == c) { c = '\r'; }
      else if ('t' == c) { c = '\t'; }
      else if (('u' == c) && p[0] && p[1] && p[2] && p[3]) {
        c = (char)((MrHexVal(p[2]) << 4) | MrHexVal(p[3]));   // the blob is ASCII
        p += 4;
      }
    }
    out[o++] = c;
  }
  out[o] = 0;
  return true;
}

// Appends a quoted, escaped JSON string; returns the new length.
static size_t MrJsonAppendStr(char *out, size_t pos, size_t cap, const char *s, size_t len) {
  if (pos + 2 >= cap) { return pos; }
  out[pos++] = '"';
  for (size_t i = 0; (i < len) && (pos + 7 < cap); i++) {
    uint8_t c = (uint8_t)s[i];
    if (('"' == c) || ('\\' == c)) {
      out[pos++] = '\\';
      out[pos++] = c;
    } else if (c < 0x20) {
      pos += snprintf(out + pos, cap - pos, "\\u%04x", c);
    } else {
      out[pos++] = c;
    }
  }
  out[pos++] = '"';
  out[pos] = 0;
  return pos;
}

static size_t MrAppend(char *out, size_t pos, size_t cap, const char *s) {
  size_t l = strlen(s);
  if (pos + l >= cap) { l = (pos < cap - 1) ? cap - 1 - pos : 0; }
  memcpy(out + pos, s, l);
  pos += l;
  out[pos] = 0;
  return pos;
}

// Integer value with an implied decimal point, e.g. 5231 with dec 2 -> 52.31
static size_t MrAppendScaled(char *out, size_t pos, size_t cap, const char *val, uint8_t dec) {
  long v = strtol(val, nullptr, 10);
  char num[24];
  if (0 == dec) {
    snprintf(num, sizeof(num), "%ld", v);
  } else {
    long div = (1 == dec) ? 10 : 100;
    long a = (v < 0) ? -v : v;
    snprintf(num, sizeof(num), "%s%ld.%0*ld", (v < 0) ? "-" : "", a / div, (int)dec, a % div);
  }
  return MrAppend(out, pos, cap, num);
}

/*********************************************************************************************\
 * Queues between the task and the Tasmota loop
\*********************************************************************************************/

static bool MrQueuePush(QueueHandle_t q, const char *topic, const uint8_t *payload, uint32_t len) {
  if (!q) { return false; }
  size_t tl = strlen(topic);
  MrMsg *m = (MrMsg*)special_malloc(sizeof(MrMsg) + tl + 1 + len + 1);
  if (!m) { return false; }
  m->topic = (char*)(m + 1);
  memcpy(m->topic, topic, tl + 1);
  m->payload = (uint8_t*)m->topic + tl + 1;
  if (len) { memcpy(m->payload, payload, len); }
  m->payload[len] = 0;
  m->len = len;
  if (xQueueSend(q, &m, 0) != pdTRUE) {
    free(m);
    return false;
  }
  return true;
}

static void MrPublishUp(const char *topic, const uint8_t *payload, uint32_t len) {
  if (!MrQueuePush(MrUpQ, topic, payload, len)) {
    MrS.up_errors++;
    AddLog(LOG_LEVEL_DEBUG, PSTR("MRY: Queue full, dropped %s"), topic);
  }
}

// Loop side: publish what the task collected. Tasmota's MQTT client is not thread safe.
static void MrUpDrain(void) {
  MrMsg *m;
  for (uint32_t i = 0; (i < 4) && MrUpQ && (xQueueReceive(MrUpQ, &m, 0) == pdTRUE); i++) {
    if (MqttIsConnected()) {
      MqttPublishPayload(m->topic, (const char*)m->payload, m->len, false, LOG_LEVEL_DEBUG_MORE);
    } else {
      MrS.up_errors++;
    }
    free(m);
  }
}

/*********************************************************************************************\
 * ID mappings (topic segment rewriting between battery and hm2mqtt)
\*********************************************************************************************/

// hm2mqtt's topic id of a B2500 on marstek_energy: AES-128-CBC(zero IV, PKCS7) of the MAC, hex.
static void MrEncryptMac(const char *mac, char *out) {
  static const uint8_t key[16] = { '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '{', '}', '[', ']' };
  uint8_t blk[16];
  uint8_t iv[16] = { 0 };
  memcpy(blk, mac, 12);
  memset(blk + 12, 4, 4);
  br_aes_small_cbcenc_keys ctx;
  br_aes_small_cbcenc_init(&ctx, key, sizeof(key));
  br_aes_small_cbcenc_run(&ctx, iv, blk, sizeof(blk));
  static const char hex[] = "0123456789abcdef";
  for (uint32_t i = 0; i < 16; i++) {
    out[i * 2] = hex[blk[i] >> 4];
    out[i * 2 + 1] = hex[blk[i] & 0x0F];
  }
  out[32] = 0;
}

static bool MrIsMac(const char *s) {
  if (strlen(s) != 12) { return false; }
  for (uint32_t i = 0; i < 12; i++) {
    if (MrHexVal(s[i]) < 0) { return false; }
  }
  return true;
}

// hm2mqtt uses the encrypted MAC only on marstek_energy/<HMA|HMF|HMK|HMJ...>/ topics.
static bool MrUsesEncrypted(const char *topic) {
  if (strncmp(topic, "marstek_energy/", 15)) { return false; }
  const char *t = topic + 15;
  size_t seg = strcspn(t, "/");
  size_t base = strcspn(t, "-/");
  if (base > seg) { base = seg; }
  if (base != 3) { return false; }
  static const char *const types[] = { "HMA", "HMF", "HMK", "HMJ" };
  for (uint32_t i = 0; i < 4; i++) {
    if (!strncasecmp(t, types[i], 3)) { return true; }
  }
  return false;
}

// to_external: battery topic -> home broker topic, else the reverse.
static void MrTranslate(const char *in, char *out, size_t cap, bool to_external) {
  MrLock();
  bool enc = to_external && MrUsesEncrypted(in);
  size_t o = 0;
  const char *p = in;
  while (true) {
    const char *e = strchr(p, '/');
    size_t n = e ? (size_t)(e - p) : strlen(p);
    const char *rep = nullptr;
    for (uint32_t i = 0; i < MrC.nmaps; i++) {
      MrMap *m = &MrC.maps[i];
      if (to_external) {
        if ((strlen(m->device) == n) && !strncmp(p, m->device, n)) {
          rep = (enc && m->external_enc[0]) ? m->external_enc : m->external;
        }
      } else {
        if (((strlen(m->external) == n) && !strncmp(p, m->external, n)) ||
            (m->external_enc[0] && (strlen(m->external_enc) == n) && !strncmp(p, m->external_enc, n))) {
          rep = m->device;
        }
      }
      if (rep) { break; }
    }
    const char *src = rep ? rep : p;
    size_t sl = rep ? strlen(rep) : n;
    if (o + sl >= cap) { sl = cap - 1 - o; }
    memcpy(out + o, src, sl);
    o += sl;
    if (!e || (o + 1 >= cap)) { break; }
    out[o++] = '/';
    p = e + 1;
  }
  out[o] = 0;
  MrUnlock();
}

/*********************************************************************************************\
 * Cloud HTTP endpoints (shared by port 80 and port 443)
\*********************************************************************************************/

static bool MrIsTimePath(const char *url) {
  // Firmware spelled it "getDateInfoeu.php" and "getDateInfo.php": match the stem of the
  // last segment and require an extension after it.
  const char *name = strrchr(url, '/');
  name = name ? name + 1 : url;
  if (strncmp(name, "getDateInfo", 11)) { return false; }
  const char *rest = name + 11;
  return !*rest || strchr(rest, '.');
}

static bool MrUploadId(const char *url, char *id, size_t cap) {
  static const char prefix[] = "/data-upload/v1/venus/";
  size_t pl = sizeof(prefix) - 1;
  if (strncmp(url, prefix, pl) || !url[pl] || strchr(url + pl, '/')) { return false; }
  strlcpy(id, url + pl, cap);
  return true;
}

static void MrHttpDate(char *out) {
  static const char days[] = "SunMonTueWedThuFriSat";
  static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
  time_t now = UtcTime();
  struct tm t;
  gmtime_r(&now, &t);
  snprintf(out, 40, "%.3s, %02d %.3s %04d %02d:%02d:%02d GMT", days + 3 * (t.tm_wday % 7), t.tm_mday,
           months + 3 * (t.tm_mon % 12), t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);
}

// Framing copied from a capture of the real cloud. The battery rejected a reply with the
// same body but ordinary headers in an ordinary order, so do not "simplify" this.
static int MrBuildRaw(const MrResp *r, char *out, size_t cap) {
  char date[40];
  MrHttpDate(date);
  if (r->code != 200) {
    return snprintf(out, cap, "HTTP/1.1 %d %s\r\nDate: %s\r\nContent-Length: 0\r\nConnection: keep-alive\r\n\r\n",
                    r->code, (404 == r->code) ? "Not Found" : "OK", date);
  }
  char trace[33];
  MrRandHex(trace, 16);
  int n = snprintf(out, cap, "HTTP/1.1 200 OK\r\nDate: %s\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\n"
                   "Connection: keep-alive\r\nTrace-Id: %s\r\n", date, r->ctype ? r->ctype : "text/plain", trace);
  if (r->kong) {
    // The upload host sits behind a Kong gateway, which adds these.
    char kid[33];
    MrRandHex(kid, 16);
    n += snprintf(out + n, cap - n, "vary: Origin\r\nAccess-Control-Allow-Credentials: true\r\n"
                  "X-Kong-Upstream-Latency: 2\r\nX-Kong-Proxy-Latency: 0\r\nVia: 1.1 kong/3.9.1\r\n"
                  "X-Kong-Request-Id: %s\r\nStrict-Transport-Security: max-age=31536000; includeSubDomains\r\n", kid);
  }
  n += snprintf(out + n, cap - n, "\r\n%x\r\n%s\r\n0\r\n\r\n", (unsigned)strlen(r->body), r->body);
  return n;
}

static void MrPublishRequest(const char *method, const char *url, const char *body, size_t blen, const char *ip) {
  size_t cap = 160 + (strlen(url) + blen) * 6;   // worst case every byte escaped
  char *js = (char*)special_malloc(cap);
  if (!js) { return; }
  size_t p = MrAppend(js, 0, cap, "{\"source_ip\":");
  p = MrJsonAppendStr(js, p, cap, ip, strlen(ip));
  p = MrAppend(js, p, cap, ",\"url\":");
  p = MrJsonAppendStr(js, p, cap, url, strlen(url));
  p = MrAppend(js, p, cap, ",\"method\":");
  p = MrJsonAppendStr(js, p, cap, method, strlen(method));
  p = MrAppend(js, p, cap, ",\"body\":");
  p = MrJsonAppendStr(js, p, cap, body, blen);
  p = MrAppend(js, p, cap, "}");
  char topic[64];
  MrLock();
  snprintf(topic, sizeof(topic), "%s/request", MrC.prefix);
  MrUnlock();
  MrPublishUp(topic, (const uint8_t*)js, p);
  free(js);
}

struct MrVField {
  const char *key;
  const char *name;
  uint8_t     dec;             // decimals of the implied point, MR_VSTR = pass through as string
};
#define MR_VSTR 0xFF

// Keys whose meaning is confirmed against Modbus readings of the same device (marsrelay).
static const MrVField kMrVenus[] = {
  { "di", "device_id", MR_VSTR },        { "sn", "serial", MR_VSTR },
  { "ip", "ip", MR_VSTR },               { "dt", "device_clock", MR_VSTR },
  { "wm", "work_mode", MR_VSTR },        { "sc", "soc", 0 },
  { "pb", "battery_power", 0 },          { "bv", "battery_voltage", 2 },
  { "bi", "battery_current", 1 },        { "go", "grid_power", 0 },
  { "gv", "grid_voltage", 1 },           { "gf", "grid_frequency", 1 },
  { "mc", "max_charge_power", 0 },       { "md", "max_discharge_power", 0 },
  { "t1", "temperature_internal", 1 },   { "t2", "temperature_mos", 1 },
  { "dn", "control_firmware", MR_VSTR }, { "bm", "bms_firmware", MR_VSTR },
  { "iv", "inverter_firmware", MR_VSTR },{ "mv", "mppt_firmware", MR_VSTR },
};

// blob: the value of the upload's "d" field, decoded once (a URL-encoded query string).
static void MrVenusPublish(const char *id, const char *blob) {
  size_t bl = strlen(blob);
  size_t cap = bl * 6 + 256;
  char *dec = (char*)special_malloc(bl + 1);
  char *js = (char*)special_malloc(cap);
  char *un = (char*)special_malloc(cap);
  if (!dec || !js || !un) {
    free(dec); free(js); free(un);
    return;
  }
  memcpy(dec, blob, bl + 1);
  MrUrlDecode(dec);

  size_t p = MrAppend(js, 0, cap, "{");
  size_t u = 0;
  un[0] = 0;
  char *save = nullptr;
  for (char *kv = strtok_r(dec, "&", &save); kv; kv = strtok_r(nullptr, "&", &save)) {
    char *eq = strchr(kv, '=');
    if (!eq) { continue; }
    *eq = 0;
    const char *key = kv;
    const char *val = eq + 1;
    const MrVField *f = nullptr;
    for (uint32_t i = 0; i < sizeof(kMrVenus) / sizeof(kMrVenus[0]); i++) {
      if (!strcmp(key, kMrVenus[i].key)) { f = &kMrVenus[i]; break; }
    }
    if (f) {
      if (p > 1) { p = MrAppend(js, p, cap, ","); }
      p = MrJsonAppendStr(js, p, cap, f->name, strlen(f->name));
      p = MrAppend(js, p, cap, ":");
      if (MR_VSTR == f->dec) {
        p = MrJsonAppendStr(js, p, cap, val, strlen(val));
      } else {
        p = MrAppendScaled(js, p, cap, val, f->dec);
      }
    } else if (!strcmp(key, "tc")) {     // cell temperatures, 0.1 degC each
      if (p > 1) { p = MrAppend(js, p, cap, ","); }
      p = MrAppend(js, p, cap, "\"cell_temperatures\":[");
      char *s2 = nullptr;
      bool first = true;
      for (char *it = strtok_r((char*)val, ",", &s2); it; it = strtok_r(nullptr, ",", &s2)) {
        if (!first) { p = MrAppend(js, p, cap, ","); }
        p = MrAppendScaled(js, p, cap, it, 1);
        first = false;
      }
      p = MrAppend(js, p, cap, "]");
    } else {                             // everything else verbatim, not guessed at
      if (u) { u = MrAppend(un, u, cap, ","); }
      u = MrJsonAppendStr(un, u, cap, key, strlen(key));
      u = MrAppend(un, u, cap, ":");
      u = MrJsonAppendStr(un, u, cap, val, strlen(val));
    }
  }
  if (u) {
    if (p > 1) { p = MrAppend(js, p, cap, ","); }
    p = MrAppend(js, p, cap, "\"unmapped\":{");
    p = MrAppend(js, p, cap, un);
    p = MrAppend(js, p, cap, "}");
  }
  p = MrAppend(js, p, cap, "}");

  char topic[MR_TOPIC_MAX];
  MrLock();
  snprintf(topic, sizeof(topic), "%s/venus/%s/telemetry", MrC.prefix, id);
  MrUnlock();
  MrPublishUp(topic, (const uint8_t*)js, p);
  free(dec); free(js); free(un);
}

// blob: pre-extracted "d" value (port 80 form posts), else taken from body.
static void MrDispatch(const char *method, const char *url, const char *body, size_t blen, bool complete,
                       const char *ip, const char *blob, MrResp *res) {
  MrS.http_reqs++;
  MrS.http_last = millis();
  MrS.http_seen = 1;
  AddLog(LOG_LEVEL_DEBUG, PSTR("MRY: %s %s from %s (body %u bytes)"), method, url, ip, blen);
  if (MrC.requests) {
    // A body that did not arrive whole is not handed on: it cannot be told from a complete one.
    MrPublishRequest(method, url, complete ? body : "", complete ? blen : 0, ip);
  }

  bool get = !strcmp(method, "GET");
  bool post = !strcmp(method, "POST");
  char id[MR_ID_MAX];
  res->code = 200;
  res->kong = false;
  res->body[0] = 0;

  if (get && !strcmp(url, "/prod/api/v1/setB2500Report")) {
    res->ctype = "application/json";
    strlcpy(res->body, "{\"code\":1,\"msg\":\"ok\"}", sizeof(res->body));
  } else if (get && MrIsTimePath(url)) {
    res->ctype = "text/html; charset=utf-8";     // the real endpoint labels it text/html
    snprintf(res->body, sizeof(res->body), "_%04d_%02d_%02d_%02d_%02d_%02d_04_0_0_0", RtcTime.year,
             RtcTime.month, RtcTime.day_of_month, RtcTime.hour, RtcTime.minute, RtcTime.second);
  } else if (!strcmp(url, "/app/Solar/puterrinfo.php") && (get || post)) {
    res->ctype = "text/plain";
    strlcpy(res->body, post ? "_1" : "_2", sizeof(res->body));
  } else if (post && MrUploadId(url, id, sizeof(id))) {
    MrS.uploads++;
    if (!complete) {
      // Answer anyway: the acknowledgement is what keeps the battery from resetting itself.
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: Upload from %s not received whole, answered but not decoded"), id);
    } else if (MrC.telemetry) {
      char *b = (char*)special_malloc(blen + 1);
      if (b) {
        bool have = false;
        if (blob) {
          strlcpy(b, blob, blen + 1);
          have = true;
        } else if ('{' == body[0]) {
          have = MrJsonStr(body, "d", b, blen + 1);
        } else {
          have = MrQueryParam(body, "d", b, blen + 1);
        }
        if (have && b[0]) {
          MrVenusPublish(id, b);
        } else {
          AddLog(LOG_LEVEL_INFO, PSTR("MRY: Upload from %s carried no 'd' field"), id);
        }
        free(b);
      }
    }
    // The firmware looks for "code": and atoi()s what follows: it must be 0.
    res->ctype = "application/json";
    strlcpy(res->body, "{\"code\":0,\"message\":\"success\",\"data\":null}", sizeof(res->body));
    res->kong = true;
  } else if (get && !strcmp(url, "/ems/api/v1/getRealtimeSoc")) {
    res->ctype = "application/json";
    strlcpy(res->body, "{\"code\":1,\"show\":0,\"msg\":\"ok\",\"data\":{\"soc\":0,\"time_no\":0}}", sizeof(res->body));
  } else {
    res->code = 404;
    res->ctype = "text/plain";
  }
}

/*********************************************************************************************\
 * Port 80: Tasmota's web server
\*********************************************************************************************/

static void MrHttpHandle(void) {
  bool post = (HTTP_POST == Webserver->method());
  String url = Webserver->uri();
  String body;
  String blob;
  bool have_blob = false;
  if (post) {
    if (Webserver->hasArg("plain")) {
      body = Webserver->arg("plain");
    } else {
      // A form post is split into arguments by the server; rebuild it for the log topic.
      for (int i = 0; i < Webserver->args(); i++) {
        if (i) { body += "&"; }
        body += Webserver->argName(i) + "=" + Webserver->arg(i);
      }
      if (Webserver->hasArg("d")) {
        blob = Webserver->arg("d");
        have_blob = true;
      }
    }
  }
  String ip = Webserver->client().remoteIP().toString();
  MrResp res;
  MrDispatch(post ? "POST" : "GET", url.c_str(), body.c_str(), body.length(), true, ip.c_str(),
             have_blob ? blob.c_str() : nullptr, &res);
  if (404 == res.code) {
    Webserver->send(404, "text/plain", "Not found");
    return;
  }
  char raw[768];
  int n = MrBuildRaw(&res, raw, sizeof(raw));
  Webserver->client().write((const uint8_t*)raw, n);
}

static void MrWebHandlers(void) {
  Webserver->on(UriGlob("*/getDateInfo*"), HTTP_GET, MrHttpHandle);
  Webserver->on("/prod/api/v1/setB2500Report", HTTP_GET, MrHttpHandle);
  Webserver->on("/app/Solar/puterrinfo.php", HTTP_GET, MrHttpHandle);
  Webserver->on("/app/Solar/puterrinfo.php", HTTP_POST, MrHttpHandle);
  Webserver->on("/ems/api/v1/getRealtimeSoc", HTTP_GET, MrHttpHandle);
  Webserver->on(UriGlob("/data-upload/v1/venus/*"), HTTP_POST, MrHttpHandle);
}

/*********************************************************************************************\
 * TLS (BearSSL server engine, non-blocking pump)
\*********************************************************************************************/

static void MrKeysInit(void) {
  MrMqttChain[0].data = (unsigned char*)MR_MQTT_CERT;
  MrMqttChain[0].data_len = sizeof(MR_MQTT_CERT);
  MrMqttKey.n_bitlen = MR_MQTT_NBITS;
  MrMqttKey.p = (unsigned char*)MR_MQTT_P;   MrMqttKey.plen = sizeof(MR_MQTT_P);
  MrMqttKey.q = (unsigned char*)MR_MQTT_Q;   MrMqttKey.qlen = sizeof(MR_MQTT_Q);
  MrMqttKey.dp = (unsigned char*)MR_MQTT_DP; MrMqttKey.dplen = sizeof(MR_MQTT_DP);
  MrMqttKey.dq = (unsigned char*)MR_MQTT_DQ; MrMqttKey.dqlen = sizeof(MR_MQTT_DQ);
  MrMqttKey.iq = (unsigned char*)MR_MQTT_IQ; MrMqttKey.iqlen = sizeof(MR_MQTT_IQ);

  MrHttpsChain[0].data = (unsigned char*)MR_HTTPS_CERT;
  MrHttpsChain[0].data_len = sizeof(MR_HTTPS_CERT);
  MrHttpsKey.n_bitlen = MR_HTTPS_NBITS;
  MrHttpsKey.p = (unsigned char*)MR_HTTPS_P;   MrHttpsKey.plen = sizeof(MR_HTTPS_P);
  MrHttpsKey.q = (unsigned char*)MR_HTTPS_Q;   MrHttpsKey.qlen = sizeof(MR_HTTPS_Q);
  MrHttpsKey.dp = (unsigned char*)MR_HTTPS_DP; MrHttpsKey.dplen = sizeof(MR_HTTPS_DP);
  MrHttpsKey.dq = (unsigned char*)MR_HTTPS_DQ; MrHttpsKey.dqlen = sizeof(MR_HTTPS_DQ);
  MrHttpsKey.iq = (unsigned char*)MR_HTTPS_IQ; MrHttpsKey.iqlen = sizeof(MR_HTTPS_IQ);
}

static bool MrTlsSetup(MrConn *c) {
  br_ssl_server_context *sc = c->sc;
  br_ssl_engine_context *eng = &sc->eng;
  br_ssl_server_zero(sc);
  // The battery's TLS version range is unknown; BearSSL can still do 1.0 and 1.1.
  br_ssl_engine_set_versions(eng, BR_TLS10, BR_TLS12);
  br_ssl_engine_add_flags(eng, BR_OPT_NO_RENEGOTIATION);
  br_ssl_engine_set_suites(eng, kMrSuites, sizeof(kMrSuites) / sizeof(kMrSuites[0]));
  br_ssl_engine_set_hash(eng, br_md5_ID, &br_md5_vtable);
  br_ssl_engine_set_hash(eng, br_sha1_ID, &br_sha1_vtable);
  br_ssl_engine_set_hash(eng, br_sha256_ID, &br_sha256_vtable);
  br_ssl_engine_set_hash(eng, br_sha384_ID, &br_sha384_vtable);
  br_ssl_engine_set_prf10(eng, &br_tls10_prf);
  br_ssl_engine_set_prf_sha256(eng, &br_tls12_sha256_prf);
  br_ssl_engine_set_prf_sha384(eng, &br_tls12_sha384_prf);
  br_ssl_engine_set_aes_cbc(eng, &br_aes_small_cbcenc_vtable, &br_aes_small_cbcdec_vtable);
  br_ssl_engine_set_cbc(eng, &br_sslrec_in_cbc_vtable, &br_sslrec_out_cbc_vtable);
  br_ssl_engine_set_aes_ctr(eng, &br_aes_small_ctr_vtable);
  br_ssl_engine_set_ghash(eng, &br_ghash_ctmul32);
  br_ssl_engine_set_gcm(eng, &br_sslrec_in_gcm_vtable, &br_sslrec_out_gcm_vtable);
  br_ssl_engine_set_ec(eng, &br_ec_p256_m15);
  if (MR_C_MQTT == c->kind) {
    br_ssl_server_set_single_rsa(sc, MrMqttChain, 1, &MrMqttKey, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN,
                                 br_rsa_i15_private, br_rsa_i15_pkcs1_sign);
  } else {
    br_ssl_server_set_single_rsa(sc, MrHttpsChain, 1, &MrHttpsKey, BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN,
                                 br_rsa_i15_private, br_rsa_i15_pkcs1_sign);
  }
  br_ssl_engine_set_buffers_bidi(eng, c->ibuf, MR_IBUF, c->obuf, MR_OBUF);
  return br_ssl_server_reset(sc);
}

static bool MrWouldBlock(void) { return (EAGAIN == errno) || (EWOULDBLOCK == errno); }

// Push pending TLS records to the socket; waits up to wait_ms for a full send buffer.
static bool MrPushRecords(MrConn *c, uint32_t wait_ms) {
  br_ssl_engine_context *eng = &c->sc->eng;
  uint32_t t0 = millis();
  while (br_ssl_engine_current_state(eng) & BR_SSL_SENDREC) {
    size_t len;
    unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
    int n = lwip_send(c->fd, buf, len, MSG_DONTWAIT);
    if (n > 0) {
      br_ssl_engine_sendrec_ack(eng, n);
      continue;
    }
    if ((n < 0) && MrWouldBlock() && !MrExpired(t0, wait_ms)) {
      vTaskDelay(1);
      continue;
    }
    return (n < 0) && MrWouldBlock();
  }
  return true;
}

// Moves records in both directions without blocking. false: the connection is gone.
static bool MrPump(MrConn *c) {
  br_ssl_engine_context *eng = &c->sc->eng;
  for (uint32_t guard = 0; guard < 64; guard++) {
    unsigned st = br_ssl_engine_current_state(eng);
    if (st & BR_SSL_CLOSED) {
      int err = br_ssl_engine_last_error(eng);
      if (err != BR_ERR_OK) {
        MrS.tls_errors++;
        MrS.tls_last_err = err;
        AddLog(LOG_LEVEL_INFO, PSTR("MRY: TLS with %s ended, BearSSL error %d%s"), c->peer, err,
               c->hs_done ? "" : " (handshake)");
      }
      return false;
    }
    if (st & BR_SSL_SENDREC) {
      size_t len;
      unsigned char *buf = br_ssl_engine_sendrec_buf(eng, &len);
      int n = lwip_send(c->fd, buf, len, MSG_DONTWAIT);
      if (n > 0) { br_ssl_engine_sendrec_ack(eng, n); continue; }
      if ((n < 0) && MrWouldBlock()) { break; }
      return false;
    }
    if (!c->hs_done && (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))) {
      c->hs_done = true;
      c->t_last = millis();
      AddLog(LOG_LEVEL_DEBUG, PSTR("MRY: TLS handshake with %s done (version %04X, suite %04X)"), c->peer,
             br_ssl_engine_get_version(eng), eng->session.cipher_suite);
    }
    if (st & BR_SSL_RECVAPP) {
      size_t len;
      unsigned char *buf = br_ssl_engine_recvapp_buf(eng, &len);
      size_t room = c->rx_cap - c->rx_len;
      if (!room) { break; }                    // the application layer consumes first
      size_t take = (len < room) ? len : room;
      memcpy(c->rx + c->rx_len, buf, take);
      c->rx_len += take;
      br_ssl_engine_recvapp_ack(eng, take);
      c->t_last = millis();
      continue;
    }
    if (st & BR_SSL_RECVREC) {
      size_t len;
      unsigned char *buf = br_ssl_engine_recvrec_buf(eng, &len);
      int n = lwip_recv(c->fd, buf, len, MSG_DONTWAIT);
      if (n > 0) { br_ssl_engine_recvrec_ack(eng, n); continue; }
      if ((n < 0) && MrWouldBlock()) { break; }
      return false;                            // closed by the peer, or an error
    }
    break;
  }
  return true;
}

static bool MrTlsWrite(MrConn *c, const uint8_t *data, size_t len, bool flush) {
  br_ssl_engine_context *eng = &c->sc->eng;
  size_t off = 0;
  uint32_t t0 = millis();
  while (off < len) {
    unsigned st = br_ssl_engine_current_state(eng);
    if ((st & BR_SSL_CLOSED) || MrExpired(t0, 3000)) { return false; }
    if (st & BR_SSL_SENDAPP) {
      size_t alen;
      unsigned char *buf = br_ssl_engine_sendapp_buf(eng, &alen);
      size_t n = (len - off < alen) ? len - off : alen;
      memcpy(buf, data + off, n);
      br_ssl_engine_sendapp_ack(eng, n);
      off += n;
      continue;
    }
    if (st & BR_SSL_SENDREC) {
      if (!MrPushRecords(c, 100)) { return false; }
      continue;
    }
    vTaskDelay(1);
  }
  if (flush) {
    br_ssl_engine_flush(eng, 0);
    return MrPushRecords(c, 3000);
  }
  return true;
}

static int MrConnCount(uint8_t kind) {
  int n = 0;
  for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
    if (MrConns[i] && (!kind || (MrConns[i]->kind == kind))) { n++; }
  }
  return n;
}

static void MrClientsUpdate(void) {
  int n = 0;
  for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
    if (MrConns[i] && (MR_C_MQTT == MrConns[i]->kind) && MrConns[i]->mqtt_up) { n++; }
  }
  MrS.clients = n;
}

static void MrConnFree(uint32_t i, bool graceful) {
  MrConn *c = MrConns[i];
  if (!c) { return; }
  if (graceful && c->hs_done && c->sc) {
    // close_notify, never a reset: the firmware's only clean exit from its receive loop
    br_ssl_engine_close(&c->sc->eng);
    MrPushRecords(c, 500);
  }
  if (c->fd >= 0) { lwip_close(c->fd); }
  if ((MR_C_MQTT == c->kind) && c->mqtt_up) {
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT client '%s' (%s) disconnected"), c->client_id, c->peer);
  }
  free(c->sc);
  free(c->ibuf);
  free(c->obuf);
  free(c->rx);
  free(c);
  MrConns[i] = nullptr;
  MrClientsUpdate();
}

static void MrConnNew(int fd, uint8_t kind, const char *peer) {
  int slot = -1;
  for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
    if (!MrConns[i]) { slot = i; break; }
  }
  MrConn *c = (slot >= 0) ? (MrConn*)MrAlloc(sizeof(MrConn)) : nullptr;
  if (c) {
    c->kind = kind;
    c->fd = fd;
    strlcpy(c->peer, peer, sizeof(c->peer));
    c->rx_cap = (MR_C_MQTT == kind) ? MR_MQTT_RX : MR_HTTPS_RX;
    c->sc = (br_ssl_server_context*)MrAlloc(sizeof(br_ssl_server_context));
    c->ibuf = (uint8_t*)special_malloc(MR_IBUF);
    c->obuf = (uint8_t*)special_malloc(MR_OBUF);
    c->rx = (uint8_t*)special_malloc(c->rx_cap);
    c->t_open = c->t_last = millis();
    MrConns[slot] = c;
    if (c->sc && c->ibuf && c->obuf && c->rx && MrTlsSetup(c)) { return; }
    AddLog(LOG_LEVEL_ERROR, PSTR("MRY: TLS setup for %s failed (memory?)"), peer);
    MrConnFree(slot, false);
    return;
  }
  lwip_close(fd);
}

/*********************************************************************************************\
 * MQTT broker (3.1 / 3.1.1, QoS 0 delivery, no retained messages)
\*********************************************************************************************/

static bool MrTopicMatch(const char *filter, const char *topic) {
  while (*filter) {
    if ('#' == *filter) { return true; }
    if ('+' == *filter) {
      while (*topic && ('/' != *topic)) { topic++; }
      filter++;
      continue;
    }
    if (*filter != *topic) { return false; }
    filter++;
    topic++;
  }
  return !*topic;
}

static size_t MrVarint(uint8_t *out, uint32_t v) {
  size_t n = 0;
  do {
    uint8_t b = v & 0x7F;
    v >>= 7;
    if (v) { b |= 0x80; }
    out[n++] = b;
  } while (v);
  return n;
}

// Delivers to every connected client with a matching subscription; returns their number.
static int MrLocalPublish(const char *topic, const uint8_t *payload, size_t plen) {
  size_t tl = strlen(topic);
  uint8_t hdr[8 + MR_TOPIC_MAX];
  if (tl >= MR_TOPIC_MAX) { return 0; }
  size_t h = 0;
  hdr[h++] = 0x30;
  h += MrVarint(hdr + h, 2 + tl + plen);
  hdr[h++] = tl >> 8;
  hdr[h++] = tl & 0xFF;
  memcpy(hdr + h, topic, tl);
  h += tl;
  int sent = 0;
  for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
    MrConn *c = MrConns[i];
    if (!c || (MR_C_MQTT != c->kind) || !c->mqtt_up) { continue; }
    bool match = false;
    for (uint32_t s = 0; (s < c->nsubs) && !match; s++) {
      match = MrTopicMatch(c->subs[s], topic);
    }
    if (!match) { continue; }
    if (MrTlsWrite(c, hdr, h, false) && MrTlsWrite(c, payload, plen, true)) {
      sent++;
    } else {
      MrConnFree(i, false);
    }
  }
  return sent;
}

static void MrOnBatteryMessage(const char *topic, const uint8_t *payload, size_t plen) {
  // Only what the battery itself publishes goes to the home broker.
  if (!strstr(topic, "/device/")) { return; }
  MrS.dev_msgs++;
  MrS.dev_last = millis();
  MrS.dev_seen = 1;
  char ext[MR_TOPIC_MAX];
  MrTranslate(topic, ext, sizeof(ext), true);
  MrPublishUp(ext, payload, plen);
}

static bool MrSend2(MrConn *c, uint8_t type, uint16_t pid) {
  uint8_t pkt[4] = { type, 0x02, (uint8_t)(pid >> 8), (uint8_t)(pid & 0xFF) };
  return MrTlsWrite(c, pkt, 4, true);
}

static uint16_t MrU16(const uint8_t *p) { return (p[0] << 8) | p[1]; }

// One complete packet; false closes the connection.
static bool MrMqttPacket(uint32_t idx, uint8_t hdr, const uint8_t *b, uint32_t len) {
  MrConn *c = MrConns[idx];
  uint8_t type = hdr >> 4;
  if (!c->mqtt_up && (type != 1)) { return false; }
  switch (type) {
    case 1: {                                  // CONNECT
      if (len < 12) { return false; }
      uint32_t pos = 2 + MrU16(b);             // protocol name ("MQTT" or "MQIsdp")
      if (pos + 6 > len) { return false; }
      c->keepalive = MrU16(b + pos + 2);
      pos += 4;
      uint32_t cl = MrU16(b + pos);
      pos += 2;
      if (pos + cl > len) { return false; }
      if (cl >= sizeof(c->client_id)) { cl = sizeof(c->client_id) - 1; }
      memcpy(c->client_id, b + pos, cl);
      c->client_id[cl] = 0;
      for (uint32_t i = 0; i < MR_MAX_CONN; i++) {  // a reconnect replaces the old session
        MrConn *o = MrConns[i];
        if ((i != idx) && o && (MR_C_MQTT == o->kind) && o->mqtt_up && c->client_id[0] &&
            !strcmp(o->client_id, c->client_id)) {
          MrConnFree(i, false);
        }
      }
      c->mqtt_up = true;
      MrClientsUpdate();
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT client '%s' connected from %s (keepalive %u s)"), c->client_id,
             c->peer, c->keepalive);
      uint8_t ack[4] = { 0x20, 0x02, 0x00, 0x00 };
      return MrTlsWrite(c, ack, 4, true);
    }
    case 3: {                                  // PUBLISH
      uint8_t qos = (hdr >> 1) & 3;
      if (len < 2) { return false; }
      uint32_t tl = MrU16(b);
      uint32_t pos = 2 + tl;
      if ((pos > len) || (tl >= MR_TOPIC_MAX)) { return false; }
      char topic[MR_TOPIC_MAX];
      memcpy(topic, b + 2, tl);
      topic[tl] = 0;
      uint16_t pid = 0;
      if (qos) {
        if (pos + 2 > len) { return false; }
        pid = MrU16(b + pos);
        pos += 2;
      }
      if ((1 == qos) && !MrSend2(c, 0x40, pid)) { return false; }  // PUBACK
      if ((2 == qos) && !MrSend2(c, 0x50, pid)) { return false; }  // PUBREC
      MrOnBatteryMessage(topic, b + pos, len - pos);
      MrLocalPublish(topic, b + pos, len - pos);
      return (MrConns[idx] == c);
    }
    case 6:                                    // PUBREL
      return (len >= 2) ? MrSend2(c, 0x70, MrU16(b)) : false;   // PUBCOMP
    case 8: {                                  // SUBSCRIBE
      if (len < 2) { return false; }
      uint16_t pid = MrU16(b);
      uint8_t ack[4 + 16];
      uint32_t n = 0;
      uint32_t pos = 2;
      while ((pos + 2 < len) && (n < 16)) {
        uint32_t fl = MrU16(b + pos);
        pos += 2;
        if (pos + fl + 1 > len) { return false; }
        uint8_t granted = 0x80;
        if (fl < MR_TOPIC_MAX) {
          char f[MR_TOPIC_MAX];
          memcpy(f, b + pos, fl);
          f[fl] = 0;
          uint32_t s = 0;
          while ((s < c->nsubs) && strcmp(c->subs[s], f)) { s++; }
          if ((s == c->nsubs) && (c->nsubs < MR_MAX_SUBS)) {
            strlcpy(c->subs[c->nsubs++], f, MR_TOPIC_MAX);
          }
          if (s < c->nsubs) {
            uint8_t q = b[pos + fl] & 3;
            granted = (q > 1) ? 1 : q;          // delivered as QoS 0, allowed below the grant
            AddLog(LOG_LEVEL_DEBUG, PSTR("MRY: '%s' subscribed to %s"), c->client_id, f);
          }
        }
        ack[4 + n++] = granted;
        pos += fl + 1;
      }
      ack[0] = 0x90;
      ack[1] = 2 + n;
      ack[2] = pid >> 8;
      ack[3] = pid & 0xFF;
      return MrTlsWrite(c, ack, 4 + n, true);
    }
    case 10: {                                 // UNSUBSCRIBE
      if (len < 2) { return false; }
      uint16_t pid = MrU16(b);
      uint32_t pos = 2;
      while (pos + 2 <= len) {
        uint32_t fl = MrU16(b + pos);
        pos += 2;
        if (pos + fl > len) { return false; }
        for (uint32_t s = 0; s < c->nsubs; s++) {
          if ((strlen(c->subs[s]) == fl) && !strncmp(c->subs[s], (const char*)b + pos, fl)) {
            memmove(c->subs[s], c->subs[s + 1], (c->nsubs - s - 1) * MR_TOPIC_MAX);
            c->nsubs--;
            break;
          }
        }
        pos += fl;
      }
      return MrSend2(c, 0xB0, pid);           // UNSUBACK
    }
    case 12: {                                 // PINGREQ
      uint8_t resp[2] = { 0xD0, 0x00 };
      return MrTlsWrite(c, resp, 2, true);
    }
    case 14:                                   // DISCONNECT
      return false;
  }
  return true;                                 // PUBACK, PUBREC, PUBCOMP: nothing to do
}

static void MrMqttProcess(uint32_t idx) {
  MrConn *c = MrConns[idx];
  while (c && (c->rx_len >= 2)) {
    uint32_t rl = 0;
    uint32_t shift = 0;
    uint32_t p = 1;
    bool done = false;
    while ((p < c->rx_len) && (p <= 4)) {
      uint8_t v = c->rx[p++];
      rl |= (uint32_t)(v & 0x7F) << shift;
      shift += 7;
      if (!(v & 0x80)) { done = true; break; }
    }
    if (!done) {
      if (p > 4) { MrConnFree(idx, false); }   // malformed length
      return;
    }
    uint32_t total = p + rl;
    if (total > c->rx_cap) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT packet of %u bytes from %s too large"), total, c->peer);
      MrConnFree(idx, false);
      return;
    }
    if (c->rx_len < total) { return; }
    bool keep = MrMqttPacket(idx, c->rx[0], c->rx + p, rl);
    if (MrConns[idx] != c) { return; }         // freed meanwhile
    if (!keep) {
      MrConnFree(idx, false);
      return;
    }
    memmove(c->rx, c->rx + total, c->rx_len - total);
    c->rx_len -= total;
  }
}

/*********************************************************************************************\
 * Port 443: the cloud over TLS (Venus telemetry upload)
\*********************************************************************************************/

static bool MrHeaderValue(const char *hdrs, size_t hl, const char *name, char *out, size_t cap) {
  size_t nl = strlen(name);
  for (size_t pos = 0; pos + nl < hl; pos++) {
    if (pos && (hdrs[pos - 1] != '\n')) { continue; }
    if (strncasecmp(hdrs + pos, name, nl) || (hdrs[pos + nl] != ':')) { continue; }
    size_t v = pos + nl + 1;
    while ((v < hl) && ((' ' == hdrs[v]) || ('\t' == hdrs[v]))) { v++; }
    size_t e = v;
    while ((e < hl) && (hdrs[e] != '\r') && (hdrs[e] != '\n')) { e++; }
    size_t l = e - v;
    if (l >= cap) { l = cap - 1; }
    memcpy(out, hdrs + v, l);
    out[l] = 0;
    return true;
  }
  return false;
}

static void MrHttpsProcess(uint32_t idx) {
  MrConn *c = MrConns[idx];
  if (c->answered) {                           // held open until close_at, input is ignored
    c->rx_len = 0;
    return;
  }
  size_t he = 0;
  bool found = false;
  for (size_t i = 0; i + 3 < c->rx_len; i++) {
    if (!memcmp(c->rx + i, "\r\n\r\n", 4)) { he = i; found = true; break; }
  }
  if (!found) {
    if (c->rx_len > MR_MAX_HEADER) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: Request headers from %s too long"), c->peer);
      MrConnFree(idx, false);
    }
    return;
  }
  const char *h = (const char*)c->rx;
  size_t cl = 0;
  bool complete = true;
  char val[32];
  if (MrHeaderValue(h, he, "Content-Length", val, sizeof(val))) {
    cl = strtoul(val, nullptr, 10);
  } else if (MrHeaderValue(h, he, "Transfer-Encoding", val, sizeof(val))) {
    // No firmware has been seen to do this; the reply still goes out.
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: Unsupported Transfer-Encoding '%s' from %s"), val, c->peer);
    complete = false;
  }
  size_t room = c->rx_cap - he - 4 - 1;
  size_t want = cl;
  if (want > room) {                           // keep the start, answer anyway, never decode it
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: Body from %s is %u bytes, keeping %u"), c->peer, cl, room);
    want = room;
    complete = false;
  }
  if (c->rx_len < he + 4 + want) { return; }  // wait for the rest

  char method[8];
  char url[160];
  size_t le = 0;
  while ((le < he) && (h[le] != '\r')) { le++; }
  size_t sp1 = 0;
  while ((sp1 < le) && (h[sp1] != ' ')) { sp1++; }
  size_t sp2 = sp1 + 1;
  while ((sp2 < le) && (h[sp2] != ' ') && (h[sp2] != '?')) { sp2++; }
  if ((sp1 >= le) || (sp1 >= sizeof(method)) || (sp2 - sp1 - 1 >= sizeof(url))) {
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: Malformed request from %s"), c->peer);
    MrConnFree(idx, false);
    return;
  }
  memcpy(method, h, sp1);
  method[sp1] = 0;
  memcpy(url, h + sp1 + 1, sp2 - sp1 - 1);
  url[sp2 - sp1 - 1] = 0;
  char *body = (char*)c->rx + he + 4;
  body[want] = 0;                              // room was reduced by one for this

  MrResp res;
  MrDispatch(method, url, body, want, complete, c->peer, nullptr, &res);
  if ((404 == res.code) && MrC.accept_all) {
    // Opt-in: answering endpoints nobody has reverse-engineered can change what the battery does.
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: AcceptAll answers %s %s with code 0"), method, url);
    res.code = 200;
    res.ctype = "application/json";
    strlcpy(res.body, "{\"code\":0,\"message\":\"success\",\"data\":null}", sizeof(res.body));
    res.kong = true;
  }
  char raw[768];
  int n = MrBuildRaw(&res, raw, sizeof(raw));
  if (!MrTlsWrite(c, (const uint8_t*)raw, n, true)) {
    MrConnFree(idx, false);
    return;
  }
  c->rx_len = 0;
  c->answered = true;
  // The firmware discards a reply whose connection is cut before its own 20 s receive
  // timeout; hold it past that and end it with close_notify.
  c->close_at = millis() + MrC.hold_s * 1000;
  if (!MrC.hold_s) { MrConnFree(idx, true); }
}

/*********************************************************************************************\
 * DNS on the AP: every A query is answered with the AP address
\*********************************************************************************************/

static bool MrApInfo(uint32_t *ip, uint32_t *mask) {
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap || !esp_netif_is_netif_up(ap)) { return false; }
  esp_netif_ip_info_t info;
  if ((esp_netif_get_ip_info(ap, &info) != ESP_OK) || !info.ip.addr) { return false; }
  *ip = info.ip.addr;
  *mask = info.netmask.addr;
  return true;
}

static void MrDnsHandle(uint32_t apip, uint32_t apmask) {
  uint8_t buf[256];
  struct sockaddr_in from;
  socklen_t fl = sizeof(from);
  int len = lwip_recvfrom(MrDnsFd, buf, 192, MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
  if (len < 13) { return; }
  if ((from.sin_addr.s_addr & apmask) != (apip & apmask)) { return; }   // AP clients only
  uint16_t flags = MrU16(buf + 2);
  if ((flags & 0x8000) || (flags & 0x7800) || (MrU16(buf + 4) != 1)) { return; }  // one standard query
  char name[96];
  size_t nl = 0;
  int p = 12;
  while ((p < len) && buf[p]) {
    uint8_t ll = buf[p];
    if ((ll > 63) || (p + ll + 1 > len)) { return; }
    if (nl && (nl < sizeof(name) - 1)) { name[nl++] = '.'; }
    for (uint32_t i = 0; (i < ll) && (nl < sizeof(name) - 1); i++) { name[nl++] = buf[p + 1 + i]; }
    p += ll + 1;
  }
  name[nl] = 0;
  if ((p >= len) || buf[p]) { return; }
  p++;
  if (p + 4 > len) { return; }
  if ((MrU16(buf + p) != 1) || (MrU16(buf + p + 2) != 1)) { return; }   // A / IN only
  p += 4;
  buf[2] = 0x84;                               // response, authoritative
  buf[3] = 0x00;
  buf[6] = 0; buf[7] = 1;                      // one answer
  buf[8] = 0; buf[9] = 0;
  buf[10] = 0; buf[11] = 0;
  static const uint8_t ans[12] = { 0xC0, 0x0C, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x01, 0x2C, 0x00, 0x04 };
  memcpy(buf + p, ans, sizeof(ans));           // name pointer, A, IN, TTL 300, 4 bytes
  memcpy(buf + p + 12, &apip, 4);
  lwip_sendto(MrDnsFd, buf, p + 16, 0, (struct sockaddr*)&from, fl);
  MrS.dns++;
  AddLog(LOG_LEVEL_DEBUG_MORE, PSTR("MRY: DNS %s"), name);
}

/*********************************************************************************************\
 * UDP proxy: battery broadcasts on the AP <-> power meter on the home network
\*********************************************************************************************/

static int MrUdpSocket(uint16_t port) {
  int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd < 0) { return -1; }
  int one = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  lwip_setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (lwip_bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) {
    lwip_close(fd);
    return -1;
  }
  lwip_ioctl(fd, FIONBIO, &one);
  return fd;
}

static void MrUdpFromAp(MrUdp *u, uint32_t apip, uint32_t apmask, uint8_t *buf) {
  struct sockaddr_in from;
  socklen_t fl = sizeof(from);
  int len = lwip_recvfrom(u->ap_fd, buf, 1500, MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
  if (len <= 0) { return; }
  if ((from.sin_addr.s_addr & apmask) != (apip & apmask)) {   // home network traffic, not ours
    MrS.udp_dropped++;
    return;
  }
  uint16_t port = ntohs(from.sin_port);
  MrSess *free_s = nullptr;
  MrSess *oldest = &u->sess[0];
  MrSess *s = nullptr;
  for (uint32_t i = 0; i < MR_MAX_SESS; i++) {
    MrSess *t = &u->sess[i];
    if (t->ip && (t->ip == from.sin_addr.s_addr) && (t->port == port)) { s = t; break; }
    if (!t->ip && !free_s) { free_s = t; }
    if ((uint32_t)(millis() - t->last) > (uint32_t)(millis() - oldest->last)) { oldest = t; }
  }
  if (!s) { s = free_s ? free_s : oldest; }
  s->ip = from.sin_addr.s_addr;
  s->port = port;
  s->last = millis();
  struct sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(u->port);
  to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  if (lwip_sendto(u->sta_fd, buf, len, 0, (struct sockaddr*)&to, sizeof(to)) < 0) {
    MrS.udp_dropped++;
  } else {
    MrS.udp_to_sta++;
  }
}

static void MrUdpFromSta(MrUdp *u, uint8_t *buf) {
  struct sockaddr_in from;
  socklen_t fl = sizeof(from);
  int len = lwip_recvfrom(u->sta_fd, buf, 1500, MSG_DONTWAIT, (struct sockaddr*)&from, &fl);
  if (len <= 0) { return; }
  MrS.udp_last = millis();
  MrS.udp_seen = 1;
  bool any = false;
  for (uint32_t i = 0; i < MR_MAX_SESS; i++) {
    MrSess *s = &u->sess[i];
    if (!s->ip) { continue; }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(s->port);
    to.sin_addr.s_addr = s->ip;
    if (lwip_sendto(u->ap_fd, buf, len, 0, (struct sockaddr*)&to, sizeof(to)) < 0) {
      MrS.udp_dropped++;
    } else {
      MrS.udp_to_ap++;
      s->last = millis();
      any = true;
    }
  }
  if (!any) { MrS.udp_dropped++; }
}

static void MrUdpClose(void) {
  for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
    if (MrU[i].ap_fd >= 0) { lwip_close(MrU[i].ap_fd); }
    if (MrU[i].sta_fd >= 0) { lwip_close(MrU[i].sta_fd); }
    memset(&MrU[i], 0, sizeof(MrU[i]));
    MrU[i].ap_fd = -1;
    MrU[i].sta_fd = -1;
  }
}

/*********************************************************************************************\
 * The network task
\*********************************************************************************************/

static int MrListen(uint16_t port) {
  int fd = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) { return -1; }
  int one = 1;
  lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if ((lwip_bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) || (lwip_listen(fd, 2) != 0)) {
    lwip_close(fd);
    return -1;
  }
  lwip_ioctl(fd, FIONBIO, &one);
  return fd;
}

static void MrAccept(int lfd, uint8_t kind) {
  struct sockaddr_in a;
  socklen_t al = sizeof(a);
  int fd = lwip_accept(lfd, (struct sockaddr*)&a, &al);
  if (fd < 0) { return; }
  char peer[16];
  MrIpStr(a.sin_addr.s_addr, peer);
  int one = 1;
  lwip_ioctl(fd, FIONBIO, &one);
  lwip_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  int budget = UsePSRAM() ? MR_MAX_CONN : MR_MAX_CONN_NOPS;
  if ((MR_C_MQTT == kind) && (MrConnCount(MR_C_MQTT) >= MR_MAX_MQTT)) {
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT connection from %s refused, %d clients max"), peer, MR_MAX_MQTT);
    lwip_close(fd);
    return;
  }
  if (MrConnCount(0) >= budget) {
    // Held HTTPS connections only wait for their clean close: end the oldest early.
    int oldest = -1;
    for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
      MrConn *c = MrConns[i];
      if (c && (MR_C_HTTPS == c->kind) && c->answered &&
          ((oldest < 0) || ((int32_t)(c->close_at - MrConns[oldest]->close_at) < 0))) {
        oldest = i;
      }
    }
    if (oldest < 0) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: Connection from %s refused, no free slot"), peer);
      lwip_close(fd);
      return;
    }
    MrConnFree(oldest, true);
  }
  MrConnNew(fd, kind, peer);
}

static void MrNetOpen(bool ap) {
  if (MrMqttFd < 0) {
    MrMqttFd = MrListen(MR_MQTT_PORT);
    if (MrMqttFd >= 0) { AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT broker (TLS) on port %d"), MR_MQTT_PORT); }
  }
  MrS.broker_up = (MrMqttFd >= 0);
  if (MrC.https && (MrHttpsFd < 0)) {
    MrHttpsFd = MrListen(MR_HTTPS_PORT);
    if (MrHttpsFd >= 0) { AddLog(LOG_LEVEL_INFO, PSTR("MRY: Cloud HTTPS on port %d"), MR_HTTPS_PORT); }
  }
  MrS.https_up = (MrHttpsFd >= 0);
  if (ap && (MrDnsFd < 0)) {
    MrDnsFd = MrUdpSocket(MR_DNS_PORT);
    if (MrDnsFd >= 0) { AddLog(LOG_LEVEL_INFO, PSTR("MRY: DNS for the AP on port %d"), MR_DNS_PORT); }
  }
  for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
    MrUdp *u = &MrU[i];
    if (!u->port || (u->ap_fd >= 0)) { continue; }
    u->ap_fd = MrUdpSocket(u->port);
    u->sta_fd = MrUdpSocket(0);
    if ((u->ap_fd < 0) || (u->sta_fd < 0)) {
      if (u->ap_fd >= 0) { lwip_close(u->ap_fd); }
      if (u->sta_fd >= 0) { lwip_close(u->sta_fd); }
      u->ap_fd = u->sta_fd = -1;
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: UDP proxy on port %d failed, retrying"), u->port);
    } else {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: UDP proxy on port %d"), u->port);
    }
  }
}

static void MrDrainDown(void) {
  MrMsg *m;
  while (MrDownQ && (xQueueReceive(MrDownQ, &m, 0) == pdTRUE)) {
    if (MrLocalPublish(m->topic, m->payload, m->len) > 0) {
      MrS.cmds++;
    } else {
      MrS.cmds_lost++;
      AddLog(LOG_LEVEL_DEBUG, PSTR("MRY: No battery subscribed to %s"), m->topic);
    }
    free(m);
  }
}

static void MrTimeouts(uint32_t i) {
  MrConn *c = MrConns[i];
  if (!c->hs_done) {
    if (MrExpired(c->t_open, MR_HS_TIMEOUT)) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: TLS handshake with %s timed out"), c->peer);
      MrConnFree(i, false);
    }
    return;
  }
  if (MR_C_HTTPS == c->kind) {
    if (c->answered) {
      if ((int32_t)(millis() - c->close_at) >= 0) { MrConnFree(i, true); }
    } else if (MrExpired(c->t_open, MR_REQ_TIMEOUT)) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: No complete request from %s"), c->peer);
      MrConnFree(i, false);
    }
    return;
  }
  if (!c->mqtt_up) {
    if (MrExpired(c->t_open, MR_REQ_TIMEOUT)) { MrConnFree(i, false); }
  } else if (c->keepalive) {
    if (MrExpired(c->t_last, c->keepalive * 1500)) {
      AddLog(LOG_LEVEL_INFO, PSTR("MRY: MQTT client '%s' keepalive expired"), c->client_id);
      MrConnFree(i, false);
    }
  } else if (MrExpired(c->t_last, 3600000)) {
    MrConnFree(i, false);
  }
}

static void MrTask(void *arg) {
  static uint8_t udpbuf[1500];
  uint32_t last_open = millis() - 10000;
  uint32_t last_sweep = millis();
  for (;;) {
    uint32_t apip = 0, apmask = 0;
    bool ap = MrApInfo(&apip, &apmask);
    if (MrUdpRestart) {
      MrUdpRestart = false;
      MrUdpClose();
      MrLock();
      for (uint32_t i = 0; i < MR_MAX_UDP; i++) { MrU[i].port = MrC.udp[i]; }
      MrUnlock();
      last_open = millis() - 10000;
    }
    if (MrHttpsRestart) {
      MrHttpsRestart = false;
      if (MrHttpsFd >= 0) { lwip_close(MrHttpsFd); MrHttpsFd = -1; }
      MrS.https_up = 0;
      last_open = millis() - 10000;
    }
    if (!ap && (MrDnsFd >= 0)) {               // AP gone: its address may change
      lwip_close(MrDnsFd);
      MrDnsFd = -1;
    }
    if (MrExpired(last_open, 5000)) {
      last_open = millis();
      MrNetOpen(ap);
    }

    MrDrainDown();

    fd_set rs;
    FD_ZERO(&rs);
    int maxfd = -1;
    int fds[4 + 2 * MR_MAX_UDP + MR_MAX_CONN];
    uint32_t nf = 0;
    fds[nf++] = MrMqttFd;
    fds[nf++] = MrHttpsFd;
    fds[nf++] = MrDnsFd;
    for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
      fds[nf++] = MrU[i].ap_fd;
      fds[nf++] = MrU[i].sta_fd;
    }
    for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
      fds[nf++] = MrConns[i] ? MrConns[i]->fd : -1;
    }
    for (uint32_t i = 0; i < nf; i++) {
      if (fds[i] >= 0) {
        FD_SET(fds[i], &rs);
        if (fds[i] > maxfd) { maxfd = fds[i]; }
      }
    }
    int ready = 0;
    if (maxfd >= 0) {
      struct timeval tv;
      tv.tv_sec = 0;
      tv.tv_usec = 20000;
      ready = lwip_select(maxfd + 1, &rs, nullptr, nullptr, &tv);
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (ready > 0) {
      if ((MrMqttFd >= 0) && FD_ISSET(MrMqttFd, &rs)) { MrAccept(MrMqttFd, MR_C_MQTT); }
      if ((MrHttpsFd >= 0) && FD_ISSET(MrHttpsFd, &rs)) { MrAccept(MrHttpsFd, MR_C_HTTPS); }
      if ((MrDnsFd >= 0) && FD_ISSET(MrDnsFd, &rs) && ap) { MrDnsHandle(apip, apmask); }
      for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
        MrUdp *u = &MrU[i];
        if ((u->ap_fd >= 0) && FD_ISSET(u->ap_fd, &rs)) {
          if (ap) {
            MrUdpFromAp(u, apip, apmask, udpbuf);
          } else {
            lwip_recv(u->ap_fd, udpbuf, sizeof(udpbuf), MSG_DONTWAIT);   // discard
          }
        }
        if ((u->sta_fd >= 0) && FD_ISSET(u->sta_fd, &rs)) { MrUdpFromSta(u, udpbuf); }
      }
    }

    for (uint32_t i = 0; i < MR_MAX_CONN; i++) {
      MrConn *c = MrConns[i];
      if (!c) { continue; }
      if (!MrPump(c)) {
        MrConnFree(i, false);
        continue;
      }
      if (c->hs_done) {
        if (MR_C_MQTT == c->kind) {
          MrMqttProcess(i);
        } else {
          MrHttpsProcess(i);
        }
      }
      if (MrConns[i] == c) { MrTimeouts(i); }
    }

    if (MrExpired(last_sweep, 5000)) {
      last_sweep = millis();
      for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
        for (uint32_t s = 0; s < MR_MAX_SESS; s++) {
          if (MrU[i].sess[s].ip && MrExpired(MrU[i].sess[s].last, MR_UDP_SESSION)) { MrU[i].sess[s].ip = 0; }
        }
      }
    }
  }
}

/*********************************************************************************************\
 * DHCP on the AP hands out our own address as DNS server
\*********************************************************************************************/

static void MrDhcpDns(void) {
  // The Range Extender offers the home network's DNS; the battery must ask us instead.
  esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (!ap || !esp_netif_is_netif_up(ap)) { return; }
  esp_netif_ip_info_t info;
  if ((esp_netif_get_ip_info(ap, &info) != ESP_OK) || !info.ip.addr) { return; }
  esp_netif_dns_info_t dns;
  memset(&dns, 0, sizeof(dns));
  if ((esp_netif_get_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) && (dns.ip.u_addr.ip4.addr == info.ip.addr)) {
    return;
  }
  esp_netif_dhcps_stop(ap);
  memset(&dns, 0, sizeof(dns));
  dns.ip.type = ESP_IPADDR_TYPE_V4;
  dns.ip.u_addr.ip4.addr = info.ip.addr;
  esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
  dhcps_offer_t offer = OFFER_DNS;
  esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
  esp_netif_dhcps_start(ap);
  char ip[16];
  MrIpStr(info.ip.addr, ip);
  AddLog(LOG_LEVEL_INFO, PSTR("MRY: AP DHCP now offers %s as DNS server"), ip);
}

/*********************************************************************************************\
 * Configuration file
\*********************************************************************************************/

static void MrCfgDefaults(void) {
  memset(&MrC, 0, sizeof(MrC));
  MrC.udp[0] = 1010;
  strlcpy(MrC.prefix, "marsrelay", sizeof(MrC.prefix));
  MrC.https = 1;
  MrC.requests = 1;
  MrC.telemetry = 1;
  MrC.hold_s = 25;
}

static void MrMapSet(MrMap *m, const char *device, const char *external) {
  strlcpy(m->device, device, sizeof(m->device));
  strlcpy(m->external, external, sizeof(m->external));
  m->external_enc[0] = 0;
  if (MrIsMac(external)) { MrEncryptMac(external, m->external_enc); }
}

static void MrCfgLoad(void) {
#ifdef USE_UFILESYS
  char *buf = (char*)calloc(2048, 1);
  if (!buf) { return; }
  if (TfsLoadFile(MR_CFG_FILE, (uint8_t*)buf, 2047)) {
    MrC.nmaps = 0;
    char *save = nullptr;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(nullptr, "\r\n", &save)) {
      char *eq = strchr(line, '=');
      if (!eq) { continue; }
      *eq = 0;
      const char *k = line;
      char *v = eq + 1;
      if (!strcmp(k, "udp")) {
        memset(MrC.udp, 0, sizeof(MrC.udp));
        char *s2 = nullptr;
        uint32_t n = 0;
        for (char *t = strtok_r(v, ",", &s2); t && (n < MR_MAX_UDP); t = strtok_r(nullptr, ",", &s2)) {
          MrC.udp[n++] = strtoul(t, nullptr, 10);
        }
      }
      else if (!strcmp(k, "prefix"))    { strlcpy(MrC.prefix, v, sizeof(MrC.prefix)); }
      else if (!strcmp(k, "https"))     { MrC.https = atoi(v) ? 1 : 0; }
      else if (!strcmp(k, "acceptall")) { MrC.accept_all = atoi(v) ? 1 : 0; }
      else if (!strcmp(k, "requests"))  { MrC.requests = atoi(v) ? 1 : 0; }
      else if (!strcmp(k, "telemetry")) { MrC.telemetry = atoi(v) ? 1 : 0; }
      else if (!strcmp(k, "hold"))      { MrC.hold_s = atoi(v); }
      else if (!strcmp(k, "map") && (MrC.nmaps < MR_MAX_MAPS)) {
        char *comma = strchr(v, ',');
        if (comma) {
          *comma = 0;
          MrMapSet(&MrC.maps[MrC.nmaps++], v, comma + 1);
        }
      }
    }
    AddLog(LOG_LEVEL_INFO, PSTR("MRY: Config loaded, %d ID mapping(s)"), MrC.nmaps);
  }
  free(buf);
#endif  // USE_UFILESYS
}

static void MrCfgSave(void) {
#ifdef USE_UFILESYS
  char *buf = (char*)calloc(2048, 1);
  if (!buf) { return; }
  MrLock();
  size_t n = snprintf(buf, 2048, "udp=%u,%u,%u\nprefix=%s\nhttps=%u\nacceptall=%u\nrequests=%u\ntelemetry=%u\nhold=%u\n",
                      MrC.udp[0], MrC.udp[1], MrC.udp[2], MrC.prefix, MrC.https, MrC.accept_all, MrC.requests,
                      MrC.telemetry, MrC.hold_s);
  for (uint32_t i = 0; (i < MrC.nmaps) && (n < 2048); i++) {
    n += snprintf(buf + n, 2048 - n, "map=%s,%s\n", MrC.maps[i].device, MrC.maps[i].external);
  }
  MrUnlock();
  if (n > 2047) { n = 2047; }
  TfsSaveFile(MR_CFG_FILE, (const uint8_t*)buf, n);
  free(buf);
#endif  // USE_UFILESYS
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

static bool MrValidId(const char *s) {
  static const char *const reserved[] = { "hame_energy", "marstek_energy", "App", "device", "ctrl" };
  if (!*s || (strlen(s) >= MR_ID_MAX) || strpbrk(s, "/+#,")) { return false; }
  for (uint32_t i = 0; i < 5; i++) {
    if (!strcmp(s, reserved[i])) { return false; }
  }
  return true;
}

static void MrRespMaps(void) {
  Response_P(PSTR("{\"%s\":["), XdrvMailbox.command);
  MrLock();
  for (uint32_t i = 0; i < MrC.nmaps; i++) {
    ResponseAppend_P(PSTR("%s{\"Device\":\"%s\",\"External\":\"%s\",\"Encrypted\":\"%s\"}"), i ? "," : "",
                     MrC.maps[i].device, MrC.maps[i].external, MrC.maps[i].external_enc);
  }
  MrUnlock();
  ResponseAppend_P(PSTR("]}"));
}

// MrMap                      list
// MrMap <device> <external>  add or replace (external may be the Bluetooth MAC)
// MrMap <device>             remove
// MrMap 0                    remove all
void CmndMrMap(void) {
  if (XdrvMailbox.data_len > 0) {
    char dev[MR_ID_MAX + 1];
    char ext[MR_ID_MAX + 1];
    dev[0] = ext[0] = 0;
    char *save = nullptr;
    char *t = strtok_r(XdrvMailbox.data, " ,", &save);
    if (t) { strlcpy(dev, t, sizeof(dev)); }
    t = strtok_r(nullptr, " ,", &save);
    if (t) { strlcpy(ext, t, sizeof(ext)); }
    MrLock();
    if (!strcmp(dev, "0") && !ext[0]) {
      MrC.nmaps = 0;
    } else if (MrValidId(dev) && (!ext[0] || MrValidId(ext))) {
      uint32_t i = 0;
      while ((i < MrC.nmaps) && strcmp(MrC.maps[i].device, dev)) { i++; }
      if (!ext[0]) {
        if (i < MrC.nmaps) {
          memmove(&MrC.maps[i], &MrC.maps[i + 1], (MrC.nmaps - i - 1) * sizeof(MrMap));
          MrC.nmaps--;
        }
      } else if (i < MR_MAX_MAPS) {
        MrMapSet(&MrC.maps[i], dev, ext);
        if (i == MrC.nmaps) { MrC.nmaps++; }
      }
    }
    MrUnlock();
    MrCfgSave();
  }
  MrRespMaps();
}

// MrUdp 1010[,2220[,...]]  power meter ports to proxy, 0 = off
void CmndMrUdp(void) {
  if (XdrvMailbox.data_len > 0) {
    MrLock();
    memset(MrC.udp, 0, sizeof(MrC.udp));
    char *save = nullptr;
    uint32_t n = 0;
    for (char *t = strtok_r(XdrvMailbox.data, " ,", &save); t && (n < MR_MAX_UDP); t = strtok_r(nullptr, " ,", &save)) {
      uint32_t p = strtoul(t, nullptr, 10);
      if (p && (p < 65536) && (p != MR_DNS_PORT)) { MrC.udp[n++] = p; }
    }
    MrUnlock();
    MrCfgSave();
    MrUdpRestart = true;
  }
  Response_P(PSTR("{\"%s\":[%u,%u,%u]}"), XdrvMailbox.command, MrC.udp[0], MrC.udp[1], MrC.udp[2]);
}

void CmndMrPrefix(void) {
  if ((XdrvMailbox.data_len > 0) && (XdrvMailbox.data_len < sizeof(MrC.prefix)) && !strpbrk(XdrvMailbox.data, "+# ")) {
    MrLock();
    strlcpy(MrC.prefix, XdrvMailbox.data, sizeof(MrC.prefix));
    MrUnlock();
    MrCfgSave();
  }
  ResponseCmndChar(MrC.prefix);
}

static void MrCmndFlag(uint8_t *flag) {
  if ((XdrvMailbox.payload >= 0) && (XdrvMailbox.payload <= 1)) {
    *flag = XdrvMailbox.payload;
    MrCfgSave();
  }
  ResponseCmndNumber(*flag);
}

void CmndMrHttps(void) {
  uint8_t old = MrC.https;
  MrCmndFlag(&MrC.https);
  if (old != MrC.https) { MrHttpsRestart = true; }
}

void CmndMrAcceptAll(void) { MrCmndFlag(&MrC.accept_all); }
void CmndMrRequests(void)  { MrCmndFlag(&MrC.requests); }
void CmndMrTelemetry(void) { MrCmndFlag(&MrC.telemetry); }

// MrHold <s>  how long an answered HTTPS connection is held open (> the battery's 20 s timeout)
void CmndMrHold(void) {
  if ((XdrvMailbox.payload >= 0) && (XdrvMailbox.payload <= 120)) {
    MrC.hold_s = XdrvMailbox.payload;
    MrCfgSave();
  }
  ResponseCmndNumber(MrC.hold_s);
}

static void MrAge(char *out, bool seen, uint32_t last) {
  if (seen) {
    snprintf(out, 12, "%u", (uint32_t)(millis() - last) / 1000);
  } else {
    strcpy(out, "null");
  }
}

static void MrJsonStatus(void) {
  char dev_age[12], http_age[12], udp_age[12];
  MrAge(dev_age, MrS.dev_seen, MrS.dev_last);
  MrAge(http_age, MrS.http_seen, MrS.http_last);
  MrAge(udp_age, MrS.udp_seen, MrS.udp_last);
  ResponseAppend_P(PSTR("\"Marsrelay\":{\"Broker\":%u,\"Clients\":%u,\"DeviceMsgs\":%u,\"DeviceAge\":%s,"
                        "\"Commands\":%u,\"CommandsLost\":%u,\"PublishErrors\":%u,\"Https\":%u,\"HttpRequests\":%u,"
                        "\"HttpAge\":%s,\"Uploads\":%u,\"MeterRequests\":%u,\"MeterAnswers\":%u,\"MeterAge\":%s,"
                        "\"Dns\":%u,\"TlsErrors\":%u,\"TlsLastError\":%u}"),
                   MrS.broker_up, MrS.clients, MrS.dev_msgs, dev_age, MrS.cmds, MrS.cmds_lost, MrS.up_errors,
                   MrS.https_up, MrS.http_reqs, http_age, MrS.uploads, MrS.udp_to_sta, MrS.udp_to_ap, udp_age,
                   MrS.dns, MrS.tls_errors, MrS.tls_last_err);
}

void CmndMrStatus(void) {
  Response_P(PSTR("{"));
  MrJsonStatus();
  ResponseJsonEnd();
}

const char kMrCommands[] PROGMEM = "Mr|"
  "Status|Map|Udp|Prefix|Https|AcceptAll|Requests|Telemetry|Hold";

void (* const MrCommand[])(void) PROGMEM = {
  &CmndMrStatus, &CmndMrMap, &CmndMrUdp, &CmndMrPrefix, &CmndMrHttps, &CmndMrAcceptAll, &CmndMrRequests,
  &CmndMrTelemetry, &CmndMrHold };

/*********************************************************************************************\
 * Loop side
\*********************************************************************************************/

static void MrInit(void) {
  MrCfgDefaults();
  MrCfgLoad();
  MrKeysInit();
  for (uint32_t i = 0; i < MR_MAX_UDP; i++) {
    MrU[i].ap_fd = MrU[i].sta_fd = -1;
    MrU[i].port = MrC.udp[i];
  }
  MrLockH = xSemaphoreCreateMutex();
  MrUpQ = xQueueCreate(MR_QUEUE_LEN, sizeof(MrMsg*));
  MrDownQ = xQueueCreate(MR_QUEUE_LEN, sizeof(MrMsg*));
  if (!MrLockH || !MrUpQ || !MrDownQ) {
    AddLog(LOG_LEVEL_ERROR, PSTR("MRY: Init failed"));
    return;
  }
  if (xTaskCreate(MrTask, "marsrelay", MR_TASK_STACK, nullptr, 1, &MrTaskH) != pdPASS) {
    AddLog(LOG_LEVEL_ERROR, PSTR("MRY: Task start failed"));
    return;
  }
  MrReady = true;
  AddLog(LOG_LEVEL_INFO, PSTR("MRY: Marstek cloud emulation started"));
}

static void MrMqttSubscribe(void) {
  // Commands hm2mqtt sends to the battery (new and old firmware prefix).
  MqttSubscribe("marstek_energy/+/App/+/ctrl");
  MqttSubscribe("hame_energy/+/App/+/ctrl");
}

static bool MrMqttData(void) {
  const char *t = XdrvMailbox.topic;
  if (strncmp(t, "marstek_energy/", 15) && strncmp(t, "hame_energy/", 12)) { return false; }
  if (!strstr(t, "/App/") || !strstr(t, "/ctrl")) { return false; }
  char dev[MR_TOPIC_MAX];
  MrTranslate(t, dev, sizeof(dev), false);
  if (!MrQueuePush(MrDownQ, dev, (const uint8_t*)XdrvMailbox.data, XdrvMailbox.data_len)) {
    MrS.cmds_lost++;
  }
  return true;
}

#ifdef USE_WEBSERVER
static void MrAgeText(char *out, bool seen, uint32_t last) {
  if (seen) {
    snprintf(out, 16, "%u s ago", (uint32_t)(millis() - last) / 1000);
  } else {
    strcpy(out, "never");
  }
}

static void MrWebSensor(void) {
  char age[16];
  WSContentSend_P(PSTR("{s}Marsrelay broker{m}%s, %u client(s){e}"), MrS.broker_up ? "running" : "down", MrS.clients);
  MrAgeText(age, MrS.dev_seen, MrS.dev_last);
  WSContentSend_P(PSTR("{s}Battery messages{m}%u, %s{e}"), MrS.dev_msgs, age);
  WSContentSend_P(PSTR("{s}Commands relayed{m}%u{e}"), MrS.cmds);
  MrAgeText(age, MrS.http_seen, MrS.http_last);
  WSContentSend_P(PSTR("{s}Cloud requests{m}%u, %s{e}"), MrS.http_reqs, age);
  WSContentSend_P(PSTR("{s}Venus uploads{m}%u{e}"), MrS.uploads);
  MrAgeText(age, MrS.udp_seen, MrS.udp_last);
  WSContentSend_P(PSTR("{s}Power meter (UDP){m}%u / %u, %s{e}"), MrS.udp_to_sta, MrS.udp_to_ap, age);
}
#endif  // USE_WEBSERVER
