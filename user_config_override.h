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
 * Tasmota Image erstellen - Anleitung für ESP32
 ******************************************************************************************************
 * Diese Features/Treiber (defines) habe ich für meine ESP32 Tasmota Images/Firmware verwendet, die ich
 * auf ottelo.jimdo.de zum Download anbiete. Diese Datei kann euch auch dabei helfen ein eigenes angepasstes 
 * Tasmota Image für euren ESP mit Gitpod (oder Visual Studio) zu erstellen, wenn ihr mit dem ESP 
 * ein Stromzähler auslesen wollt (SML) oder eine smarte Steckdose mit Energiemessfunktion habt und ihr 
 * die schönen Liniendiagramme (Google Chart Script) für den Verbrauch haben wollt. Andernfalls verwendet 
 * einfach die originalen Images. Diese Datei in euer Tasmota Verzeichnis "tasmota\user_config_override.h"
 * kopieren. Die andere Datei "platformio_tasmota_cenv.ini" mit meinen Varianten kommt ins Hauptverzeichnis
 * von Tasmota.
 *
 * Zum Kompilieren unter Gitpod den passenden Befehl in die Console eingeben:
 * platformio run -e tasmota32_ottelo          (Generic ESP32)
 * platformio run -e tasmota32s2_ottelo
 * platformio run -e tasmota32s3_ottelo
 * platformio run -e tasmota32c3_ottelo
 * platformio run -e tasmota32c6_ottelo
 * platformio run -e tasmota32solo1_ottelo (für ESP32-S1 Single Core z.B. WT32-ETH01 v1.1)
 * Mehr Infos bzgl. ESP32 Versionen: https://tasmota.github.io/docs/ESP32/#esp32_1
 * für weitere ESPs siehe: https://github.com/arendst/Tasmota/blob/development/platformio_override_sample.ini bei default_envs
 * 
 ******************************************************************************************************
 * Features/Treiber (de)aktivieren: https://tasmota.github.io/docs/Compile-your-build/#enabling-a-feature-in-tasmota
 * Hier ist eine Übersicht aller Features/Treiber: https://github.com/arendst/Tasmota/blob/development/tasmota/my_user_config.h
 * Wenn beim Kompilieren eine Standard Tasmota Variante verwendet wird (z.B. -e tasmota32c3), dann werden Features/Treiber für diese Konfiguration
 * (siehe https://github.com/arendst/Tasmota/blob/development/platformio_tasmota_env32.ini z.B. [env:tasmota32c3])
 * verwendet und die deaktivierten Features, die ihr in der user_config_override.h eingetragen habt, überschrieben und somit doch verwendet!
 * Wenn Features/Treiber (de)aktivieren werden sollen, dann eine eigene Variante in platformio_tasmota_cenv.ini erstellen und -DFIRMWARE_TASMOTA32
 * entfernen, da wie bereits oben erwähnt, ESP32 Standard Features wie Berry usw verwendet werden (siehe FIRMWARE_TASMOTA32 in tasmota_configuration_ESP32.h)
 * Siehe auch https://tasmota.github.io/docs/Compile-your-build/#customize-your-build
 * Noch eine Info: Immer die neuste Tasmota Platform Framework builds verwenden. D.h. in der platformio_tasmota32.ini bei [core32] platform url aktualisieren
\*****************************************************************************************************/

//siehe platformio_tasmota_cenv.ini
#if ( defined(TASMOTA32_OTTELO) || defined(TASMOTA32C3_OTTELO) || defined(TASMOTA32C6_OTTELO) || defined(TASMOTA32_ETH_OTTELO) )

// Folgende Features (siehe my_user_config.h) habe ich für das Image deaktiviert, um die Reboot-Probleme beim C3/C6 zu beheben und es schlank zu halten.
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
#undef USE_I2C
#undef USE_SERIAL_BRIDGE  //https://tasmota.github.io/docs/Serial-to-TCP-Bridge/#serial-to-tcp-bridge
#ifndef TASMOTA_ENERGY_OTTELO
  #undef USE_ENERGY_SENSOR  // SonOff / Gosund EP2 verwenden wollt (ESP8266)
  #undef USE_ENERGY_DUMMY
  #undef USE_HLW8012        // SonOff POW / Gosund EP2 verwenden wollt (ESP8266)
  #undef USE_CSE7766        // SonOff POW R2 verwendet (ESP8266)
  #undef USE_BL09XX         // SonOff Dual R3 v2 / Gosund EP2 verwendet (ESP8266)
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

// ESP32 Features aus "tasmota_configuration_ESP32.h" wenn build_flags = -DFIRMWARE_TASMOTA32
/*#define USE_MATTER_DEVICE
#define USE_INFLUXDB                             // Enable influxdb support (+5k code)
#define USE_ENHANCED_GUI_WIFI_SCAN
#define USE_SDCARD
#define USE_BUZZER                               // Add support for a buzzer (+0k6 code)
#define USE_DEEPSLEEP                            // Add support for deepsleep (+1k code)
#define USE_LIGHT_PALETTE                        // Add support for color palette (+0k9 code)
#define USE_LIGHT_ARTNET                         // Add support for DMX/ArtNet via UDP on port 6454 (+3.5k code)
#ifdef CONFIG_IDF_TARGET_ESP32C3
  #define USE_MAGIC_SWITCH                         // Add Sonoff MagicSwitch support as implemented in Sonoff Basic R4
#endif
#define USE_DS18x20                              // Add support for DS18x20 sensors with id sort, single scan and read retry (+1k3 code)
// TEST 1 - Reboot  12min
#define USE_I2C                                  // I2C using library wire (+10k code, 0k2 mem, 124 iram)
#define USE_SHT                                // [I2cDriver8] Enable SHT1X sensor (+1k4 code)
#define USE_HTU                                // [I2cDriver9] Enable HTU21/SI7013/SI7020/SI7021 sensor (I2C address 0x40) (+1k5 code)
#define USE_BMP                                // [I2cDriver10] Enable BMP085/BMP180/BMP280/BME280 sensors (I2C addresses 0x76 and 0x77) (+4k4 code)
#define USE_BME68X                           // Enable support for BME680/BME688 sensor using Bosch BME68x library (+6k9 code)
#define USE_BH1750                             // [I2cDriver11] Enable BH1750 sensor (I2C address 0x23 or 0x5C) (+0k5 code)
#define USE_VEML6070                           // [I2cDriver12] Enable VEML6070 sensor (I2C addresses 0x38 and 0x39) (+1k5 code)
#define USE_ADS1115                            // [I2cDriver13] Enable ADS1115 16 bit A/D converter (I2C address 0x48, 0x49, 0x4A or 0x4B) based on Adafruit ADS1x15 library (no library needed) (+0k7 code)
#define USE_INA219                             // [I2cDriver14] Enable INA219 (I2C address 0x40, 0x41 0x44 or 0x45) Low voltage and current sensor (+1k code)
#define USE_SHT3X                              // [I2cDriver15] Enable SHT3x (I2C address 0x44 or 0x45) or SHTC3 (I2C address 0x70) sensor (+0k7 code)
#define USE_MGS                                // [I2cDriver17] Enable Xadow and Grove Mutichannel Gas sensor using library Multichannel_Gas_Sensor (+10k code)
#define USE_SGP30                              // [I2cDriver18] Enable SGP30 sensor (I2C address 0x58) (+1k1 code)
#define USE_SGP40                              // [I2cDriver69] Enable SGP40 sensor (I2C address 0x59) (+1k4 code)
#define USE_SGP4X                              // [I2cDriver82] Enable SGP41 sensor (I2C address 0x59) (+7k2 code)
#define USE_SEN5X                              // [I2cDriver76] Enable SEN5X sensor (I2C address 0x69) (+3k code)
#define USE_LM75AD                             // [I2cDriver20] Enable LM75AD sensor (I2C addresses 0x48 - 0x4F) (+0k5 code)
#define USE_MCP23XXX_DRV                       // [I2cDriver77] Enable MCP23xxx support as virtual switch/button/relay (+3k(I2C)/+5k(SPI) code)
#define USE_CCS811_V2                          // [I2cDriver24] Enable CCS811 sensor (I2C addresses 0x5A and 0x5B) (+2k8 code)
#define USE_MPU_ACCEL                          // [I2cDriver58] Enable MPU6886, MPU9250 6-axis MotionTracking sensor (I2C address 0x68)
#define USE_SCD30                              // [I2cDriver29] Enable Sensiron SCd30 CO2 sensor (I2C address 0x61) (+3k3 code)
#define USE_SCD40                              // [I2cDriver62] Enable Sensiron SCd40 CO2 sensor (I2C address 0x62) (+3k5 code)
#define USE_ADE7880                            // [I2cDriver65] Enable ADE7880 Energy monitor as used on Shelly 3EM (I2C address 0x38) (+3k8)
#define USE_ADE7953                            // [I2cDriver7] Enable ADE7953 Energy monitor as used on Shelly 2.5 (I2C address 0x38) (+1k5)
#define USE_VL53L0X                            // [I2cDriver31] Enable VL53L0x time of flight sensor (I2C address 0x29) (+4k code)
#define USE_HIH6                               // [I2cDriver36] Enable Honeywell HIH Humidity and Temperature sensor (I2C address 0x27) (+0k6)
#define USE_DHT12                              // [I2cDriver41] Enable DHT12 humidity and temperature sensor (I2C address 0x5C) (+0k7 code)
#define USE_DS1624                             // [I2cDriver42] Enable DS1624, DS1621 temperature sensor (I2C addresses 0x48 - 0x4F) (+1k2 code)
#define USE_WEMOS_MOTOR_V1                     // [I2cDriver44] Enable Wemos motor driver V1 (I2C addresses 0x2D - 0x30) (+0k7 code)
  #define WEMOS_MOTOR_V1_ADDR  0x30              // Default I2C address 0x30
  #define WEMOS_MOTOR_V1_FREQ  1000              // Default frequency
#define USE_IAQ                                // [I2cDriver46] Enable iAQ-core air quality sensor (I2C address 0x5a) (+0k6 code)
#define USE_AS3935                             // [I2cDriver48] Enable AS3935 Franklin Lightning Sensor (I2C address 0x03) (+5k4 code)
#define USE_SPI                                // Hardware SPI using GPIO12(MISO), GPIO13(MOSI) and GPIO14(CLK) in addition to two user selectable GPIOs(CS and DC)
#define USE_MCP23XXX_DRV                         // [I2cDriver77] Enable MCP23xxx support as virtual switch/button/relay (+3k(I2C)/+5k(SPI) code)
#define USE_SHELLY_PRO                           // Add support for Shelly Pro
#define USE_SPI_LORA                             // Add support for LoRaSend and LoRaCommand (+4k code)
  #define USE_LORA_SX126X                        // Add driver support for LoRa on SX126x based devices like LiliGo T3S3 Lora32 (+16k code)
  #define USE_LORA_SX127X                        // Add driver support for LoRa on SX127x based devices like M5Stack LoRa868, RFM95W (+5k code)
  #define USE_LORAWAN_BRIDGE                     // Add support for LoRaWan bridge (+8k code)
#define USE_MHZ19                                // Add support for MH-Z19 CO2 sensor (+2k code)
#define USE_SENSEAIR                             // Add support for SenseAir K30, K70 and S8 CO2 sensor (+2k3 code)
#define USE_CM110x                               // Add support for CM110x CO2 sensors (+2k7 code)
#ifndef CO2_LOW
  #define CO2_LOW              800               // Below this CO2 value show green light (needs PWM or WS2812 RG(B) led and enable with SetOption18 1)
#endif
#ifndef CO2_HIGH
  #define CO2_HIGH             1200              // Above this CO2 value show red light (needs PWM or WS2812 RG(B) led and enable with SetOption18 1)
#endif
#define USE_PMS5003                              // Add support for PMS5003 and PMS7003 particle concentration sensor (+1k3 code)
#define USE_NOVA_SDS                             // Add support for SDS011 and SDS021 particle concentration sensor (+0k7 code)
#define USE_HPMA                                 // Add support for Honeywell HPMA115S0 particle concentration sensor
#define USE_SR04                                 // Add support for HC-SR04 ultrasonic devices (+1k code)
#define USE_SERIAL_BRIDGE                        // Add support for software Serial Bridge (+2k code)
#define USE_MODBUS_BRIDGE                        // Add support for software Modbus Bridge (+3k code)
#define USE_MP3_PLAYER                           // Use of the DFPlayer Mini MP3 Player RB-DFR-562 commands: play, volume and stop
#define USE_PN532_HSU                            // Add support for PN532 using HSU (Serial) interface (+1k8 code, 140 bytes mem)
#define USE_RDM6300                              // Add support for RDM6300 125kHz RFID Reader (+0k8)
#define USE_IBEACON                              // Add support for bluetooth LE passive scan of ibeacon devices (uses HM17 module)
#define USE_HRXL                                 // Add support for MaxBotix HRXL-MaxSonar ultrasonic range finders (+0k7)
#define USE_ENERGY_SENSOR                        // Add energy sensors (-14k code)
#define USE_PZEM004T                             // Add support for PZEM004T Energy monitor (+2k code)
#define USE_PZEM_AC                              // Add support for PZEM014,016 Energy monitor (+1k1 code)
#define USE_PZEM_DC                              // Add support for PZEM003,017 Energy monitor (+1k1 code)
#define USE_MCP39F501                            // Add support for MCP39F501 Energy monitor as used in Shelly 2 (+3k1 code)
#define USE_SDM72                                // Add support for Eastron SDM72-Modbus energy monitor (+0k3 code)
#define USE_SDM120                               // Add support for Eastron SDM120-Modbus energy monitor (+1k1 code)
#define USE_SDM230                               // Add support for Eastron SDM230-Modbus energy monitor (+?? code)
#define USE_SDM630                               // Add support for Eastron SDM630-Modbus energy monitor (+0k6 code)
#define USE_DDS2382                              // Add support for Hiking DDS2382 Modbus energy monitor (+0k6 code)
#define USE_DDSU666                              // Add support for Chint DDSU666 Modbus energy monitor (+0k6 code)
#define USE_WE517                                // Add support for Orno WE517-Modbus energy monitor (+1k code)
#define USE_SONOFF_SPM                           // Add support for ESP32 based Sonoff Smart Stackable Power Meter (+11k code)
#define USE_MODBUS_ENERGY                        // Add support for generic modbus energy monitor using a user file in rule space (+5k code)
#define USE_BL0906                               // Add support for BL0906 up to 6 channel Energy monitor as used in Athom EM2/EM6
#define USE_DHT                                  // Add support for DHT11, AM2301 (DHT21, DHT22, AM2302, AM2321) and SI7021 Temperature and Humidity sensor
#define USE_MAX31855                             // Add support for MAX31855 K-Type thermocouple sensor using softSPI
#define USE_IR_REMOTE                            // Send IR remote commands using library IRremoteESP8266 and ArduinoJson (+4k code, 0k3 mem, 48 iram)
  #define USE_IR_RECEIVE                         // Support for IR receiver (+5k5 code, 264 iram)
#define USE_LMT01                                // Add support for TI LMT01 temperature sensor, count pulses on single GPIO (+0k5 code)
#define USE_SHIFT595                             // Add support for 74xx595 8-bit shift registers (+0k7 code)
#define USE_TM1638                               // Add support for TM1638 switches copying Switch1 .. Switch8 (+1k code)
#define USE_HX711                                // Add support for HX711 load cell (+1k5 code)
#define USE_RC_SWITCH                            // Add support for RF transceiver using library RcSwitch (+2k7 code, 460 iram)
#define USE_RF_SENSOR                            // Add support for RF sensor receiver (434MHz or 868MHz) (+0k8 code)
#define USE_ALECTO_V2                            // Add support for decoding Alecto V2 sensors like ACH2010, WS3000 and DKW2012 using 868MHz RF sensor receiver (+1k7 code)
#define USE_HRE                                  // Add support for Badger HR-E Water Meter (+1k4 code)
#define USE_BP1658CJ                             // Add support for BP1658CJ 5 channel led controller as used in Orein OS0100411267 Bulb
#define USE_ETHERNET                             // Add support for ethernet (+20k code)
#define USE_DISPLAY_TM1621_SONOFF                // Add support for TM1621 display driver used by Sonoff POWR3xxD and THR3xxD
#define USE_LOX_O2                               // Add support for LuminOx LOX O2 Sensor (+0k8 code)
#ifndef USE_KNX
  #define USE_KNX                                // Enable KNX IP Protocol Support (+23k code, +3k3 mem)
#endif
#define USE_DALI                                 // Add support for DALI gateway (+5k code)
*/

//----------------------------------------------------------------------------

//-- Stack size erhöhen (Empfehlung: seit Core3 wird mehr benötigt)
#undef SET_ESP32_STACK_SIZE
#define SET_ESP32_STACK_SIZE (12 * 1024)

//-- Optional: Für mein SML Simulator Script (max String Länge = 128). Im Script >D 128
//-- Möglicher Rebootfix - Testen!
#undef SCRIPT_MAXSSIZE
#define SCRIPT_MAXSSIZE 128

//-- enables to use 4096 in stead of 256 bytes buffer for variable names
#define SCRIPT_LARGE_VNBUFF

//-- ESP32 Skriptgröße (max Anzahl an Zeichen) https://tasmota.github.io/docs/Scripting-Language/#script-buffer-size
#define USE_UFILESYS
#undef UFSYS_SIZE
#define UFSYS_SIZE 16384  //ESP32

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
/*#ifndef USE_SML_AUTHKEY
#define USE_SML_AUTHKEY
#endif*/

//-- Verwende Home Assistant API
#define USE_HOME_ASSISTANT  //(+12k code, +6 bytes mem)

//-- Optionale Features beim ESP32-C6 deaktivieren (Rebootproblem)
//#ifndef TASMOTA32C6_OTTELO

//-- Software Serial für ESP32 (nur RX), Pin mit dem Zeichen '-' in der SML Sektion definieren (bei mehr als 2/3-Leseköpfen, je nach ESP32 Variante)
#define USE_ESP32_SW_SERIAL

//-- Optional: Serielle Schnittstelle (RX/TX RS232) im Script verwenden
#define USE_SCRIPT_SERIAL

//-- Optional: ESP32 WT32_ETH01 (Ethernet LAN Modul)
#ifdef TASMOTA32_ETH_OTTELO
  #define USE_ETHERNET          // Add support for ethernet (+20k code)
  #define USE_WT32_ETH01
  #define ETH_TYPE          0
  #define ETH_ADDRESS       0
  #define ETH_CLKMODE       3
#endif

//-- Optional: TCP-Server Script Support
#define USE_SCRIPT_TCP_SERVER
#define USE_SCRIPT_TASK

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