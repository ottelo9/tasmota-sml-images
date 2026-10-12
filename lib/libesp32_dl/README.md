# ESP-DL in Tasmota (nur ESP32-S3)

Rechenkern von [esp-dl](https://github.com/espressif/esp-dl) 3.3.11 plus
`vision/detect` (nur der Pico-Nachbearbeiter) und `vision/image`.
Gemessen am 16.09.2026 auf einer DFRobot AI CAM DFR1154:

| | |
|---|---|
| Flash: Rechenkern | +700 kB |
| Flash: + vision | +59 kB |
| Personenerkennung 224×224 | 216 ms |
| Kamerabild 640×480 entpacken | 252 ms (1/2) |
| Modell laden von der SD-Karte | 1,1 s, einmalig |

## ⚠️ Warum das eine PlatformIO-Bibliothek ist und keine IDF-Komponente

Tasmota bindet ein **vorkompiliertes** arduino-esp32. Ein Eintrag in
`tasmota/idf_component.yml` tut bei einem PlatformIO-Bau **nichts** — das
Manifest wird ausgewertet, wenn das Framework-Bündel selbst gebaut wird.

Weggelassen sind `audio/` und `dl_image_jpeg.cpp`; damit entfallen die
Abhängigkeiten `dl_fft` und `esp_new_jpeg`. Das JPEG entpackt `jpg2rgb565()`
aus esp32-camera, das ohnehin in der Firmware liegt.

`fbs_loader/lib/esp32s3/libfbs_model.a` ist vorkompiliert (gegen IDF 5.4.4;
bindet sauber gegen 5.5.4).

## Einbinden

⚠️ `platformio_override.ini` ist gitignoriert — dieser Block gehört in die
gewünschte **S3**-Umgebung, sonst fehlt alles Folgende:

```ini
build_flags = ${env:tasmota32.build_flags} -DARDUINO_USB_MODE=1 -DUSE_USB_CDC_CONSOLE
              -DUSE_TINYC_ESPDL
              -Ilib/libesp32_dl/esp-dl/dl
              -Ilib/libesp32_dl/esp-dl/dl/tool/include
              -Ilib/libesp32_dl/esp-dl/dl/tensor/include
              -Ilib/libesp32_dl/esp-dl/dl/base
              -Ilib/libesp32_dl/esp-dl/dl/base/isa
              -Ilib/libesp32_dl/esp-dl/dl/base/isa/tie728
              -Ilib/libesp32_dl/esp-dl/dl/base/isa/xtensa
              -Ilib/libesp32_dl/esp-dl/dl/math/include
              -Ilib/libesp32_dl/esp-dl/dl/model/include
              -Ilib/libesp32_dl/esp-dl/dl/module/include
              -Ilib/libesp32_dl/esp-dl/fbs_loader/include
              -Ilib/libesp32_dl/esp-dl/vision/detect
              -Ilib/libesp32_dl/esp-dl/vision/image
              -Ilib/libesp32_dl/esp-dl/vision/image/isa
              -DCONFIG_PIX_CVT_RGB888_TO_RGB888_SUPPORT=1
              -DCONFIG_PIX_CVT_RGB888_TO_GRAY_SUPPORT=1
              -DCONFIG_PIX_CVT_RGB565_TO_RGB888_SUPPORT=1
              -Llib/libesp32_dl/esp-dl/fbs_loader/lib/esp32s3
              -lfbs_model
lib_deps    = symlink://lib/libesp32_dl/esp-dl
```

Seit dem 17.09.2026 steht der Block auch in **`[env:tinyc32s3]`**, dem
S3-Testbuild des GitHub-Releases (Matter + Kamera + LVGL): dort hängt er an
`${env:tinyc_base.build_flags}` statt an `env:tasmota32`, sonst wortgleich.
Preis: +820 kB Flash (1,99 → 2,81 MB), die 16-MB-Partition trägt das.

⚠️ Zwei JPEG-Decoder, zwei Enums: der DFRobot-Build decodiert mit esp32-camera
(`esp_jpeg_image_scale_t`), der S3-Testbuild mit ESP32_JPDEC (`jpg_scale_t`).
Die Konstanten `JPG_SCALE_2X/4X/8X` gibt es in beiden — `tc_dl_person_run()`
übergibt sie darum direkt an `jpg2rgb565()` und nennt den Typ nicht; der erste
S3-Bau scheiterte genau daran.

⚠️ `lib_deps = symlink://…` ist Pflicht: die Bibliothek liegt zwei Ebenen tief,
und `lib_compat_mode = strict` verwirft alles, was in `library.json` nicht
`platforms` und `frameworks` deklariert. Ohne das wird sie **wortlos** nicht
gebaut, und es fällt erst beim Binden auf.

⚠️ Die Farbumwandlungen sind bei ESP-DL per Kconfig zugeschaltet. Ohne Kconfig
sind nur die drei `dl_image_pixel_cvt_dispatch_*.cpp` mitkopiert, die hier
gebraucht werden; die passenden `CONFIG_PIX_CVT_*`-Schalter stehen oben.

## Benutzung

    TinyCDl /sd/ped.espdl              Modell laden, pruefen, vermessen
    TinyCDlCam [schwelle] [skala] [bo] Kamerabild durch das Netz

Die Modelle sind **Dateien** (`MODEL_LOCATION_IN_SDCARD` ist nur
`fopen`/`fread`), liegen also nicht in der Firmware: die OTA bleibt klein und
ein Modellwechsel ist ein Datei-Upload. ⚠️ Auf .124 hängt die SD-Karte unter
`/sd` — die Weboberflaeche zeigt `/ped.espdl`, `fopen` braucht `/sd/ped.espdl`.

⚠️ `max_internal_size` ist **unbenutzbar** und im Treiber auf 0 gedeckelt:
esp-dl prueft fehlgeschlagene Allokationen nicht und laeuft in einen
Nullzeiger (`LoadProhibited`, `EXCVADDR 00000000`). 64 kB lief zweimal und warf
das Geraet beim dritten identischen Aufruf um.
