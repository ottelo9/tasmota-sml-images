# TinyC — C Scripting for Tasmota

TinyC is a C-subset compiler and VM that runs on ESP32/ESP8266 as Tasmota driver `XDRV_124`. Write C code in the browser IDE, compile to bytecode, upload and run — no firmware rebuild needed.

## Key Advantages

- **Portable bytecode** — compile once in the browser, run the same binary on ESP32, ESP32-S3, ESP32-C3, or ESP8266. No recompilation needed per target.
- **No on-device compiler** — the device only needs the lightweight VM (~12 KB flash). Compilation happens in the browser IDE, saving precious flash and RAM on constrained devices.
- **Compact binary upload** — bytecode is smaller than source text, reducing upload time and filesystem usage.
- **Instant execution** — uploaded bytecode runs immediately, no parsing or compilation step on the device.
- **Familiar C syntax** — no new language to learn. Standard C subset with `int`, `float`, arrays, functions, `for`/`while`/`if` — anyone who knows C can write TinyC.
- **10× faster than text interpreters** — the bytecode VM with direct-threaded dispatch executes significantly faster than script engines that re-parse source text on every statement.
- **True background tasks** — `TaskLoop()` runs in a dedicated FreeRTOS task (ESP32) with full `delay()` support. Long-running loops, sensor polling, and protocol handling never block the main Tasmota loop. Other script engines share the main loop and cannot use blocking delays without stalling WiFi, MQTT, and the web server.

## Building Tasmota with TinyC

Add the following to your `user_config_override.h`:

```c
#define USE_TINYC           // Enable TinyC VM (XDRV_124)
#define USE_TINYC_IDE       // Enable self-hosted browser IDE (requires USE_UFILESYS)
```

`USE_TINYC` enables the VM and console commands. `USE_TINYC_IDE` adds the `/tinyc_ide.html` endpoint that serves the IDE directly from flash — requires a filesystem-enabled build (`USE_UFILESYS`).

After compiling and flashing Tasmota, upload the IDE file to the device filesystem:

1. Run `bash bundle.sh` to generate `tinyc_ide.html.gz`
2. Upload `tinyc_ide.html.gz` to the device via **Consoles > Manage File System** (or `http://<device-ip>/ufsd`)
3. Open `http://<device-ip>/tinyc_ide.html` in a browser
4. Write code, press **Ctrl+Enter** to compile, **Ctrl+Shift+Enter** to run

## Language

Standard C subset: `int`, `float`, `char`, `void`, `bool` types. Control flow with `if/else`, `while`, `for`, `switch/case`, `break`, `continue`. Functions, arrays (local up to 64 elements, heap-allocated for larger), `#define` preprocessor, `// line` and `/* block */` comments. No pointers, no structs.

## Tasmota Integration

Callbacks run automatically from Tasmota's main loop:

| Callback | When | Use |
|---|---|---|
| `EveryLoop()` | Every main loop (~1-5ms) | Ultra-fast polling, bit-banging |
| `Every50ms()` | Every 50ms | Fast I/O, radio polling |
| `EverySecond()` | Every 1s | Sensor polling, status updates |
| `JsonCall()` | MQTT telemetry | Append JSON via `responseAppend()` |
| `WebCall()` | Web page refresh | Add sensor rows via `webSend()` |
| `WebPage()` | Page load (once) | Charts, custom HTML |
| `UdpCall()` | UDP packet received | Inter-device communication |
| `TaskLoop()` | FreeRTOS task (ESP32) | Background loop with `delay()` support |
| `TouchButton(btn, val)` | Touch event | GFX button/slider touch callback |
| `HomeKitWrite(dev, var, val)` | HomeKit write | Control lights, switches, outlets from Apple Home |
| `Command(char cmd[])` | Custom console command | Handle registered prefix commands (e.g., MP3Play) |
| `OnExit()` | Script stop | Close serial ports, release resources |

`main()` runs first (in a FreeRTOS task on ESP32, with full `delay()` support). After `main()` returns, globals persist and callbacks activate. If `TaskLoop()` is defined, it continues running in the same task independently of the main thread.

## Built-in Functions

