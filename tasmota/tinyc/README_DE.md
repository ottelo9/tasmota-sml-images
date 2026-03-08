# TinyC — C-Scripting fuer Tasmota

TinyC ist ein C-Subset-Compiler und eine VM, die auf ESP32/ESP8266 als Tasmota-Treiber `XDRV_124` laeuft. C-Code im Browser-IDE schreiben, zu Bytecode kompilieren, hochladen und ausfuehren — kein Firmware-Rebuild noetig.

## Schnellstart

1. `tinyc_ide.html.gz` auf das Flash-Dateisystem des Geraets hochladen
2. `http://<geraete-ip>/tinyc_ide.html` im Browser oeffnen
3. Code schreiben, **Ctrl+Enter** zum Kompilieren, **Ctrl+Shift+Enter** zum Ausfuehren

## Sprache

Standard-C-Subset: `int`, `float`, `char`, `void`, `bool` Datentypen. Kontrollfluss mit `if/else`, `while`, `for`, `switch/case`, `break`, `continue`. Funktionen, Arrays (Stack bis 255, Heap fuer groessere), `#define` Praeprozessor, `// Zeile` und `/* Block */` Kommentare. Keine Pointer, keine Structs.

## Tasmota-Integration

Callbacks werden automatisch aus Tasmotas Hauptschleife aufgerufen:

| Callback | Wann | Verwendung |
|---|---|---|
| `EveryLoop()` | Jede Hauptschleife (~1-5ms) | Ultraschnelles Polling, Bit-Banging |
| `Every50ms()` | Alle 50ms | Schnelle I/O, Funk-Polling |
| `EverySecond()` | Jede Sekunde | Sensor-Abfrage, Status-Updates |
| `JsonCall()` | MQTT-Telemetrie | JSON anfuegen via `responseAppend()` |
| `WebCall()` | Web-Seiten-Aktualisierung | Sensor-Zeilen via `webSend()` |
| `WebPage()` | Seitenaufruf (einmalig) | Charts, eigenes HTML |
| `UdpCall()` | UDP-Paket empfangen | Geraetekommunikation |
| `TaskLoop()` | FreeRTOS-Task (ESP32) | Hintergrundschleife mit `delay()` |

`main()` wird zuerst ausgefuehrt (in einem FreeRTOS-Task auf ESP32, mit voller `delay()`-Unterstuetzung). Nach Rueckkehr von `main()` bleiben Globale erhalten und Callbacks werden aktiviert. Wenn `TaskLoop()` definiert ist, laeuft es unabhaengig vom Haupt-Thread weiter.

## Eingebaute Funktionen

**GPIO:** `pinMode`, `digitalWrite`, `digitalRead`, `analogRead`, `analogWrite`, `gpioInit`
**Timing:** `delay`, `delayMicroseconds`, `millis`, `micros`
**Timer:** `timerStart`, `timerDone`, `timerStop`, `timerRemaining`
**Seriell:** `serialBegin`, `serialPrint`, `serialPrintInt`, `serialPrintFloat`, `serialPrintln`, `serialRead`, `serialAvailable`
**Mathe:** `abs`, `min`, `max`, `map`, `random`, `sqrt`, `sin`, `cos`
**Strings:** `strlen`, `strcpy`, `strcat`, `strcmp`, `printString`, `printStr`, `strToken`, `strSub`, `strFind`
**Format:** `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr`
**I2C:** `i2cRead8`, `i2cWrite8`, `i2cRead`, `i2cWrite`, `i2cExists`, `i2cRead0`, `i2cWrite0`
**SPI:** `spiInit`, `spiSetCS`, `spiTransfer`
**Dateien:** `fileOpen`, `fileClose`, `fileRead`, `fileWrite`, `fileExists`, `fileDelete`, `fileSize`
**Tasmota:** `tasmCmd`, `sensorGet`, `responseAppend`, `webSend`, `webFlush`, `addLog`
**HTTP:** `httpGet`, `httpPost`, `httpHeader`
**UDP:** `udpRecv`, `udpReady`, `udpSendArray`, `udpRecvArray`, `udp` (allgemein, Modi 0-7) — skalare `global` Floats senden automatisch bei Zuweisung
**Display:** `dspText`, `dspClear`, `dspPos`, `dspFont`, `dspSize`, `dspColor`, `dspDraw`, `dspPad`, `dspPixel`, `dspLine`, `dspRect`, `dspFillRect`, `dspCircle`, `dspFillCircle`, `dspHLine`, `dspVLine`, `dspRoundRect`, `dspFillRoundRect`, `dspTriangle`, `dspFillTriangle`, `dspDim`, `dspOnOff`, `dspUpdate`, `dspPicture`, `dspWidth`, `dspHeight`
**Audio:** `audioVol`, `audioPlay`, `audioSay`
**WebUI:** `webButton`, `webSlider`, `webCheckbox`, `webText`, `webNumber`, `webPulldown`, `webRadio`, `webTime`, `webPageLabel`, `webPage`, `webSendFile`, `webOn`, `webHandler`, `webArg`
**SML:** `smlGet`, `smlGetStr`, `smlWrite`, `smlRead`, `smlSetBaud`, `smlSetWStr`, `smlSetOpt`, `smlGetV`
**mDNS:** `mdnsRegister`
**System:** `tasm_wifi`, `tasm_mqttcon`, `tasm_teleperiod`, `tasm_uptime`, `tasm_heap`, `tasm_power`, `tasm_dimmer`, `tasm_temp`, `tasm_hum`, `tasm_hour`, `tasm_minute`, `tasm_second`, `tasm_year`, `tasm_month`, `tasm_day`, `tasm_wday`, `tasm_cw`, `tasm_sunrise`, `tasm_sunset`, `tasm_time`
**HomeKit:** `hkSetCode`, `hkAdd`, `hkVar`, `hkReady`, `hkStart`, `hkReset`, `hkStop` + Callback `HomeKitWrite(dev, var, val)`
**LED-Streifen:** `setPixels(array, len, offset)` — WS2812/NeoPixel-Steuerung
**Debug:** `print`, `dumpVM`

