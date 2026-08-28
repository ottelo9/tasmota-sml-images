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

// ⚠️ TWO BOUNDS, without which ONE kept-alive connection takes down the WHOLE
// web server on port 80. The Arduino WebServer serves exactly ONE client at a
// time: handleClient() only accepts a new one while `_currentStatus ==
// HC_NONE`. Whoever holds that connection holds the server.
//
// The first version of this override (2026-05-12) refreshed `_statusChange` on
// every poll while `_ka_flag` was set, so HTTP_MAX_DATA_WAIT never expired and
// there was no upper bound at all. When the peer disappears WITHOUT a FIN
// (battery reboots, wifi drops, NAT entry expires), the socket is held until
// lwIP's TCP keepalive reaps it -- and TCP_KEEPIDLE_DEFAULT is 7200000 ms,
// EXACTLY TWO HOURS. That is the outage length accolon measured twice in
// ottelo9/tasmota-sml-images#52: port 80 dead while MQTT, ports 82/83, the SML
// reader and the TinyC VM keep running, and after roughly two hours the web UI
// comes back on its own without a reboot.
//
// TC_KEEPALIVE_MAX_IDLE  How long a kept-alive connection may sit without a
//                        new request. Reaps peers that went away. 30 s is
//                        generous: the storage systems poll every 1-9 s, and
//                        the Jackery-Emu tester defaults to 13 s.
// TC_KEEPALIVE_YIELD_AFTER  The second bound: if ANOTHER client shows up
//                        (`_server.hasClient()`) while the held connection has
//                        been idle at least this long, the held one is given
//                        up. Without it a peer that politely polls every two
//                        seconds would lock the browser out forever -- the
//                        idle bound alone would never trip on such a peer.
//
// ⚠️ Why the grace period rather than yielding immediately: the whole reason
// keep-alive exists here is firmware that insists on ONE socket across its
// polls (Jackery Homepower 2000 Ultra and friends). Yielding at the first
// knock would tear that socket down between every pair of polls as soon as a
// browser sits on the Tasmota UI, which is legal HTTP but hands a reconnect to
// exactly the firmware that dislikes them. One second protects a tight
// request/response conversation while capping how long the browser waits.
// Closing a persistent connection between two requests is explicitly allowed
// by HTTP/1.1; a well-behaved client reconnects.
#ifndef TC_KEEPALIVE_MAX_IDLE
#define TC_KEEPALIVE_MAX_IDLE 30000
#endif
#ifndef TC_KEEPALIVE_YIELD_AFTER
#define TC_KEEPALIVE_YIELD_AFTER 1000
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

  // True only while the current request has a live multipart UPLOAD context.
  // The Arduino-ESP32 WebServer routes a RAW (non-multipart) POST body to the
  // SAME registered upload function (FunctionRequestHandler::raw() -> _ufn),
  // but with _currentUpload == nullptr. An upload handler that unconditionally
  // calls upload() then dereferences a null unique_ptr -> LoadProhibited
  // reboot (the /tc_upload "camera regression": a `curl --data-binary` POST
  // rebooted the S3). Handlers gate on this and bail on a raw invocation.
  // _currentUpload is protected in WebServer; this subclass already touches it.
  bool hasUploadCtx(void) const { return (bool)_currentUpload; }

#ifdef USE_HTTP_KEEPALIVE
  // Per-request keep-alive opt-in. Set inside a request handler AFTER the
  // response has been written. The override of handleClient() below sees
  // the flag and keeps the TCP connection in HC_WAIT_READ for the next
  // request instead of destroying _currentClient. The flag auto-clears
  // at the start of each new request (caller must re-arm per request)
  // and when a new client connects.
  // Asking to hold the socket is a REQUEST, not a decree: a client that said
  // `Connection: close` gets closed even when the handler asks for keep-alive.
  // The EcoTracker-style handlers call webKeepAlive() unconditionally, and this
  // is what makes that honest -- the handler asks, the client decides.
  //
  // ⚠️ POLARITY: a MISSING header means keep-alive, not close. HTTP/1.1 defaults
  // to persistent, and the only trace we have of a real EcoTracker
  // (sdeigm/uni-meter#265) sends `GET /v1/json HTTP/1.1` with Host, User-Agent
  // and Accept and NO Connection header at all. Requiring the token would
  // refuse exactly the clients this feature exists for. (Hans, 2026-08-27.)
  //
  // ⚠️ Needs "Connection" in collectHeaders() -- see xdrv_01_9_webserver.ino.
  // Without it header("Connection") is empty for every request and this reads
  // as "keep-alive" throughout, which is the old behaviour, not a crash.
  void setKeepAlive(bool en) {
    if (!en) { _ka_flag = false; return; }
    String c = hasHeader(F("Connection")) ? header(F("Connection")) : String();
    c.toLowerCase();
    _ka_flag = (c.indexOf(F("close")) < 0);
  }
  bool keepAlive(void) const { return _ka_flag; }

  // Diagnostics. Both counters only ever go up; poll them to see whether the
  // bounds above are firing. Deliberately counters and not AddLog: this
  // header must not depend on anything from the Tasmota core. The log line
  // is emitted by xdrv_124_tinyc.ino, next to the lwIP PCB census.
  uint16_t kaDropIdle(void)  const { return _ka_drop_idle; }
  uint16_t kaDropOther(void) const { return _ka_drop_other; }

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
                // TC_KEEPALIVE_MAX_IDLE measures the idle time BETWEEN
                // requests, not the lifetime of the connection -- a peer that
                // keeps polling is never torn down for age alone.
                _currentStatus = HC_WAIT_READ;
                _statusChange  = millis();
                keepCurrentClient = true;
              }
            }
          } else {
            if (_ka_flag) {
              // Kept alive and nothing to do right now. THIS is the trap:
              // setting `keepCurrentClient = true` unconditionally here holds
              // the server's only client slot for an unbounded time. Two
              // bounds, see the note above the class.
              //
              // ⚠️ hasClient() already ACCEPTS the waiting peer and parks it
              // in `_accepted_sockfd`; the next accept() hands back exactly
              // that one (NetworkServer.cpp). So the call loses nobody -- but
              // it may only be made where a `true` result actually gives up
              // the held connection, otherwise the accepted peer would sit
              // there unserved. The listening socket is O_NONBLOCK
              // (NetworkServer::begin), so this never blocks.
              const uint32_t idle = millis() - _statusChange;
              if (idle > TC_KEEPALIVE_MAX_IDLE) {
                if (_ka_drop_idle < 0xffff) _ka_drop_idle++;
              } else if (idle >= TC_KEEPALIVE_YIELD_AFTER && _server.hasClient()) {
                if (_ka_drop_other < 0xffff) _ka_drop_other++;
              } else {
                keepCurrentClient = true;
              }
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
  uint16_t _ka_drop_idle  = 0;   // given up: idle too long
  uint16_t _ka_drop_other = 0;   // given up: another client was waiting
#endif // USE_HTTP_KEEPALIVE
};
#endif // ESP32

#endif // __TASMOTA_WEBSERVER__
