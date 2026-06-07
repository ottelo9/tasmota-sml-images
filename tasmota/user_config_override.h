/*
  user_config_override.h - user configuration overrides my_user_config.h for Tasmota

  Copyright (C) 2021  Theo Arends

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _USER_CONFIG_OVERRIDE_H_
#define _USER_CONFIG_OVERRIDE_H_

/*****************************************************************************************************\
 * USAGE:
 *   To modify the stock configuration without changing the my_user_config.h file:
 *   (1) copy this file to "user_config_override.h" (It will be ignored by Git)
 *   (2) define your own settings below
 *
 ******************************************************************************************************
 * ATTENTION:
 *   - Changes to SECTION1 PARAMETER defines will only override flash settings if you change define CFG_HOLDER.
 *   - Expect compiler warnings when no ifdef/undef/endif sequence is used.
 *   - You still need to update my_user_config.h for major define USE_MQTT_TLS.
 *   - All parameters can be persistent changed online using commands via MQTT, WebConsole or Serial.
\*****************************************************************************************************/

/*****************************************************************************************************\
 * Tasmota Image erstellen - Anleitung für ESP32 / ESP8266
 ******************************************************************************************************
 * Diese Features/Treiber (defines) habe ich für meine ESP32 / ESP8266 Tasmota Images/Firmware verwendet, die
 * ich auf ottelo.jimdo.de zum Download anbiete. Diese Datei kann euch auch dabei helfen ein eigenes
 * angepasstes Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP
 * ein Stromzähler mit einem Lesekopf auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion
 * (SonOff, Gosund, Shelly) habt und ihr Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt.
 * Andernfalls verwendet einfach die originalen Images. Diese Datei in euer Tasmota Verzeichnis
 * "tasmota\user_config_override.h" kopieren. Die andere Datei "platformio_tasmota_cenv.ini" mit meinen
 * Varianten kommt ins Hauptverzeichnis von Tasmota. Eine detaillierte Anleitung findet ihr auf github.
 *
 * Es gibt für jede Plattform (außer Berry und ESP8266-1M) zwei Varianten:
 *   _tas  → klassischer Tasmota-Scripter (Tasmota Scripting Language, Google Charts)
 *   _tc   → TinyC VM + Browser-IDE (https://github.com/gemu2015/Sonoff-Tasmota/tree/universal/tasmota/tinyc)
 *
 * Zum Kompilieren unter Gitpod den passenden Befehl in die Console eingeben:
 * ESP32 (jeweils _tas oder _tc):
 * platformio run -e tasmota32_ottelo_tas        (Generic ESP32, Scripter)
 * platformio run -e tasmota32_ottelo_tc         (Generic ESP32, TinyC)
 * platformio run -e tasmota32berry_ottelo_tas   (ESP32 mit Berry, nur Scripter)
 * platformio run -e tasmota32s2_ottelo_tas / _tc
 * platformio run -e tasmota32s3_ottelo_tas / _tc
 * platformio run -e tasmota32c3_ottelo_tas / _tc
 * platformio run -e tasmota32c6_ottelo_tas / _tc
 * platformio run -e tasmota32solo1_ottelo_tas / _tc   (ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)
 * platformio run -e tasmota32p4_ottelo_tas / _tc
 * Mehr Infos bzgl. ESP32 Versionen: https://tasmota.github.io/docs/ESP32/#esp32_1
 *
 * ESP8266:
 * platformio run -e tasmota1m_ottelo_tas        (1M Flash, nur Scripter)
 * platformio run -e tasmota1m_energy_ottelo_tas (1M, Update nur über minimal. SonOff POW (R2) / Gosund EP2 / SonOff Dual R3 v2 / Nous A1T)
 * platformio run -e tasmota1m_shelly_ottelo_tas (1M, Update nur über minimal. Mit Shelly Pro 3EM / EcoTracker Emulation für Marstek Venus / Jupiter)
 * platformio run -e tasmota4m_ottelo_tas / _tc  (>= 4M Flash)
 *
 * Alle Images bauen (Bash + jq):
 *   pio run $(pio project config --json-output | jq -r '.[] | .[0] | select(test("_ottelo_(tc|tas)$")) | sub("env:"; "-e ")')
 *
 * für weitere ESPs siehe: https://github.com/arendst/Tasmota/blob/development/platformio_override_sample.ini bei default_envs
\*****************************************************************************************************/

