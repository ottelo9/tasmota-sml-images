# matter_c — a pure-C Matter 1.4 device for Tasmota + TinyC

A from-scratch, pure-C Matter 1.4 device stack for the gemu2015/Sonoff-Tasmota
fork. The C core is a **generic Matter engine**; a **TinyC script defines the
device** — it declares endpoints/clusters, publishes attribute values and
handles commands, with no firmware rebuild. Crypto rides the already-resident
**BearSSL** (no mbedTLS), so the incremental flash is small and per-handshake
bignum scratch stays on the stack rather than the heap.

> **Status:** working end-to-end. Commissions and operates with the CSA
> reference controller (**chip-tool**, full PASE → attestation → CSR → AddNOC →
> CASE over IPv6) **and Apple Home** — an On/Off plug + an Extended Color Light
> commission, appear as separate accessories, and control (on/off, brightness,
> colour) live. Fabrics persist on UFS across reboots; multiple fabrics and
> concurrent controller (iPhone + HomePods/Apple TV) sessions are supported.

**Provenance:** *inspired by* (not converted from) the device-subset
architecture of Tasmota Berry Matter (S. Hadinger). Wire format / protocol is
implemented from the CSA Matter 1.4.1 spec; pure algorithms (SPAKE2+, CASE,
Base38, Verhoeff, QR) are re-derived from the spec; crypto calls BearSSL
(T. Pornin, BSD) directly. Namespace `mtrc_`, C-native idioms. **GPLv3.**

---

## Architecture

```
matter_c.c        engine: lifecycle, RX ring, sessions, IM dispatch, scripting API
mtrc_tlv          Matter TLV encode/decode
mtrc_frame        message + protocol header codec
mtrc_mrp          Message Reliability Protocol (ack / backoff retransmit / dedup)
mtrc_crypto       thin BearSSL wrappers (AES-CCM, HKDF/HMAC/SHA-256, P-256, raw ECDSA)
mtrc_spake2p      PASE handshake (SPAKE2+ assembly on BearSSL muladd)
mtrc_case         CASE handshake + operational key schedule
mtrc_case_msg     Sigma1/2/3 + TBE/TBS message codecs
mtrc_sec          secured (AES-CCM) frame encode/decode
mtrc_cert         compact-TLV operational cert (RCAC/ICAC/NOC) parse
mtrc_csr          PKCS#10 CSR builder
mtrc_store        fabric table + UFS serialization (persists across reboot)
mtrc_dm           data-model registry (endpoints / clusters / attributes)
mtrc_im           Interaction Model (Read / Write / Subscribe / Invoke / Events)
qrcodegen         on-device SVG pairing QR
```

The host integration (mDNS, UDP:5540, UFS kv store, relay/LED bridge) is the
`matter_port_t` struct in `include/matter_c.h`, wired in
`tasmota/tasmota_xdrv_driver/xdrv_124_tinyc.ino`. Copy the folder + implement
the port to reuse on another firmware.

---

## TinyC scripting API