**GPIO:** `pinMode`, `digitalWrite`, `digitalRead`, `analogRead`, `analogWrite`, `gpioInit`
**Timing:** `delay`, `delayMicroseconds`, `millis`, `micros`
**Timers:** `timerStart`, `timerDone`, `timerStop`, `timerRemaining`
**Serial:** `serialBegin`, `serialPrint`, `serialPrintInt`, `serialPrintFloat`, `serialPrintln`, `serialRead`, `serialAvailable`
**1-Wire:** `owSetPin`, `owReset`, `owWrite`, `owRead`, `owWriteBit`, `owReadBit`, `owSearchReset`, `owSearch`
**Math:** `abs`, `min`, `max`, `map`, `random`, `sqrt`, `sin`, `cos`, `floor`, `ceil`, `round`
**Strings:** `strlen`, `strcpy`, `strcat`, `strcmp`, `printString`, `printStr`, `strToken`, `strSub`, `strFind`
**Format:** `sprintf`, `sprintfAppend` (auto-detect type; legacy: `sprintfInt`, `sprintfFloat`, `sprintfStr`, `sprintfAppendInt`, `sprintfAppendFloat`, `sprintfAppendStr`)
**I2C:** `i2cRead8`, `i2cWrite8`, `i2cRead`, `i2cWrite`, `i2cExists`, `i2cRead0`, `i2cWrite0`, `i2cSetDevice`, `i2cSetActiveFound`
**SPI:** `spiInit`, `spiSetCS`, `spiTransfer`
**Files:** `fileOpen`, `fileClose`, `fileRead`, `fileWrite`, `fileExists`, `fileDelete`, `fileSize`, `fileFormat`, `fileMkdir`, `fileRmdir`, `fileReadArray`, `fileWriteArray`, `fileLog`, `fileDownload`, `fileGetStr`, `fileExtract`, `fileExtractFast`, `fsInfo`
**Time:** `timeStamp`, `timeConvert`, `timeOffset`, `timeToSecs`, `secsToTime`
**Tasmota:** `tasmCmd`, `sensorGet`, `responseAppend`, `webSend`, `webFlush`, `addLog`, `addCommand`, `responseCmnd`
**HTTP:** `httpGet`, `httpPost`, `httpHeader`
**TCP:** `tcpServer`, `tcpClose`, `tcpAvailable`, `tcpRead`, `tcpWrite`, `tcpReadArray`, `tcpWriteArray`
**UDP:** `udpRecv`, `udpReady`, `udpSendArray`, `udpRecvArray`, `udp` (general-purpose, modes 0-7) — scalar `global` floats auto-broadcast on assignment
**Display:** `dspText`, `dspClear`, `dspPos`, `dspFont`, `dspSize`, `dspColor`, `dspDraw`, `dspPad`, `dspPixel`, `dspLine`, `dspRect`, `dspFillRect`, `dspCircle`, `dspFillCircle`, `dspHLine`, `dspVLine`, `dspRoundRect`, `dspFillRoundRect`, `dspTriangle`, `dspFillTriangle`, `dspDim`, `dspOnOff`, `dspUpdate`, `dspPicture`, `dspWidth`, `dspHeight`
**Touch Buttons:** `dspButton`, `dspTButton`, `dspPButton`, `dspSlider`, `dspButtonState`, `touchButton`
**Audio:** `audioVol`, `audioPlay`, `audioSay`
**Deep Sleep:** `deepSleep`, `deepSleepGpio`, `wakeupCause`
**Email:** `mailBody`, `mailAttach`, `mailSend`
**Persist:** `persist` keyword for auto-saved variables, `saveVars` for manual save
**Watch:** `watch` keyword for change detection, `changed`, `delta`, `written`, `snapshot` intrinsics
**WebUI:** `webButton`, `webSlider`, `webCheckbox`, `webText`, `webNumber`, `webPulldown`, `webRadio`, `webTime`, `webPageLabel`, `webPage`, `webSendFile`, `webOn`, `webHandler`, `webArg`
**SML:** `smlGet`, `smlGetStr`, `smlWrite`, `smlRead`, `smlSetBaud`, `smlSetWStr`, `smlSetOpt`, `smlGetV`
**mDNS:** `mdnsRegister`
**System:** `tasm_wifi`, `tasm_mqttcon`, `tasm_teleperiod`, `tasm_uptime`, `tasm_heap`, `tasm_power`, `tasm_dimmer`, `tasm_temp`, `tasm_hum`, `tasm_hour`, `tasm_minute`, `tasm_second`, `tasm_year`, `tasm_month`, `tasm_day`, `tasm_wday`, `tasm_cw`, `tasm_sunrise`, `tasm_sunset`, `tasm_time`
**HomeKit:** `hkSetCode`, `hkAdd`, `hkVar`, `hkReady`, `hkStart`, `hkReset`, `hkStop` + `HomeKitWrite(dev, var, val)` callback
**LED Strip:** `setPixels(array, len, offset)` — WS2812/NeoPixel control
**Debug:** `print`, `dumpVM`