//siehe platformio_tasmota_cenv.ini
#if ( defined(TASMOTA32_OTTELO)       || defined(TASMOTA32C3_OTTELO)       || defined(TASMOTA32C6_OTTELO)      || defined(TASMOTA32S2_OTTELO) || defined(TASMOTA32S3_OTTELO) || \
      defined(TASMOTA32SOLO1_OTTELO)  || defined(TASMOTA32BERRY_OTTELO)   || defined(TASMOTA32P4_OTTELO)      || \
      defined(TASMOTA1M_OTTELO)       || defined(TASMOTA1M_SHELLY_OTTELO)  || defined(TASMOTA1M_ENERGY_OTTELO) || defined(TASMOTA4M_OTTELO) )

// (1) Folgende unnötige Features (siehe my_user_config.h) habe ich deaktiviert, um Tasmota schlank zu halten. Der ESP8266 z.B. hat wenig RAM,
//     dort müssen mindestens 12k RAM für einen stabilen Betrieb frei sein (inkl. Script).
#undef USE_DOMOTICZ       //https://tasmota.github.io/docs/Domoticz/ MQTT
#undef USE_EMULATION_HUE
#undef USE_EMULATION_WEMO
#undef ROTARY_V1
#undef USE_SONOFF_RF
#undef USE_SONOFF_SC
#undef USE_TUYA_MCU
#undef USE_ARMTRONIX_DIMMERS
#undef USE_PS_16_DZ
#undef USE_SONOFF_IFAN
#undef USE_BUZZER
#undef USE_ARILUX_RF
#undef USE_SHUTTER
#undef USE_EXS_DIMMER
#undef USE_DEVICE_GROUPS
#undef USE_PWM_DIMMER
#undef USE_SONOFF_D1
#undef USE_SHELLY_DIMMER
#undef SHELLY_CMDS
#undef SHELLY_FW_UPGRADE
#undef USE_LIGHT
#undef USE_WS2812
#undef USE_MY92X1
#undef USE_SM16716
#undef USE_SM2135
#undef USE_SM2335
#undef USE_BP1658CJ
#undef USE_BP5758D
#undef USE_SONOFF_L1
#undef USE_ELECTRIQ_MOODL
#undef USE_LIGHT_PALETTE
#undef USE_LIGHT_VIRTUAL_CT
#undef USE_DGR_LIGHT_SEQUENCE
#undef USE_SERIAL_BRIDGE  //https://tasmota.github.io/docs/Serial-to-TCP-Bridge/#serial-to-tcp-bridge
#undef USE_ENERGY_DUMMY

// diese Treiber/Features sind !nicht! im ESP8266-1M,-4M und -1M-Shelly Image inkludiert, sondern nur 1M-Energy oder ESP32
#if ( defined(TASMOTA1M_OTTELO) || defined(TASMOTA1M_SHELLY_OTTELO) || defined(TASMOTA4M_OTTELO))
  #undef USE_I2C            // I2C ist für die nachfolgenden Treiber erforderlich.
  #undef USE_ENERGY_SENSOR  // Ist für die nachfolgenden Treiber erforderlich.
  #undef USE_HLW8012        // SonOff POW / Gosund EP2 (ESP8266)
  #undef USE_CSE7766        // SonOff POW R2 (ESP8266)
  #undef USE_BL09XX         // SonOff Dual R3 v2 (ESP8266) / Shelly Plus Plug S (ESP32) / Gosund EP2 (ESP8266)
  #undef USE_DHT            // DHT11, AM2301 (DHT21, DHT22, AM2302, AM2321) and SI7021 Temperature and Humidity sensor (1k6 code)
  #undef USE_DS18x20        // Temperature Sensoren z.B. DS18B20
  #undef USE_DEEPSLEEP      // +1K https://tasmota.github.io/docs/DeepSleep/
#else
  #define USE_BMP           // +4K BMP085 BMP180 BMP280 BME280 BME680 (standardmäßig nicht enthalten)!
#endif

