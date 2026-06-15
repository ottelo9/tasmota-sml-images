[![GitHub release (latest SemVer)](https://img.shields.io/github/v/release/ottelo9/tasmota-sml-images?style=for-the-badge)](https://github.com/ottelo9/tasmota-sml-images/releases/latest)
[![GitHub All Releases](https://img.shields.io/github/downloads/ottelo9/tasmota-sml-images/total?logo=github&style=for-the-badge)](https://github.com/ottelo9/tasmota-sml-images/releases/latest)

## Tasmota Images (Firmware) mit SML/Script Support [by ottelo]
Die fertigen Images findet ihr im Release Bereich. Einfach das korrekte Image für euren ESP herunterladen, entpacken und entweder über OTA oder über einen Flasher+Kabel übertragen.
Eine ausführliche Anleitung dazu findet ihr auf meiner [Homepage](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/). Welche Tasmota Features aktiviert/deaktiviert sind findet ihr im Release.  
  
[Download Statistik](https://tooomm.github.io/github-release-stats/?username=ottelo9&repository=tasmota-sml-images)  
[ottelo.jimdofree.com](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/)

### Welches Image (Firmware Binary) für welchen ESP?

**Wichtig — Varianten-Suffix:** Jedes ESP32-Image gibt es in zwei Varianten (ESP8266 nur `_tas`):
- `_tas` → klassischer **Tasmota-Scripter** + Google Charts (wie bisher)
- `_tc`  → **TinyC VM** + Browser-IDE (kein Scripter)

| Imagename | Beschreibung |
| ------------- | ------------- |
| **ESP32 Images:**    | **tasmota32xx_ottelo.zip** |
| tasmota32_ottelo_tas / _tc       | Generic ESP32, mit Ethernet Support |
| tasmota32x_ottelo_tas / _tc      | ESP32 Variante z.B. c3, c6, s2, s3, solo1, p4 (solo1, s3 mit Ethernet Support) |
| | |
| **ESP8266 Images:** | **tasmota8266_bundle_ottelo.zip** |
| Hinweis: | Ab V15.1.0 können nur die Scripte im Ordner [ESP8266](https://github.com/ottelo9/tasmota-sml-script/tree/main/ESP8266) verwendet werden! In den entsprechenden komprimiert Ordnern sind die Scripte ohne Kommentare, sie können 1:1 kopiert werden. |
| tasmota1m_ottelo_tas         | ESP mit 1M Flash (z.B. ESP01s = Hichi v1 Lesekopf). Nur Scripter (kein UFILESYS → kein TinyC). |
| tasmota4m_ottelo_tas         | ESP mit 4M+ Flash. Nur Scripter (kein _tc — ESP8266 4M hat kein I2C für den BinPlugin-Loader, und TinyC lohnt bei dem RAM kaum). Mit Shelly Pro 3EM / EcoTracker Emulation (+mDNS) als Meter für smarte Akkus (z.B. Marstek Venus / Jupiter) und für Steckdosen mit Energiemessung (mit 4M Speicher). |
| tasmota1m_energy_ottelo_tas  | ESP mit 1M Flash für Steckdosen mit Energiemessung z.B. Nous A1T, Sonoff Pow R2, Gosund EP2. Nur Scripter. Web-Upgrade nur über tasmota-minimal! |
| tasmota1m_shelly_ottelo_tas  | ESP mit 1M Flash. Mit Shelly Pro 3EM / EcoTracker Emulation (+mDNS) als Meter für smarte Akkus (z.B. Marstek Venus / Jupiter). Nur Scripter. Der Scriptspeicher ist auf 4096 Zeichen begrenzt (statt 8192). Für Scripte siehe, [ESP8266 Scripte - Ordner komprimiert](https://github.com/ottelo9/tasmota-sml-script/tree/main/ESP8266). HomeAssistant/MQTT aber weiterhin möglich. Web-Upgrade nur über tasmota-minimal! |
| tasmota-minimal              | Minimalimage, siehe Beschreibung unten oder tasmota_energy_ottelo |

Die Images sind alle gezippt. Im ZIP-Archiv befindet sich für den ESP32 immer auch das factory Image. Das wird beim erstmaligen Flashen auf einen leeren ESP32 benötigt bzw. wenn vorher noch kein Tasmota drauf war. Im ZIP-Archiv für den ESP8266 befinden sich alle Varianten gesammelt im bin.gz Format. Das .bin.gz muss immer für das Firmware-Upgrade via "Use file upload" über den Webbrowser verwendet werden, da das nicht komprimierte .bin Image sonst nicht übertragen werden kann (zu wenig Flash-Speicher). Falls es mal doch nicht passenden sollte (Fehlermeldung), dann muss vorher einmal das tasmota-minimal.bin.gz Image übertragen werden. Anschließend kann das .bin.gz Image übertragen werden. Zum erstmaligen Flashen via USB-Flasher muss die .gz entpackt werden.  

### (Factory)Image übertragen / flashen
Falls Tasmota bereits auf dem ESP ist, werden die Images einfach via OTA übertragen (Firmware Upgrade -> Use file upload)  
Ist das nicht der Fall muss das (Factory ESP32) Image (.bin) geflasht werden z.B. mit [Tasmota Web Installer](https://tasmota.github.io/install/)   
I.d.R. haben die ESP Boards eine USB-Buchse, die am PC einen COM-Port zur Verfügung stellen. Über diese wird das Image dann geflasht. Ist sowas nicht vorhanden braucht ihr einen USB-Seriell Adapter.  

### Passende Tasmota SML / Shelly / EcoTracker Scripte
Die findet ihr [hier](https://github.com/ottelo9/tasmota-sml-script).  

### Shelly Pro 3EM / EcoTracker Emulation für smarte Akkus (wie z.B. Marstek Venus C,E, Jupiter, Hoymiles, Growatt NOAH 2000)
[Kompatible Akku Liste](https://github.com/ottelo9/tasmota-sml-script/blob/main/README.md#pvakku-powermeter-emulator-esp32)  
Ab V15.0.1 habe ich den Support für die Emulation des Shelly/EcoTracker inkludiert. Die Emulation ist in allen ESP32 Images inkludiert. Für den ESP8266 habe ich eine abgespeckte Firmware erstellt (tasmota1m_shelly), dort funktionieren nur die kleinen Basisscripte (_Simple.tas findet ihr im [ESP8266 Ordner](https://github.com/ottelo9/tasmota-sml-script/tree/main/ESP8266/pvakku-powermeter-emulator/komprimiert)). Die Scripte findet ihr direkt auf euren ESP in Tasmota (DropDown) oder [hier](https://github.com/ottelo9/tasmota-sml-script/tree/main/pvakku-powermeter-emulator). Eine [Anleitung](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/#13a) habe ich auf meinem Blog veröffentlicht.  

## TinyC - Alternative zum Scripting/Berry (ESP32 / ESP8266 4M+)
**TinyC** von [gemu2015](https://github.com/gemu2015) — eine sehr gute und schnelle Alternative zum Scripting/Berry. Ihr könnt eure Programme direkt auf dem ESP in Tasmota schreiben und ausführen, in einer webbasierten TinyC-IDE. In der IDE sind sehr viele Beispiele im DropDown Menü wählbar. Der Sourcecode und auch das kompilierte Programm wird im Dateisystem von Tasmota gespeichert und von dort auch ausgeführt. Es können sogar mehrere Programme parallel ausgeführt werden. Das Ganze läuft wesentlich schneller als Scripting und benötigt auch weniger Platz. Und ihr könnt einfach alles in C-Code schreiben, statt kompliziertes Script.

**Image-Variante wählen:**  
Ab dieser Release gibt es pro ESP32-Plattform getrennte Images — `*_ottelo_tas` mit Scripter (wie bisher) oder `*_ottelo_tc` mit TinyC ohne Scripter. Beides gleichzeitig wird nicht mehr gebaut, weil das Flash unnötig aufbläht. ESP8266 (1M + 4M) gibt es nur als `_tas`.

**SML in `_tc`-Builds aktivieren:**  
Wenn ihr eins der [sml Beispielprogramme von gemus Repo](https://github.com/gemu2015/Sonoff-Tasmota/tree/universal/tasmota/tinyc/examples) verwendet (sml_..tc), dann müsst ihr einmal SML aktivieren via Checkbox (erreichbar via Button Einstellungen / Daten). Damit ihr die Beispielprogramme selbst kompilieren könnt müsst ihr vorher übrigens sml_chart_common.tc und sml_descriptor.tc via Manage File System hochladen!  
Wenn ihr ein eigenes Programm schreiben wollt, dann muss in eurem Code einmalig `tasm_rule = 1;` gesetzt werden (in `main()` oder `BootInit()`). Der Treiber lädt dann den Meter-Descriptor aus `/sml_meter.def`. Boot-festes Pattern siehe [`marstek_emu.tc`](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tinyc/examples/marstek_emu.tc):
```c
int main() {
tasm_rule = 1;
    return 0;
}
```

**=>** hier findet ihr eine [allgemeine Beschreibung](https://github.com/gemu2015/Sonoff-Tasmota/tree/universal/tasmota/tinyc). Und hier die TinyC Referenz in [Englisch ](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tinyc/TinyC_Reference.md) und [Deutsch](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tinyc/TinyC_Reference_DE.md).
**=>** Die IDE ist nicht vorinstalliert und muss 1x via Manage File System hochgeladen werden. Da die IDE immer weiter von gemu angepasst wird solltet ihr immer nur die IDE von meiner Repo verwenden, da diese für mein erzeugtes Images passt. Ansonsten gibt es ein Versions Hinweis in der IDE. Download der [tinyc_ide.html.gz](https://github.com/ottelo9/tasmota-sml-images/blob/main/tinyc/tinyc_ide.html.gz) und dann via File Upload auf euren ESP laden (Tools > Manage File System). Dann könnt ihr die IDE starten (Tools > TinyC Console)  
<img width="640" height="266" alt="image" src="https://github.com/user-attachments/assets/92bce2d3-cc8d-42eb-beb5-d7d98ee6ecea" />  
<img width="300" height="366" alt="image" src="https://github.com/user-attachments/assets/b18e905c-58bb-4252-8580-d76ca0374169" />

### Partitionslayout für TinyC-Images (4 MB ESP32)
TinyC speichert IDE (~150 KB), Source (`.tc`), Bytecode (`.tcb`), persist-Daten (`.pvs`) und ggf. `/sml_meter.def` im Filesystem. Default-Layout (`app2880k_fs320k`) hat nur 320 KB FS — bei TinyC wird's schnell eng. Gleichzeitig ist die TinyC-Firmware ~120 KB schlanker als Scripter+Charts, der App-Bereich kann also kleiner sein.

Deswegen nutzen die `_tc`-Images auf 4 MB-Boards das Layout [`esp32_partition_app1856k_fs1344k.csv`](https://github.com/arendst/Tasmota/blob/development/partitions/esp32_partition_app1856k_fs1344k.csv): 1856 KB app0 + 1344 KB FS (statt 2880 KB / 320 KB). Siehe auch [Discussion #37](https://github.com/ottelo9/tasmota-sml-images/discussions/37) und [Tasmota Safeboot-Doku](https://tasmota.github.io/docs/Safeboot/).

| Image | Partition |
|---|---|
| `tasmota32_ottelo_tc`, `tasmota32c3_ottelo_tc`, `tasmota32c6_ottelo_tc`, `tasmota32s2_ottelo_tc`, `tasmota32solo1_ottelo_tc` | `app1856k_fs1344k` (1024 KB mehr FS) |
| `tasmota32s3_ottelo_tc` | Default (16 MB Flash: `app3904k_fs11584k`) |
| `tasmota32p4_ottelo_tc` | Default (boardabhängig) |
| alle `_tas` | Default belassen — Scripter+Charts+HA+InfluxDB+MQTT-TLS würde 1856 KB sprengen |

#### Partitionen ohne Factory-Flash umstellen — `chkpt`
Der **gemu-Fork** (auf dem die ottelo-Images basieren) hat einen Laufzeit-Partition-Manager eingebaut — du brauchst kein USB-Kabel und keinen Factory-Flash, um das Layout zu ändern. Im TinyC-Build heißt der Befehl `TinyCChkpt`, in Builds ohne TinyC `chkpt`.

**Befehle:**
| Befehl | Wirkung |
|---|---|
| `TinyCChkpt` | Aktuelles Partition-Layout anzeigen |
| `TinyCChkpt p` | Auto-Pack: app0 auf Firmware-Größe + ~200 KB Overhead schrumpfen, Rest an FS |
| `TinyCChkpt p 1856` | app0 explizit auf 1856 KB setzen, Rest an FS (FS wird formatiert!) |
| `TinyCChkpt p 2880` | Zurück zum Default-Layout (wenn die laufende Firmware reinpasst) |

**Praktischer Workflow** (Lesegerät im Einsatz, kein Aufschrauben):
1. Files via WebUI → "Manage File System" sichern (Settings.json, Skripte, `/sml_meter.def`, `tinyc_ide.html.gz`).
2. OTA-Upgrade auf das neue `_tc`-Image (`.bin.gz` über "Firmware Upgrade → Use file upload"). Das überschreibt nur app0, Partitionen bleiben unverändert.
3. Nach dem Reboot in der Konsole: `TinyCChkpt p 1856` → packt um, formatiert FS, rebootet automatisch.
4. Files wieder hochladen.

**Voraussetzungen / Stolperfallen:**
- **Safeboot-Partition muss existieren** (Recovery-Pfad). Ohne Safeboot lehnt `chkpt p` ab. Bei allen ottelo-ESP32-Builds ist Safeboot dabei.
- **Neue app0 ≥ laufende Firmware.** Sonst meldet der Befehl `requested size too small for current firmware!` und macht nichts.
- **FS wird IMMER formatiert** bei `chkpt p`, auch wenn die Tabelle sich nicht ändert ("nothing to do" → trotzdem `LittleFS.format()`). Backup vorher ist Pflicht.
- Nach `chkpt p` ist ein **Reboot** nötig, sonst zeigt die Info-Seite noch den alten FS-Wert (Tasmota cached `LittleFS.totalBytes()` beim Boot).

**Factory-Flash** brauchst du nur noch, wenn:
- Das Image zu groß für die aktuell konfigurierte app0 ist,
- Kein Safeboot vorhanden ist,
- Du das Layout sauber neu aufsetzen willst (z.B. nach einem unsauberen `chkpt`-Abbruch).

## Tasmota Image selbst erstellen - Anleitung für ESP32 / ESP8266
In der user_config_override.h findet ihr eine Liste mit Features/Treibern (#define bzw. #undef), die ich für meine ESP Tasmota Images/Firmware verwende und auf ottelo.jimdofree.com zum Download anbiete. Die hier hochgeladenen Dateien können euch dabei helfen, ein eigenes angepasstes Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP ein Stromzähler über ein Lesekopf auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion (SonOff, Gosund, Shelly) habt und ihr die Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt. Das passende Script findet ihr in meinem anderen Repo https://github.com/ottelo9/tasmota-sml-script.  

### Wie verwenden?
Die Dateien in euer Tasmota Projektverzeichnis von Visual Studio Code oder Gitpod kopieren (ggf. überschreiben). (Liste kann unvollständig sein)  
- TasmotaProjekt/`tasmota/user_config_override.h`
- TasmotaProjekt/`platformio_tasmota_cenv.ini`
- TasmotaProjekt/`ccache_wrapper.py` <- siehe ccache (unten)
- TasmotaProjekt/tasmota/tasmota_xdrv_driver/`xdrv_10_scripter.ino` <- (optional) die aktuellste Scripter Source aus der [gemu2015 Repo](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tasmota_xdrv_driver/xdrv_10_scripter.ino)
- TasmotaProjekt/tasmota/tasmota_xsns_sensor/`xsns_53_sml.ino` <- (optional) die aktuellste SML Source aus der [gemu2015 Repo](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tasmota_xsns_sensor/xsns_53_sml.ino)
- TasmotaProjekt/tasmota/include/`xdrv_124_tinyc_vm.h` <- (wichtig für TinyC) die aktuellste TinyC-VM Source aus der [gemu2015 Repo](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/include/xdrv_124_tinyc_vm.h) — ändert sich fast täglich
- TasmotaProjekt/tasmota/`tasmota.ino` <- (optional) `image_name`-Buffer von 33 auf 64 Bytes erweitert, damit lange `CODE_IMAGE_STR` (z.B. `ESP32-C3 TC ottelo.jimdofree.com`) nicht abgeschnitten werden
- TasmotaProjekt/tasmota/tasmota_xdrv_driver/`xdrv_01_9_webserver.ino` <- (optional) HTTP-Footer auf zwei Zeilen aufgeteilt: Zeile 1 `Tasmota <version> <image_name>`, Zeile 2 `Tasmota developed by Theo Arends`
- TasmotaProjekt/boards/`esp32s3-qio.json` <- (optional) für ESP32-S3 Image (siehe `platformio_tasmota_cenv.ini`) ohne PSRAM Support, siehe [Issue 32](https://github.com/ottelo9/tasmota-sml-script/issues/32)
- ggf. noch weitere Dateien, je nach Release...

Eine ausführliche Anleitung zum Einrichten von Tasmota und weitere Details findet ihr auf meinem Blog:
[https://ottelo.jimdofree.com](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/)

### Kompilieren
Zum Kompilieren unter Gitpod/VSC den passenden Befehl in die Console eingeben: 

Für die ESP32 Images muss man als erstes die Safeboot Images erstellen (die werden dann in die .factory Images eingefügt, wie ein Bootloader) z.B. `pio run -e tasmota32-safeboot`.  

Um alle gleichzeitig zu erstellen:  
`pio run -e tasmota32-safeboot -e tasmota32c3-safeboot -e tasmota32c6-safeboot -e tasmota32s2-safeboot -e tasmota32s3-safeboot -e tasmota32solo1-safeboot -e tasmota32p4-safeboot`

Nun können die einzelnen Images erstellt werden (landen dann im Ordner build_output). Suffix `_tas` = Scripter, `_tc` = TinyC:  

ESP32 (Scripter-Variante `_tas`):  
`pio run -e tasmota32_ottelo_tas`        (Generic ESP32)  
`pio run -e tasmota32s2_ottelo_tas`  
`pio run -e tasmota32s3_ottelo_tas`  
`pio run -e tasmota32c3_ottelo_tas`  
`pio run -e tasmota32c6_ottelo_tas`  
`pio run -e tasmota32solo1_ottelo_tas`   (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)  
`pio run -e tasmota32p4_ottelo_tas`  

ESP32 (TinyC-Variante `_tc`):  
`pio run -e tasmota32_ottelo_tc`  
`pio run -e tasmota32s2_ottelo_tc`  
`pio run -e tasmota32s3_ottelo_tc`  
`pio run -e tasmota32c3_ottelo_tc`  
`pio run -e tasmota32c6_ottelo_tc`  
`pio run -e tasmota32solo1_ottelo_tc`  
`pio run -e tasmota32p4_ottelo_tc`  

ESP8266:  
`pio run -e tasmota1m_ottelo_tas`        ( = 1M Flash, nur Scripter)  
`pio run -e tasmota1m_energy_ottelo_tas` ( = 1M Flash, nur Scripter. Update nur über minimal Image. Für SonOff POW (R2) / Gosund EP2 / SonOff Dual R3 v2 / Nous A1T)  
`pio run -e tasmota1m_shelly_ottelo_tas` ( = 1M Flash, nur Scripter. Update nur über minimal Image. Für Shelly/EcoTracker Emu Scripte für smarte Akkus wie z.B. Marstek (Venus, Jupiter, B2500) oder Hoymiles (MS-A2))  
`pio run -e tasmota4m_ottelo_tas`        (>= 4M Flash, nur Scripter — kein _tc, ESP8266 4M hat kein I2C für den BinPlugin-Loader)  

Um alle gleichzeitig zu erstellen (Bash + jq):  
`pio run $(pio project config --json-output | jq -r '.[] | .[0] | select(test("_ottelo_(tc|tas)$")) | sub("env:"; "-e ")')`

PowerShell:  
```powershell
$envs = pio project config --json-output | jq -r '.[][0] | select(test("_ottelo_(tc|tas)$")) | sub("env:"; "-e ")'
pio run ($envs -split "`n")
```

### Compile-Zeit reduzieren mit ccache (optional, aber sehr empfohlen)
Wenn ihr mehrere Varianten hintereinander baut (`_tas` + `_tc`, oder mehrere Plattformen), kompiliert PlatformIO jede env komplett neu — der eingebaute `build_cache_dir` (PIO's eigener Object-Cache) greift nur innerhalb derselben env, weil andere `-D`-Flags andere Object-Hashes erzeugen.

**ccache** ergänzt das als zweite Stufe im **Preprocessor-Mode**: Files wie lwip/freertos/lvgl/esp32-arduino-core, die `OTTELO_VARIANT_*` gar nicht sehen, haben in beiden Builds identischen Preprocessor-Output → Cache-Hit. Bei einem Build-Paar `_tas` + `_tc` derselben Plattform werden typisch ~95 % der Files aus dem ccache wiederverwendet. Spart bei kompletten Multi-Variant-Builds 60–75 % Wallclock-Zeit.

**Setup (einmalig):**

1. ccache installieren:
   - Linux/Gitpod/VMware-Ubuntu: `sudo apt-get install ccache`
   - macOS: `brew install ccache`
   - Windows: `choco install ccache` (alternativ Scoop/MSYS2)

2. Cache-Größe und sloppiness konfigurieren — **ohne diese Settings bleibt die Hit-Rate bei 0 %**, weil Tasmota `__DATE__` / `__TIME__` / Build-Counter in den Sources hat:
   ```bash
   ccache -M 4G
   ccache --set-config sloppiness=time_macros,pch_defines,include_file_mtime,include_file_ctime,locale,system_headers
   ccache --set-config hash_dir=false
   ccache --set-config compiler_check=content
   ```
   Was die einzeln machen:
   - `time_macros` → `__DATE__`/`__TIME__` werden beim Hashing ignoriert
   - `pch_defines` → Header-Defines beeinflussen Hash nicht künstlich
   - `include_file_mtime,include_file_ctime` → Header-Zeitstempel werden ignoriert
   - `hash_dir=false` → Build-Verzeichnis-Pfad geht nicht in den Hash (essenziell für `_tas`/`_tc`-Cross-Hits)
   - `compiler_check=content` → Compiler-Binary-Hash statt mtime (kein Miss nach PIO-Update)

3. Settings verifizieren:
   ```bash
   ccache --get-config sloppiness
   ccache --get-config hash_dir
   ccache --get-config compiler_check
   ```

4. Die [`ccache_wrapper.py`](ccache_wrapper.py) aus diesem Repo mit ins Tasmota-Root kopieren (neben `platformio.ini`).

5. In `platformio_tasmota_cenv.ini` ist das Script im `[env:tasmota_ottelo_base]` als Pre-Hook eingetragen und wird von allen `_ottelo_tas`/`_tc`-Envs geerbt — also nichts mehr zu tun.

**Funktionscheck nach einem Build:**

```bash
ccache -s
```

Erwartete Zeilen nach dem ersten Build:
```
Cacheable calls: 384 / 384 (100 %)
Hits:              0 / 384 (0 %)
Misses:          384 / 384 (100 %)
```

Erwartete Zeilen nach einem zweiten Build (andere Variante derselben Plattform, z.B. `_tc` nach `_tas`):
```
Cacheable calls: 764 / 764 (100 %)
Hits:            380 / 764 (~50 %)       ← die ~95% Cache-Hits aus dem TC-Build
  Preprocessed:  380 / 380 (100 %)
Misses:          384 / 764
```

Im Build-Output erscheint außerdem direkt am Anfang:
```
[ccache_wrapper] aktiv: /usr/bin/ccache
[ccache_wrapper] patched: CCCOM, CXXCOM, SHCCCOM, SHCXXCOM
```

**Wenn der Cache leer bleibt / 0 % Hits:**

- Im Output `[ccache_wrapper]`-Zeile checken — wenn die fehlt, läuft das Pre-Script nicht (Pfad/extra_scripts-Vererbung prüfen).
- `ccache --get-config sloppiness` zeigen lassen — wenn `time_macros` fehlt, alle Misses durch `__DATE__`/`__TIME__`.
- Für einen sauberen Cross-Env-Test (PIO-Cache stört sonst):
  ```bash
  rm -rf .cache .pio/build
  ccache -C && ccache -z
  pio run -e tasmota32c3_ottelo_tas       # Build #1: füllt ccache
  pio run -e tasmota32c3_ottelo_tc        # Build #2: zieht aus ccache
  ccache -s
  ```

**Hinweis zu PIO's eigenem Cache (`.cache/` im Tasmota-Root):**

Tasmota's `platformio.ini` setzt `build_cache_dir = .cache` — das ist PIO's eigener Object-Cache (Stufe 1), unabhängig von ccache (Stufe 2). PIO-Cache greift bei **gleicher Env** (z.B. zweiter Build derselben `_tc`-Variante), ccache zusätzlich bei **Env-Wechsel** (`_tas` ↔ `_tc`). Beide aktiv lassen — sie ergänzen sich.

### Release-ZIPs erstellen mit `make_tasmota_zips_extended.sh`

Nach dem Bauen aller Envs liegen die Firmware-Dateien (`*.bin`, `*.factory.bin`, `*.bin.gz`) in `Tasmota-xxx/build_output/firmware/`. Das mitgelieferte [`make_tasmota_zips_extended.sh`](make_tasmota_zips_extended.sh) erstellt daraus die Release-ZIPs pro Plattform und Variante.

**Verwendung:**

1. Script in das Firmware-Output-Verzeichnis kopieren:
   ```bash
   cp /pfad/zu/tasmota-sml-images/make_tasmota_zips_extended.sh \
      ~/Tasmota-15.4.0/build_output/firmware/
   ```

2. Ins Verzeichnis wechseln und ausführen:
   ```bash
   cd ~/Tasmota-15.4.0/build_output/firmware
   chmod +x make_tasmota_zips_extended.sh
   ./make_tasmota_zips_extended.sh
   ```

3. Die ZIPs landen im Unterordner `tasmota_zips/`:
   - `tasmota8266_bundle_ottelo.zip` — alle ESP8266-Builds gebündelt + tasmota-minimal
   - `tasmota32<board>_ottelo_tas.zip` / `_tc.zip` — pro ESP32-Plattform jeweils zwei ZIPs (Scripter / TinyC), insgesamt 14 Stück

**Verhalten bei fehlenden Builds:** Wenn eine erwartete Datei fehlt (z.B. C6 oder P4 nicht gebaut), meldet das Script `FEHLER: Datei fehlt: …` und überspringt nur dieses eine ZIP — die anderen werden weiterhin erstellt.

### FAQ
- Beim Übertragen eines Images (z.B. tasmota1m_shelly_ottelo) für ESP8266 via OTA (Use file upload) bekomme ich folgenden Fehler:
`Upload Failed. Not enough space.`
Das liegt daran, dass der ESP zu wenig Speicher hat. Verwende zuerst das tasmota-minimal.bin Image, danach kannst du dann das normale Image übertragen.


### Infos
Mehr Infos bzgl. ESP32 Versionen:  
https://tasmota.github.io/docs/ESP32/#esp32_1

Für weitere ESPs siehe:  
https://github.com/arendst/Tasmota/blob/development/platformio_override_sample.ini bei default_envs

Features/Treiber (de)aktivieren:  
https://tasmota.github.io/docs/Compile-your-build/#enabling-a-feature-in-tasmota

Hier ist eine Übersicht aller Features/Treiber:  
https://github.com/arendst/Tasmota/blob/development/tasmota/my_user_config.h

Wenn beim Kompilieren eine Standard Tasmota Variante verwendet wird (z.B. `-e tasmota32c3`), dann werden Features/Treiber für diese Konfiguration 
(siehe https://github.com/arendst/Tasmota/blob/development/platformio_tasmota_env32.ini z.B. [env:tasmota32c3]) verwendet und die deaktivierten Features, 
die ihr in der `user_config_override.h` eingetragen habt, überschrieben und somit doch verwendet! 
Wenn Features/Treiber (de)aktivieren werden sollen, dann eine eigene Variante in `platformio_tasmota_cenv.ini` erstellen und `-DFIRMWARE_TASMOTA32` entfernen, 
da wie bereits oben erwähnt, ESP32 Standard Features wie Berry usw verwendet werden (siehe `FIRMWARE_TASMOTA32` in `tasmota_configuration_ESP32.h`). 
Siehe auch https://tasmota.github.io/docs/Compile-your-build/#customize-your-build  

Noch eine Info:  
Immer die neuste Tasmota Platform Framework builds verwenden. D.h. in der platformio_tasmota32.ini bei [core32] platform url aktualisieren  

[Offizielle Tasmota Github Seite](https://github.com/arendst/Tasmota)  

### Testtools:
<b>UDP/HTTP/PING Tester</b>  
Ihr könnt die Emus mit meinem [PowerScript](https://github.com/ottelo9/tasmota-sml-script/blob/main/pvakku-powermeter-emulator/Shelly-EcoTracker%20Tester.ps1) Tool testen (UDP, HTTP Get, Ping):  
<img width="500" height="1109" alt="image" src="https://github.com/user-attachments/assets/c70370f6-00c6-4fc8-bef0-9253bf2b6ece" />

<b>SML Simulator</b>  
Gemu2015 hat ein [SML Simulator Tool](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tinyc/sml_emulator.html) (simuliert einen x-beliebigen Stromzähler) für den Google Chrome Browser erstellt. Zum Simulieren eines Stromzählers wird ein USB Lesekopf am PC benötigt. Das Tool kann auch Zählerscripte (Meter Descriptor) erstellen und auch direkt an Tasmota senden. Wieso Google Chrome Browser? Nur Chrome unterstützt das serielle Interface.  
<img width="500" height="385" alt="image" src="https://github.com/user-attachments/assets/9a5190da-2e8c-4198-97d0-4152276ad7a1" />

### Debugging
Falls Tasmota mal ab und zu aus unbekannten Gründen neustarten (Reboots) sollte, gibt es mehrere Möglichkeiten die Ursache des Problems festzustellen.

ESP via USB mit dem PC verbinden und ein Terminalprogramm (putty oder MobaXterm) laufen (loggen) lassen bis der Reboot passiert. 
Tasmota gibt dann beim Crash/Reboot ein Crash Dump aus. Das könnte z.B. so aussehen:  

`Guru Meditation Error: Core  0 panic'ed (Stack protection fault).`
`Detected in task "loopTask" at 0x42023fee`

Hat man das Image selbst kompiliert, so kann man in der .map Datei nach der Adresse 0x42023fee bzw. 0x42023* suchen. Die map Datei liegt unter `\.pio\build\tasmota32xxx\firmware.map`. 
Dort findet man dann die Funktion, in der das Problem (in diesem Fall Buffer Overflow) aufgetaucht ist. Die genaue Stelle kann man nur herausfinden, wenn man die firmware.asm Datei hat. 
Die wird erst erstellt, wenn man folgendes mit in die [platformio_tasmota_cenv.ini](platformio_tasmota_cenv.ini) einfügt:  
`extra_scripts           = ${env:tasmota32_base.extra_scripts}`  
`                          post:pio-tools/obj-dump.py`  
  
Die extrem große Datei liegt unter `\.pio\build\tasmota32xxx\firmware.asm`. Dort kann man dann exakt nach der Adresse suchen.  
Für den C6 muss man aber noch die `obj-dump.py` anpassen, da der C6 dort fehlt. Ich habe einfach die Zeile vom C3 kopiert.

## Welche Images welchen Funktionsumfang bieten, könnt ihr anhand dieser Features/Treiber erkennen:
**(1) Allgemeine Features (in beiden Varianten — `_tas` und `_tc`):**

```
USE_COUNTER
USE_DEEPSLEEP (ESP32)
USE_HOME_ASSISTANT
USE_IMPROV
USE_LIGHT
USE_PING
USE_SML_M
USE_SML_CRC
USE_SML_AUTHKEY
USE_SPI (nur ESP32)
USE_SUNRISE
USE_TIMERS
USE_TIMERS_WEB
USE_UFILESYS (nur ESP32, ESP8266 4M+)
USE_WEBSERVER
USE_WEBCLIENT_HTTPS
USE_TLS
USE_MQTT_TLS (nur ESP32)
USE_INFLUXDB (nur ESP32)
USE_ESP32_SW_SERIAL (nur ESP32)
USE_BMP (ESP32, ESP8266 1M Energy)
USE_DS18x20 (ESP32, ESP8266 1M Energy)
SET_ESP32_STACK_SIZE 12 * 1024
UFSYS_SIZE 16384 (ESP32)
UFSYS_SIZE 8192 (ESP8266 4M+)
```

**(2) Features für <sup>(1)</sup>Steckdosen mit Energiemessfunktion:**
In allen ESP32 bzw. nur im ESP8266 (1M) Image `tasmota_energy_ottelo_tas` sind diese Treiber aktiv!
<sup>(1)</sup>ESP8266: z.B. SonOff POW(R2), Gosund EP2, SonOff Dual R3 v2, NousA1T
<sup>(1)</sup>ESP32: z.B. Shelly Plus Plug S

```
USE_ADE7953
USE_BL09XX
USE_DHT
USE_CSE7766
USE_ENERGY_MARGIN_DETECTION
USE_ENERGY_POWER_LIMIT
USE_ENERGY_SENSOR
USE_HLW8012
USE_I2C
```

**(3) [Tasmota Scripting](https://tasmota.github.io/docs/Scripting-Language/) (USE_SCRIPT) — nur in `_tas`-Images:**

```
USE_SCRIPT
USE_SCRIPT_FATFS_EXT (ESP32, ESP8266 4M+)
USE_EEPROM (ESP8266 1M)
EEP_SCRIPT_SIZE 8192 (ESP8266 1M / 1M Energy)
EEP_SCRIPT_SIZE 4096 (ESP8266 1M Shelly)
USE_GOOGLE_CHARTS
USE_SCRIPT_WEB_DISPLAY
USE_HTML_CALLBACK
LARGE_ARRAYS
SCRIPT_LARGE_VNBUFF (ESP32)
MAX_ARRAY_SIZE 2000 (ESP32)
USE_CW_CALC
USE_ANGLE_FUNC (ESP32, ESP8266 +4M)
USE_FEXTRACT (ESP32, ESP8266 +4M)
USE_SCRIPT_SERIAL (nur ESP32)
SCRIPT_FULL_WEBPAGE (nur ESP32)
USE_SCRIPT_TCP_SERVER (nur ESP32)
USE_SCRIPT_TASK (nur ESP32)
USE_SCRIPT_MDNS (ESP32, ESP8266 1M Shelly)
USE_SCRIPT_GLOBVARS
USE_SCRIPT_JSON_EXPORT
------------------
für Script-DropDown-Menüs:
SCRIPT_LIST_DOWNLOAD_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-script/main/script-list-menu/scripts/"
SCRIPT_LIST "scripts.json"
```

**(4) [TinyC](https://gemu2015.github.io/Sonoff-Tasmota/) — nur in `_tc`-Images:**

```
USE_MATTER
USE_TINYC          (XDRV_124, TinyC-VM)
USE_TINYC_IDE      (selbstgehostete Browser-IDE)
USE_SML_SCRIPT_CMD (entkoppelt SML_SetBaud / SML_Write vom Scripter)
------------------
Partitionslayout:
ESP32 4M / ESP32-C3 / -C6 / -S2 / -SOLO1:  app1856k_fs1344k
ESP32-S3 16M:                              app3904k_fs11584k (Default)
ESP32-P4:                                  Board-Default
ESP8266 4M:                                eigenes Schema
------------------
SML aktivieren im TC-Programm:
  tasm_rule = 1;         // einmalig setzen
  // Meter-Descriptor liegt in /sml_meter.def
```

**(5) Features für ESP32-Module mit LAN-Port (Ethernet/LAN):**
Nur in `tasmota32_ottelo_*`, `tasmota32solo1_ottelo_*`, `tasmota32s3_ottelo_*` und `tasmota32p4_ottelo_*` aktiviert!

```
USE_ETHERNET
USE_WT32_ETH01
```

**(6) Features und Treiber die ich deaktiviert habe:**
Siehe `#undef FEATURE` in der Datei [user_config_override.h](user_config_override.h).

------------------
Bedanken möchte ich mich besonders bei [gemu2015](https://github.com/gemu2015), der das Tasmota Scripting und SML entwickelt hat und mir immer sofort bei Problemen geholfen hat. 
Und natürlich beim restlichen [Tasmota Entwickler-Team](https://tasmota.github.io/docs/About/), für das grandiose Tasmota :).
