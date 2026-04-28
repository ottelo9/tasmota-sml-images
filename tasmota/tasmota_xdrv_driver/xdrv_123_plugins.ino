/*
  xdrv_123_plugins.ino - Prove of concept for flash plugins

  Copyright (C) 2021  Gerhard Mutz

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


/* 
relocatable flash module driver handler
with runtime link and unlink
adds about 20k flash size 

to doo:


*/

#ifdef USE_BINPLUGINS

#define XDRV_123             123

//#define EXECUTE_FROM_BINARY

#include "./Plugins/modules_def.h"
#include <TasmotaSerial.h>
#include "TimeLib.h"
#ifdef ESP32
// for `struct linger` / SOL_SOCKET / SO_LINGER used by jt[171] op 103
// (client_setLinger). ESP8266 path doesn't expose setSocketOption().
#include <lwip/sockets.h>
#endif

// minimal plugin rev
#define MINREV 0x00010004
#define CURR_MINREV 0x00010005

#define MAX_MOD_STORES 4
// this descriptor is in flash so only 32 bit access allowed
#pragma pack(4)
typedef struct {
  MD_TYPE sync;
  MD_TYPE arch;
  MD_TYPE type;
  MD_TYPE revision;
  char name[16];
  // 32 => 0x20
  int32_t (*mod_func_execute)(uint32_t);
  void (*end_of_module)(void);
  MD_TYPE size;
  // 40 => 0x28
  MD_TYPE execution_offset;
  // 44 => 0x2c
  MD_TYPE mtv;
  MD_TYPE jtab;
  // 52 = 0x34
  uint32_t mod_start_org;
  int32_t (*mod_func_execute_org)(uint32_t);
  
  // 56
  MODULE_STORE ms[];
} FLASH_MODULE;

#define EXEC_OFFSET ((FLASH_MODULE*)mt->mod_addr)->execution_offset

#ifdef EXECUTE_FROM_BINARY
extern const FLASH_MODULE module_header;
#else
// set dummy header to calm linker
FLASH_MODULE module_header = {
  0,
  CURR_ARCH,
  MODULE_TYPE_SENSOR,
  0,
  "MLX90614",
  0,
  0,
  0,
  0
};
#endif

extern FS *ffsp;

#ifndef module_name
#define module_name "/module.bin"
#endif


//  command line commands
const char kModuleCommands[] PROGMEM = "|"// no Prefix
  "mdir" "|"
  "link" "|"
  "unlink" "|"
  "iniz" "|"
  "deiniz" "|"
  "dump" "|"
  "chkpt" "|"
  "hbn"
  ;

void (* const ModuleCommand[])(void) PROGMEM = {
  &Module_mdir,  &Module_link, &Module_unlink, &Module_iniz, &Module_deiniz, &Module_dump, &Check_partition, &Test_prog
#ifdef USE_FLASH_BDIR 
   ,&BinDir_list
#endif
};

#ifdef ESP32
const char kModuleCommands1[] PROGMEM = "|" "chkpt" "|" "hbn";
void (* const ModuleCommand1[])(void) PROGMEM = {
 &Check_partition, &Test_prog
};
#endif // ESP32

void Serial_print(const char *txt) {
  //Serial.printf("test: %x %x\n",(uint32_t)txt, *(uint32_t*)txt);
  //Serial.printf("test: %x\n",(uint32_t)txt);
  Serial.printf_P(PSTR("test: %s\n"),txt);
}

// ESP32 combined hardware and software serial driver, software read only
#if defined(ESP32)

#define USE_ESP32_SW_SERIAL

#ifdef USE_ESP32_SW_SERIAL

#ifndef ESP32_SWS_BUFFER_SIZE
#define ESP32_SWS_BUFFER_SIZE 256
#endif

class PLUGIN_ESP32_SERIAL : public Stream {
public:
	PLUGIN_ESP32_SERIAL(uint32_t uart_index);
  virtual ~PLUGIN_ESP32_SERIAL();
  bool begin(uint32_t speed, uint32_t smode, int32_t recpin, int32_t trxpin, int32_t invert);
  int peek(void);
  int read(void) override;
  size_t write(uint8_t) override;
  int available(void) override;
  void flush(void) override;
  void setRxBufferSize(uint32_t size);
  void updateBaudRate(uint32_t baud);
  void plugin_rxRead(void);
  void end();
  using Print::write;
private:
  // Member variables
  void setbaud(uint32_t speed);
  uint32_t uart_index;
  int8_t m_rx_pin;
  int8_t m_tx_pin;
  uint32_t cfgmode;
  uint32_t ss_byte;
  uint32_t ss_bstart;
  uint32_t ss_index;
  uint32_t m_bit_time;
  uint32_t m_in_pos;
  uint32_t m_out_pos;
  uint16_t serial_buffer_size;
  bool m_valid;
  uint8_t *m_buffer;
  HardwareSerial *hws;
};

void IRAM_ATTR plugin_callRxRead(void *self);

void plugin_callRxRead(void *self) { 
  ((PLUGIN_ESP32_SERIAL*)self)->plugin_rxRead(); 
};

PLUGIN_ESP32_SERIAL::PLUGIN_ESP32_SERIAL(uint32_t index) {
  uart_index = index;
  m_valid = true;
}

PLUGIN_ESP32_SERIAL::~PLUGIN_ESP32_SERIAL(void) {
  if (hws) {
    hws->end();
		delete(hws);
  } else {
    detachInterrupt(m_rx_pin);
    if (m_buffer) {
      free(m_buffer);
    }
  }
}

void PLUGIN_ESP32_SERIAL::setbaud(uint32_t speed) {
#ifdef __riscv
  m_bit_time = 1000000 / speed;
#else
  m_bit_time = ESP.getCpuFreqMHz() * 1000000 / speed;
#endif
}

void PLUGIN_ESP32_SERIAL::end(void) {
  if (m_buffer) {
    free(m_buffer);
  }
}

bool PLUGIN_ESP32_SERIAL::begin(uint32_t speed, uint32_t smode, int32_t recpin, int32_t trxpin, int32_t invert) {
  if (!m_valid) { return false; }

  m_buffer = 0;
  if (recpin < 0) {
    setbaud(speed);
    m_rx_pin = -recpin;
    serial_buffer_size = ESP32_SWS_BUFFER_SIZE;
    m_buffer = (uint8_t*)malloc(serial_buffer_size);
    if (m_buffer == NULL) return false;
    pinMode(m_rx_pin, INPUT_PULLUP);
    attachInterruptArg(m_rx_pin, plugin_callRxRead, this, CHANGE);
    m_in_pos = m_out_pos = 0;
    hws = nullptr;
  } else {
    cfgmode = smode;
    m_rx_pin = recpin;
    m_tx_pin = trxpin;
    hws = new HardwareSerial(uart_index);
    if (hws) {
      hws->begin(speed, cfgmode, m_rx_pin, m_tx_pin, invert);
    }
  }
  return true;
}

void PLUGIN_ESP32_SERIAL::flush(void) {
  if (hws) {
    hws->flush();
  } else {
    m_in_pos = m_out_pos = 0;
  }
}

int PLUGIN_ESP32_SERIAL::peek(void) {
  if (hws) {
    return  hws->peek();
  } else {
    if (m_in_pos == m_out_pos) return -1;
    return m_buffer[m_out_pos];
  }
}

int PLUGIN_ESP32_SERIAL::read(void) {
  if (hws) {
    return hws->read();
  } else {
    if (m_in_pos == m_out_pos) return -1;
    uint32_t ch = m_buffer[m_out_pos];
    m_out_pos = (m_out_pos + 1) % serial_buffer_size;
    return ch;
  }
}

int PLUGIN_ESP32_SERIAL::available(void) {
  if (hws) {
    return hws->available();
  } else {
    int avail = m_in_pos - m_out_pos;
    if (avail < 0) avail += serial_buffer_size;
    return avail;
  }
}

size_t PLUGIN_ESP32_SERIAL::write(uint8_t v) {
  if (hws) {
    return hws->write(v);
  }
  return 0;
}

void PLUGIN_ESP32_SERIAL::setRxBufferSize(uint32_t size) {
  if (hws) {
    hws->setRxBufferSize(size);
  } else {
    if (m_buffer) {
        free(m_buffer);
    }
    serial_buffer_size = size;
    m_buffer = (uint8_t*)malloc(size);
  }
}
void PLUGIN_ESP32_SERIAL::updateBaudRate(uint32_t baud) {
  if (hws) {
    hws->updateBaudRate(baud);
  } else {
    setbaud(baud);
  }
}

// no wait mode only 8N1  (or 7X1, obis only, ignoring parity)

void IRAM_ATTR PLUGIN_ESP32_SERIAL::plugin_rxRead(void) {
  uint32_t diff;
  uint32_t level;

#define SML_LASTBIT 9

  level = digitalRead(m_rx_pin);

  if (!level && !ss_index) {
    // start condition
#ifdef __riscv
    ss_bstart = micros() - (m_bit_time / 4);
#else
    ss_bstart = ESP.getCycleCount() - (m_bit_time / 4);
#endif
    ss_byte = 0;
    ss_index++;
  } else {
    // now any bit changes go here
    // calc bit number
#ifdef __riscv
    diff = (micros() - ss_bstart) / m_bit_time;
#else
    diff = (ESP.getCycleCount() - ss_bstart) / m_bit_time;
#endif

    if (!level && diff > SML_LASTBIT) {
      // start bit of next byte, store  and restart
      // leave irq at change
      for (uint32_t i = ss_index; i <= SML_LASTBIT; i++) {
        ss_byte |= (1 << i);
      }
      uint32_t next = (m_in_pos + 1) % serial_buffer_size;
      if (next != (uint32_t)m_out_pos) {
        m_buffer[m_in_pos] = ss_byte >> 1;
        m_in_pos = next;
      }
#ifdef __riscv
      ss_bstart = micros() - (m_bit_time / 4);
#else
      ss_bstart = ESP.getCycleCount() - (m_bit_time / 4);
#endif
      ss_byte = 0;
      ss_index = 1;
      return;
    }
    if (diff >= SML_LASTBIT) {
      // bit zero was 0,
      uint32_t next = (m_in_pos + 1) % serial_buffer_size;
      if (next != (uint32_t)m_out_pos) {
        m_buffer[m_in_pos] = ss_byte >> 1;
        m_in_pos = next;
      }
      ss_byte = 0;
      ss_index = 0;
    } else {
      // shift in
      for (uint32_t i = ss_index; i < diff; i++) {
        if (!level) ss_byte |= (1 << i);
      }
      ss_index = diff;
    }
  }
}

#endif // USE_ESP32_SW_SERIAL
#endif // ESP32

#define USE_PLUGIN_COUNTER

#ifdef USE_PLUGIN_COUNTER

PLUGIN_COUNTER *global_pcnt;

void IRAM_ATTR Plugin_CounterIsr(void *arg);
void Plugin_CounterIsr(void *arg) {

  uint32_t index = *static_cast<uint8_t*>(arg);

  PLUGIN_COUNTER *pars = global_pcnt;
  pars += index;

  uint32_t time = millis();

  if (digitalRead(pars->srcpin) == pars->pinstate) {
    return;
  }
  
  uint32_t debounce_time = time - pars->counter_ltime;

  if (debounce_time <= pars->debounce) return;

  if (pars->pinstate) {
    // falling edge
    RtcSettings.pulse_counter[index]++;
    pars->counter_pulsewidth = time - pars->counter_lfalltime;
    pars->counter_lfalltime = time;
    pars->cnt_updated = 1;
  }
  pars->counter_ltime = time;
  pars->pinstate ^= 1;
}
#endif

TasmotaSerial *tmod_newTS(int32_t rpin, int32_t tpin);
int tmod_beginTS(TasmotaSerial *ts, uint32_t baud);
size_t tmod_writeTS(TasmotaSerial *ts, char *buf, uint32_t size);
void tmod_flushTS(TasmotaSerial *ts);
void tmod_deleteTS(TasmotaSerial *ts);
size_t tmod_readTS(TasmotaSerial *ts, char *buf, uint32_t size);
int tmod_read1TS(TasmotaSerial *ts);
uint8_t tmod_availTS(TasmotaSerial *ts);
bool hardwareSerialTS(TasmotaSerial *ts);
void AddlogT(char* txt);
bool MT_DecodeCommand(const char* haystack, void (* const InCommand[])(void), MODULES_TABLE *mt);
size_t tmod_write1TS(TasmotaSerial *ts, uint8_t val);
#ifdef ESP32
void twi_readFrom(uint8_t address, uint8_t* data, uint8_t length);
#endif
bool tmod_I2cSetDevice(uint32_t addr, uint32_t bus);
void tmod_I2cSetActiveFound(uint32_t addr, const char *types, uint32_t bus);
int tmod_strncasecmp_P(const char* s1, const char *s2, size_t len);
char *copyStr(const char * str);
void tmod_setClockStretchLimit(TwoWire *wp, uint32_t val);
void tmod_writen(TwoWire *wp, uint8_t *buf, uint32_t len);
int tmod_snprintf_P(char *s, size_t n,  const char *format, ...);
int tmod_sprintf_P(char *s, const char *format, ...);
int tmod_ResponseAppend_P(const char* format, ...);
void tmod_WSContentSend_PD(const char* format, ...);
void tmod_WSContentSend_P(const char* format, ...);
float fl_const(int32_t m, int32_t d);
char *tm_trim(char *s);
void tmod_vTaskEnterCritical( void * );
void tmod_vTaskExitCritical( void * );
uint32_t IRAM_ATTR tmod_directRead(uint32_t pin);
void IRAM_ATTR tmod_directWriteLow(uint32_t pin);
void IRAM_ATTR tmod_directWriteHigh(uint32_t pin);
void IRAM_ATTR tmod_directModeInput(uint32_t pin);
void IRAM_ATTR tmod_directModeOutput(uint32_t pin);
char * tmod_GetTextIndexed(char* destination, size_t destination_size, uint32_t index, const char* haystack);
bool WebServer_hasArg(const char * str);
void tmod_WSContentStart_P(const char* title);
char * tmod_strcpy_P(char *dst , const char *src);
char * tmod_strncpy_P(char *dst , const char *src, size_t len);
void tmod_WebServer_on(const char * prefix, void (*func)(void), uint8_t method);
void *tmod_gtbl(void);
#ifdef USE_SPI
SPIClass *tmod_getspi(uint8_t sel);
void tmod_spi_begin(SPIClass *spi, uint8_t flg, int8_t sck, int8_t miso, int8_t mosi);
void tmod_spi_write(SPIClass *spi, uint8_t data);
void tmod_spi_writebytes(SPIClass *spi, const uint8_t * data, uint32_t size);
void tmod_Transaction(SPIClass *spi, uint8_t flg, uint32_t spibaud);
uint8_t tmod_transfer(SPIClass *spi, uint8_t data);
#endif

char* ftostrfd(float number, unsigned char prec, char *s);
class File * tmod_file_open(char *path, char mode);
void tmod_file_close(class File *fp);
int32_t tmod_file_seek(class File *fp, uint32_t pos, uint32_t mode);
int32_t tmod_file_read(class File *fp, uint8_t *buff, uint32_t size);
int32_t tmod_file_write(class File *fp, uint8_t *buff, uint32_t size);
uint32_t tmod_file_size(class File *fp);
uint32_t tmod_file_pos(class File *fp);
void tmod_AddLogData(uint32_t loglevel, const char* log_data);
char *Plugin_Get_SensorNames(char *type, uint32_t index);
char *tmod_Run_Scripter(char *sect);
double tmod_double_dispatch(uint32_t sel, double a, double b);
int32_t tmod_double_cmp_dispatch(uint32_t sel, double a, double b);
uint32_t tmod_task_create(TASKPARS *tp);
int64_t tmod_double2long(double in);
double tmod_long2double(int64_t in);
void *tmod_strncat(char *dst, char *src, uint32_t size);
const uint8_t tmod_pgm_read_byte(uint8_t *ptr);
const uint16_t tmod_pgm_read_word(uint16_t *ptr);
void *tmod_special_malloc(uint32_t size);
uint32_t tmod_serialdispatch(uint32_t sel, uint32_t p1, uint32_t p2, uint32_t p3);
double tmod_floatdidf(int64_t);
double tmod_floatundidf(uint64_t);
double tmod_floatsidf(int32_t);
double tmod_floatunsidf(uint32_t);
int32_t tmod_fixdfdi(double);
uint32_t tmod_fixunsdfsi(double);
double tmod_extendsfdf2(float);
uint32_t tmod_random(uint32_t par);
double  tmod_floattidf(int64_t in);
double  realloc_floatuntidf(uint64_t in);
uint32_t GetNumGPIO(void);

extern "C" {
 extern void (* const MODULE_JUMPTABLE[])(void);
}

#define JMPTBL (void (*)())