#undef USE_PZEM004T
#undef USE_PZEM_AC
#undef USE_PZEM_DC
#undef USE_MCP39F501
#undef USE_IR_REMOTE
#undef GV_USE_ESPINFO //ESP8266 info (+2k1 code) ab 15.2.0
//ESP32 only features
#undef USE_GPIO_VIEWER
#if ( !defined(TASMOTA32_OTTELO) && !defined(TASMOTA32BERRY_OTTELO) )
  #undef USE_ADC
#endif
#undef USE_NETWORK_LIGHT_SCHEMES
#if ( !defined(TASMOTA32BERRY_OTTELO) )
  #undef USE_BERRY          //https://tasmota.github.io/docs/Berry/
#endif
#undef USE_AUTOCONF       //https://tasmota.github.io/docs/ESP32/#autoconf
#undef USE_CSE7761
//----------------------------------------------------------------------------

// (2) Welche ESP32 / ESP8266 Features aktiv sind, wenn mit "build_flags = -DFIRMWARE_TASMOTA32" bzw
// "build_flags = -DFIRMWARE_TASMOTA32"kompiliert wird
//     findet ihr in der "tasmota_configurations_ESP32.h" unter #ifdef FIRMWARE_TASMOTA32

//----------------------------------------------------------------------------

// (3) Aktivierte zusätzliche Features (SML, Scripter ODER TinyC, TCP, Ethernet, ...)

//-- Variant-Selektoren (gesetzt via build_flags in platformio_tasmota_cenv.ini):
//--   -DOTTELO_VARIANT_TAS → klassischer Tasmota-Scripter
//--   -DOTTELO_VARIANT_TC  → TinyC VM + Browser-IDE (KEIN Scripter)
//-- ESP8266 1M (alle drei Varianten) und Berry haben nur _tas.
#if !defined(OTTELO_VARIANT_TAS) && !defined(OTTELO_VARIANT_TC)
  #error "OTTELO_VARIANT_TAS oder OTTELO_VARIANT_TC muss in build_flags gesetzt sein"
#endif
#if defined(OTTELO_VARIANT_TAS) && defined(OTTELO_VARIANT_TC)
  #error "OTTELO_VARIANT_TAS und OTTELO_VARIANT_TC schliessen sich gegenseitig aus"
#endif

//-- Stack size erhöhen (Empfehlung: seit Core3 wird mehr benötigt)
#undef SET_ESP32_STACK_SIZE
#define SET_ESP32_STACK_SIZE (12 * 1024)

//-- Filesystem (für SML-Meter-Files, Scripte, TinyC-Bytecode, TinyC-IDE)
//-- ESP8266 1M hat kein UFILESYS — dort nutzt der Scripter EEPROM (siehe TAS-Block).
#if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) )
  #define USE_PING // Enable Ping command (+2k code)
  #define USE_UFILESYS
  #undef UFSYS_SIZE
  #if defined(TASMOTA4M_OTTELO)
    #define UFSYS_SIZE 8192   //ESP8266 4M+
  #else
    #define UFSYS_SIZE 16384  //ESP32
  #endif
#endif

//-- SML (in beiden Varianten — beim _tc liegt der Meter-Descriptor unter /sml_meter.def)
#define USE_SML_M
#define USE_SML_CRC       //enables CRC support for binary SML. Must still be enabled via line like "1,=soC,1024,15". https://tasmota.github.io/docs/Smart-Meter-Interface/#special-commands
#define USE_SML_AUTHKEY   //selten genutzt, M,=so5

//-- Home Assistant + HTTPS-Client (gemeinsam)
#define USE_HOME_ASSISTANT  //HA API (+12k code, +6 bytes mem)
#define USE_WEBCLIENT_HTTPS //für HA benötigt
#define USE_TLS

//-- Software Serial / MQTT-TLS / InfluxDB — nur ESP32 (gemeinsam _tas + _tc)
#if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA4M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) )
  #define USE_ESP32_SW_SERIAL //Pin mit '-' in der SML/TinyC Konfiguration definieren
  #define USE_MQTT_TLS        //3KB
  #define USE_INFLUXDB        //6KB
#endif

