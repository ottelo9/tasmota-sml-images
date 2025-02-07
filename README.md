# Tasmota Image erstellen - Anleitung für ESP32 (ESP8266 folgt)
In der user_config_override.h findet ihr eine Liste mit Features/Treibern (#define bzw. #undef), die ich für meine ESP32 Tasmota Images/Firmware verwende und auf ottelo.jimdo.de zum Download anbiete. Die hier hochgeladenen Dateien können euch dabei helfen, ein eigenes angepasstes Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP ein Stromzähler auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion habt und ihr die schönen Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt. Andernfalls verwendet einfach die originalen Images. Das passende Script findet ihr in meinem anderen Repo https://github.com/ottelo9/tasmota-sml-script.

### Wie verwenden?
Die Dateien in euer Tasmota Projektverzeichnis von Visual Studio Code oder Gitpod kopieren (ggf. überschreiben).
- TasmotaProjekt/tasmota/user_config_override.h
- TasmotaProjekt/platformio_tasmota_cenv.ini

### Kompilieren
unter Gitpod den passenden Befehl in die Console eingeben:

`platformio run -e tasmota32_ottelo`          (Generic ESP32)  
`platformio run -e tasmota32s2_ottelo`  
`platformio run -e tasmota32s3_ottelo`  
`platformio run -e tasmota32c3_ottelo`  
`platformio run -e tasmota32c6_ottelo`  
`platformio run -e tasmota32solo1_ottelo` (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)  


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