| Builtin | Purpose |
|---|---|
| `matterReset()` | clear the data model (root node only) |
| `matterAdd(deviceType)` → ep | add an endpoint (auto-adds Descriptor + Identify + the type's mandatory clusters) |
| `matterCluster(ep, cluster)` | add a cluster to an endpoint |
| `matterAttr(ep, cl, attr, type)` | declare an attribute (`MTR_BOOL/U8/U16/U32/S16/S32/S64/FLOAT/…`) |
| `matterSet(ep, cl, attr, value)` | set an integer/bool attribute |
| `matterSetFloat(ep, cl, attr, value, scale)` | set a value as `round(value*scale)` (S64) or float bits (FLOAT) |
| `matterGet(ep, cl, attr)` | read an attribute back |
| `matterEvent(ep, cl, eventId, a, b)` | emit a Matter event (e.g. Generic Switch press) |
| `matterName(ep, "label")` | name an endpoint so it shows as the accessory title (see *Naming* below) |
| `matterStart()` | go operational (publish operational mDNS for stored fabrics) |
| `MatterInvoke(ep, cl, cmd)` | callback: a controller invoked a command (OnOff/Level/Color/…) |
| `EverySecond()` | callback: push live sensor/meter values |

Pairing is opened from the **`/mt`** web page (Bind → 10-min window + on-device
QR); `/mt?unbind=1` factory-resets the fabrics. The device is not openly
pairable until Bind.

### Naming endpoints (`matterName`)

Plain Matter has no per-endpoint name, so a multi-endpoint node shows up in Apple
Home as *"Temperature Sensor 1 … N"*. `matterName(ep, "label")` fixes this by
turning the node into a Matter **bridge**: the first call lazily creates an
**Aggregator** (`0x000E`) endpoint, the named endpoint becomes a **Bridged Node**
(its Descriptor `DeviceTypeList` gains `0x0013`) and gets a **Bridged Device
Basic Information** cluster (`0x0039`) whose `NodeLabel` is the label the
controller displays. Call it right after `matterAdd` for that endpoint; it's
opt-in per endpoint (unnamed endpoints stay plain) and idempotent (re-call to
rename). Labels are ASCII — a string literal stores one byte per char, so use
plain ASCII (`"Buero Temp"`), then rename in the controller if you want umlauts.
Because adding a bridge changes the node's identity, an **already-commissioned
node must be removed and re-added** in the controller to pick up the names.

---

## Examples (`tasmota/tinyc/examples/`)

| Example | Device type(s) |
|---|---|
| `matter_plug.tc` | On/Off Plug-in Unit (relay) + Electrical Power Measurement |
| `matter_rgb.tc` | On/Off plug + Extended Color Light (WS2812) |
| `matter_sensors.tc` | Temperature + Humidity + Pressure (float) |
| `matter_powermeter.tc` | Electrical Sensor — power/voltage/current + cumulative energy (SML) |
| `matter_fan.tc` | Fan Control |
| `matter_shutter.tc` | Window Covering |
| `matter_leak.tc` | Water-Leak + Rain (Boolean State) |
| `matter_airquality.tc` | Air Quality — CO2 / PM2.5 / TVOC (float) |
| `matter_button.tc` | Generic Switch (events: single / double / long press) |

Deploy with `node tasmota/tinyc/tc_deploy.mjs examples/<name>.tc <device-ip>`.

### Apple Home device-type support

The device exposes every endpoint per the Matter spec, but **Apple Home only
renders device types it has a category for** — others commission cleanly but show
no tile (no error; not a device bug). Other controllers (Home Assistant, Google)
show more.

| Shown in Apple Home | Apple commissions but shows **no tile** |
|---|---|
| plug/outlet, on-off/dimmable/color light, temperature, humidity, contact, air quality, occupancy, leak | **pressure** (`0x0305`), **standalone Electrical Sensor / power meter** (`0x0510`) |

For energy, **Home Assistant** renders the Electrical Power/Energy clusters
(`0x0090`/`0x0091`) with graphs. To get consumption visible in *Apple*, put the
power cluster on a *plug* endpoint (`0x010A`) rather than a standalone sensor.

---

## Build & flash

Gate: `USE_MATTER_C` (mutually exclusive with `USE_HOMEKIT`). The local
`platformio_override.ini` envs `tinyc32c6-matter` / `tinyc32s3-matter` /
`tinyc32c3-matter` add `-DTINYC_MATTER` and unflag `-DTINYC_HOMEKIT`.

```
pio run -e tinyc32c6-matter                              # build
bash lib/libesp32_div/matter_c/test/flash_c6.sh <ip>    # safeboot-aware OTA
```

Primary DUT: ESP32-C6. Targets ESP32 / S3 / C6 over Wi-Fi/IP (BLE and Thread
are intentionally not used — Tasmota is already on the network).

---

## Flash & RAM footprint

Measured on ESP32-C6 as the firmware delta between `tinyc32c6-matter` (option ON)
and `tinyc32c6` (option OFF):

| | |
|---|---|
| **Flash cost of the Matter option** | **~56.7 kB** (58,048 B: 1,697,328 − 1,639,280) |
| vs the retired HomeKit stack (~156 kB) | **~1/3 the flash** |
| Crypto | **≈ 0 incremental** — BearSSL is already linked for TLS builds |
| Firmware in the ota_0 app partition (1.81 MB) | 1,697,328 B → **89.3 % used, ~198 kB free** |
| Static RAM delta | ~99 kB — but most of that is the **IPv6 + mDNS** stacks the gate switches on; `matter_c`'s own fixed tables are ~15–20 kB at the 32-endpoint sizing. Runtime free heap stays ~220 kB. |

The whole point of pure-C over Berry Matter (~343 kB) is this footprint: a complete
Matter 1.4 device — multi-fabric, all the device types above, the TinyC API — for
~57 kB of flash. (Per-object `size` reports 0 because the build uses LTO; measure
via the ON-vs-OFF firmware delta.)

## Debugging

Build with `-DMTRC_DIAG` to enable verbose debug logging (data-model endpoint
dump at `matterStart`, raw IM StatusResponse decode). Off in normal builds.

```
PLATFORMIO_BUILD_FLAGS="-DMTRC_DIAG" pio run -e tinyc32c6-matter
```

## Host self-tests

Pure-algorithm units run on the host (no device needed):

```
bash test/build_host.sh      # SPAKE2+      bash test/build_case.sh      # CASE keys
bash test/build_tlv.sh       # TLV codec    bash test/build_case_msg.sh  # CASE msgs
bash test/build_msg.sh       # frame + MRP  bash test/build_cert.sh      # cert parse
bash test/build_pase.sh      # PASE         bash test/build_csr.sh       # CSR builder
bash test/build_sec.sh       # AES-CCM      bash test/build_store.sh     # fabric store
bash test/build_ec.sh        # ECDH/ECDSA   bash test/build_dm.sh        # data model
```