// this vector table table must contain all api calls needed by module
// and in sync with vectortable in module.h
void (* const MODULE_JUMPTABLE[])(void) PROGMEM = {
  JMPTBL&Wire,
#ifdef ESP32
#ifdef USE_I2C_BUS2
  JMPTBL&Wire1,
#else
  JMPTBL&Wire,
#endif
#else
  JMPTBL&Wire,
#endif
  JMPTBL&Serial,
  JMPTBL&tmod_I2cSetDevice,
  //JMPTBL&I2cSetActiveFound,
  JMPTBL&tmod_I2cSetActiveFound,
  JMPTBL&AddLog,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&ResponseAppend_P,
  JMPTBL&WSContentSend_PD,
#else
  JMPTBL&tmod_ResponseAppend_P,
  JMPTBL&tmod_WSContentSend_PD,
#endif
  JMPTBL&ftostrfd,
  JMPTBL&calloc,
  JMPTBL&fscale,
  JMPTBL&Serial_print,
  JMPTBL&tmod_beginTransmission,
  JMPTBL&tmod_write,
  JMPTBL&tmod_endTransmission,
  JMPTBL&tmod_requestFrom,
  JMPTBL&tmod_read,
  JMPTBL&show_hex_address,
  JMPTBL&free,
  JMPTBL&I2cWrite16,
  JMPTBL&I2cRead16,
  JMPTBL&I2cValidRead16,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&snprintf_P,
#else
  JMPTBL&tmod_snprintf_P,
#endif
  //JMPTBL&XdrvRulesProcess,
  JMPTBL(bool (*)(bool teleperiod))&XdrvRulesProcess,
  JMPTBL&ResponseJsonEnd,
  JMPTBL&delay,
  JMPTBL&I2cActive,
  JMPTBL&ResponseJsonEndEnd,
  JMPTBL&IndexSeparator,
  JMPTBL&Response_P,
  JMPTBL&I2cResetActive,
  JMPTBL&tmod_isnan,
  JMPTBL&ConvertTemp,
  JMPTBL&ConvertHumidity,
  JMPTBL&TempHumDewShow,
  JMPTBL&strlcpy,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&GetTextIndexed,
#else
  JMPTBL&tmod_GetTextIndexed,
#endif
  JMPTBL&GetTasmotaGlobal,
  JMPTBL&tmod_iseq,
  JMPTBL&tmod_fdiv,
  JMPTBL&tmod_fmul,
  JMPTBL&tmod_fdiff,
  JMPTBL&tmod_tofloat,
  JMPTBL&tmod_fadd,
  JMPTBL&I2cRead8,
  JMPTBL&I2cWrite8,
  JMPTBL&tmod_available,
  JMPTBL&AddLogMissed,
  JMPTBL&tmod_NAN,
  JMPTBL&tmod_gtsf2,
  JMPTBL&tmod_ltsf2,
  JMPTBL&tmod_eqsf2,
  JMPTBL&tmod_Pin,
  JMPTBL&tmod_newTS,
  JMPTBL&tmod_writeTS,
  JMPTBL&tmod_flushTS,
  JMPTBL&tmod_beginTS,
  JMPTBL&XdrvMailbox,
  JMPTBL&tmod_GetCommandCode,
  JMPTBL&strlen,
  JMPTBL&tmod_strncasecmp_P,
  JMPTBL&toupper,
  JMPTBL&iscale,
  JMPTBL&tmod_deleteTS,
  JMPTBL&tmod_readTS,
  JMPTBL&tmod_read1TS,
  JMPTBL&tmod_availTS,
  JMPTBL&MqttPublishTeleSensor,
  JMPTBL&strtoul,
  JMPTBL&AddLogBuffer,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&ResponseTime_P,
#else
  JMPTBL&tmod_ResponseTime_P,
#endif
  JMPTBL&ClaimSerial,
  JMPTBL&hardwareSerialTS,
  JMPTBL&millis,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&sprintf_P,
#else
  JMPTBL&tmod_sprintf_P,
#endif
  JMPTBL&AddlogT,
  JMPTBL&tmod__divsi3,
  JMPTBL&tmod__udivsi3,
  JMPTBL&tmod__floatsisf,
  JMPTBL&tmod__floatunsisf,
  JMPTBL&FastPrecisePowf,
  JMPTBL&GetTasmotaGlobalf,
  JMPTBL&tmod__muldi3,
  JMPTBL&tmod__fixunssfsi,
  JMPTBL&tmod__umodsi3,
  JMPTBL&twi_readFrom,
  JMPTBL&MT_DecodeCommand,
  JMPTBL&ResponseCmndDone,
  JMPTBL&tmod_write1TS,
  JMPTBL&memcmp_P,
  JMPTBL&ToHex_P,
  JMPTBL&memset,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&memmove_P,
#else
  JMPTBL&tmod_memmove_P,
#endif
  JMPTBL&ResponseCmndNumber,
  JMPTBL&ResponseCmndFloat,
  JMPTBL&ResponseAppendTHD,
  JMPTBL&WSContentSend_THD,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&strncpy_P,
#else
  JMPTBL&tmod_strncpy_P,
#endif
  JMPTBL&isprint,
  JMPTBL&tmod_isinf,
  JMPTBL&copyStr,
  JMPTBL&tmod_setClockStretchLimit,
  JMPTBL&tmod_writen,
  JMPTBL&modff,
  JMPTBL&fl_const,
  JMPTBL&WSContentSend_Temp,
  JMPTBL&delayMicroseconds,
  JMPTBL&digitalRead,
  JMPTBL&digitalWrite,
  JMPTBL&pinMode,
  JMPTBL&strchr,
  JMPTBL&tm_trim,
  JMPTBL&tmod_vTaskEnterCritical,
  JMPTBL&tmod_vTaskExitCritical,
  JMPTBL&tmod_directRead,
  JMPTBL&tmod_directWriteLow,
  JMPTBL&tmod_directWriteHigh,
  JMPTBL&tmod_directModeInput,
  JMPTBL&tmod_directModeOutput,
  JMPTBL&CalcTempHumToAbsHum,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&WSContentSend_P,
#else
  JMPTBL&tmod_WSContentSend_P,
#endif
  JMPTBL&HttpCheckPriviledgedAccess,
  JMPTBL&tmod_WSContentStart_P,
  JMPTBL&WSContentSendStyle,
  JMPTBL&WSContentSpaceButton,
  JMPTBL&WSContentStop,
  JMPTBL&tmod_WebGetArg,
  JMPTBL&WebRestart,
  JMPTBL&WebServer_hasArg,
  JMPTBL&tmod_WebServer_on,
  JMPTBL&atoi,
  JMPTBL&tmod_strcpy_P,
  JMPTBL&SetTasmotaGlobal,
  JMPTBL&tmod_fixsfti,
  JMPTBL&tmod_gtbl,
  JMPTBL&Settings,
  #ifdef USE_SPI
  JMPTBL&tmod_getspi,
  JMPTBL&tmod_spi_begin,
  JMPTBL&tmod_spi_write,
  JMPTBL&tmod_spi_writebytes,
  JMPTBL&tmod_Transaction,
  JMPTBL&tmod_transfer,
  #else
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
  #endif
  JMPTBL&tmod_file_open,
  JMPTBL&tmod_file_close,
  JMPTBL&tmod_file_seek,
  JMPTBL&tmod_file_read,
  JMPTBL&tmod_file_write,
  JMPTBL&CharToFloat,
  JMPTBL&tmod_AddLogData,
  JMPTBL&tmod_file_exists,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&strncmp_P,
#else
  JMPTBL&tmod_strncmp_P,
#endif
  JMPTBL&tmod_special_malloc,
  JMPTBL&ResponseCmndChar,
  JMPTBL&strtol,
  JMPTBL&tmod_udp,
  JMPTBL&tmod_i2s,
#ifdef ESP32
  JMPTBL&tmod_task_create,
  JMPTBL&tmod_task_delete,
#else
  JMPTBL&tmod_dummy,
  JMPTBL&tmod_dummy,
#endif
  JMPTBL&Plugin_Get_SensorNames,
  JMPTBL&tmod_Run_Scripter,
  JMPTBL&tmod_file_size,
  JMPTBL&tmod_file_pos,
  JMPTBL&OsWatchLoop,
  JMPTBL&tmod_double_dispatch,
  JMPTBL&tmod_double2long,
  JMPTBL&tmod_long2double,
  JMPTBL&MqttPublishSensor,
  JMPTBL&ParseParameters,
  JMPTBL&tmod__modsi3,
  JMPTBL&tmod__ashldi3,
  JMPTBL&tmod__lshrdi3,
  JMPTBL&tmod_wifi,
  JMPTBL&tmod_strncat,
  JMPTBL&tmod_pgm_read_byte,
  JMPTBL&tmod_pgm_read_word,
  JMPTBL&tmod_serialdispatch,
  JMPTBL&dtostrfd,
#ifdef USE_SCRIPT
  JMPTBL&Replace_Cmd_Vars,
#else
  JMPTBL&tmod_dummy,
#endif
  JMPTBL&PinUsed,
  JMPTBL&atoll,
  JMPTBL&tmod_double_cmp_dispatch,
  JMPTBL&tmod_floatdidf,
  JMPTBL&tmod_floatundidf,
  JMPTBL&tmod_floatsidf,
  JMPTBL&tmod_floatunsidf,
  JMPTBL&tmod_fixdfdi,
  JMPTBL&tmod_fixunsdfsi,
  JMPTBL&tmod_extendsfdf2,
  JMPTBL&tmod_random,
  JMPTBL&realloc,
  JMPTBL&tmod_floattidf,
  JMPTBL&tmod_floatuntidf,
  JMPTBL&br_aes_small_ctr_init,
  JMPTBL&br_gcm_init,
  JMPTBL&br_gcm_reset,
  JMPTBL&br_gcm_aad_inject,
  JMPTBL&br_gcm_flip,
  JMPTBL&br_gcm_run,
  JMPTBL&br_gcm_check_tag_trunc,
#if defined(ESP8266) || defined(__riscv)
  JMPTBL&vsnprintf_P,
#else
  JMPTBL&tmod_vsnprintf_P,
#endif
  JMPTBL&makeTime,
  JMPTBL&GetNumGPIO,
  JMPTBL&tmod_wc,
  JMPTBL&tmod_jpeg_picture,
  JMPTBL&tmod_shine,
  JMPTBL&tmod_sinf,       // 205
  JMPTBL&tmod_cosf,       // 206
  JMPTBL&tmod_logf,       // 207
  JMPTBL&tmod_sqrtf       // 208
};


uint32_t GetNumGPIO(void) {
  return nitems(TasmotaGlobal.gpio_pin);
}

int tmod_vsnprintf_P(char *s, size_t strSize, const char *format, ...) {
  int res = 0;
#ifdef ESP32
  char *fcopy = copyStr(format);
  va_list arglist;
  va_start(arglist, format);
  res = vsnprintf_P(s, strSize, fcopy, arglist);
  va_end(arglist);
  free(fcopy);
#endif
  return res;
}


double  tmod_floattidf(int64_t in) {
  return in;
}

double  tmod_floatuntidf(uint64_t in) {
  return in;
}

uint32_t tmod_random(uint32_t par) {
  return random(par);
}

double tmod_floatdidf(int64_t in) {
  return in;
}
double tmod_floatundidf(uint64_t in) {
  return in;
}
double tmod_floatsidf(int32_t in) {
  return in;
}
double tmod_floatunsidf(uint32_t in) {
  return in;
}
int32_t tmod_fixdfdi(double in) {
  return in;
}
uint32_t tmod_fixunsdfsi(double in) {
  return in;
}
double tmod_extendsfdf2(float in) {
  return in;
}


#define USE_DOUBLE_DISPATCH


#ifdef ESP32
#ifndef __riscv
#define pgm_read_with_offset_32(addr, res) \
  asm("extui    %0, %1, 0, 2\n"     /* Extract offset within word (in bytes) */ \
      "sub      %1, %1, %0\n"       /* Subtract offset from addr, yielding an aligned address */ \
      "l32i.n   %1, %1, 0x0\n"      /* Load word from aligned address */ \
      "ssa8l    %0\n"               /* Prepare to shift by offset (in bits) */ \
      "src      %0, %1, %1\n"       /* Shift right; now the requested byte is the first one */ \
      :"=r"(res), "=r"(addr) \
      :"1"(addr) \
      :);

#define pgm_read_dword_with_offset_32(addr, res) \
  asm("extui    %0, %1, 0, 2\n"     /* Extract offset within word (in bytes) */ \
      "sub      %1, %1, %0\n"       /* Subtract offset from addr, yielding an aligned address */ \
      "l32i     a15, %1, 0\n" \
      "l32i     %1, %1, 4\n" \
      "ssa8l    %0\n" \
      "src      %0, %1, a15\n" \
      :"=r"(res), "=r"(addr) \
      :"1"(addr) \
      :"a15");
#endif
#endif

const uint8_t tmod_pgm_read_byte(uint8_t *ptr) {
#ifdef ESP8266  
  return pgm_read_byte(ptr);
#endif
#ifdef ESP32
#ifndef __riscv
 uint32_t res;
  pgm_read_with_offset_32(ptr, res);
  return (uint8_t) res;
#else
 return (*(const unsigned char *)(ptr));
#endif
#endif
}

const uint16_t tmod_pgm_read_word(uint16_t *ptr) {
#ifdef ESP8266 
  return pgm_read_word(ptr);
#endif
#ifdef ESP32
#ifndef __riscv
 uint32_t res;
  pgm_read_dword_with_offset_32(ptr, res);
  return (uint16_t) res;
#else
  return *(const uint16_t *)(ptr);
#endif
#endif
}

double tmod_double_dispatch(uint32_t sel, double a, double b) {
  double result = 0;
#ifdef USE_DOUBLE_DISPATCH 
  switch (sel) {
    case 0:
      result = a + b;
      break;
    case 1:
      result = a - b;
      break;
    case 2:
      result = a * b;
      break;
    case 3:
      result = a / b;
      break;
  }
#endif
  return result;
}

int32_t tmod_double_cmp_dispatch(uint32_t sel, double a, double b) {
int32_t result = 0;

  switch(sel) {
    case 0:
      result = a < b;
      break;
    case 1:
      result = a != b;
      break;
    case 2:
      result = a > b;
      break;
    case 3:
      result = a == b;
      break;
  }
  return result;
}

int64_t tmod_double2long(double in) {
  return in;
}

double tmod_long2double(int64_t in) {
  return in;
}


char *tmod_Run_Scripter(char *sect) {

#ifdef USE_SCRIPT  
  char *cp = copyStr(sect);
  uint8_t meter_script = Run_Scripter(cp, -2, 0);
  free(cp);
  if (meter_script != 99) {
    return nullptr;
  }
  return glob_script_mem.section_ptr;
#else
  return nullptr;
#endif
}

#ifdef ESP32
uint32_t tmod_task_create(TASKPARS *tp) {
  uint32_t result;
  char *cp = copyStr(tp->constpcName);
  //AddLog(LOG_LEVEL_INFO,PSTR("task Init %s - %d"), cp, tp->usStackDepth);

  result = xTaskCreatePinnedToCore(tp->pvTaskCode, cp, tp->usStackDepth, tp->constpvParameters, (UBaseType_t)tp->uxPriority, (TaskHandle_t*)tp->constpvCreatedTask, (const BaseType_t)tp->xCoreID);
  free(cp);
  return result;
}
uint32_t tmod_task_delete(uint32_t xTaskToDelete) {
  vTaskDelete((TaskHandle_t)xTaskToDelete);
  return 0;
}
#endif

uint32_t tmod_dummy() {
  return 0;
}


#if defined(ESP32) && defined(USE_TLS)
#include "WiFiClientSecureLightBearSSL.h"
#endif