## Vordefinierte Konstanten

**Farben (RGB565):** 16 Farben ohne `#define` verfuegbar: `BLACK`, `WHITE`, `RED`, `GREEN`, `BLUE`, `YELLOW`, `CYAN`, `MAGENTA`, `ORANGE`, `PURPLE`, `GREY`, `DARKGREY`, `LIGHTGREY`, `DARKGREEN`, `NAVY`, `MAROON`, `OLIVE`

**HomeKit-Typen:** `HK_TEMPERATURE`, `HK_HUMIDITY`, `HK_LIGHT_SENSOR`, `HK_BATTERY`, `HK_CONTACT`, `HK_SWITCH`, `HK_OUTLET`, `HK_LIGHT`

**Datei-Modi:** `r` (Lesen), `w` (Schreiben), `a` (Anhaengen) — fuer `fileOpen()`

## Tasmota-Befehle

| Befehl | Beschreibung |
|---|---|
| `TinyC` | VM-Status anzeigen |
| `TinyCRun [Datei]` | Geladenen Bytecode ausfuehren (oder .tcb-Datei laden) |
| `TinyCStop` | Laufendes Programm stoppen |
| `TinyCReset` | VM-Zustand zuruecksetzen |
| `TinyCExec <Code>` | Code kompilieren und direkt ausfuehren |

REST-API: `http://<ip>/tc_api?cmd=run`, `cmd=stop`, `cmd=status`

## VM-Limits

| Ressource | ESP8266 | ESP32 |
|---|---|---|
| Stack-Tiefe | 64 | 256 |
| Aufruf-Frames | 8 | 32 |
| Globale | 64 | dynamisch |
| Konstanten | 32 | 128 |
| Konst.-Daten | 512 B | 4 KB |
| Code-Groesse | 4 KB | 16 KB |
| Heap | 8 KB | 32 KB |

> **ESP8266-Einschraenkung:** Der ESP8266 hat sehr wenig RAM (~40 KB freier Heap). TinyC funktioniert fuer einfache Skripte (Sensoren lesen, MQTT, einfache Automatisierung), aber Programme mit Heap-Arrays, WS2812-LED-Streifen oder IR zusammen mit der Tasmota-Web-Oberflaeche fuehren zu Instabilitaet wegen Speicherknappheit. Fuer alles ueber triviale Skripte hinaus ESP32, ESP32-S3 oder ESP32-C3 verwenden.

## Beispiele

Siehe [`examples/`](examples/) fuer vollstaendige Programme:

- **blink** — LED blinken
- **callbacks** — Tasmota MQTT + Web-Integration
- **sht31** — I2C Temperatur-/Feuchtigkeitssensor
- **max31855** — SPI Thermoelement-Leser
- **bresser** — CC1101 868 MHz Wetterstation-Empfaenger (5/6/7-in-1 + Bodenfeuchte)
- **bresser_chart** — Bresser Wetterstation mit Google Charts Ringpuffer und Flash-Speicherung
- **chart** — Google Charts mit 1000-Punkt-Ringpuffer
- **display_demo** — Sensor-Dashboard fuer ILI9488 Display
- **editor** — Code-Editor Beispiel
- **udp** — Multicast-Datenaustausch zwischen Geraeten
- **benchmark** — VM-Leistungsmessung
- **file_io** — LittleFS Lesen/Schreiben
- **sensor_read** — Analogsensor mit serieller Ausgabe
- **sort** — Bubble-Sort Algorithmus
- **strings** — String-Operationen
- **fibonacci** — Rekursive Funktionsdemo

## VS Code Unterstuetzung

Die `vscode-tinyc` Erweiterung fuer `.tc` Syntax-Highlighting installieren mit voller TinyC-Schluesselwort-, Builtin- und Callback-Faerbung. Nach `~/.vscode/extensions/tinyc-1.0.0` kopieren oder verlinken.

## Referenz

Vollstaendige Sprachspezifikation: [TinyC_Reference.md](TinyC_Reference.md) | [Deutsche Version](TinyC_Reference_DE.md)
