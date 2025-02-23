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
 * Zum Kompilieren unter Gitpod den passenden Befehl in die Console eingeben:
 * ESP32:
 * platformio run -e tasmota32_ottelo      (Generic ESP32)
 * platformio run -e tasmota32s2_ottelo
 * platformio run -e tasmota32s3_ottelo
 * platformio run -e tasmota32c3_ottelo
 * platformio run -e tasmota32c6_ottelo
 * platformio run -e tasmota32solo1_ottelo (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)
 * Mehr Infos bzgl. ESP32 Versionen: https://tasmota.github.io/docs/ESP32/#esp32_1
 * 
 * ESP8266:
 * platformio run -e tasmota_ottelo        ( = 1M Flash)
 * platformio run -e tasmota_energy_ottelo ( = 1M Flash, Update nur über minimal da Img zu groß. für SonOff POW (R2) / Gosund EP2 SonOff Dual R3 v2)
 * platformio run -e tasmota4m_ottelo      (>= 4M Flash)
 * 
 * für weitere ESPs siehe: https://github.com/arendst/Tasmota/blob/development/platformio_override_sample.ini bei default_envs
\*****************************************************************************************************/

//siehe platformio_tasmota_cenv.ini
#if ( defined(TASMOTA32_OTTELO) || defined(TASMOTA32C3_OTTELO)    || defined(TASMOTA32C6_OTTELO) || defined(TASMOTA32S2_OTTELO) || defined(TASMOTA32S3_OTTELO) || defined(TASMOTA32SOLO1_OTTELO) || \
      defined(TASMOTA_OTTELO)   || defined(TASMOTA_ENERGY_OTTELO) || defined(TASMOTA4M_OTTELO) )

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
//#undef USE_DEEPSLEEP
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
#undef USE_DS18x20
#undef USE_SERIAL_BRIDGE  //https://tasmota.github.io/docs/Serial-to-TCP-Bridge/#serial-to-tcp-bridge
#undef USE_ENERGY_DUMMY

#if defined(TASMOTA_OTTELO)
  #undef USE_I2C            // I2C ist für die nachfolgenden Treiber erforderlich.
  #undef USE_ENERGY_SENSOR  // Ist für die nachfolgenden Treiber erforderlich.
  #undef USE_HLW8012        // SonOff POW / Gosund EP2 (ESP8266)
  #undef USE_CSE7766        // SonOff POW R2 (ESP8266)
  #undef USE_BL09XX         // SonOff Dual R3 v2 (ESP8266) / Shelly Plus Plug S (ESP32) / Gosund EP2 (ESP8266)
#endif

#undef USE_PZEM004T
#undef USE_PZEM_AC
#undef USE_PZEM_DC
#undef USE_MCP39F501
#undef USE_DHT
#undef USE_IR_REMOTE
//ESP32 only features
#undef USE_GPIO_VIEWER
#undef USE_ADC
#undef USE_NETWORK_LIGHT_SCHEMES
#undef USE_BERRY          //https://tasmota.github.io/docs/Berry/
#undef USE_AUTOCONF       //https://tasmota.github.io/docs/ESP32/#autoconf
#undef USE_CSE7761

//----------------------------------------------------------------------------

// (2) Welche ESP32 / ESP8266 Features aktiv sind, wenn mit "build_flags = -DFIRMWARE_TASMOTA32" bzw
// "build_flags = -DFIRMWARE_TASMOTA32"kompiliert wird
//     findet ihr in der "tasmota_configurations_ESP32.h" unter #ifdef FIRMWARE_TASMOTA32

//----------------------------------------------------------------------------

// (3) Aktivierte zusätzliche Features (SML, Scripting, TCP, Ethernet, ...)

//-- Stack size erhöhen (Empfehlung: seit Core3 wird mehr benötigt)
#undef SET_ESP32_STACK_SIZE
#define SET_ESP32_STACK_SIZE (12 * 1024)