uint32_t tmod_wifi(uint32_t sel, uint32_t p1, uint32_t p2, uint32_t p3, uint32_t p4) {
  WiFiClient *client =(WiFiClient*) p1;
  HTTPClient *http = (HTTPClient*) p1;
#if defined(ESP32) && defined(USE_TLS)
  BearSSL::WiFiClientSecure_light *sclient =(BearSSL::WiFiClientSecure_light*) p1;
#endif

  switch (sel) {
    case 0:
      client = new WiFiClient;
      //AddLog(LOG_LEVEL_INFO,PSTR(">>> %8x"),(uint32_t)client);
      return (uint32_t)client;
    case 1:
    {
      int32_t err = client->connect((char*)p2, p3);
      return err;
    }
    case 2:
      return client->connected();
    case 3:
      return client->available();
    case 4:
      return client->read();
    case 5:
      return client->read((uint8_t*)p2, p3);
    case 6:
      client->stop();
      break;
    case 7:
      delete client;
      break;
    case 8:
      return client->write((uint8_t*)p2, p3);
    case 9:
      return client->peek();
    case 100:
      client->flush();
      break;
    case 101:
      client->setTimeout(p2);
      break;
    case 102:
      {
#if defined(ESP8266) || defined(__riscv)
      client->print((char*)p2);
#else
      char *fcopy = copyStr((char*)p2);
      client->print(fcopy);
      free(fcopy);
#endif
      }
      break;
#ifdef ESP32
    case 103:
      // setLinger(on, seconds): sends TCP RST instead of FIN when on=1, time=0,
      // freeing the peer's session slot immediately. Routed through here so PIC
      // plugins (e.g. xsns_53_sml) don't need to call WiFiClient::setSocketOption
      // directly — that would be an unrouted external symbol.
      // p2 = on flag (0|1), p3 = linger time in seconds.
      {
        struct linger sl = { (int)p2, (int)p3 };
        client->setSocketOption(SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
      }
      break;
#endif

#if defined(ESP32) && defined(USE_TLS)
    case 10:
      sclient = new BearSSL::WiFiClientSecure_light(1024,1024);;
      return (uint32_t)sclient;
    case 11:
    {
      int32_t err = sclient->connect((char*)p2, p3);
      return err;
    }
    case 12:
      return sclient->connected();
    case 13:
      return sclient->available();
    case 14:
      return sclient->read();
    case 15:
      return sclient->read((uint8_t*)p2, p3);
    case 16:
      sclient->stop();
      break;
    case 17:
      delete sclient;
      break;
   case 18:
      sclient->setInsecure();
      break;
    case 19:
      sclient->setTimeout(p2);
#endif // ESP32 + USE_TLS

    // class http
    case 30:
      http = new HTTPClient;
      return (uint32_t)http;
    case 31:
      http->end();
      break;
    case 32:
      delete http;
      break;
    case 33:
      {
      WiFiClient *client = (WiFiClient*)p2;
      //AddLog(LOG_LEVEL_INFO,PSTR(">>> %8x"),(uint32_t)client);
      return http->begin(*client, (char*)p3);
      }
    case 34:
      http->setReuse(p2);
      break;
    case 35:
      return http->GET();
    case 36:
      return http->getSize();
    case 37:
      return http->connected();
    case 38:
      // returns client
      return (uint32_t)http->getStreamPtr();
    case 39:
      { 
        char *cp2 = copyStr((char*)p2);
        char *cp3 = copyStr((char*)p3);
        http->addHeader((const char*)cp2, (const char*)cp3);
        free(cp2);
        free(cp3);
        break;
      }
    case 40:
      { // gets array of char pointers without execoffset
        const char *hdr[8];
        const char **sap = (const char**)p2;
        if (p3 > 8) p3  = 8;
        MODULES_TABLE *mt = (MODULES_TABLE *)p4;
        for (uint32_t cnt = 0; cnt < p3; cnt++) {
          hdr[cnt] = sap[cnt];
          hdr[cnt] += EXEC_OFFSET;
          hdr[cnt] = copyStr(hdr[cnt]);
        }
        http->collectHeaders(hdr, p3);
        for (uint32_t cnt = 0; cnt < p3; cnt++) {
          free((void*)hdr[cnt]);
        }
      }
      break;
    case 41:
      {
      char *cp = copyStr((char*)p2);
      String hd = http->header(cp);
      free(cp);
      const char *sp = hd.c_str();
      uint16_t len = strlen(sp);
      if (len) {
        cp = (char*)malloc(len + 2);
        strlcpy(cp, sp, len + 2);
        return (uint32_t) cp;
      } else {
        return 0;
      }
      }
    case 42:
      {
        char *cp = copyStr((char*)p2);
        bool hd = http->hasHeader(cp);
        free(cp);
        return hd;
      }
    case 43:
      http->setFollowRedirects((followRedirects_t)p2);
      break;
    case 44:
      {
      //return http->begin(xclient, (char*)p3);
      }
      break;
    case 50:
      break;
    case 51:
      break;
    case 52:
      break;
    case 53:
      //Test_prog();
      break;
    case 60:
    { IPAddress ip;
      bool res = WifiHostByName((const char*)p2, ip);
      if (res == true) {
        String sres = ip.toString();
        strcpy((char *)p3, sres.c_str());
      } else {
        *(char*)p3 = 0;
      }
      return res;
    }

    case 70:
      {
      IPAddress *ip_addr = (IPAddress *)p1;
      ip_addr->fromString((char*)p2);
      }
      break;
    case 71:
      {
      IPAddress *ip_addr = (IPAddress *)p2;
      strcpy((char*)p1, ip_addr->toString().c_str());
      }
      break;
    case 72:
      global_pcnt = (PLUGIN_COUNTER*)p1;
      attachInterruptArg(p2, Plugin_CounterIsr, (void*)p3, p4);
      break;
    case 73:
      detachInterrupt(p1);
      break;
      
    case 80:
      {  
        ESP8266WebServer * ws = new ESP8266WebServer(p1);
        return (uint32_t) ws;
      }
    case 81:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
#if defined(ESP8266) || defined(__riscv)
        ws->on((const char*)p2, (void(*)(void))p3);
#else
        char *fcopy = copyStr((char*)p2);
        ws->on((const char*)fcopy, (void(*)(void))p3);
        //AddLog(LOG_LEVEL_INFO,PSTR("I2S Init %s - %x"), fcopy, (uint32_t)p3);
        free(fcopy);
#endif
      }
      break;
    case 82:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->begin();
        break;
      }
    case 83:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->stop();
        break;
      }
      break;
    case 84:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        WiFiClient *ws_client = new WiFiClient;
        *(ws_client) = ws->client();
        return (uint32_t)ws_client;
      }
    case 85:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->handleClient();
        break;
      }
    case 86:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        delete ws;
        break;
      }
    case 87:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
#if defined(ESP8266) || defined(__riscv)
        ws->client().print((char*)p2);
#else
        char *fcopy = copyStr((char*)p2);
        ws->client().print(fcopy);
        free(fcopy);
#endif
        break;
      }
    case 88:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        return ws->client().write((char*)p2, p3);
      }
    case 89:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->client().stop();
        break;
      }
    case 90:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        return ws->client().connected();
      }
    case 91:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->client().flush();
        break;
      }
    case 92:
      {
        ESP8266WebServer * ws = (ESP8266WebServer *)p1;
        ws->client().setTimeout(p2);
        break;
      }

    default:
      return 0;
  }
  return 0;
}

//#define I2S_DEBUG

#ifdef ESP8266
#include <i2s.h>
#endif

#ifdef ESP32
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#endif // ESP32

uint32_t tmod_i2s(uint32_t sel, uint32_t p1) {
I2S_PARS *i2cp = (I2S_PARS *) p1;

uint8_t xmode = sel >> 8;
sel &= 0xff;

#ifdef ESP32
  if ((sel > 0) && !i2cp->txhandle) {
    return 0;
  }
  i2s_chan_handle_t chn_handle;

  if (xmode) {
    chn_handle = (i2s_chan_handle_t)i2cp->rxhandle;
  } else {
    chn_handle = (i2s_chan_handle_t)i2cp->txhandle;
  }
#endif

  switch (sel) {
    case 0:
#ifdef ESP8266
      i2s_begin();
      return 0;
#endif
#ifdef ESP32
      {
#ifdef USE_WEBCAM        
      i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
#else
      i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
#endif
// Allocate channels
      chan_cfg.auto_clear = true;
      if (i2cp->pdm_clk >= 0) {
        // PDM mic needs separate I2S port - TX and RX on different ports
        i2s_new_channel(&chan_cfg, (i2s_chan_handle_t*)&i2cp->txhandle, nullptr);
        if (i2cp->din >= 0) {
          i2s_chan_config_t rx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
          rx_chan_cfg.auto_clear = true;
          i2s_new_channel(&rx_chan_cfg, nullptr, (i2s_chan_handle_t*)&i2cp->rxhandle);
        }
      } else if (i2cp->din >= 0) {
        // STD mode - TX and RX can share same port (duplex)
        i2s_new_channel(&chan_cfg, (i2s_chan_handle_t*)&i2cp->txhandle, (i2s_chan_handle_t*)&i2cp->rxhandle);
      } else {
        // TX only
        i2s_new_channel(&chan_cfg, (i2s_chan_handle_t*)&i2cp->txhandle, nullptr);
      }

      i2s_slot_mode_t channels;
      if (i2cp->channels == 1) {
        channels = I2S_SLOT_MODE_MONO;
      } else {
        channels = I2S_SLOT_MODE_STEREO;
      }

      if (i2cp->pdm_clk < 0) {
        // non PDM channel - STD mode for both TX and RX
        i2s_std_config_t std_cfg = {
          .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(8000),
          .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
          .gpio_cfg = {
            .mclk = (gpio_num_t)i2cp->mclk,
            .bclk = (gpio_num_t)i2cp->bclk,
            .ws = (gpio_num_t)i2cp->ws,
            .dout = (gpio_num_t)i2cp->dout,
            .din = (gpio_num_t)i2cp->din,
            .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
            },
          },
        };
        uint8_t mode = i2cp->bmode & 3;
        if (mode > 2) mode = 2;
          switch (mode) {
            case 0:
              std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
              break;
            case 1:
              std_cfg.slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
              break;
            case 2:
              std_cfg.slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
              break;
          }
          // Initialize the channels
          i2s_channel_init_std_mode((i2s_chan_handle_t)i2cp->txhandle, &std_cfg);
          i2s_channel_enable((i2s_chan_handle_t)i2cp->txhandle);
          if (i2cp->rxhandle) {
            i2s_channel_init_std_mode((i2s_chan_handle_t)i2cp->rxhandle, &std_cfg);
            i2s_channel_enable((i2s_chan_handle_t)i2cp->rxhandle);
          }
      } else {
        // PDM microphone - TX stays STD, RX uses PDM mode on separate channel
#if SOC_I2S_SUPPORTS_PDM_RX
        // Init TX channel in STD mode (speaker)
        i2s_std_config_t std_cfg = {
          .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(8000),
          .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
          .gpio_cfg = {
            .mclk = (gpio_num_t)i2cp->mclk,
            .bclk = (gpio_num_t)i2cp->bclk,
            .ws = (gpio_num_t)i2cp->ws,
            .dout = (gpio_num_t)i2cp->dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
              .mclk_inv = false,
              .bclk_inv = false,
              .ws_inv = false,
            },
          },
        };
        i2s_channel_init_std_mode((i2s_chan_handle_t)i2cp->txhandle, &std_cfg);
        i2s_channel_enable((i2s_chan_handle_t)i2cp->txhandle);

        // Init RX channel in PDM mode (microphone) on separate port
        if (i2cp->rxhandle) {
          i2s_pdm_rx_config_t pdm_rx_cfg = {
            .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
            .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
            .gpio_cfg = {
              .clk = (gpio_num_t)i2cp->pdm_clk,
              .din = (gpio_num_t)i2cp->din,
              .invert_flags = {
                .clk_inv = false,
              },
            },
          };
          i2s_channel_init_pdm_rx_mode((i2s_chan_handle_t)i2cp->rxhandle, &pdm_rx_cfg);
          i2s_channel_enable((i2s_chan_handle_t)i2cp->rxhandle);
        }
#endif
      }

#ifdef I2S_DEBUG
      AddLog(LOG_LEVEL_INFO,PSTR("I2S Init %x - %x"), (uint32_t)i2cp->txhandle, (uint32_t)i2cp->rxhandle);
#endif
      return 0;
      }
      
#endif
      break;
    case 1:
#ifdef ESP8266
      i2s_end();
#endif
#ifdef ESP32
      {
      i2s_channel_disable(chn_handle);
      i2s_del_channel(chn_handle);
      //AddLog(LOG_LEVEL_INFO,PSTR("I2S Exit"));
      }
#endif
      break;
    case 2:
#ifdef ESP8266
      i2s_set_rate(i2cp->dlen);
#endif
#ifdef ESP32
      {
      i2s_channel_disable(chn_handle);

#ifdef I2S_DEBUG
      AddLog(LOG_LEVEL_INFO,PSTR("I2S freq %d"), i2cp->dlen);
#endif
      i2s_std_clk_config_t clk_cfg;
      i2s_std_slot_config_t slot_cfg;

      i2s_slot_mode_t channels;
      if (1 == i2cp->channels) {
        channels = I2S_SLOT_MODE_MONO;
      }   else {
        channels = I2S_SLOT_MODE_STEREO;
      }

      if (i2cp->pdm_clk >= 0 && xmode) {
        // PDM RX channel reconfig
#if SOC_I2S_SUPPORTS_PDM_RX
        i2s_pdm_rx_clk_config_t pdm_clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(i2cp->dlen);
        i2s_channel_reconfig_pdm_rx_clock(chn_handle, &pdm_clk_cfg);
        i2s_pdm_rx_slot_config_t pdm_slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
        i2s_channel_reconfig_pdm_rx_slot(chn_handle, &pdm_slot_cfg);
#endif
      } else {
        // STD channel reconfig (TX always, RX when not PDM)
        clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(i2cp->dlen);
        i2s_channel_reconfig_std_clock(chn_handle, &clk_cfg);
        uint8_t mode = i2cp->bmode & 3;
        if (mode > 2) mode = 2;
        switch (mode) {
          case 0:
            slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
            break;
          case 1:
            slot_cfg = I2S_STD_PCM_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
            break;
          case 2:
            slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels);
            break;
        }
        i2s_channel_reconfig_std_slot(chn_handle, &slot_cfg);
      }

      

      i2s_channel_enable(chn_handle);
      //AddLog(LOG_LEVEL_INFO,PSTR("I2S Setrate %d"), p2);
      }
#endif
      break;
    case 3:
      // write samples
#ifdef ESP8266
      { 
#if 0
        int16_t *left = (int16_t*)p2;
        int16_t *right = (int16_t*)p2 + 2;
        for (uint32_t cnt = 0; cnt < (p3 >> 2); cnt++) {
          i2s_write_lr(*left++, *right++);
        }
        *(uint32_t*)p4 = p3;
#endif
        int16_t *swp = (int16_t*)i2cp->dptr;
        for (uint32_t cnt = 0; cnt < i2cp->dlen; cnt++) {
          i2s_write_sample(*swp++);
        }
      }
#endif 
#ifdef ESP32
      size_t bytes_written;
      i2cp->error = i2s_channel_write(chn_handle, (uint8_t *)i2cp->dptr, i2cp->dlen, &bytes_written, i2cp->timeout);
      return bytes_written;
#endif
      break;
    case 4:
      // read samples
#ifdef ESP8266
      //return i2s_read_sample((int16_t *)i2cp->dptr, i2cp->dlen, 0);
      return 0; 
#endif

#ifdef ESP32
      {
        size_t bytes_read;
        i2cp->error = i2s_channel_read(chn_handle, (void*)i2cp->dptr, i2cp->dlen, &bytes_read, i2cp->timeout);
        return bytes_read;
      }
#endif
      break;
    case 5:
      // write one sample
#ifdef ESP8266
      i2s_write_sample(*i2cp->dptr);
#endif // ESP8266
#ifdef ESP32
      {
        i2s_channel_write(chn_handle, i2cp->dptr, 2, nullptr, 5);
      }
      break;
#endif // ESP32
    case 6:
#ifdef ESP32
      return i2s_channel_enable(chn_handle);
#endif
      break;
#ifdef ESP32
    case 7:
#if 0     
    does not work ???
      uint8_t zero_buffer[240] = {0};
      for (uint32_t i = 0; i < 6; i++) {
        i2s_channel_write(chn_handle, zero_buffer, sizeof(zero_buffer), nullptr, 5); // fill DMA buffer with silence
      }
#endif
      return i2s_channel_disable(chn_handle);
    case 8:
      return i2s_channel_register_event_callback(chn_handle, (i2s_event_callbacks_t*)i2cp->cbp, 0);

 #endif
  }
  return 0;
}

uint32_t tmod_jpeg_picture(uint32_t mem, uint32_t jpgsize, uint32_t xp, uint32_t yp, uint32_t scale) {
#if  defined(ESP32) && defined(JPEG_PICTS)
  Draw_jpeg((uint8_t*)mem, jpgsize, xp, yp, scale);
#endif
  return 0;
}

// layer3.h only exists in lib/lib_audio/mp3_shine_esp32 (ESP32-only).
// ESP8266 builds that still define USE_SHINE via user_config_override.h must not pull it in.
#if defined(USE_SHINE) && defined(ESP32)
#include <layer3.h>
#endif

// math function wrappers for plugin jump table
float tmod_sinf(float a) { return sinf(a); }
float tmod_cosf(float a) { return cosf(a); }
float tmod_logf(float a) { return logf(a); }
float tmod_sqrtf(float a) { return sqrtf(a); }

// shine mpeg3 encoder about 31kB code
uint32_t tmod_shine(uint32_t sel, uint32_t p1, uint32_t p2, uint32_t p3) {
#if defined(USE_SHINE) && defined(ESP32)
  switch (sel) {
    case 0:
      return (uint32_t)shine_initialise((shine_config_t*)p1);
    case 1:
      shine_set_config_mpeg_defaults((shine_mpeg_t*)p1);
      return 0;
    case 2:
      //shine_check_config(config.wave.samplerate, config.mpeg.bitr) 
      return (uint32_t)shine_check_config(p1, p2);
    case 3:
      return (uint32_t)shine_samples_per_pass((shine_t)p1);
    case 4:
      //return shine_encode_buffer_interleaved(s, buffer, &written);
      return (uint32_t)shine_encode_buffer_interleaved((shine_t)p1, (int16_t*)p2, (int*)p3);
    case 5:
      //shine_flush(s, &written);
      return (uint32_t)shine_flush((shine_t)p1, (int*)p2);
    case 6:
      shine_close((shine_t)p1);
      return 0;
  }
#endif
  return 0;
}



uint32_t tmod_wc(uint32_t sel, int32_t p1, int32_t p2) {
#if defined(ESP32) && defined(USE_WEBCAM)
  switch (sel) {
    case 0:
      return WcSetup(p1);
    case 1:
      return WcGetFrame(p1);
    case 2:
      return WcSetOptions(p1, p2);
    case 3:
      return WcGetWidth();
    case 4:
      return WcGetHeight();
    case 5:
      return WcSetStreamserver(p1);
    case 6:
      return WcGetPicstore(p1, (uint8_t **)p2);
  }
#endif
  return 0;
}


uint32_t tmod_udp(WiFiUDP *udp, uint32_t sel, uint32_t p1, uint32_t p2) {
  if (sel > 0 && !udp) {
    return 0;
  }
  switch (sel & 0xff) {
    case 0:
      udp = new WiFiUDP;
      return (uint32_t)udp;
    case 1:
      udp->stop();
      break;
    case 2:
      return udp->begin(p1);
    case 3:
      return udp->parsePacket();
    case 4:
      return udp->available();
    case 5:
      return udp->read((uint8_t *)p1, p2);
    case 6:
      udp->flush();
      break;
    case 7:
      return udp->beginPacket(p1, p2);
    case 8:
      return udp->write((const uint8_t*)p1, p2);
    case 9:
      return udp->endPacket();
    case 10:
      return udp->remoteIP();
    case 99:
      udp->stop();
      delete udp;
      break;
  }
  return 0;
}

#ifdef ESP32
#include <can.h>
#include "driver/twai.h"
#endif


