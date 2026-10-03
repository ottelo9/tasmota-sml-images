/*
  xdrv_10_ct002_registration.h - Marstek CT002 cloud registration V1.1

  This is deliberately firmware-side rather than Scripter source.  The page is
  only invoked by the user, credentials live for the duration of that request,
  and the running meter/emulator script never performs cloud logins.
*/

#ifndef _XDRV_10_CT002_REGISTRATION_H_
#define _XDRV_10_CT002_REGISTRATION_H_

#if defined(USE_SCRIPT_CT002_REGISTRATION) && defined(ESP8266) && defined(USE_WEBSERVER)

#include <MD5Builder.h>

static char CtRegStatus[128] = "Not executed";
static char CtRegMac[13] = "";
static char CtRegMail[97] = "";
static char CtRegHash[33] = "";
static uint8_t CtRegPending = 0;

const char HTTP_BTN_CT002_REG[] PROGMEM =
  "<p><form action='/ctreg' method='get'><button>Marstek CT002</button></form></p>";

static void CtRegSetStatus(const char *text, const char *mac = nullptr) {
  strlcpy(CtRegStatus, text ? text : "", sizeof(CtRegStatus));
  if (mac) {
    strlcpy(CtRegMac, mac, sizeof(CtRegMac));
  }
}

static bool CtRegHex(char c) {
  return ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f')) ||
         ((c >= 'A') && (c <= 'F'));
}

static bool CtRegMacAt(const String &body, int pos, char out[13]) {
  if ((pos < 0) || ((pos + 12) > (int)body.length())) { return false; }
  for (uint8_t i = 0; i < 12; i++) {
    char c = body[pos + i];
    if (!CtRegHex(c)) { return false; }
    out[i] = (c >= 'A' && c <= 'F') ? c + 32 : c;
  }
  out[12] = 0;
  return true;
}

// Find a managed HME-4 object.  A device occurs twice in the reply (devid and
// mac); restricting the match to its surrounding JSON object avoids adopting
// an HME-3 entry when both kinds exist in one account.
static bool CtRegFindExisting(const String &body, char out[13]) {
  int pos = 0;
  while ((pos = body.indexOf(F("02b250"), pos)) >= 0) {
    char candidate[13];
    if (CtRegMacAt(body, pos, candidate)) {
      int begin = body.lastIndexOf('{', pos);
      int end = body.indexOf('}', pos);
      if ((begin >= 0) && (end > pos)) {
        String object = body.substring(begin, end + 1);
        if (object.indexOf(F("HME-4")) >= 0) {
          strlcpy(out, candidate, 13);
          return true;
        }
      }
    }
    pos += 6;
  }
  return false;
}

static bool CtRegToken(const String &body, String &token) {
  int pos = body.indexOf(F("\"token\""));
  if (pos < 0) { return false; }
  pos = body.indexOf(':', pos + 7);
  if (pos < 0) { return false; }
  pos++;
  while ((pos < (int)body.length()) &&
         ((body[pos] == ' ') || (body[pos] == '\t') || (body[pos] == '"'))) {
    pos++;
  }
  int end = body.indexOf('"', pos);
  if ((end <= pos) || ((end - pos) > 95)) { return false; }
  token = body.substring(pos, end);
  return token.length() > 0;
}

static int CtRegHttpsGet(const String &url, const String &token, String &body) {
  if (TasmotaGlobal.global_state.network_down) { return -1; }

  BearSSL::WiFiClientSecure_light client(1024, 1024);
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) { return -2; }
  http.setUserAgent(F("Dart/2.19 (dart:io)"));
  http.addHeader(F("Accept"), F("application/json"));
  if (token.length()) { http.addHeader(F("token"), token); }

  int code = http.GET();
  if (code > 0) {
    body = http.getString();
    if (body.length() > 4096) { body.remove(4096); }
  }
  http.end();
  client.stop();
  return code;
}

static void CtRegForget(String &secret) {
  for (uint16_t i = 0; i < secret.length(); i++) { secret.setCharAt(i, 0); }
  secret = "";
}

static void CtRegClearCredentials(void) {
  memset(CtRegMail, 0, sizeof(CtRegMail));
  memset(CtRegHash, 0, sizeof(CtRegHash));
}

// Called in the request handler.  It hashes immediately but defers TLS until
// the WebServer has released its POST buffers; this matters on ESP8266 where a
// fragmented heap can otherwise make the BearSSL handshake fail.
static bool CtRegQueue(void) {
  String mail = Webserver->arg(F("mail"));
  String pass = Webserver->arg(F("pw"));
  mail.trim();

  if ((mail.length() < 5) || (mail.length() > 96) ||
      (pass.length() < 1) || (pass.length() > 96)) {
    CtRegSetStatus("Invalid email or password");
    CtRegForget(pass);
    return false;
  }

  MD5Builder md5;
  md5.begin();
  md5.add(pass);
  md5.calculate();
  String hash = md5.toString();
  CtRegForget(pass);

  strlcpy(CtRegMail, mail.c_str(), sizeof(CtRegMail));
  strlcpy(CtRegHash, hash.c_str(), sizeof(CtRegHash));
  CtRegForget(hash);
  mail = "";
  CtRegPending = 1;
  CtRegSetStatus("Registration started ...", "");
  return true;
}