//-- Optional: Für mein SML Simulator Script (max String Länge = 128). Im Script >D 128
#undef SCRIPT_MAXSSIZE
#define SCRIPT_MAXSSIZE 128

//-- enables to use 4096 in stead of 256 bytes buffer for variable names
#define SCRIPT_LARGE_VNBUFF


//-- Skriptgröße (max Anzahl an Zeichen) https://tasmota.github.io/docs/Scripting-Language/#script-buffer-size
//-- ESP8266 1M Flash
#if ( defined(TASMOTA_OTTELO) || defined(TASMOTA_ENERGY_OTTELO) )
  #define USE_EEPROM
  #undef EEP_SCRIPT_SIZE
  #define EEP_SCRIPT_SIZE 8192
#else
//-- ESP8266 4M+ Flash / ESP32
  #define USE_SCRIPT_FATFS_EXT //https://tasmota.github.io/docs/Scripting-Language/#extended-commands-09k-flash
  #define USE_UFILESYS
  #undef UFSYS_SIZE
  #if defined(TASMOTA4M_OTTELO)
    #define UFSYS_SIZE 8192  //ESP8266
  #else
    #define UFSYS_SIZE 16384 //ESP32
  #endif
#endif

//-- benötigt USE_HOME_ASSISTANT
#define USE_WEBCLIENT_HTTPS

//-- SML, Script und Google Chart Support
#define USE_SCRIPT        //(+36k code, +1k mem)
#define USE_SML_M
#undef USE_RULES          //USE_SCRIPT & USE_RULES can't both be used at the same time
#define USE_GOOGLE_CHARTS
#define LARGE_ARRAYS
#define USE_SCRIPT_WEB_DISPLAY

//-- enables authentication, this is not needed by most energy meters. M,=so5
#define USE_SML_AUTHKEY
#define USE_TLS

//-- Verwende Home Assistant API
#define USE_HOME_ASSISTANT  //(+12k code, +6 bytes mem)

//-- Software Serial für ESP32 (nur RX), Pin mit dem Zeichen '-' in der SML Sektion definieren (bei mehr als 2/3-Leseköpfen, je nach ESP32 Variante)
#if ( !defined(TASMOTA_OTTELO) && !defined(TASMOTA4M_OTTELO) && !defined(TASMOTA_ENERGY_OTTELO) )
  #define USE_ESP32_SW_SERIAL
#endif

//-- Optional: Serielle Schnittstelle (RX/TX RS232) im Script verwenden
#define USE_SCRIPT_SERIAL

//-- Optional: ESP32 WT32_ETH01 (Ethernet LAN Modul)
#if ( defined(TASMOTA32_OTTELO) || defined(TASMOTA32SOLO1_OTTELO) )
  #define USE_ETHERNET          // Add support for ethernet (+20k code)
  #define USE_WT32_ETH01
  #define ETH_TYPE          0
  #define ETH_ADDRESS       0
  #define ETH_CLKMODE       3
#endif

//-- Optional: TCP-Server Script Support
#if ( !defined(TASMOTA_OTTELO) && !defined(TASMOTA4M_OTTELO) && !defined(TASMOTA_ENERGY_OTTELO) )
  #define USE_SCRIPT_TCP_SERVER
  #define USE_SCRIPT_TASK
#endif

//-- Optional: Optionale SML Features deaktivieren
//#define NO_USE_SML_SPECOPT
//#define NO_USE_SML_SCRIPT_CMD
//#define NO_SML_REPLACE_VARS
//#define NO_USE_SML_DECRYPT
//#define NO_USE_SML_TCP
//#define NO_USE_SML_CANBUS

//-- Optional: Verwende globale Variablen im Script
#define USE_SCRIPT_GLOBVARS

#endif // TASMOTA32 OTTELO


#endif  // _USER_CONFIG_OVERRIDE_H_