uint32_t tmod_serialdispatch(uint32_t sel, uint32_t p1, uint32_t p2, uint32_t p3) {
  TasmotaSerial *ts = (TasmotaSerial*)p1;
 #ifdef ESP32
  PLUGIN_ESP32_SERIAL *ps = (PLUGIN_ESP32_SERIAL*)p1;
 #endif
  switch (sel) {
    case 0:
      {
      TSPARS *spars = (TSPARS*) p2; 
      ts = new TasmotaSerial(spars->rxpin, spars->txpin, spars->hwfb, spars->nwmode, spars->bsize, spars->invert);
      return (uint32_t)ts;
      }
      break;
    case 1:
      ts->end();
    case 2:
      delete ts;
      break;
    case 3:
      return ts->begin(p2, p3);
    case 4:
      return ts->available();
    case 5:
      return ts->peek();
    case 6:
      return ts->read();
    case 7:
      {
        uint8_t *p = (uint8_t*)p2;
        for (uint32_t cnt = 0; cnt < p3; cnt++) {
          ts->write(*p++);
        }
      }
      break;
    case 8:
      ts->flush();
      break;
    case 9:
      return ts->hardwareSerial();

#ifdef ESP32
    case 20:
      ps = new PLUGIN_ESP32_SERIAL(p2);
      return (uint32_t)ps;
    case 21:
      ps->end();
      break;
    case 22:
      delete ps;
      break;
    case 23:
      {
      TSPARS *spars = (TSPARS*) p2; 
      // bool begin(uint32_t speed, uint32_t smode, int32_t recpin, int32_t trxpin, int32_t invert);
      return ps->begin(spars->speed, spars->nwmode, spars->rxpin, spars->txpin, spars->invert);
      }
    case 24:
      return ps->available();
    case 25:
      return ps->peek();
    case 26:
      return ps->read();
    case 27:
      {
        uint8_t *p = (uint8_t*)p2;
        for (uint32_t cnt = 0; cnt < p3; cnt++) {
          ps->write(*p++);
        }
      }
      break;
    case 28:
      ps->flush();
      break;
    case 29:
      ps->updateBaudRate(p2);
      break;   
    case 30:
      ps->setRxBufferSize(p2);
      break;

    case 50:
      return SOC_UART_HP_NUM;
    case 51:
      gpio_pullup_dis((gpio_num_t)p1);
      break;
#endif

#ifdef ESP32
    case 70:
      return twai_driver_install((twai_general_config_t *)p1, (twai_timing_config_t*)p2, (twai_filter_config_t*)p3);
    case 71:
      return twai_driver_uninstall();
    case 72:
      return twai_start();
    case 73:
      return twai_stop();
    case 74:
      return twai_reconfigure_alerts(p1, (uint32_t*)p2);
    case 75:
      return twai_get_status_info((twai_status_info_t*)p1);
    case 76:
      return twai_receive((twai_message_t*)p1, (TickType_t)p2);
    case 77:
      return twai_transmit((twai_message_t*)p1, (TickType_t)p2);
    case 78:
      return twai_read_alerts((uint32_t*)p1, (TickType_t)p2);
    case 79:
      return twai_clear_receive_queue();
#endif

    case 100:
      return ValidPin(p1, p2);

    
  }
  return 0;
}

void *tmod_special_malloc(uint32_t size) {
  void *ptr = special_malloc(size);
  memset(ptr, 0, size);
  return ptr;
};


int tmod_strncmp_P(const char * str1P, const char * str2P, size_t size) {
  char *cp = copyStr(str2P);
  int res = strncmp(str1P, cp, size);
  free(cp);
  return res;
}

extern FS *ufsp;

uint32_t tmod_file_exists(const char *path) {
  int32_t result = 0;
#ifdef USE_UFILESYS
  char *cpath = copyStr(path);
#ifdef USE_SCRIPT  
  FS *cfp = script_file_path(cpath);
#else
  FS *cfp = ufsp;
#endif
  result = cfp->exists(cpath);
  free(cpath);
#endif // USE_UFILESYS
  return result;
}


void tmod_AddLogData(uint32_t loglevel, const char* log_data) {
  AddLogData(loglevel,log_data);
}

void *tmod_strncat(char *dst, char *src, uint32_t size) {
  void *res;
  char *cp = copyStr(src);
  res = strncat(dst, cp, size);
  free(cp);
  return res;
}

static File temp_file;

class File *tmod_file_open(char *path, char mode) {
#ifdef USE_UFILESYS
  char *cpath = copyStr(path);
#ifdef USE_SCRIPT  
  FS *cfp = script_file_path(cpath);
#else
  FS *cfp = ufsp;
#endif
  switch (mode) {
    case 'r':
      temp_file = cfp->open(cpath, FS_FILE_READ);
      break;
    case 'w':
      temp_file = cfp->open(cpath, FS_FILE_WRITE);
      break;
    case 'a':
      temp_file = cfp->open(cpath, FS_FILE_APPEND);
      break;
    case 'u':
      temp_file = cfp->open(cpath, "w+");
      break;
    case 'U':
      temp_file = cfp->open(cpath, "r+");
      break;
  }
  free(cpath);
  if (temp_file > 0) {
    return &temp_file;
  } else {
    return nullptr;
  }
#else
  return nullptr;
#endif // USE_UFILESYS
}

void tmod_file_close(class File *fp) {
#ifdef USE_UFILESYS
  fp->close();
#endif
}

int32_t tmod_file_seek(class File *fp, uint32_t pos, uint32_t mode) {
#ifdef USE_UFILESYS
  return fp->seek(pos, (fs::SeekMode)mode);
#else
  return 0;
#endif
}
int32_t tmod_file_read(class File *fp, uint8_t *buff, uint32_t size) {
#ifdef USE_UFILESYS
  return fp->read(buff, size);
#else
  return 0;
#endif
}
int32_t tmod_file_write(class File *fp, uint8_t *buff, uint32_t size) {
#ifdef USE_UFILESYS
  return fp->write(buff, size);
#else
  return 0;
#endif
}

uint32_t tmod_file_size(class File *fp) {
#ifdef USE_UFILESYS
  return fp->size();
#else
  return 0;
#endif
}

uint32_t tmod_file_pos(class File *fp) {
#ifdef USE_UFILESYS
  return fp->position();
#else
  return 0;
#endif
}

#ifdef USE_SPI
SPIClass *tmod_getspi(uint8_t sel) {
  if (!sel) {
    return &SPI;
  } else {
#ifdef ESP32
    return &SPI;
    //return &SPI1;
#else
    return &SPI;
#endif    
  }
}

void tmod_spi_begin(SPIClass *spi, uint8_t flg, int8_t sck, int8_t miso, int8_t mosi) {
#ifdef ESP32 
  if (!flg) {
    if (sck < 0) {
      sck = Pin(GPIO_SPI_CLK);
    }
    if (miso < 0) {
      miso = Pin(GPIO_SPI_MISO);
    }
    if (mosi < 0) {
      mosi = Pin(GPIO_SPI_MOSI);
    }
    spi->begin(sck, miso, mosi, -1);
  } else {
    spi->end();
  }
#else
  if (!flg) {
    spi->begin();
  } else {
    spi->end();
  }
#endif
}

void tmod_spi_write(SPIClass *spi, uint8_t data) {
  spi->write(data);
}

void tmod_spi_writebytes(SPIClass *spi, const uint8_t * data, uint32_t size) {
  spi->writeBytes(data, size);
}

void tmod_Transaction(SPIClass *spi, uint8_t flg, uint32_t spibaud) {
  if (!flg) {
    SPISettings settings = SPISettings(spibaud, MSBFIRST, SPI_MODE0);
    spi->beginTransaction(settings);
  } else {
    spi->endTransaction();
  }
}

uint8_t tmod_transfer(SPIClass *spi, uint8_t data) {
  return spi->transfer(data);
}
#endif


void tmod_WebServer_on(const char * prefix, void (*func)(void), uint8_t method) {
#if defined(ESP8266) || defined(__riscv)
  WebServer_on(prefix, func, method);
#else
  char *fcopy = copyStr(prefix);
  WebServer_on(fcopy, func, method);
  free(fcopy);
#endif
}


char * tmod_strncpy_P(char *dst , const char *src, size_t len)  {
  char *out = 0;
#ifdef ESP32
  char *fcopy = copyStr(src);
  out = strncpy_P(dst, fcopy, len);
  free(fcopy);
#endif
  return out;
}

char * tmod_strcpy_P(char *dst , const char *src)  {
#if defined(ESP8266) || defined(__riscv)
  return strcpy_P(dst, src);
#else
  char *fcopy = copyStr(src);
  char *out = strcpy_P(dst, fcopy);
  free(fcopy);
  return out;
#endif
} 

bool WebServer_hasArg(const char * str) {
  //return Webserver->hasArg(str);
  char *fcopy = copyStr(str);
  bool out = Webserver->hasArg(fcopy);
  free(fcopy);
  return out;
}

void tmod_WSContentStart_P(const char* title) {
#if defined(ESP8266) || defined(__riscv)
   WSContentStart_P(title);
#else
  char *fcopy = copyStr(title);
  WSContentStart_P(fcopy);
  free(fcopy);
#endif
}

void tmod_vTaskEnterCritical( void *mux ) {
#ifdef ESP32
  *(portMUX_TYPE*)mux = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL((portMUX_TYPE*)mux);
#endif
}

void tmod_vTaskExitCritical( void *mux ) {
#ifdef ESP32
  portEXIT_CRITICAL((portMUX_TYPE*)mux);
#endif
}


#ifdef ESP32
#include <driver/rtc_io.h>
#endif

#if ESP_IDF_VERSION_MAJOR >= 5
#include "soc/gpio_periph.h"
#endif // ESP_IDF_VERSION_MAJOR >= 5

/* esp8266
#define DIRECT_READ(base, mask)         ((GPI & (mask)) ? 1 : 0)    //GPIO_IN_ADDRESS
#define DIRECT_MODE_INPUT(base, mask)   (GPE &= ~(mask))            //GPIO_ENABLE_W1TC_ADDRESS
#define DIRECT_MODE_OUTPUT(base, mask)  (GPE |= (mask))             //GPIO_ENABLE_W1TS_ADDRESS
#define DIRECT_WRITE_LOW(base, mask)    (GPOC = (mask))             //GPIO_OUT_W1TC_ADDRESS
#define DIRECT_WRITE_HIGH(base, mask)   (GPOS = (mask))             //GPIO_OUT_W1TS_ADDRESS
*/

uint32_t tmod_directRead(uint32_t pin) {

#ifdef ESP32
//    return digitalRead(pin);               // Works most of the time
//    return gpio_ll_get_level(&GPIO, pin);  // The hal is not public api, don't use in application code
//#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#if SOC_GPIO_PIN_COUNT <= 32
    return (GPIO.in.val >> pin) & 0x1;
#else  // ESP32 with over 32 gpios
    if ( pin < 32 )
        return (GPIO.in >> pin) & 0x1;
    else
        return (GPIO.in1.val >> (pin - 32)) & 0x1;
#endif
#endif
#ifdef ESP8266
  return digitalRead(pin);
#endif
}


void tmod_directWriteLow(uint32_t pin) {
    //digitalWrite(pin, 0);                  // Works most of the time
    //return;
//    gpio_ll_set_level(&GPIO, pin, 0);      // The hal is not public api, don't use in application code
#ifdef ESP32
//#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#if SOC_GPIO_PIN_COUNT <= 32
    GPIO.out_w1tc.val = ((uint32_t)1 << pin);
#else  // ESP32 with over 32 gpios
    if ( pin < 32 )
        GPIO.out_w1tc = ((uint32_t)1 << pin);
    else
        GPIO.out1_w1tc.val = ((uint32_t)1 << (pin - 32));
#endif
#endif
#ifdef ESP8266
  digitalWrite(pin, LOW);
#endif
}

void tmod_directWriteHigh(uint32_t pin) {
    //digitalWrite(pin, 1);                  // Works most of the time
    //return;
//    gpio_ll_set_level(&GPIO, pin, 1);      // The hal is not public api, don't use in application code

#ifdef ESP32
//#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#if SOC_GPIO_PIN_COUNT <= 32
    GPIO.out_w1ts.val = ((uint32_t)1 << pin);
#else  // ESP32 with over 32 gpios
    if ( pin < 32 )
        GPIO.out_w1ts = ((uint32_t)1 << pin);
    else
        GPIO.out1_w1ts.val = ((uint32_t)1 << (pin - 32));
#endif
#endif
#ifdef ESP8266
  digitalWrite(pin, HIGH);
#endif
}

void tmod_directModeInput(uint32_t pin) {
   // pinMode(pin, INPUT);                   // Too slow - doesn't work
   // return;
//    gpio_ll_output_disable(&GPIO, pin);    // The hal is not public api, don't use in application code

#ifdef ESP32
    if ( digitalPinIsValid(pin) ) {
        // Input
//#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#if SOC_GPIO_PIN_COUNT <= 32
        GPIO.enable_w1tc.val = ((uint32_t)1 << (pin));
#else  // ESP32 with over 32 gpios
        if ( pin < 32 )
            GPIO.enable_w1tc = ((uint32_t)1 << pin);
        else
            GPIO.enable1_w1tc.val = ((uint32_t)1 << (pin - 32));
#endif
    }
#endif   
#ifdef ESP8266
  pinMode(pin, INPUT);
#endif
}


void tmod_directModeOutput(uint32_t pin) {
   // pinMode(pin, OUTPUT);                 // Too slow - doesn't work
  //return;
//    gpio_ll_output_enable(&GPIO, pin);    // The hal is not public api, don't use in application code
#ifdef ESP32
    if ( digitalPinCanOutput(pin) ) {
        // Output
//#if CONFIG_IDF_TARGET_ESP32C2 || CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6
#if SOC_GPIO_PIN_COUNT <= 32
        GPIO.enable_w1ts.val = ((uint32_t)1 << (pin));
#else  // ESP32 with over 32 gpios
        if ( pin < 32 )
            GPIO.enable_w1ts = ((uint32_t)1 << pin);
        else
            GPIO.enable1_w1ts.val = ((uint32_t)1 << (pin - 32));
#endif
    }
#endif
#ifdef ESP8266
  pinMode(pin, OUTPUT);
#endif
}


char *tm_trim(char *s) {
    char *ptr;
    if (!s)
        return NULL;   // handle NULL string
    if (!*s)
        return s;      // handle empty string
    for (ptr = s + strlen(s) - 1; (ptr >= s) && isspace(*ptr); --ptr);
    ptr[1] = '\0';
    return s;
}

#ifdef ESP32
void twi_readFrom(uint8_t address, uint8_t* data, uint8_t length) {
  Wire.requestFrom(address, (size_t)length, (bool)true);
  Wire.readBytes(data, length);
}
#endif  // ESP32


float fl_const(int32_t m, int32_t d) {
  if (d == 0 ) return 0;
  return (float)m / (float)d;
}

int tmod_GetCommandCode(char* destination, size_t destination_size, const char* needle, const char* haystack) {
  char *cph = copyStr(haystack);
  int res = GetCommandCode(destination, destination_size, needle, cph);
  free(cph);
  return res;
}

// modified decode command, no synonyms
bool MT_DecodeCommand(const char* haystack, void (* const MyCommand[])(void), MODULES_TABLE *mt) {

  haystack += EXEC_OFFSET;

#ifdef ESP32
  char *cph = copyStr(haystack);
  if (!cph) {
    return false;
  }
#else
  const char *cph = haystack;
#endif

  const uint8_t *synonyms = nullptr;
  GetTextIndexed(XdrvMailbox.command, CMDSZ, 0, cph);  // Get prefix if available

  int prefix_length = strlen(XdrvMailbox.command);
  if (prefix_length) {
    char prefix[prefix_length + 1];
    snprintf_P(prefix, sizeof(prefix), XdrvMailbox.topic);  // Copy prefix part only
    if (strcasecmp(prefix, XdrvMailbox.command)) {
#ifdef ESP32
      free(cph);
#endif
      return false;                                         // Prefix not in command
    }
  }
  size_t syn_count = synonyms ? pgm_read_byte(synonyms) : 0;
  int command_code = GetCommandCode(XdrvMailbox.command + prefix_length, CMDSZ, XdrvMailbox.topic + prefix_length, cph);
  if (command_code > 0) {                                   // Skip prefix
    if (command_code > syn_count) {
      // We passed the synonyms zone, it's a regular command
      XdrvMailbox.command_code = command_code - 1 - syn_count;
      uint32_t *lp = (uint32_t*)MyCommand;
      lp += EXEC_OFFSET / 4;
      uint32_t lval = lp[XdrvMailbox.command_code];
      lval += EXEC_OFFSET;
      void (*Command)(void) = (void (*)(void))lval;
      Command();

      //MyCommand[XdrvMailbox.command_code]();
    } else {
      // We have a SetOption synonym
      XdrvMailbox.index = pgm_read_byte(synonyms + command_code);
      CmndSetoptionBase(0);
    }
#ifdef ESP32
    free(cph);
#endif
    return true;
  }
#ifdef ESP32
  free(cph);
#endif
  return false;
}


void tmod_memmove_P(void *dst, const void *src, size_t size) {
  uint32_t buff[size/4 + 1];
  uint32_t *lp = (uint32_t*) src;
  for (uint32_t cnt = 0; cnt < size / 4 + 1; cnt++) {
    buff[cnt] = *lp++;
  }
  memmove(dst, buff, size);
}

char * tmod_GetTextIndexed(char* destination, size_t destination_size, uint32_t index, const char* haystack) {
  char *sx = copyStr(haystack);
  char *retval = GetTextIndexed(destination, destination_size, index, sx);
  free(sx);
  return retval;
}

