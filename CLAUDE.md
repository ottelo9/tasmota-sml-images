# tasmota-sml-images — notes for Claude

Release firmware for smart-meter reading with Tasmota. This repo is **not** a
Tasmota tree: it holds only the files that differ from official Tasmota, plus
the build configuration. Personal setup and working rules: `CLAUDE.local.md`
(not in the repo).

## How an image is built

Official Tasmota as the base, the files of this repo copied over it, then
PlatformIO. The overlay files come mostly from **gemu2015's fork**
(`github.com/gemu2015/Sonoff-Tasmota`) — TinyC, SML, Scripter and the webserver
changes they need. The fork is the **source** of the overlays, never the build
base.

| Path | What |
|---|---|
| `tasmota/`, `lib/` | overlay files (`git ls-files` is the current list) |
| `platformio_tasmota_cenv.ini` | envs per chip and variant |
| `platformio_tasmota32.ini`, `boards/` | platform settings, extra boards |
| `tasmota/user_config_override.h` | feature selection |
| `make_tasmota_zips_extended.sh` | packs the release ZIPs |
| `ota_firmware/` | binaries for OTA, tracked |
| `gemu2015 Repo Update.txt` | which fork commit the overlays were taken from |

Variants: `_tas` = classic Scripter + Google Charts, `_tc` = TinyC VM + browser
IDE. ESP8266 builds are `_tas` only.

## Taking a new gemu release

1. Find what changed since the last adoption:
   `git -C <fork> diff --name-only <marker sha>..HEAD -- '*.h' '*.ino' '*.cpp'`
2. Before calling a file needed or unneeded, check whether it is already in this
   repo **and** whether official Tasmota has the symbols it uses. Example: the
   5-argument `MqttPublishPayload(...)` in `xdrv_124_tinyc_vm.h` only exists in
   the fork, so the fork's `xdrv_02_9_mqtt.ino` has to be an overlay too.
3. Update `gemu2015 Repo Update.txt` — exactly two lines, CRLF between them,
   **no trailing newline**; the date is the commit's own date:

   ```
   <dd.MM.yyyy HH:mm>
   https://github.com/gemu2015/Sonoff-Tasmota/commit/<full 40-char sha>
   ```

   `git -C <fork> log -1 --format="%ad" --date=format:"%d.%m.%Y %H:%M" <sha>`

   A wrong entry makes the next diff silently miss or repeat files.

4. **Platform:** this repo builds with platform `2026.05.50` like gemu
   (Arduino 3.3.8 / IDF 5.5.4). Going back to `2026.02.30` (IDF 5.3.4, the
   fallback for the WT32-ETH01 reboots of issue #53) breaks the build of every
   env with Ethernet: `ETH_CMD_S_ALL_MULTICAST` in `xdrv_124_tinyc_vm.h`
   needs IDF 5.5 and would have to go behind an `ESP_IDF_VERSION` check.

## Local patches on gemu files

Re-apply after copying a gemu file, until gemu has them (diff against the fork):

- `tasmota/tasmota_xdrv_driver/xdrv_10_scripter.ino`: four hooks for the
  Marstek CT002 registration page `/ctreg` (include of
  `include/xdrv_10_ct002_registration.h`, `CtRegEverySecond()` in
  `FUNC_EVERY_SECOND`, management button, two `Webserver->on("/ctreg")`),
  all behind `USE_SCRIPT_CT002_REGISTRATION`. Only the image
  `tasmota1m_ct002_ottelo_tas` sets it. Code by next145.

## Backports from official Tasmota (not from gemu)

- `tasmota/tasmota_xdrv_driver/xdrv_82_esp32_ethernet.ino`: the 15.5.0 file
  plus Tasmota PR #25051 (WT32-ETH01 init: `eth_type` read before the module
  defaults were set, issue #25043). Drop this overlay when the Tasmota base is
  a release that contains the PR — it would otherwise replace a newer driver.

## Marsrelay (own driver, not from gemu)

`tasmota_xdrv_driver/xdrv_100_marsrelay.ino` + `include/xdrv_100_marsrelay*.h`:
port of github.com/tomquist/marsrelay (Marstek battery without cloud: DNS, TLS
MQTT broker 8883, HTTPS 443, clock endpoint on port 80, UDP proxy). Built only by
the envs `tasmota32s3_marsrelay` / `tasmota32s3opi_marsrelay` (`MARSRELAY_BUILD`).
The TLS server needs `lib/lib_ssl/bearssl-esp8266/src/ssl/ssl_server_min.c` —
keep that overlay. Cert/key arrays in `_certs.h` are generated from marsrelay's
PEMs (DER + RSA CRT components).

## TinyC compatibility

`TC_RELEASE` and `TC_SYSCALL_ABI` live in `tasmota/include/xdrv_124_tinyc_vm.h`.
The loader **refuses** a `.tcb` whose `abi_rev` is higher than its own, so when
the ABI steps, the image has to reach the devices before the programs from
`tasmota-sml-script` that were built against it.

## Conventions

- Source comments English and short; log lines always English.
- Do not commit unless asked.
