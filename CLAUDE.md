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

## TinyC compatibility

`TC_RELEASE` and `TC_SYSCALL_ABI` live in `tasmota/include/xdrv_124_tinyc_vm.h`.
The loader **refuses** a `.tcb` whose `abi_rev` is higher than its own, so when
the ABI steps, the image has to reach the devices before the programs from
`tasmota-sml-script` that were built against it.

## Conventions

- Source comments English and short; log lines always English.
- Do not commit unless asked.