int tmod_strncasecmp_P(const char *s1, const char *s2, size_t len) {
#ifdef ESP8266
  return strncasecmp_P(s1, s2, len);
#endif
#ifdef ESP32
  char *sx = copyStr(s2);
  int res = strncasecmp_P(s1, sx, len);
  free(sx);
  return res;
#endif

}

int tmod_ResponseTime_P(const char* format, ...)    // Content send snprintf_P char data
{

#ifdef ESP32
  // This uses char strings. Be aware of sending %% if % is needed
  char timestr[100];
  TasmotaGlobal.mqtt_data = ResponseGetTime(Settings->flag2.time_format, timestr);

  char *fcopy = copyStr(format);
  va_list arg;
  va_start(arg, format);
  char* mqtt_data = ext_vsnprintf_malloc_P(fcopy, arg);
  va_end(arg);
  if (mqtt_data != nullptr) {
    TasmotaGlobal.mqtt_data += mqtt_data;
    free(mqtt_data);
  }
  free(fcopy);
#endif
  return TasmotaGlobal.mqtt_data.length();
}

int tmod_snprintf_P(char *str, size_t strSize,  const char *format, ...) {
int res = 0;
#ifdef ESP32
  char *fcopy = copyStr(format);
  va_list arglist;
  va_start(arglist, format);
  res = vsnprintf_P(str, strSize, fcopy, arglist);
  va_end(arglist);
  free(fcopy);
#endif
  return res;
}

#define SIZE_IRRELEVANT 0x7fffffff

int tmod_sprintf_P(char *str, const char *format, ...) {
int res = 0;
#ifdef ESP32
  char *fcopy = copyStr(format);
  va_list arglist;
  va_start(arglist, format);
  res = vsnprintf_P(str, SIZE_IRRELEVANT, fcopy, arglist);
  va_end(arglist);
  free(fcopy);
#endif
  return res;
}

/*
int tmod_ResponseAppend_P(const char* format, ...) {
  int res = 0;
#ifdef ESP32
  char *fcopy = copyStr(format);
   // This uses char strings. Be aware of sending %% if % is needed
  va_list args;
  va_start(args, format);
  int mlen = ResponseLength();
  int len = ext_vsnprintf_P((char*)TasmotaGlobal.mqtt_data.c_str() + mlen, ResponseSize() - mlen, fcopy, args);
  va_end(args);
  res = len + mlen;
  free(fcopy);
#endif
  return res;
}
*/

int tmod_ResponseAppend_P(const char* format, ...)  // Content send snprintf_P char data
{
#ifdef ESP32
  // This uses char strings. Be aware of sending %% if % is needed
  char *fcopy = copyStr(format);
  va_list arg;
  va_start(arg, format);
  char* mqtt_data = ext_vsnprintf_malloc_P(fcopy, arg);
  va_end(arg);
  if (mqtt_data != nullptr) {
    TasmotaGlobal.mqtt_data += mqtt_data;
    free(mqtt_data);
  }
  free(fcopy);
#endif
  return TasmotaGlobal.mqtt_data.length();
}


void tmod_WebGetArg(const char* arg, char* out, size_t max) {
#if defined(ESP8266) || defined(__riscv)
  WebGetArg(arg, out, max);
#else
  char *fcopy = copyStr(arg);
  WebGetArg(fcopy, out, max);
  String s = Webserver->arg(fcopy);
  strlcpy(out, s.c_str(), max);
  //AddLog(LOG_LEVEL_INFO,PSTR(">>> %s - %s"), fcopy, out);
  free(fcopy);
#endif
}


void tmod_WSContentSend_PD(const char* format, ...) {
#ifdef ESP32
  char *fcopy = copyStr(format);
  va_list arg;
  va_start(arg, format);
  _WSContentSendBuffer(true, fcopy, arg);
  va_end(arg);
  free(fcopy);
#endif
}

void tmod_WSContentSend_P(const char* format, ...) {
#ifdef ESP32
  char *fcopy = copyStr(format);
  //WSContentSend_P(fcopy, va);
  va_list arg;
  va_start(arg, format);
  _WSContentSendBuffer(false, fcopy, arg);
  va_end(arg);
  free(fcopy);
#endif
}



void AddlogT(char* txt) {
   AddLog(LOG_LEVEL_INFO ,PSTR("%s"), txt);
}
// some helper functions
void tmod_beginTransmission(TwoWire *wp, uint8_t addr) {
  wp->beginTransmission(addr);
}
void tmod_write(TwoWire *wp, uint8_t val) {
  wp->write(val);
}

void tmod_writen(TwoWire *wp, uint8_t *buf, uint32_t len) {
  wp->write(buf, len);
}

uint8_t tmod_endTransmission(TwoWire *wp, bool flag) {
  return wp->endTransmission(flag);
}
size_t tmod_requestFrom(TwoWire *wp, uint8_t addr, uint8_t num) {
  return wp->requestFrom(addr, num);
}

bool tmod_I2cSetDevice(uint32_t addr, uint32_t bus) {
  return I2cSetDevice(addr, bus);
}

void tmod_I2cSetActiveFound(uint32_t addr, const char *types, uint32_t bus) {
#ifdef ESP8266
  I2cSetActiveFound(addr, types, bus);
#else
  char *cp = copyStr(types);
  I2cSetActiveFound(addr, cp, bus);
  free(cp);
#endif
}

void tmod_setClockStretchLimit(TwoWire *wp, uint32_t val) {
 #ifdef ESP8266 
  wp->setClockStretchLimit(val);
#endif
}


int tmod_read(TwoWire *wp) {
  return wp->read();
}

uint8_t tmod_available(TwoWire *wp) {
  return wp->available();
}

bool tmod_isnan(float val) {
  return isnan(val);
}

bool tmod_isinf(float val) {
  return isinf(val);
}

float tmod_NAN(void) {
  return NAN;
}

bool tmod_gtsf2(float p1, float p2) {
  return p1 > p2;
}
bool tmod_ltsf2(float p1, float p2) {
  return p1 < p2;
}
bool tmod_eqsf2(float p1, float p2) {
  return p1 == p2;
}

bool tmod_iseq(float val) {
  return val == 0.0;
}

float tmod__floatsisf(int32_t in) {
  return in;
}

float tmod__floatunsisf(uint32_t in) {
  return in;
}

uint32_t tmod__fixunssfsi(float in) {
  return in;
}

int32_t tmod_fixsfti(float in) {
  return in;
}


uint32_t tmod__udivsi3(uint32_t p1, uint32_t p2) {
  return p1 / p2;
}

uint32_t tmod__umodsi3(uint32_t p1, uint32_t p2) {
  return p1 % p2;
}



int32_t tmod__divsi3(int32_t p1, int32_t p2) {
  return p1 / p2;
}

int64_t tmod__muldi3(int64_t p1, int64_t p2) {
  return p1 * p2;
}

int32_t tmod__modsi3(int32_t p1, int32_t p2) {
  return p1 % p2;
}

int64_t tmod__ashldi3(int64_t p1, uint32_t p2) {
  return p1 << p2;
}

uint64_t tmod__lshrdi3(uint64_t p1, uint32_t p2) {
  return p1 >> p2;
}

float tmod_fdiv(float p1, float p2) {
  return p1 / p2;
}
float tmod_fmul(float p1, float p2) {
  return p1 * p2;
}

float tmod_fdiff(float p1, float p2) {
  return p1 - p2;
}

float tmod_fadd(float p1, float p2) {
  return p1 + p2;
}


float tmod_tofloat(uint64_t in) {
  return in;
}

int tmod_Pin(uint32_t pin, uint32_t index) {
  return Pin(pin, index);
}

TasmotaSerial *tmod_newTS(int32_t rpin, int32_t tpin) {
  TasmotaSerial *ts = new TasmotaSerial(rpin, tpin, 1);
  return ts;
}

int tmod_beginTS(TasmotaSerial *ts, uint32_t baud) {
  return ts->begin(baud);
}


void tmod_deleteTS(TasmotaSerial *ts) {
  delete(ts);
}

size_t tmod_writeTS(TasmotaSerial *ts, char *buf, uint32_t size) {
  return ts->write(buf, size);
}

size_t tmod_write1TS(TasmotaSerial *ts, uint8_t val) {
  return ts->write(val);
}

size_t tmod_readTS(TasmotaSerial *ts, char *buf, uint32_t size) {
  return ts->read(buf, size);
}

int tmod_read1TS(TasmotaSerial *ts) {
  return ts->read();
}

uint8_t tmod_availTS(TasmotaSerial *ts) {
  return ts->available();
}

void tmod_flushTS(TasmotaSerial *ts) {
  return ts->flush();
}

bool hardwareSerialTS(TasmotaSerial *ts) {
  return  ts->hardwareSerial();
}


const void * TGTAB[] PROGMEM = {
  &TasmotaGlobal.tele_period,
  &TasmotaGlobal.global_update,
  &TasmotaGlobal.temperature_celsius,
  &TasmotaGlobal.humidity,
  &TasmotaGlobal.uptime,
  &TasmotaGlobal.rel_inverted,
  &TasmotaGlobal.devices_present,
  &TasmotaGlobal.spi_enabled,
  &TasmotaGlobal.soft_spi_enabled,
  &RtcTime,
  &TasmotaGlobal.global_state,
  &TasmotaGlobal.gpio_pin,
  &RtcSettings,
};

void *tmod_gtbl(void) {
  return TGTAB;
}

// deprecated
uint32_t GetTasmotaGlobal(uint32_t sel) {
  switch (sel) {
    case tele_period:
      return TasmotaGlobal.tele_period;
      break;
    case global_update:
      return TasmotaGlobal.global_update;
      break;
    case humidity:
      return TasmotaGlobal.humidity;
      break;
    case uptime:
      return TasmotaGlobal.uptime;
      break;
    case rel_inverted:
      return TasmotaGlobal.rel_inverted;
      break;
    case devices_present:
      return TasmotaGlobal.devices_present;
      break;
  }
  return 0;
}

// deprecated
void SetTasmotaGlobal(uint32_t sel, uint32_t val) {
  switch (sel) {
    case rel_inverted:
      TasmotaGlobal.rel_inverted = val;
      break;
    case devices_present:
      TasmotaGlobal.devices_present = val;
      break;
  }
}

float GetTasmotaGlobalf(uint32_t sel) {
  return TasmotaGlobal.temperature_celsius;
}

void show_hex_address(uint32_t addr) {
  AddLog(LOG_LEVEL_INFO,PSTR(">>> %08x"), addr);
}

// convert float to string
char* ftostrfd(float number, unsigned char prec, char *s) {
  if ((isnan(number)) || (isinf(number))) {  // Fix for JSON output (https://stackoverflow.com/questions/1423081/json-left-out-infinity-and-nan-json-status-in-ecmascript)
    strcpy_P(s, PSTR("null"));
    return s;
  } else {
    return dtostrf(number, 1, prec, s);
  }
}

// scale a float number
float fscale(int32_t number, float mulfac, float subfac) {
  return (float)number * mulfac - subfac;
}

int32_t iscale(int32_t number, int32_t mulfac, int32_t divfac) {
  return (number * mulfac) / divfac;
}


const char plugin_sensor_names[] PROGMEM = 
D_TEMPERATURE "|"
D_PRESSURE "|"
D_HUMIDITY "|"
D_ABSOLUTE_HUMIDITY "|"
D_DISTANCE "|";


#define TYPESIZE 32
char *Plugin_Get_SensorNames(char *type, uint32_t index) {
  GetTextIndexed(type, TYPESIZE, index, plugin_sensor_names);
  return type;
}

/* ****************************** module handler ***********************************/

#define MOD_UPL_ERR_NONE 0
#define MOD_UPL_ERR_SYNC 1
#define MOD_UPL_ERR_ARCH 2
#define MOD_UPL_ERR_OLD 3
#define MOD_UPL_ERR_SLOTS 4
#define MOD_UPL_ERR_MEM 5

const char MOD_UPL_ERRMSG[] PROGMEM =
"OK|SYNC|ARCH|OLD|SLOTS|MEM";


uint8_t *Load_Module(char *path, uint32_t *rsize);
uint32_t Store_Module(uint8_t *fdesc, uint32_t size, uint32_t *offset, uint8_t flag, uint8_t index);

#ifndef MAX_PLUGINS
#define MAX_PLUGINS 8
#endif

#define SPEC_SCRIPT_FLASH 0x000F2000

#ifdef ESP8266
#undef FLASH_BASE_OFFSET
#define FLASH_BASE_OFFSET 0x40200000
#undef MODUL_END_OFFSET
#define MODUL_END_OFFSET 4
#else
#undef FLASH_BASE_OFFSET
#define FLASH_BASE_OFFSET 0x3F400000

#ifdef __riscv
#undef MODUL_END_OFFSET
#define MODUL_END_OFFSET 4
#else
#undef MODUL_END_OFFSET
#define MODUL_END_OFFSET 8
#endif
#endif

struct PLUGINS {
uint32_t free_flash_start;
uint32_t free_flash_end;
uint32_t flashbase;
uint32_t pagesize;
#ifdef ESP32
const esp_partition_t *flash_pptr;
spi_flash_mmap_handle_t map_handle;
#endif
uint8_t *module_input_buffer;
uint8_t *module_input_ptr;
uint16_t module_bytes_read;
uint16_t module_size;
char   mod_name[16];
uint8_t upload_error;
uint32_t eeprom_start_block;
uint8_t upload_slot;
uint8_t upload_start_block;
uint8_t upload_start_flag;

bool ready;
#ifdef EXECUTE_FROM_BINARY
uint16_t mod_size;
#endif
} plugins;



// 35 + 8 x MODULES_TABLE (18*8 = 144) = about 179 Bytes
MODULES_TABLE modules[MAX_PLUGINS];

#ifdef EXECUTE_FROM_BINARY
#undef Get_mod_size
#define Get_mod_size plugins.mod_size
#else
#undef Get_mod_size
#define Get_mod_size fm->size
#endif


#define MOD_EXEC(A)  fm->mod_func_execute(A)


#define ESP32_PLUGIN_HSIZE SPI_FLASH_SEC_SIZE

void Setplugins(void) {

#ifdef ESP8266
  plugins.free_flash_start = ESP_getSketchSize();
  plugins.free_flash_end = (ESP_getSketchSize() + ESP.getFreeSketchSpace());
  plugins.pagesize = SPI_FLASH_SEC_SIZE;
  plugins.flashbase = FLASH_BASE_OFFSET;
   // 00210000: 00400000: 400d758c:
  // align to sector start
  plugins.free_flash_start =  (plugins.free_flash_start + plugins.pagesize) & (plugins.pagesize-1^0xffffffff);
  plugins.free_flash_end   =  (plugins.free_flash_end + plugins.pagesize) & (plugins.pagesize-1^0xffffffff);
  plugins.ready = true;
#endif
#ifdef ESP32
  plugins.pagesize = SPI_FLASH_SEC_SIZE;
  plugins.flash_pptr = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, "custom");
  if (plugins.flash_pptr) {
    const void *out_ptr;
    //esp_err_t err = esp_partition_mmap(plugins.flash_pptr, 0, plugins.flash_pptr->size, SPI_FLASH_MMAP_DATA, &out_ptr, &plugins.map_handle);
#if ESP_IDF_VERSION_MAJOR < 5 
    esp_err_t err = esp_partition_mmap(plugins.flash_pptr, 0, plugins.flash_pptr->size, SPI_FLASH_MMAP_INST, &out_ptr, &plugins.map_handle);
#else
    esp_err_t err = esp_partition_mmap(plugins.flash_pptr, 0, plugins.flash_pptr->size, ESP_PARTITION_MMAP_INST, &out_ptr, &plugins.map_handle);
#endif
    plugins.free_flash_start = (uint32_t)out_ptr;
    plugins.free_flash_end = plugins.free_flash_start + plugins.flash_pptr->size;
    plugins.flashbase = 0;
    AddLog(LOG_LEVEL_INFO,PSTR("Plugins-> start: %08x, end: %08x"),plugins.free_flash_start, plugins.free_flash_end);
    plugins.ready = true;
  } else {
    plugins.ready = false;
    AddLog(LOG_LEVEL_INFO,PSTR("Plugins: Partition not found"));
  }
#endif

}

// scan for modules in flash and add to modules table
void InitModules(void) {

  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    modules[cnt].mod_addr = 0;
  }

  Setplugins();

  if (!plugins.ready) {
    return;
  }

  strlcpy(plugins.mod_name, module_name, sizeof(plugins.mod_name));

  uint32_t offset = 0;

//  const FLASH_MODULE *xfm = (FLASH_MODULE*)&module_header;
//  AddLog(LOG_LEVEL_INFO, PSTR("Module  %x: %x"), *(uint32_t*)corr_pc, *(uint32_t*)xfm->mod_func_execute);

#ifdef EXECUTE_FROM_BINARY
  // add one testmodule
  modules[0].mod_addr = (void *) &module_header;
//  AddLog(LOG_LEVEL_INFO, PSTR("Module %x: - %x: - %x:"),(uint32_t)modules[0].mod_addr,(uint32_t)&mod_func_execute,(uint32_t)&end_of_module);

  const FLASH_MODULE *fm = (FLASH_MODULE*)modules[0].mod_addr;
  modules[0].jt = MODULE_JUMPTABLE;
  //modules[0].execution_offset = offset;
#ifdef ESP8266
  plugins.mod_size = (uint32_t)fm->end_of_module - (uint32_t)modules[0].mod_addr + 4;
#else
  plugins.mod_size = (uint32_t)fm->end_of_module - (uint32_t)modules[0].mod_addr + 8;
#endif
  //modules[0].settings = Settings;

  modules[0].flags.data = 0;

  plugins.free_flash_start = (uint32_t)modules[0].mod_addr;
  plugins.free_flash_end = plugins.free_flash_start + SPI_FLASH_SEC_SIZE;
  plugins.pagesize = SPI_FLASH_SEC_SIZE;
  plugins.flashbase = 0;

