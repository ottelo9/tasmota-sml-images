# TinyC — Custom Build Guide

Pre-built firmware is provided for ESP32 (4MB), ESP32-S3 (16MB), ESP32-C3 (4MB), and ESP8266 (4MB).
For other ESP32 variants, build your own firmware using the configurations below.

## Prerequisites

1. PlatformIO installed (CLI or VS Code extension)
2. Tasmota source tree cloned: `git clone https://github.com/gemu2015/Sonoff-Tasmota`
3. No other `#define device_*` active in `user_config_override.h`

## Required Defines

The `TINYC_TESTING` build flag (set via `-DTINYC_TESTING` in platformio) activates all
necessary defines in `user_config_override.h`. This includes:

```c
// Core TinyC
#define USE_TINYC              // TinyC VM (XDRV_124)
#define USE_TINYC_IDE          // Browser IDE endpoint

// Infrastructure
#define USE_SCRIPT             // Script engine infrastructure
#define USE_SCRIPT_I2C         // I2C from scripts
#define USE_SCRIPT_SPI         // SPI from scripts
#define USE_SCRIPT_SERIAL      // Serial from scripts
#define USE_SCRIPT_TIMER       // Timer support
#define USE_UFILESYS           // Filesystem (required for IDE)
#define USE_I2C                // I2C bus
#define USE_SPI                // SPI bus

// Display (ESP32 only)
#define USE_DISPLAY            // Display driver
#define USE_UNIVERSAL_DISPLAY  // Universal display
#define USE_UNIVERSAL_TOUCH    // Touch input

// Networking
#define USE_WEBCLIENT_HTTPS    // HTTPS client
#define USE_WEBSEND_RESPONSE   // Web response forwarding
#define USE_SENDMAIL           // Email support

// Optional features
#define USE_SML_M              // Smart meter interface
#define USE_FEXTRACT           // File extract functions
#define USE_DEEPSLEEP          // Deep sleep support
#define USE_COUNTER            // Pulse counter
#define USE_SUNRISE            // Sunrise/sunset calculation
```

## Optional Build Flags

| Flag | Effect |
|---|---|
| `-DTINYC_HOMEKIT` | Enable Apple HomeKit support |
| `-DTINYC_NO_SCRIPTER` | Exclude Tasmota Scripter engine (~98 KB flash saved). SML smart meter remains available. Use this when TinyC fully replaces Scripter. |

## platformio_override.ini Sections

Add these sections to your `platformio_override.ini`. Each extends the TinyC base
configuration. Build with: `pio run -e <environment-name>`

### Common Base (required)

```ini
[env:tinyc_base]
; Common TinyC test build settings
build_flags             = ${env:tasmota32_base.build_flags}
                          -DTINYC_TESTING
```

---

### ESP32-S2 (4MB)

Single-core Xtensa, 4MB flash. Good for simple USB-connected devices.

```ini
[env:tinyc32s2]
extends                 = env:tasmota32_base
board                   = esp32s2
board_build.f_cpu       = 240000000L
board_build.partitions  = partitions/esp32_partition_app1856k_fs1344k.csv
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
                          lib/libesp32/CORE2_Library
                          libesp32_lvgl
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

### ESP32-C6 (4MB)

RISC-V single-core with WiFi 6 and Zigbee/Thread. Same architecture as C3.

```ini
[env:tinyc32c6]
extends                 = env:tasmota32_base
board                   = esp32c6
board_build.f_cpu       = 160000000L
board_build.partitions  = partitions/esp32_partition_app1856k_fs1344k.csv
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
                          lib/libesp32/CORE2_Library
                          libesp32_lvgl
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

### ESP32 Solo1 (4MB)

Single-core ESP32 (some Sonoff devices, ESP32-SOLO1 module).

```ini
[env:tinyc32solo1]
extends                 = env:tasmota32_base
board                   = esp32_solo1_4M
board_build.f_cpu       = 160000000L
board_build.partitions  = partitions/esp32_partition_app1856k_fs1344k.csv
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

### ESP32-S3 (8MB)

Dual-core Xtensa with 8MB flash. Many devkits and modules use 8MB.

```ini
[env:tinyc32s3-8M]
extends                 = env:tasmota32_base
board                   = esp32s3_8M
board_build.f_cpu       = 240000000L
board_build.mcu         = esp32s3
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

