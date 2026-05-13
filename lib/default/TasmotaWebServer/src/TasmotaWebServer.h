/*
  TasmotaWebServer.h - lib/default/TasmotaWebServer/library.json

  Copyright (C) 2021  Theo Arends & Stephan Hadinger

  This library is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __TASMOTA_WEBSERVER__
#define __TASMOTA_WEBSERVER__

#ifdef ESP8266
#include <ESP8266WebServer.h>

class TasmotaWebServer : public ESP8266WebServer
{
public:
	TasmotaWebServer(int port) : ESP8266WebServer(port)
	{
	}

  bool isChunked(void) const { return _chunked; }
};
#endif // ESP8266

#ifdef ESP32
#include <WebServer.h>

// USE_HTTP_KEEPALIVE: per-handler opt-in HTTP/1.1 keep-alive on Tasmota's
// main WebServer. Defaults ON for ESP32. Compile-out by adding
// `#undef USE_HTTP_KEEPALIVE` to user_config_override.h.
//
// Why this exists: the Arduino-ESP32 WebServer in handleClient() destroys
// _currentClient after every request (WebServer.cpp:482-487 in framework
// 3.x), citing a Chrome compat issue from espressif/arduino-esp32#3652.
// That makes Tasmota incompatible with battery storages whose firmware
// expects an EcoTracker-style kept-alive TCP connection — most prominently
// the Jackery Homepower 2000 Ultra (sdeigm/uni-meter#265,
// ottelo/tasmota-sml-script#24), but the Growatt NOAH 2000 and similar
// hardware show the same symptom. Pekko HTTP (uni-meter) keeps the
// connection alive by default — that's the gap we close here.
//
// API for handlers: call Webserver->setKeepAlive(true) AFTER writing the
// response and BEFORE returning. The flag is reset at the start of every
// request and when a new client connects, so handlers must opt in each
// time. Default WebServer behaviour for handlers that never call
// setKeepAlive() is byte-identical to the framework original.
//
// Note about response headers: this only governs the TCP socket lifecycle.
// The framework's _prepareHeader() still hardcodes `Connection: close`
// (WebServer.cpp:657), so handlers that use Webserver->send() /
// WSContentBegin() will tell the client to close even though we keep
// the socket alive — which the client may honour. Use cases that need
// fully raw responses (no auto-injected Connection / Date / chunked
// encoding) write directly via `Webserver->client().write()` and
// bypass _prepareHeader() entirely. See xdrv_124_tinyc.ino's
// HandleTinyCWebOn for the raw-response wiring used by the EcoTracker
// emulation.
#ifndef USE_HTTP_KEEPALIVE
#define USE_HTTP_KEEPALIVE
#endif

class TasmotaWebServer : public WebServer
{
public:
	TasmotaWebServer(int port) :WebServer(port)
#ifdef USE_HTTP_KEEPALIVE
	, _ka_flag(false)
#endif
	{
	}

  bool isChunked(void) const { return _chunked; }

#ifdef USE_HTTP_KEEPALIVE
  // Per-request keep-alive opt-in. Set inside a request handler AFTER the
  // response has been written. The override of handleClient() below sees
  // the flag and keeps the TCP connection in HC_WAIT_READ for the next
  // request instead of destroying _currentClient. The flag auto-clears
  // at the start of each new request (caller must re-arm per request)
  // and when a new client connects.
  void setKeepAlive(bool en) { _ka_flag = en; }
  bool keepAlive(void) const { return _ka_flag; }

  // Override of WebServer::handleClient(). Mirrors the framework
  // implementation at WebServer.cpp:408-491 (Arduino-ESP32 3.x) with
  // one added branch: when the handler set _ka_flag = true, we stay
  // in HC_WAIT_READ instead of destroying the client.
  //
  // Why we duplicate ~80 lines of framework code instead of patching
  // the framework: the framework's `_currentStatus`/`_currentClient`/
  // `_statusChange` fields are protected, so a subclass can do this
  // without touching any vendored framework source. The trade-off is
  // that this method must be reviewed when Arduino-ESP32 makes
  // non-trivial changes to handleClient. Stable since framework 2.x.
  void handleClient() {
    if (_currentStatus == HC_NONE) {
      _currentClient = _server.accept();
      if (!_currentClient) {
        if (_nullDelay) {
          delay(1);
        }
        return;
      }
      _currentStatus = HC_WAIT_READ;
      _statusChange  = millis();
      _ka_flag       = false;     // fresh client, fresh keep-alive state
    }

    bool keepCurrentClient = false;
    bool callYield = false;

    if (_currentClient.connected()) {
      switch (_currentStatus) {
        case HC_NONE:
          break;
        case HC_WAIT_READ:
          if (_currentClient.available()) {
            _currentClient.setTimeout(HTTP_MAX_SEND_WAIT);
            if (_parseRequest(_currentClient)) {
              _contentLength = CONTENT_LENGTH_NOT_SET;
              _responseCode  = 0;
              _clearResponseHeaders();
              _ka_flag = false;   // handler must opt in for each request

              if (_chain) {
                _chain->runChain(*this, [this]() {
                  return _handleRequest();
                });
              } else {
                _handleRequest();
              }

              if (_currentClient.isSSE()) {
                _currentStatus = HC_WAIT_CLOSE;
                _statusChange  = millis();
                keepCurrentClient = true;
              } else if (_ka_flag && _currentClient.connected()) {
                // Keep-alive: socket stays open, ready for next request
                // on the same TCP connection. _statusChange is reset so
                // HTTP_MAX_DATA_WAIT measures idle time between requests.
                _currentStatus = HC_WAIT_READ;
                _statusChange  = millis();
                keepCurrentClient = true;
              }
            }
          } else {
            if (_ka_flag) {
              // Already keep-alive on this connection from a previous
              // request — keep waiting for the next request on the same
              // socket. Refresh _statusChange so HTTP_MAX_DATA_WAIT
              // (5 s default) doesn't trip between polls. The outer
              // _currentClient.connected() check still tears us down
              // when the peer closes its end.
              _statusChange = millis();
              keepCurrentClient = true;
            } else if (millis() - _statusChange <= HTTP_MAX_DATA_WAIT) {
              keepCurrentClient = true;
            }
            callYield = true;
          }
          break;
        case HC_WAIT_CLOSE:
          if (_currentClient.isSSE()) {
            // Never close connection (preserve framework SSE semantics)
          }
          if (millis() - _statusChange <= HTTP_MAX_CLOSE_WAIT) {
            keepCurrentClient = true;
            callYield = true;
          }
      }
    }

    if (!keepCurrentClient) {
      _currentClient = NetworkClient();
      _currentStatus = HC_NONE;
      _currentUpload.reset();
      _currentRaw.reset();
      _ka_flag = false;
    }

    if (callYield) {
      yield();
    }
  }

private:
  bool _ka_flag;
#endif // USE_HTTP_KEEPALIVE
};
#endif // ESP32

#endif // __TASMOTA_WEBSERVER__