#else
  AddModules();
#endif // EXECUTE_FROM_BINARY
}


void Module_Execute(uint32_t sel) {
  if (!plugins.ready) {
    return;
  }
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      if (modules[cnt].flags.initialized) {
        const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
        MOD_EXEC(sel);
      }
    }
  }
}

uint32_t Plugin_Query(uint16_t index, uint8_t sel, char *params) {
uint32_t result = 0;
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      if (modules[cnt].flags.initialized) {
        const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
        if (params) {
          char **ccp = (char**)modules[cnt].mod_memory;
          *ccp = params;
        }
        result = MOD_EXEC(FUNC_QUERY_LOW | (index << 16) | sel );
        if (result) {
          return result;
        }
      }
    }
  }
  return result;
}

bool Module_Command(uint32_t sel) {
bool result = false;
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      if (modules[cnt].flags.initialized) {
        const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
        result = MOD_EXEC(sel);
        if (result) break;
      }
    }
  }
  return result;
}

void ModuleWebSensor() {
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      if (modules[cnt].flags.initialized && modules[cnt].flags.web_sensor) {
        const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
        MOD_EXEC(pFUNC_WEB_SENSOR);
      }
    }
  }
}

void ModuleJsonAppend() {
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      if (modules[cnt].flags.initialized && modules[cnt].flags.json_append) {
        const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
        MOD_EXEC(pFUNC_JSON_APPEND);
      }
    }
  }
}

uint8_t *Load_Module(char *path, uint32_t *rsize) {

 #ifdef USE_UFILESYS
  if (!ffsp) return 0;
  File fp;
  fp = ffsp->open(path, "r");
  if (fp <= 0) return 0;
  uint32_t size = fp.size();
#ifdef ESP8266
  uint8_t *fdesc = (uint8_t *)calloc(size / 4 + 4, 4);
#endif
#ifdef ESP32
  //uint8_t *fdesc = (uint8_t *)heap_caps_malloc(size + 4, MALLOC_CAP_EXEC);
  uint8_t *fdesc = (uint8_t *)special_malloc(size + 4);
#endif
  if (!fdesc) return 0;
  fp.read(fdesc, size);
  fp.close();
  *rsize = size;
  return fdesc;
#else
  return 0;
#endif  
}

uint32_t Module_CheckFree(uint32_t size, uint16_t *block ) {
uint32_t eeprom_block;

  eeprom_block = plugins.free_flash_start;

  // search for free entry
  uint32_t *lp = (uint32_t*) ( plugins.flashbase + plugins.free_flash_start );
  uint32_t addr = plugins.free_flash_start;
  uint16_t cblock = 0;
  while (addr < plugins.free_flash_end) {
      uint32_t blocksize = SPI_FLASH_SEC_SIZE;
      if (*lp == MODULE_SYNC) {
        // get module size
        const FLASH_MODULE *fm = (FLASH_MODULE*)lp;
        blocksize = (fm->size / SPI_FLASH_SEC_SIZE) + 1;
        blocksize *= SPI_FLASH_SEC_SIZE;
      } else {
        // free module block, check required size
        uint8_t blocks = (size / SPI_FLASH_SEC_SIZE) + 1;
        //AddLog(LOG_LEVEL_INFO, PSTR("needed blocks: %d"), blocks);
        uint32_t *bp = lp;
        uint8_t free = 1;
        for (uint32_t cnt = 0; cnt < blocks; cnt++) {
          if (*bp == MODULE_SYNC) {
            free = 0;
          }
          bp += SPI_FLASH_SEC_SIZE / 4;
          if ((uint32_t)bp >= plugins.free_flash_end) {
            break;
          }
          //AddLog(LOG_LEVEL_INFO, PSTR("blocks: %d - %d"), cnt, free);
        }
        if (free) {
          eeprom_block = addr;
          *block = cblock;
          return eeprom_block;
          break;
        }
      }
      lp += (blocksize / 4);
      addr += blocksize;
      cblock++;
      //AddLog(LOG_LEVEL_INFO, PSTR("progress: %d"), addr);
      yield();
  }
  return 0;
}

// store a single Block to Flash
uint32_t Store_Module_Block(uint8_t *fdesc, uint8_t index) {

  //AddLog(LOG_LEVEL_INFO,PSTR("store: %d"), index);

  uint32_t new_pc = 0;

  uint32_t eeprom_block = plugins.eeprom_start_block;
  uint32_t *lwp=(uint32_t*)fdesc;

  if (!plugins.upload_start_block) {
    // write 1. sector, correct header entries

#ifdef ESP8266  
    const FLASH_MODULE *fm = (FLASH_MODULE*)fdesc;
    new_pc = (uint32_t)eeprom_block + plugins.flashbase;

    uint32_t offset = new_pc - fm->mod_start_org;
    uint32_t *lp = (uint32_t*)&fm->execution_offset; 
    *lp = offset;
  
    lp = (uint32_t*)&fm->mod_func_execute;
    *lp = (uint32_t)fm->mod_func_execute_org + fm->execution_offset;;
  
    lp = (uint32_t*)&fm->mtv;
    *lp = (uint32_t)&modules[index];

    lp = (uint32_t*)&fm->jtab;
    *lp = (uint32_t)&MODULE_JUMPTABLE;

#endif // ESP8266

#ifdef ESP32
    FLASH_MODULE *fm = (FLASH_MODULE*)fdesc;
    fm->execution_offset = (uint32_t)eeprom_block - fm->mod_start_org;

    uint32_t *lp = (uint32_t*)&fm->mod_func_execute;
    *lp = (uint32_t)fm->mod_func_execute_org + fm->execution_offset;

    fm->mtv = (uint32_t)&modules[index];
    fm->jtab = (uint32_t)&MODULE_JUMPTABLE;
    new_pc = eeprom_block;
#endif // ESP32
  }

#ifdef ESP8266
//  AddLog(LOG_LEVEL_INFO, PSTR("Module offset %x: %x: %x: %x: %x: %x"),old_pc, new_pc, offset, corr_pc, (uint32_t)fm->mod_func_execute, (uint32_t)&module_header);
  ESP.flashEraseSector(eeprom_block / SPI_FLASH_SEC_SIZE);
  ESP.flashWrite(eeprom_block , lwp, SPI_FLASH_SEC_SIZE);
  yield();
#endif // ESP8266

#ifdef ESP32
  //AddLog(LOG_LEVEL_INFO, PSTR("save module: %08x, size: %d"),eeprom_block, size);
  uint32_t offset = eeprom_block - plugins.free_flash_start;
  esp_err_t err = err = esp_partition_erase_range(plugins.flash_pptr, offset, ESP32_PLUGIN_HSIZE);
  err = esp_partition_write(plugins.flash_pptr, offset, (void*)lwp, ESP32_PLUGIN_HSIZE);
  yield();
#endif // ESP32

  if (!plugins.upload_start_block) {
    Set_Module_Start(index, new_pc);
  }
  return new_pc;;
}

void AddModules(void) {
  uint16_t module = 0;
  uint32_t *lp = (uint32_t*) ( plugins.flashbase + plugins.free_flash_start );
  for (uint32_t addr = plugins.free_flash_start; addr < plugins.free_flash_end; addr += plugins.pagesize) {
    //AddLog(LOG_LEVEL_INFO,PSTR("addr, sync %08x: %08x: %04x"),addr,(uint32_t)lp, *lp);
    const volatile FLASH_MODULE *fm = (FLASH_MODULE*)lp;
    if (fm->sync == MODULE_SYNC) {
      // add module
      modules[module].mod_addr = (FLASH_MODULE*)lp;
      modules[module].jt = MODULE_JUMPTABLE;
      //modules[module].execution_offset = fm->execution_offset;
      //modules[module].mod_size = fm->size;
      //modules[module].settings = Settings;
      modules[module].flags.data = 0;
      if (TasmotaGlobal.gpio_optiona.shelly_pro) {
        Init_module(module);
      }
      // add addr according to module size, currently assume module < SPI_FLASH_SEC_SIZE
      module++;
      if (module >= MAX_PLUGINS) {
        break;
      }
    }
    lp += plugins.pagesize/4;
  }
}

const char mod_types[] PROGMEM = "xsns|xlgt|xnrg|xdrv|";

// show all linked modules
void Module_mdir(void) {

   uint32_t *vp = (uint32_t *)calloc(sizeof(FLASH_MODULE) / 4 , 4);
  if (!vp) {
    return;
  }

 #if 1

  Response_P(PSTR("{"));
  uint8_t index = 0;
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      uint32_t *mp = (uint32_t*)modules[cnt].mod_addr;
      for (uint16_t cnt = 0; cnt < sizeof(FLASH_MODULE) / 4; cnt++) {
        vp[cnt] = mp[cnt];
      }
      const FLASH_MODULE *fm = (FLASH_MODULE*)vp;
      const uint32_t volatile mtype = fm->type;
      const uint32_t volatile rev = fm->revision;
      char name[18];
      strncpy(name, fm->name, 16);
      name[15] = 0;
      char type[6];
      GetTextIndexed(type, sizeof(type), mtype, mod_types );
      if (index > 0) {
        ResponseAppend_P(PSTR(","));
      }
      ResponseAppend_P(PSTR("\"MOD #%d\":{\"name\":\"%s\",\"addr\":\"%08x\",\"ex-offs\":\"%08x\", \"size\":%d,\"type\":\"%s\",\"rev\":%d.%d,\"mem\":%d,\"init\":%d}"),cnt + 1, name, modules[cnt].mod_addr, fm->execution_offset,
       Get_mod_size, type, (rev>>16),(rev&0xff), modules[cnt].mem_size, modules[cnt].flags.initialized);
       index++;
    }
  }
  ResponseJsonEnd();
 #else 
  AddLog(LOG_LEVEL_INFO, PSTR("| ======== Module directory ========"));
  AddLog(LOG_LEVEL_INFO, PSTR("| nr | name           | address  | size | type | rev  | ram  | init |"));
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      const FLASH_MODULE *fm = (FLASH_MODULE*)modules[cnt].mod_addr;
      const uint32_t volatile mtype = fm->type;
      const uint32_t volatile rev = fm->revision;
      // esp32 crashes when fm->name is given as addlog parameter ???, so copy to charbuffer
      // ESP8266 does not crash
      char name[16];
      strncpy(name, fm->name, 16);
      char type[6];
      GetTextIndexed(type, sizeof(type), mtype, mod_types );
      AddLog(LOG_LEVEL_INFO, PSTR("| %2d | %-15s| %08x | %4d | %4s | %04x | %4d |  %1d   |"), cnt + 1, name, modules[cnt].mod_addr,
       modules[cnt].mod_size,  type, rev, modules[cnt].mem_size, modules[cnt].flags.initialized);
      // AddLog(LOG_LEVEL_INFO, PSTR("| %2d | %-16s| %08x | %4d | %4s | %04x | %4d | %1d | %08x"), cnt + 1, fm->name, modules[cnt].mod_addr,
      //  modules[cnt].mod_size,  type, fm->revision, modules[cnt].mem_size, modules[cnt].flags.initialized, fm->execution_offset);

      //AddLog(LOG_LEVEL_INFO, PSTR("Module %d: %s %08x"), cnt + 1, fm->name, modules[cnt].mod_addr);
    }
  }
  #endif
  free(vp);
}



void Unlink_Named_Module(char *name) {
  for (uint8_t module = 0; module < MAX_PLUGINS; module++) {
    if (modules[module].mod_addr) {
      // compare name
      const FLASH_MODULE *fm = (FLASH_MODULE*)modules[module].mod_addr;
      char nam[32];
      strcpy(nam, name);
      char *cp = strchr(nam, '.');
      if (cp) {
        *cp = 0;
      }
      cp = strchr(nam, '_');
      if (cp) {
        *cp = 0;
      }

      uint32_t lval[4];
      uint32_t *lp = (uint32_t*)&fm->name[0];
      for (uint32_t cnt = 0; cnt < 4; cnt++) {
        lval[cnt] = *lp++;
      }

      if (!strcmp(nam, (char *)lval)) {
        Unlink_Module(module);
      }
    }
  }
}

void Unlink_Module(uint32_t module) {
  if (modules[module].mod_addr) {
    if (modules[module].flags.initialized) {
      // call deiniz
      Deiniz_module(module);
    }
    // remove from module table, erase flash
    if ((uint32_t)modules[module].mod_addr != (uint32_t)&module_header) {
#ifdef ESP8266
      ESP.flashEraseSector(((uint32_t)modules[module].mod_addr - plugins.flashbase) / SPI_FLASH_SEC_SIZE);
#endif
#ifdef ESP32
      esp_err_t err = esp_partition_erase_range(plugins.flash_pptr, (uint32_t)modules[module].mod_addr - plugins.free_flash_start, SPI_FLASH_SEC_SIZE);
#endif
    }
    modules[module].mod_addr = 0;
    AddLog(LOG_LEVEL_INFO,PSTR("module %d unlinked"),module + 1);
  }
}


void Read_Module_Data(uint32_t module, uint32_t *data) {
  if (modules[module].mod_addr) {
    FLASH_MODULE *fm = (FLASH_MODULE*)modules[module].mod_addr;
    if (fm->sync == MODULE_SYNC) {
      //AddLog(LOG_LEVEL_INFO,PSTR("read flash data:"));
      uint32_t num = fm->arch & 0xff000000;
      if (num) {
        num = num >> 24;
      } else {
        num = MAX_MOD_STORES;
      }
      for (uint16_t cnt = 0; cnt < num; cnt++ ) {
        *data = fm->ms[cnt].value;
        data++;
      }
    }
  }
}

void Update_Module_Data(uint32_t module, uint32_t *data) {
  if (modules[module].mod_addr) {
    uint8_t flag = modules[module].flags.initialized;
    if (flag) {
      Deiniz_module(module);
    }
    uint32_t *buff = (uint32_t *)calloc(SPI_FLASH_SEC_SIZE / 4 , 4);
    if (buff) {
#ifdef ESP8266
      ESP.flashRead((uint32_t)modules[module].mod_addr - plugins.flashbase, buff, SPI_FLASH_SEC_SIZE);
      FLASH_MODULE *fm = (FLASH_MODULE*)buff;
      //AddLog(LOG_LEVEL_INFO,PSTR("read flash: %08x"),fm->sync);
      if (fm->sync == MODULE_SYNC) {
        //AddLog(LOG_LEVEL_INFO,PSTR("modify data"));
        uint32_t num = fm->arch & 0xff000000;
        if (num) {
          num = num >> 24;
        } else {
          num = MAX_MOD_STORES;
        }
        for (uint16_t cnt = 0; cnt < num; cnt++ ) {
          fm->ms[cnt].value = *data++;
        }
        // rewrite modified module
        //AddLog(LOG_LEVEL_INFO,PSTR("write flash"));
        ESP.flashEraseSector(((uint32_t)modules[module].mod_addr - plugins.flashbase) / SPI_FLASH_SEC_SIZE);
        ESP.flashWrite((uint32_t)modules[module].mod_addr - plugins.flashbase, (uint32_t*)(uint32_t*)buff, SPI_FLASH_SEC_SIZE);
      }
#endif // ESP8266
#ifdef ESP32
      uint32_t offset = (uint32_t)modules[module].mod_addr - plugins.free_flash_start;
      AddLog(LOG_LEVEL_INFO, PSTR("part offset: %08x"), offset);
      esp_err_t err = esp_partition_read(plugins.flash_pptr, offset, (void*)buff, ESP32_PLUGIN_HSIZE);
      FLASH_MODULE *fm = (FLASH_MODULE*)buff;
      //AddLog(LOG_LEVEL_INFO,PSTR("read flash: %08x"),fm->sync);
      if (fm->sync == MODULE_SYNC) {
        AddLog(LOG_LEVEL_INFO,PSTR("modify data"));
        uint32_t num = fm->arch & 0xff000000;
        if (num) {
          num = num >> 24;
        } else {
          num = MAX_MOD_STORES;
        }
        for (uint16_t cnt = 0; cnt < num; cnt++ ) {
          fm->ms[cnt].value = *data++;
        }
        err = esp_partition_erase_range(plugins.flash_pptr, offset, ESP32_PLUGIN_HSIZE);
        err = esp_partition_write(plugins.flash_pptr, offset, (void*)buff, ESP32_PLUGIN_HSIZE);
      }
#endif // ESP32
      free(buff);
    
    }
    if (flag) {
      Init_module(module);
    }
  }
}

// link 1 module from file
void Module_link(void) {
  uint8_t *fdesc = 0;

  if (XdrvMailbox.data_len) {
    uint32_t size;
#ifdef USE_UFILESYS
   //uint8_t *mp = Load_Module(XdrvMailbox.data, &size);
    //LinkModule(mp, size, XdrvMailbox.data);
#endif
  }
  ResponseCmndDone();
}

// unlink 1 module
void Module_unlink(void) {
  if ((XdrvMailbox.payload >= 1) && (XdrvMailbox.payload <= MAX_PLUGINS)) {
    uint8_t module = XdrvMailbox.payload - 1;
    Unlink_Module(module);
  } if (XdrvMailbox.payload == 0) {
    for (uint8_t module = 0; module < MAX_PLUGINS; module++) {
      Unlink_Module(module);
    }
  }
  ResponseCmndDone();
}
int32_t mod_func_execute(uint32_t sel);