static void CtRegRun(void) {
  String mail = CtRegMail;
  String hash = CtRegHash;
  CtRegClearCredentials();

  String url;
  url.reserve(320);
  url = F("https://eu.hamedata.com/app/Solar/v2_get_device.php?mailbox=");
  url += UrlEncode(mail.c_str());
  url += F("&pwd=");
  url += hash;

  String body;
  body.reserve(1024);
  String token;
  int code = CtRegHttpsGet(url, "", body);
  CtRegForget(hash);
  url = "";

  if (code != HTTP_CODE_OK) {
    snprintf_P(CtRegStatus, sizeof(CtRegStatus), PSTR("Login failed (HTTP %d)"), code);
    CtRegForget(mail);
    return;
  }
  if (body.indexOf(F("\"code\":4")) >= 0) {
    CtRegSetStatus("Login rejected - check password");
    CtRegForget(mail);
    return;
  }
  if (!CtRegToken(body, token)) {
    CtRegSetStatus("Login response did not contain a token");
    CtRegForget(mail);
    return;
  }

  char mac[13];
  if (CtRegFindExisting(body, mac)) {
    CtRegSetStatus("Existing CT002 found", mac);
    CtRegForget(token);
    CtRegForget(mail);
    return;
  }
  body = "";

  // Stable and idempotent for this ESP: locally administered Marstek prefix
  // plus the ESP8266 chip id (normally the last three bytes of its Wi-Fi MAC).
  snprintf_P(mac, sizeof(mac), PSTR("02b250%06x"), ESP_getChipId() & 0xFFFFFF);
  String suffix = String(mac + 8);

  url.reserve(400);
  url = F("https://eu.hamedata.com/app/Solar/v2_add_device.php?mailbox=");
  url += UrlEncode(mail.c_str());
  CtRegForget(mail);
  url += F("&devid="); url += mac;
  url += F("&mac="); url += mac;
  url += F("&type=HME-4&token="); url += token;
  url += F("&access=1&name=CT002&bluetooth_name=MST-SMR_"); url += suffix;
  url += F("&timeZone=Europe%2FBerlin&version=121&position=%7B%7D");

  code = CtRegHttpsGet(url, token, body);
  CtRegForget(token);
  url = "";
  bool accepted = (body.indexOf(F("\"code\":1")) >= 0) ||
                  (body.indexOf(F("\"code\":2")) >= 0) ||
                  (body.indexOf(F("\"code\":\"1\"")) >= 0) ||
                  (body.indexOf(F("\"code\":\"2\"")) >= 0);
  body = "";

  if ((code == HTTP_CODE_OK) && accepted) {
    CtRegSetStatus("CT002 registered successfully", mac);
    AddLog(LOG_LEVEL_INFO, PSTR("CT2: registered %s"), mac);
  } else {
    snprintf_P(CtRegStatus, sizeof(CtRegStatus), PSTR("Device creation rejected (HTTP %d)"), code);
  }
}

static void CtRegEverySecond(void) {
  if (CtRegPending != 1) { return; }
  CtRegPending = 2;
  CtRegRun();
  CtRegPending = 0;
}

static void HandleCt002Registration(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  if (!CtRegMac[0] && !CtRegPending && (Webserver->method() == HTTP_POST) &&
      Webserver->hasArg(F("register")) && CtRegQueue()) {
    Webserver->sendHeader(F("Location"), F("/ctreg"));
    Webserver->send(303, F("text/plain"), "");
    return;
  }

  WSContentStart_P(PSTR("Marstek CT002"));
  WSContentSendStyle();
  WSContentSend_P(PSTR(
    "<fieldset><legend><b>Marstek CT002</b></legend>"
    "<p>One-time Marstek account registration. Password hashed in RAM; not stored.</p>"
    "<p><b>Status:</b> %s</p>"), CtRegStatus);
  if (CtRegPending) {
    WSContentSend_P(PSTR("<meta http-equiv='refresh' content='2'>"));
  }
  if (CtRegMac[0]) {
    WSContentSend_P(PSTR(
      "<p><b>CT-MAC:</b> <code>%s</code></p>"
      "<p>Registration completed. No further cloud requests will be made.</p>"), CtRegMac);
  } else if (!CtRegPending) {
    WSContentSend_P(PSTR(
      "<form method='post' action='/ctreg' autocomplete='off'>"
      "<p><label>Marstek Email<br><input name='mail' type='email' maxlength='96' required></label></p>"
      "<p><label>Marstek Password<br><input name='pw' type='password' maxlength='96' required></label></p>"
      "<p><button name='register' value='1' type='submit'>Log in / Register CT002</button></p>"
      "</form><p><small>Local web page: Enter credentials in a trusted LAN only. "
      "After success, assign your batteries to this CT002.</small></p>"));
  }
  WSContentSend_P(PSTR("</fieldset>"));
  WSContentSpaceButton(BUTTON_MANAGEMENT);
  WSContentStop();
}

#endif  // USE_SCRIPT_CT002_REGISTRATION && ESP8266 && USE_WEBSERVER
#endif  // _XDRV_10_CT002_REGISTRATION_H_
