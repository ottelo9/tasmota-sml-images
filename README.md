# Tasmota Image erstellen - Anleitung für ESP32 (ESP8266 folgt)
In der user_config_override.h findet ihr eine Liste mit Features/Treibern (#define bzw. #undef), die ich für meine ESP32 Tasmota Images/Firmware verwende und auf ottelo.jimdo.de zum Download anbiete. Die hier hochgeladenen Dateien können euch dabei helfen, ein eigenes angepasstes Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP ein Stromzähler auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion habt und ihr die schönen Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt. Andernfalls verwendet einfach die originalen Images. Das passende Script findet ihr in meinem anderen Repo https://github.com/ottelo9/tasmota-sml-script.

### Wie verwenden?
Die Dateien in euer Tasmota Projektverzeichnis von Visual Studio Code oder Gitpod kopieren (ggf. überschreiben).
- TasmotaProjekt/`tasmota/user_config_override.h`
- TasmotaProjekt/`platformio_tasmota_cenv.ini`
- TasmotaProjekt/`platformio_tasmota32.ini`  <- habe ich nur als Referenz mit hochgeladen. Bei [core32] platform sollte immer das neuste framework verwendet werden!

Eine ausführliche Anleitung und weitere Details findet ihr auf meinem Blog:
[https://ottelo.jimdo.de](https://ottelo.jimdofree.com/stromz%C3%A4hler-auslesen-tasmota/)

### Kompilieren
unter Gitpod den passenden Befehl in die Console eingeben:

`platformio run -e tasmota32_ottelo`          (Generic ESP32)  
`platformio run -e tasmota32s2_ottelo`  
`platformio run -e tasmota32s3_ottelo`  
`platformio run -e tasmota32c3_ottelo`  
`platformio run -e tasmota32c6_ottelo`  
`platformio run -e tasmota32solo1_ottelo` (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)  

### (Factory)Image übertragen / flashen
[Tasmota Web Installer](https://tasmota.github.io/install/) (nur für die factory Images)  
Die Non-Factory Images überträgt ihr via OTA (Firmware Upgrade -> Use file upload)  

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

Wenn beim Kompilieren eine Standard Tasmota Variante verwendet wird (z.B. `-e tasmota32c3`), dann werden Features/Treiber für diese Konfiguration (siehe https://github.com/arendst/Tasmota/blob/development/platformio_tasmota_env32.ini z.B. [env:tasmota32c3]) verwendet und die deaktivierten Features, die ihr in der `user_config_override.h` eingetragen habt, überschrieben und somit doch verwendet! Wenn Features/Treiber (de)aktivieren werden sollen, dann eine eigene Variante in `platformio_tasmota_cenv.ini` erstellen und `-DFIRMWARE_TASMOTA32` entfernen, da wie bereits oben erwähnt, ESP32 Standard Features wie Berry usw verwendet werden (siehe `FIRMWARE_TASMOTA32` in `tasmota_configuration_ESP32.h`). Siehe auch https://tasmota.github.io/docs/Compile-your-build/#customize-your-build  

Noch eine Info:  
Immer die neuste Tasmota Platform Framework builds verwenden. D.h. in der platformio_tasmota32.ini bei [core32] platform url aktualisieren  

[Offizielle Tasmota Github Seite](https://github.com/arendst/Tasmota)  

### Debugging
Falls Tasmota mal ab und zu aus unbekannten Gründen neustarten (Reboots) sollte, gibt es mehrere Möglichkeiten die Ursache des Problems festzustellen.

ESP via USB mit dem PC verbinden und ein Terminalprogramm (putty oder MobaXterm) laufen (loggen) lassen bis der Reboot passiert. Tasmota gibt dann beim Crash/Reboot ein Crash Dump aus. Das könnte z.B. so aussehen:  

`Guru Meditation Error: Core  0 panic'ed (Stack protection fault).`
`Detected in task "loopTask" at 0x42023fee`

Hat man das Image selbst kompiliert, so kann man in der .map Datei nach der Adresse 0x42023fee bzw. 0x42023* suchen. Die map Datei liegt unter `\.pio\build\tasmota32xxx\firmware.map`. Dort findet man dann die Funktion, in der das Problem (in diesem Fall Buffer Overflow) aufgetaucht ist. Die genaue Stelle kann man nur herausfinden, wenn man die firmware.asm Datei hat. Die wird erst erstellt, wenn man folgendes mit in die [platformio_tasmota_cenv.ini](platformio_tasmota_cenv.ini) einfügt:  
`extra_scripts           = ${env:tasmota32_base.extra_scripts}`  
`                          post:pio-tools/obj-dump.py`  
  
Die extrem große Datei liegt unter `\.pio\build\tasmota32xxx\firmware.asm`. Dort kann man dann exakt nach der Adresse suchen.  
Für den C6 muss man aber noch die `obj-dump.py` anpassen, da der C6 dort fehlt. Ich habe einfach die Zeile vom C3 kopiert.

------------------
Bedanken möchte ich mich besonders bei [gemu2015](https://github.com/gemu2015), der das Tasmota Scripting und SML entwickelt hat und mir immer sofort bei Problemen geholfen hat. Und natürlich beim restlichen [Tasmota Entwickler-Team](https://tasmota.github.io/docs/About/), für das grandiose Tasmota :).