int32_t Init_module(uint32_t module) {
  if (modules[module].mod_addr && !modules[module].flags.initialized) {
    const FLASH_MODULE *fm = (FLASH_MODULE*)modules[module].mod_addr;
    uint32_t mtv = fm->mtv;
    uint32_t jtab = fm->jtab;
    uint32_t exoffs = fm->execution_offset;
    // recalc execution offset
    uint32_t mfe_org = (uint32_t)fm->mod_start_org;
    uint32_t coffs = (uint32_t)fm - mfe_org;

    if (((uint32_t)&modules[module] != mtv) || ((uint32_t)&MODULE_JUMPTABLE != jtab) || (exoffs != coffs)) {
      AddLog(LOG_LEVEL_INFO,PSTR("reinit memory link of module %d"), module + 1);
      uint32_t *buff = (uint32_t *)calloc(SPI_FLASH_SEC_SIZE / 4 , 4);
      if (buff) {
#ifdef ESP8266
        ESP.flashRead((uint32_t)modules[module].mod_addr-plugins.flashbase, buff, SPI_FLASH_SEC_SIZE);
        FLASH_MODULE *fm = (FLASH_MODULE*)buff;
        if (fm->sync == MODULE_SYNC) {
          
          uint32_t *lp = (uint32_t*)&fm->mtv;
          *lp = (uint32_t)&modules[module];

          lp = (uint32_t*)&fm->jtab;
          *lp = (uint32_t)&MODULE_JUMPTABLE;

          lp = (uint32_t*)&fm->execution_offset;
          *lp = coffs;

          ESP.flashEraseSector(((uint32_t)modules[module].mod_addr - plugins.flashbase) / SPI_FLASH_SEC_SIZE);
          ESP.flashWrite((uint32_t)modules[module].mod_addr - plugins.flashbase, (uint32_t*)buff, SPI_FLASH_SEC_SIZE);
        }
#endif // ESP8266

#ifdef ESP32
        uint32_t offset = (uint32_t)modules[module].mod_addr - plugins.free_flash_start;
        esp_err_t err = esp_partition_read(plugins.flash_pptr, offset, (void*)buff, ESP32_PLUGIN_HSIZE);
        FLASH_MODULE *fm = (FLASH_MODULE*)buff;
        if (fm->sync == MODULE_SYNC) {
          AddLog(LOG_LEVEL_INFO, PSTR("part update"));
          fm->mtv = (uint32_t)&modules[module];
          fm->jtab = (uint32_t)&MODULE_JUMPTABLE;
          err = esp_partition_erase_range(plugins.flash_pptr, offset, ESP32_PLUGIN_HSIZE);
          err = esp_partition_write(plugins.flash_pptr, offset, (void*)buff, ESP32_PLUGIN_HSIZE);
        }
#endif // EPS32
        free(buff);
      }
    }
    int32_t result = MOD_EXEC(pFUNC_INIT);
    
    modules[module].flags.web_sensor = 1;
    modules[module].flags.json_append = 1;
    AddLog(LOG_LEVEL_INFO,PSTR("module %d inizialized: %08x"),module + 1, result);
    return 1;
  }
  return 0;
}

// iniz 1 module
void Module_iniz(void) {

  if ((XdrvMailbox.payload >= 1) && (XdrvMailbox.payload <= MAX_PLUGINS)) {
    uint8_t module = XdrvMailbox.payload - 1;
    Init_module(module);
  } else if (XdrvMailbox.payload == 0) {
    for (uint8_t module = 0; module < MAX_PLUGINS; module++) {
      Init_module(module);
    }
  }
  ResponseCmndDone();
}

void Deiniz_module(uint32_t module) {
  if (modules[module].mod_addr && modules[module].flags.initialized) {
    const FLASH_MODULE *fm = (FLASH_MODULE*)modules[module].mod_addr;
    int32_t result = MOD_EXEC(pFUNC_DEINIT);
    modules[module].flags.data = 0;
    AddLog(LOG_LEVEL_INFO,PSTR("module %d deinizialized"),module + 1);
  }
}

// deiniz 1 module
void Module_deiniz(void) {
  if ((XdrvMailbox.payload >= 1) && (XdrvMailbox.payload <= MAX_PLUGINS)) {
    Deiniz_module(XdrvMailbox.payload - 1);
  }
  ResponseCmndDone();
}

// dump module hex 32 bit words
void Module_dump(void) {

#if 1
  if (XdrvMailbox.data_len) {
    
    char *cp = XdrvMailbox.data;
    uint16_t module = strtol(cp, &cp, 10);
    if (module >= 1 && module <= MAX_PLUGINS) {
      module--;

      int16_t block = 0;

      if (*cp == ' ') {
        cp++;
        block = strtol(cp, &cp, 10);
        if (block < 0 || block >= 8 ) {
          block = 0;
        }
      }
      uint16_t size = 512;
      uint32_t *lp = (uint32_t*) modules[module].mod_addr;
      lp += (512 / sizeof(uint32_t)) * block; 
      for (uint32_t cnt = 0; cnt < (size / 32) + 1; cnt ++) {
        AddLog(LOG_LEVEL_INFO,PSTR("%08x: %08x %08x %08x %08x %08x %08x %08x %08x"),lp,lp[0],lp[1],lp[2],lp[3],lp[4],lp[5],lp[6],lp[7]);
        lp += 8;
      }

    }
  }

#else  
  if ((XdrvMailbox.payload >= 1) && (XdrvMailbox.payload <= MAX_PLUGINS)) {
    uint8_t module = XdrvMailbox.payload - 1;
    if (modules[module].mod_addr) {
      uint16_t size = modules[module].mod_size;
#ifdef __riscv
      // actually should test for single core
      size = 512;
#endif      
      uint32_t *lp = (uint32_t*) modules[module].mod_addr;
      for (uint32_t cnt = 0; cnt < (size / 32) + 1; cnt ++) {
        AddLog(LOG_LEVEL_INFO,PSTR("%08x: %08x %08x %08x %08x %08x %08x %08x %08x"),lp,lp[0],lp[1],lp[2],lp[3],lp[4],lp[5],lp[6],lp[7]);
        lp += 8;
      }
    }
  }
#endif
  ResponseCmndDone();
}


void Test_prog(void) {
  if (XdrvMailbox.data_len) {
    IPAddress ip;
    String sres;
    bool res = WifiHostByName((const char*)XdrvMailbox.data, ip);
    if (res == true) {
      sres = ip.toString();
    } else {
      sres="null";
    }
    AddLog(LOG_LEVEL_INFO,PSTR("ip resolved: %s"), sres.c_str());
  }
  ResponseCmndDone();
}

#ifdef ESP8266
void Check_partition(void) {
}
#endif

#ifdef ESP32
#include <MD5Builder.h>


bool scan_ptable(uint8_t *mp, uint32_t num) {
  int num_partitions = num;
  esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
  for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
    AddLog(LOG_LEVEL_INFO,PSTR("partition addr: 0x%06x; size: 0x%06x; label: %s"), peptr->pos.offset, peptr->pos.size, peptr->label);
    peptr++;
  }
  esp_err_t ret = esp_partition_table_verify((const esp_partition_info_t *)mp, false, &num_partitions);
  AddLog(LOG_LEVEL_INFO, "partition table status: err: %d - entries: %d", ret, num_partitions);
  return ret;
}

// show or add(aX) or remove(r) custom partition (X 1..4, optional size extender time 64k)
// pack(p) shrinks app0 to 1856k and expands spiffs, preserving custom partition
// we steel the size from the spiffs partition
void Check_partition(void) {
  const esp_partition_t *pptr;

  uint32_t custom_size = 0x10000; // 64k default size
  uint32_t new_app_size = 0;
  uint8_t add = 0;
  uint8_t remove = 0;
  uint8_t pack = 0;
  if (XdrvMailbox.data_len) {
    char *cp = XdrvMailbox.data;
    while (*cp == ' ') cp++;
    if (*cp == 'a') {
      add = 1;
      cp++;
      uint32_t fac = strtol(cp, &cp, 10);
      if (fac > 4) {
        fac = 4;
      }
      if (!fac) {
        fac = 1;
      }
      custom_size *= fac;
    } else if (*cp == 'r') {
      remove = 1;
    } else if (*cp == 'p') {
      pack = 1;
      cp++;
      while (*cp == ' ') cp++;
      if (*cp) {
        // optional: app size in KB, e.g. "chkpt p 2880"
        uint32_t req_kb = strtol(cp, &cp, 10);
        if (req_kb >= 1024 && req_kb <= 3904) {
          new_app_size = req_kb * 1024;
          // align to 64k
          new_app_size = (new_app_size + 0xFFFF) & ~0xFFFF;
        }
      }
    }
  }

  if (add || remove) {
    pptr = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, "custom");
    if (pptr) {
      if (add) {
        AddLog(LOG_LEVEL_INFO,PSTR("custom plugin partition already there!"));
        ResponseCmndDone();
        return;
      }
    } else {
      if (remove) {
        AddLog(LOG_LEVEL_INFO,PSTR("custom plugin partition already removed!"));
        ResponseCmndDone();
        return;
      }
    }
    LittleFS.format();
  }

  if (pack) {
    uint32_t sketch_size = ESP.getSketchSize();
    if (!new_app_size) {
      // auto-calculate: firmware + ~200k overhead, aligned to 64k
      new_app_size = ((sketch_size + 0xFFFF) & ~0xFFFF) + 0x30000;
      new_app_size = (new_app_size + 0xFFFF) & ~0xFFFF;
      AddLog(LOG_LEVEL_INFO, PSTR("pack: firmware %d KB, auto app %d KB (overhead %d KB)"),
        sketch_size / 1024, new_app_size / 1024, (new_app_size - sketch_size) / 1024);
    } else {
      AddLog(LOG_LEVEL_INFO, PSTR("pack: firmware %d KB, requested app %d KB"),
        sketch_size / 1024, new_app_size / 1024);
      if (new_app_size < sketch_size) {
        AddLog(LOG_LEVEL_INFO, PSTR("pack: requested size too small for current firmware!"));
        ResponseCmndDone();
        return;
      }
    }
    LittleFS.format();
  }

  // partition talble is aways at 0x8000
/*
typedef struct {
    uint32_t offset;
    uint32_t size;
} esp_partition_pos_t;

typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    esp_partition_pos_t pos;
    uint8_t  label[16];
    uint32_t flags;
} esp_partition_info_t;
*/

  #define PART_OFFSET 0x8000

  int num_partitions;

  uint8_t *mp = (uint8_t*)calloc(SPI_FLASH_SEC_SIZE >> 2, 4);
  esp_err_t ret = esp_flash_read(NULL, mp, PART_OFFSET, SPI_FLASH_SEC_SIZE);
  if (ret) { 
    AddLog(LOG_LEVEL_INFO, "partition read error:", ret);
  } else {
    if (mp[0] != 0xAA || mp[1] != 0x50) {
      AddLog(LOG_LEVEL_INFO, "partition table not valid");
    } else {    
      ret = esp_partition_table_verify((const esp_partition_info_t *)mp, false, &num_partitions);
      if (!ret) {
        AddLog(LOG_LEVEL_INFO, "partition table is valid: %d entries", num_partitions);
        bool custom = false;
        int8_t hasspiffs = -1;
        esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
        for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
          AddLog(LOG_LEVEL_INFO,PSTR("partition addr: 0x%06x; size: 0x%06x; label: %s"), peptr->pos.offset, peptr->pos.size, peptr->label);
          if (!strcmp((char*)peptr->label, "spiffs")) {
            //AddLog(LOG_LEVEL_INFO,PSTR("spiffs partition found!"));
            hasspiffs = cnt;
          }
          if (!strcmp((char*)peptr->label, "custom")) {
            //AddLog(LOG_LEVEL_INFO,PSTR("custom partition found!"));
            custom = true;
          }
          peptr++;
          if (peptr->magic != ESP_PARTITION_MAGIC) {
            break;
          }
        }
        if (pack) {
          // pack: resize app0 and spiffs, preserve custom
          int8_t hasapp0 = -1;
          int8_t hascustom = -1;
          int8_t hassafeboot = -1;
          esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
          for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
            if (!strcmp((char*)peptr[cnt].label, "app0")) hasapp0 = cnt;
            if (!strcmp((char*)peptr[cnt].label, "custom")) hascustom = cnt;
            if (!strcmp((char*)peptr[cnt].label, "safeboot")) hassafeboot = cnt;
          }
          if (hassafeboot < 0) {
            AddLog(LOG_LEVEL_INFO, PSTR("pack: no safeboot partition — resize refused (no recovery possible)"));
            pack = 0;
          } else if (hasapp0 < 0 || hasspiffs < 0) {
            AddLog(LOG_LEVEL_INFO, PSTR("pack: app0 or spiffs not found"));
          } else if (peptr[hasapp0].pos.size == new_app_size) {
            AddLog(LOG_LEVEL_INFO, PSTR("pack: app0 already %d KB, nothing to do"), new_app_size / 1024);
            pack = 0; // prevent write-back
          } else {
            // calculate new spiffs offset and size
            uint32_t new_spiffs_offset = peptr[hasapp0].pos.offset + new_app_size;
            uint32_t spiffs_end;
            if (hascustom >= 0) {
              spiffs_end = peptr[hascustom].pos.offset;
              AddLog(LOG_LEVEL_INFO, PSTR("pack: preserving custom at 0x%06x"), spiffs_end);
            } else {
              spiffs_end = ESP.getFlashChipSize();
            }
            if (new_spiffs_offset >= spiffs_end) {
              AddLog(LOG_LEVEL_INFO, PSTR("pack: no room for spiffs, aborting"));
              pack = 0;
            } else {
              uint32_t new_spiffs_size = spiffs_end - new_spiffs_offset;
              AddLog(LOG_LEVEL_INFO, PSTR("pack: app0 %d KB -> %d KB"), peptr[hasapp0].pos.size / 1024, new_app_size / 1024);
              AddLog(LOG_LEVEL_INFO, PSTR("pack: spiffs %d KB @ 0x%06x -> %d KB @ 0x%06x"),
                peptr[hasspiffs].pos.size / 1024, peptr[hasspiffs].pos.offset,
                new_spiffs_size / 1024, new_spiffs_offset);
              // apply changes
              peptr[hasapp0].pos.size = new_app_size;
              peptr[hasspiffs].pos.offset = new_spiffs_offset;
              peptr[hasspiffs].pos.size = new_spiffs_size;
            }
          }
        } else if (custom == true) {
          if (remove && hasspiffs > 0) {
            AddLog(LOG_LEVEL_INFO,PSTR("may remove custom!"));
            // assuming custom directly after spiffs
            esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
            peptr += hasspiffs;
            esp_partition_info_t *peptr_custom = peptr + 1;
            peptr->pos.size += peptr_custom->pos.size;
            memset(peptr_custom, 0, sizeof(esp_partition_info_t));
            memset(peptr_custom + 1, 0xff, sizeof(esp_partition_info_t));
            num_partitions--;
          }
        } else {
          if (hasspiffs < 0) {
            AddLog(LOG_LEVEL_INFO,PSTR("no spiffs partition found!"));
          } else {
            // we may patch spiffs
            AddLog(LOG_LEVEL_INFO,PSTR("may add custom!"));
            // reiterate
            esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
            for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
              if (cnt == hasspiffs) {
                if (hasspiffs == num_partitions - 1) {
                  // spiffs is last partition
                  // shrink spiffs size by 64k
                  peptr->pos.size -= custom_size;
                  uint32_t custom_offset = peptr->pos.offset + peptr->pos.size;
                  memmove(peptr + 1, peptr, sizeof(esp_partition_info_t));
                  peptr++;
                  // insert custom part
                  peptr->pos.offset = custom_offset;
                  peptr->pos.size = custom_size;
                  peptr->type = PART_TYPE_APP;
                  peptr->subtype = PART_SUBTYPE_TEST;
                  strcpy((char*)peptr->label,"custom");
                  num_partitions++;
                  break;
                }
              }
              peptr++;
            }
          }
        }
      }
    }
  }

  MD5Builder md5;
  md5.begin();
  md5.add(mp, num_partitions * sizeof(esp_partition_info_t));
  md5.calculate();
  uint8_t result[16];
  md5.getBytes(result);
  uint8_t *end_offset = mp + (num_partitions * sizeof(esp_partition_info_t));
  end_offset[0] = 0xeb;
  end_offset[1] = 0xeb;
  memmove(end_offset + 16, result, 16);

#if 0
  File wf = ufsp->open("/partition.bin", FS_FILE_WRITE);
  wf.write(mp, SPI_FLASH_SEC_SIZE);
  wf.close();
#endif

  if (add || remove || pack) {
    scan_ptable(mp, num_partitions);
  }

  if (add || remove || pack) {
    // ESP_PARTITION_MAGIC_MD5
    // esp_partition_is_flash_region_writable
    ret = esp_flash_erase_region(NULL, PART_OFFSET, SPI_FLASH_SEC_SIZE);
    ret = esp_flash_write(NULL, mp, PART_OFFSET, SPI_FLASH_SEC_SIZE);
    // restart immediately
    ESP_Restart();
  }

  free(mp);

  ResponseCmndDone();
}
#endif // ESP32

const char HTTP_MODULES_CSS[] PROGMEM =
"<head><style>rc{color:red;}gc{color:green;}yc{color:yellow;}</style></head>"
"<table border='3' frame='void' style='width:800px;background-color:#00BFFF;'>"
"<tr align='center';><th>Slot</th><th align='left'>Name</th><th>Type</th><th>Vers</th><th>Size</th><th>RAM</th><th>GPIO</th><th>I</th><th>X</th></tr>";
const char HTTP_MODULES_TEND[] PROGMEM =
"</table>";

const char HTTP_MODULES_COMMONa[] PROGMEM =
"<tr align='center' style ='background-color: #%s'>"
"<td><yc>%02d</yc></td><td align='left'>%s</td><td>%s</td><td>%s</td><td>%d</td><td>%d</td>"; // <td>%s</td>
const char HTTP_MODULES_COMMONc[] PROGMEM =
"<td><input type='checkbox' %s onchange='miva(%d,\"%s\")';></td><td><a href='modu?delete=%d' onclick=\"return confirm('delete module ?')\">&#128293;</a></td>";

