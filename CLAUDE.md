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
5. **No `custom_sdkconfig` for now:** it makes PlatformIO build the ESP-IDF
   itself, and that fails on the owner's PlatformIO 6.2.0 with
   `No module named 'SCons.Tool.FortranCommon'` (the same error the ESP8266
   builds hit). `[bt_off]` in `platformio_tasmota_cenv.ini` (Bluetooth out of
   the SDK, ~36 KB heap on a classic ESP32 per gemu) is prepared but not used
   by any env until that build works.

## CT002 registration page (ESP8266, Scripter)

`include/xdrv_10_ct002_registration.h` + four hooks in `xdrv_10_scripter.ino`
(page `/ctreg`), behind `USE_SCRIPT_CT002_REGISTRATION`; only the image
`tasmota1m_ct002_ottelo_tas` sets it. Code by next145, in gemu's fork since
`67c8e784e` — no local patch any more, both files are plain gemu copies.

## Matter: plugin only

All `_tc` envs set `USE_MATTER_C_PLUGIN_ONLY`; Matter comes as gemu's ready-made
`MATTERF_32r.bin` from his release (never built here). So
`lib/libesp32_div/matter_c/` carries only `include/`, `library.json` and the
two build-hook scripts `test/prebuild_creds.py` + `test/gen_csa_creds.py`
(`library.json` calls them; without them the build stops). No `src/` — do
not copy gemu's `src/` or the other `test/` files on an adoption.

## Backports from official Tasmota (not from gemu)

- `tasmota/tasmota_xdrv_driver/xdrv_82_esp32_ethernet.ino`: the 15.5.0 file
  plus Tasmota PR #25051 (WT32-ETH01 init: `eth_type` read before the module
  defaults were set, issue #25043), plus our own fix: the WT32-ETH01 branch
  writes `Settings->eth_type = 0` instead of `ETH_PHY_LAN8720` — since IDF 5.4
  `ETH_PHY_GENERIC` comes first, so `ETH_PHY_LAN8720` is 1 = TLK110/IP101 in
  Tasmota's index (no Ethernet, `EthType` back to 1 on every boot, images
  issue #68). Upstream `development` still had it on 25.10.2026. Drop this
  overlay only when the Tasmota base contains both fixes.

## Marsrelay (own driver, not from gemu)

`tasmota_xdrv_driver/xdrv_100_marsrelay.ino` + `include/xdrv_100_marsrelay*.h`:
port of github.com/tomquist/marsrelay (Marstek battery without cloud: DNS, TLS
MQTT broker 8883, HTTPS 443, clock endpoint on port 80, UDP proxy). Built only by
the envs `tasmota32s3_marsrelay` / `tasmota32s3opi_marsrelay` (`MARSRELAY_BUILD`).
The TLS server needs `lib/lib_ssl/bearssl-esp8266/src/ssl/ssl_server_min.c` —
keep that overlay. Cert/key arrays in `_certs.h` are generated from marsrelay's
PEMs (DER + RSA CRT components).

## Own patches in gemu's overlays

None since `69c57e6b3` (21.10.2026, TinyC 1.6.74): gemu took over the Domoticz
values by unit (`xsns_53_sml.ino`, `b47f6fa1c`) and the OTA fix (a torn-down
slot is not saved again before the restart, `tc_persist_save()` refuses without
heap, `73763c524`; plus Andreas' `started` guard: a never-started program does
not save). Before that, `xdrv_124_tinyc_vm.h` / `xdrv_124_tinyc.ino` became plain gemu copies
(since `5651c41b3`, 18.10.2026; the version date in the repository list,
`SML Chart · 14.10.26`, since `a40299982`). He took over what was ours: the `spawnTask` worker sharing the primary's heap, `"ordered"`/`{"sep":1}`
in index.json, the name-only repository list, and the slot row with name,
version (`// @version: YYYYMMDD_HHMM`, .tcb header TLV 3, `"version"` in
index.json) and the 🔄 update button. tasmota-sml-script's build.html derives
the version from `sml_vers`.

