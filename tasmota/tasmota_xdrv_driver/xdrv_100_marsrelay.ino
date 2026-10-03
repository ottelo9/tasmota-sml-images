/*
  xdrv_100_marsrelay.ino - Marstek cloud emulation for Tasmota (port of marsrelay)

  Runs a Marstek battery without the Marstek cloud, see include/xdrv_100_marsrelay.h.
  Based on marsrelay by tomquist (github.com/tomquist/marsrelay), GPL-3.0.

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Needs (see user_config_override.h, MARSRELAY_BUILD):
    USE_MARSRELAY, USE_WIFI_RANGE_EXTENDER (the battery's AP), ESP32 with PSRAM
    ssl_server_min.c in lib/lib_ssl/bearssl-esp8266 (overlay from gemu2015's fork)

  Setup, once:
    Backlog RgxSSID Marsrelay; RgxPassword <min. 8 chars>; RgxAddress 192.168.4.1; RgxSubnet 255.255.255.0; RgxState 1
    Timezone 99   (the battery's clock is set from Tasmota's local time)
    MQTT to the home broker as usual (MqttHost, MqttUser, ...)

  Commands:
    MrStatus                     diagnostics
    MrMap [<device> [<external>]] ID mappings for hm2mqtt (list / add / remove, 0 = all)
    MrUdp <port>[,<port>...]     UDP proxy ports for the power meter (default 1010, 0 = off)
    MrPrefix <topic>             topic prefix for requests and telemetry (default marsrelay)
    MrHttps 0|1                  TLS listener on 443 for the Venus upload (default 1)
    MrAcceptAll 0|1              answer unknown HTTPS paths with code 0 (default 0)
    MrRequests 0|1               publish every cloud request to <prefix>/request (default 1)
    MrTelemetry 0|1              publish decoded Venus uploads (default 1)
    MrHold <s>                   hold an answered HTTPS connection (default 25)
*/

#ifdef ESP32
#ifdef USE_MARSRELAY

#define XDRV_100 100

#include "include/xdrv_100_marsrelay.h"

bool Xdrv100(uint32_t function) {
  bool result = false;

  if (FUNC_INIT == function) {
    MrInit();
  }
  else if (MrReady) {
    switch (function) {
      case FUNC_LOOP:
        MrUpDrain();
        break;
      case FUNC_EVERY_SECOND:
        if (0 == (TasmotaGlobal.uptime % 5)) { MrDhcpDns(); }
        break;
      case FUNC_MQTT_SUBSCRIBE:
        MrMqttSubscribe();
        break;
      case FUNC_MQTT_DATA:
        result = MrMqttData();
        break;
      case FUNC_COMMAND:
        result = DecodeCommand(kMrCommands, MrCommand);
        break;
      case FUNC_JSON_APPEND:
        ResponseAppend_P(PSTR(","));
        MrJsonStatus();
        break;
#ifdef USE_WEBSERVER
      case FUNC_WEB_ADD_HANDLER:
        MrWebHandlers();
        break;
      case FUNC_WEB_SENSOR:
        MrWebSensor();
        break;
#endif  // USE_WEBSERVER
    }
  }
  return result;
}

#endif  // USE_MARSRELAY
#endif  // ESP32