const char HTTP_MODULES_SCRIPT[] PROGMEM =
"<script>function miva(par,ivar){"
  //"rfsh=1;"
  "la('&modules='+ivar+'_'+par);"
  //"rfsh=0;
  "setTimeout(function(){"
   "window.location.reload();"
  "}, 500);"
"}";

const char MOD_DIRECTORY[] PROGMEM =
  "<p><form action='" "mo_upl" "' method='get'><button>" "%s" "</button></form></p>";

const char MOD_FORM_FILE_UPG[] PROGMEM =
  "<form method='post' action='modu' enctype='multipart/form-data'>"
  "<br><input type='file' name='modu'><br>"
  "<br><button type='submit' onclick='eb(\"f1\").style.display=\"none\";eb(\"f2\").style.display=\"block\";this.form.submit();'>" D_START " %s</button></form>"
  "<br>";


//const char MOD_FORM_FILE_UPGc[] PROGMEM =
//  "<div style='text-align:left;color:#%06x;'>" "Max Slots" " %d - " "Free Slots" " %d";

const char MOD_FORM_FILE_UPGc[] PROGMEM =
  "<p><span style='text-align:left;color:#%06x;'>" "Max Slots" " %d - " "Free Slots" " %d" " - Status: </span>"
  "<span style='text-align:right;color:%s;'>" "%s" "</span></p>";

uint16_t MOD_FreeSlots() {
  uint16_t slots = 0;
  for (uint16_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      slots += 1;
    }
  }
  return MAX_PLUGINS - slots;
}

void Modul_Check_HTML_Setvars(void) {

  if (!HttpCheckPriviledgedAccess()) { 
    return;
  }

  if (Webserver->hasArg(F("modules"))) {
    String stmp = Webserver->arg(F("modules"));
    uint32_t ind;
    char *cp=(char*)stmp.c_str();
    if (!strncmp(cp, "enb", 3)) {
      // enable sensor
      cp += 3;
      ind = strtol(cp, &cp, 10);
      cp++;
      uint8_t enabled = strtol(cp, &cp, 10);
      if (enabled) {
        Init_module(ind); 
      } else {
        Deiniz_module(ind);
      }
    }

  }

  if (Webserver->hasArg(F("sv"))) {
    // set selector
    String stmp = Webserver->arg(F("sv"));
    char *cp = (char*)stmp.c_str();
    if (!strncmp(cp, "sel", 3)) {
      cp += 3;
      uint8_t mind = strtol(cp, &cp, 10);
      cp++;
      uint8_t pinn = strtol(cp, &cp, 10);
      cp++;
      uint8_t pind = strtol(cp, &cp, 10);
      
      // should better update values on closing menu
      uint32_t vals[16];
      Read_Module_Data(mind, vals);
      uint32_t old = vals[pinn] & 0xff;
      vals[pinn] = (vals[pinn] & 0xffffff00) | pind;
      //AddLog(LOG_LEVEL_INFO,PSTR(">>> %d - %d - %d -> %d"), mind, pinn, old, pind);
      Update_Module_Data(mind, vals);
    }
  }

}

void Module_upload() {

  if (!HttpCheckPriviledgedAccess()) { return; }

  if (Webserver->hasArg(F("delete"))) {
    String stmp = Webserver->arg(F("delete"));
    char *cp = (char*)stmp.c_str();
    // unlink module
    uint8_t module = strtol(cp, &cp, 10);
    Unlink_Module(module - 1);
  }

  WSContentStart_P(PSTR("Plugins Directory"));
  WSContentSendStyle();
  WSContentSend_P(PSTR("Plugins Directory"));


  char type[16];
  char color[8];
  if (plugins.upload_error) {
    strcpy_P(color, "red");
  } else {
    strcpy_P(color, "green");
  } 

  WSContentSend_P(MOD_FORM_FILE_UPGc, WebColor(COL_TEXT), MAX_PLUGINS, MOD_FreeSlots(),color,GetTextIndexed(type, sizeof(type), plugins.upload_error, MOD_UPL_ERRMSG));

#ifdef EXECUTE_FROM_BINARY
  WSContentSend_P(MOD_FORM_FILE_UPG, PSTR("Plugin upload disabled"));
#else
  WSContentSend_P(MOD_FORM_FILE_UPG, PSTR("Plugin upload"));
#endif

  WSContentSend_P(PSTR("<div>"));
  WSContentSend_P(HTTP_MODULES_SCRIPT);
  WSContentSend_P(HTTP_SCRIPT_ROOT, Settings->web_refresh, Settings->web_refresh);
  WSContentSend_P(PSTR("</script>"));

  WSContentSend_P(HTTP_MODULES_CSS);

  // reserve space for larger headers
  uint16_t size2copy = sizeof(FLASH_MODULE) + (8 * sizeof(MODULE_STORE));
  uint32_t *vp = (uint32_t *)calloc(size2copy  / 2 , 4);
  if (!vp) {
    return;
  }

  for (uint16_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (modules[cnt].mod_addr) {
      uint32_t *mp = (uint32_t*)modules[cnt].mod_addr;
      for (uint16_t cnt = 0; cnt < size2copy / 2; cnt++) {
        vp[cnt] = mp[cnt];
      }
#if defined(ESP32)
     // const FLASH_MODULE *fm = (const FLASH_MODULE*)vp;
     // esp_err_t err = esp_partition_read(plugins.flash_pptr, (uint32_t)modules[cnt].mod_addr - plugins.free_flash_start, vp, sizeof(FLASH_MODULE));
#endif

      const FLASH_MODULE *fm = (FLASH_MODULE*)vp;

      const uint32_t volatile mtype = fm->type;
      const uint32_t volatile rev = fm->revision;
      char name[16];
      strncpy(name, fm->name, 16);
      char type[6];
      GetTextIndexed(type, sizeof(type), mtype, mod_types );

      char srev[8];
      float frev = (float)(rev >> 16) + (float)(rev & 0xffff)/100;
      dtostrf(frev, 1, 2, srev);
      WSContentSend_P(HTTP_MODULES_COMMONa, "808080", cnt + 1, name, type, srev, Get_mod_size, modules[cnt].mem_size);

      WSContentSend_P(PSTR("<td>"));
      uint32_t num = fm->arch & 0xff000000;
      if (num) {
        num = num >> 24;
      } else {
        num = MAX_MOD_STORES;
      }
      for (uint8_t xcnt = 0; xcnt < num; xcnt++) {
        char name[8];
        strncpy(name, fm->ms[xcnt].name, 8);
        if (name[0]) {
          char vn[12];
          sprintf(vn,"sel%d_%d", cnt, xcnt);
          uint32_t val32 = fm->ms[xcnt].value;
          uint8_t selector = val32 >> 24;
          WSContentSend_P(PSTR("<label for=\"p%d_%d\">%s:</label> <select  id=\"p%d_%d\" style='width: 60px;' onchange='seva(value,\"%s\")'>"),cnt,xcnt,name,cnt,xcnt,vn);
          if (!selector) {
            // pulldown
            for (int8_t pins = 0; pins <= nitems(TasmotaGlobal.gpio_pin); pins++) {
              char sel[10];
              sel[0] = 0;
              if ((val32 & 0xff) == pins) {
                strcpy_P(sel, PSTR("selected"));
              }

              int8_t xpins = pins;
              if (pins == nitems(TasmotaGlobal.gpio_pin)) {
                xpins = -1;
              } else {
                uint8_t disabled = FlashPin(pins) || RedPin(pins) || TasmotaGlobal.gpio_pin[pins];
                if (disabled) {
                  strcpy_P(sel, PSTR("disabled"));
                }
              }
                // AddLog(LOG_LEVEL_INFO,PSTR(">>> %d - %d"), pins, TasmotaGlobal.gpio_pin[pins]);
              //if (TasmotaGlobal.gpio_pin[pins] == 0) {
              WSContentSend_P(PSTR("<option value=\"%d\" %s>%d</option>"), pins, sel, xpins);
              //}
            }
          } else {
            // selector 1 
            int8_t from = (val32 >> 16);
            int8_t spin = val32 & 0xff;
            uint8_t to = val32 >> 8;
            for (int8_t pins = from; pins <= to; pins++) {
              char sel[10];
              if (spin == pins) {
                strcpy_P(sel, PSTR("selected"));
              } else {
                sel[0] = 0;
              }
              // AddLog(LOG_LEVEL_INFO,PSTR(">>> %d - %d"), pins, TasmotaGlobal.gpio_pin[pins]);
              WSContentSend_P(PSTR("<option value=\"%d\" %s>%d</option>"), pins, sel, pins);
            }
          }
          WSContentSend_P(PSTR("</select><br>"));
        }
      }
      WSContentSend_P(PSTR("</td>"));
      const char *cp;
      uint8_t uval;
      if (modules[cnt].flags.initialized) {
        cp = "checked='checked'";
        uval = 0;
      } else {
        cp = "";
        uval = 1;
      }
      char enblid[8];
      sprintf_P(enblid,PSTR("enb%d"),cnt);
      WSContentSend_P(HTTP_MODULES_COMMONc, cp, uval, enblid, cnt + 1);

    }
  }
  free(vp);      

  WSContentSend_P(HTTP_MODULES_TEND);

  WSContentSend_P(PSTR("</div>"));
  
  WSContentSpaceButton(BUTTON_MANAGEMENT);
  WSContentStop();
  
  Webserver->sendHeader(F("Location"),F("/modu"));
  Webserver->send(303);  
}

bool Check_Arch(FLASH_MODULE *fm);
bool Check_Arch(FLASH_MODULE *fm) {
    if (fm->sync != MODULE_SYNC) {
      AddLog(LOG_LEVEL_INFO,PSTR("module sync error"));
      plugins.upload_error = MOD_UPL_ERR_SYNC;
      return false;
    }

    if ((fm->arch & 0x000000ff) != CURR_ARCH) {
      AddLog(LOG_LEVEL_INFO,PSTR("plugin architecture error"));
      plugins.upload_error = MOD_UPL_ERR_ARCH;
      return false;
    }

    if (fm->revision < MINREV) {
      AddLog(LOG_LEVEL_INFO,PSTR("plugin revision to old"));
      plugins.upload_error = MOD_UPL_ERR_OLD;
      return false;
    }    
    return true;
}

int8_t Check_Slots(void) {
  for (uint8_t cnt = 0; cnt < MAX_PLUGINS; cnt++) {
    if (!modules[cnt].mod_addr) {
      return cnt;
    }
  }
  return -1;
}

void Set_Module_Start(uint8_t slot, uint32_t start) {
    modules[slot].mod_addr = (void *) start;
    modules[slot].jt = MODULE_JUMPTABLE;
    modules[slot].flags.data = 0;
    AddLog(LOG_LEVEL_INFO,PSTR("module %s loaded at slot %d"), plugins.mod_name, slot + 1);
}

bool Module_upload_start(const char* upload_filename) {
  strlcpy(plugins.mod_name, upload_filename, sizeof(plugins.mod_name));
  char *cp = strchr(plugins.mod_name, '_');
  if (cp) {
    *cp = 0;
  }
  plugins.module_bytes_read = 0;
  plugins.upload_start_block = 0;
  plugins.upload_start_flag = 0;
  plugins.upload_error = MOD_UPL_ERR_NONE;
  return true;
}

bool Module_upload_write(uint8_t *upload_buf, size_t current_size) {
  int8_t slot = 0;

  if (plugins.upload_error) {
      return false;
  }


  if (0 == plugins.upload_start_flag) {
    plugins.upload_start_flag = 1;
    // 1. block
    FLASH_MODULE *fm = (FLASH_MODULE*)upload_buf;
    if (!Check_Arch(fm)) {
      return false;
    }
    
    Unlink_Named_Module(plugins.mod_name);

    slot = Check_Slots();
    if (slot < 0) {
      plugins.upload_error = MOD_UPL_ERR_SLOTS;
      return false;
    }
    plugins.upload_slot = slot;

    plugins.module_size = fm->size;
    uint32_t size = (fm->size / SPI_FLASH_SEC_SIZE) + 1 ;
    size *= SPI_FLASH_SEC_SIZE;
    uint16_t block;
    plugins.eeprom_start_block = Module_CheckFree(size, &block);
    if (!plugins.eeprom_start_block) {
      AddLog(LOG_LEVEL_INFO,PSTR("flash slot memory error"));
      plugins.upload_error = MOD_UPL_ERR_MEM;
      return false;
    }

    // allocate 1 sector size
    plugins.module_input_buffer = (uint8_t *)special_malloc(SPI_FLASH_SEC_SIZE + 4);
    if (!plugins.module_input_buffer) {
      AddLog(LOG_LEVEL_INFO,PSTR("memory error"));
      plugins.upload_error = MOD_UPL_ERR_MEM;
      return false;
    }
        
    plugins.module_input_ptr = plugins.module_input_buffer;
  }

  delay(0);
  
  if (plugins.module_bytes_read == 0) {
    //AddLog(LOG_LEVEL_INFO,PSTR("progress bytes read 1; %d"),plugins.module_bytes_read);
    memcpy(plugins.module_input_ptr, upload_buf, current_size);
    plugins.module_bytes_read += current_size;
    if (current_size < 2048) {
      // last block
      Store_Module_Block(plugins.module_input_buffer, plugins.upload_slot);
      return false;
    }
  } else {
    //AddLog(LOG_LEVEL_INFO,PSTR("progress bytes read 2; %d"),plugins.module_bytes_read);
    memcpy(plugins.module_input_ptr + plugins.module_bytes_read, upload_buf, current_size);
    Store_Module_Block(plugins.module_input_buffer, plugins.upload_slot);
    plugins.upload_start_block++;
    plugins.eeprom_start_block += SPI_FLASH_SEC_SIZE;
    plugins.module_bytes_read = 0;
  }
  if (current_size < 2048) {
    return false;
  }
  
  return true;
}

void Module_upload_stop(void) {
  if (plugins.module_input_buffer) {
    free(plugins.module_input_buffer);
  }
}

void Module_HandleUploadLoop(void) {

  if (HTTP_USER == Web.state) { return; }
    
  HTTPUpload& upload = Webserver->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START:
    // ***** Step1: Start upload file
      Module_upload_start(upload.filename.c_str());
      break;
    case UPLOAD_FILE_WRITE:
    // ***** Step2: Write upload file
      Module_upload_write(upload.buf, upload.currentSize);
      break;
    case UPLOAD_FILE_END:
    // ***** Step3: Finish upload file
      Module_upload_stop();
      break;
  }
}

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv123(uint32_t function) {
  bool result = false;

  switch (function) {
    case FUNC_PRE_INIT:
      //InitModules();
      break;
    case FUNC_INIT:
      InitModules();
      break;
    case FUNC_COMMAND:
      if (plugins.ready) {
        result = DecodeCommand(kModuleCommands, ModuleCommand);
        if (!result) {
          result = Module_Command(pFUNC_COMMAND);
        }
      } else {
#ifdef ESP32
        result = DecodeCommand(kModuleCommands1, ModuleCommand1);
#endif
      }
      break;
    case FUNC_EVERY_100_MSECOND:
      Module_Execute(pFUNC_EVERY_100_MSECOND);
      break;
    case FUNC_EVERY_250_MSECOND:
      Module_Execute(pFUNC_EVERY_250_MSECOND);
      break;
    case FUNC_EVERY_SECOND:
      Module_Execute(pFUNC_EVERY_SECOND);
      break;
    case FUNC_WEB_ADD_BUTTON:
      Module_Execute(pFUNC_WEB_ADD_BUTTON);
      break;
    case FUNC_SET_POWER:
      Module_Execute(pFUNC_SET_POWER);
      break;
    case FUNC_LOOP:
      Module_Execute(pFUNC_LOOP);
      break;
    case FUNC_COMMAND_SENSOR:
      Module_Execute(pFUNC_COMMAND_SENSOR);
      break;
    case FUNC_WEB_ADD_MAIN_BUTTON:
      Module_Execute(pFUNC_WEB_ADD_MAIN_BUTTON);
      break;
    case FUNC_SAVE_BEFORE_RESTART:
      Module_Execute(pFUNC_SAVE_BEFORE_RESTART);
      break;
    case FUNC_SAVE_AT_MIDNIGHT:
      Module_Execute(pFUNC_SAVE_AT_MIDNIGHT);
      break;

    case FUNC_WEB_SENSOR:
      if (plugins.ready) {
        Modul_Check_HTML_Setvars();
        ModuleWebSensor();
      }
      break;
    case FUNC_JSON_APPEND:
      if (plugins.ready) {
        ModuleJsonAppend();
      }
      break;
    case FUNC_WEB_ADD_MANAGEMENT_BUTTON:
      if (plugins.ready) {
        if (XdrvMailbox.index) {
          XdrvMailbox.index++;
        } else {
          WSContentSend_P(MOD_DIRECTORY, PSTR("Plugins directory"));
        }
      }
      break;
    case FUNC_WEB_ADD_HANDLER:
      if (plugins.ready) {
        Webserver->on("/mo_upl", Module_upload);
        Webserver->on("/modu", HTTP_GET, Module_upload);
        Webserver->on("/modu", HTTP_POST,[](){Webserver->sendHeader(F("Location"),F("/modu"));Webserver->send(303);}, Module_HandleUploadLoop);
        Module_Execute(pFUNC_WEB_ADD_HANDLER);
      }
      break;
    case FUNC_ACTIVE:
      result = true;
      break;
  }
  return result;
}

#endif  // USE_BINPLUGINS