## Predefined Constants

**Colors (RGB565):** 16 colors available without `#define`: `BLACK`, `WHITE`, `RED`, `GREEN`, `BLUE`, `YELLOW`, `CYAN`, `MAGENTA`, `ORANGE`, `PURPLE`, `GREY`, `DARKGREY`, `LIGHTGREY`, `DARKGREEN`, `NAVY`, `MAROON`, `OLIVE`

**HomeKit Types:** `HK_TEMPERATURE`, `HK_HUMIDITY`, `HK_LIGHT_SENSOR`, `HK_BATTERY`, `HK_CONTACT`, `HK_SWITCH`, `HK_OUTLET`, `HK_LIGHT`

**File Modes:** `r` (read), `w` (write), `a` (append) — for `fileOpen()`

## Tasmota Commands

| Command | Description |
|---|---|
| `TinyC` | Show VM status |
| `TinyCRun [file]` | Run loaded bytecode (or load .tcb file first) |
| `TinyCStop` | Stop running program |
| `TinyCReset` | Reset VM state |
| `TinyCExec <code>` | Compile and run inline code |
| `chkpt` | List current partition table |
| `chkpt p` | Auto-resize app partition (firmware + ~200 KB overhead), expand filesystem. **Warning: filesystem is formatted — all files are lost!** |
| `chkpt p <KB>` | Set app partition to specific size in KB, adjust filesystem |
| `chkpt a1`..`a4` | Add custom partition (64–256 KB) for plugin drivers |
| `chkpt r` | Remove custom partition |

REST API: `http://<ip>/tc_api?cmd=run`, `cmd=stop`, `cmd=status`

File Download Server (port 82, ESP32): `http://<ip>:82/ufs/<filename>` — supports `@from_to` time-range filter

## VM Limits

| Resource | ESP8266 | ESP32 |
|---|---|---|
| Stack depth | 64 | 256 |
| Call frames | 8 | 32 |
| Globals | 64 | dynamic |
| Constants | 32 | 128 |
| Const data | 512 B | 4 KB |
| Code size | 4 KB | 16 KB |
| Heap | 8 KB | 32 KB |

> **ESP8266 limitation:** The ESP8266 has very limited RAM (~40 KB free heap). TinyC works for simple scripts (sensor reading, MQTT, basic automation), but programs using heap arrays, WS2812 LED strips, or IR together with the Tasmota web UI will cause instability due to memory pressure. For anything beyond trivial scripts, use ESP32, ESP32-S3, or ESP32-C3.

## Examples

See [`examples/`](examples/) for complete working programs:

- **blink** — LED blink
- **callbacks** — Tasmota MQTT + web integration
- **sht31** — I2C temperature/humidity sensor (dual-bus scan, address claiming)
- **max31855** — SPI thermocouple reader
- **bresser** — CC1101 868 MHz weather station receiver (5/6/7-in-1 + soil moisture)
- **bresser_chart** — Bresser weather station with Google Charts ring buffer and flash persistence
- **chart** — Google Charts with 1000-point ring buffer
- **display_demo** — Sensor dashboard for ILI9488 display
- **editor** — Code editor example
- **udp** — Multicast data sharing between devices
- **benchmark** — VM performance measurement
- **file_io** — File read/write (SD card default, `/ffs/` for flash, `/sdfs/` for SD)
- **sensor_read** — Analog sensor with serial output
- **sort** — Bubble sort algorithm
- **strings** — String operations
- **fibonacci** — Recursive function demo
- **dysv17f** — DY-SV17F MP3 player (serial TX, custom console commands)
- **onewire** — 1-Wire bus driver: DS18B20 temperature + DS2406/DS2413/DS2408 switches, GPIO and DS2480B serial bridge modes, WebUI config, ROM-based aliases, OW console command

## VS Code Support

Install the `vscode-tinyc` extension for `.tc` syntax highlighting with full TinyC keyword, builtin, and callback coloring. Copy or symlink to `~/.vscode/extensions/tinyc-1.0.0`.

## Reference

Full language specification: [TinyC_Reference.md](TinyC_Reference.md)