## gemu's fork is on Tasmota 16, we are on 15.5.0

Since `0fc40e045` (merge of arendst/development, 25.10.2026) the fork is a
Tasmota 16 tree. Its TinyC files barely changed for that (only `TC_RELEASE`),
so they still go into the 15.5.0 overlay as plain copies — last taken
`d1aafa5b5` (13.11.2026, TinyC 1.6.80): `xdrv_124_tinyc_vm.h`,
`xdrv_124_tinyc.ino`, `xdrv_124_tinyc_usb.h`, the new
`xdrv_124_tinyc_espnow.h` and `xdrv_124_tinyc_pcnt.h` (both included
unconditionally, their code behind `USE_TINYC_ESPNOW` / `USE_TINYC_PCNT`), `tls_mini` (the VM calls `setRSAOnly`,
`setClientECCert`, `TlsPeerCertInfo`) and `xsns_53_sml.ino`. Not taken:
`xdrv_50_filesystem.ino` - the fork's copy carries Tasmota 16 changes. Only
gemu's hunk of `b8ff22f4f` is in ours: `TinyCFsWritePause(ufs_fsz, dfsp == ffsp)`
(the second argument is new; without it the link fails). On every further
adoption check both directions: the functions the new TinyC files call that
the old ones did not, and the TinyC functions whose parameters changed and
that other overlay files call (`extern` declarations).

## ESP32-P4 Rev.3 (`tasmota32p4r3_ottelo_*`)

Official board `esp32p4r3` (16 MB, also fine on 32 MB boards), safeboot from
the official env `tasmota32p4r3-safeboot`. Own define `TASMOTA32P4R3_OTTELO`
(same feature set and Ethernet as P4) only for its OTA URL - the P4 image does
not run on Rev.3. `BOARD_SDMMC_POWER_CHANNEL=4` (SD power LDO of Espressif's
P4 reference boards). From a user's package (18.10.2026); `_tc` ran on his
board. There is no `_tas` env for it: README and the zip script (`TC_ONLY`) know that.

## DFR1154 camera image (`tasmota32s3_dfr1154`)

DFRobot FireBeetle 2 ESP32-S3 AI CAM (OV3660): S3 TC + camera + person detection
(ESP-DL) + PicoTTS, after gemu's env `dfrobot-ai-cam` (`tasmota/tinyc/dfrobot/`
in the fork). Official board `esp32s3-qio_opi` (OPI PSRAM, USB-CDC console) set
to 16 MB and `esp32_partition_app3904k_fs11584k.csv` in the env - gemu's own
`esp32s3_16M` board is not in official Tasmota. Libraries taken from the fork at 72b55d3c6, only for this env:
`lib/lib_dfr1154/esp32-camera` (without examples/test), `lib/lib_dfr1154/pico`
(without the voice files in `lang/`), `lib/libesp32_dl` (plus the `-I` paths). All
three via `lib_deps = symlink://…`: appended to the inherited `lib_extra_dirs`
(whose line ends in a comment) they were not found. They sit outside the shared lib dirs on purpose: LDF runs in
`chain` mode and ignores `#if`, so in `lib/lib_div` every ESP32 image would
compile them. On a gemu adoption, check these dirs for changes too.
Audio comes from gemu's plugin `I2SAUDIO_32.bin` (testing release), not from
the firmware.

## TinyC compatibility

`TC_RELEASE` and `TC_SYSCALL_ABI` live in `tasmota/include/xdrv_124_tinyc_vm.h`.
The loader **refuses** a `.tcb` whose `abi_rev` is higher than its own, so when
the ABI steps, the image has to reach the devices before the programs from
`tasmota-sml-script` that were built against it.

## Conventions

- Source comments English and short; log lines always English.
- Do not commit unless asked.