//-- Ethernet (WT32-ETH01) - gemeinsam
#if ( defined(TASMOTA32_OTTELO) || defined(TASMOTA32SOLO1_OTTELO) || defined(TASMOTA32S3_OTTELO) || defined(TASMOTA32P4_OTTELO) )
  #define USE_ETHERNET          //Add support for ethernet (+20k code)
  #define USE_WT32_ETH01
  #define ETH_TYPE          0
  #define ETH_ADDRESS       0
  #define ETH_CLKMODE       3
#endif


//=============================================================================
// SCRIPTER-Variante (klassische Tasmota Scripting Language + Google Charts)
//=============================================================================
#ifdef OTTELO_VARIANT_TAS

  //-- Max String Size: default 255. Wird nun aber im Script mit >D xx definiert!
  //#define SCRIPT_MAXSSIZE 128

  //-- 4096 statt 256 bytes für Variablennamen und größere Arrays (24h Diagramm) — nur ESP32
  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) && !defined(TASMOTA4M_OTTELO) )
    #define SCRIPT_LARGE_VNBUFF
    #define MAX_ARRAY_SIZE 2000
  #endif

  //-- Skriptgröße — ESP8266 1M nutzt EEPROM, größere Targets Filesystem
  #if ( defined(TASMOTA1M_OTTELO) || defined(TASMOTA1M_ENERGY_OTTELO) || defined(TASMOTA1M_SHELLY_OTTELO) )
    #define USE_EEPROM
    #undef EEP_SCRIPT_SIZE
    #if ( defined(TASMOTA1M_SHELLY_OTTELO) )
      #define EEP_SCRIPT_SIZE 4096
    #else
      #define EEP_SCRIPT_SIZE 8192
    #endif
  #else
    #define USE_SCRIPT_FATFS_EXT //https://tasmota.github.io/docs/Scripting-Language/#extended-commands-09k-flash
  #endif

  //-- Scripter + Charts + Web-Display
  #define USE_SCRIPT             //(+36k code, +1k mem)
  #undef USE_RULES               //USE_SCRIPT & USE_RULES can't both be used at the same time
  #define USE_GOOGLE_CHARTS
  #define LARGE_ARRAYS
  #define USE_SCRIPT_WEB_DISPLAY
  #define USE_CW_CALC            //Kalenderwochen via Variable cw
  #define USE_HTML_CALLBACK      //für Smartmeter Descriptor dropdown list smlpd()

  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) )
    #define USE_ANGLE_FUNC       //~2KB
    #define USE_FEXTRACT         //~8KB cts()
  #endif

  //-- Serial / TCP-Server / Task — nur ESP32
  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA4M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) )
    #define USE_SCRIPT_SERIAL    //3KB
    #define SCRIPT_FULL_WEBPAGE  //1KB
    #define USE_SCRIPT_TCP_SERVER
    #define USE_SCRIPT_TASK
  #endif

  //-- shellypro3em emulieren (z.B. für Marstek Venus E) — braucht mDNS
  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) )
    #define USE_SCRIPT_MDNS      //14KB
  #endif

  //-- globale Variablen + >J Sektion
  #define USE_SCRIPT_GLOBVARS    //2KB
  #define USE_SCRIPT_JSON_EXPORT //0KB

  //-- Scriptliste-Menü
  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) && !defined(TASMOTA4M_OTTELO) )
    #define SCRIPT_LIST_DOWNLOAD_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-script/main/script-list-menu/scripts/"
    #define SCRIPT_LIST "scripts.json"
  #endif

#endif // OTTELO_VARIANT_TAS


