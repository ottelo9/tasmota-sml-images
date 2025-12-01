[![GitHub release (latest SemVer)](https://img.shields.io/github/v/release/ottelo9/tasmota-sml-images?style=for-the-badge)](https://github.com/ottelo9/tasmota-sml-images/releases/latest)
[![GitHub All Releases](https://img.shields.io/github/downloads/ottelo9/tasmota-sml-images/total?logo=github&style=for-the-badge)](https://github.com/ottelo9/tasmota-sml-images/releases/latest)

## Fertige Tasmota Images/Firmware mit SML/Script Support
Die fertigen Images findet ihr im Release Bereich. Einfach das korrekte Image für euren ESP herunterladen, entpacken und entweder über OTA oder über einen Flasher+Kabel übertragen.
Eine ausführliche Anleitung dazu findet ihr auf meiner [Homepage](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/). Welche Tasmota Features aktiviert/deaktiviert sind findet ihr im Release.  
  
[Download Statistik](https://tooomm.github.io/github-release-stats/?username=ottelo9&repository=tasmota-sml-images)  
[ottelo.jimdo.de](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/)

### Welches Image (Firmware Binary) für welchen ESP?
| Imagename | Beschreibung |
| ------------- | ------------- |
| **ESP32 Images:**    | **tasmota32xx_ottelo.zip** |
| tasmota32_ottelo       | Generic ESP32, keine Variante, mit Ethernet Support |
| tasmota32berry_ottelo  | Generic ESP32, wie tasmota32_ottelo aber mit Berry Scripting Support |
| tasmota32x_ottelo      | ESP32 Variante z.B. c3, s3 (solo1, s3 mit Ethernet Support) |
| | |
| **ESP8266 Images:** | **tasmota8266_bundle_ottelo.zip** |
| tasmota1m_ottelo         | ESP mit 1M Flash (z.B. ESP01s = Hichi v1 Lesekopf |
| tasmota4m_ottelo         | ESP mit 4M+ Flash (auch für Steckdosen mit Energiemessung und mit Shelly, EcoTracker Emulation (+mDNS)) |
| tasmota1m_energy_ottelo  | ESP mit 1M Flash für Steckdosen mit Energiemessung z.B. Nous A1T, Sonoff Pow R2, Gosund EP2. Web-Upgrade nur über tasmota-minimal! |
| tasmota1m_shelly_ottelo  | ESP mit 1M Flash. Mit Shelly Pro 3EM / EcoTracker Emulation (+mDNS) als Meter für smarte Akkus (z.B. Marstek Venus / Jupiter). Der Scriptspeicher ist auf 4096 Zeichen begrenzt (statt 8192). Es funktioniert nur ein kleines Script ohne Charts, [Beispielscript mit Shelly Emu](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/scripting/shelly_emu_script.tas). HomeAssistant/MQTT aber weiterhin möglich. Web-Upgrade nur über tasmota-minimal! |
| tasmota-minimal          | Minimalimage, siehe Beschreibung unten oder tasmota_energy_ottelo |

Die Images sind alle gezippt. Im ZIP-Archiv befindet sich für den ESP32 immer auch das factory Image. Das wird beim erstmaligen Flashen auf einen leeren ESP32 benötigt bzw. wenn vorher noch kein Tasmota drauf war. Im ZIP-Archiv für den ESP8266 befinden sich alle Varianten gesammelt im bin.gz Format. Das .bin.gz muss immer für das Firmware-Upgrade via "Use file upload" über den Webbrowser verwendet werden, da das nicht komprimierte .bin Image sonst nicht übertragen werden kann (zu wenig Flash-Speicher). Falls es mal doch nicht passenden sollte (Fehlermeldung), dann muss vorher einmal das tasmota-minimal.bin.gz Image übertragen werden. Anschließend kann das .bin.gz Image übertragen werden. Zum erstmaligen Flashen via USB-Flasher muss die .gz entpackt werden.

### Shelly Pro 3EM / EcoTracker Emulation für smarte Akkus (wie z.B. Marstek Venus E oder Jupiter)
Ab V15.0.1 habe ich den Support für die Emulation des Shelly/EcoTracker inkludiert. Die Emulation ist in allen ESP32 Images inkludiert. Für den ESP8266 habe ich eine abgespeckte Firmware erstellt (tasmota1m_shelly), dort funktionieren nur die kleinen Basisscripte (_Simple.tas). Das alles haben wir dem Tasmota Script Entwickler [gemu2015](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/scripting/shelly_emu_script.tas) zu verdanken, der es zusammen mit Kalle entwickelt und getestet hat! Siehe auch [im Forum](https://forum.creationx.de/forum/index.php?thread/1095-d0-z%C3%A4hler-sml-auslesen-mit-tasmota/&pageNo=110). Gemu2015 hat ein Testscript zusammen mit Kalle erstellt. Ich habe die Teile aus dem Script entnommen, optimiert und eigene Scripte entworfen [siehe hier](https://github.com/ottelo9/tasmota-sml-script/tree/main/pvakku-powermeter-emulator). Eine [Anleitung](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/#13a) habe ich auf meinem Blog veröffentlicht.  

# Tasmota Image selbst erstellen - Anleitung für ESP32 / ESP8266
In der user_config_override.h findet ihr eine Liste mit Features/Treibern (#define bzw. #undef), die ich für meine ESP Tasmota Images/Firmware verwende und auf ottelo.jimdo.de zum Download anbiete. Die hier hochgeladenen Dateien können euch dabei helfen, ein eigenes angepasstes Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP ein Stromzähler über ein Lesekopf auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion (SonOff, Gosund, Shelly) habt und ihr die Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt. Das passende Script findet ihr in meinem anderen Repo https://github.com/ottelo9/tasmota-sml-script.  

### Wie verwenden?
Die Dateien in euer Tasmota Projektverzeichnis von Visual Studio Code oder Gitpod kopieren (ggf. überschreiben).
- TasmotaProjekt/`tasmota/user_config_override.h`
- TasmotaProjekt/`platformio_tasmota_cenv.ini`
- TasmotaProjekt/tasmota/tasmota_xdrv_driver/`xdrv_10_scripter.ino` <- (optional) die aktuellste Scripter Source aus der [gemu2015 Repo](https://github.com/gemu2015/Sonoff-Tasmota/blob/universal/tasmota/tasmota_xdrv_driver/xdrv_10_scripter.ino)

Eine ausführliche Anleitung zum Einrichten von Tasmota und weitere Details findet ihr auf meinem Blog:
[https://ottelo.jimdo.de](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/)

### Kompilieren
Zum Kompilieren unter Gitpod/VSC den passenden Befehl in die Console eingeben: 

Für die ESP32 Images muss man als erstes die Safeboot Images erstellen (die werden dann in die .factory Images eingefügt, wie ein Bootloader) z.B. `platformio run -e tasmota32-safeboot`.  

Um alle gleichzeitig zu erstellen:  
`platformio run -e tasmota32-safeboot -e tasmota32c3-safeboot -e tasmota32c6-safeboot -e tasmota32s2-safeboot -e tasmota32s3-safeboot -e tasmota32solo1-safeboot -e tasmota32p4-safeboot`

Nun können die einzelnen Images erstellt werden (landen dann im Ordner build_output):  

ESP32:  
`platformio run -e tasmota32_ottelo`      (Generic ESP32)  
`platformio run -e tasmota32berry_ottelo`      (Generic ESP32 + Berry)  
`platformio run -e tasmota32s2_ottelo`  
`platformio run -e tasmota32s3_ottelo`  
`platformio run -e tasmota32c3_ottelo`  
`platformio run -e tasmota32c6_ottelo`  
`platformio run -e tasmota32solo1_ottelo` (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)  

ESP8266:  
`platformio run -e tasmota1m_ottelo`        ( = 1M Flash)  
`platformio run -e tasmota1m_energy_ottelo` ( = 1M Flash, Update nur über minimal Image. Für SonOff POW (R2) / Gosund EP2 SonOff Dual R3 v2 / Nous A1T)  
`platformio run -e tasmota1m_shelly_ottelo` ( = 1M Flash, Update nur über minimal Image. Für Shelly/EcoTracker Emu Scripte für smarte Akkus wie z.B. Marstek (Venus, Jupiter, B2500) oder Hoymiles (MS-A2))  
`platformio run -e tasmota4m_ottelo`        (>= 4M Flash)  

Um alle gleichzeitig zu erstellen:  
`platformio run $(pio project config --json-output | jq -r '.[] | .[0] | select(test("ottelo")) | sub("env:";"-e ")')`


### (Factory)Image übertragen / flashen
[Tasmota Web Installer](https://tasmota.github.io/install/) (ESP32: nur Factory Images)  
Die ESP32 Non-Factory Images überträgt man via OTA (Firmware Upgrade -> Use file upload)  

Der ESP32-S2 hat einen integrierten USB-Controller und ist direkt mit der USB-Buchse verbunden. Den Treiber dafür könnt ihr hier herunterladen: https://zadig.akeo.ie/

### Passende SML Scripte
Die findet ihr [hier](https://github.com/ottelo9/tasmota-sml-script).  

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

------------------
Bedanken möchte ich mich besonders bei [gemu2015](https://github.com/gemu2015), der das Tasmota Scripting und SML entwickelt hat und mir immer sofort bei Problemen geholfen hat. 
Und natürlich beim restlichen [Tasmota Entwickler-Team](https://tasmota.github.io/docs/About/), für das grandiose Tasmota :).