### ESP32-S3 (4MB)

Dual-core Xtensa S3 with only 4MB flash. Use optimized partition.

```ini
[env:tinyc32s3-4M]
extends                 = env:tasmota32_base
board                   = esp32s3_4M
board_build.f_cpu       = 240000000L
board_build.mcu         = esp32s3
board_build.partitions  = partitions/esp32_partition_app1856k_fs1344k.csv
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

### ESP32 (16MB)

Classic ESP32 with 16MB flash (e.g., ESP32-WROVER-E 16MB).

```ini
[env:tinyc32-16M]
extends                 = env:tasmota32_base
board                   = esp32_16M
board_build.f_cpu       = 240000000L
build_flags             = ${env:tinyc_base.build_flags}
                          -DTINYC_HOMEKIT
lib_ignore              = ${env:tasmota32_base.lib_ignore}
                          TTGO TWatch Library
                          Micro-RTSP
                          epdiy
lib_extra_dirs          = ${common.lib_extra_dirs}
                          lib/libesp32
                          lib/libesp32_div
```

---

## Partition Tables

| Flash Size | Partition File | App Size | Filesystem | Notes |
|---|---|---|---|---|
| 4MB | `esp32_partition_app1856k_fs1344k.csv` | 1,856 KB | 1,344 KB | Recommended for 4MB with safeboot |
| 8MB | `esp32_partition_app3904k_fs3392k.csv` | 3,904 KB | 3,392 KB | 8MB with safeboot |
| 16MB | `esp32_partition_app3904k_fs11584k.csv` | 3,904 KB | 11,584 KB | 16MB with safeboot (default for S3-16M) |

For 4MB boards: the TinyC firmware is ~1,650 KB, leaving ~200 KB headroom in the 1,856 KB app partition.

## Build Command

```bash
# Build a specific environment
pio run -e tinyc32c6

# Build and upload via serial
pio run -e tinyc32c6 --target upload

# Build and upload via OTA
pio run -e tinyc32c6 --target upload --upload-port <device-ip>
```

## OTA Partition Management

Devices in sealed housings without serial access can resize their partition layout
at runtime using the `chkpt` console command. This is useful when switching between
firmware versions of different sizes.

### Commands

| Command | Effect |
|---|---|
| `chkpt` | List current partition table |
| `chkpt p` | Auto-pack: shrink app0 to firmware size + ~200 KB overhead, expand filesystem. **Warning: all files on the filesystem are lost!** |
| `chkpt p <KB>` | Set app0 to a specific size in KB (1024–3904), adjust filesystem accordingly. **Warning: all files on the filesystem are lost!** |
| `chkpt a1` | Add 64 KB custom partition (for plugin drivers), taken from filesystem |
| `chkpt a2`..`a4` | Add 128/192/256 KB custom partition |
| `chkpt r` | Remove custom partition, return space to filesystem |

### Examples

```
# Device has app0=2880 KB, spiffs=256 KB — shrink app to gain filesystem:
chkpt p
  → app0=1856 KB, spiffs=1280 KB (firmware 1603 KB + 252 KB overhead)

# Need to flash a larger firmware later — expand app first:
chkpt p 2880
  → app0=2880 KB, spiffs=256 KB

# With custom plugin partition present — custom is always preserved:
chkpt p
  → app0=1856 KB, spiffs=1280 KB, custom=64 KB (unchanged at end of flash)
```

### Notes

- The device reboots automatically after any partition change
- Filesystem (LittleFS) is formatted during resize — **back up files first**
- The custom partition (plugin drivers) is never moved or removed by `chkpt p`
- The `chkpt` infrastructure is always compiled into TinyC firmware, no extra build flags needed
- App size is aligned to 64 KB boundaries
- `chkpt p` requires a safeboot partition — devices without safeboot are refused (no recovery if partition table gets corrupted)

## After Flashing

1. Upload `tinyc_ide.html.gz` to the device filesystem via **Consoles > Manage File System**
2. Open `http://<device-ip>/tinyc_ide.html` in a browser
3. Write code, press **Ctrl+Enter** to compile, **Ctrl+Shift+Enter** to run on device