//=============================================================================
// TINYC-Variante (TinyC VM + Browser-IDE, KEIN Scripter)
//=============================================================================
#ifdef OTTELO_VARIANT_TC

  // Matter-Bridge — https://gemu2015.github.io/Sonoff-Tasmota/reference/?h=#matter-esp32-requires-use_matter_c
  // Braucht zwingend USE_DISCOVERY (mDNS) für Service-Advertising, sonst
  // gibt's Linker-Error "undefined reference to StartMdns()" beim Matter-Init.
  // Tasmota's my_user_config.h hat USE_DISCOVERY per Default auskommentiert.
  #if ( !defined(TASMOTA1M_OTTELO) && !defined(TASMOTA1M_ENERGY_OTTELO) && !defined(TASMOTA1M_SHELLY_OTTELO) && !defined(TASMOTA4M_OTTELO) )
    #define USE_MATTER_C
    #define USE_DISCOVERY        //+8KB Flash, +0.3KB RAM — mDNS, von Matter benötigt
    #define WEBSERVER_ADVERTISE  //<Hostname>.local/ — Standard zusammen mit mDNS
  #endif

  //-- TinyC braucht USE_RULES nicht — der SML-Init-Gate (`Settings->rule_enabled`
  //-- Bit 0) wird vom TinyC-Programm direkt über die Built-in-Variable `tasm_rule`
  //-- gesetzt: einfach `tasm_rule = 1;` schaltet SML frei.
  //-- Beispiel: tasmota/tinyc/examples/marstek_emu.tc
  //-- Doku: https://gemu2015.github.io/Sonoff-Tasmota/reference/?h=use_sml_m#smart-meter-sml
  //-- Meter-Descriptor liegt unter /sml_meter.def im Filesystem.
  #undef USE_RULES

  //-- SML_SetBaud / SML_Write in xsns_53_sml.ino sind hinter USE_SML_SCRIPT_CMD
  //-- gegated. Normalerweise setzt xdrv_10_scripter.ino dieses Macro — ohne
  //-- Scripter müssen wir es selbst definieren, sonst gibt's beim TinyC-VM-
  //-- Aufruf (mscr SML_BAUD / SML_HEX Opcodes) einen "not declared in this scope"
  //-- Fehler. Die Funktionen selbst haben keine Scripter-Abhängigkeit.
  #define USE_SML_SCRIPT_CMD

  //-- TinyC VM + selbstgehostete Browser-IDE
  //-- https://github.com/gemu2015/Sonoff-Tasmota/tree/universal/tasmota/tinyc
  #define USE_TINYC          //Enable TinyC VM (XDRV_124)
  #define USE_TINYC_IDE      //Enable self-hosted browser IDE (requires USE_UFILESYS)

  //-- BinPlugin-Loader: ermöglicht das Nachladen relocatabler Plugin-.bin
  //-- (Audio, Sensoren, …) zur Laufzeit in die custom-Partition — ohne die
  //-- Firmware neu zu flashen. Nicht automatisch an (siehe Plugins/readme.md).
  //-- Die Plugins selbst werden separat mit tasmota/Plugins/build_plugin.py
  //-- gebaut und sind erst nach Laden+Aktivieren via Konsole aktiv.
  #define USE_BINPLUGINS

#endif // OTTELO_VARIANT_TC


//-- Weiteres optionales
//#define USE_DISPLAY_TM1621_SONOFF //4KB
//#define USE_SENDMAIL


//=============================================================================
// OTA-URLs (pro Plattform und Variante)
//=============================================================================
#undef OTA_URL

#ifdef OTTELO_VARIANT_TAS
  #define _OTTELO_SFX "_tas"
#else
  #define _OTTELO_SFX "_tc"
#endif

#if defined(TASMOTA32_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32C3_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32c3_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32C6_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32c6_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32S2_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32s2_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32S3_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32s3_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32SOLO1_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32solo1_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA32BERRY_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32berry_ottelo_tas.bin"
#elif defined(TASMOTA32P4_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP32/tasmota32p4_ottelo" _OTTELO_SFX ".bin"
#elif defined(TASMOTA1M_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP8266/tasmota1m_ottelo_tas.bin.gz"
#elif defined(TASMOTA1M_SHELLY_OTTELO)
  #define OTA_URL "Upgrade nur via minimal.bin moeglich!"
#elif defined(TASMOTA1M_ENERGY_OTTELO)
  #define OTA_URL "Upgrade nur via minimal.bin moeglich!"
#elif defined(TASMOTA4M_OTTELO)
  #define OTA_URL "https://raw.githubusercontent.com/ottelo9/tasmota-sml-images/main/ota_firmware/ESP8266/tasmota4m_ottelo" _OTTELO_SFX ".bin.gz"
#endif

#endif // TASMOTA OTTELO


#endif  // _USER_CONFIG_OVERRIDE_H_
