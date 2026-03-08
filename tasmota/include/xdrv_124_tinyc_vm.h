/*
  xdrv_124_tinyc_vm.h - TinyC Bytecode VM engine (header-only)

  Separated into .h to avoid Arduino IDE auto-prototype generation issues.
  Included by xdrv_124_tinyc.ino
*/

#ifndef _XDRV_124_TINYC_VM_H_
#define _XDRV_124_TINYC_VM_H_

#ifndef HARDWARE_FALLBACK
#define HARDWARE_FALLBACK 2
#endif

#include <OneWire.h>

#ifdef USE_UFILESYS
extern FS *ffsp;
extern FS *ufsp;
// Resolve filesystem from path prefix (like Scripter's script_file_path):
//   /ffs/xxx  → flash (ffsp), strip prefix
//   /sdfs/xxx → SD card (ufsp), strip prefix
//   default   → ufsp (SD preferred), ensure leading /
static FS *tc_file_path(char *path) {
  if (!strncmp_P(path, PSTR("/ffs/"), 5)) {
    memmove(path, path + 4, strlen(path) - 3);
    return ffsp;
  }
  if (!strncmp_P(path, PSTR("/sdfs/"), 6)) {
    memmove(path, path + 5, strlen(path) - 4);
    return ufsp;
  }
  if (path[0] != '/') {
    memmove(path + 1, path, strlen(path) + 1);
    path[0] = '/';
  }
  return ufsp;
}
#endif

/*********************************************************************************************\
 * VM Configuration — ESP8266 vs ESP32
 *
 * ESP8266: ~30-35KB heap, 4KB stack → keep struct under ~4KB
 * ESP32:   ~150KB+ heap, 8KB stack → can afford larger arrays
\*********************************************************************************************/

#ifdef ESP8266
  #define TC_MAX_PROGRAM     4096    // max bytecode size
  #define TC_STACK_SIZE      64      // operand stack (256 bytes)
  #define TC_MAX_FRAMES      8       // call depth — frames are small (locals allocated dynamically)
  #define TC_MAX_LOCALS      256     // locals per frame (1KB, dynamically allocated)
  #define TC_MAX_GLOBALS     64      // global slots (256 bytes)
  #define TC_MAX_CONSTANTS   64      // constant pool entries
  #define TC_MAX_CONST_DATA  512     // string constant bytes
  #define TC_INSTR_PER_TICK  500     // instructions per 50ms tick
  #define TC_OUTPUT_SIZE     128     // output buffer for MQTT
#else  // ESP32
  #define TC_MAX_PROGRAM     16384   // max bytecode size
  #define TC_STACK_SIZE      256     // operand stack (1KB)
  #define TC_MAX_FRAMES      32      // call depth
  #define TC_MAX_LOCALS      256     // locals per frame (1KB) - enough for char arrays
  #define TC_MAX_GLOBALS     256     // global slots (1KB)
  #define TC_MAX_CONSTANTS   256     // constant pool entries (dynamic alloc, uint16_t)
  #define TC_MAX_CONST_DATA  4096    // string constant bytes
  #define TC_INSTR_PER_TICK  1000    // instructions per 50ms tick
  #define TC_OUTPUT_SIZE     128     // output buffer for MQTT (was 512)
#endif

#define TC_MAX_FILE_HANDLES  4      // max simultaneously open files

// Heap memory for large arrays (> 255 elements)
#ifdef ESP8266
  #define TC_MAX_HEAP           2048   // heap slots (8KB)
  #define TC_MAX_HEAP_HANDLES   8
#else  // ESP32
  #define TC_MAX_HEAP           8192   // heap slots (32KB)
  #define TC_MAX_HEAP_HANDLES   64     // max concurrent heap arrays (was 16, energy script needs 41+)
#endif

#define TC_MAGIC           0x54434300  // "TCC\0"
#define TC_VERSION         5           // V5: global (UDP auto-update) variables
#define TC_FILE_NAME       "/autoexec.tcb"
#define TC_MAX_PERSIST     32          // max persist variable entries
#define TC_MAX_UDP_GLOBALS 64          // max global (UDP auto-update) variable entries

// Flash-safe byte read — enables execute-from-flash on ESP32
// When USE_TINYC_FLASH_EXEC is defined, bytecode can reside in memory-mapped flash.
// pgm_read_byte() handles the aligned 32-bit read + byte extraction required by Xtensa.
// Without it, direct byte access to flash causes LoadStoreAlignment exceptions.
#ifdef USE_TINYC_FLASH_EXEC
  #define TC_READ_BYTE(ptr)  pgm_read_byte(ptr)
#else
  #define TC_READ_BYTE(ptr)  (*(ptr))
#endif
// Flash-safe memcpy (for copying string constants from binary)
#ifdef USE_TINYC_FLASH_EXEC
  static inline void tc_memcpy_flash(void *dst, const uint8_t *src, uint16_t len) {
    uint8_t *d = (uint8_t *)dst;
    for (uint16_t i = 0; i < len; i++) d[i] = pgm_read_byte(&src[i]);
  }
  #define TC_MEMCPY(dst, src, len) tc_memcpy_flash(dst, src, len)
#else
  #define TC_MEMCPY(dst, src, len) memcpy(dst, src, len)
#endif

// Callback support
#define TC_MAX_CALLBACKS  10           // max well-known callback functions
#ifdef ESP8266
  #define TC_CALLBACK_MAX_INSTR 20000  // instruction limit per callback (ESP8266)
#else
  #define TC_CALLBACK_MAX_INSTR 200000 // instruction limit per callback (ESP32)
#endif
#define TC_CALLBACK_NAME_MAX 16        // max callback name length

// UDP multicast support (Scripter-compatible protocol)
#define TC_UDP_PORT          1999
#define TC_UDP_MAX_VARS      64         // max tracked UDP variable names
#define TC_UDP_VAR_NAME_MAX  16         // max variable name length
#define TC_UDP_BUF_SIZE      320        // receive buffer (max: 2+16+1+2+64*4 = 277)
#define TC_UDP_MAX_ARRAY     64         // max float array elements per UDP variable

/*********************************************************************************************\
 * VM Opcodes
\*********************************************************************************************/

enum TcOp {
  OP_NOP          = 0x00, OP_HALT         = 0x01,
  OP_PUSH_I32     = 0x02, OP_PUSH_F32     = 0x03,
  OP_PUSH_I8      = 0x04, OP_PUSH_I16     = 0x05,
  OP_POP          = 0x06, OP_DUP          = 0x07,
  // Integer arithmetic
  OP_ADD          = 0x10, OP_SUB          = 0x11,
  OP_MUL          = 0x12, OP_DIV          = 0x13,
  OP_MOD          = 0x14, OP_NEG          = 0x15,
  // Float arithmetic
  OP_FADD         = 0x18, OP_FSUB         = 0x19,
  OP_FMUL         = 0x1A, OP_FDIV         = 0x1B,
  OP_FNEG         = 0x1C,
  // Bitwise
  OP_BIT_AND      = 0x20, OP_BIT_OR       = 0x21,
  OP_BIT_XOR      = 0x22, OP_BIT_NOT      = 0x23,
  OP_SHL          = 0x24, OP_SHR          = 0x25,
  // Integer comparison
  OP_EQ           = 0x30, OP_NEQ          = 0x31,
  OP_LT           = 0x32, OP_GT           = 0x33,
  OP_LTE          = 0x34, OP_GTE          = 0x35,
  // Float comparison
  OP_FEQ          = 0x36, OP_FNEQ         = 0x37,
  OP_FLT          = 0x38, OP_FGT          = 0x39,
  OP_FLTE         = 0x3A, OP_FGTE         = 0x3B,
  // Logical
  OP_LOGIC_AND    = 0x40, OP_LOGIC_OR     = 0x41,
  OP_LOGIC_NOT    = 0x42,
  // Control flow
  OP_JMP          = 0x50, OP_JZ           = 0x51,
  OP_JNZ          = 0x52, OP_CALL         = 0x53,
  OP_RET          = 0x54, OP_RET_VAL      = 0x55,
  // Variables
  OP_LOAD_LOCAL   = 0x60, OP_STORE_LOCAL  = 0x61,
  OP_LOAD_GLOBAL  = 0x62, OP_STORE_GLOBAL = 0x63, OP_STORE_GLOBAL_UDP = 0x64,
  // Arrays
  OP_LOAD_LOCAL_ARR  = 0x68, OP_STORE_LOCAL_ARR = 0x69,
  OP_LOAD_GLOBAL_ARR = 0x6A, OP_STORE_GLOBAL_ARR= 0x6B,
  // Type conversion
  OP_I2F          = 0x70, OP_F2I          = 0x71,
  OP_I2C          = 0x72,
  // Array address (push ref for string functions)
  OP_ADDR_LOCAL   = 0x78,  // push packed ref: (fp << 16) | base_idx
  OP_ADDR_GLOBAL  = 0x79,  // push packed ref: 0x80000000 | base_idx
  // Syscalls
  OP_SYSCALL      = 0x80,
  // Heap arrays (large arrays > 255 elements)
  OP_LOAD_HEAP_ARR  = 0xA0,  // u8 handle; pop idx -> push value
  OP_STORE_HEAP_ARR = 0xA1,  // u8 handle; pop val, pop idx -> store
  OP_ADDR_HEAP      = 0xA2,  // u8 handle -> push ref: 0xC0000000 | handle
  // Watch variables (change tracking)
  OP_STORE_WATCH    = 0xA5,  // u16 varIdx, u16 shadowIdx, u16 writtenIdx — store with shadow update
  // Constants
  OP_LOAD_CONST   = 0x90,
};

/*********************************************************************************************\
 * Syscall IDs
\*********************************************************************************************/

enum TcSyscall {
  // GPIO
  SYS_PIN_MODE        = 0,  SYS_DIGITAL_WRITE   = 1,
  SYS_DIGITAL_READ    = 2,  SYS_ANALOG_READ     = 3,
  SYS_ANALOG_WRITE    = 4,  SYS_GPIO_INIT       = 5,
  // 1-Wire (native bit-bang — timing-critical, runs in C)
  SYS_OW_SET_PIN      = 6,  // (pin) -> void — configure 1-Wire pin
  SYS_OW_RESET        = 7,  // () -> int — reset pulse, 1=presence detected
  SYS_OW_WRITE        = 8,  // (byte) -> void — write byte LSB first
  SYS_OW_READ         = 9,  // () -> int — read byte LSB first
  SYS_OW_WRITE_BIT    = 98, // (bit) -> void — write single bit
  SYS_OW_READ_BIT     = 99, // () -> int — read single bit
  SYS_OW_SEARCH_RESET = 204, // () -> void — reset search state
  SYS_OW_SEARCH       = 205, // (buf_addr) -> int — search next, copies 8-byte ROM to buf
  // Timing
  SYS_DELAY           = 10, SYS_DELAY_MICRO     = 11,
  SYS_MILLIS          = 12, SYS_MICROS          = 13,
  SYS_TIMER_START     = 14, SYS_TIMER_DONE      = 15,
  SYS_TIMER_STOP      = 16, SYS_TIMER_REMAINING = 17,
  // Serial / Output
  SYS_SERIAL_BEGIN    = 20, SYS_SERIAL_PRINT     = 21,
  SYS_SERIAL_PRINT_INT= 22, SYS_SERIAL_PRINT_FLT = 23,
  SYS_SERIAL_PRINTLN  = 24, SYS_SERIAL_READ      = 25,
  SYS_SERIAL_AVAILABLE= 26, SYS_SERIAL_CLOSE     = 27,
  SYS_SERIAL_WRITE_BYTE= 28, SYS_SERIAL_WRITE_STR= 29,
  SYS_SERIAL_WRITE_BUF = 18, // (buf_ref, len) -> void — write len bytes (binary safe)
  // Math
  SYS_MATH_ABS  = 30, SYS_MATH_MIN  = 31, SYS_MATH_MAX  = 32,
  SYS_MATH_MAP  = 33, SYS_MATH_RANDOM= 34, SYS_MATH_SQRT = 35,
  SYS_MATH_SIN  = 36, SYS_MATH_COS   = 37,
  SYS_MATH_FLOOR= 38, SYS_MATH_CEIL  = 39, SYS_MATH_ROUND = 40,
  SYS_MATH_EXP  = 198, SYS_MATH_LOG  = 199, // exp(f)->float, log(f)->float (natural)
  SYS_INT_BITS_TO_FLOAT = 49, // (int_bits) -> float — reinterpret int32 as IEEE754 float
  // String operations (work with array refs from OP_ADDR_LOCAL/OP_ADDR_GLOBAL)
  SYS_STRLEN       = 50,  // (ref) -> int
  SYS_STRCPY       = 51,  // (dst_ref, src_ref) -> void
  SYS_STRCAT       = 52,  // (dst_ref, src_ref) -> void
  SYS_STRCMP        = 53,  // (ref_a, ref_b) -> int
  SYS_STR_PRINT    = 54,  // (ref) -> void  (print char array to output)
  SYS_STRCPY_CONST = 55,  // (dst_ref, const_idx) -> void  (copy string literal into array)
  SYS_STRCAT_CONST = 56,  // (dst_ref, const_idx) -> void  (append string literal to array)
  SYS_STR_FIND_CONST = 47, // (haystack_ref, needle_const_idx) -> int (-1=not found)
  SYS_RESPONSE_CMND_STR = 48, // (const_idx) -> void — responseCmnd with string literal
  SYS_SPRINTF_INT  = 57,  // (dst_ref, fmt_const_idx, int_val) -> int chars written
  SYS_SPRINTF_FLT  = 58,  // (dst_ref, fmt_const_idx, float_val) -> int chars written
  SYS_SPRINTF_STR  = 59,  // (dst_ref, fmt_const_idx, src_ref) -> int chars written
  // sprintf append variants — same args, but append to existing string in dst
  SYS_SPRINTF_INT_CAT = 70, // (dst_ref, fmt_const_idx, int_val) -> total len
  SYS_SPRINTF_FLT_CAT = 71, // (dst_ref, fmt_const_idx, float_val) -> total len
  SYS_SPRINTF_STR_CAT = 72, // (dst_ref, fmt_const_idx, src_ref) -> total len
  // Tasmota-specific
  SYS_MQTT_PUBLISH = 44,  // publish output buffer
  SYS_GET_POWER    = 41,  // get relay state
  SYS_SET_POWER    = 42,  // set relay state
  SYS_TASM_CMD     = 43,  // (const_idx_cmd, buf_ref) -> response length
  // File I/O (LittleFS)
  SYS_FILE_OPEN    = 60,  // (const_idx_path, mode) -> handle (-1=err)
  SYS_FILE_CLOSE   = 61,  // (handle) -> 0/-1
  SYS_FILE_READ    = 62,  // (handle, buf_ref, maxBytes) -> bytes_read/-1
  SYS_FILE_WRITE   = 63,  // (handle, buf_ref, len) -> bytes_written/-1
  SYS_FILE_EXISTS  = 64,  // (const_idx_path) -> 1/0
  SYS_FILE_DELETE  = 65,  // (const_idx_path) -> 0/-1
  SYS_FILE_SIZE    = 66,  // (const_idx_path) -> bytes/-1
  SYS_FILE_FORMAT  = 67,  // () -> int — format LittleFS
  SYS_FILE_MKDIR   = 68,  // (const_idx_path) -> 1/0
  SYS_FILE_RMDIR   = 69,  // (const_idx_path) -> 1/0
  SYS_FILE_SEEK    = 73,  // (handle, offset, whence) -> 1/0
  SYS_FILE_TELL    = 79,  // (handle) -> position/-1
  SYS_FILE_DOWNLOAD= 220, // (const_idx_path, url_ref) -> HTTP status code
  SYS_FILE_GET_STR = 221, // (dst_ref, handle, const_idx_delim, index, endChar) -> strlen
  SYS_FILE_EXTRACT = 222, // (handle, from_ref, to_ref, col_offs, accum, arr_refs..., N) -> rows
  SYS_FILE_EXTRACT_FAST = 223, // same, seeks to saved position for sequential access
  // Heap allocation
  SYS_HEAP_ALLOC   = 80,  // pop size -> push handle (-1 on fail)
  SYS_HEAP_FREE    = 81,  // pop handle -> void
  // File array I/O (tab-delimited text)
  SYS_FILE_READ_ARR = 82, // (arr_ref, handle) -> element_count
  SYS_FILE_WRITE_ARR= 83, // (arr_ref, handle, append) -> void
  SYS_FILE_LOG     = 84,  // (const_idx_path, str_ref, limit) -> file_size
  // Time / timestamp functions
  SYS_TIME_STAMP   = 85,  // (buf_ref) -> int — get current Tasmota timestamp
  SYS_TIME_CONVERT = 86,  // (buf_ref, flg) -> int — 0=to web, 1=to German
  SYS_TIME_OFFSET  = 87,  // (buf_ref, days, zeroFlag) -> int — add day offset
  SYS_TIME_TO_SECS = 88,  // (buf_ref) -> int — timestamp to epoch seconds
  SYS_SECS_TO_TIME = 89,  // (buf_ref, secs) -> int — epoch seconds to timestamp
  // Tasmota output (for callbacks — route to Tasmota APIs)
  SYS_RESPONSE_APPEND     = 90, // (char_ref) -> void — ResponseAppend_P
  SYS_WEB_SEND            = 91, // (char_ref) -> void — WSContentSend_PD
  SYS_WEB_FLUSH           = 92, // () -> void — WSContentFlush
  SYS_RESPONSE_APPEND_STR = 93, // (const_idx) -> void — string literal variant
  SYS_WEB_SEND_STR        = 94, // (const_idx) -> void — string literal variant
  SYS_LOG                 = 95, // (char_ref) -> void — AddLog to Tasmota console
  SYS_LOG_STR             = 96, // (const_idx) -> void — AddLog string literal
  SYS_LGETSTRING          = 97, // (index, dst_ref) -> int — get localized string
  // UDP multicast (Scripter-compatible, 239.255.255.250:1999)
  SYS_UDP_SEND            = 100, // (const_idx_name, float_val) -> void — binary float
  SYS_UDP_RECV            = 101, // (const_idx_name) -> float — last received value
  SYS_UDP_READY           = 102, // (const_idx_name) -> int — 1 if new value available
  SYS_UDP_SEND_ARRAY      = 103, // (const_idx_name, arr_ref, count) -> void — float array
  SYS_UDP_RECV_ARRAY      = 104, // (const_idx_name, arr_ref, maxcount) -> int — recv array
  SYS_UDP_SEND_STR        = 149, // (const_idx_name, str_ref) -> void — send string (ASCII mode)
  // I2C bus (last param = bus: 0 or 1)
  SYS_I2C_READ8           = 105, // (addr, reg, bus) -> int — read byte
  SYS_I2C_WRITE8          = 106, // (addr, reg, val, bus) -> int — write byte, 1=ok
  SYS_I2C_READ_BUF        = 107, // (addr, reg, buf_ref, len, bus) -> int — read into char[]
  SYS_I2C_WRITE_BUF       = 108, // (addr, reg, buf_ref, len, bus) -> int — write from char[]
  SYS_I2C_EXISTS           = 109, // (addr, bus) -> int — 1 if device on bus
  SYS_I2C_SET_DEVICE      = 127, // (addr, bus) -> int — check available & not claimed
  SYS_I2C_SET_FOUND       = 128, // (addr, const_type, bus) -> void — register as claimed
  SYS_I2C_READ_RS         = 129, // (addr, reg, buf_ref, len, bus) -> int — repeated START read
  SYS_I2C_READ_BUF0       = 112, // (addr, buf_ref, len, bus) -> int — read without register
  SYS_I2C_WRITE0          = 113, // (addr, reg, bus) -> int — write register only (no data)
  // Smart Meter (SML)
  SYS_SML_GET             = 110, // (index) -> float — meter value (1-based, 0=count)
  SYS_SML_GETSTR          = 111, // (index, buf_ref) -> int — meter ID string into buf
  SYS_SML_WRITE           = 114, // (meter, buf_ref) -> int — send hex string to meter
  SYS_SML_READ            = 115, // (meter, buf_ref) -> int — read raw buffer into char[]
  SYS_SML_SETBAUD         = 116, // (meter, baud) -> int — change baud rate
  SYS_SML_SETWSTR         = 117, // (meter, buf_ref) -> int — set async write string
  SYS_SML_SETOPT          = 118, // (options) -> int — set SML global options
  SYS_SML_GETV            = 119, // (sel) -> int — get/reset data valid flags
  SYS_SML_WRITE_STR       = 124, // (meter, const_idx) -> int — send string literal to meter
  SYS_SML_SETWSTR_STR     = 125, // (meter, const_idx) -> int — set async write from string literal
  // General-purpose UDP (Scripter-compatible udp() function)
  SYS_UDP_FUNC            = 126, // (args..., mode) -> int — mode-based UDP dispatcher
  // SPI bus
  SYS_SPI_INIT            = 120, // (sclk, mosi, miso, speed_mhz) -> int (1=ok)
  SYS_SPI_SET_CS          = 121, // (index, pin) -> void
  SYS_SPI_TRANSFER        = 122, // (cs, buf_ref, len, mode) -> int bytes transferred
  // String manipulation
  SYS_STR_TOKEN       = 74,  // (dst_ref, src_ref, delim_char, n) -> int
  SYS_STR_SUB         = 75,  // (dst_ref, src_ref, pos, len) -> int
  SYS_STR_FIND        = 76,  // (haystack_ref, needle_ref) -> int (-1=not found)
  SYS_STR_TO_INT      = 77,  // (src_ref) -> int — parse string to integer (atoi)
  SYS_STR_TO_FLOAT    = 78,  // (src_ref) -> float — parse string to float (atof)
  // Tasmota system variables (virtual — accessed as tasm_xxx in TinyC)
  SYS_TASM_GET        = 130, // (index) -> int/float — read Tasmota variable
  SYS_TASM_SET        = 131, // (index, value) -> void — write Tasmota variable
  // Sensor JSON parsing
  SYS_SENSOR_GET      = 132, // (const_idx_path) -> float — read sensor by JSON path
  // HTTP
  SYS_HTTP_GET        = 140, // (url_ref, response_ref) -> int length
  SYS_HTTP_POST       = 141, // (url_ref, data_ref, response_ref) -> int length
  SYS_HTTP_HEADER     = 142, // (name_ref, value_ref) -> void
  SYS_WEB_PARSE       = 143, // (src_ref, delim_const, index, dst_ref) -> int length
  SYS_WEB_SEND_JSON_ARRAY = 148, // (arr_ref, count) -> void — send float array as JSON integer array
  // WebUI widgets (generate HTML for /tc_ui page)
  SYS_WEB_BUTTON      = 150, // (gref, label_const) -> void
  SYS_WEB_SLIDER      = 151, // (gref, min, max, label_const) -> void
  SYS_WEB_CHECKBOX    = 152, // (gref, label_const) -> void
  SYS_WEB_TEXT        = 153, // (gref, maxlen, label_const) -> void
  SYS_WEB_NUMBER      = 154, // (gref, min, max, label_const) -> void
  SYS_WEB_PULLDOWN    = 155, // (gref, opts_const) -> void
  SYS_WEB_RADIO       = 156, // (gref, opts_const) -> void
  SYS_WEB_TIME        = 157, // (gref, label_const) -> void
  SYS_WEB_PAGE_LABEL  = 158, // (page_num, label_const) -> void — register page with button label
  SYS_WEB_PAGE        = 159, // () -> int — returns current page number being rendered
  SYS_WEB_SEND_FILE   = 160, // (filename_const) -> void — send file contents to web page
  SYS_WEB_ON          = 161, // (handler_num, url_const) -> void — register custom web endpoint
  SYS_WEB_HANDLER     = 162, // () -> int — returns current web handler number (in WebOn callback)
  SYS_WEB_ARG         = 163, // (name_const, buf_ref) -> int — get HTTP arg into buffer, returns length
  SYS_MDNS            = 164, // (name_const, mac_const, type_const) -> int — register mDNS service
  SYS_WEB_CONSOLE_BTN = 165, // (url_const, label_const) -> void — add button to Utilities menu
  SYS_WEB_CHART       = 166, // (type, title_const, unit_const, color, pos, count, array_ref, decimals, interval, ymin, ymax) -> void
  SYS_PLUGIN_QUERY    = 167, // (dst_ref, index, p1, p2) -> strlen — Plugin_Query(index, (p1<<8)|p2, 0)
  SYS_SORT_ARRAY      = 168, // (arr_ref, count, flags) -> void — sort array in-place
  // Webcam (ESP32 — requires USE_WEBCAM)
  SYS_CAM_CONTROL     = 169, // (sel, p1, p2) -> int — multiplexed webcam control
  // Display drawing (direct renderer calls — requires USE_DISPLAY)
  SYS_DSP_TEXT        = 170, // (buf_ref) -> void — raw DisplayText command string
  SYS_DSP_CLEAR       = 171, // () -> void — clear display
  SYS_DSP_POS         = 172, // (x, y) -> void — set draw position
  SYS_DSP_FONT        = 173, // (f) -> void — set font (0-7)
  SYS_DSP_SIZE        = 174, // (s) -> void — set text size
  SYS_DSP_COLOR       = 175, // (fg, bg) -> void — set fg/bg color (16-bit 565)
  SYS_DSP_DRAW        = 176, // (buf_ref) -> void — draw string at current pos
  SYS_DSP_PIXEL       = 177, // (x, y) -> void — draw pixel
  SYS_DSP_LINE        = 178, // (x1, y1) -> void — draw line from pos to (x1,y1)
  SYS_DSP_RECT        = 179, // (w, h) -> void — draw rectangle at pos
  SYS_DSP_FILL_RECT   = 180, // (w, h) -> void — draw filled rectangle at pos
  SYS_DSP_CIRCLE      = 181, // (r) -> void — draw circle at pos
  SYS_DSP_FILL_CIRCLE = 182, // (r) -> void — draw filled circle at pos
  SYS_DSP_HLINE       = 183, // (w) -> void — horizontal line from pos
  SYS_DSP_VLINE       = 184, // (h) -> void — vertical line from pos
  SYS_DSP_ROUND_RECT  = 185, // (w, h, r) -> void — rounded rectangle
  SYS_DSP_FILL_RRECT  = 186, // (w, h, r) -> void — filled rounded rectangle
  SYS_DSP_TRIANGLE    = 187, // (x1, y1, x2, y2) -> void — triangle from pos
  SYS_DSP_FILL_TRI    = 188, // (x1, y1, x2, y2) -> void — filled triangle
  SYS_DSP_DIM         = 189, // (val) -> void — set brightness (0-15)
  SYS_DSP_ONOFF       = 190, // (on) -> void — display on/off
  SYS_DSP_UPDATE      = 191, // () -> void — update display (e-paper refresh)
  SYS_DSP_PICTURE     = 192, // (filename_const, scale) -> void — draw image file at pos
  SYS_DSP_WIDTH       = 193, // () -> int — display width in pixels
  SYS_DSP_HEIGHT      = 194, // () -> int — display height in pixels
  SYS_DSP_TEXT_STR    = 195, // (const_idx) -> void — DisplayText from string literal
  SYS_DSP_DRAW_STR    = 196, // (const_idx) -> void — draw string literal at current pos
  SYS_DSP_PAD         = 197, // (n) -> void — set text padding for dspDraw (0=off)
  // Audio
  SYS_AUDIO_VOL       = 200, // (vol) -> void — set volume 0-100
  SYS_AUDIO_PLAY      = 201, // (file_const) -> void — play MP3 file
  SYS_AUDIO_SAY       = 202, // (text_const) -> void — text-to-speech
  // Persistent variables
  SYS_PERSIST_SAVE    = 203, // () -> void — save persist globals to file
  // TCP server (Scripter-compatible ws* functions)
  SYS_TCP_OPEN        = 210, // (port) -> int — start TCP server
  SYS_TCP_CLOSE       = 211, // () -> void — close TCP server
  SYS_TCP_AVAILABLE   = 212, // () -> int — accept client + bytes available
  SYS_TCP_READ_STR    = 213, // (buf_ref) -> int — read string into char[]
  SYS_TCP_WRITE_STR   = 214, // (str_ref) -> void — write string to client
  SYS_TCP_READ_ARR    = 215, // (arr_ref) -> int — read bytes into int array
  SYS_TCP_WRITE_ARR   = 216, // (arr_ref, count, type) -> void — write array to client
  // Deep sleep (ESP32 only)
  SYS_DEEP_SLEEP      = 230, // (seconds) -> void — deep sleep with timer wakeup
  SYS_DEEP_SLEEP_GPIO = 231, // (seconds, pin, level) -> void — + GPIO wakeup
  SYS_WAKEUP_CAUSE    = 232, // () -> int — return wakeup cause
  // Email (requires USE_SENDMAIL)
  SYS_EMAIL_BODY      = 234, // (body_ref) -> void — set email body (HTML)
  SYS_EMAIL_ATTACH    = 235, // (path_const) -> void — add file attachment
  SYS_EMAIL_SEND      = 236, // (params_ref) -> int — send email, 0=ok
  SYS_EMAIL_ATTACH_PIC= 237, // (bufnum) -> void — attach webcam picture from RAM buffer 1..4
  SYS_EMAIL_BODY_STR  = 238, // (const_idx) -> void — set email body from string literal
  SYS_SPRINTF_STR_CONST     = 239, // (dst_ref, fmt_const, src_const) -> int chars
  SYS_SPRINTF_STR_CAT_CONST = 247, // (dst_ref, fmt_const, src_const) -> total len
  // _REF variants: path from char array instead of string literal
  SYS_FILE_OPEN_REF   = 224, // (path_ref, mode) -> int handle (-1=err)
  SYS_FILE_EXISTS_REF = 225, // (path_ref) -> int (1=yes, 0=no)
  SYS_FILE_DELETE_REF = 226, // (path_ref) -> int (0=ok, -1=err)
  SYS_FILE_OPENDIR    = 227, // (const_idx_path) -> int handle (-1=err)
  SYS_FILE_OPENDIR_REF= 228, // (path_ref) -> int handle (-1=err)
  SYS_FILE_READDIR    = 229, // (handle, name_buf_ref) -> int (1=entry, 0=end)
  SYS_TASM_CMD_REF   = 248, // (cmd_ref, out_buf_ref) -> int — tasmCmd with char array command
  SYS_I2C_FREE       = 249, // (addr, bus) -> void — release claimed I2C address
  SYS_WEB_CHART_SIZE  = 233, // (width, height) -> void — set chart div size in pixels (0=default)
  // Console command callback
  SYS_ADD_COMMAND     = 45, // (const_idx_prefix) -> void — register command prefix
  SYS_RESPONSE_CMND  = 46, // (buf_ref) -> void — send console response
  // Touch buttons & sliders (display GFX)
  SYS_DSP_BUTTON      = 240, // (num,x,y,w,h,oc,fc,tc,ts,text_const) -> void — power button
  SYS_DSP_TBUTTON     = 241, // (num,x,y,w,h,oc,fc,tc,ts,text_const) -> void — virtual toggle
  SYS_DSP_PBUTTON     = 242, // (num,x,y,w,h,oc,fc,tc,ts,text_const) -> void — virtual push
  SYS_DSP_SLIDER      = 243, // (num,x,y,w,h,ne,bg,fc,bc) -> void — slider
  SYS_DSP_BTN_STATE   = 244, // (num,val) -> void — set button/slider state
  SYS_TOUCH_BUTTON    = 245, // (num) -> int — read button/slider state (-1 if undef)
  SYS_DSP_BTN_DEL     = 246, // (num) -> void — delete button/slider (-1 = all)
  // Tesla Powerwall (ESP32 — requires TESLA_POWERWALL + email SSL library)
  SYS_PWL_REQUEST     = 133, // (url_const) -> int — config or API request, 0=ok
  SYS_PWL_GET_FLOAT   = 134, // (path_const) -> float — extract float from last response
  SYS_PWL_GET_STR     = 135, // (path_const, buf_ref) -> int — extract string, returns len
  SYS_PWL_BIND        = 136, // (var_ref, path_const) -> void — register auto-fill binding
  // HomeKit (ESP32 — requires USE_HOMEKIT)
  SYS_HK_SET_CODE     = 137, // (const_idx_code) -> void — set pairing code
  SYS_HK_ADD          = 138, // (const_idx_name, type) -> void — start device definition
  SYS_HK_START        = 139, // () -> int — build descriptor + start HomeKit, 0=ok
  SYS_HK_VAR          = 144, // (var_ref) -> void — bind variable to current device
  SYS_HK_READY        = 145, // (var_ref) -> int — 1 if HomeKit changed this var
  SYS_FS_INFO          = 146, // (sel) -> int — filesystem info: 0=total kB, 1=free kB

  SYS_WS2812          = 147, // (arr_ref, len, offset) -> void — set LED pixels from array + show

  SYS_HK_INIT         = 253, // (desc_ref) -> int — start HomeKit with raw descriptor, 0=ok
  SYS_HK_STOP         = 254, // () -> void — stop HomeKit
  SYS_HK_RESET        = 255, // () -> void — factory reset HomeKit pairings
  // Debug
  SYS_DEBUG_PRINT     = 250, SYS_DEBUG_PRINT_STR = 251,
  SYS_DEBUG_DUMP      = 252,
};

/*********************************************************************************************\
 * VM Error codes
\*********************************************************************************************/

enum {
  TC_OK = 0,
  TC_ERR_STACK_OVERFLOW, TC_ERR_STACK_UNDERFLOW,
  TC_ERR_FRAME_OVERFLOW, TC_ERR_DIV_ZERO,
  TC_ERR_BAD_OPCODE,     TC_ERR_BAD_SYSCALL,
  TC_ERR_BAD_BINARY,     TC_ERR_INSTRUCTION_LIMIT,
  TC_ERR_BOUNDS,         TC_ERR_PAUSED,
  TC_ERR_FORBIDDEN_PIN,
};

// Error strings in PROGMEM — saves ~120 bytes RAM on ESP8266
static const char TC_ERR_00[] PROGMEM = "OK";
static const char TC_ERR_01[] PROGMEM = "Stack overflow";
static const char TC_ERR_02[] PROGMEM = "Stack underflow";
static const char TC_ERR_03[] PROGMEM = "Call stack overflow";
static const char TC_ERR_04[] PROGMEM = "Division by zero";
static const char TC_ERR_05[] PROGMEM = "Unknown opcode";
static const char TC_ERR_06[] PROGMEM = "Unknown syscall";
static const char TC_ERR_07[] PROGMEM = "Invalid binary";
static const char TC_ERR_08[] PROGMEM = "Instruction limit";
static const char TC_ERR_09[] PROGMEM = "Bounds error";
static const char TC_ERR_10[] PROGMEM = "Paused (delay)";
static const char TC_ERR_11[] PROGMEM = "Forbidden pin";
static const char TC_ERR_XX[] PROGMEM = "Unknown";

static const char * const tc_error_table[] PROGMEM = {
  TC_ERR_00, TC_ERR_01, TC_ERR_02, TC_ERR_03, TC_ERR_04,
  TC_ERR_05, TC_ERR_06, TC_ERR_07, TC_ERR_08, TC_ERR_09, TC_ERR_10,
  TC_ERR_11
};

static const char* tc_error_str(int err) {
  static char buf[24];
  const char *p;
  if (err >= 0 && err <= TC_ERR_FORBIDDEN_PIN) {
    p = (const char *)pgm_read_ptr(&tc_error_table[err]);
  } else {
    p = TC_ERR_XX;
  }
  strncpy_P(buf, p, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  return buf;
}

/*********************************************************************************************\
 * VM Data structures
\*********************************************************************************************/

typedef struct {
  uint16_t return_pc;
  int32_t  *locals;     // dynamically allocated — TC_MAX_LOCALS int32_t's per frame
} TcFrame;

typedef struct {
  uint8_t  type;    // 1=string, 2=float
  union {
    struct { const char *ptr; uint16_t len; } str;
    float f;
  };
} TcConstant;

typedef struct {
  uint16_t offset;   // start offset in heap_data[]
  uint16_t size;     // number of int32 slots
  bool     alive;    // true if block is in use
} TcHeapHandle;

typedef struct {
  char     name[TC_CALLBACK_NAME_MAX];  // e.g. "JsonCall", "WebCall", "EverySecond"
  uint16_t address;                      // code-relative address
} TcCallback;

typedef struct {
  // Program
  const uint8_t *code;
  uint16_t code_size;
  uint16_t code_offset;
  // Execution
  uint16_t pc;
  bool     running;
  bool     halted;
  int      error;
  uint32_t instruction_count;
  // Delay support (non-blocking)
  uint32_t delay_until;    // millis() target for current delay
  bool     delayed;        // VM is waiting for delay
  // 1-Wire (using TasmotaOneWire library)
  int8_t   ow_pin;
  OneWire  *ow_bus;
  // Software timers (millis-based)
#define TC_MAX_TIMERS 4
  uint32_t timer_deadline[TC_MAX_TIMERS];
  bool     timer_active[TC_MAX_TIMERS];
  // Stack (dynamically allocated in tc_vm_load)
  int32_t  *stack;
  uint16_t stack_size;         // allocated size (entries)
  uint16_t sp;
  // Globals (dynamically allocated based on binary header globalSize)
  int32_t  *globals;
  uint16_t globals_size;
  // Frames
  TcFrame  frames[TC_MAX_FRAMES];
  uint8_t  fp;
  uint8_t  frame_count;
  // Constants (dynamically allocated in tc_vm_load)
  TcConstant *constants;
  uint16_t const_count;
  uint16_t const_capacity;     // allocated count
  char     *const_data;
  uint16_t const_data_size;    // allocated bytes
  uint16_t const_data_used;
  // Heap (for large arrays > 255 elements)
  int32_t      *heap_data;       // malloc'd on demand, NULL if no heap used
  uint16_t      heap_used;       // bump allocator: next free slot
  uint16_t      heap_capacity;   // allocated capacity in int32_t slots
  TcHeapHandle *heap_handles;    // malloc'd on first heap alloc, NULL if no heap used
  uint8_t       heap_handle_count;
  // Callback function table (V3)
  TcCallback    callbacks[TC_MAX_CALLBACKS];
  uint8_t       callback_count;
  // Persist table (V4: global entries for auto-save/load)
  struct { uint16_t index; uint16_t count; } persist[TC_MAX_PERSIST];
  uint8_t       persist_count;
  char          persist_file[32];  // e.g. "/ecotracker.pvs" — derived from .tcb filename
  // UDP globals table (V5: auto-update from UDP packets)
  // Dynamically allocated in tc_vm_load only when bytecode declares UDP globals
  struct TcUdpGlobalEntry {
    char     name[TC_UDP_VAR_NAME_MAX];  // variable name = UDP packet name
    uint16_t index;                       // globals[] slot
    uint16_t slot_count;                  // 1 for scalar, N for array
    uint8_t  type;                        // 0=float scalar, 1=float array, 2=char array
  };
  struct TcUdpGlobalEntry *udp_globals;  // NULL if no UDP globals declared
  uint8_t       udp_global_count;
} TcVM;

/*********************************************************************************************\
 * UDP multicast variable entry
\*********************************************************************************************/

typedef struct {
  char     name[TC_UDP_VAR_NAME_MAX];
  float    value;
  bool     ready;     // true if updated since last udpReady() check
  bool     used;
  // Array support (Scripter-compatible binary array protocol)
  float   *arr_data;  // malloc'd on first array receive, NULL if scalar
  uint16_t arr_count;  // number of elements in arr_data
} TcUdpVar;

/*********************************************************************************************\
 * SPI bus state
\*********************************************************************************************/

#define TC_SPI_MAX_CS  4

typedef struct {
  int8_t    sclk;            // clock pin (-1 = HW SPI bus 1, -2 = HW SPI bus 2, >=0 = bitbang)
  int8_t    mosi;            // MOSI pin (-1 = not used for bitbang)
  int8_t    miso;            // MISO pin (-1 = not used for bitbang)
  int8_t    cs[TC_SPI_MAX_CS]; // chip select pins (-1 = unused)
  bool      initialized;
#ifdef ESP32
  SPIClass *spip;            // SPI instance (hardware mode only)
  SPISettings settings;
#endif
#ifdef ESP8266
  SPIClass *spip;
  SPISettings settings;
#endif
} TcSpi;

/*********************************************************************************************\
 * Per-program VM slot (dynamically allocated — one per loaded program)
\*********************************************************************************************/

#ifdef ESP32
  #define TC_MAX_VMS 6    // max simultaneous VM instances (ESP32)
#else
  #define TC_MAX_VMS 1    // ESP8266: single VM only
#endif

struct TcSlot {
  TcVM     vm;
  uint8_t *program;
  uint32_t program_size;
  bool     loaded;
  bool     running;
  bool     autoexec;              // auto-run on boot
  char     filename[32];          // e.g. "/bresser.tcb"
  char     cmd_prefix[17];       // registered command prefix, e.g. "MP3"
  // Output buffer
  char     output[TC_OUTPUT_SIZE];
  uint16_t output_len;
  // fileExtractFast state — saved position for optimized sequential access
  int32_t  extract_handle;
  uint32_t extract_file_pos;
  uint32_t extract_last_epoch;
#ifdef ESP32
  // FreeRTOS task for VM execution (main() and TaskLoop)
  TaskHandle_t task_handle;
  volatile bool task_running;     // task loop is active
  volatile bool task_stop;        // signal task to stop
  SemaphoreHandle_t vm_mutex;     // serialize VM access between task and main thread
#endif
};

/*********************************************************************************************\
 * Driver state (shared infrastructure + VM slot pointers)
\*********************************************************************************************/

struct TINYC {
  TcSlot  *slots[TC_MAX_VMS];     // per-program VM slots (NULL = unused, malloc'd on demand)
  // Lightweight slot config — persists filename + autoexec without loading into RAM
  struct {
    char filename[32];            // .tcb filename (empty = no file assigned)
    bool autoexec;                // auto-run on boot
  } slot_config[TC_MAX_VMS];
  uint32_t instr_per_tick;
  bool     autorun;
  bool     show_info;             // show TinyC status rows on main web page
  // Upload state (one upload at a time, shared)
  uint8_t *upload_buf;
  uint32_t upload_size;
  uint32_t upload_received;
  uint8_t  upload_slot;           // target slot for current upload
  char     upload_filename[32];   // filename during upload (copied to slot on completion)
  // File I/O state (File objects are stored separately as statics — see below)
  bool     file_used[TC_MAX_FILE_HANDLES];
  // UDP multicast (Scripter-compatible, 239.255.255.250:1999)
  bool     udp_used;              // true if any udp* function was called
  bool     udp_connected;         // multicast socket active
  TcUdpVar *udp_vars;               // lazy-allocated on first UDP use
  char     udp_last_name[TC_UDP_VAR_NAME_MAX]; // name of last received var (for UdpCall)
  // TinyC always owns its own UDP multicast socket for receiving
  WiFiUDP  udp;
  char     udp_buf[TC_UDP_BUF_SIZE];
  // General-purpose UDP port (Scripter-compatible udp() function)
  WiFiUDP  udp_port;              // general-purpose UDP socket
  uint16_t udp_port_num;          // bound port number
  bool     udp_port_open;         // port is listening
  // WebUI pages (up to 6, set by wLabel(), buttons on main page)
#define TC_MAX_WEB_PAGES 6
  char     page_label[TC_MAX_WEB_PAGES][32];
  uint8_t  page_count;            // number of registered pages
  uint8_t  current_page;          // current page being rendered (for wPage())
  // Custom web handlers (webOn)
#define TC_MAX_WEB_HANDLERS 4
  char     web_handler_url[TC_MAX_WEB_HANDLERS][32];
  uint8_t  web_handler_count;
  uint8_t  current_web_handler;   // handler number during WebOn callback
  // Console buttons (webConsoleButton)
#define TC_MAX_CONSOLE_BTNS 4
  char     console_btn_url[TC_MAX_CONSOLE_BTNS][32];
  char     console_btn_label[TC_MAX_CONSOLE_BTNS][24];
  uint8_t  console_btn_count;
  // SPI bus
  TcSpi    spi;
  // HTTP request state
#define TC_HTTP_MAX_HEADERS 4
  char     (*http_hdr_name)[64];    // lazy-allocated [TC_HTTP_MAX_HEADERS][64]
  char     (*http_hdr_value)[64];   // lazy-allocated [TC_HTTP_MAX_HEADERS][64]
  uint8_t  http_hdr_count;
  // TCP server state (Scripter-compatible ws* functions)
  WiFiServer *tcp_server;              // TCP listening server (heap-allocated)
  WiFiClient tcp_client;               // current connected client
  // Deferred command — executed in main loop (safe for task-spawning commands like I2SPlay)
  char     deferred_cmd[128];
  volatile bool deferred_pending;
#ifdef ESP32
  // Port 82 download server — background task for large file serving
#ifndef TC_DLPORT
#define TC_DLPORT 82
#endif
  ESP8266WebServer *dl_server;       // download server on port TC_DLPORT
  volatile bool     dl_busy;         // download task is running
#endif
  // Email state (requires USE_SENDMAIL)
#define TC_MAX_EMAIL_ATTACH 8
  char    *email_body;               // email body text (malloc'd, freed after send)
  char    *email_attach[TC_MAX_EMAIL_ATTACH];  // file paths (malloc'd, freed after send)
  uint8_t  email_attach_count;
  bool     email_active;             // true while TinyC-initiated email is being sent
  // Tesla Powerwall state (requires TESLA_POWERWALL)
  char    *pwl_json;                 // last Powerwall JSON response (malloc'd, max 4096)
} *Tinyc = nullptr;

// Currently executing slot — set before VM execution, used by output functions
static TcSlot *tc_current_slot = nullptr;

// File handles stored as statics (not in calloc'd struct) so C++ File constructor runs properly
static File tc_file_handles[TC_MAX_FILE_HANDLES];

// HomeKit descriptor build buffer (used by hkSetCode/hkAdd/hkStart API)
// Provide Is_gpio_used() when Scripter is excluded (normally in xdrv_10_scripter.ino)
#ifndef USE_SCRIPT
bool Is_gpio_used(uint8_t gpiopin) {
  return (gpiopin < nitems(TasmotaGlobal.gpio_pin)) && (TasmotaGlobal.gpio_pin[gpiopin] > 0);
}
#endif

#ifdef USE_HOMEKIT
extern "C" int32_t homekit_main(char *, uint32_t);
static char hk_build_buf[512];
static uint16_t hk_build_pos = 0;
static bool hk_line_open = false;  // true when current device line needs closing \n

// Dirty tracking for hkReady() — set by homekit.c when Apple Home writes a variable
#define TC_HK_MAX_VARS 32
static int16_t hk_var_gidx[TC_HK_MAX_VARS];          // global index per registered HK var
static volatile uint8_t hk_var_dirty[TC_HK_MAX_VARS]; // dirty flag (set from HAP thread)
static uint8_t hk_var_count = 0;
#endif

// WebChart state (reset at start of each WebPage callback)
static uint8_t tc_chart_seq = 0;        // auto-incrementing chart div ID (tc0, tc1, ...)
static bool    tc_chart_lib_sent = false; // true after Google Charts loader emitted
static TcSlot *tc_sensor_get_slot = nullptr; // re-entry guard: skip this slot's JsonCall during sensorGet
static uint16_t tc_chart_width = 0;     // chart div width in px (0 = 100%)
static uint16_t tc_chart_height = 0;    // chart div height in px (0 = 300px)
static TasmotaSerial *tc_serial_port = nullptr; // TinyC serial port (shared across VMs)

// Helper: allocate a new slot
static TcSlot *tc_slot_alloc(void) {
  TcSlot *s = (TcSlot *)calloc(1, sizeof(TcSlot));
  if (s) {
    s->extract_handle = -1;
  }
  return s;
}

// Helper: free a slot (caller must stop VM and free program first)
static void tc_slot_free(TcSlot *s) {
  if (!s) return;
  if (s->program) { free(s->program); s->program = nullptr; }
  if (s->vm.stack) { free(s->vm.stack); s->vm.stack = nullptr; s->vm.stack_size = 0; }
  if (s->vm.globals) { free(s->vm.globals); s->vm.globals = nullptr; s->vm.globals_size = 0; }
  if (s->vm.constants) { free(s->vm.constants); s->vm.constants = nullptr; s->vm.const_capacity = 0; }
  if (s->vm.const_data) { free(s->vm.const_data); s->vm.const_data = nullptr; s->vm.const_data_size = 0; }
  if (s->vm.heap_data) { free(s->vm.heap_data); s->vm.heap_data = nullptr; }
  if (s->vm.heap_handles) { free(s->vm.heap_handles); s->vm.heap_handles = nullptr; }
  if (s->vm.udp_globals) { free(s->vm.udp_globals); s->vm.udp_globals = nullptr; s->vm.udp_global_count = 0; }
#ifdef ESP32
  if (s->vm_mutex) { vSemaphoreDelete(s->vm_mutex); s->vm_mutex = nullptr; }
#endif
  free(s);
}

/*********************************************************************************************\
 * Helper: reinterpret int32 <-> float
\*********************************************************************************************/

static inline float i2f(int32_t i) {
  union { int32_t i; float f; } u; u.i = i; return u.f;
}
static inline int32_t f2i(float f) {
  union { int32_t i; float f; } u; u.f = f; return u.i;
}

/*********************************************************************************************\
 * VM: Read helpers (big-endian bytecode)
\*********************************************************************************************/

static inline uint8_t tc_read_u8(TcVM *vm) { return TC_READ_BYTE(&vm->code[vm->pc++]); }
static inline int8_t  tc_read_i8(TcVM *vm) { return (int8_t)TC_READ_BYTE(&vm->code[vm->pc++]); }
static inline uint16_t tc_read_u16(TcVM *vm) {
  uint16_t v = ((uint16_t)TC_READ_BYTE(&vm->code[vm->pc]) << 8) | TC_READ_BYTE(&vm->code[vm->pc + 1]);
  vm->pc += 2; return v;
}
static inline int32_t tc_read_i32(TcVM *vm) {
  int32_t v = ((int32_t)TC_READ_BYTE(&vm->code[vm->pc]) << 24) | ((int32_t)TC_READ_BYTE(&vm->code[vm->pc+1]) << 16) |
              ((int32_t)TC_READ_BYTE(&vm->code[vm->pc+2]) << 8) | TC_READ_BYTE(&vm->code[vm->pc+3]);
  vm->pc += 4; return v;
}
static inline float tc_read_f32(TcVM *vm) { return i2f(tc_read_i32(vm)); }

/*********************************************************************************************\
 * VM: Array ref encoding for string functions
 * Local ref:  (fp << 16) | base_index   (bit 31 = 0)
 * Global ref: 0x80000000 | base_index   (bit 31 = 1)
\*********************************************************************************************/

static inline int32_t tc_make_local_ref(uint8_t fp, uint8_t base) {
  return ((int32_t)fp << 16) | base;
}
static inline int32_t tc_make_global_ref(uint16_t base) {
  return (int32_t)0x80000000 | base;
}

// Resolve a packed array ref to a pointer into VM memory, returns NULL on error
// Ref encoding:  bits 31-30 = 00/01 → local, 10 → global, 11 → heap
static int32_t* tc_resolve_ref(TcVM *vm, int32_t ref) {
  uint32_t uref = (uint32_t)ref;
  uint8_t tag = uref >> 30;
  if (tag == 3) {
    // Heap ref: 0xC0000000 | handle
    uint16_t handle = uref & 0xFFFF;
    if (handle < TC_MAX_HEAP_HANDLES && vm->heap_data && vm->heap_handles &&
        vm->heap_handles[handle].alive) {
      return &vm->heap_data[vm->heap_handles[handle].offset];
    }
    return nullptr;
  }
  if (tag == 2) {
    // Global ref: 0x80000000 | base_idx
    uint16_t idx = uref & 0xFFFF;
    if (idx < vm->globals_size) return &vm->globals[idx];
  } else {
    // Local ref: (fp << 16) | base_idx
    uint8_t fp = (uref >> 16) & 0xFF;
    uint8_t idx = uref & 0xFF;
    if (fp < TC_MAX_FRAMES && idx < TC_MAX_LOCALS && vm->frames[fp].locals) return vm->frames[fp].locals + idx;
  }
  return nullptr;
}

// How many int32 slots remain from the ref's base index to the end of the array?
static int32_t tc_ref_maxlen(TcVM *vm, int32_t ref) {
  uint32_t uref = (uint32_t)ref;
  uint8_t tag = uref >> 30;
  if (tag == 3) {
    // Heap ref
    uint16_t handle = uref & 0xFFFF;
    if (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive) {
      return vm->heap_handles[handle].size;
    }
    return 0;
  }
  if (tag == 2) {
    uint16_t base = uref & 0xFFFF;
    return (base < vm->globals_size) ? vm->globals_size - base : 0;
  } else {
    uint8_t base = uref & 0xFF;
    return (base < TC_MAX_LOCALS) ? TC_MAX_LOCALS - base : 0;
  }
}

// Normalize file mode: accept both char ('r','w','a') and int (0,1,2)
static inline int32_t tc_file_mode(int32_t mode) {
  if (mode == 'r') return 0;
  if (mode == 'w') return 1;
  if (mode == 'a') return 2;
  return mode;  // already 0/1/2 or invalid
}

// Extract null-terminated C string from VM array ref into char buffer
// Returns number of chars written (excluding null terminator)
static int tc_ref_to_cstr(TcVM *vm, int32_t ref, char *out, int maxOut) {
  int32_t *buf = tc_resolve_ref(vm, ref);
  if (!buf) { out[0] = '\0'; return 0; }
  int32_t maxLen = tc_ref_maxlen(vm, ref);
  int i;
  for (i = 0; i < maxLen && i < maxOut - 1; i++) {
    if (buf[i] == 0) break;
    out[i] = (char)(buf[i] & 0xFF);
  }
  out[i] = '\0';
  return i;
}

// Write a C string into a VM int32 array (one char per slot, null-terminated)
// Returns number of chars written (excluding null terminator)
static int tc_cstr_to_ref(TcVM *vm, int32_t ref, const char *src) {
  int32_t *buf = tc_resolve_ref(vm, ref);
  if (!buf || !src) return 0;
  int32_t maxLen = tc_ref_maxlen(vm, ref) - 1;
  int i;
  for (i = 0; i < maxLen && src[i]; i++) {
    buf[i] = (int32_t)(uint8_t)src[i];
  }
  buf[i] = 0;
  return i;
}

// Stream VM string ref through a callback in chunks — no size limit
// Calls sendFn(chunk, len) for each chunk.  Uses small stack buffer.
#define TC_STREAM_CHUNK 256
typedef void (*tc_send_fn)(const char *buf, int len);

static void tc_stream_ref(TcVM *vm, int32_t ref, tc_send_fn sendFn) {
  int32_t *buf = tc_resolve_ref(vm, ref);
  if (!buf) return;
  int32_t maxLen = tc_ref_maxlen(vm, ref);
  char chunk[TC_STREAM_CHUNK];
  int ci = 0;
  for (int i = 0; i < maxLen; i++) {
    if (buf[i] == 0) break;
    chunk[ci++] = (char)(buf[i] & 0xFF);
    if (ci >= TC_STREAM_CHUNK - 1) {
      chunk[ci] = '\0';
      sendFn(chunk, ci);
      ci = 0;
    }
  }
  if (ci > 0) {
    chunk[ci] = '\0';
    sendFn(chunk, ci);
  }
}

// Stream send targets for Tasmota APIs
static void tc_send_response(const char *buf, int len) {
  ResponseAppend_P(PSTR("%s"), buf);
}
#ifdef USE_WEBSERVER
static void tc_send_web(const char *buf, int len) {
  WSContentSend(buf, len);
}
#endif

/*********************************************************************************************\
 * Smart Meter (SML) access — read meter values via SML_GetVal/SML_GetSVal
\*********************************************************************************************/

#if defined(USE_SML_M) || defined(USE_SML)
  extern double SML_GetVal(uint32_t index);
  extern char *SML_GetSVal(uint32_t index);
#ifdef USE_SML_SCRIPT_CMD
  extern uint32_t SML_Write(int32_t meter, char *hstr);
  extern uint32_t SML_Read(int32_t meter, char *str, uint32_t slen);
  extern uint32_t SML_SetBaud(uint32_t meter, uint32_t br);
  extern int32_t SML_Set_WStr(uint32_t meter, char *hstr);
  extern uint32_t SML_SetOptions(uint32_t in);
  extern uint32_t sml_getv(uint32_t sel);
#endif
#endif

/*********************************************************************************************\
 * Display renderer externs — for TinyC display drawing syscalls
\*********************************************************************************************/

#ifdef USE_DISPLAY
  #include <renderer.h>
  extern Renderer *renderer;
  extern uint16_t fg_color;
  extern uint16_t bg_color;
  extern int16_t disp_xpos;
  extern int16_t disp_ypos;
  extern void DisplayText(void);
  extern void DisplayOnOff(uint8_t on);
  extern void Draw_RGB_Bitmap(char *file, uint16_t xp, uint16_t yp, uint8_t scale, bool inverted, uint16_t xs, uint16_t ys);
  #ifdef USE_TOUCH_BUTTONS
    extern VButton *buttons[MAX_TOUCH_BUTTONS];
  #endif
  static int16_t tc_dsp_pad = 0;  // padding: 0=none, >0=left-aligned, <0=right-aligned

  // Helper: draw text with optional padding via DisplayText [pN] command
  static void tc_display_text_padded(const char *text) {
    char tbuf[256];
    if (tc_dsp_pad != 0) {
      snprintf(tbuf, sizeof(tbuf), "[p%d]%s", tc_dsp_pad, text);
    } else {
      strlcpy(tbuf, text, sizeof(tbuf));
    }
    char *savptr = XdrvMailbox.data;
    XdrvMailbox.data = tbuf;
    XdrvMailbox.data_len = strlen(tbuf);
    DisplayText();
    XdrvMailbox.data = savptr;
  }
#endif

/*********************************************************************************************\
 * Tesla Powerwall — uses email library SSL (standard SSL does not work)
 * Ported from Scripter's call2pwl / Powerwall class
 * Uses ESP_SSLClient from ESP-Mail-Client library (BearSSL) instead of Arduino SSL
 *
 * Key design: bypasses Tasmota's JsonParser entirely for value extraction.
 * Powerwall responses are too large for jsmn's token limits (aggregates has
 * 4 sub-objects × ~10 keys each = 200+ tokens). Scripter worked around this
 * with 8 String.replace() calls to shrink keys + splitting the response in two.
 *
 * Instead, we use direct string scanning: find the key in the raw JSON buffer,
 * scope to the correct sub-object via brace counting, then parse the number.
 * No token array, no parser limits, no string replacements, single-pass.
\*********************************************************************************************/
#if defined(ESP32) && defined(TESLA_POWERWALL)
  #include "SSLClient/ESP_SSLClient.h"
  #include "extras/WiFiClientImpl.h"

  // SSL client instances — use email library (standard ssl does not work at all)
  static ESP_SSLClient tc_ssl_client;
  static WiFiClientImpl tc_basic_client;

#define TC_PWL_RETRIES   2
#define TC_PWL_LOGLVL    LOG_LEVEL_DEBUG
#define TC_PWL_BUFSIZE   8192
#define TC_PWL_MAX_BINDS 24

  // Powerwall connection state
  static String tc_pwl_ip       = "192.168.188.60";
  static String tc_pwl_email    = "";
  static String tc_pwl_password = "";
  static String tc_pwl_cookie   = "";
  static String tc_pwl_cts1     = "";
  static String tc_pwl_cts2     = "";

  // Variable binding table: pwlBind(&var, "path#key") registers these
  struct tc_pwl_bind_t {
    int32_t ref;          // packed global variable ref (0x80000000 | idx)
    char    path[48];     // JSON path, e.g. "site#instant_power"
  };
  static tc_pwl_bind_t tc_pwl_binds[TC_PWL_MAX_BINDS];
  static uint8_t tc_pwl_bind_count = 0;

  // ── Lightweight JSON string scanner ──────────────────────────────
  // Finds a JSON key in raw text and returns a pointer to its value.
  // Handles 1 or 2 level paths ("key" or "obj#key") using brace counting
  // to scope the search — no token array, no parser size limits.

  // Find the opening '{' of a named sub-object at the current nesting level.
  // Returns pointer to the '{', or nullptr.  end = scope boundary.
  static const char *tc_pwl_find_object(const char *start, const char *end, const char *name) {
    char needle[52];
    snprintf(needle, sizeof(needle), "\"%s\"", name);
    int nlen = strlen(needle);
    const char *p = start;
    while (p && p < end) {
      p = strstr(p, needle);
      if (!p || p >= end) return nullptr;
      // Skip past the key and any whitespace / colon to find '{'
      const char *vp = p + nlen;
      while (vp < end && (*vp == ' ' || *vp == ':' || *vp == '\t' || *vp == '\n' || *vp == '\r')) vp++;
      if (vp < end && *vp == '{') return vp;
      p = vp;  // not an object — keep searching
    }
    return nullptr;
  }

  // Given pointer to '{', find the matching '}'. Returns pointer past '}'.
  static const char *tc_pwl_match_brace(const char *open) {
    int depth = 1;
    const char *p = open + 1;
    bool in_string = false;
    while (*p && depth > 0) {
      if (*p == '"' && *(p - 1) != '\\') in_string = !in_string;
      if (!in_string) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
      }
      p++;
    }
    return p;
  }

  // Find a JSON key within [start, end) and return pointer to first char of its value.
  // Only matches keys at the current nesting level (skips nested objects/arrays).
  static const char *tc_pwl_find_key_value(const char *start, const char *end, const char *key) {
    char needle[52];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    int nlen = strlen(needle);
    const char *p = start;
    while (p && p < end) {
      p = strstr(p, needle);
      if (!p || p >= end) return nullptr;
      // Skip to colon and value
      const char *vp = p + nlen;
      while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == '\n' || *vp == '\r')) vp++;
      if (vp < end && *vp == ':') {
        vp++;
        while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == '\n' || *vp == '\r')) vp++;
        return vp;  // points to first char of value
      }
      p += nlen;  // not a key:value pair, keep going
    }
    return nullptr;
  }

  // Find the Nth occurrence of a key's value in the JSON.
  // Returns pointer to the value after the Nth "key": match, or nullptr.
  static const char *tc_pwl_find_key_value_nth(const char *start, const char *end, const char *key, int nth) {
    const char *p = start;
    int count = 0;
    char needle[52];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    int nlen = strlen(needle);
    while (p && p < end) {
      p = strstr(p, needle);
      if (!p || p >= end) return nullptr;
      const char *vp = p + nlen;
      while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == '\n' || *vp == '\r')) vp++;
      if (vp < end && *vp == ':') {
        vp++;
        while (vp < end && (*vp == ' ' || *vp == '\t' || *vp == '\n' || *vp == '\r')) vp++;
        count++;
        if (count >= nth) return vp;
      }
      p += nlen;
    }
    return nullptr;
  }

  // Scan raw JSON for a path like "site#instant_power" → float value.
  // Handles 1-level ("percentage"), 2-level ("site#instant_power"),
  // and nth-occurrence ("p_W[6]" = 6th occurrence of "p_W") paths.
  static float tc_pwl_scan_float(const char *json, int32_t json_len, const char *path) {
    // Check for [N] occurrence suffix — e.g. "p_W[6]"
    char pathbuf[48];
    strlcpy(pathbuf, path, sizeof(pathbuf));
    int nth = 1;
    char *bracket = strchr(pathbuf, '[');
    if (bracket) {
      nth = atoi(bracket + 1);
      if (nth < 1) nth = 1;
      *bracket = 0;  // truncate path at '['
    }

    // Split path into segments at '#'
    char seg1[32], seg2[32];
    seg2[0] = 0;
    const char *hash = strchr(pathbuf, '#');
    if (hash) {
      int len1 = hash - pathbuf;
      if (len1 > 31) len1 = 31;
      memcpy(seg1, pathbuf, len1); seg1[len1] = 0;
      strlcpy(seg2, hash + 1, sizeof(seg2));
    } else {
      strlcpy(seg1, pathbuf, sizeof(seg1));
    }

    const char *end = json + json_len;
    const char *scope_start = json;
    const char *scope_end = end;

    if (seg2[0]) {
      // 2-level: find the sub-object first
      const char *obj = tc_pwl_find_object(json, end, seg1);
      if (!obj) return 0.0f;
      scope_start = obj + 1;
      scope_end = tc_pwl_match_brace(obj);
      // Now search for seg2 within this sub-object (with nth)
      const char *vp = tc_pwl_find_key_value_nth(scope_start, scope_end, seg2, nth);
      return vp ? strtof(vp, nullptr) : 0.0f;
    }

    // 1-level: search root (with nth)
    const char *vp = tc_pwl_find_key_value_nth(json, end, seg1, nth);
    return vp ? strtof(vp, nullptr) : 0.0f;
  }

  // Scan raw JSON for a string value by path.  Returns length copied to buf.
  static int32_t tc_pwl_scan_str(const char *json, int32_t json_len, const char *path, char *buf, int32_t maxlen) {
    char seg1[32], seg2[32];
    seg2[0] = 0;
    const char *hash = strchr(path, '#');
    if (hash) {
      int len1 = hash - path;
      if (len1 > 31) len1 = 31;
      memcpy(seg1, path, len1); seg1[len1] = 0;
      strlcpy(seg2, hash + 1, sizeof(seg2));
    } else {
      strlcpy(seg1, path, sizeof(seg1));
    }

    const char *end = json + json_len;
    const char *scope_start = json;
    const char *scope_end = end;

    if (seg2[0]) {
      const char *obj = tc_pwl_find_object(json, end, seg1);
      if (!obj) { buf[0] = 0; return 0; }
      scope_start = obj + 1;
      scope_end = tc_pwl_match_brace(obj);
      const char *vp = tc_pwl_find_key_value(scope_start, scope_end, seg2);
      if (!vp) { buf[0] = 0; return 0; }
      if (*vp == '"') {
        vp++;
        const char *ve = strchr(vp, '"');
        int32_t slen = ve ? (ve - vp) : 0;
        if (slen > maxlen - 1) slen = maxlen - 1;
        memcpy(buf, vp, slen);
        buf[slen] = 0;
        return slen;
      }
      // Numeric — format as string
      return snprintf(buf, maxlen, "%g", strtof(vp, nullptr));
    }

    const char *vp = tc_pwl_find_key_value(json, end, seg1);
    if (!vp) { buf[0] = 0; return 0; }
    if (*vp == '"') {
      vp++;
      const char *ve = strchr(vp, '"');
      int32_t slen = ve ? (ve - vp) : 0;
      if (slen > maxlen - 1) slen = maxlen - 1;
      memcpy(buf, vp, slen);
      buf[slen] = 0;
      return slen;
    }
    return snprintf(buf, maxlen, "%g", strtof(vp, nullptr));
  }

  // ── Fill bindings from raw buffer (no JsonParser!) ───────────────
  static void tc_pwl_fill_bindings(TcVM *vm, const char *json, int32_t json_len) {
    uint8_t filled = 0;
    for (uint8_t i = 0; i < tc_pwl_bind_count; i++) {
      float fval = tc_pwl_scan_float(json, json_len, tc_pwl_binds[i].path);
      int32_t *p = tc_resolve_ref(vm, tc_pwl_binds[i].ref);
      if (p) {
        // Check if value was actually found (non-zero, or key really exists)
        // For zero values: the scan returns 0.0 both for "not found" and "value is 0".
        // Accept it — binding a var to a missing key just leaves it at 0.
        uint32_t fi;
        memcpy(&fi, &fval, 4);
        *p = (int32_t)fi;
        filled++;
      }
    }
    AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: filled %d/%d bindings"), filled, tc_pwl_bind_count);
  }

  // ── HTTPS connection ─────────────────────────────────────────────

  // Get auth cookie from Powerwall (POST /api/login/Basic)
  // Auth response is small (~500 bytes) — JsonParser is fine here.
  static String tc_pwl_get_cookie(void) {
    AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: requesting auth cookie from %s"), tc_pwl_ip.c_str());

    tc_ssl_client.setInsecure();
    tc_ssl_client.setTimeout(3000);
    tc_ssl_client.setClient(&tc_basic_client);
    tc_ssl_client.setDebugLevel(0);

    int retry = 0;
    while (retry < TC_PWL_RETRIES) {
      if (tc_ssl_client.connect(tc_pwl_ip.c_str(), 443)) break;
      delay(100);
      retry++;
    }
    if (retry >= TC_PWL_RETRIES) {
      AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: auth connect failed"));
      return "";
    }

    String data = "{\"username\":\"customer\",\"email\":\"" + tc_pwl_email +
                  "\",\"password\":\"" + tc_pwl_password + "\",\"force_sm_off\":false}";

    String payload = "POST /api/login/Basic HTTP/1.1\r\nHost: " + tc_pwl_ip +
                     "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                     String(data.length()) + "\r\n\r\n" + data + "\r\n\r\n";

    tc_ssl_client.println(payload);

    // Read response — look for token in JSON body
    uint32_t timeout = 30;
    while (tc_ssl_client.connected()) {
      if (tc_ssl_client.available()) {
        const char *cp = tc_ssl_client.peekBuffer();
        if (cp && cp[0] == 'H') {
          const char *js = strchr(cp, '{');
          if (js) {
            char *je = strchr((char*)js, '}');
            if (je) {
              *(je + 1) = 0;
              char str_value[256];
              str_value[0] = 0;
              float fv;
              JsonParser parser((char*)js);
              JsonParserObject obj = parser.getRootObject();
              JsonParsePath(&obj, "token", '#', &fv, str_value, sizeof(str_value));
              tc_ssl_client.stop();
              AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: got token"));
              return String(str_value);
            }
          }
        }
        tc_ssl_client.readStringUntil('\n');
      }
      timeout--;
      delay(100);
      if (!timeout) break;
    }
    tc_ssl_client.stop();
    return "";
  }

  // GET request — reads body, fills bindings via string scanning, stores for ad-hoc access
  static int32_t tc_pwl_get_request(TcVM *vm, const String &url) {
    AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: GET %s"), url.c_str());

    tc_ssl_client.setInsecure();
    tc_ssl_client.setTimeout(5000);
    tc_ssl_client.setClient(&tc_basic_client);
    tc_ssl_client.setBufferSizes(16384, 512);

    // Get cookie if empty
    if (tc_pwl_cookie.length() == 0) {
      tc_pwl_cookie = tc_pwl_get_cookie();
      if (tc_pwl_cookie.length() == 0) return -1;
    }

    int retry = 0;
    while (retry < TC_PWL_RETRIES) {
      if (tc_ssl_client.connect(tc_pwl_ip.c_str(), 443)) break;
      delay(100);
      retry++;
    }
    if (retry >= TC_PWL_RETRIES) {
      AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: connect failed"));
      return -1;
    }

    // HTTP/1.0 to avoid chunked transfer encoding
    String request = "GET " + url + " HTTP/1.0\r\nHost: " + tc_pwl_ip +
                     "\r\nCookie: AuthCookie=" + tc_pwl_cookie +
                     "\r\nConnection: close\r\n\r\n";
    tc_ssl_client.println(request);

    // Read response headers
    uint32_t timeout = 500;
    while (tc_ssl_client.connected()) {
      if (tc_ssl_client.available()) {
        String response = tc_ssl_client.readStringUntil('\n');
        char *cp = (char*)response.c_str();
        if (!strncmp_P(cp, PSTR("HTTP"), 4)) {
          char *sp = strchr(cp, ' ');
          if (sp) {
            uint16_t code = strtol(sp + 1, 0, 10);
            AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: HTTP %d"), code);
            if (code == 401) {
              tc_pwl_cookie = "";
              tc_ssl_client.stop();
              return -1;
            } else if (code != 200) {
              tc_ssl_client.stop();
              return -1;
            }
          }
        }
        if (response == "\r") break;
      }
      timeout--;
      delay(10);
      if (!timeout) break;
    }

    // Read body into temp buffer
    char *buf = (char*)calloc(TC_PWL_BUFSIZE, 1);
    if (!buf) {
      tc_ssl_client.stop();
      return -2;
    }
    char *wp = buf;
    timeout = 100;
    while (tc_ssl_client.connected()) {
      uint16_t dlen = tc_ssl_client.available();
      if (dlen) {
        int32_t remain = TC_PWL_BUFSIZE - 1 - (wp - buf);
        if (remain <= 0) break;
        if (dlen > (uint16_t)remain) dlen = remain;
        tc_ssl_client.read((uint8_t*)wp, dlen);
        wp += dlen;
        *wp = 0;
      }
      delay(10);
      timeout--;
      if (!timeout) break;
    }
    tc_ssl_client.stop();

    int32_t body_len = wp - buf;
    AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: got %d bytes"), body_len);

    // Fill all bindings via direct string scanning — no JsonParser, no token limits
    if (tc_pwl_bind_count > 0 && vm) {
      tc_pwl_fill_bindings(vm, buf, body_len);
    }

    // Store buffer for optional ad-hoc pwlGet()/pwlStr() access
    if (Tinyc->pwl_json) { free(Tinyc->pwl_json); Tinyc->pwl_json = nullptr; }
    Tinyc->pwl_json = buf;  // transfer ownership

    return 0;
  }

  // Main entry: config (@D, @C, @N) or API request
  static int32_t tc_call2pwl(TcVM *vm, const char *url) {
    if (*url == '@') {
      if (url[1] == 'D') {
        const char *p = url + 2;
        uint16_t pos = strcspn(p, ",");
        tc_pwl_ip = String(p).substring(0, pos);
        p += pos + 1;
        pos = strcspn(p, ",");
        tc_pwl_email = String(p).substring(0, pos);
        tc_pwl_password = String(p + pos + 1);
        AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: config ip=%s"), tc_pwl_ip.c_str());
        return 0;
      }
      if (url[1] == 'C') {
        const char *p = url + 2;
        uint16_t pos = strcspn(p, ",");
        tc_pwl_cts1 = String(p).substring(0, pos);
        tc_pwl_cts2 = String(p + pos + 1);
        return 0;
      }
      if (url[1] == 'N') {
        tc_pwl_cookie = "";
        return 0;
      }
      return -1;
    }
    return tc_pwl_get_request(vm, String(url));
  }

#endif // ESP32 && TESLA_POWERWALL

/*********************************************************************************************\
 * UDP multicast (Scripter-compatible protocol, 239.255.255.250:1999)
 * Send: binary mode  =>name:[4 bytes float]
 * Recv: both modes   =>name=ascii  or  =>name:[4 bytes float]
 *
 * TinyC always owns its own UDP socket (independent of Scripter).
 * tc_udp_poll() called from FUNC_LOOP, retries init until WiFi is ready.
\*********************************************************************************************/

// Lazy-allocate the UDP variable table
static bool tc_udp_ensure_vars(void) {
  if (Tinyc->udp_vars) return true;
  Tinyc->udp_vars = (TcUdpVar*)calloc(TC_UDP_MAX_VARS, sizeof(TcUdpVar));
  return Tinyc->udp_vars != nullptr;
}

// Find or create a UDP variable slot by name
static TcUdpVar* tc_udp_find_var(const char *name, bool create) {
  if (!Tinyc) return nullptr;
  if (!Tinyc->udp_vars) {
    if (!create) return nullptr;
    if (!tc_udp_ensure_vars()) return nullptr;
  }
  // Search existing
  for (int i = 0; i < TC_UDP_MAX_VARS; i++) {
    if (Tinyc->udp_vars[i].used && !strcmp(Tinyc->udp_vars[i].name, name)) {
      return &Tinyc->udp_vars[i];
    }
  }
  if (!create) return nullptr;
  // Create new slot
  for (int i = 0; i < TC_UDP_MAX_VARS; i++) {
    if (!Tinyc->udp_vars[i].used) {
      strlcpy(Tinyc->udp_vars[i].name, name, TC_UDP_VAR_NAME_MAX);
      Tinyc->udp_vars[i].value = 0;
      Tinyc->udp_vars[i].ready = false;
      Tinyc->udp_vars[i].used = true;
      Tinyc->udp_vars[i].arr_data = nullptr;
      Tinyc->udp_vars[i].arr_count = 0;
      return &Tinyc->udp_vars[i];
    }
  }
  return nullptr;  // table full
}

// Forward declaration — defined later in this file
static int tc_vm_call_callback(TcVM *vm, const char *name);

// Check if a named callback exists in the loaded program
static bool tc_has_callback(TcVM *vm, const char *name) {
  for (int i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) return true;
  }
  return false;
}

// Called when a UDP variable is received (from own poll or Scripter hook)
// name: variable name, umode: '=' (ASCII) or ':' (binary), data: raw bytes after delimiter
// datalen: length of remaining data (for array detection)
void tc_udp_on_receive(const char *name, char umode, const char *data, int datalen) {
  if (!Tinyc) return;
  if (!Tinyc->udp_used) return;

  // Only update variables that TinyC already registered (via udpRecv/udpSend calls)
  // Don't create new slots — avoids filling the table with unneeded network variables
  TcUdpVar *var = tc_udp_find_var(name, false);
  if (var) {
    if (umode == '=') {
      // ASCII mode: data points to string like "23.45"
      var->value = CharToFloat((char*)data);
      // Clear any array data — this is a scalar
      var->arr_count = 0;
    } else {
      // Binary mode: either single float (4 bytes) or array (2-byte len + N*4 bytes)
      uint8_t *src = (uint8_t*)data;
      uint16_t alen = 0;
      if (datalen > 4) {
        alen = (uint16_t)src[0] | ((uint16_t)src[1] << 8);  // LE 16-bit
        // Validate: alen > 0, and remaining data after 2-byte header is exactly alen*4 bytes
        if (alen > 0 && datalen == (int)(2 + alen * sizeof(float))) {
          // Array receive
          if (alen > TC_UDP_MAX_ARRAY) alen = TC_UDP_MAX_ARRAY;
          // Allocate/resize array buffer on demand
          if (!var->arr_data || var->arr_count < alen) {
            if (var->arr_data) free(var->arr_data);
            var->arr_data = (float*)malloc(alen * sizeof(float));
          }
          if (var->arr_data) {
            var->arr_count = alen;
            uint8_t *ap = src + 2;  // skip 2-byte length
            for (uint16_t i = 0; i < alen; i++) {
              union { float f; uint8_t b[4]; } u;
              u.b[0] = ap[0]; u.b[1] = ap[1]; u.b[2] = ap[2]; u.b[3] = ap[3];
              var->arr_data[i] = u.f;
              ap += sizeof(float);
            }
            var->value = var->arr_data[0];  // first element as scalar value too
          }
        } else {
          // Not a valid array header — treat as single float
          goto single_float;
        }
      } else {
        single_float:
        // Single float: 4 bytes IEEE-754
        if (datalen >= 4) {
          union { float f; uint8_t b[4]; } u;
          u.b[0] = src[0]; u.b[1] = src[1]; u.b[2] = src[2]; u.b[3] = src[3];
          var->value = u.f;
        }
        var->arr_count = 0;
      }
    }
    var->ready = true;
  }

  // Always store name and trigger UdpCall on all active slots
  strlcpy(Tinyc->udp_last_name, name, TC_UDP_VAR_NAME_MAX);

  for (int si = 0; si < TC_MAX_VMS; si++) {
    TcSlot *s = Tinyc->slots[si];
    if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif

    // Auto-update global variables from UDP packet (V5)
    TcVM *vmp = &s->vm;
    for (uint8_t gi = 0; gi < vmp->udp_global_count; gi++) {
      if (strcmp(vmp->udp_globals[gi].name, name) == 0) {
        uint16_t idx = vmp->udp_globals[gi].index;
        uint16_t cnt = vmp->udp_globals[gi].slot_count;
        uint8_t gtype = vmp->udp_globals[gi].type;
        if (gtype == 2) {
          // Char array: copy string from ASCII data into globals (1 char per slot)
          if (umode == '=' && data) {
            uint16_t slen = strlen(data);
            if (slen >= cnt) slen = cnt - 1;  // leave room for null
            for (uint16_t j = 0; j < slen && (idx + j) < vmp->globals_size; j++) {
              vmp->globals[idx + j] = (int32_t)(uint8_t)data[j];
            }
            // Null-terminate
            if ((idx + slen) < vmp->globals_size) {
              vmp->globals[idx + slen] = 0;
            }
          }
        } else if (cnt == 1 && idx < vmp->globals_size) {
          // Scalar float: store as f2i bits
          if (var) vmp->globals[idx] = f2i(var->value);
        } else if (cnt > 1 && var && var->arr_data) {
          // Float array: copy from UDP array data
          uint16_t n = (var->arr_count < cnt) ? var->arr_count : cnt;
          for (uint16_t j = 0; j < n && (idx + j) < vmp->globals_size; j++) {
            vmp->globals[idx + j] = f2i(var->arr_data[j]);
          }
        }
        break;  // one entry per name per VM
      }
    }

    tc_current_slot = s;
    tc_vm_call_callback(&s->vm, "UdpCall");
    tc_current_slot = nullptr;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
  }
}

// Clean up SPI resources
static void tc_spi_cleanup(void) {
  if (!Tinyc) return;
  TcSpi *spi = &Tinyc->spi;
  if (!spi->initialized) return;
#ifdef ESP32
  if (spi->sclk < 0 && spi->spip) {
    spi->spip->end();
    if (spi->spip != &SPI) { delete spi->spip; }
    spi->spip = nullptr;
  }
#endif
#ifdef ESP8266
  if (spi->sclk < 0 && spi->spip) {
    spi->spip->end();
    spi->spip = nullptr;
  }
#endif
  spi->initialized = false;
  for (int i = 0; i < TC_SPI_MAX_CS; i++) { spi->cs[i] = -1; }
}

// Free all UDP variable array buffers and the table itself
static void tc_udp_free_arrays(void) {
  if (!Tinyc || !Tinyc->udp_vars) return;
  for (int i = 0; i < TC_UDP_MAX_VARS; i++) {
    if (Tinyc->udp_vars[i].arr_data) {
      free(Tinyc->udp_vars[i].arr_data);
    }
  }
  free(Tinyc->udp_vars);
  Tinyc->udp_vars = nullptr;
}

// Send a float variable via binary multicast
static void tc_udp_send(const char *name, float value) {
  if (!Tinyc || !Tinyc->udp_connected) return;

  char hdr[TC_UDP_VAR_NAME_MAX + 4];   // "=>" + name + ":"
  strcpy(hdr, "=>");
  strlcat(hdr, name, sizeof(hdr) - 1);
  strlcat(hdr, ":", sizeof(hdr));

  Tinyc->udp.beginPacket(IPAddress(239, 255, 255, 250), TC_UDP_PORT);
  Tinyc->udp.write((const uint8_t*)hdr, strlen(hdr));
  Tinyc->udp.write((const uint8_t*)&value, sizeof(float));
  Tinyc->udp.endPacket();
}

// Send a float array via binary multicast: =>name:[2-byte LE count][N × 4-byte float]
static void tc_udp_send_array(const char *name, float *values, uint16_t count) {
  if (!Tinyc || !Tinyc->udp_connected) return;

  char hdr[TC_UDP_VAR_NAME_MAX + 4];
  strcpy(hdr, "=>");
  strlcat(hdr, name, sizeof(hdr) - 1);
  strlcat(hdr, ":", sizeof(hdr));

  Tinyc->udp.beginPacket(IPAddress(239, 255, 255, 250), TC_UDP_PORT);
  Tinyc->udp.write((const uint8_t*)hdr, strlen(hdr));
  // Write 2-byte LE array length
  uint8_t lenbuf[2];
  lenbuf[0] = count & 0xFF;
  lenbuf[1] = (count >> 8) & 0xFF;
  Tinyc->udp.write(lenbuf, 2);
  // Write N × 4-byte floats
  for (uint16_t i = 0; i < count; i++) {
    Tinyc->udp.write((const uint8_t*)&values[i], sizeof(float));
  }
  Tinyc->udp.endPacket();
}

// Send a string variable via ASCII multicast: =>name=string
static void tc_udp_send_str(const char *name, const char *str) {
  if (!Tinyc || !Tinyc->udp_connected) return;

  char hdr[TC_UDP_VAR_NAME_MAX + 4];   // "=>" + name + "="
  strcpy(hdr, "=>");
  strlcat(hdr, name, sizeof(hdr) - 1);
  strlcat(hdr, "=", sizeof(hdr));

  Tinyc->udp.beginPacket(IPAddress(239, 255, 255, 250), TC_UDP_PORT);
  Tinyc->udp.write((const uint8_t*)hdr, strlen(hdr));
  Tinyc->udp.write((const uint8_t*)str, strlen(str));
  Tinyc->udp.endPacket();
}

// ── UDP socket management (TinyC always owns its own multicast socket) ──

static void tc_udp_init(void) {
  if (!Tinyc) return;
  if (TasmotaGlobal.global_state.network_down) return;
  if (Tinyc->udp_connected) return;

#ifdef ESP8266
  if (Tinyc->udp.beginMulticast(WiFi.localIP(), IPAddress(239,255,255,250), TC_UDP_PORT)) {
#else
  if (Tinyc->udp.beginMulticast(IPAddress(239,255,255,250), TC_UDP_PORT)) {
#endif
    Tinyc->udp_connected = true;
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP multicast started on port %d"), TC_UDP_PORT);
  } else {
    Tinyc->udp_connected = false;
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: UDP multicast failed"));
  }
}

static void tc_udp_stop(void) {
  if (!Tinyc) return;
  if (Tinyc->udp_connected) {
    Tinyc->udp.flush();
    Tinyc->udp.stop();
    Tinyc->udp_connected = false;
  }
  tc_udp_free_arrays();
  Tinyc->udp_used = false;
  // Stop general-purpose UDP port
  if (Tinyc->udp_port_open) {
    Tinyc->udp_port.stop();
    Tinyc->udp_port_open = false;
    Tinyc->udp_port_num = 0;
  }
  // Stop TCP server
  if (Tinyc->tcp_server) {
    Tinyc->tcp_client.stop();
    Tinyc->tcp_server->stop();
    delete Tinyc->tcp_server;
    Tinyc->tcp_server = nullptr;
  }
}

// Poll for incoming UDP packets — called from FUNC_LOOP (standalone only)
static void tc_udp_poll(void) {
  if (!Tinyc || !Tinyc->udp_used) return;
  if (!Tinyc->udp_connected) {
    tc_udp_init();
    return;
  }

  uint32_t timeout = millis();
  while (1) {
    uint16_t plen = Tinyc->udp.parsePacket();
    if (!plen || plen >= TC_UDP_BUF_SIZE) {
      if (plen > 0) {
        Tinyc->udp.read(Tinyc->udp_buf, TC_UDP_BUF_SIZE - 1);
        Tinyc->udp.flush();
      }
      break;
    }
    if (millis() - timeout > 100) break;  // max 100ms processing

    int32_t len = Tinyc->udp.read(Tinyc->udp_buf, TC_UDP_BUF_SIZE - 1);
    Tinyc->udp_buf[len] = 0;

    char *lp = Tinyc->udp_buf;
    if (len < 4 || lp[0] != '=' || lp[1] != '>') continue;
    lp += 2;

    // Find delimiter: '=' (ASCII) or ':' (binary)
    char *cp = lp;
    char umode = 0;
    while (*cp) {
      if (*cp == '=') { umode = '='; break; }
      if (*cp == ':') { umode = ':'; break; }
      cp++;
    }
    if (!umode) continue;

    // Extract variable name
    *cp = 0;
    char *data = cp + 1;
    int datalen = len - (data - Tinyc->udp_buf);

    // Forward to shared handler
    tc_udp_on_receive(lp, umode, data, datalen);

    optimistic_yield(100);
  }
}


/*********************************************************************************************\
 * VM: Stack macros
\*********************************************************************************************/

#define TC_PUSH(vm, val) do { \
  if ((vm)->sp >= (vm)->stack_size) { (vm)->error = TC_ERR_STACK_OVERFLOW; return (vm)->error; } \
  (vm)->stack[(vm)->sp++] = (val); } while(0)

#define TC_POP(vm) (((vm)->sp > 0) ? (vm)->stack[--(vm)->sp] : ((vm)->error = TC_ERR_STACK_UNDERFLOW, 0))
#define TC_PEEK(vm) ((vm)->stack[(vm)->sp - 1])
#define TC_PUSHF(vm, val) do { \
  if ((vm)->sp >= (vm)->stack_size) { (vm)->error = TC_ERR_STACK_OVERFLOW; return (vm)->error; } \
  (vm)->stack[(vm)->sp++] = f2i(val); } while(0)
#define TC_POPF(vm) i2f(TC_POP(vm))

// ── Inner-loop macros for computed-goto dispatch (use local vars, goto on error) ──
#define TC_IPUSH(val) do { \
  if (_sp >= _stack_size) { _err = TC_ERR_STACK_OVERFLOW; goto _vm_exit; } \
  _stack[_sp++] = (val); } while(0)
#define TC_IPUSHF(val) do { \
  if (_sp >= _stack_size) { _err = TC_ERR_STACK_OVERFLOW; goto _vm_exit; } \
  _stack[_sp++] = f2i(val); } while(0)
#define TC_IPOP()  (_stack[--_sp])
#define TC_IPEEK() (_stack[_sp - 1])
#define TC_IPOPF() i2f(TC_IPOP())

/*********************************************************************************************\
 * Deferred command execution (main-loop safe for task-spawning commands)
 * Audio playback (I2SPlay) spawns a FreeRTOS task — calling it from the TinyC task
 * causes crashes. Queue it here and execute from TinyCEvery50ms() in the main loop.
\*********************************************************************************************/

static void tc_defer_command(const char *cmd) {
  if (!Tinyc || Tinyc->deferred_pending) return;  // drop if one is already pending
  strlcpy(Tinyc->deferred_cmd, cmd, sizeof(Tinyc->deferred_cmd));
  Tinyc->deferred_pending = true;
}

static void tc_deferred_exec(void) {
  if (!Tinyc || !Tinyc->deferred_pending) return;
  Tinyc->deferred_pending = false;
  AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: deferred exec '%s'"), Tinyc->deferred_cmd);
  ExecuteCommand(Tinyc->deferred_cmd, SRC_TCL);
}

/*********************************************************************************************\
 * Output: append to slot's buffer, flush to AddLog + MQTT
 * Uses tc_current_slot (set before VM execution) to find the right buffer.
\*********************************************************************************************/

static void tc_output_char(char c) {
  TcSlot *s = tc_current_slot;
  if (!s) return;
  if (s->output_len < TC_OUTPUT_SIZE - 1) {
    s->output[s->output_len++] = c;
    s->output[s->output_len] = '\0';
  }
  // Flush on newline or buffer full
  if (c == '\n' || s->output_len >= TC_OUTPUT_SIZE - 2) {
    uint16_t len = s->output_len;
    if (len > 0 && s->output[len-1] == '\n') {
      s->output[len-1] = '\0';
    }
    if (s->output[0]) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s"), s->output);
    }
    s->output_len = 0;
    s->output[0] = '\0';
  }
}

static void tc_output_string(const char *s) {
  while (*s) tc_output_char(*s++);
}

static void tc_output_int(int32_t v) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%d", v);
  tc_output_string(buf);
}

static void tc_output_float(float v) {
  char buf[16];
  dtostrfd((double)v, 2, buf);
  tc_output_string(buf);
}

// Flush output buffer to AddLog only (safe to call from any context, including FreeRTOS task)
static void tc_output_flush(void) {
  TcSlot *s = tc_current_slot;
  if (!s || s->output_len == 0) return;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s"), s->output);
  s->output_len = 0;
  s->output[0] = '\0';
}

// Flush output buffer AND publish to MQTT (only call from main Tasmota context, not from task)
static void tc_output_flush_mqtt(void) {
  TcSlot *s = tc_current_slot;
  if (!s || s->output_len == 0) return;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s"), s->output);
  Response_P(PSTR("{\"TinyC\":{\"Output\":\"%s\"}}"), s->output);
  MqttPublishPrefixTopicRulesProcess_P(RESULT_OR_TELE, PSTR("TINYC"));
  s->output_len = 0;
  s->output[0] = '\0';
}

/*********************************************************************************************\
 * Frame locals: dynamic allocation
 * Each frame's locals[] is malloc'd on OP_CALL and freed on OP_RET.
 * This saves ~2KB+ RAM on ESP8266 vs static arrays in every frame.
\*********************************************************************************************/

// Allocate locals for a frame, returns false on OOM
static bool tc_frame_alloc(TcFrame *frame) {
  frame->locals = (int32_t *)calloc(TC_MAX_LOCALS, sizeof(int32_t));
  return (frame->locals != nullptr);
}

// Free locals for a frame (safe to call if already freed)
static void tc_frame_free(TcFrame *frame) {
  if (frame->locals) {
    free(frame->locals);
    frame->locals = nullptr;
  }
}

// Free all allocated frames (call on VM stop/reset/reload)
static void tc_free_all_frames(TcVM *vm) {
  for (int i = 0; i < TC_MAX_FRAMES; i++) {
    tc_frame_free(&vm->frames[i]);
  }
  vm->frame_count = 0;
  vm->fp = 0;
}

/*********************************************************************************************\
 * Heap allocation: bump allocator with handle table
 * Used for large arrays (> 255 elements). heap_data is malloc'd on demand.
\*********************************************************************************************/

// Allocate a heap block, returns handle index or -1 on failure
static int tc_heap_alloc(TcVM *vm, uint16_t size) {
  // Lazy-allocate heap buffer
  if (!vm->heap_data) {
    uint16_t init_cap = size > 256 ? size : 256;  // start small
    vm->heap_data = (int32_t *)special_calloc(init_cap, sizeof(int32_t));
    if (!vm->heap_data) return -1;
    vm->heap_capacity = init_cap;
    vm->heap_used = 0;
  }
  // Lazy-allocate heap handles array
  if (!vm->heap_handles) {
    vm->heap_handles = (TcHeapHandle *)calloc(TC_MAX_HEAP_HANDLES, sizeof(TcHeapHandle));
    if (!vm->heap_handles) return -1;
    vm->heap_handle_count = 0;
  }
  // Find free handle slot
  int handle = -1;
  for (int i = 0; i < TC_MAX_HEAP_HANDLES; i++) {
    if (!vm->heap_handles[i].alive) { handle = i; break; }
  }
  if (handle < 0) return -1;
  // Grow heap if needed (up to TC_MAX_HEAP)
  if (vm->heap_used + size > vm->heap_capacity) {
    uint16_t new_cap = vm->heap_capacity;
    while (new_cap < vm->heap_used + size && new_cap < TC_MAX_HEAP) {
      new_cap = new_cap + (new_cap >> 1);  // grow 1.5x
      if (new_cap > TC_MAX_HEAP) new_cap = TC_MAX_HEAP;
    }
    if (vm->heap_used + size > new_cap) return -1;  // hard limit
    int32_t *new_data = (int32_t *)special_realloc(vm->heap_data, new_cap * sizeof(int32_t));
    if (!new_data) return -1;
    // Zero new portion
    memset(&new_data[vm->heap_capacity], 0, (new_cap - vm->heap_capacity) * sizeof(int32_t));
    vm->heap_data = new_data;
    vm->heap_capacity = new_cap;
  }
  // Bump-allocate
  vm->heap_handles[handle].offset = vm->heap_used;
  vm->heap_handles[handle].size = size;
  vm->heap_handles[handle].alive = true;
  // Zero-initialize the new block
  memset(&vm->heap_data[vm->heap_used], 0, size * sizeof(int32_t));
  vm->heap_used += size;
  if ((uint8_t)(handle + 1) > vm->heap_handle_count) vm->heap_handle_count = handle + 1;
  return handle;
}

// Mark a heap handle as dead (no compaction — bump allocator)
static void tc_heap_free_handle(TcVM *vm, int handle) {
  if (handle >= 0 && handle < TC_MAX_HEAP_HANDLES && vm->heap_handles) {
    vm->heap_handles[handle].alive = false;
  }
}

// Free entire heap buffer (call on VM stop/reset/reload)
static void tc_heap_free_all(TcVM *vm) {
  if (vm->heap_data) {
    free(vm->heap_data);
    vm->heap_data = nullptr;
  }
  if (vm->heap_handles) {
    free(vm->heap_handles);
    vm->heap_handles = nullptr;
  }
  vm->heap_used = 0;
  vm->heap_capacity = 0;
  vm->heap_handle_count = 0;
}

/*********************************************************************************************\
 * File I/O helpers
\*********************************************************************************************/

// Close all open file handles (call on VM stop/reset)
static void tc_close_all_files(void) {
  if (!Tinyc) return;
  for (int i = 0; i < TC_MAX_FILE_HANDLES; i++) {
    if (Tinyc->file_used[i]) {
      tc_file_handles[i].close();
      Tinyc->file_used[i] = false;
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: file cleanup handle %d"), i);
    }
  }
}

static void tc_serial_close(void) {
  if (tc_serial_port) {
    tc_serial_port->flush();
    delay(50);
    delete tc_serial_port;
    tc_serial_port = nullptr;
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial port closed"));
  }
}

// Find a free file handle slot, returns -1 if none available
static int tc_alloc_file_handle(void) {
  if (!Tinyc) return -1;
  for (int i = 0; i < TC_MAX_FILE_HANDLES; i++) {
    if (!Tinyc->file_used[i]) return i;
  }
  return -1;
}

// Get a constant string from the constant pool, returns nullptr on error
static const char* tc_get_const_str(TcVM *vm, int32_t idx) {
  if (idx >= 0 && idx < vm->const_count && vm->constants[idx].type == 1) {
    return vm->constants[idx].str.ptr;
  }
  return nullptr;
}

/*********************************************************************************************\
 * VM: sprintf helper — format into temp buf, copy to VM int32 array
 * Returns number of chars written (excluding null terminator)
\*********************************************************************************************/

static int32_t tc_sprintf_to_ref(int32_t *dst, int32_t maxSlots, const char *tmpbuf) {
  int32_t len = strlen(tmpbuf);
  if (len > maxSlots - 1) len = maxSlots - 1;
  for (int32_t i = 0; i < len; i++) {
    dst[i] = (int32_t)(uint8_t)tmpbuf[i];
  }
  dst[len] = 0;
  return len;
}

// Find null terminator in VM int32 array, returns offset (capped at maxSlots)
static int32_t tc_strlen_ref(int32_t *p, int32_t maxSlots) {
  int32_t i = 0;
  while (i < maxSlots && p[i] != 0) i++;
  return i;
}

// Format a float using dtostrfd() (Arduino-safe, no %f dependency).
// Handles format strings like "%.2f", "%f", "Value: %.3f units"
// Finds the first %[.N]f specifier, replaces it with dtostrfd output,
// copies any prefix/suffix text around it.
static int32_t tc_sprintf_float(char *out, int outSize, const char *fmt, float fval) {
  // Find the '%' that starts a float format specifier
  const char *p = fmt;
  const char *pct = nullptr;
  while (*p) {
    if (*p == '%') {
      if (*(p + 1) == '%') { p += 2; continue; }  // skip %%
      pct = p;
      break;
    }
    p++;
  }
  if (!pct) {
    // No format specifier found — just copy the format string as-is
    strncpy(out, fmt, outSize - 1);
    out[outSize - 1] = '\0';
    return strlen(out);
  }

  // Parse precision from %[width][.prec]f/e/g
  const char *sp = pct + 1;  // skip '%'
  // Skip flags: -, +, space, 0, #
  while (*sp == '-' || *sp == '+' || *sp == ' ' || *sp == '0' || *sp == '#') sp++;
  // Skip width digits
  while (*sp >= '0' && *sp <= '9') sp++;
  // Parse precision
  int prec = 2;  // default precision
  if (*sp == '.') {
    sp++;
    prec = 0;
    while (*sp >= '0' && *sp <= '9') {
      prec = prec * 10 + (*sp - '0');
      sp++;
    }
  }
  // Skip conversion char (f, e, E, g, G)
  if (*sp == 'f' || *sp == 'e' || *sp == 'E' || *sp == 'g' || *sp == 'G') sp++;
  // sp now points past the format specifier

  // Convert float to string
  char fbuf[32];
  dtostrfd((double)fval, prec, fbuf);

  // Build result: prefix + fbuf + suffix
  int pos = 0;
  // Copy prefix (text before %)
  for (const char *c = fmt; c < pct && pos < outSize - 1; c++) {
    out[pos++] = *c;
  }
  // Copy float string
  for (const char *c = fbuf; *c && pos < outSize - 1; c++) {
    out[pos++] = *c;
  }
  // Copy suffix (text after format specifier)
  for (const char *c = sp; *c && pos < outSize - 1; c++) {
    out[pos++] = *c;
  }
  out[pos] = '\0';
  return pos;
}

// Forward declarations (defined after tc_vm_load)
static void tc_persist_save(TcVM *vm);
static void tc_persist_load(TcVM *vm);

/*********************************************************************************************\
 * Helper: parse timestamp string (ISO or German) to epoch seconds (for time functions)
\*********************************************************************************************/
static uint32_t tc_parse_ts(const char *ts) {
  struct tm tmx;
  memset(&tmx, 0, sizeof(tmx));
  if (strchr(ts, 'T')) {
    // ISO: 2024-01-15T12:30:45
    char *p = (char*)ts;
    int Y = strtol(p, &p, 10); p++;
    int M = strtol(p, &p, 10); p++;
    int D = strtol(p, &p, 10); p++;
    int h = strtol(p, &p, 10); p++;
    int m = strtol(p, &p, 10); p++;
    int s = strtol(p, &p, 10);
    tmx.tm_year = Y - 1900;
    tmx.tm_mon  = M - 1;
    tmx.tm_mday = D;
    tmx.tm_hour = h;
    tmx.tm_min  = m;
    tmx.tm_sec  = s;
  } else if (strchr(ts, '.')) {
    char *p = (char*)ts;
    int D = strtol(p, &p, 10); p++;
    int M = strtol(p, &p, 10); p++;
    int Y = strtol(p, &p, 10); p++;
    int h = strtol(p, &p, 10); p++;
    int m = strtol(p, &p, 10);
    if (Y < 100) Y += 2000;
    tmx.tm_year = Y - 1900;
    tmx.tm_mon  = M - 1;
    tmx.tm_mday = D;
    tmx.tm_hour = h;
    tmx.tm_min  = m;
    tmx.tm_sec  = 0;
  } else {
    return 0;
  }
  return (uint32_t)mktime(&tmx);
}

/*********************************************************************************************\
 * Helper: fast timestamp comparison value — NO mktime(), just bit-packed ordering
 * Returns packed uint32: YYYYYYY MMMM DDDDD HHHHH MMMMMM (27 bits)
 * Correct ordering for valid dates without expensive calendar math
\*********************************************************************************************/
static uint32_t tc_ts_cmp(const char *ts) {
  char *p = (char*)ts;
  uint32_t Y, M, D, h = 0, m = 0;
  if (*p >= '2' && *(p + 4) == '-') {
    // ISO: 2024-01-15T12:30:45 (starts with 2xxx-)
    Y = strtol(p, &p, 10) - 2000; p++;
    M = strtol(p, &p, 10); p++;
    D = strtol(p, &p, 10); p++;
    h = strtol(p, &p, 10); p++;
    m = strtol(p, &p, 10);
  } else if (*p >= '0' && *p <= '9') {
    // German: 13.12.21 00:00 (starts with day)
    D = strtol(p, &p, 10); if (*p) p++;
    M = strtol(p, &p, 10); if (*p) p++;
    Y = strtol(p, &p, 10); if (*p) p++;
    h = strtol(p, &p, 10); if (*p) p++;
    m = strtol(p, &p, 10);
    if (Y >= 2000) Y -= 2000;
  } else {
    return 0;  // not a timestamp (header line)
  }
  return (Y << 20) | (M << 16) | (D << 11) | (h << 6) | m;
}

/*********************************************************************************************\
 * VM: GPIO pin safety check — halt VM on forbidden/in-use pins
\*********************************************************************************************/

static bool tc_pin_forbidden(int32_t pin) {
  if (pin < 0 || pin >= MAX_GPIO_PIN) return true;
  if (FlashPin(pin)) return true;
  if (RedPin(pin)) return true;
  if (TasmotaGlobal.gpio_pin[pin] != 0) return true;  // claimed by Tasmota peripheral
  return false;
}

#define TC_CHECK_PIN(vm, pin) \
  if (tc_pin_forbidden(pin)) { \
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: HALT — forbidden pin %d (flash:%d red:%d tasmota:%d)"), \
      (int)(pin), FlashPin(pin), RedPin(pin), \
      (pin >= 0 && pin < MAX_GPIO_PIN) ? TasmotaGlobal.gpio_pin[pin] : -1); \
    (vm)->error = TC_ERR_FORBIDDEN_PIN; \
    (vm)->halted = true; \
    return TC_ERR_FORBIDDEN_PIN; \
  }

/*********************************************************************************************\
 * VM: Syscall dispatch
\*********************************************************************************************/

static int tc_syscall(TcVM *vm, uint8_t id) {
  int32_t a, b;
  float fa;

  switch (id) {
    // ── GPIO (with bounds check) ────────────────────────
    case SYS_PIN_MODE:
      b = TC_POP(vm); a = TC_POP(vm);
      TC_CHECK_PIN(vm, a);
      pinMode(a, b);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: pinMode(%d, %d)"), a, b);
      break;
    case SYS_DIGITAL_WRITE:
      b = TC_POP(vm); a = TC_POP(vm);
      TC_CHECK_PIN(vm, a);
      digitalWrite(a, b);
      break;
    case SYS_DIGITAL_READ:
      a = TC_POP(vm);
      if (a >= 0 && a < MAX_GPIO_PIN) {
        TC_PUSH(vm, digitalRead(a));
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    case SYS_ANALOG_READ:
      a = TC_POP(vm);
#ifdef ESP32
      if (a >= 0 && a < MAX_GPIO_PIN) {
        TC_PUSH(vm, analogRead(a));
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, analogRead(A0));  // ESP8266 only has A0
#endif
      break;
    case SYS_ANALOG_WRITE:
      b = TC_POP(vm); a = TC_POP(vm);
      TC_CHECK_PIN(vm, a);
      analogWrite(a, b);
      break;
    case SYS_GPIO_INIT:
      // Release pin from Tasmota GPIO management and set mode
      // gpio_init() is intentional override — still block flash/red pins
      b = TC_POP(vm); a = TC_POP(vm);  // pin, mode
      if (a < 0 || a >= MAX_GPIO_PIN || FlashPin(a) || RedPin(a)) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: HALT — gpio_init forbidden pin %d"), (int)a);
        vm->error = TC_ERR_FORBIDDEN_PIN;
        vm->halted = true;
        return TC_ERR_FORBIDDEN_PIN;
      }
      // Detach from Tasmota function assignment
      if (TasmotaGlobal.gpio_pin[a] != AGPIO(GPIO_NONE)) {
        TasmotaGlobal.gpio_pin[a] = AGPIO(GPIO_NONE);
      }
      pinMode(a, b);
      break;

    // ── 1-Wire (using Tasmota OneWire library) ────────
    // Delegates to the proven TasmotaOneWire library which uses
    // direct GPIO register access and critical sections.
    case SYS_OW_SET_PIN: {
      a = TC_POP(vm);
      TC_CHECK_PIN(vm, a);
      if (TasmotaGlobal.gpio_pin[a] != AGPIO(GPIO_NONE)) {
        TasmotaGlobal.gpio_pin[a] = AGPIO(GPIO_NONE);
      }
      if (vm->ow_bus) { delete vm->ow_bus; vm->ow_bus = nullptr; }
      vm->ow_bus = new OneWire(a);
      vm->ow_pin = a;
      // Debug: manual 1-Wire test at C level
      {
        uint8_t rst = vm->ow_bus->reset();
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: owDbg reset=%d"), rst);
        if (rst) {
          // Manual search: write SEARCH_ROM, read first 3 bit-pairs
          vm->ow_bus->write(0xF0); // SEARCH_ROM
          for (int b = 0; b < 3; b++) {
            uint8_t id = vm->ow_bus->read_bit();
            uint8_t cmp = vm->ow_bus->read_bit();
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: owDbg bit%d: id=%d cmp=%d"), b, id, cmp);
            vm->ow_bus->write_bit(id); // follow the id direction
          }
        }
        // Now test library search
        vm->ow_bus->reset_search();
        uint8_t taddr[8];
        uint8_t tf = vm->ow_bus->search(taddr);
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: owDbg search=%d fam=0x%02X"), tf, tf ? taddr[0] : 0);
        vm->ow_bus->reset_search();
      }
      break;
    }
    case SYS_OW_RESET: {
      if (!vm->ow_bus) { TC_PUSH(vm, 0); break; }
      TC_PUSH(vm, vm->ow_bus->reset());
      break;
    }
    case SYS_OW_WRITE: {
      a = TC_POP(vm);
      if (!vm->ow_bus) break;
      vm->ow_bus->write(a & 0xFF);
      break;
    }
    case SYS_OW_READ: {
      if (!vm->ow_bus) { TC_PUSH(vm, 0); break; }
      TC_PUSH(vm, vm->ow_bus->read());
      break;
    }
    case SYS_OW_WRITE_BIT: {
      a = TC_POP(vm);
      if (!vm->ow_bus) break;
      vm->ow_bus->write_bit(a & 1);
      break;
    }
    case SYS_OW_READ_BIT: {
      if (!vm->ow_bus) { TC_PUSH(vm, 1); break; }
      TC_PUSH(vm, vm->ow_bus->read_bit());
      break;
    }
    case SYS_OW_SEARCH_RESET: {
      if (vm->ow_bus) vm->ow_bus->reset_search();
      break;
    }
    case SYS_OW_SEARCH: {
      // owSearch(buf) — buf is char[8], receives ROM if found
      a = TC_POP(vm);  // buffer ref (strArgs encoding)
      if (!vm->ow_bus) { TC_PUSH(vm, 0); break; }
      int32_t *buf_ptr = tc_resolve_ref(vm, a);
      if (!buf_ptr) { TC_PUSH(vm, 0); break; }
      uint8_t addr[8];
      uint8_t found = vm->ow_bus->search(addr);
      if (found) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: owSearch found fam=0x%02X"), addr[0]);
        for (int i = 0; i < 8; i++) {
          buf_ptr[i] = (int32_t)addr[i];
        }
      } else {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: owSearch done (no more)"));
      }
      TC_PUSH(vm, found ? 1 : 0);
      break;
    }

    // ── Timing ────────────────────────────────────────
    case SYS_DELAY: {
      a = TC_POP(vm);
      // Non-blocking: pause VM, set resume time
      vm->delay_until = millis() + a;
      vm->delayed = true;
      return TC_ERR_PAUSED;  // signal caller to stop executing
    }
    case SYS_DELAY_MICRO:
      a = TC_POP(vm);
      if (a < 1000) delayMicroseconds(a);  // only allow short delays
      break;
    case SYS_MILLIS:
      TC_PUSH(vm, (int32_t)millis());
      break;
    case SYS_MICROS:
      TC_PUSH(vm, (int32_t)micros());
      break;
    case SYS_TIMER_START: {
      a = TC_POP(vm);  // ms
      b = TC_POP(vm);  // id
      if (b >= 0 && b < TC_MAX_TIMERS) {
        vm->timer_deadline[b] = millis() + (uint32_t)a;
        vm->timer_active[b] = true;
      }
      break;
    }
    case SYS_TIMER_DONE: {
      a = TC_POP(vm);  // id
      int32_t result = 1;  // default: done (not started)
      if (a >= 0 && a < TC_MAX_TIMERS && vm->timer_active[a]) {
        result = ((int32_t)(millis() - vm->timer_deadline[a]) >= 0) ? 1 : 0;
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_TIMER_STOP: {
      a = TC_POP(vm);  // id
      if (a >= 0 && a < TC_MAX_TIMERS) {
        vm->timer_active[a] = false;
      }
      break;
    }
    case SYS_TIMER_REMAINING: {
      a = TC_POP(vm);  // id
      int32_t remaining = 0;
      if (a >= 0 && a < TC_MAX_TIMERS && vm->timer_active[a]) {
        int32_t diff = (int32_t)(vm->timer_deadline[a] - millis());
        remaining = (diff > 0) ? diff : 0;
      }
      TC_PUSH(vm, remaining);
      break;
    }

    // ── Serial ─────────────────────────────────────────
    case SYS_SERIAL_BEGIN: {
      // serialBegin(rxpin, txpin, baud, config, bufsize) -> int
      int32_t bufsize = TC_POP(vm);
      int32_t config  = TC_POP(vm);
      int32_t baud    = TC_POP(vm);
      int32_t txpin   = TC_POP(vm);
      int32_t rxpin   = TC_POP(vm);
      if (tc_serial_port) {
        tc_serial_close();  // close previous instance
      }
      if (bufsize < 64) bufsize = 64;
      if (bufsize > 2048) bufsize = 2048;
      if (config < 0 || config > 23) config = 3;  // default 8N1
#ifdef ESP32
      if (Is_gpio_used(rxpin) || Is_gpio_used(txpin)) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial warning — pins %d/%d may be in use"), rxpin, txpin);
      }
#endif
      tc_serial_port = new TasmotaSerial(rxpin, txpin, HARDWARE_FALLBACK, 0, bufsize);
      if (tc_serial_port) {
        if (tc_serial_port->begin(baud, ConvertSerialConfig(config))) {
          if (tc_serial_port->hardwareSerial()) {
            ClaimSerial();
          }
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial opened rx=%d tx=%d baud=%d cfg=%d buf=%d hw=%d"),
                 rxpin, txpin, baud, config, bufsize, tc_serial_port->hardwareSerial());
          TC_PUSH(vm, 1);
        } else {
          delete tc_serial_port;
          tc_serial_port = nullptr;
          AddLog(LOG_LEVEL_ERROR, PSTR("TCC: serial begin failed"));
          TC_PUSH(vm, -1);
        }
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: serial alloc failed"));
        TC_PUSH(vm, -1);
      }
      break;
    }
    case SYS_SERIAL_CLOSE:
      tc_serial_close();
      break;
    case SYS_SERIAL_PRINT:
      a = TC_POP(vm);
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        const char *s = vm->constants[a].str.ptr;
        if (tc_serial_port) {
          tc_serial_port->write(s, strlen(s));
        } else {
          tc_output_string(s);
        }
      }
      break;
    case SYS_SERIAL_PRINT_INT: {
      a = TC_POP(vm);
      char ibuf[16];
      itoa(a, ibuf, 10);
      if (tc_serial_port) {
        tc_serial_port->write(ibuf, strlen(ibuf));
      } else {
        tc_output_int(a);
      }
      break;
    }
    case SYS_SERIAL_PRINT_FLT: {
      fa = TC_POPF(vm);
      char fbuf[24];
      dtostrf(fa, 1, 2, fbuf);
      if (tc_serial_port) {
        tc_serial_port->write(fbuf, strlen(fbuf));
      } else {
        tc_output_float(fa);
      }
      break;
    }
    case SYS_SERIAL_PRINTLN:
      a = TC_POP(vm);
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        const char *s = vm->constants[a].str.ptr;
        if (tc_serial_port) {
          tc_serial_port->write(s, strlen(s));
          tc_serial_port->write("\r\n", 2);
        } else {
          tc_output_string(s);
          tc_output_char('\n');
        }
      } else {
        if (tc_serial_port) {
          tc_serial_port->write("\r\n", 2);
        } else {
          tc_output_char('\n');
        }
      }
      break;
    case SYS_SERIAL_READ:
      if (tc_serial_port && tc_serial_port->available()) {
        TC_PUSH(vm, tc_serial_port->read());
      } else {
        TC_PUSH(vm, -1);
      }
      break;
    case SYS_SERIAL_AVAILABLE:
      if (tc_serial_port) {
        TC_PUSH(vm, tc_serial_port->available());
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    case SYS_SERIAL_WRITE_BYTE:
      a = TC_POP(vm);
      if (tc_serial_port) {
        tc_serial_port->write((uint8_t)(a & 0xFF));
      }
      break;
    case SYS_SERIAL_WRITE_STR: {
      a = TC_POP(vm);  // buf_ref
      if (tc_serial_port) {
        char tbuf[256];
        int len = tc_ref_to_cstr(vm, a, tbuf, sizeof(tbuf));
        if (len > 0) {
          tc_serial_port->write(tbuf, len);
        }
      }
      break;
    }
    case SYS_SERIAL_WRITE_BUF: {
      // serialWriteBytes(buf_ref, len) — write exactly len bytes (binary safe)
      b = TC_POP(vm);  // len
      a = TC_POP(vm);  // buf_ref
      if (tc_serial_port && b > 0 && b <= 256) {
        int32_t *buf = tc_resolve_ref(vm, a);
        if (buf) {
          int32_t maxLen = tc_ref_maxlen(vm, a);
          if (b > maxLen) b = maxLen;
          uint8_t tbuf[256];
          for (int i = 0; i < b; i++) {
            tbuf[i] = (uint8_t)(buf[i] & 0xFF);
          }
          tc_serial_port->write(tbuf, b);
        }
      }
      break;
    }

    // ── Math ──────────────────────────────────────────
    case SYS_MATH_ABS:
      a = TC_POP(vm); TC_PUSH(vm, a < 0 ? -a : a); break;
    case SYS_MATH_MIN:
      b = TC_POP(vm); a = TC_POP(vm); TC_PUSH(vm, a < b ? a : b); break;
    case SYS_MATH_MAX:
      b = TC_POP(vm); a = TC_POP(vm); TC_PUSH(vm, a > b ? a : b); break;
    case SYS_MATH_MAP: {
      int32_t toHi = TC_POP(vm), toLo = TC_POP(vm);
      int32_t fromHi = TC_POP(vm), fromLo = TC_POP(vm);
      int32_t val = TC_POP(vm);
      if (fromHi == fromLo) { TC_PUSH(vm, toLo); break; }
      TC_PUSH(vm, toLo + (val - fromLo) * (toHi - toLo) / (fromHi - fromLo));
      break;
    }
    case SYS_MATH_RANDOM:
      b = TC_POP(vm); a = TC_POP(vm);
      TC_PUSH(vm, a + (random(b - a)));
      break;
    case SYS_MATH_SQRT:
      fa = TC_POPF(vm); TC_PUSHF(vm, sqrtf(fa)); break;
    case SYS_MATH_SIN:
      fa = TC_POPF(vm); TC_PUSHF(vm, sinf(fa)); break;
    case SYS_MATH_COS:
      fa = TC_POPF(vm); TC_PUSHF(vm, cosf(fa)); break;
    case SYS_MATH_FLOOR:
      fa = TC_POPF(vm); TC_PUSH(vm, (int32_t)floorf(fa)); break;
    case SYS_MATH_CEIL:
      fa = TC_POPF(vm); TC_PUSH(vm, (int32_t)ceilf(fa)); break;
    case SYS_MATH_ROUND:
      fa = TC_POPF(vm); TC_PUSH(vm, (int32_t)roundf(fa)); break;
    case SYS_MATH_EXP:
      fa = TC_POPF(vm); TC_PUSHF(vm, expf(fa)); break;
    case SYS_MATH_LOG:
      fa = TC_POPF(vm); TC_PUSHF(vm, logf(fa)); break;
    case SYS_INT_BITS_TO_FLOAT:
      // Identity: int32 bits ARE the float — just leave on stack
      // Compiler knows return type is float, so subsequent ops use FADD/FMUL etc.
      break;

    // ── Tasmota-specific ──────────────────────────────
    case SYS_MQTT_PUBLISH:
      // From task context, only AddLog is safe; MQTT publish is not thread-safe
      tc_output_flush();
      break;
    case SYS_GET_POWER:
      a = TC_POP(vm);  // relay index (1-based)
      TC_PUSH(vm, bitRead(TasmotaGlobal.power, a - 1));
      break;
    case SYS_SET_POWER:
      b = TC_POP(vm); a = TC_POP(vm);  // relay, state
      ExecuteCommandPower(a, b, SRC_IGNORE);
      break;
    case SYS_TASM_CMD: {
      int32_t buf_ref = TC_POP(vm);    // output buffer array ref
      int32_t ci = TC_POP(vm);         // const pool index for command
      const char *cmd = tc_get_const_str(vm, ci);
      int32_t *buf = tc_resolve_ref(vm, buf_ref);
      if (!cmd || !buf) {
        TC_PUSH(vm, -1);
        break;
      }
      // Cap to remaining slots from base index (leave room for null terminator)
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref) - 1;
      if (maxLen <= 0) { TC_PUSH(vm, 0); break; }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: tasmCmd(\"%s\")"), cmd);
      // Execute Tasmota command — response goes to global buffer
      ExecuteCommand((char*)cmd, SRC_TCL);
      // Capture response immediately before it's overwritten
      const char *resp = ResponseData();
      int32_t rlen = strlen(resp);
      if (rlen > maxLen) rlen = maxLen;
      for (int32_t i = 0; i < rlen; i++) {
        buf[i] = (int32_t)(uint8_t)resp[i];
      }
      buf[rlen] = 0;  // null-terminate
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: tasmCmd -> %d bytes"), rlen);
      TC_PUSH(vm, rlen);
      break;
    }
    case SYS_TASM_CMD_REF: {
      int32_t buf_ref = TC_POP(vm);    // output buffer array ref
      int32_t cmd_ref = TC_POP(vm);    // command char array ref
      int32_t *buf = tc_resolve_ref(vm, buf_ref);
      int32_t *cmd_arr = tc_resolve_ref(vm, cmd_ref);
      if (!cmd_arr || !buf) {
        TC_PUSH(vm, -1);
        break;
      }
      // Extract command string from VM int32 array
      int32_t cmdMax = tc_ref_maxlen(vm, cmd_ref);
      char cmdbuf[128];
      int32_t ci = 0;
      while (cmd_arr[ci] != 0 && ci < cmdMax && ci < (int32_t)sizeof(cmdbuf) - 1) {
        cmdbuf[ci] = (char)(cmd_arr[ci] & 0xFF); ci++;
      }
      cmdbuf[ci] = '\0';
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref) - 1;
      if (maxLen <= 0) { TC_PUSH(vm, 0); break; }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: tasmCmd(\"%s\")"), cmdbuf);
      ExecuteCommand(cmdbuf, SRC_TCL);
      const char *resp = ResponseData();
      int32_t rlen = strlen(resp);
      if (rlen > maxLen) rlen = maxLen;
      for (int32_t i = 0; i < rlen; i++) {
        buf[i] = (int32_t)(uint8_t)resp[i];
      }
      buf[rlen] = 0;
      TC_PUSH(vm, rlen);
      break;
    }

    // ── String operations (all bounds-checked via tc_ref_maxlen) ──
    case SYS_STRLEN: {
      a = TC_POP(vm);  // array ref
      int32_t *p = tc_resolve_ref(vm, a);
      int32_t len = 0;
      if (p) { int32_t max = tc_ref_maxlen(vm, a); while (p[len] != 0 && len < max) len++; }
      TC_PUSH(vm, len);
      break;
    }
    case SYS_STRCPY: {
      int32_t src_ref = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      if (dst && src) {
        int32_t max = tc_ref_maxlen(vm, dst_ref) - 1;
        int32_t i = 0;
        while (src[i] != 0 && i < max) { dst[i] = src[i]; i++; }
        dst[i] = 0;
      }
      break;
    }
    case SYS_STRCAT: {
      int32_t src_ref = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      if (dst && src) {
        int32_t max = tc_ref_maxlen(vm, dst_ref) - 1;
        int32_t di = 0;
        while (dst[di] != 0 && di < max) di++;
        int32_t si = 0;
        while (src[si] != 0 && di < max) { dst[di++] = src[si++]; }
        dst[di] = 0;
      }
      break;
    }
    case SYS_STRCMP: {
      int32_t b_ref = TC_POP(vm);
      int32_t a_ref = TC_POP(vm);
      int32_t *pa = tc_resolve_ref(vm, a_ref);
      int32_t *pb = tc_resolve_ref(vm, b_ref);
      int32_t result = 0;
      if (pa && pb) {
        int32_t max_a = tc_ref_maxlen(vm, a_ref);
        int32_t max_b = tc_ref_maxlen(vm, b_ref);
        int32_t max = max_a < max_b ? max_a : max_b;
        int32_t i = 0;
        while (pa[i] != 0 && pb[i] != 0 && pa[i] == pb[i] && i < max) i++;
        result = pa[i] - pb[i];
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_STR_PRINT: {
      a = TC_POP(vm);  // array ref
      int32_t *p = tc_resolve_ref(vm, a);
      if (p) {
        int32_t max = tc_ref_maxlen(vm, a);
        int32_t i = 0;
        while (p[i] != 0 && i < max) {
          tc_output_char((char)(p[i] & 0xFF));
          i++;
        }
      }
      break;
    }
    case SYS_STRCPY_CONST: {
      int32_t ci = TC_POP(vm);  // constant pool index
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      if (dst && ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *s = vm->constants[ci].str.ptr;
        int32_t max = tc_ref_maxlen(vm, dst_ref) - 1;
        int32_t i = 0;
        while (s[i] != 0 && i < max) { dst[i] = (int32_t)(uint8_t)s[i]; i++; }
        dst[i] = 0;
      }
      break;
    }
    case SYS_STRCAT_CONST: {
      int32_t ci = TC_POP(vm);  // constant pool index
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      if (dst && ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *s = vm->constants[ci].str.ptr;
        int32_t max = tc_ref_maxlen(vm, dst_ref) - 1;
        int32_t di = 0;
        while (dst[di] != 0 && di < max) di++;
        int32_t si = 0;
        while (s[si] != 0 && di < max) { dst[di++] = (int32_t)(uint8_t)s[si++]; }
        dst[di] = 0;
      }
      break;
    }

    // ── sprintf variants ──────────────────────────────
    // All use snprintf() on device, then copy result into VM array.
    // Supports: %d %u %x %X %o %c %s %f %e %g and width/precision modifiers.
    case SYS_SPRINTF_INT: {
      int32_t val = TC_POP(vm);          // int argument
      int32_t ci  = TC_POP(vm);          // format string const index
      int32_t dst_ref = TC_POP(vm);      // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      char tmp[64];
      snprintf(tmp, sizeof(tmp), fmt, val);
      TC_PUSH(vm, tc_sprintf_to_ref(dst, maxSlots, tmp));
      break;
    }
    case SYS_SPRINTF_FLT: {
      float fval = TC_POPF(vm);          // float argument
      int32_t ci  = TC_POP(vm);          // format string const index
      int32_t dst_ref = TC_POP(vm);      // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      char tmp[64];
      tc_sprintf_float(tmp, sizeof(tmp), fmt, fval);
      TC_PUSH(vm, tc_sprintf_to_ref(dst, maxSlots, tmp));
      break;
    }
    case SYS_SPRINTF_STR: {
      int32_t src_ref = TC_POP(vm);      // source string array ref
      int32_t ci  = TC_POP(vm);          // format string const index
      int32_t dst_ref = TC_POP(vm);      // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !src || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      // Extract source string from VM int32 array into temp char buffer
      int32_t srcMax = tc_ref_maxlen(vm, src_ref);
      char srcbuf[128];
      int32_t si = 0;
      while (src[si] != 0 && si < srcMax && si < (int32_t)sizeof(srcbuf) - 1) {
        srcbuf[si] = (char)(src[si] & 0xFF);
        si++;
      }
      srcbuf[si] = '\0';
      char tmp[128];
      snprintf(tmp, sizeof(tmp), fmt, srcbuf);
      TC_PUSH(vm, tc_sprintf_to_ref(dst, maxSlots, tmp));
      break;
    }

    // ── sprintf append variants (same as above but append to existing string) ──
    case SYS_SPRINTF_INT_CAT: {
      int32_t val = TC_POP(vm);
      int32_t ci  = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      int32_t ofs = tc_strlen_ref(dst, maxSlots);
      char tmp[64];
      snprintf(tmp, sizeof(tmp), fmt, val);
      tc_sprintf_to_ref(dst + ofs, maxSlots - ofs, tmp);
      TC_PUSH(vm, ofs + (int32_t)strlen(tmp));
      break;
    }
    case SYS_SPRINTF_FLT_CAT: {
      float fval = TC_POPF(vm);
      int32_t ci  = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      int32_t ofs = tc_strlen_ref(dst, maxSlots);
      char tmp[64];
      tc_sprintf_float(tmp, sizeof(tmp), fmt, fval);
      tc_sprintf_to_ref(dst + ofs, maxSlots - ofs, tmp);
      TC_PUSH(vm, ofs + (int32_t)strlen(tmp));
      break;
    }
    case SYS_SPRINTF_STR_CAT: {
      int32_t src_ref = TC_POP(vm);
      int32_t ci  = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !src || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      int32_t ofs = tc_strlen_ref(dst, maxSlots);
      int32_t srcMax = tc_ref_maxlen(vm, src_ref);
      char srcbuf[128];
      int32_t si = 0;
      while (src[si] != 0 && si < srcMax && si < (int32_t)sizeof(srcbuf) - 1) {
        srcbuf[si] = (char)(src[si] & 0xFF); si++;
      }
      srcbuf[si] = '\0';
      char tmp[128];
      snprintf(tmp, sizeof(tmp), fmt, srcbuf);
      tc_sprintf_to_ref(dst + ofs, maxSlots - ofs, tmp);
      TC_PUSH(vm, ofs + (int32_t)strlen(tmp));
      break;
    }

    // ── sprintf with const pool source string (for #define string literals) ──
    case SYS_SPRINTF_STR_CONST: {
      int32_t src_ci  = TC_POP(vm);       // source string const index
      int32_t ci      = TC_POP(vm);       // format string const index
      int32_t dst_ref = TC_POP(vm);       // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      const char *srcStr = tc_get_const_str(vm, src_ci);
      if (!dst || !fmt || !srcStr) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      char tmp[128];
      snprintf(tmp, sizeof(tmp), fmt, srcStr);
      TC_PUSH(vm, tc_sprintf_to_ref(dst, maxSlots, tmp));
      break;
    }
    case SYS_SPRINTF_STR_CAT_CONST: {
      int32_t src_ci  = TC_POP(vm);       // source string const index
      int32_t ci      = TC_POP(vm);       // format string const index
      int32_t dst_ref = TC_POP(vm);       // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      const char *srcStr = tc_get_const_str(vm, src_ci);
      if (!dst || !fmt || !srcStr) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      int32_t ofs = tc_strlen_ref(dst, maxSlots);
      char tmp[128];
      snprintf(tmp, sizeof(tmp), fmt, srcStr);
      tc_sprintf_to_ref(dst + ofs, maxSlots - ofs, tmp);
      TC_PUSH(vm, ofs + (int32_t)strlen(tmp));
      break;
    }

    // ── File I/O (SD preferred, /ffs/ for flash, /sdfs/ for SD) ──
    case SYS_FILE_OPEN: {
#ifdef USE_UFILESYS
      int32_t mode = tc_file_mode(TC_POP(vm));  // 0/'r'=read, 1/'w'=write, 2/'a'=append
      int32_t ci = TC_POP(vm);         // const pool index for path
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      int slot = tc_alloc_file_handle();
      if (slot < 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: fileOpen no free handle"));
        TC_PUSH(vm, -1);
        break;
      }
      const char *mode_str;
      switch (mode) {
        case 0:  mode_str = "r";  tc_file_handles[slot] = fsp->open(path, "r"); break;
        case 1:  mode_str = "w";  tc_file_handles[slot] = fsp->open(path, "w"); break;
        case 2:  mode_str = "a";  tc_file_handles[slot] = fsp->open(path, "a"); break;
        default: mode_str = "?";  TC_PUSH(vm, -1); break;
      }
      if (mode > 2) break;  // invalid mode already pushed -1
      if (!tc_file_handles[slot]) {
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpen(\"%s\", %s) failed"), path, mode_str);
        TC_PUSH(vm, -1);
      } else {
        Tinyc->file_used[slot] = true;
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpen(\"%s\", %s) -> handle %d"), path, mode_str, slot);
        TC_PUSH(vm, slot);
      }
#else
      TC_POP(vm); TC_POP(vm);  // consume args
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_FILE_CLOSE: {
      int32_t h = TC_POP(vm);
      if (h >= 0 && h < TC_MAX_FILE_HANDLES && Tinyc->file_used[h]) {
        tc_file_handles[h].close();
        Tinyc->file_used[h] = false;
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileClose(%d)"), h);
        TC_PUSH(vm, 0);
      } else {
        TC_PUSH(vm, -1);
      }
      break;
    }
    case SYS_FILE_READ: {
      int32_t maxBytes = TC_POP(vm);   // max bytes to read
      int32_t buf_ref = TC_POP(vm);    // array ref for destination buffer
      int32_t h = TC_POP(vm);          // file handle
      int32_t *buf = tc_resolve_ref(vm, buf_ref);
      if (!buf || h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, -1);
        break;
      }
      // Limit to actual remaining slots from base index
      int32_t maxSlots = tc_ref_maxlen(vm, buf_ref);
      if (maxBytes > maxSlots) maxBytes = maxSlots;
      // Read via temp byte buffer (File reads bytes, VM stores int32 per element)
      uint8_t tmp[256];
      int32_t total = 0;
      while (total < maxBytes) {
        int32_t chunk = maxBytes - total;
        if (chunk > (int32_t)sizeof(tmp)) chunk = sizeof(tmp);
        int n = tc_file_handles[h].read(tmp, chunk);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
          buf[total + i] = (int32_t)tmp[i];
        }
        total += n;
      }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileRead(%d, %d) -> %d bytes"), h, maxBytes, total);
      TC_PUSH(vm, total);
      break;
    }
    case SYS_FILE_WRITE: {
      int32_t len = TC_POP(vm);        // bytes to write
      int32_t buf_ref = TC_POP(vm);    // array ref for source buffer
      int32_t h = TC_POP(vm);          // file handle
      int32_t *buf = tc_resolve_ref(vm, buf_ref);
      if (!buf || h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, -1);
        break;
      }
      int32_t maxSlots = tc_ref_maxlen(vm, buf_ref);
      if (len > maxSlots) len = maxSlots;
      // Convert int32 array elements to bytes and write
      uint8_t tmp[256];
      int32_t total = 0;
      while (total < len) {
        int32_t chunk = len - total;
        if (chunk > (int32_t)sizeof(tmp)) chunk = sizeof(tmp);
        for (int32_t i = 0; i < chunk; i++) {
          tmp[i] = (uint8_t)(buf[total + i] & 0xFF);
        }
        int n = tc_file_handles[h].write(tmp, chunk);
        if (n <= 0) break;
        total += n;
      }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileWrite(%d, %d) -> %d bytes"), h, len, total);
      TC_PUSH(vm, total);
      break;
    }
    case SYS_FILE_EXISTS: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, 0); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, 0); break; }
      bool exists = fsp->exists(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileExists(\"%s\") -> %d"), path, exists ? 1 : 0);
      TC_PUSH(vm, exists ? 1 : 0);
#else
      TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_FILE_DELETE: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      bool ok = fsp->remove(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileDelete(\"%s\") -> %d"), path, ok ? 0 : -1);
      TC_PUSH(vm, ok ? 0 : -1);
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    // ── _REF variants: path from char array (dynamic filenames) ──
    case SYS_FILE_OPEN_REF: {
#ifdef USE_UFILESYS
      int32_t mode = tc_file_mode(TC_POP(vm));  // 0/'r'=read, 1/'w'=write, 2/'a'=append
      int32_t path_ref = TC_POP(vm);
      char path[128];
      tc_ref_to_cstr(vm, path_ref, path, sizeof(path));
      if (!path[0]) { TC_PUSH(vm, -1); break; }
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      int slot = tc_alloc_file_handle();
      if (slot < 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: fileOpen no free handle"));
        TC_PUSH(vm, -1);
        break;
      }
      const char *mode_str;
      switch (mode) {
        case 0:  mode_str = "r";  tc_file_handles[slot] = fsp->open(path, "r"); break;
        case 1:  mode_str = "w";  tc_file_handles[slot] = fsp->open(path, "w"); break;
        case 2:  mode_str = "a";  tc_file_handles[slot] = fsp->open(path, "a"); break;
        default: mode_str = "?";  TC_PUSH(vm, -1); break;
      }
      if (mode > 2) break;
      if (!tc_file_handles[slot]) {
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpen(\"%s\", %s) failed"), path, mode_str);
        TC_PUSH(vm, -1);
      } else {
        Tinyc->file_used[slot] = true;
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpen(\"%s\", %s) -> handle %d"), path, mode_str, slot);
        TC_PUSH(vm, slot);
      }
#else
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_FILE_EXISTS_REF: {
#ifdef USE_UFILESYS
      int32_t path_ref = TC_POP(vm);
      char path[128];
      tc_ref_to_cstr(vm, path_ref, path, sizeof(path));
      if (!path[0]) { TC_PUSH(vm, 0); break; }
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, 0); break; }
      bool exists = fsp->exists(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileExists(\"%s\") -> %d"), path, exists ? 1 : 0);
      TC_PUSH(vm, exists ? 1 : 0);
#else
      TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_FILE_DELETE_REF: {
#ifdef USE_UFILESYS
      int32_t path_ref = TC_POP(vm);
      char path[128];
      tc_ref_to_cstr(vm, path_ref, path, sizeof(path));
      if (!path[0]) { TC_PUSH(vm, -1); break; }
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      bool ok = fsp->remove(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileDelete(\"%s\") -> %d"), path, ok ? 0 : -1);
      TC_PUSH(vm, ok ? 0 : -1);
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    // ── Directory listing (reuses file handle slots) ──
    case SYS_FILE_OPENDIR: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      int slot = tc_alloc_file_handle();
      if (slot < 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: fileOpenDir no free handle"));
        TC_PUSH(vm, -1);
        break;
      }
      tc_file_handles[slot] = fsp->open(path, "r");
      if (!tc_file_handles[slot] || !tc_file_handles[slot].isDirectory()) {
        tc_file_handles[slot].close();
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpenDir(\"%s\") not a directory"), path);
        TC_PUSH(vm, -1);
      } else {
        Tinyc->file_used[slot] = true;
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpenDir(\"%s\") -> handle %d"), path, slot);
        TC_PUSH(vm, slot);
      }
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_FILE_OPENDIR_REF: {
#ifdef USE_UFILESYS
      int32_t path_ref = TC_POP(vm);
      char path[128];
      tc_ref_to_cstr(vm, path_ref, path, sizeof(path));
      if (!path[0]) { TC_PUSH(vm, -1); break; }
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      int slot = tc_alloc_file_handle();
      if (slot < 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: fileOpenDir no free handle"));
        TC_PUSH(vm, -1);
        break;
      }
      tc_file_handles[slot] = fsp->open(path, "r");
      if (!tc_file_handles[slot] || !tc_file_handles[slot].isDirectory()) {
        tc_file_handles[slot].close();
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpenDir(\"%s\") not a directory"), path);
        TC_PUSH(vm, -1);
      } else {
        Tinyc->file_used[slot] = true;
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileOpenDir(\"%s\") -> handle %d"), path, slot);
        TC_PUSH(vm, slot);
      }
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_FILE_READDIR: {
#ifdef USE_UFILESYS
      int32_t name_ref = TC_POP(vm);
      int32_t h = TC_POP(vm);
      if (h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, 0);
        break;
      }
      File entry = tc_file_handles[h].openNextFile();
      if (!entry) {
        TC_PUSH(vm, 0);  // no more entries
        break;
      }
      // Extract just the filename (strip leading path)
      const char *ep = entry.name();
      if (*ep == '/') ep++;
      const char *lcp = strrchr(ep, '/');
      if (lcp) ep = lcp + 1;
      // Skip directories, only return files
      if (entry.isDirectory()) {
        entry.close();
        // Try next entry (recursive would be complex, just skip one)
        // For simplicity, caller should loop: while(fileReadDir(h, buf)) { ... }
        entry = tc_file_handles[h].openNextFile();
        while (entry && entry.isDirectory()) {
          entry.close();
          entry = tc_file_handles[h].openNextFile();
        }
        if (!entry) {
          TC_PUSH(vm, 0);
          break;
        }
        ep = entry.name();
        if (*ep == '/') ep++;
        lcp = strrchr(ep, '/');
        if (lcp) ep = lcp + 1;
      }
      // Write filename to name buffer
      int32_t *dst = tc_resolve_ref(vm, name_ref);
      int32_t maxLen = tc_ref_maxlen(vm, name_ref);
      int32_t slen = strlen(ep);
      if (slen >= maxLen) slen = maxLen - 1;
      for (int32_t i = 0; i < slen; i++) {
        dst[i] = (int32_t)(uint8_t)ep[i];
      }
      dst[slen] = 0;
      entry.close();
      TC_PUSH(vm, 1);  // entry found
#else
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_FILE_SIZE: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      File f = fsp->open(path, "r");
      if (!f) {
        TC_PUSH(vm, -1);
      } else {
        int32_t sz = f.size();
        f.close();
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileSize(\"%s\") -> %d"), path, sz);
        TC_PUSH(vm, sz);
      }
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    case SYS_FILE_FORMAT: {
#ifdef USE_UFILESYS
      bool ok = LittleFS.format();
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: fileFormat() -> %d"), ok);
      TC_PUSH(vm, ok ? 0 : -1);
#else
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_FILE_MKDIR: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, 0); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, 0); break; }
      bool ok = fsp->mkdir(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileMkdir(\"%s\") -> %d"), path, ok);
      TC_PUSH(vm, ok ? 1 : 0);
#else
      TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_FILE_RMDIR: {
#ifdef USE_UFILESYS
      int32_t ci = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, 0); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, 0); break; }
      bool ok = fsp->rmdir(path);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileRmdir(\"%s\") -> %d"), path, ok);
      TC_PUSH(vm, ok ? 1 : 0);
#else
      TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_FILE_SEEK: {
      // fileSeek(handle, offset, whence) -> 1=ok, 0=fail
      // whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
#ifdef USE_UFILESYS
      int32_t whence = TC_POP(vm);
      int32_t offset = TC_POP(vm);
      int32_t h = TC_POP(vm);
      if (h >= 0 && h < TC_MAX_FILE_HANDLES && Tinyc->file_used[h]) {
        SeekMode mode = SeekSet;
        if (whence == 1) mode = SeekCur;
        else if (whence == 2) mode = SeekEnd;
        bool ok = tc_file_handles[h].seek(offset, mode);
        TC_PUSH(vm, ok ? 1 : 0);
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_FILE_TELL: {
      // fileTell(handle) -> current position, -1 on error
#ifdef USE_UFILESYS
      int32_t h = TC_POP(vm);
      if (h >= 0 && h < TC_MAX_FILE_HANDLES && Tinyc->file_used[h]) {
        TC_PUSH(vm, (int32_t)tc_file_handles[h].position());
      } else {
        TC_PUSH(vm, -1);
      }
#else
      TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    case SYS_FILE_DOWNLOAD: {
      // fileDownload("filepath", char url[]) — download URL to file
#ifdef USE_UFILESYS
      int32_t urlRef = TC_POP(vm);
      int32_t ci     = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      char url[256];
      tc_ref_to_cstr(vm, urlRef, url, sizeof(url));
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      WiFiClient http_client;
      HTTPClient http;
      http.setTimeout(10000);
      http.begin(http_client, url);
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        File f = fsp->open(path, "w");
        if (f) {
          WiFiClient *stream = http.getStreamPtr();
          int32_t len = http.getSize();
          if (len < 0) len = 99999999;  // unknown size
          uint8_t buf[512];
          while (http.connected() && (len > 0)) {
            size_t avail = stream->available();
            if (avail) {
              if (avail > sizeof(buf)) avail = sizeof(buf);
              int rd = stream->readBytes(buf, avail);
              f.write(buf, rd);
              len -= rd;
            }
            delay(1);
          }
          f.close();
          AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: fileDownload(\"%s\") -> %d"), path, httpCode);
        } else {
          httpCode = -3;  // file open error
        }
      } else {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: fileDownload HTTP error %d"), httpCode);
      }
      http.end();
      http_client.stop();
      TC_PUSH(vm, httpCode);
#else
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    case SYS_FILE_GET_STR: {
      // fileGetStr(char dst[], handle, "delim", index, endChar)
      // Search file for Nth delimiter occurrence, extract string until endChar
#ifdef USE_UFILESYS
      int32_t endChar  = TC_POP(vm);
      int32_t index    = TC_POP(vm);
      int32_t ci       = TC_POP(vm);  // const pool index for delimiter
      int32_t h        = TC_POP(vm);  // file handle
      int32_t dstRef   = TC_POP(vm);  // destination buffer ref
      const char *delim = tc_get_const_str(vm, ci);
      int32_t *dst = tc_resolve_ref(vm, dstRef);
      if (!dst || !delim || h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, 0);
        break;
      }
      int32_t maxLen = tc_ref_maxlen(vm, dstRef);
      uint8_t dstrlen = strlen(delim);
      if (dstrlen == 0 || index < 1) { TC_PUSH(vm, 0); break; }
      // Phase 1: seek to start, find Nth delimiter with sliding window
      tc_file_handles[h].seek(0, SeekSet);
      char fstr[256];
      memset(fstr, 0, dstrlen);
      bool match = false;
      int32_t count = index;
      while (tc_file_handles[h].available()) {
        uint8_t c;
        if (tc_file_handles[h].read(&c, 1) != 1) break;
        memmove(&fstr[0], &fstr[1], dstrlen - 1);
        fstr[dstrlen - 1] = (char)c;
        if (!strncmp(delim, fstr, dstrlen)) {
          count--;
          if (count == 0) { match = true; break; }
        }
      }
      // Phase 2: extract content until endChar or EOF
      int32_t clen = 0;
      if (match) {
        while (tc_file_handles[h].available() && clen < maxLen - 1 && clen < (int32_t)sizeof(fstr) - 1) {
          uint8_t c;
          if (tc_file_handles[h].read(&c, 1) != 1) break;
          if (endChar > 0 && (char)c == (char)endChar) break;
          fstr[clen] = (char)c;
          clen++;
        }
      }
      fstr[clen] = 0;
      // Copy to dst (int32 per char)
      for (int32_t i = 0; i <= clen && i < maxLen; i++) {
        dst[i] = (int32_t)(uint8_t)fstr[i];
      }
      TC_PUSH(vm, clen);
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── File data extract (IoT time-series CSV) ─────
    // Optimized: tc_ts_cmp() avoids mktime(), 4KB buffered I/O
    case SYS_FILE_EXTRACT:
    case SYS_FILE_EXTRACT_FAST: {
#ifdef USE_UFILESYS
      // Pop variable-length array refs + fixed args
      int32_t numArrays = TC_POP(vm);
      int32_t arrRefs[32];
      if (numArrays > 32) numArrays = 32;
      for (int i = numArrays - 1; i >= 0; i--) {
        arrRefs[i] = TC_POP(vm);
      }
      int32_t accum    = TC_POP(vm);
      int32_t col_offs = TC_POP(vm);
      int32_t toRef    = TC_POP(vm);
      int32_t fromRef  = TC_POP(vm);
      int32_t h        = TC_POP(vm);

      if (h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, 0);
        break;
      }

      // Parse time range to fast comparison values (once, no mktime)
      char ts_from[24], ts_to[24];
      tc_ref_to_cstr(vm, fromRef, ts_from, sizeof(ts_from));
      tc_ref_to_cstr(vm, toRef, ts_to, sizeof(ts_to));
      uint32_t cmp_from = tc_ts_cmp(ts_from);
      uint32_t cmp_to   = tc_ts_cmp(ts_to);

      // Seek strategy: fast version reuses saved position
      TcSlot *_es = tc_current_slot;
      if (id == SYS_FILE_EXTRACT_FAST && _es &&
          _es->extract_handle == h &&
          _es->extract_file_pos > 0 &&
          cmp_from >= _es->extract_last_epoch) {
        tc_file_handles[h].seek(_es->extract_file_pos, SeekSet);
      } else {
        tc_file_handles[h].seek(0, SeekSet);
      }

      // Resolve array bases and limits once (avoid per-row lookups)
      int32_t *arrBase[32];
      int32_t  arrMax[32];
      for (int c = 0; c < numArrays; c++) {
        arrBase[c] = tc_resolve_ref(vm, arrRefs[c]);
        arrMax[c]  = arrBase[c] ? tc_ref_maxlen(vm, arrRefs[c]) : 0;
      }

      // Buffered I/O: read 4KB blocks, parse lines from memory
      const int FBUF_SZ = 4096;
      uint8_t *fbuf = (uint8_t*)malloc(FBUF_SZ);
      if (!fbuf) { TC_PUSH(vm, 0); break; }
      int fbuf_len = 0, fbuf_pos = 0;
      char line[512];
      int32_t rowCount = 0;
      int32_t subCount = 0;                     // sub-row counter for averaging
      int32_t avgN = (accum < -1) ? -accum : 0; // e.g., accum=-4 → avgN=4
      float prevVals[32];                        // previous row values for delta mode
      memset(prevVals, 0, sizeof(prevVals));
      bool firstRow = true;

      while (true) {
        // ── Read next line from buffer ──
        int llen = 0;
        bool got_line = false;
        while (true) {
          if (fbuf_pos >= fbuf_len) {
            fbuf_len = tc_file_handles[h].read(fbuf, FBUF_SZ);
            fbuf_pos = 0;
            if (fbuf_len <= 0) break;  // EOF
          }
          char c = (char)fbuf[fbuf_pos++];
          if (c == '\n') { got_line = true; break; }
          if (c == '\r') continue;
          if (llen < (int)sizeof(line) - 1) line[llen++] = c;
        }
        if (!got_line && llen == 0) break;  // EOF
        line[llen] = 0;

        // ── Parse timestamp in-place (first column before tab) ──
        // Find tab to null-terminate timestamp temporarily
        char *tab = line;
        while (*tab && *tab != '\t') tab++;
        char saved = *tab;
        *tab = 0;

        uint32_t cmp = tc_ts_cmp(line);  // fast: no mktime, no sscanf
        *tab = saved;  // restore

        if (cmp == 0) continue;         // skip header / invalid
        if (cmp < cmp_from) continue;   // before range
        if (cmp > cmp_to) break;        // past range — done (data is chronological)

        // ── Skip timestamp column + col_offs data columns ──
        char *p = (saved == '\t') ? tab + 1 : tab;
        for (int skip = 0; skip < col_offs && *p; skip++) {
          while (*p && *p != '\t') p++;
          if (*p == '\t') p++;
        }

        // ── Parse float values into destination arrays ──
        for (int c = 0; c < numArrays; c++) {
          float val = 0;
          if (*p) {
            val = strtof(p, &p);
            if (*p == '\t') p++;
          }
          if (accum == -1) {
            // Delta mode: output = current - previous
            float delta = firstRow ? 0 : (val - prevVals[c]);
            prevVals[c] = val;
            if (arrBase[c] && rowCount < arrMax[c]) {
              memcpy(&arrBase[c][rowCount], &delta, sizeof(float));
            }
          } else if (accum < -1) {
            // Averaging mode: accumulate avgN rows, then divide
            if (arrBase[c] && rowCount < arrMax[c]) {
              if (subCount == 0) {
                memcpy(&arrBase[c][rowCount], &val, sizeof(float));
              } else {
                float existing;
                memcpy(&existing, &arrBase[c][rowCount], sizeof(float));
                existing += val;
                memcpy(&arrBase[c][rowCount], &existing, sizeof(float));
              }
            }
          } else if (accum > 0) {
            // Simple accumulation (existing behavior)
            if (arrBase[c] && rowCount < arrMax[c]) {
              float existing;
              memcpy(&existing, &arrBase[c][rowCount], sizeof(float));
              val += existing;
              memcpy(&arrBase[c][rowCount], &val, sizeof(float));
            }
          } else {
            // Normal mode (accum == 0): direct store
            if (arrBase[c] && rowCount < arrMax[c]) {
              memcpy(&arrBase[c][rowCount], &val, sizeof(float));
            }
          }
        }
        // Row counting depends on mode
        if (accum < -1) {
          subCount++;
          if (subCount >= avgN) {
            // Divide accumulated sums by avgN
            for (int c = 0; c < numArrays; c++) {
              if (arrBase[c] && rowCount < arrMax[c]) {
                float avg;
                memcpy(&avg, &arrBase[c][rowCount], sizeof(float));
                avg /= (float)avgN;
                memcpy(&arrBase[c][rowCount], &avg, sizeof(float));
              }
            }
            rowCount++;
            subCount = 0;
          }
        } else {
          rowCount++;
          firstRow = false;
        }
      }

      // For averaging: if partial group remains, compute average of partial
      if (accum < -1 && subCount > 0) {
        for (int c = 0; c < numArrays; c++) {
          if (arrBase[c] && rowCount < arrMax[c]) {
            float avg;
            memcpy(&avg, &arrBase[c][rowCount], sizeof(float));
            avg /= (float)subCount;
            memcpy(&arrBase[c][rowCount], &avg, sizeof(float));
          }
        }
        rowCount++;
      }

      free(fbuf);

      // Save position for fileExtractFast
      if (id == SYS_FILE_EXTRACT_FAST && _es) {
        _es->extract_handle     = h;
        _es->extract_file_pos   = tc_file_handles[h].position();
        _es->extract_last_epoch = cmp_to;
      }

      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: file%s(h=%d) %s→%s → %d rows, %d cols"),
        id == SYS_FILE_EXTRACT_FAST ? "ExtractFast" : "Extract",
        h, ts_from, ts_to, rowCount, numArrays);
      TC_PUSH(vm, rowCount);
#else
      // Consume all args
      int32_t n = TC_POP(vm);
      for (int i = 0; i < n + 5; i++) TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── Heap allocation ──────────────────────────────
    case SYS_HEAP_ALLOC: {
      int32_t size = TC_POP(vm);
      TC_PUSH(vm, tc_heap_alloc(vm, (uint16_t)size));
      break;
    }
    case SYS_HEAP_FREE: {
      int32_t handle = TC_POP(vm);
      tc_heap_free_handle(vm, handle);
      break;
    }

    // ── File array I/O (tab-delimited text) ─────────
    case SYS_FILE_READ_ARR: {
#ifdef USE_UFILESYS
      int32_t handle = TC_POP(vm);
      int32_t ref    = TC_POP(vm);
      int32_t count = 0;
      if (handle >= 0 && handle < TC_MAX_FILE_HANDLES && Tinyc->file_used[handle]) {
        File &f = tc_file_handles[handle];
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base && f.available()) {
          char line[512];
          int len = f.readBytesUntil('\n', line, sizeof(line) - 1);
          line[len] = 0;
          // Parse tab/comma-delimited values
          char *p = line;
          while (*p && count < 256) {
            while (*p == '\t' || *p == ',' || *p == '\r' || *p == ' ') p++;
            if (!*p) break;
            base[count] = atoi(p);
            count++;
            while (*p && *p != '\t' && *p != ',' && *p != '\r' && *p != '\n') p++;
          }
        }
      }
      TC_PUSH(vm, count);
#else
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_FILE_WRITE_ARR: {
#ifdef USE_UFILESYS
      int32_t append = TC_POP(vm);
      int32_t handle = TC_POP(vm);
      int32_t ref    = TC_POP(vm);
      if (handle >= 0 && handle < TC_MAX_FILE_HANDLES && Tinyc->file_used[handle]) {
        File &f = tc_file_handles[handle];
        int32_t *base = tc_resolve_ref(vm, ref);
        int32_t alen  = tc_ref_maxlen(vm, ref);
        if (base && alen > 0) {
          char tmp[16];
          for (int32_t i = 0; i < alen; i++) {
            if (i > 0) f.write((uint8_t)'\t');
            int slen = snprintf(tmp, sizeof(tmp), "%d", base[i]);
            f.write((const uint8_t*)tmp, slen);
          }
          if (!append) f.write((uint8_t)'\n');
          f.flush();
        }
      }
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
#endif
      break;
    }
    case SYS_FILE_LOG: {
#ifdef USE_UFILESYS
      int32_t limit  = TC_POP(vm);
      int32_t strRef = TC_POP(vm);
      int32_t ci     = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, ci);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      char payload[256];
      tc_ref_to_cstr(vm, strRef, payload, sizeof(payload));
      // Append payload + newline
      File f = fsp->open(path, "a");
      if (!f) { TC_PUSH(vm, -1); break; }
      f.print(payload);
      f.print('\n');
      int32_t fsize = f.size();
      f.close();
      // Rotate if over limit
      if (limit > 0 && fsize > limit) {
        f = fsp->open(path, "r");
        if (f) {
          // Read entire file
          char *buf = (char*)malloc(fsize + 1);
          if (buf) {
            int rd = f.readBytes(buf, fsize);
            buf[rd] = 0;
            f.close();
            // Find first newline
            char *nl = strchr(buf, '\n');
            if (nl) {
              nl++;  // skip past newline
              // Rewrite without first line
              f = fsp->open(path, "w");
              if (f) {
                int remain = rd - (nl - buf);
                f.write((const uint8_t*)nl, remain);
                fsize = remain;
                f.close();
              }
            }
            free(buf);
          } else {
            f.close();
          }
        }
      }
      TC_PUSH(vm, fsize);
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
#endif
      break;
    }

    // ── Time / timestamp functions ─────────────────────
    case SYS_TIME_STAMP: {
      // timeStamp(char buf[]) — get current Tasmota local timestamp
      int32_t ref = TC_POP(vm);
      String ts = GetDateAndTime(DT_LOCAL);
      tc_cstr_to_ref(vm, ref, ts.c_str());
      TC_PUSH(vm, 0);
      break;
    }
    case SYS_TIME_CONVERT: {
      // timeConvert(char buf[], int flg) — convert format in-place
      // flg=0: German → Web  "15.1.24 12:30" → "2024-01-15T12:30:00"
      // flg=1: Web → German  "2024-01-15T12:30:45" → "15.1.24 12:30"
      int32_t flg = TC_POP(vm);
      int32_t ref = TC_POP(vm);
      char tmp[32];
      tc_ref_to_cstr(vm, ref, tmp, sizeof(tmp));
      if (tmp[0]) {
        struct {
          uint16_t year; uint8_t month, day, hour, mins, secs;
        } tm;
        if (strchr(tmp, 'T')) {
          char *p = tmp;
          tm.year  = strtol(p, &p, 10) - 2000; p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.day   = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10); p++;
          tm.secs  = strtol(p, &p, 10);
        } else {
          char *p = tmp;
          tm.day   = strtol(p, &p, 10); p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.year  = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10);
          tm.secs  = 0;
        }
        if (flg & 1) {
          sprintf(tmp, "%d.%d.%d %d:%02d", tm.day, tm.month, tm.year, tm.hour, tm.mins);
        } else {
          sprintf(tmp, "%04d-%02d-%02dT%02d:%02d:%02d", tm.year + 2000, tm.month, tm.day, tm.hour, tm.mins, tm.secs);
        }
        tc_cstr_to_ref(vm, ref, tmp);
      }
      TC_PUSH(vm, 0);
      break;
    }
    case SYS_TIME_OFFSET: {
      // timeOffset(char buf[], int days [, int zeroFlag])
      // add day offset, optionally zero time portion
      int32_t zflag = TC_POP(vm);
      int32_t days  = TC_POP(vm);
      int32_t ref   = TC_POP(vm);
      char tmp[32];
      tc_ref_to_cstr(vm, ref, tmp, sizeof(tmp));
      if (tmp[0]) {
        struct {
          uint16_t year; uint8_t month, day, hour, mins, secs;
        } tm;
        uint8_t mode = 0;
        if (strchr(tmp, 'T')) {
          char *p = tmp;
          tm.year  = strtol(p, &p, 10) - 2000; p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.day   = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10); p++;
          tm.secs  = strtol(p, &p, 10);
          mode = 0;
        } else {
          char *p = tmp;
          tm.day   = strtol(p, &p, 10); p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.year  = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10);
          tm.secs  = 0;
          mode = 1;
        }
        struct tm tmx;
        tmx.tm_sec  = tm.secs;
        tmx.tm_min  = tm.mins;
        tmx.tm_hour = tm.hour;
        tmx.tm_mon  = tm.month - 1;
        tmx.tm_year = tm.year + 100;
        tmx.tm_mday = tm.day;
        time_t tmd = mktime(&tmx);
        tmd += days * 86400;
        struct tm *tmr = gmtime(&tmd);
        if (zflag) {
          tm.secs = 0; tm.mins = 0; tm.hour = 0;
        } else {
          tm.secs = tmr->tm_sec;
          tm.mins = tmr->tm_min;
          tm.hour = tmr->tm_hour;
        }
        tm.month = tmr->tm_mon + 1;
        tm.year  = tmr->tm_year - 100;
        tm.day   = tmr->tm_mday;
        if (mode & 1) {
          sprintf(tmp, "%d.%d.%d %d:%02d", tm.day, tm.month, tm.year, tm.hour, tm.mins);
        } else {
          sprintf(tmp, "%04d-%02d-%02dT%02d:%02d:%02d", tm.year + 2000, tm.month, tm.day, tm.hour, tm.mins, tm.secs);
        }
        tc_cstr_to_ref(vm, ref, tmp);
      }
      TC_PUSH(vm, 0);
      break;
    }
    case SYS_TIME_TO_SECS: {
      // timeToSecs(char buf[]) — parse timestamp, return epoch seconds
      int32_t ref = TC_POP(vm);
      char tmp[32];
      tc_ref_to_cstr(vm, ref, tmp, sizeof(tmp));
      int32_t secs = 0;
      if (tmp[0]) {
        struct {
          uint16_t year; uint8_t month, day, hour, mins, secs;
        } tm;
        if (strchr(tmp, 'T')) {
          char *p = tmp;
          tm.year  = strtol(p, &p, 10) - 2000; p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.day   = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10); p++;
          tm.secs  = strtol(p, &p, 10);
        } else {
          char *p = tmp;
          tm.day   = strtol(p, &p, 10); p++;
          tm.month = strtol(p, &p, 10); p++;
          tm.year  = strtol(p, &p, 10); p++;
          tm.hour  = strtol(p, &p, 10); p++;
          tm.mins  = strtol(p, &p, 10);
          tm.secs  = 0;
        }
        struct tm tmx;
        tmx.tm_sec  = tm.secs;
        tmx.tm_min  = tm.mins;
        tmx.tm_hour = tm.hour;
        tmx.tm_mon  = tm.month - 1;
        tmx.tm_year = tm.year + 100;
        tmx.tm_mday = tm.day;
        secs = (int32_t)mktime(&tmx);
      }
      TC_PUSH(vm, secs);
      break;
    }
    case SYS_SECS_TO_TIME: {
      // secsToTime(char buf[], int secs) — format epoch seconds as timestamp
      int32_t secs = TC_POP(vm);
      int32_t ref  = TC_POP(vm);
      time_t tmd = (time_t)secs;
      struct tm *tmr = gmtime(&tmd);
      char tmp[32];
      sprintf(tmp, "%04d-%02d-%02dT%02d:%02d:%02d",
        tmr->tm_year + 1900, tmr->tm_mon + 1, tmr->tm_mday,
        tmr->tm_hour, tmr->tm_min, tmr->tm_sec);
      tc_cstr_to_ref(vm, ref, tmp);
      TC_PUSH(vm, 0);
      break;
    }

    // ── Tasmota output (for callbacks) ────────────────
    // Streams VM string in 256-byte chunks — no size limit
    case SYS_RESPONSE_APPEND: {
      a = TC_POP(vm);
      tc_stream_ref(vm, a, tc_send_response);
      break;
    }
    case SYS_WEB_SEND: {
      a = TC_POP(vm);
#ifdef USE_WEBSERVER
      tc_stream_ref(vm, a, tc_send_web);
#endif
      break;
    }
    case SYS_WEB_FLUSH: {
#ifdef USE_WEBSERVER
      WSContentFlush();
#endif
      break;
    }

    // ── Tasmota output — string literal variants ──────
    case SYS_RESPONSE_APPEND_STR: {
      a = TC_POP(vm);  // constant pool index
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        ResponseAppend_P(PSTR("%s"), vm->constants[a].str.ptr);
      }
      break;
    }
    case SYS_WEB_SEND_STR: {
      a = TC_POP(vm);  // constant pool index
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
#ifdef USE_WEBSERVER
        int slen = strlen(vm->constants[a].str.ptr);
        WSContentSend(vm->constants[a].str.ptr, slen);
#endif
      }
      break;
    }

    case SYS_LOG: {
      a = TC_POP(vm);  // char array ref
      char tmp[TC_OUTPUT_SIZE];
      if (tc_ref_to_cstr(vm, a, tmp, sizeof(tmp)) > 0) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s"), tmp);
      }
      break;
    }
    case SYS_LOG_STR: {
      a = TC_POP(vm);  // constant pool index
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s"), vm->constants[a].str.ptr);
      }
      break;
    }

    // ── Localized strings ─────────────────────────────
    case SYS_LGETSTRING: {
      // LGetString(index, char dst[]) -> int length
      a = TC_POP(vm);   // dst char array ref
      b = TC_POP(vm);   // string index
      static const char tc_ls_0[] PROGMEM = D_TEMPERATURE;
      static const char tc_ls_1[] PROGMEM = D_HUMIDITY;
      static const char tc_ls_2[] PROGMEM = D_PRESSURE;
      static const char tc_ls_3[] PROGMEM = D_DEWPOINT;
      static const char tc_ls_4[] PROGMEM = D_CO2;
      static const char tc_ls_5[] PROGMEM = D_ECO2;
      static const char tc_ls_6[] PROGMEM = D_TVOC;
      static const char tc_ls_7[] PROGMEM = D_VOLTAGE;
      static const char tc_ls_8[] PROGMEM = D_CURRENT;
      static const char tc_ls_9[] PROGMEM = D_POWERUSAGE;
      static const char tc_ls_10[] PROGMEM = D_POWER_FACTOR;
      static const char tc_ls_11[] PROGMEM = D_ENERGY_TODAY;
      static const char tc_ls_12[] PROGMEM = D_ENERGY_YESTERDAY;
      static const char tc_ls_13[] PROGMEM = D_ENERGY_TOTAL;
      static const char tc_ls_14[] PROGMEM = D_FREQUENCY;
      static const char tc_ls_15[] PROGMEM = D_ILLUMINANCE;
      static const char tc_ls_16[] PROGMEM = D_DISTANCE;
      static const char tc_ls_17[] PROGMEM = D_MOISTURE;
      static const char tc_ls_18[] PROGMEM = D_LIGHT;
      static const char tc_ls_19[] PROGMEM = D_SPEED;
      static const char tc_ls_20[] PROGMEM = D_ABSOLUTE_HUMIDITY;
      static const char *const tc_lstrings[] PROGMEM = {
        tc_ls_0, tc_ls_1, tc_ls_2, tc_ls_3, tc_ls_4,
        tc_ls_5, tc_ls_6, tc_ls_7, tc_ls_8, tc_ls_9,
        tc_ls_10, tc_ls_11, tc_ls_12, tc_ls_13, tc_ls_14,
        tc_ls_15, tc_ls_16, tc_ls_17, tc_ls_18, tc_ls_19,
        tc_ls_20,
      };
      #define TC_LSTRING_COUNT 21
      if (b >= 0 && b < TC_LSTRING_COUNT) {
        char tmp[32];
        const char *p = (const char *)pgm_read_ptr(&tc_lstrings[b]);
        strncpy_P(tmp, p, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';
        int len = strlen(tmp);
        tc_cstr_to_ref(vm, a, tmp);
        TC_PUSH(vm, len);
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }

    // ── UDP multicast ─────────────────────────────────
    case SYS_UDP_SEND: {
      // Stack: [const_idx_name, float_bits]  — float on top
      a = TC_POP(vm);  // float value (as int32 bits)
      b = TC_POP(vm);  // const pool index for variable name
      if (b >= 0 && b < vm->const_count && vm->constants[b].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        float fv = i2f(a);
        tc_udp_send(vm->constants[b].str.ptr, fv);
      }
      break;
    }
    case SYS_UDP_RECV: {
      a = TC_POP(vm);  // const pool index for variable name
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        TcUdpVar *var = tc_udp_find_var(vm->constants[a].str.ptr, true);
        if (var) {
          TC_PUSH(vm, f2i(var->value));
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }
    case SYS_UDP_READY: {
      a = TC_POP(vm);  // const pool index for variable name
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        TcUdpVar *var = tc_udp_find_var(vm->constants[a].str.ptr, true);
        if (var && var->ready) {
          var->ready = false;
          TC_PUSH(vm, 1);
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }

    // ── UDP array send/receive ────────────────────────
    case SYS_UDP_SEND_ARRAY: {
      // Stack: [const_idx_name, arr_ref, count] — count on top
      int32_t count = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);
      b = TC_POP(vm);  // const pool index for variable name
      if (b >= 0 && b < vm->const_count && vm->constants[b].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        int32_t *arr = tc_resolve_ref(vm, arr_ref);
        int32_t maxLen = tc_ref_maxlen(vm, arr_ref);
        if (arr && count > 0) {
          if (count > maxLen) count = maxLen;
          if (count > TC_UDP_MAX_ARRAY) count = TC_UDP_MAX_ARRAY;
          // Convert int32 (float bits) to float array on stack
          float fbuf[TC_UDP_MAX_ARRAY];
          for (int32_t i = 0; i < count; i++) {
            fbuf[i] = i2f(arr[i]);
          }
          tc_udp_send_array(vm->constants[b].str.ptr, fbuf, (uint16_t)count);
        }
      }
      break;
    }
    case SYS_UDP_RECV_ARRAY: {
      // Stack: [const_idx_name, arr_ref, maxcount] — maxcount on top
      int32_t maxcount = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);
      a = TC_POP(vm);  // const pool index for variable name
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        TcUdpVar *var = tc_udp_find_var(vm->constants[a].str.ptr, true);  // create slot to register interest
        if (var && var->arr_data && var->arr_count > 0) {
          int32_t *arr = tc_resolve_ref(vm, arr_ref);
          int32_t maxLen = tc_ref_maxlen(vm, arr_ref);
          if (arr) {
            int32_t n = var->arr_count;
            if (n > maxcount) n = maxcount;
            if (n > maxLen) n = maxLen;
            for (int32_t i = 0; i < n; i++) {
              arr[i] = f2i(var->arr_data[i]);
            }
            TC_PUSH(vm, n);
          } else {
            TC_PUSH(vm, 0);
          }
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }

    case SYS_UDP_SEND_STR: {
      // Stack: [const_idx_name, str_ref] — str_ref on top
      int32_t str_ref = TC_POP(vm);
      b = TC_POP(vm);  // const pool index for variable name
      if (b >= 0 && b < vm->const_count && vm->constants[b].type == 1) {
        if (!Tinyc->udp_used) {
          Tinyc->udp_used = true;
          tc_udp_init();
        }
        int32_t *arr = tc_resolve_ref(vm, str_ref);
        int32_t maxLen = tc_ref_maxlen(vm, str_ref);
        if (arr && maxLen > 0) {
          // Convert char array (1 int32 per char) to C string
          char sbuf[256];
          int32_t slen = 0;
          for (int32_t i = 0; i < maxLen && i < 255; i++) {
            if (arr[i] == 0) break;
            sbuf[slen++] = (char)arr[i];
          }
          sbuf[slen] = '\0';
          tc_udp_send_str(vm->constants[b].str.ptr, sbuf);
        }
      }
      break;
    }

    // ── General-purpose UDP (Scripter-compatible udp() function) ──
    case SYS_UDP_FUNC: {
      int32_t mode = TC_POP(vm);
      switch (mode) {
        case 0: { // udp(0, port) → open listening port
          int32_t port = TC_POP(vm);
          if (port > 0 && port < 65536) {
            Tinyc->udp_port.stop();
            if (Tinyc->udp_port.begin(port)) {
              Tinyc->udp_port_num = port;
              Tinyc->udp_port_open = true;
              AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP port %d opened"), port);
              TC_PUSH(vm, 1);
            } else {
              Tinyc->udp_port_open = false;
              TC_PUSH(vm, 0);
            }
          } else {
            TC_PUSH(vm, 0);
          }
          break;
        }
        case 1: { // udp(1, buf) → read string from UDP
          int32_t buf_ref = TC_POP(vm);
          int32_t result = 0;
          if (Tinyc->udp_port_open && !TasmotaGlobal.global_state.network_down) {
            int32_t plen = Tinyc->udp_port.parsePacket();
            if (plen > 0) {
              char packet[512];
              int32_t len = Tinyc->udp_port.read(packet, sizeof(packet) - 1);
              if (len > 0) {
                packet[len] = 0;
                int32_t *buf = tc_resolve_ref(vm, buf_ref);
                int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
                if (buf) {
                  int n = (len < maxLen - 1) ? len : maxLen - 1;
                  for (int i = 0; i < n; i++) buf[i] = (int32_t)(uint8_t)packet[i];
                  buf[n] = 0;
                  result = n;
                }
              }
            }
          }
          TC_PUSH(vm, result);
          break;
        }
        case 2: { // udp(2, str) → reply to sender's IP:port
          int32_t str_ref = TC_POP(vm);
          if (Tinyc->udp_port_open) {
            char payload[512];
            tc_ref_to_cstr(vm, str_ref, payload, sizeof(payload));
            Tinyc->udp_port.beginPacket(
              Tinyc->udp_port.remoteIP(),
              Tinyc->udp_port.remotePort()
            );
            Tinyc->udp_port.write((const uint8_t*)payload, strlen(payload));
            Tinyc->udp_port.endPacket();
          }
          break;
        }
        case 3: { // udp(3, url, str) → send to url with stored port
          int32_t str_ref = TC_POP(vm);
          int32_t url_ref = TC_POP(vm);
          if (Tinyc->udp_port_open) {
            char url[64];
            tc_ref_to_cstr(vm, url_ref, url, sizeof(url));
            char payload[512];
            tc_ref_to_cstr(vm, str_ref, payload, sizeof(payload));
            IPAddress adr;
            adr.fromString(url);
            Tinyc->udp_port.beginPacket(adr, Tinyc->udp_port_num);
            Tinyc->udp_port.write((const uint8_t*)payload, strlen(payload));
            Tinyc->udp_port.endPacket();
          }
          break;
        }
        case 4: { // udp(4, buf) → get remote IP as string
          int32_t buf_ref = TC_POP(vm);
          int32_t result = 0;
          if (Tinyc->udp_port_open) {
            String ip = Tinyc->udp_port.remoteIP().toString();
            int32_t *buf = tc_resolve_ref(vm, buf_ref);
            int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
            if (buf) {
              int n = ip.length();
              if (n > maxLen - 1) n = maxLen - 1;
              for (int i = 0; i < n; i++) buf[i] = (int32_t)(uint8_t)ip[i];
              buf[n] = 0;
              result = n;
            }
          }
          TC_PUSH(vm, result);
          break;
        }
        case 5: { // udp(5) → get remote port
          uint16_t rport = 0;
          if (Tinyc->udp_port_open) {
            rport = Tinyc->udp_port.remotePort();
          }
          TC_PUSH(vm, (int32_t)rport);
          break;
        }
        case 6: { // udp(6, url, port, str) → send string to url:port
          int32_t str_ref = TC_POP(vm);
          int32_t port = TC_POP(vm);
          int32_t url_ref = TC_POP(vm);
          char url[64];
          tc_ref_to_cstr(vm, url_ref, url, sizeof(url));
          char payload[512];
          tc_ref_to_cstr(vm, str_ref, payload, sizeof(payload));
          IPAddress adr;
          adr.fromString(url);
          WiFiUDP tmpUdp;
          tmpUdp.begin(port);
          tmpUdp.beginPacket(adr, port);
          tmpUdp.write((const uint8_t*)payload, strlen(payload));
          tmpUdp.endPacket();
          tmpUdp.stop();
          TC_PUSH(vm, 0);
          break;
        }
        case 7: { // udp(7, url, port, arr, count) → send array bytes to url:port
          int32_t count = TC_POP(vm);
          int32_t arr_ref = TC_POP(vm);
          int32_t port = TC_POP(vm);
          int32_t url_ref = TC_POP(vm);
          char url[64];
          tc_ref_to_cstr(vm, url_ref, url, sizeof(url));
          int32_t *arr = tc_resolve_ref(vm, arr_ref);
          int32_t maxLen = tc_ref_maxlen(vm, arr_ref);
          if (arr && count > 0) {
            if (count > maxLen) count = maxLen;
            if (count > 512) count = 512;
            uint8_t *payload = (uint8_t*)malloc(count);
            if (payload) {
              for (int32_t i = 0; i < count; i++) {
                payload[i] = (uint8_t)(arr[i] & 0xFF);
              }
              IPAddress adr;
              adr.fromString(url);
              WiFiUDP tmpUdp;
              tmpUdp.begin(port);
              tmpUdp.beginPacket(adr, port);
              tmpUdp.write(payload, count);
              tmpUdp.endPacket();
              tmpUdp.stop();
              free(payload);
            }
          }
          TC_PUSH(vm, 0);
          break;
        }
        default:
          TC_PUSH(vm, 0);
          break;
      }
      break;
    }

    // ── I2C bus (all calls take bus as last param: 0 or 1) ──
    case SYS_I2C_READ8: {
      // Stack: [addr, reg, bus] — bus on top
      int32_t bus = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      TC_PUSH(vm, (int32_t)I2cRead8((uint8_t)a, (uint8_t)b, (uint8_t)bus));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_WRITE8: {
      // Stack: [addr, reg, val, bus] — bus on top
      int32_t bus = TC_POP(vm);
      int32_t val = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      TC_PUSH(vm, I2cWrite8((uint8_t)a, (uint8_t)b, (uint32_t)(val & 0xFF), (uint8_t)bus) ? 1 : 0);
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_READ_BUF: {
      // Stack: [addr, reg, buf_ref, len, bus] — bus on top
      int32_t bus = TC_POP(vm);
      int32_t len = TC_POP(vm);
      int32_t buf_ref = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      int32_t *arr = tc_resolve_ref(vm, buf_ref);
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
      if (arr && len > 0) {
        if (len > maxLen) len = maxLen;
        if (len > 255) len = 255;  // I2C practical limit
        uint8_t tmpbuf[256];
        bool err = I2cReadBuffer((uint8_t)a, (int)b, tmpbuf, (uint16_t)len, (uint8_t)bus);
        if (!err) {  // I2cReadBuffer returns 0=OK, 1=Error
          for (int32_t i = 0; i < len; i++) { arr[i] = (int32_t)tmpbuf[i]; }
          TC_PUSH(vm, 1);
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_READ_RS: {
      // Stack: [addr, reg, buf_ref, len, bus] — bus on top
      // Same as I2C_READ_BUF but uses repeated START (for SMBus devices like MLX90614)
      int32_t bus = TC_POP(vm);
      int32_t len = TC_POP(vm);
      int32_t buf_ref = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      int32_t *arr = tc_resolve_ref(vm, buf_ref);
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
      if (arr && len > 0) {
        if (len > maxLen) len = maxLen;
        if (len > 255) len = 255;
        TwoWire& myWire = I2cGetWire((uint8_t)bus);
        myWire.beginTransmission((uint8_t)a);
        myWire.write((uint8_t)b);
        if (myWire.endTransmission(false) == 0) {  // repeated START
          if ((uint8_t)len == myWire.requestFrom((uint8_t)a, (uint8_t)len)) {
            for (int32_t i = 0; i < len; i++) { arr[i] = (int32_t)myWire.read(); }
            TC_PUSH(vm, 1);
          } else {
            TC_PUSH(vm, 0);
          }
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_WRITE_BUF: {
      // Stack: [addr, reg, buf_ref, len, bus] — bus on top
      int32_t bus = TC_POP(vm);
      int32_t len = TC_POP(vm);
      int32_t buf_ref = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      int32_t *arr = tc_resolve_ref(vm, buf_ref);
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
      if (arr && len > 0) {
        if (len > maxLen) len = maxLen;
        if (len > 255) len = 255;
        uint8_t tmpbuf[256];
        for (int32_t i = 0; i < len; i++) { tmpbuf[i] = (uint8_t)(arr[i] & 0xFF); }
        TC_PUSH(vm, I2cWriteBuffer((uint8_t)a, (uint8_t)b, tmpbuf, (uint16_t)len, (uint8_t)bus) ? 0 : 1);  // returns 0=OK,1=Err
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_EXISTS: {
      // Stack: [addr, bus] — bus on top
      int32_t bus = TC_POP(vm);
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      TC_PUSH(vm, I2cSetDevice((uint32_t)a, (uint8_t)bus) ? 1 : 0);
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_SET_DEVICE: {
      // i2cSetDevice(addr, bus) — check if address is available (not claimed) and responsive
      int32_t bus = TC_POP(vm);
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      TC_PUSH(vm, I2cSetDevice((uint32_t)a, (uint8_t)bus) ? 1 : 0);
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_SET_FOUND: {
      // i2cSetActiveFound(addr, "type", bus) — register device as claimed + log
      int32_t bus = TC_POP(vm);
      int32_t ci  = TC_POP(vm);  // const pool index for type string
      a = TC_POP(vm);            // addr
#ifdef USE_I2C
      const char *type_str = tc_get_const_str(vm, ci);
      if (type_str) {
        I2cSetActiveFound((uint32_t)a, type_str, (uint8_t)bus);
      }
#endif
      break;
    }
    case SYS_I2C_FREE: {
      // i2cFree(addr, bus) — release a claimed I2C address
      int32_t bus = TC_POP(vm);
      a = TC_POP(vm);
#ifdef USE_I2C
      I2cResetActive((uint32_t)a, (uint8_t)bus);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: I2C released 0x%02x bus %d"), (uint8_t)a, (uint8_t)bus);
#endif
      break;
    }
    case SYS_ADD_COMMAND: {
      // addCommand("MP3") — register command prefix for this slot
      a = TC_POP(vm);  // const index
      if (tc_current_slot && a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        strlcpy(tc_current_slot->cmd_prefix, vm->constants[a].str.ptr, sizeof(tc_current_slot->cmd_prefix));
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: Registered command prefix \"%s\""), tc_current_slot->cmd_prefix);
      }
      break;
    }
    case SYS_RESPONSE_CMND: {
      // responseCmnd(buf) — send text as console/MQTT command response
      a = TC_POP(vm);  // buf ref
      int32_t *arr = tc_resolve_ref(vm, a);
      int32_t maxLen = tc_ref_maxlen(vm, a);
      if (arr && maxLen > 0) {
        char tmpbuf[256];
        int32_t n = (maxLen < 255) ? maxLen : 255;
        int32_t i;
        for (i = 0; i < n && arr[i]; i++) { tmpbuf[i] = (char)(arr[i] & 0xFF); }
        tmpbuf[i] = '\0';
        Response_P(PSTR("%s"), tmpbuf);
      }
      break;
    }
    case SYS_RESPONSE_CMND_STR: {
      // responseCmnd("literal") — send const string as console response
      int32_t ci = TC_POP(vm);
      if (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        Response_P(PSTR("%s"), vm->constants[ci].str.ptr);
      }
      break;
    }
    case SYS_I2C_READ_BUF0: {
      // Stack: [addr, buf_ref, len, bus] — bus on top  (no register byte sent)
      int32_t bus = TC_POP(vm);
      int32_t len = TC_POP(vm);
      int32_t buf_ref = TC_POP(vm);
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      int32_t *arr = tc_resolve_ref(vm, buf_ref);
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
      if (arr && len > 0) {
        if (len > maxLen) len = maxLen;
        if (len > 255) len = 255;
        uint8_t tmpbuf[256];
        bool err = I2cReadBuffer0((uint8_t)a, tmpbuf, (uint16_t)len, (uint8_t)bus);
        if (!err) {  // I2cReadBuffer0 returns 0=OK, 1=Error
          for (int32_t i = 0; i < len; i++) { arr[i] = (int32_t)tmpbuf[i]; }
          TC_PUSH(vm, 1);
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_I2C_WRITE0: {
      // Stack: [addr, reg, bus] — bus on top  (write register byte only, no data)
      int32_t bus = TC_POP(vm);
      b = TC_POP(vm);  // reg
      a = TC_POP(vm);  // addr
#ifdef USE_I2C
      TC_PUSH(vm, I2cWrite0((uint8_t)a, (uint8_t)b, (uint8_t)bus) ? 1 : 0);
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── Smart Meter (SML) ──────────────────────────────
    case SYS_SML_GET: {
      a = TC_POP(vm);  // meter index (0=count, 1..N=values)
#if defined(USE_SML_M) || defined(USE_SML)
      double dval = SML_GetVal((uint32_t)a);
      TC_PUSH(vm, f2i((float)dval));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_GETSTR: {
      int32_t ref = TC_POP(vm);  // buf_ref for output
      a = TC_POP(vm);            // meter index
#if defined(USE_SML_M) || defined(USE_SML)
      char *sval = SML_GetSVal((uint32_t)a);
      if (sval && ref) {
        // Copy string into TinyC char array
        int32_t *buf = tc_resolve_ref(vm, ref);
        int32_t maxLen = tc_ref_maxlen(vm, ref);
        if (buf && maxLen > 0) {
          int slen = strlen(sval);
          if (slen >= maxLen) slen = maxLen - 1;
          for (int i = 0; i < slen; i++) buf[i] = (int32_t)(uint8_t)sval[i];
          buf[slen] = 0;
          TC_PUSH(vm, slen);
        } else {
          TC_PUSH(vm, 0);
        }
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── SML advanced ─────────────────────────────────────
    case SYS_SML_WRITE: {
      // Stack: [meter, buf_ref] — buf_ref on top
      int32_t ref = TC_POP(vm);  // hex string buffer ref
      a = TC_POP(vm);            // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      char tmp[256];
      tc_ref_to_cstr(vm, ref, tmp, sizeof(tmp));
      TC_PUSH(vm, (int32_t)SML_Write(a, tmp));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_READ: {
      // Stack: [meter, buf_ref] — buf_ref on top
      int32_t ref = TC_POP(vm);  // output buffer ref
      a = TC_POP(vm);            // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      char tmp[256];
      int32_t maxLen = tc_ref_maxlen(vm, ref);
      uint32_t slen = (maxLen > 0 && maxLen < (int32_t)sizeof(tmp)) ? maxLen : sizeof(tmp) - 1;
      uint32_t got = SML_Read(a, tmp, slen);
      // Copy result into TinyC char array
      int32_t *buf = tc_resolve_ref(vm, ref);
      if (buf && got > 0) {
        for (uint32_t i = 0; i < got && (int32_t)i < maxLen; i++) {
          buf[i] = (int32_t)(uint8_t)tmp[i];
        }
        if ((int32_t)got < maxLen) buf[got] = 0;
      }
      TC_PUSH(vm, (int32_t)got);
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_SETBAUD: {
      // Stack: [meter, baud] — baud on top
      b = TC_POP(vm);  // baud rate
      a = TC_POP(vm);  // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      TC_PUSH(vm, (int32_t)SML_SetBaud((uint32_t)a, (uint32_t)b));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_SETWSTR: {
      // Stack: [meter, buf_ref] — buf_ref on top
      int32_t ref = TC_POP(vm);  // hex string buffer ref
      a = TC_POP(vm);            // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      char tmp[256];
      tc_ref_to_cstr(vm, ref, tmp, sizeof(tmp));
      TC_PUSH(vm, (int32_t)SML_Set_WStr((uint32_t)a, tmp));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_SETOPT: {
      a = TC_POP(vm);  // options bitmask
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      TC_PUSH(vm, (int32_t)SML_SetOptions((uint32_t)a));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_GETV: {
      a = TC_POP(vm);  // selector
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      TC_PUSH(vm, (int32_t)sml_getv((uint32_t)a));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_WRITE_STR: {
      // Stack: [meter, const_idx] — const_idx on top
      int32_t ci = TC_POP(vm);  // constant pool index
      a = TC_POP(vm);           // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      if (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        TC_PUSH(vm, (int32_t)SML_Write(a, (char*)vm->constants[ci].str.ptr));
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }
    case SYS_SML_SETWSTR_STR: {
      // Stack: [meter, const_idx] — const_idx on top
      int32_t ci = TC_POP(vm);  // constant pool index
      a = TC_POP(vm);           // meter index
#if defined(USE_SML_SCRIPT_CMD) && (defined(USE_SML_M) || defined(USE_SML))
      if (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        TC_PUSH(vm, (int32_t)SML_Set_WStr((uint32_t)a, (char*)vm->constants[ci].str.ptr));
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── SPI bus ────────────────────────────────────────
    case SYS_SPI_INIT: {
      // Stack: [sclk, mosi, miso, speed_mhz] — speed on top
      int32_t speed_mhz = TC_POP(vm);
      int32_t miso_pin  = TC_POP(vm);
      int32_t mosi_pin  = TC_POP(vm);
      int32_t sclk_pin  = TC_POP(vm);
      if (!Tinyc) { TC_PUSH(vm, 0); break; }
      TcSpi *spi = &Tinyc->spi;
      // Clean up previous hardware SPI instance if any
#ifdef ESP32
      if (spi->initialized && spi->sclk < 0 && spi->spip) {
        spi->spip->end();
        if (spi->spip != &SPI) { delete spi->spip; }
        spi->spip = nullptr;
      }
#endif
#ifdef ESP8266
      if (spi->initialized && spi->sclk < 0 && spi->spip) {
        spi->spip->end();
        spi->spip = nullptr;
      }
#endif
      spi->sclk = (int8_t)sclk_pin;
      spi->mosi = (int8_t)mosi_pin;
      spi->miso = (int8_t)miso_pin;

      if (sclk_pin < 0) {
        // Hardware SPI mode
        uint32_t freq = (uint32_t)speed_mhz * 1000000UL;
#ifdef ESP32
        if (sclk_pin == -1) {
          // Use Tasmota's primary SPI bus
          if (TasmotaGlobal.spi_enabled) {
            SPI.begin(Pin(GPIO_SPI_CLK), Pin(GPIO_SPI_MISO), Pin(GPIO_SPI_MOSI), -1);
            spi->spip = &SPI;
          } else {
            AddLog(LOG_LEVEL_ERROR, PSTR("TIC: SPI pins not configured in Tasmota"));
            spi->initialized = false;
            TC_PUSH(vm, 0);
            break;
          }
        } else {
          // Use HSPI (secondary bus)
          spi->spip = new SPIClass(HSPI);
          if (TasmotaGlobal.spi_enabled) {
            spi->spip->begin(Pin(GPIO_SPI_CLK, 1), Pin(GPIO_SPI_MISO, 1), Pin(GPIO_SPI_MOSI, 1), -1);
          } else {
            AddLog(LOG_LEVEL_ERROR, PSTR("TIC: SPI pins not configured in Tasmota"));
            delete spi->spip;
            spi->spip = nullptr;
            spi->initialized = false;
            TC_PUSH(vm, 0);
            break;
          }
        }
        spi->settings = SPISettings(freq, MSBFIRST, SPI_MODE0);
#endif // ESP32
#ifdef ESP8266
        if (TasmotaGlobal.spi_enabled) {
          SPI.begin();
          spi->spip = &SPI;
        } else {
          AddLog(LOG_LEVEL_ERROR, PSTR("TIC: SPI pins not configured in Tasmota"));
          spi->initialized = false;
          TC_PUSH(vm, 0);
          break;
        }
        spi->settings = SPISettings(freq, MSBFIRST, SPI_MODE0);
#endif // ESP8266
      } else {
        // Bitbang mode — check pins before configuring
        TC_CHECK_PIN(vm, sclk_pin);
        if (mosi_pin >= 0) { TC_CHECK_PIN(vm, mosi_pin); }
        if (miso_pin >= 0) { TC_CHECK_PIN(vm, miso_pin); }
        pinMode(sclk_pin, OUTPUT);
        digitalWrite(sclk_pin, 0);
        if (mosi_pin >= 0) {
          pinMode(mosi_pin, OUTPUT);
          digitalWrite(mosi_pin, 0);
        }
        if (miso_pin >= 0) {
          pinMode(miso_pin, INPUT_PULLUP);
        }
        spi->spip = nullptr;
      }
      spi->initialized = true;
      AddLog(LOG_LEVEL_INFO, PSTR("TIC: SPI init sclk=%d mosi=%d miso=%d %dMHz"),
        sclk_pin, mosi_pin, miso_pin, speed_mhz);
      TC_PUSH(vm, 1);
      break;
    }

    case SYS_SPI_SET_CS: {
      // Stack: [index, pin] — pin on top
      int32_t pin   = TC_POP(vm);
      int32_t index = TC_POP(vm);
      if (!Tinyc) break;
      // index is 1-based in the API, stored 0-based
      index = (index - 1) & (TC_SPI_MAX_CS - 1);
      TC_CHECK_PIN(vm, pin);
      Tinyc->spi.cs[index] = (int8_t)pin;
      pinMode(pin, OUTPUT);
      digitalWrite(pin, HIGH);  // CS inactive = high
      break;
    }

    case SYS_SPI_TRANSFER: {
      // Stack: [cs, buf_ref, len, mode] — mode on top
      int32_t mode    = TC_POP(vm);   // 1=8bit, 2=16bit, 3=24bit, 4=byte-with-cs
      int32_t len     = TC_POP(vm);
      int32_t buf_ref = TC_POP(vm);
      int32_t cs_idx  = TC_POP(vm);   // 1-based CS index (0 = no CS management)

      if (!Tinyc || !Tinyc->spi.initialized) {
        TC_PUSH(vm, 0);
        break;
      }
      TcSpi *spi = &Tinyc->spi;
      int32_t *arr = tc_resolve_ref(vm, buf_ref);
      int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
      if (!arr || len <= 0) { TC_PUSH(vm, 0); break; }
      if (len > maxLen) len = maxLen;
      if (mode < 1 || mode > 4) mode = 1;

      // Resolve CS pin (1-based index, 0 = no CS)
      int8_t cs_pin = -1;
      if (cs_idx > 0) {
        int ci = (cs_idx - 1) & (TC_SPI_MAX_CS - 1);
        cs_pin = spi->cs[ci];
      }

      // Assert CS (low)
      if (cs_pin >= 0 && mode != 4) {
        digitalWrite(cs_pin, LOW);
      }

      if (spi->sclk < 0 && spi->spip) {
        // ── Hardware SPI transfer ──
        spi->spip->beginTransaction(spi->settings);
        for (int32_t cnt = 0; cnt < len; cnt++) {
          uint32_t out = 0;
          uint32_t val = (uint32_t)arr[cnt];
          if (mode == 1) {
            out = spi->spip->transfer((uint8_t)val);
          } else if (mode == 2) {
            out = spi->spip->transfer16((uint16_t)val);
          } else if (mode == 3) {
            out = spi->spip->transfer((uint8_t)(val >> 16));
            out <<= 16;
            out |= spi->spip->transfer16((uint16_t)val);
          } else if (mode == 4) {
            // Per-byte CS toggle
            if (cs_pin >= 0) digitalWrite(cs_pin, LOW);
            out = spi->spip->transfer((uint8_t)val);
            if (cs_pin >= 0) digitalWrite(cs_pin, HIGH);
          }
          arr[cnt] = (int32_t)out;
        }
        spi->spip->endTransaction();
      } else if (spi->sclk >= 0) {
        // ── Bitbang SPI transfer ──
        for (int32_t cnt = 0; cnt < len; cnt++) {
          uint32_t out = 0;
          uint32_t val = (uint32_t)arr[cnt];
          int nbits;
          if (mode == 4) {
            nbits = 8;
            if (cs_pin >= 0) digitalWrite(cs_pin, LOW);
          } else {
            nbits = mode * 8;  // 8, 16, or 24 bits
            if (nbits > 24) nbits = 24;
          }
          uint32_t bit = 1UL << (nbits - 1);
          while (bit) {
            digitalWrite(spi->sclk, LOW);
            if (spi->mosi >= 0) {
              digitalWrite(spi->mosi, (val & bit) ? HIGH : LOW);
            }
            digitalWrite(spi->sclk, HIGH);
            if (spi->miso >= 0) {
              if (digitalRead(spi->miso)) out |= bit;
            }
            bit >>= 1;
          }
          arr[cnt] = (int32_t)out;
          if (mode == 4 && cs_pin >= 0) {
            digitalWrite(cs_pin, HIGH);
          }
        }
      } else {
        TC_PUSH(vm, 0);
        break;
      }

      // Deassert CS (high)
      if (cs_pin >= 0 && mode != 4) {
        digitalWrite(cs_pin, HIGH);
      }

      TC_PUSH(vm, len);
      break;
    }

    // ── Tasmota system variables ─────────────────────
    // Index mapping (must match TASM_VARS in compiler):
    //   0 = tasm_wifi       (ro) 1=network up, 0=down
    //   1 = tasm_mqttcon    (ro) 1=mqtt connected, 0=down
    //   2 = tasm_teleperiod (rw) teleperiod in seconds
    //   3 = tasm_uptime     (ro) uptime in seconds
    //   4 = tasm_heap       (ro) free heap in bytes
    //   5 = tasm_power      (rw) relay bitmask (all relays)
    //   6 = tasm_dimmer     (rw) light dimmer 0-100
    //   7 = tasm_temp       (ro) global temperature (float)
    //   8 = tasm_hum        (ro) global humidity (float)
    //   9 = tasm_hour       (ro) current hour 0-23
    //  10 = tasm_min        (ro) current minute 0-59
    //  11 = tasm_sec        (ro) current second 0-59
    //  12 = tasm_year       (ro) current year (e.g. 2026)
    //  13 = tasm_month      (ro) current month 1-12
    //  14 = tasm_day        (ro) day of month 1-31
    //  15 = tasm_wday       (ro) day of week 1=Sun..7=Sat
    //  16 = tasm_cw         (ro) ISO calendar week 1-53
    //  17 = tasm_sunrise    (ro) sunrise minutes since midnight
    //  18 = tasm_sunset     (ro) sunset minutes since midnight
    //  19 = tasm_time       (ro) current minutes since midnight
    case SYS_TASM_GET: {
      a = TC_POP(vm);  // variable index
      int32_t val = 0;
      switch (a) {
        case 0: val = TasmotaGlobal.global_state.network_down ? 0 : 1; break;
        case 1: val = TasmotaGlobal.global_state.mqtt_down ? 0 : 1; break;
        case 2: val = (int32_t)Settings->tele_period; break;
        case 3: val = (int32_t)(millis() / 1000); break;
        case 4: val = (int32_t)ESP_getFreeHeap(); break;
        case 5: val = (int32_t)TasmotaGlobal.power; break;
        case 6:
#ifdef USE_LIGHT
          val = (int32_t)Settings->light_dimmer;
#endif
          break;
        case 7: {
          float tf = TasmotaGlobal.temperature_celsius;
          uint32_t ti; memcpy(&ti, &tf, 4);
          TC_PUSH(vm, (int32_t)ti);
          goto tasm_get_done;
        }
        case 8: {
          float hf = TasmotaGlobal.humidity;
          uint32_t hi; memcpy(&hi, &hf, 4);
          TC_PUSH(vm, (int32_t)hi);
          goto tasm_get_done;
        }
        case 9: val = (int32_t)RtcTime.hour; break;
        case 10: val = (int32_t)RtcTime.minute; break;
        case 11: val = (int32_t)RtcTime.second; break;
        case 12: val = (int32_t)RtcTime.year; break;
        case 13: val = (int32_t)RtcTime.month; break;
        case 14: val = (int32_t)RtcTime.day_of_month; break;
        case 15: val = (int32_t)RtcTime.day_of_week; break;
        case 16: {
          // ISO calendar week (same algorithm as scripter calc_cw)
          float a16 = floor((14.0f - RtcTime.month) / 12.0f);
          float y16 = RtcTime.year + 4800 - a16;
          float m16 = RtcTime.month + (12 * a16) - 3;
          float jd = RtcTime.day_of_month + floor((153.0f * m16 + 2) / 5.0f) +
                     (365 * y16) + floor(y16 / 4) - floor(y16 / 100) +
                     floor(y16 / 400) - 32045;
          float d4 = (uint32_t)((uint32_t)(jd + 31741 - ((uint32_t)jd % 7)) % 146097 % 36524 % 1461);
          float L = floor(d4 / 1460);
          float d1 = ((uint32_t)(d4 - L) % 365) + L;
          int cw = (int)floor(d1 / 7) + 1;
          if (cw == 1 && RtcTime.month == 12) { }  // year rollover
          if (cw >= 52 && RtcTime.month == 1) { }   // year rollback
          val = (int32_t)cw;
          break;
        }
#ifdef USE_SUNRISE
        case 17: val = (int32_t)SunMinutes(0); break;  // tasm_sunrise
        case 18: val = (int32_t)SunMinutes(1); break;  // tasm_sunset
#endif
        case 19: val = (int32_t)MinutesPastMidnight(); break;  // tasm_time
#ifdef ESP32
        case 20: val = (int32_t)ESP.getFreePsram(); break;  // tasm_pheap
#endif
        default: break;
      }
      TC_PUSH(vm, val);
      tasm_get_done:
      break;
    }
    case SYS_TASM_SET: {
      a = TC_POP(vm);            // variable index (pushed last by compiler)
      int32_t val = TC_POP(vm);  // value (compiled first, pushed earlier)
      switch (a) {
        case 2:  // tasm_teleperiod
          if (val >= 10 && val <= 3600) {
            Settings->tele_period = (uint16_t)val;
            TasmotaGlobal.tele_period = 0;
          }
          break;
        case 5:  // tasm_power
          for (uint32_t i = 0; i < TasmotaGlobal.devices_present; i++) {
            ExecuteCommandPower(i + 1, (val >> i) & 1, SRC_IGNORE);
          }
          break;
        case 6: // tasm_dimmer
#ifdef USE_LIGHT
          { char cmd[16];
            snprintf_P(cmd, sizeof(cmd), PSTR("Dimmer %d"), val);
            ExecuteCommand(cmd, SRC_IGNORE); }
#endif
          break;
        default: break;  // read-only variables silently ignored
      }
      break;
    }

    // ── String manipulation ────────────────────────────
    case SYS_STR_TOKEN: {
      // strToken(dst, src, delim, n) -> length of token (1-based index)
      int32_t n = TC_POP(vm);           // 1-based token index
      int32_t delim = TC_POP(vm);       // delimiter char code
      int32_t src_ref = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t result = 0;
      if (src && dst && n >= 1) {
        int32_t src_max = tc_ref_maxlen(vm, src_ref);
        int32_t dst_max = tc_ref_maxlen(vm, dst_ref) - 1;
        // find start of nth token
        int32_t si = 0, idx = 1;
        while (idx < n && si < src_max && src[si] != 0) {
          if (src[si] == delim) idx++;
          si++;
        }
        if (idx == n && si < src_max && src[si] != 0) {
          int32_t di = 0;
          while (si < src_max && src[si] != 0 && src[si] != delim && di < dst_max) {
            dst[di++] = src[si++];
          }
          dst[di] = 0;
          result = di;
        } else {
          dst[0] = 0;
        }
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_STR_SUB: {
      // strSub(dst, src, pos, len) -> actual length copied
      int32_t slen = TC_POP(vm);        // number of chars to copy
      int32_t pos = TC_POP(vm);         // start position (0-based, neg=from end)
      int32_t src_ref = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t result = 0;
      if (src && dst) {
        int32_t src_max = tc_ref_maxlen(vm, src_ref);
        int32_t dst_max = tc_ref_maxlen(vm, dst_ref) - 1;
        // compute source string length
        int32_t srclen = 0;
        while (srclen < src_max && src[srclen] != 0) srclen++;
        if (pos < 0) pos = srclen + pos;  // negative = from end
        if (pos < 0) pos = 0;
        if (pos > srclen) pos = srclen;
        if (slen <= 0 || pos + slen > srclen) slen = srclen - pos;
        if (slen > dst_max) slen = dst_max;
        for (int32_t i = 0; i < slen; i++) {
          dst[i] = src[pos + i];
        }
        dst[slen] = 0;
        result = slen;
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_STR_FIND: {
      // strFind(haystack, needle) -> position (-1 if not found)
      int32_t needle_ref = TC_POP(vm);
      int32_t haystack_ref = TC_POP(vm);
      int32_t *haystack = tc_resolve_ref(vm, haystack_ref);
      int32_t *needle = tc_resolve_ref(vm, needle_ref);
      int32_t result = -1;
      if (haystack && needle) {
        int32_t hmax = tc_ref_maxlen(vm, haystack_ref);
        int32_t nmax = tc_ref_maxlen(vm, needle_ref);
        int32_t hlen = 0;
        while (hlen < hmax && haystack[hlen] != 0) hlen++;
        int32_t nlen = 0;
        while (nlen < nmax && needle[nlen] != 0) nlen++;
        if (nlen > 0 && nlen <= hlen) {
          for (int32_t i = 0; i <= hlen - nlen; i++) {
            bool match = true;
            for (int32_t j = 0; j < nlen; j++) {
              if (haystack[i + j] != needle[j]) { match = false; break; }
            }
            if (match) { result = i; break; }
          }
        }
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_STR_FIND_CONST: {
      // strFind(haystack, "needle_literal") -> position (-1 if not found)
      int32_t ci = TC_POP(vm);  // const pool index
      int32_t haystack_ref = TC_POP(vm);
      int32_t *haystack = tc_resolve_ref(vm, haystack_ref);
      int32_t result = -1;
      if (haystack && ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *needle = vm->constants[ci].str.ptr;
        int32_t hmax = tc_ref_maxlen(vm, haystack_ref);
        int32_t hlen = 0;
        while (hlen < hmax && haystack[hlen] != 0) hlen++;
        int32_t nlen = strlen(needle);
        if (nlen > 0 && nlen <= hlen) {
          for (int32_t i = 0; i <= hlen - nlen; i++) {
            bool match = true;
            for (int32_t j = 0; j < nlen; j++) {
              if (haystack[i + j] != (int32_t)(uint8_t)needle[j]) { match = false; break; }
            }
            if (match) { result = i; break; }
          }
        }
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_STR_TO_INT: {
      // atoi(str) -> int
      int32_t ref = TC_POP(vm);
      char tbuf[32];
      tc_ref_to_cstr(vm, ref, tbuf, sizeof(tbuf));
      TC_PUSH(vm, (int32_t)atoi(tbuf));
      break;
    }
    case SYS_STR_TO_FLOAT: {
      // atof(str) -> float
      int32_t ref = TC_POP(vm);
      char tbuf[32];
      tc_ref_to_cstr(vm, ref, tbuf, sizeof(tbuf));
      TC_PUSHF(vm, (float)atof(tbuf));
      break;
    }

    // ── Sensor JSON parsing ─────────────────────────────
    case SYS_SENSOR_GET: {
      // sensorGet("SensorName#Key#SubKey") -> float value
      int32_t ci = TC_POP(vm);  // constant pool index with JSON path
      float fval = 0.0f;
      if (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *path = vm->constants[ci].str.ptr;
        // Build sensor JSON via MqttShowSensor
        // Guard: prevent re-entry into TinyC's FUNC_JSON_APPEND
        tc_sensor_get_slot = tc_current_slot;  // mark THIS slot to skip its JsonCall
        ResponseClear();
        MqttShowSensor(true);  // builds complete JSON: {"Time":"...","SCD30":{...},...}
        tc_sensor_get_slot = nullptr;
        // Parse the JSON path (segments separated by #)
        char jpath[64];
        strlcpy(jpath, path, sizeof(jpath));
        JsonParser parser(ResponseData());
        JsonParserObject obj = parser.getRootObject();
        char *seg = jpath;
        char *next;
        bool valid = true;
        while (valid && seg && *seg) {
          next = strchr(seg, '#');
          if (next) { *next = 0; next++; }
          JsonParserToken tok = obj[seg];
          if (!tok.isValid()) { valid = false; break; }
          if (next && *next) {
            // intermediate object — descend
            obj = tok.getObject();
            if (!obj.isValid()) { valid = false; break; }
          } else {
            // leaf value — extract float
            fval = tok.getFloat();
          }
          seg = next;
        }
      }
      // Push float as int32 bit pattern
      uint32_t fi;
      memcpy(&fi, &fval, 4);
      TC_PUSH(vm, (int32_t)fi);
      break;
    }

    // ── HTTP requests ─────────────────────────────────
    case SYS_HTTP_GET: {
      b = TC_POP(vm);  // response buffer ref
      a = TC_POP(vm);  // url ref
      char url[256];
      tc_ref_to_cstr(vm, a, url, sizeof(url));
      WiFiClient http_client;
      HTTPClient http;
      http.setTimeout(5000);
      http.begin(http_client, url);
      // Add custom headers
      for (int i = 0; i < Tinyc->http_hdr_count; i++) {
        http.addHeader(Tinyc->http_hdr_name[i], Tinyc->http_hdr_value[i]);
      }
      Tinyc->http_hdr_count = 0;
      int httpCode = http.GET();
      int32_t result = -1;
      if (httpCode > 0) {
        String payload = http.getString();
        int32_t *buf = tc_resolve_ref(vm, b);
        if (buf) {
          int32_t maxLen = tc_ref_maxlen(vm, b);
          int len = payload.length();
          if (len > maxLen - 1) len = maxLen - 1;
          for (int i = 0; i < len; i++) buf[i] = (int32_t)(uint8_t)payload[i];
          buf[len] = 0;
          result = len;
        }
      } else {
        result = httpCode;  // negative error code
      }
      http.end();
      http_client.stop();
      TC_PUSH(vm, result);
      break;
    }
    case SYS_HTTP_POST: {
      int32_t respRef = TC_POP(vm);  // response buffer ref
      int32_t dataRef = TC_POP(vm);  // POST data ref
      a = TC_POP(vm);                // url ref
      char url[256];
      tc_ref_to_cstr(vm, a, url, sizeof(url));
      char postData[TC_OUTPUT_SIZE];
      tc_ref_to_cstr(vm, dataRef, postData, sizeof(postData));
      WiFiClient http_client;
      HTTPClient http;
      http.setTimeout(5000);
      http.begin(http_client, url);
      http.addHeader(F("Content-Type"), F("application/x-www-form-urlencoded"));
      for (int i = 0; i < Tinyc->http_hdr_count; i++) {
        http.addHeader(Tinyc->http_hdr_name[i], Tinyc->http_hdr_value[i]);
      }
      Tinyc->http_hdr_count = 0;
      int httpCode = http.POST(postData);
      int32_t result = -1;
      if (httpCode > 0) {
        String payload = http.getString();
        int32_t *buf = tc_resolve_ref(vm, respRef);
        if (buf) {
          int32_t maxLen = tc_ref_maxlen(vm, respRef);
          int len = payload.length();
          if (len > maxLen - 1) len = maxLen - 1;
          for (int i = 0; i < len; i++) buf[i] = (int32_t)(uint8_t)payload[i];
          buf[len] = 0;
          result = len;
        }
      } else {
        result = httpCode;
      }
      http.end();
      http_client.stop();
      TC_PUSH(vm, result);
      break;
    }
    case SYS_HTTP_HEADER: {
      b = TC_POP(vm);  // value ref
      a = TC_POP(vm);  // name ref
      if (Tinyc->http_hdr_count < TC_HTTP_MAX_HEADERS) {
        // Lazy-allocate header arrays on first use
        if (!Tinyc->http_hdr_name) {
          Tinyc->http_hdr_name = (char(*)[64])calloc(TC_HTTP_MAX_HEADERS, 64);
          Tinyc->http_hdr_value = (char(*)[64])calloc(TC_HTTP_MAX_HEADERS, 64);
          if (!Tinyc->http_hdr_name || !Tinyc->http_hdr_value) break;
        }
        tc_ref_to_cstr(vm, a, Tinyc->http_hdr_name[Tinyc->http_hdr_count], 64);
        tc_ref_to_cstr(vm, b, Tinyc->http_hdr_value[Tinyc->http_hdr_count], 64);
        Tinyc->http_hdr_count++;
      }
      break;
    }

    case SYS_WEB_PARSE: {
      // webParse(source[], "delim", index, result[]) → int length
      // index > 0: split by delimiter, return Nth segment (1-based)
      // index < 0: find "delim=value", extract value (stops at , : or NUL)
      int32_t dstRef  = TC_POP(vm);
      int32_t index   = TC_POP(vm);
      int32_t ci      = TC_POP(vm);
      int32_t srcRef  = TC_POP(vm);

      const char *delim = tc_get_const_str(vm, ci);
      int32_t *src = tc_resolve_ref(vm, srcRef);
      int32_t *dst = tc_resolve_ref(vm, dstRef);

      if (!delim || !src || !dst) {
        TC_PUSH(vm, 0);
        break;
      }

      int32_t srcMax = tc_ref_maxlen(vm, srcRef);
      int32_t dstMax = tc_ref_maxlen(vm, dstRef) - 1;
      int32_t dlen = strlen(delim);

      if (dlen == 0 || index == 0) {
        dst[0] = 0;
        TC_PUSH(vm, 0);
        break;
      }

      // Convert source int32 array to C string
      char sbuf[512];
      int32_t slen = 0;
      for (int32_t i = 0; i < srcMax && i < (int32_t)sizeof(sbuf) - 1; i++) {
        if (src[i] == 0) break;
        sbuf[i] = (char)(src[i] & 0xFF);
        slen++;
      }
      sbuf[slen] = 0;

      char rbuf[256];
      rbuf[0] = 0;
      int32_t rlen = 0;

      if (index > 0) {
        // Split by delimiter, return Nth segment (1-based)
        char *wd = sbuf;
        char *lwd = wd;
        int32_t count = index;
        while (count > 0) {
          char *cp = strstr(wd, delim);
          if (cp) {
            count--;
            if (count == 0) {
              *cp = 0;
              strlcpy(rbuf, lwd, sizeof(rbuf));
            } else {
              wd = cp + dlen;
              lwd = wd;
            }
          } else {
            strlcpy(rbuf, lwd, sizeof(rbuf));
            break;
          }
        }
        rlen = strlen(rbuf);
      } else {
        // Find "delim=value", extract value
        char *cp = strstr(sbuf, delim);
        if (cp) {
          cp = strchr(cp, '=');
          if (cp) {
            cp++;
            for (int32_t cnt = 0; cnt < (int32_t)sizeof(rbuf) - 1; cnt++) {
              if (*cp == ',' || *cp == ':' || *cp == 0) {
                rbuf[cnt] = 0;
                break;
              }
              rbuf[cnt] = *cp++;
              rlen = cnt + 1;
            }
            rbuf[rlen] = 0;
          }
        }
      }

      // Copy result to VM destination buffer
      if (rlen > dstMax) rlen = dstMax;
      for (int32_t i = 0; i < rlen; i++) {
        dst[i] = (int32_t)(uint8_t)rbuf[i];
      }
      dst[rlen] = 0;

      TC_PUSH(vm, rlen);
      break;
    }

    // ── WebUI widgets ─────────────────────────────────
    // Each generates HTML for the /tc_ui AJAX page.
    // gref = variable ref (ADDR_GLOBAL/ADDR_LOCAL), label/opts = const pool index.
    // Global index extracted from ref for seva()/siva() JavaScript callbacks.
    case SYS_WEB_BUTTON: {
      int32_t ci = TC_POP(vm);   // label const idx
      int32_t gref = TC_POP(vm); // variable ref
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      int32_t nval = val ? 0 : 1;
      WSContentSend_P(PSTR("<div><button onclick='seva(%d,%d)' style='width:100%%'>%s: %s</button></div>"),
                      nval, idx, label, val ? "ON" : "OFF");
      break;
    }
    case SYS_WEB_SLIDER: {
      int32_t ci = TC_POP(vm);   // label const idx
      int32_t vmax = TC_POP(vm);
      int32_t vmin = TC_POP(vm);
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      WSContentSend_P(PSTR("<div><label><b>%s</b> <span id='sv%d'>%d</span></label>"
                           "<input type='range' min='%d' max='%d' value='%d' "
                           "oninput='document.getElementById(\"sv%d\").textContent=this.value' "
                           "onchange='seva(value,%d)'></div>"),
                      label, idx, val, vmin, vmax, val, idx, idx);
      break;
    }
    case SYS_WEB_CHECKBOX: {
      int32_t ci = TC_POP(vm);
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      int32_t nval = val ? 0 : 1;
      WSContentSend_P(PSTR("<div><label><b>%s</b> "
                           "<input type='checkbox' %s onchange='seva(%d,%d)'>"
                           "</label></div>"),
                      label, val ? "checked" : "", nval, idx);
      break;
    }
    case SYS_WEB_TEXT: {
      int32_t ci = TC_POP(vm);      // label const idx
      int32_t maxlen = TC_POP(vm);  // max char length
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      char tbuf[128];
      tc_ref_to_cstr(vm, gref, tbuf, sizeof(tbuf));
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      WSContentSend_P(PSTR("<div><label><b>%s</b> "
                           "<input type='text' value='%s' maxlength='%d' style='width:200px' "
                           "onfocusin='pr(0)' onfocusout='pr(1)' "
                           "onchange='siva(value,%d)'>"
                           "</label></div>"),
                      label, tbuf, maxlen - 1, idx);
      break;
    }
    case SYS_WEB_NUMBER: {
      int32_t ci = TC_POP(vm);
      int32_t vmax = TC_POP(vm);
      int32_t vmin = TC_POP(vm);
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      WSContentSend_P(PSTR("<div><label><b>%s</b> "
                           "<input type='number' min='%d' max='%d' value='%d' style='width:80px' "
                           "onfocusin='pr(0)' onfocusout='pr(1)' "
                           "onchange='seva(value,%d)'>"
                           "</label></div>"),
                      label, vmin, vmax, val, idx);
      break;
    }
    case SYS_WEB_PULLDOWN: {
      int32_t oi = TC_POP(vm);   // options const idx ("opt1|opt2|opt3" or "@getfreepins")
      int32_t li = TC_POP(vm);   // label const idx
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label = tc_get_const_str(vm, li);
      const char *opts = tc_get_const_str(vm, oi);
      if (!opts) opts = "";
      if (label && label[0]) {
        WSContentSend_P(PSTR("<div><label><b>%s</b> "), label);
      } else {
        WSContentSend_P(PSTR("<div>"));
      }
      WSContentSend_P(PSTR("<select onfocusin='pr(0)' onfocusout='pr(1)' onchange='seva(value,%d)'>"), idx);

      if (opts[0] == '@' && strcmp(opts + 1, "getfreepins") == 0) {
        // Dynamic GPIO pin list — show free (unassigned) pins
        WSContentSend_P(PSTR("<option value='-1'%s>None</option>"),
                        (val < 0) ? " selected" : "");
        for (uint32_t pin = 0; pin < MAX_GPIO_PIN; pin++) {
          if (FlashPin(pin)) continue;
          bool free = (TasmotaGlobal.gpio_pin[pin] == 0);
          // Always show the currently selected pin even if now in use
          if (free || (int32_t)pin == val) {
            WSContentSend_P(PSTR("<option value='%d'%s>GPIO %d%s</option>"),
                            pin, ((int32_t)pin == val) ? " selected" : "",
                            pin, free ? "" : " (used)");
          }
        }
      } else {
        // Static pipe-separated options
        char obuf[256];
        strlcpy(obuf, opts, sizeof(obuf));
        int oi = 0;
        char *op = obuf;
        char *sep;
        while (op && *op) {
          sep = strchr(op, '|');
          if (sep) *sep = 0;
          WSContentSend_P(PSTR("<option value='%d'%s>%s</option>"),
                          oi, (oi == val) ? " selected" : "", op);
          oi++;
          op = sep ? sep + 1 : nullptr;
        }
      }
      if (label && label[0]) {
        WSContentSend_P(PSTR("</select></label></div>"));
      } else {
        WSContentSend_P(PSTR("</select></div>"));
      }
      break;
    }
    case SYS_WEB_RADIO: {
      int32_t ci = TC_POP(vm);   // options const idx ("opt1|opt2|opt3")
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;  // 0-based selected index
      const char *opts = tc_get_const_str(vm, ci);
      if (!opts) opts = "";
      WSContentSend_P(PSTR("<div><fieldset><legend></legend>"));
      char obuf[256];
      strlcpy(obuf, opts, sizeof(obuf));
      int oi = 0;
      char *op = obuf;
      char *sep;
      while (op && *op) {
        sep = strchr(op, '|');
        if (sep) *sep = 0;
        WSContentSend_P(PSTR("<div><label><input type='radio' name='r%d' onclick='seva(%d,%d)' %s>%s</label></div>"),
                        idx, oi, idx, (oi == val) ? "checked" : "", op);
        oi++;
        op = sep ? sep + 1 : nullptr;
      }
      WSContentSend_P(PSTR("</fieldset></div>"));
      break;
    }
    case SYS_WEB_TIME: {
      int32_t ci = TC_POP(vm);
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;  // HHMM format
      const char *label = tc_get_const_str(vm, ci);
      if (!label) label = "?";
      int hh = val / 100;
      int mm = val % 100;
      WSContentSend_P(PSTR("<div><label><b>%s</b> "
                           "<input type='time' value='%02d:%02d' style='width:80px' "
                           "onfocusin='pr(0)' onfocusout='pr(1)' "
                           "onchange='sivat(value,%d)'>"
                           "</label></div>"),
                      label, hh, mm, idx);
      break;
    }

    case SYS_WEB_PAGE_LABEL: {
      int32_t ci = TC_POP(vm);      // label const index
      int32_t pn = TC_POP(vm);      // page number (0-5)
      const char *label = tc_get_const_str(vm, ci);
      if (label && Tinyc && pn >= 0 && pn < TC_MAX_WEB_PAGES) {
        strlcpy(Tinyc->page_label[pn], label, sizeof(Tinyc->page_label[0]));
        if (pn >= Tinyc->page_count) Tinyc->page_count = pn + 1;
      }
      break;
    }
    case SYS_WEB_PAGE: {
      // Push current page number being rendered
      TC_PUSH(vm, Tinyc ? Tinyc->current_page : 0);
      break;
    }
    case SYS_WEB_SEND_FILE: {
      // Send file contents to web page (like Scripter's %/filename)
      int32_t ci = TC_POP(vm);
      const char *fname = tc_get_const_str(vm, ci);
#ifdef USE_UFILESYS
      if (fname) {
        char path[48];
        if (fname[0] != '/') {
          snprintf(path, sizeof(path), "/%s", fname);
        } else {
          strlcpy(path, fname, sizeof(path));
        }
        // Try ufsp first, then ffsp
        File f;
        if (ufsp) f = ufsp->open(path, "r");
        if (!f && ffsp) f = ffsp->open(path, "r");
        if (f) {
          char buf[256];
          WSContentFlush();
          while (f.available()) {
            int len = f.readBytes(buf, sizeof(buf) - 1);
            buf[len] = 0;
            WSContentSend_P(PSTR("%s"), buf);
          }
          f.close();
        } else {
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: webFile '%s' not found"), path);
        }
      }
#endif
      break;
    }

    case SYS_WEB_SEND_JSON_ARRAY: {
      int32_t count = TC_POP(vm);
      int32_t ref = TC_POP(vm);
#ifdef USE_WEBSERVER
      int32_t *arr = tc_resolve_ref(vm, ref);
      int32_t maxLen = tc_ref_maxlen(vm, ref);
      if (arr && count > 0) {
        if (count > maxLen) count = maxLen;
        char tmp[16];
        WSContentSend_P(PSTR("["));
        for (int32_t i = 0; i < count; i++) {
          int32_t val = (int32_t)i2f(arr[i]);
          int slen = snprintf(tmp, sizeof(tmp), "%s%d", (i > 0) ? "," : "", val);
          WSContentSend(tmp, slen);
        }
        WSContentSend_P(PSTR("]"));
      }
#endif
      break;
    }

    case SYS_WEB_ON: {
      // Register custom web endpoint: webOn(handler_num, "/url/path")
      int32_t ci = TC_POP(vm);   // url const index
      int32_t hn = TC_POP(vm);   // handler number (1-4)
      const char *url = tc_get_const_str(vm, ci);
#ifdef USE_WEBSERVER
      if (url && Tinyc && hn >= 1 && hn <= TC_MAX_WEB_HANDLERS) {
        strlcpy(Tinyc->web_handler_url[hn - 1], url, sizeof(Tinyc->web_handler_url[0]));
        if (hn > Tinyc->web_handler_count) Tinyc->web_handler_count = hn;
        // Register with Tasmota web server — URL persists in Tinyc struct
        Webserver->on(Tinyc->web_handler_url[hn - 1], TinyCWebOnHandlers[hn - 1]);
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: webOn(%d, \"%s\") registered"), hn, url);
      }
#endif
      break;
    }
    case SYS_WEB_HANDLER: {
      // Returns current handler number during WebOn callback
      TC_PUSH(vm, Tinyc ? Tinyc->current_web_handler : 0);
      break;
    }
    case SYS_WEB_ARG: {
      // Get HTTP request argument: webArg("name", buf) -> length
      int32_t buf_ref = TC_POP(vm);   // char array ref for result
      int32_t ci = TC_POP(vm);        // arg name const index
      int32_t result = 0;
      const char *argname = tc_get_const_str(vm, ci);
#ifdef USE_WEBSERVER
      if (argname && Webserver->hasArg(String(argname))) {
        String val = Webserver->arg(String(argname));
        int32_t *buf = tc_resolve_ref(vm, buf_ref);
        int32_t maxLen = tc_ref_maxlen(vm, buf_ref);
        if (buf && maxLen > 0) {
          int slen = val.length();
          if (slen >= maxLen) slen = maxLen - 1;
          for (int i = 0; i < slen; i++) buf[i] = (int32_t)(uint8_t)val[i];
          buf[slen] = 0;
          result = slen;
        }
      }
#endif
      TC_PUSH(vm, result);
      break;
    }

    case SYS_MDNS: {
      // Register mDNS service: mdns("name", "mac", "type") -> 0
      int32_t ci_type = TC_POP(vm);
      int32_t ci_mac  = TC_POP(vm);
      int32_t ci_name = TC_POP(vm);
      const char *name  = tc_get_const_str(vm, ci_name);
      const char *mac   = tc_get_const_str(vm, ci_mac);
      const char *xtype = tc_get_const_str(vm, ci_type);
      int32_t result = -1;
#if defined(ESP32) || defined(ESP8266)
      if (name && mac && xtype) {
        // Build MAC string
        char mdns_mac[13];
        if (mac[0] == '-') {
          String strMac = NetworkMacAddress();
          strMac.toLowerCase();
          strMac.replace(":", "");
          strlcpy(mdns_mac, strMac.c_str(), sizeof(mdns_mac));
        } else {
          strlcpy(mdns_mac, mac, sizeof(mdns_mac));
        }
        // Build hostname
        char mdns_name[48];
        if (name[0] == '-') {
          strlcpy(mdns_name, NetworkHostname(), sizeof(mdns_name));
        } else {
          strlcpy(mdns_name, name, sizeof(mdns_name));
          strlcat(mdns_name, mdns_mac, sizeof(mdns_name));
        }
        // Start mDNS responder
        const char *cMac = (const char*)mdns_mac;
        String ipStr = NetworkAddress().toString();
        if (MDNS.begin(mdns_name)) {
          if (!strcmp(xtype, "everhome")) {
            MDNS.addService("everhome", "tcp", 80);
            MDNS.addServiceTxt("everhome", "tcp", "ip", ipStr.c_str());
            MDNS.addServiceTxt("everhome", "tcp", "serial", cMac);
            MDNS.addServiceTxt("everhome", "tcp", "productid", "1137");
          } else if (!strcmp(xtype, "shelly")) {
            MDNS.addService("http", "tcp", 80);
            MDNS.addService("shelly", "tcp", 80);
            MDNS.addServiceTxt("http", "tcp", "fw_id", "20241011-114455/1.4.4-g6d2a586");
            MDNS.addServiceTxt("http", "tcp", "arch", "esp8266");
            MDNS.addServiceTxt("http", "tcp", "id", "");
            MDNS.addServiceTxt("http", "tcp", "gen", "2");
            MDNS.addServiceTxt("shelly", "tcp", "fw_id", "20241011-114455/1.4.4-g6d2a586");
            MDNS.addServiceTxt("shelly", "tcp", "arch", "esp8266");
            MDNS.addServiceTxt("shelly", "tcp", "id", "");
            MDNS.addServiceTxt("shelly", "tcp", "gen", "2");
          } else {
            MDNS.addService(xtype, "tcp", 80);
            MDNS.addServiceTxt(xtype, "tcp", "ip", ipStr.c_str());
            MDNS.addServiceTxt(xtype, "tcp", "serial", cMac);
          }
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: mDNS started, service=%s hostname=%s"), xtype, mdns_name);
          result = 0;
        } else {
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: mDNS failed to start"));
        }
      }
#endif
      TC_PUSH(vm, result);
      break;
    }

    case SYS_WEB_CONSOLE_BTN: {
      // webConsoleButton("/url", "Label") — register button for Utilities menu
      int32_t ci_label = TC_POP(vm);
      int32_t ci_url   = TC_POP(vm);
      const char *url   = tc_get_const_str(vm, ci_url);
      const char *label = tc_get_const_str(vm, ci_label);
      if (url && label && Tinyc && Tinyc->console_btn_count < TC_MAX_CONSOLE_BTNS) {
        uint8_t idx = Tinyc->console_btn_count;
        strlcpy(Tinyc->console_btn_url[idx], url, sizeof(Tinyc->console_btn_url[0]));
        strlcpy(Tinyc->console_btn_label[idx], label, sizeof(Tinyc->console_btn_label[0]));
        Tinyc->console_btn_count++;
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: consoleButton(\"%s\", \"%s\") registered"), url, label);
      }
      break;
    }

    case SYS_WEB_CHART_SIZE: {
      // WebChartSize(width, height) — set chart div dimensions in pixels
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      tc_chart_width  = (w > 0 && w < 4096) ? (uint16_t)w : 0;
      tc_chart_height = (h > 0 && h < 4096) ? (uint16_t)h : 0;
      break;
    }

    case SYS_WEB_CHART: {
      // WebChart(type, title, unit, color, pos, count, array, decimals, interval, ymin, ymax)
#ifdef USE_WEBSERVER
      int32_t ymax_bits = TC_POP(vm);
      int32_t ymin_bits = TC_POP(vm);
      int32_t interval = TC_POP(vm);
      int32_t decimals = TC_POP(vm);
      int32_t arr_ref  = TC_POP(vm);
      int32_t count    = TC_POP(vm);
      int32_t pos_bits = TC_POP(vm);
      int32_t pos      = (int32_t)i2f(pos_bits);  // pos is float (from array[0])
      int32_t color    = TC_POP(vm);
      int32_t ci_unit  = TC_POP(vm);
      int32_t ci_title = TC_POP(vm);
      int32_t type     = TC_POP(vm);

      float ymin = i2f(ymin_bits);
      float ymax = i2f(ymax_bits);
      bool fixed_range = (ymin < ymax);

      const char *title = tc_get_const_str(vm, ci_title);
      const char *unit  = tc_get_const_str(vm, ci_unit);
      if (!title || !unit) break;
      if (decimals < 0) decimals = 0;
      if (decimals > 6) decimals = 6;

      // Parse "name|unit" format: name goes to legend, unit to y-axis
      // If no '|', unit serves as both (backward compatible)
      const char *series_name = unit;
      const char *axis_unit = unit;
      char name_buf[32], unit_buf[32];
      const char *pipe = strchr(unit, '|');
      if (pipe) {
        int nlen = pipe - unit;
        if (nlen > 31) nlen = 31;
        memcpy(name_buf, unit, nlen);
        name_buf[nlen] = '\0';
        series_name = name_buf;
        strlcpy(unit_buf, pipe + 1, sizeof(unit_buf));
        axis_unit = unit_buf;
      }

      bool new_chart = (title[0] != '\0');  // empty title = add series to previous chart

      // 1) Google Charts loader + helper JS — once per page render
      // Type codes: 'l'=line, 'c'=column, 'b'=bar, 'h'=histogram, 's'=stacked column
      // Legacy: 0=line, 1=column
      if (!tc_chart_lib_sent) {
        WSContentSend_P(PSTR(
          "<style>"
          ".google-visualization-table-table{background:#fff;color:#000;border-collapse:collapse;}"
          ".google-visualization-table-table td,.google-visualization-table-table th{padding:4px 8px;border:1px solid #ccc;}"
          ".google-visualization-table-th{background:#e8e8e8;font-weight:bold;}"
          ".google-visualization-table-tr-odd{background:#f5f5f5;}"
          ".google-visualization-table-tr-even{background:#fff;}"
          "</style>"
          "<script src=\"https://www.gstatic.com/charts/loader.js\"></script>"
          "<script>google.charts.load('current',{packages:['corechart','table']});</script>"
          "<script>"
          "var _tcC=[];"
          "function _tcA(ci,lbl,clr,d,mn,mx,u){_tcC[ci].s.push({l:lbl,c:clr,d:d,mn:mn,mx:mx,u:u||lbl});}"
          "function _tcN(ci,t,u,tp,mn,mx){"
            "var lb=null;"
            "if(tp==116){var p=t.indexOf('|');if(p>=0){lb=t.substring(p+1).split('|');t=t.substring(0,p);}}"
            "_tcC[ci]={t:t,u:u,tp:tp,mn:mn,mx:mx,s:[],lb:lb};"
          "}"
          "function _tcD(){"
            "var N=new Date();"
            "for(var i=0;i<_tcC.length;i++){"
              "var c=_tcC[i];if(!c)continue;"
              "var dt=new google.visualization.DataTable();"
              "var tp=c.tp,el=document.getElementById('tc'+i);"
              "if(tp==116){"                                                               // table
                "dt.addColumn('string',c.t||'');"
                "for(var j=0;j<c.s.length;j++)dt.addColumn('number',c.s[j].l);"
                "var rows=[];"
                "if(c.s.length>0)for(var k=0;k<c.s[0].d.length;k++){"
                  "var lb=c.lb&&c.lb[k]?c.lb[k]:''+(k+1);"
                  "var r=[lb];"
                  "for(var j=0;j<c.s.length;j++)r.push(c.s[j].d[k][1]);"
                  "rows.push(r);}"
                "dt.addRows(rows);"
                "new google.visualization.Table(el).draw(dt,{showRowNumber:false,width:'100%%'});"
              "}else{"                                                                     // charts
                "dt.addColumn('datetime','Time');"
                "for(var j=0;j<c.s.length;j++)dt.addColumn('number',c.s[j].l);"
                "var rows=[];"
                "if(c.s.length>0)for(var k=0;k<c.s[0].d.length;k++){"
                  "var r=[new Date(N.getTime()+c.s[0].d[k][0]*60000)];"
                  "for(var j=0;j<c.s.length;j++)r.push(c.s[j].d[k][1]);"
                  "rows.push(r);}"
                "dt.addRows(rows);"
                "var colors=c.s.map(function(x){return x.c;});"
                "var va={title:c.u};"
                "if(c.mn<c.mx){va.minValue=c.mn;va.maxValue=c.mx;}"
                "var dual=false,sr={},vx={};"
                "vx[0]=va;"
                "for(var j=0;j<c.s.length;j++){"
                  "var s=c.s[j];"
                  "if(j>0&&(s.mn!=c.s[0].mn||s.mx!=c.s[0].mx)){"
                    "dual=true;sr[j]={targetAxisIndex:1};"
                    "var a2={title:s.u};"
                    "if(s.mn<s.mx){a2.minValue=s.mn;a2.maxValue=s.mx;}"
                    "vx[1]=a2;"
                  "}else{sr[j]={targetAxisIndex:0};}"
                "}"
                "if(dual&&c.s[0].mn<c.s[0].mx){vx[0]={title:c.s[0].u,minValue:c.s[0].mn,maxValue:c.s[0].mx};}"
                "var dw=c.s[0].d[0]?c.s[0].d[0][0]*60000:-86400000;"
                "var hfmt=dw<-172800000?'EEE HH:mm':'HH:mm';"
                "var o={title:c.t,curveType:'none',"
                  "hAxis:{format:hfmt,viewWindow:{min:new Date(N.getTime()+dw),max:N}},"
                  "colors:colors,"
                  "lineWidth:1,pointSize:0,"
                  "chartArea:{width:'75%%',height:'65%%'}};"
                "if(dual){o.series=sr;o.vAxes=vx;}else{o.vAxis=va;}"
                "if(tp==98)new google.visualization.BarChart(el).draw(dt,o);"              // 'b'=98
                "else if(tp==99||tp==1)new google.visualization.ColumnChart(el).draw(dt,o);" // 'c'=99 or legacy 1
                "else if(tp==104)new google.visualization.Histogram(el).draw(dt,o);"       // 'h'=104
                "else if(tp==115){o.isStacked=true;new google.visualization.ColumnChart(el).draw(dt,o);}" // 's'=115
                "else new google.visualization.LineChart(el).draw(dt,o);"                  // 'l'=108 or legacy 0
              "}"
            "}"
          "}"
          "google.charts.setOnLoadCallback(_tcD);"
          "</script>"
        ));
        tc_chart_lib_sent = true;
      }

      // 2) Determine chart ID: new chart gets next seq, continuation uses previous
      int chart_id;
      if (new_chart) {
        chart_id = tc_chart_seq;
      } else {
        chart_id = tc_chart_seq > 0 ? tc_chart_seq - 1 : 0;
      }

      // 3) New chart: emit div container + _tcN() registration with optional y-axis range
      if (new_chart) {
        char div_style[64];
        if (type == 116) {
          strcpy(div_style, "width:100%;overflow-x:auto");
        } else if (tc_chart_width > 0 && tc_chart_height > 0) {
          snprintf(div_style, sizeof(div_style), "width:%dpx;height:%dpx;margin:0 auto",
            tc_chart_width, tc_chart_height);
        } else if (tc_chart_width > 0) {
          snprintf(div_style, sizeof(div_style), "width:%dpx;height:300px;margin:0 auto",
            tc_chart_width);
        } else if (tc_chart_height > 0) {
          snprintf(div_style, sizeof(div_style), "width:100%%;height:%dpx", tc_chart_height);
        } else {
          strcpy(div_style, "width:100%;height:300px");
        }
        if (fixed_range) {
          char ymin_s[16], ymax_s[16];
          dtostrf(ymin, 1, 1, ymin_s);
          dtostrf(ymax, 1, 1, ymax_s);
          WSContentSend_P(PSTR(
            "<div id=\"tc%d\" style=\"%s\"></div>"
            "<script>_tcN(%d,'%s','%s',%d,%s,%s);</script>"
          ), chart_id, div_style, chart_id, title, axis_unit, type, ymin_s, ymax_s);
        } else {
          WSContentSend_P(PSTR(
            "<div id=\"tc%d\" style=\"%s\"></div>"
            "<script>_tcN(%d,'%s','%s',%d,0,0);</script>"
          ), chart_id, div_style, chart_id, title, axis_unit, type);
        }
      }

      // 4) Resolve float array and emit data as _tcA() call
      int32_t *arr = tc_resolve_ref(vm, arr_ref);
      if (!arr) break;
      int32_t arr_len = tc_ref_maxlen(vm, arr_ref);
      if (count > arr_len) count = arr_len;

      char cbuf[12];
      snprintf(cbuf, sizeof(cbuf), "#%06x", (unsigned int)(color & 0xFFFFFF));

      char ymin_a[16], ymax_a[16];
      dtostrf(ymin, 1, 1, ymin_a);
      dtostrf(ymax, 1, 1, ymax_a);

      WSContentSend_P(PSTR("<script>_tcA(%d,'%s','%s',["),
        chart_id, series_name, cbuf);

      // Unwind ring buffer: oldest entry first, interpret as float
      // Use count (not arr_len) for wrap — arr_len is "remaining globals" not array size
      for (int32_t i = 0; i < count; i++) {
        int32_t idx = pos - count + i;
        if (idx < 0) idx += count;
        float fval;
        memcpy(&fval, &arr[idx], sizeof(float));
        int32_t mins_ago = -((count - 1 - i) * interval);
        char vbuf[16];
        dtostrf(fval, 1, decimals, vbuf);
        WSContentSend_P(PSTR("%s[%d,%s]"),
          (i > 0) ? "," : "",
          mins_ago, vbuf);
        if ((i & 63) == 63) WSContentFlush();
      }

      WSContentSend_P(PSTR("],%s,%s,'%s');</script>"), ymin_a, ymax_a, axis_unit);
      WSContentFlush();

      if (new_chart) tc_chart_seq++;
#else
      // No webserver — still pop all 11 args
      for (int i = 0; i < 11; i++) TC_POP(vm);
#endif
      break;
    }

    case SYS_PLUGIN_QUERY: {
      // pluginQuery(dst, index, p1, p2) -> strlen
      // Calls Plugin_Query(index, (p1<<8)|p2, 0), copies result string to dst
      int32_t p2_val    = TC_POP(vm);
      int32_t p1_val    = TC_POP(vm);
      int32_t index     = TC_POP(vm);
      int32_t dst_ref   = TC_POP(vm);
#ifdef USE_BINPLUGINS
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      if (!dst) { TC_PUSH(vm, 0); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref) - 1;
      if (maxSlots <= 0) { TC_PUSH(vm, 0); break; }
      uint16_t par = ((uint8_t)p1_val << 8) | (uint8_t)p2_val;
      char *rbuff = (char*)Plugin_Query((uint16_t)index, par, 0);
      if (rbuff) {
        int32_t rlen = strlen(rbuff);
        if (rlen > maxSlots) rlen = maxSlots;
        for (int32_t i = 0; i < rlen; i++) dst[i] = (int32_t)(uint8_t)rbuff[i];
        dst[rlen] = 0;
        free(rbuff);
        TC_PUSH(vm, rlen);
      } else {
        dst[0] = 0;
        TC_PUSH(vm, 0);
      }
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_SORT_ARRAY: {
      // sortArray(arr, count, flags)
      // flags: 0=int asc, 1=float asc, 2=int desc, 3=float desc
      int32_t flags   = TC_POP(vm);
      int32_t count   = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);
      int32_t *arr = tc_resolve_ref(vm, arr_ref);
      if (!arr || count <= 1) break;
      int32_t maxLen = tc_ref_maxlen(vm, arr_ref);
      if (count > maxLen) count = maxLen;
      // Insertion sort — small arrays, no extra memory, stable
      for (int32_t i = 1; i < count; i++) {
        int32_t key = arr[i];
        int32_t j = i - 1;
        if (flags & 1) {
          // Float comparison
          float fkey; memcpy(&fkey, &key, sizeof(float));
          while (j >= 0) {
            float fj; memcpy(&fj, &arr[j], sizeof(float));
            bool swap = (flags & 2) ? (fj < fkey) : (fj > fkey);
            if (!swap) break;
            arr[j + 1] = arr[j];
            j--;
          }
        } else {
          // Integer comparison
          while (j >= 0) {
            bool swap = (flags & 2) ? (arr[j] < key) : (arr[j] > key);
            if (!swap) break;
            arr[j + 1] = arr[j];
            j--;
          }
        }
        arr[j + 1] = key;
      }
      break;
    }

    // ── Webcam (multiplexed) ───────────────────────────
#if defined(ESP32) && defined(USE_WEBCAM)
    case SYS_CAM_CONTROL: {
      // camControl(sel, p1, p2) -> int
      int32_t p2  = TC_POP(vm);
      int32_t p1  = TC_POP(vm);
      int32_t sel = TC_POP(vm);
      int32_t res = 0;
      switch (sel) {
        case 0: res = WcSetup(p1); break;                  // init(resolution)
        case 1: res = WcGetFrame(p1); break;               // capture(bufnum) -> framesize
        case 2: res = WcSetOptions(p1, p2); break;         // options(sel, val)
        case 3: res = WcGetWidth(); break;                 // width()
        case 4: res = WcGetHeight(); break;                // height()
        case 5: res = WcSetStreamserver(p1); break;        // stream(on/off)
        case 6: res = WcSetMotionDetect(p1); break;        // motion(param)
        case 7: {
          // savePic(bufnum, filehandle) — write picture from RAM buffer to open file
          uint8_t *buff;
          int32_t maxps = WcGetPicstore(-1, 0);
          int32_t bnum = p1;
          if (bnum < 1 || bnum > maxps) bnum = 1;
          uint32_t len = WcGetPicstore(bnum - 1, &buff);
          if (len && p2 >= 0 && p2 < TC_MAX_FILE_HANDLES && Tinyc->file_used[p2]) {
            res = tc_file_handles[p2].write(buff, len);
          }
          break;
        }
        default:
          AddLog(LOG_LEVEL_ERROR, PSTR("TCC: camControl unknown sel=%d"), sel);
          res = -1;
          break;
      }
      TC_PUSH(vm, res);
      break;
    }
#else
    case SYS_CAM_CONTROL: {
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
      break;
    }
#endif

    // ── Display drawing (direct renderer calls) ──────
#ifdef USE_DISPLAY
    case SYS_DSP_TEXT: {
      int32_t ref = TC_POP(vm);
      char tbuf[256];
      tc_ref_to_cstr(vm, ref, tbuf, sizeof(tbuf));
      char *savptr = XdrvMailbox.data;
      XdrvMailbox.data = tbuf;
      XdrvMailbox.data_len = strlen(tbuf);
      DisplayText();
      XdrvMailbox.data = savptr;
      break;
    }
    case SYS_DSP_CLEAR:
      if (renderer) renderer->clearDisplay();
      disp_xpos = 0;
      disp_ypos = 0;
      break;
    case SYS_DSP_POS: {
      int32_t y = TC_POP(vm);
      int32_t x = TC_POP(vm);
      disp_xpos = x;
      disp_ypos = y;
      break;
    }
    case SYS_DSP_FONT: {
      int32_t f = TC_POP(vm);
      if (renderer) {
        renderer->setTextFont(f);
        if (f) renderer->setTextSize(1);
      }
      break;
    }
    case SYS_DSP_SIZE: {
      int32_t s = TC_POP(vm);
      if (renderer) renderer->setTextSize(s);
      break;
    }
    case SYS_DSP_COLOR: {
      int32_t cbg = TC_POP(vm);
      int32_t cfg = TC_POP(vm);
      fg_color = (uint16_t)cfg;
      bg_color = (uint16_t)cbg;
      if (renderer) renderer->setTextColor(fg_color, bg_color);
      break;
    }
    case SYS_DSP_DRAW: {
      int32_t ref = TC_POP(vm);
      char tbuf[128];
      tc_ref_to_cstr(vm, ref, tbuf, sizeof(tbuf));
      tc_display_text_padded(tbuf);
      break;
    }
    case SYS_DSP_PIXEL: {
      int32_t y = TC_POP(vm);
      int32_t x = TC_POP(vm);
      if (renderer) renderer->drawPixel(x, y, fg_color);
      break;
    }
    case SYS_DSP_LINE: {
      int32_t y1 = TC_POP(vm);
      int32_t x1 = TC_POP(vm);
      if (renderer) renderer->writeLine(disp_xpos, disp_ypos, x1, y1, fg_color);
      disp_xpos = x1;
      disp_ypos = y1;
      break;
    }
    case SYS_DSP_RECT: {
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      if (renderer) renderer->drawRect(disp_xpos, disp_ypos, w, h, fg_color);
      break;
    }
    case SYS_DSP_FILL_RECT: {
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      if (renderer) renderer->fillRect(disp_xpos, disp_ypos, w, h, fg_color);
      break;
    }
    case SYS_DSP_CIRCLE: {
      int32_t r = TC_POP(vm);
      if (renderer) renderer->drawCircle(disp_xpos, disp_ypos, r, fg_color);
      break;
    }
    case SYS_DSP_FILL_CIRCLE: {
      int32_t r = TC_POP(vm);
      if (renderer) renderer->fillCircle(disp_xpos, disp_ypos, r, fg_color);
      break;
    }
    case SYS_DSP_HLINE: {
      int32_t w = TC_POP(vm);
      if (renderer) renderer->writeFastHLine(disp_xpos, disp_ypos, w, fg_color);
      disp_xpos += w;
      break;
    }
    case SYS_DSP_VLINE: {
      int32_t h = TC_POP(vm);
      if (renderer) renderer->writeFastVLine(disp_xpos, disp_ypos, h, fg_color);
      disp_ypos += h;
      break;
    }
    case SYS_DSP_ROUND_RECT: {
      int32_t r = TC_POP(vm);
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      if (renderer) renderer->drawRoundRect(disp_xpos, disp_ypos, w, h, r, fg_color);
      break;
    }
    case SYS_DSP_FILL_RRECT: {
      int32_t r = TC_POP(vm);
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      if (renderer) renderer->fillRoundRect(disp_xpos, disp_ypos, w, h, r, fg_color);
      break;
    }
    case SYS_DSP_TRIANGLE: {
      int32_t y2 = TC_POP(vm);
      int32_t x2 = TC_POP(vm);
      int32_t y1 = TC_POP(vm);
      int32_t x1 = TC_POP(vm);
      if (renderer) renderer->drawTriangle(disp_xpos, disp_ypos, x1, y1, x2, y2, fg_color);
      break;
    }
    case SYS_DSP_FILL_TRI: {
      int32_t y2 = TC_POP(vm);
      int32_t x2 = TC_POP(vm);
      int32_t y1 = TC_POP(vm);
      int32_t x1 = TC_POP(vm);
      if (renderer) renderer->fillTriangle(disp_xpos, disp_ypos, x1, y1, x2, y2, fg_color);
      break;
    }
    case SYS_DSP_DIM: {
      int32_t val = TC_POP(vm);
      if (renderer) renderer->dim(val);
      break;
    }
    case SYS_DSP_ONOFF: {
      int32_t on = TC_POP(vm);
      DisplayOnOff(on);
      break;
    }
    case SYS_DSP_UPDATE:
      if (renderer) renderer->Updateframe();
      break;
    case SYS_DSP_PICTURE: {
      int32_t scale = TC_POP(vm);
      int32_t ci = TC_POP(vm);
      const char *fname = tc_get_const_str(vm, ci);
      if (fname) {
        Draw_RGB_Bitmap((char*)fname, disp_xpos, disp_ypos, (uint8_t)scale, false, 0, 0);
      }
      break;
    }
    case SYS_DSP_WIDTH:
      TC_PUSH(vm, renderer ? renderer->width() : 0);
      break;
    case SYS_DSP_HEIGHT:
      TC_PUSH(vm, renderer ? renderer->height() : 0);
      break;
    case SYS_DSP_TEXT_STR: {
      int32_t ci = TC_POP(vm);
      const char *cmd = tc_get_const_str(vm, ci);
      if (cmd) {
        char tbuf[256];
        strlcpy(tbuf, cmd, sizeof(tbuf));
        char *savptr = XdrvMailbox.data;
        XdrvMailbox.data = tbuf;
        XdrvMailbox.data_len = strlen(tbuf);
        DisplayText();
        XdrvMailbox.data = savptr;
      }
      break;
    }
    case SYS_DSP_DRAW_STR: {
      int32_t ci = TC_POP(vm);
      const char *str = tc_get_const_str(vm, ci);
      if (str) {
        tc_display_text_padded(str);
      }
      break;
    }
    case SYS_DSP_PAD:
      tc_dsp_pad = (int16_t)TC_POP(vm);
      break;

    // ── Touch buttons & sliders ──────────────────────
#ifdef USE_TOUCH_BUTTONS
    case SYS_DSP_BUTTON:    // power button
    case SYS_DSP_TBUTTON:   // virtual toggle
    case SYS_DSP_PBUTTON: { // virtual push
      int32_t textCI = TC_POP(vm);
      int32_t ts  = TC_POP(vm);
      int32_t tc_ = TC_POP(vm);
      int32_t fc  = TC_POP(vm);
      int32_t oc  = TC_POP(vm);
      int32_t h   = TC_POP(vm);
      int32_t w   = TC_POP(vm);
      int32_t y   = TC_POP(vm);
      int32_t x   = TC_POP(vm);
      int32_t num = TC_POP(vm);
      num = num % MAX_TOUCH_BUTTONS;
      const char *text = tc_get_const_str(vm, textCI);
      if (text && renderer) {
        if (buttons[num]) delete buttons[num];
        buttons[num] = new VButton();
        if (buttons[num]) {
          char lbl[32];
          strlcpy(lbl, text, sizeof(lbl));
          buttons[num]->vpower.slider = 0;
          // Colors are RGB565 — pass directly (no GetColorFromIndex)
          buttons[num]->xinitButtonUL(renderer, x, y, w, h,
            (uint16_t)oc, (uint16_t)fc, (uint16_t)tc_, lbl, (uint8_t)ts);
          if (id == SYS_DSP_BUTTON) {
            // power button
            buttons[num]->vpower.is_virtual = 0;
            buttons[num]->xdrawButton(bitRead(TasmotaGlobal.power, num));
          } else {
            // virtual button (toggle or push)
            buttons[num]->vpower.is_virtual = 1;
            buttons[num]->vpower.is_pushbutton = (id == SYS_DSP_PBUTTON) ? 1 : 0;
            buttons[num]->xdrawButton(buttons[num]->vpower.on_off);
          }
        }
      }
      break;
    }
    case SYS_DSP_SLIDER: { // slider
      int32_t bc  = TC_POP(vm);
      int32_t fc  = TC_POP(vm);
      int32_t bg  = TC_POP(vm);
      int32_t ne  = TC_POP(vm);
      int32_t h   = TC_POP(vm);
      int32_t w   = TC_POP(vm);
      int32_t y   = TC_POP(vm);
      int32_t x   = TC_POP(vm);
      int32_t num = TC_POP(vm);
      num = num % MAX_TOUCH_BUTTONS;
      if (renderer) {
        if (buttons[num]) delete buttons[num];
        buttons[num] = new VButton();
        if (buttons[num]) {
          buttons[num]->vpower.slider = 1;
          // Colors are RGB565 — pass directly
          buttons[num]->SliderInit(renderer, x, y, w, h, ne,
            (uint16_t)bg, (uint16_t)fc, (uint16_t)bc);
        }
      }
      break;
    }
    case SYS_DSP_BTN_STATE: { // set button/slider state
      int32_t val = TC_POP(vm);
      int32_t num = TC_POP(vm);
      num = num % MAX_TOUCH_BUTTONS;
      if (buttons[num]) {
        if (buttons[num]->vpower.slider) {
          buttons[num]->UpdateSlider(-val, -val);
        } else {
          buttons[num]->vpower.on_off = val;
          buttons[num]->xdrawButton(val);
        }
      }
      break;
    }
    case SYS_TOUCH_BUTTON: { // read button/slider state
      int32_t num = TC_POP(vm);
      int32_t result = -1;
      if (num >= 0 && num < MAX_TOUCH_BUTTONS && buttons[num]) {
        result = buttons[num]->vpower.on_off;
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_DSP_BTN_DEL: { // delete button/slider
      int32_t num = TC_POP(vm);
      if (num == -1) {
        // delete all
        for (int i = 0; i < MAX_TOUCH_BUTTONS; i++) {
          if (buttons[i]) {
            if (renderer) renderer->fillRect(buttons[i]->spars.xp, buttons[i]->spars.yp,
              buttons[i]->spars.xs, buttons[i]->spars.ys, bg_color);
            delete buttons[i];
            buttons[i] = 0;
          }
        }
      } else if (num >= 0 && num < MAX_TOUCH_BUTTONS && buttons[num]) {
        if (renderer) renderer->fillRect(buttons[num]->spars.xp, buttons[num]->spars.yp,
          buttons[num]->spars.xs, buttons[num]->spars.ys, bg_color);
        delete buttons[num];
        buttons[num] = 0;
      }
      break;
    }
#else  // !USE_TOUCH_BUTTONS — pop args but do nothing
    case SYS_DSP_BUTTON:
    case SYS_DSP_TBUTTON:
    case SYS_DSP_PBUTTON:
      for (int i = 0; i < 10; i++) TC_POP(vm); break;
    case SYS_DSP_SLIDER:
      for (int i = 0; i < 9; i++) TC_POP(vm); break;
    case SYS_DSP_BTN_STATE:
      TC_POP(vm); TC_POP(vm); break;
    case SYS_TOUCH_BUTTON:
      TC_POP(vm); TC_PUSH(vm, -1); break;
    case SYS_DSP_BTN_DEL:
      TC_POP(vm); break;
#endif // USE_TOUCH_BUTTONS

#else  // !USE_DISPLAY — pop args from stack but do nothing
    case SYS_DSP_TEXT:
    case SYS_DSP_DRAW:
      TC_POP(vm); break;
    case SYS_DSP_CLEAR:
    case SYS_DSP_UPDATE:
      break;
    case SYS_DSP_POS:
    case SYS_DSP_COLOR:
    case SYS_DSP_PIXEL:
    case SYS_DSP_LINE:
    case SYS_DSP_RECT:
    case SYS_DSP_FILL_RECT:
      TC_POP(vm); TC_POP(vm); break;
    case SYS_DSP_FONT:
    case SYS_DSP_SIZE:
    case SYS_DSP_CIRCLE:
    case SYS_DSP_FILL_CIRCLE:
    case SYS_DSP_HLINE:
    case SYS_DSP_VLINE:
    case SYS_DSP_DIM:
    case SYS_DSP_ONOFF:
      TC_POP(vm); break;
    case SYS_DSP_ROUND_RECT:
    case SYS_DSP_FILL_RRECT:
      TC_POP(vm); TC_POP(vm); TC_POP(vm); break;
    case SYS_DSP_TRIANGLE:
    case SYS_DSP_FILL_TRI:
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); break;
    case SYS_DSP_PICTURE:
      TC_POP(vm); TC_POP(vm); break;
    case SYS_DSP_WIDTH:
    case SYS_DSP_HEIGHT:
      TC_PUSH(vm, 0); break;
    case SYS_DSP_TEXT_STR:
    case SYS_DSP_DRAW_STR:
    case SYS_DSP_PAD:
      TC_POP(vm); break;
    case SYS_DSP_BUTTON:
    case SYS_DSP_TBUTTON:
    case SYS_DSP_PBUTTON:
      // 10 args: num,x,y,w,h,oc,fc,tc,ts,text_const
      for (int i = 0; i < 10; i++) TC_POP(vm); break;
    case SYS_DSP_SLIDER:
      // 9 args: num,x,y,w,h,ne,bg,fc,bc
      for (int i = 0; i < 9; i++) TC_POP(vm); break;
    case SYS_DSP_BTN_STATE:
      TC_POP(vm); TC_POP(vm); break;
    case SYS_TOUCH_BUTTON:
      TC_POP(vm); TC_PUSH(vm, -1); break;
    case SYS_DSP_BTN_DEL:
      TC_POP(vm); break;
#endif // USE_DISPLAY

    // ── Audio ──────────────────────────────────────────
    // With binary plugins: call I2S plugin directly via Plugin_Query (no deferral needed)
    // Without plugins: defer through ExecuteCommand in main loop
#if defined(USE_BINPLUGINS) && !defined(USE_I2S_AUDIO)
    case SYS_AUDIO_VOL: {
      int32_t vol = TC_POP(vm);
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "%d", vol);
      Plugin_Query(42, 1, cmd);
      break;
    }
    case SYS_AUDIO_PLAY: {
      int32_t ci = TC_POP(vm);
      const char *file = tc_get_const_str(vm, ci);
      if (file) {
        Plugin_Query(42, 0, (char*)file);
      }
      break;
    }
    case SYS_AUDIO_SAY: {
      int32_t ci = TC_POP(vm);
      const char *text = tc_get_const_str(vm, ci);
      if (text) {
        Plugin_Query(42, 2, (char*)text);
      }
      break;
    }
#else
    case SYS_AUDIO_VOL: {
      int32_t vol = TC_POP(vm);
      char cmd[24];
      snprintf(cmd, sizeof(cmd), "I2SVol %d", vol);
      tc_defer_command(cmd);
      break;
    }
    case SYS_AUDIO_PLAY: {
      int32_t ci = TC_POP(vm);
      const char *file = tc_get_const_str(vm, ci);
      if (file) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "I2SPlay %s", file);
        tc_defer_command(cmd);
      }
      break;
    }
    case SYS_AUDIO_SAY: {
      int32_t ci = TC_POP(vm);
      const char *text = tc_get_const_str(vm, ci);
      if (text) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "I2SSay %s", text);
        tc_defer_command(cmd);
      }
      break;
    }
#endif

    // ── TCP server (Scripter-compatible ws* functions) ──
    case SYS_TCP_OPEN: {  // wso(port)
      int32_t port = TC_POP(vm);
      if (TasmotaGlobal.global_state.network_down) {
        TC_PUSH(vm, -2);
      } else {
        if (Tinyc->tcp_server) {
          Tinyc->tcp_client.stop();
          Tinyc->tcp_server->stop();
          delete Tinyc->tcp_server;
        }
        Tinyc->tcp_server = new WiFiServer(port);
        if (!Tinyc->tcp_server) {
          TC_PUSH(vm, -1);
        } else {
          Tinyc->tcp_server->begin();
          Tinyc->tcp_server->setNoDelay(true);
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP server started on port %d"), port);
          TC_PUSH(vm, 0);
        }
      }
      break;
    }
    case SYS_TCP_CLOSE: {  // wsc()
      if (Tinyc->tcp_server) {
        Tinyc->tcp_client.stop();
        Tinyc->tcp_server->stop();
        delete Tinyc->tcp_server;
        Tinyc->tcp_server = nullptr;
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP server closed"));
      }
      break;
    }
    case SYS_TCP_AVAILABLE: {  // wsa()
      int32_t avail = 0;
      if (Tinyc->tcp_server) {
        if (Tinyc->tcp_server->hasClient()) {
          Tinyc->tcp_client = Tinyc->tcp_server->available();
        }
        if (Tinyc->tcp_client && Tinyc->tcp_client.connected()) {
          avail = Tinyc->tcp_client.available();
        }
      }
      TC_PUSH(vm, avail);
      break;
    }
    case SYS_TCP_READ_STR: {  // wsrs(buf)
      int32_t ref = TC_POP(vm);
      int32_t count = 0;
      if (Tinyc->tcp_server && Tinyc->tcp_client.connected()) {
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base) {
          uint16_t slen = Tinyc->tcp_client.available();
          if (slen > 254) slen = 254;  // cap to reasonable char[] size
          for (uint16_t i = 0; i < slen; i++) {
            base[i] = Tinyc->tcp_client.read();
          }
          base[slen] = 0;  // null terminate
          count = slen;
        }
      }
      TC_PUSH(vm, count);
      break;
    }
    case SYS_TCP_WRITE_STR: {  // wsws(str)
      int32_t ref = TC_POP(vm);
      if (Tinyc->tcp_server && Tinyc->tcp_client.connected()) {
        char buf[256];
        tc_ref_to_cstr(vm, ref, buf, sizeof(buf));
        Tinyc->tcp_client.write(buf, strlen(buf));
      }
      break;
    }
    case SYS_TCP_READ_ARR: {  // wsra(arr)
      int32_t ref = TC_POP(vm);
      int32_t count = 0;
      if (Tinyc->tcp_server && Tinyc->tcp_client.connected()) {
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base) {
          uint16_t slen = Tinyc->tcp_client.available();
          for (uint16_t i = 0; i < slen; i++) {
            base[i] = Tinyc->tcp_client.read();
          }
          count = slen;
        }
      }
      TC_PUSH(vm, count);
      break;
    }
    case SYS_TCP_WRITE_ARR: {  // wswa(arr, num, type)
      int32_t type = TC_POP(vm);
      int32_t num  = TC_POP(vm);
      int32_t ref  = TC_POP(vm);
      if (Tinyc->tcp_server && Tinyc->tcp_client.connected()) {
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base) {
          uint8_t *abf = (uint8_t*)malloc(num * 4);
          if (abf) {
            uint8_t *p = abf;
            uint16_t dlen = 0;
            for (int32_t i = 0; i < num; i++) {
              int32_t val = base[i];
              switch (type) {
                case 0:  // uint8
                  *p++ = (uint8_t)val;
                  dlen++;
                  break;
                case 1: {  // uint16 big-endian
                  uint16_t wval = (uint16_t)val;
                  *p++ = (wval >> 8);
                  *p++ = wval & 0xFF;
                  dlen += 2;
                  break;
                }
                case 2: {  // sint16 big-endian
                  int16_t swval = (int16_t)val;
                  *p++ = ((uint16_t)swval >> 8);
                  *p++ = swval & 0xFF;
                  dlen += 2;
                  break;
                }
                case 3: {  // float (raw 32-bit big-endian)
                  uint32_t lval = (uint32_t)val;
                  *p++ = (lval >> 24);
                  *p++ = (lval >> 16) & 0xFF;
                  *p++ = (lval >> 8) & 0xFF;
                  *p++ = lval & 0xFF;
                  dlen += 4;
                  break;
                }
              }
            }
            Tinyc->tcp_client.write(abf, dlen);
            free(abf);
          }
        }
      }
      break;
    }

    // ── Persist variables ─────────────────────────────
    case SYS_PERSIST_SAVE:
      tc_persist_save(vm);
      break;

    // ── Deep Sleep (ESP32) ──────────────────────────────
#ifdef ESP32
    case SYS_DEEP_SLEEP: {
      a = TC_POP(vm);  // seconds
      if (a > 0) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: deep sleep %d sec"), a);
        tc_persist_save(vm);  // save persist vars before sleep
        SettingsSaveAll();
        esp_sleep_enable_timer_wakeup((uint64_t)a * 1000000ULL);
        esp_deep_sleep_start();
      }
      break;
    }
    case SYS_DEEP_SLEEP_GPIO: {
      int32_t level = TC_POP(vm);  // wakeup level (0=low, 1=high)
      int32_t pin   = TC_POP(vm);  // GPIO pin
      a = TC_POP(vm);              // seconds (0 = GPIO only, no timer)
      if (pin < 0 || pin >= MAX_GPIO_PIN) break;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: deep sleep %d sec, wake GPIO%d %s"),
        a, pin, level ? "HIGH" : "LOW");
      tc_persist_save(vm);
      SettingsSaveAll();
      if (a > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)a * 1000000ULL);
      }
#if SOC_PM_SUPPORT_EXT1_WAKEUP
      if (level == 0) {
        esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ANY_HIGH);
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
        rtc_gpio_pullup_dis((gpio_num_t)pin);
        rtc_gpio_pulldown_en((gpio_num_t)pin);
#endif
      } else {
#if CONFIG_IDF_TARGET_ESP32
        esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ALL_LOW);
#else
        esp_sleep_enable_ext1_wakeup_io(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW);
#endif
#if SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
        rtc_gpio_pullup_en((gpio_num_t)pin);
        rtc_gpio_pulldown_dis((gpio_num_t)pin);
#endif
      }
#else
      // Fallback: GPIO wakeup mode
      const gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = (gpio_pullup_t)!level,
        .pull_down_en = (gpio_pulldown_t)level
      };
      gpio_config(&config);
      if (level == 0) {
        esp_deep_sleep_enable_gpio_wakeup(1ULL << pin, ESP_GPIO_WAKEUP_GPIO_LOW);
      } else {
        esp_deep_sleep_enable_gpio_wakeup(1ULL << pin, ESP_GPIO_WAKEUP_GPIO_HIGH);
      }
#endif
      esp_deep_sleep_start();
      break;
    }
    case SYS_WAKEUP_CAUSE:
      TC_PUSH(vm, (int32_t)esp_sleep_get_wakeup_cause());
      break;
#else
    // ESP8266: not supported, push 0
    case SYS_DEEP_SLEEP:
      TC_POP(vm);
      break;
    case SYS_DEEP_SLEEP_GPIO:
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      break;
    case SYS_WAKEUP_CAUSE:
      TC_PUSH(vm, 0);
      break;
#endif // ESP32

    // ── Email (requires USE_SENDMAIL) ─────────────────
#if defined(USE_SENDMAIL) || defined(USE_ESP32MAIL)
    case SYS_EMAIL_BODY: {
      // mailBody(body) — set email body text from char array
      int32_t body_ref = TC_POP(vm);
      // Free previous body if any
      if (Tinyc->email_body) { free(Tinyc->email_body); Tinyc->email_body = nullptr; }
      // Convert char array ref to C string
      int32_t *bp = tc_resolve_ref(vm, body_ref);
      if (bp) {
        int32_t maxLen = tc_ref_maxlen(vm, body_ref);
        char *tmp = (char*)malloc(maxLen + 1);
        if (tmp) {
          int32_t i = 0;
          while (i < maxLen && bp[i] != 0) { tmp[i] = (char)bp[i]; i++; }
          tmp[i] = 0;
          Tinyc->email_body = tmp;
          AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: mailBody set (%d chars)"), i);
        }
      }
      break;
    }
    case SYS_EMAIL_BODY_STR: {
      // mailBody("literal") — set email body from string literal in const pool
      int32_t ci = TC_POP(vm);
      if (Tinyc->email_body) { free(Tinyc->email_body); Tinyc->email_body = nullptr; }
      const char *str = tc_get_const_str(vm, ci);
      if (str) {
        Tinyc->email_body = strdup(str);
        if (Tinyc->email_body) {
          AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: mailBody set from literal (%d chars)"), strlen(str));
        }
      }
      break;
    }
    case SYS_EMAIL_ATTACH: {
      // mailAttach("/path") — add file attachment (string literal from const pool)
      int32_t ci = TC_POP(vm);
      const char *path = tc_get_const_str(vm, ci);
      if (path && Tinyc->email_attach_count < TC_MAX_EMAIL_ATTACH) {
        Tinyc->email_attach[Tinyc->email_attach_count] = strdup(path);
        if (Tinyc->email_attach[Tinyc->email_attach_count]) {
          AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: mailAttach[%d] = %s"), Tinyc->email_attach_count, path);
          Tinyc->email_attach_count++;
        }
      }
      break;
    }
    case SYS_EMAIL_ATTACH_PIC: {
      // mailAttachPic(bufnum) — attach webcam picture from RAM buffer 1..4
      int32_t bufnum = TC_POP(vm);
      if (bufnum < 1) bufnum = 1;
      if (bufnum > 4) bufnum = 4;
      if (Tinyc->email_attach_count < TC_MAX_EMAIL_ATTACH) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "$%d", bufnum);
        Tinyc->email_attach[Tinyc->email_attach_count] = strdup(tmp);
        if (Tinyc->email_attach[Tinyc->email_attach_count]) {
          AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: mailAttachPic[%d] = buffer %d"), Tinyc->email_attach_count, bufnum);
          Tinyc->email_attach_count++;
        }
      }
      break;
    }
    case SYS_EMAIL_SEND: {
      // mailSend(params) — send email, params = char[] with "[server:port:user:passwd:from:to:subject]"
      int32_t params_ref = TC_POP(vm);
      int32_t *pp = tc_resolve_ref(vm, params_ref);
      if (!pp) { TC_PUSH(vm, 1); break; }
      int32_t maxLen = tc_ref_maxlen(vm, params_ref);
      // Build the SendMail command string: [params] body_or_*
      bool has_attachments = (Tinyc->email_attach_count > 0);
      // Determine body: use email_body if set, otherwise extract from params tail
      int32_t plen = 0;
      while (plen < maxLen && pp[plen] != 0) plen++;
      // Allocate buffer: params + body + margin
      int32_t bodyLen = Tinyc->email_body ? strlen(Tinyc->email_body) : 0;
      char *cmd = (char*)malloc(plen + bodyLen + 4);
      if (!cmd) {
        TC_PUSH(vm, 4);  // memory error
        break;
      }
      // Copy params string from int32 array to char buffer
      for (int32_t i = 0; i < plen; i++) cmd[i] = (char)pp[i];
      cmd[plen] = 0;
      // If we have pre-registered body/attachments, replace body with '*' to trigger callback
      if (has_attachments || Tinyc->email_body) {
        // Find end of ']' in params to replace body
        char *bracket = strchr(cmd, ']');
        if (bracket) {
          bracket[1] = '*';
          bracket[2] = 0;
        }
        Tinyc->email_active = true;
      }
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: mailSend(%s)"), cmd);
      uint16_t result = SendMail(cmd);
      free(cmd);
      // Clean up email state
      if (Tinyc->email_body) { free(Tinyc->email_body); Tinyc->email_body = nullptr; }
      for (uint8_t i = 0; i < Tinyc->email_attach_count; i++) {
        if (Tinyc->email_attach[i]) { free(Tinyc->email_attach[i]); Tinyc->email_attach[i] = nullptr; }
      }
      Tinyc->email_attach_count = 0;
      Tinyc->email_active = false;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: mailSend result = %d"), result);
      TC_PUSH(vm, (int32_t)result);
      break;
    }
#else
    case SYS_EMAIL_BODY:
    case SYS_EMAIL_BODY_STR:
      TC_POP(vm);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mailBody — USE_SENDMAIL not enabled"));
      break;
    case SYS_EMAIL_ATTACH:
      TC_POP(vm);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mailAttach — USE_SENDMAIL not enabled"));
      break;
    case SYS_EMAIL_ATTACH_PIC:
      TC_POP(vm);
      break;
    case SYS_EMAIL_SEND:
      TC_POP(vm);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mailSend — USE_SENDMAIL not enabled"));
      TC_PUSH(vm, -1);
      break;
#endif  // USE_SENDMAIL

    // ── Tesla Powerwall (requires TESLA_POWERWALL) ───
#if defined(ESP32) && defined(TESLA_POWERWALL)
    case SYS_PWL_REQUEST: {
      // pwlRequest("url") → int (0=ok, -1=fail)
      int32_t ci = TC_POP(vm);
      const char *url = tc_get_const_str(vm, ci);
      if (url) {
        TC_PUSH(vm, tc_call2pwl(vm, url));
      } else {
        TC_PUSH(vm, -1);
      }
      break;
    }
    case SYS_PWL_BIND: {
      // pwlBind(&var, "path#key") — register auto-fill binding
      int32_t ci = TC_POP(vm);    // const pool index for path string
      int32_t ref = TC_POP(vm);   // global variable ref
      // Only allow global refs (tag 0x80) — locals would go stale
      uint8_t tag = ((uint32_t)ref) >> 30;
      if (tag != 2) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC-PWL: pwlBind requires global variable"));
        break;
      }
      const char *path = tc_get_const_str(vm, ci);
      if (path && tc_pwl_bind_count < TC_PWL_MAX_BINDS) {
        tc_pwl_binds[tc_pwl_bind_count].ref = ref;
        strlcpy(tc_pwl_binds[tc_pwl_bind_count].path, path, sizeof(tc_pwl_binds[0].path));
        tc_pwl_bind_count++;
        AddLog(TC_PWL_LOGLVL, PSTR("TCC-PWL: bind[%d] = %s"), tc_pwl_bind_count - 1, path);
      }
      break;
    }
    case SYS_PWL_GET_FLOAT: {
      // pwlGet("path#key") → float (ad-hoc, uses string scanner)
      int32_t ci = TC_POP(vm);
      float fval = 0.0f;
      const char *path = tc_get_const_str(vm, ci);
      if (path && Tinyc->pwl_json) {
        fval = tc_pwl_scan_float(Tinyc->pwl_json, strlen(Tinyc->pwl_json), path);
      }
      uint32_t fi;
      memcpy(&fi, &fval, 4);
      TC_PUSH(vm, (int32_t)fi);
      break;
    }
    case SYS_PWL_GET_STR: {
      // pwlStr("path#key", buf) → int (length)
      int32_t buf_ref = TC_POP(vm);
      int32_t ci = TC_POP(vm);
      const char *path = tc_get_const_str(vm, ci);
      int32_t *buf = tc_resolve_ref(vm, buf_ref);
      if (path && buf && Tinyc->pwl_json) {
        int32_t maxLen = tc_ref_maxlen(vm, buf_ref) - 1;
        char tmp[256];
        int32_t slen = tc_pwl_scan_str(Tinyc->pwl_json, strlen(Tinyc->pwl_json), path, tmp, sizeof(tmp));
        if (slen > maxLen) slen = maxLen;
        for (int32_t i = 0; i < slen; i++) buf[i] = (int32_t)(uint8_t)tmp[i];
        buf[slen] = 0;
        TC_PUSH(vm, slen);
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }
#else
    case SYS_PWL_REQUEST:
      TC_POP(vm);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: pwlRequest — TESLA_POWERWALL not enabled"));
      TC_PUSH(vm, -1);
      break;
    case SYS_PWL_BIND:
      TC_POP(vm); TC_POP(vm);
      break;
    case SYS_PWL_GET_FLOAT:
      TC_POP(vm);
      TC_PUSH(vm, 0);
      break;
    case SYS_PWL_GET_STR:
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
      break;
#endif  // TESLA_POWERWALL

    // ── HomeKit ──────────────────────────────────────
#ifdef USE_HOMEKIT
    case SYS_HK_SET_CODE: {
      // hkSetCode("111-22-333") — set pairing code, start building descriptor
      a = TC_POP(vm);  // const pool index
      const char *code = tc_get_const_str(vm, a);
      if (!code) break;
      hk_build_pos = 0;
      hk_line_open = false;
      hk_var_count = 0;  // reset dirty tracking for new descriptor
      hk_build_pos += snprintf(hk_build_buf, sizeof(hk_build_buf), "%s\n", code);
      break;
    }
    case SYS_HK_ADD: {
      // hkAdd("name", HK_TEMPERATURE) — start a new device definition
      int32_t type = TC_POP(vm);
      a = TC_POP(vm);  // const pool index for name
      const char *name = tc_get_const_str(vm, a);
      if (!name) break;
      // Map type constant to CID + sub_type
      // 1=temp(10,0) 2=hum(10,1) 3=light_sensor(10,2) 4=battery(10,3)
      // 5=contact(10,5) 6=switch(8,0) 7=outlet(7,0) 8=light(5,0)
      static const struct { uint8_t cid; uint8_t sub; } hk_type_map[] = {
        {0,0},    // 0: unused
        {10,0},   // 1: HK_TEMPERATURE
        {10,1},   // 2: HK_HUMIDITY
        {10,2},   // 3: HK_LIGHT_SENSOR
        {10,3},   // 4: HK_BATTERY
        {10,5},   // 5: HK_CONTACT
        {8,0},    // 6: HK_SWITCH
        {7,0},    // 7: HK_OUTLET
        {5,0},    // 8: HK_LIGHT
      };
      uint8_t cid = 0, sub = 0;
      if (type >= 1 && type <= 8) {
        cid = hk_type_map[type].cid;
        sub = hk_type_map[type].sub;
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: hkAdd — unknown type %d"), type);
        break;
      }
      // Close previous device line if open
      if (hk_line_open) {
        hk_build_pos += snprintf(hk_build_buf + hk_build_pos,
          sizeof(hk_build_buf) - hk_build_pos, "\n");
      }
      // Start new device: name,cid,sub (no newline — hkVar will append ,idx)
      hk_build_pos += snprintf(hk_build_buf + hk_build_pos,
        sizeof(hk_build_buf) - hk_build_pos,
        "%s,%d,%d", name, cid, sub);
      hk_line_open = true;
      break;
    }
    case SYS_HK_VAR: {
      // hkVar(variable) — bind a variable to the current device
      a = TC_POP(vm);  // global index (from ADDR_GLOBAL)
      hk_build_pos += snprintf(hk_build_buf + hk_build_pos,
        sizeof(hk_build_buf) - hk_build_pos, ",%d", a);
      // Register for hkReady() dirty tracking
      if (hk_var_count < TC_HK_MAX_VARS) {
        hk_var_gidx[hk_var_count] = (int16_t)a;
        hk_var_dirty[hk_var_count] = 0;
        hk_var_count++;
      }
      break;
    }
    case SYS_HK_READY: {
      // hkReady(variable) — returns 1 if HomeKit changed this var since last check
      a = TC_POP(vm);  // global index (from ADDR_GLOBAL)
      int32_t result = 0;
      for (uint8_t i = 0; i < hk_var_count; i++) {
        if (hk_var_gidx[i] == (int16_t)a) {
          result = hk_var_dirty[i];
          hk_var_dirty[i] = 0;
          break;
        }
      }
      TC_PUSH(vm, result);
      break;
    }
    case SYS_HK_START: {
      // hkStart() — finalize descriptor and start HomeKit
      if (hk_build_pos == 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: hkStart — no hkSetCode() called"));
        TC_PUSH(vm, -1);
        break;
      }
      // Close last device line
      if (hk_line_open) {
        hk_build_pos += snprintf(hk_build_buf + hk_build_pos,
          sizeof(hk_build_buf) - hk_build_pos, "\n");
        hk_line_open = false;
      }
      // Allocate persistent copy (HAP thread keeps a reference)
      char *desc = (char *)malloc(hk_build_pos + 1);
      if (!desc) { TC_PUSH(vm, -1); break; }
      memcpy(desc, hk_build_buf, hk_build_pos);
      desc[hk_build_pos] = 0;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: hkStart descriptor:\n%s"), desc);
      // homekit_main declared at top of file
      int32_t ret = homekit_main(desc, 0);
      TC_PUSH(vm, ret);
      break;
    }
    case SYS_HK_INIT: {
      // hkInit(descriptor[]) -> int (0=ok, -1=error)
      a = TC_POP(vm);  // descriptor char[] ref
      int32_t *src = tc_resolve_ref(vm, a);
      if (!src) { TC_PUSH(vm, -1); break; }
      // Convert int32 char array to C string
      int32_t srcMax = tc_ref_maxlen(vm, a);
      char *desc = (char *)malloc(srcMax + 1);
      if (!desc) { TC_PUSH(vm, -1); break; }
      int32_t len = 0;
      for (int32_t i = 0; i < srcMax; i++) {
        if (src[i] == 0) break;
        desc[i] = (char)(src[i] & 0xFF);
        len++;
      }
      desc[len] = 0;
      // homekit_main declared at top of file
      int32_t ret = homekit_main(desc, 0);
      // desc is used by the HAP thread — don't free it (it's referenced as hk_desc)
      TC_PUSH(vm, ret);
      break;
    }
    case SYS_HK_STOP: {
      // homekit_main declared at top of file
      homekit_main(0, 3);  // flag 3 = stop
      break;
    }
    case SYS_HK_RESET: {
      // Direct partition erase — works even before hkStart()
      // (hap_reset_to_factory sends an event that needs a running HAP loop)
      // homekit_main declared at top of file
      homekit_main(0, 3);   // stop HAP loop if running
      homekit_main(0, 98);  // direct NVS/LittleFS erase
      break;
    }
#else
    case SYS_HK_SET_CODE:
      TC_POP(vm);  // discard const idx
      break;
    case SYS_HK_ADD:
      TC_POP(vm); TC_POP(vm);  // discard 2 args (name, type)
      break;
    case SYS_HK_VAR:
      TC_POP(vm);  // discard var ref
      break;
    case SYS_HK_READY:
      TC_POP(vm);  // discard var ref
      TC_PUSH(vm, 0);  // always 0 when HomeKit not enabled
      break;
    case SYS_HK_START:
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: hkStart — USE_HOMEKIT not enabled"));
      TC_PUSH(vm, -1);
      break;
    case SYS_HK_INIT:
      TC_POP(vm);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: hkInit — USE_HOMEKIT not enabled"));
      TC_PUSH(vm, -1);
      break;
    case SYS_HK_STOP:
    case SYS_HK_RESET:
      break;
#endif  // USE_HOMEKIT

    // ── Filesystem info ────────────────────────────────
    case SYS_FS_INFO: {
      // fsi(sel) -> int — sel: 0=total kB, 1=free kB (main FS)
      int32_t sel = TC_POP(vm);
#ifdef USE_UFILESYS
      TC_PUSH(vm, (int32_t)UfsInfo(sel ? 1 : 0, 0));
#else
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── Addressable LED strip (WS2812) ────────────────
#if defined(USE_LIGHT) && defined(USE_WS2812)
    case SYS_WS2812: {
      // setPixels(array, len, offset) — set LED pixels from array + show
      int32_t offset = TC_POP(vm);
      int32_t len    = TC_POP(vm);
      int32_t ref    = TC_POP(vm);
      int32_t *arr = tc_resolve_ref(vm, ref);
      if (!arr || len <= 0) break;
      extern void Ws2812ForceSuspend(void);
      extern void Ws2812ForceUpdate(void);
      extern void Ws2812SetColor(uint32_t, uint8_t, uint8_t, uint8_t, uint8_t);
      Ws2812ForceSuspend();
      for (int32_t cnt = 0; cnt < len; cnt++) {
        uint32_t index;
        if (!(offset & 0x1000)) {
          index = cnt + (offset & 0x7FF);
        } else {
          index = cnt / 2 + (offset & 0x7FF);
        }
        if (index > Settings->light_pixels) break;
        if (!(offset & 0x1000)) {
          // RGB mode: each element is 0xRRGGBB
          uint32_t col = (uint32_t)arr[cnt];
          Ws2812SetColor(index + 1, col >> 16, col >> 8, col, 0);
        } else {
          // RGBW mode: two elements per pixel (high word + low word)
          uint32_t hcol = (uint32_t)arr[cnt];
          cnt++;
          if (cnt >= len) break;
          uint32_t lcol = (uint32_t)arr[cnt];
          Ws2812SetColor(index + 1, hcol >> 8, hcol, lcol >> 8, lcol);
        }
      }
      Ws2812ForceUpdate();
      break;
    }
#else
    case SYS_WS2812:
      TC_POP(vm); TC_POP(vm); TC_POP(vm);  // discard 3 args
      break;
#endif  // USE_LIGHT && USE_WS2812

    // ── Debug ─────────────────────────────────────────
    case SYS_DEBUG_PRINT:
      a = TC_POP(vm);
      tc_output_int(a);
      tc_output_char('\n');
      break;
    case SYS_DEBUG_PRINT_STR:
      a = TC_POP(vm);
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        tc_output_string(vm->constants[a].str.ptr);
        tc_output_char('\n');
      }
      break;
    case SYS_DEBUG_DUMP:
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: PC=%d SP=%d FP=%d Frames=%d"),
        vm->pc - vm->code_offset, vm->sp, vm->fp, vm->frame_count);
      break;

    default:
      AddLog(LOG_LEVEL_ERROR, PSTR("TIC: unknown syscall %d at PC=%d"), id, vm->pc - vm->code_offset);
      return TC_ERR_BAD_SYSCALL;
  }
  return vm->error;
}

/*********************************************************************************************\
 * Persist variables: save/load binary file
 * Format: ['P','V'] [count u8] [index u16 LE, slotCount u16 LE, data: slotCount×4 bytes LE]
\*********************************************************************************************/

static void tc_persist_save(TcVM *vm) {
  if (vm->persist_count == 0 || vm->persist_file[0] == '\0') return;
#ifdef USE_UFILESYS
  // Calculate total size: 2 (magic) + 1 (count) + entries
  uint16_t total = 3;
  for (uint8_t i = 0; i < vm->persist_count; i++) {
    total += 4 + vm->persist[i].count * 4;  // index(2) + slotCount(2) + data
  }
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return;

  uint16_t pos = 0;
  buf[pos++] = 'P';
  buf[pos++] = 'V';
  buf[pos++] = vm->persist_count;

  for (uint8_t i = 0; i < vm->persist_count; i++) {
    uint16_t idx = vm->persist[i].index;
    uint16_t cnt = vm->persist[i].count;
    buf[pos++] = idx & 0xFF;
    buf[pos++] = (idx >> 8) & 0xFF;
    buf[pos++] = cnt & 0xFF;
    buf[pos++] = (cnt >> 8) & 0xFF;

    int32_t *src = nullptr;
    uint16_t src_max = 0;
    if (idx & 0x8000) {
      // Heap persist: index = 0x8000 | heapHandle
      uint16_t handle = idx & 0x7FFF;
      if (handle < TC_MAX_HEAP_HANDLES && vm->heap_data && vm->heap_handles &&
          vm->heap_handles[handle].alive) {
        src = &vm->heap_data[vm->heap_handles[handle].offset];
        src_max = vm->heap_handles[handle].size;
      }
    } else {
      // Global persist
      src = &vm->globals[idx];
      src_max = (idx < vm->globals_size) ? vm->globals_size - idx : 0;
    }

    for (uint16_t s = 0; s < cnt; s++) {
      int32_t val = (src && s < src_max) ? src[s] : 0;
      buf[pos++] = val & 0xFF;
      buf[pos++] = (val >> 8) & 0xFF;
      buf[pos++] = (val >> 16) & 0xFF;
      buf[pos++] = (val >> 24) & 0xFF;
    }
  }

  File f = ufsp->open(vm->persist_file, "w");
  if (f) {
    f.write(buf, total);
    f.close();
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: persist saved %d entries to %s (%d bytes)"),
           vm->persist_count, vm->persist_file, total);
  }
  free(buf);
#endif
}

static void tc_persist_load(TcVM *vm) {
  if (vm->persist_count == 0 || vm->persist_file[0] == '\0') return;
#ifdef USE_UFILESYS
  File f;
  if (ufsp) f = ufsp->open(vm->persist_file, "r");
  if (!f && ffsp && ffsp != ufsp) f = ffsp->open(vm->persist_file, "r");
  if (!f) return;

  uint16_t fsize = f.size();
  if (fsize < 3) { f.close(); return; }

  uint8_t *buf = (uint8_t *)malloc(fsize);
  if (!buf) { f.close(); return; }
  f.read(buf, fsize);
  f.close();

  if (buf[0] != 'P' || buf[1] != 'V') { free(buf); return; }
  uint8_t count = buf[2];
  uint16_t pos = 3;

  for (uint8_t i = 0; i < count && pos < fsize; i++) {
    if (pos + 4 > fsize) break;
    uint16_t idx = buf[pos] | (buf[pos + 1] << 8);
    pos += 2;
    uint16_t slotCount = buf[pos] | (buf[pos + 1] << 8);
    pos += 2;
    // Find matching persist entry
    for (uint8_t j = 0; j < vm->persist_count; j++) {
      if (vm->persist[j].index == idx) {
        uint16_t slots = (slotCount < vm->persist[j].count) ? slotCount : vm->persist[j].count;

        int32_t *dst = nullptr;
        uint16_t dst_max = 0;
        if (idx & 0x8000) {
          // Heap persist: index = 0x8000 | heapHandle
          uint16_t handle = idx & 0x7FFF;
          if (handle < TC_MAX_HEAP_HANDLES && vm->heap_data && vm->heap_handles &&
              vm->heap_handles[handle].alive) {
            dst = &vm->heap_data[vm->heap_handles[handle].offset];
            dst_max = vm->heap_handles[handle].size;
          }
        } else {
          // Global persist
          dst = &vm->globals[idx];
          dst_max = (idx < vm->globals_size) ? vm->globals_size - idx : 0;
        }

        for (uint16_t s = 0; s < slots && pos + 4 <= fsize; s++) {
          int32_t val = buf[pos] | (buf[pos + 1] << 8) |
                       (buf[pos + 2] << 16) | (buf[pos + 3] << 24);
          if (dst && s < dst_max) dst[s] = val;
          pos += 4;
        }
        // Skip remaining slots if file has more than current entry
        if (slotCount > slots) pos += (slotCount - slots) * 4;
        goto next_entry;
      }
    }
    // No match — skip data
    pos += slotCount * 4;
    next_entry:;
  }

  free(buf);
  AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: persist loaded from %s"), vm->persist_file);
#endif
}

/*********************************************************************************************\
 * VM: Load binary
\*********************************************************************************************/

static int tc_vm_load(TcVM *vm, const uint8_t *binary, uint16_t size) {
  // All binary[] reads use TC_READ_BYTE() for flash-safe access
  #define B(i) TC_READ_BYTE(&binary[i])

  if (size < 14) return TC_ERR_BAD_BINARY;  // minimum header size

  uint32_t magic = ((uint32_t)B(0) << 24) | ((uint32_t)B(1) << 16) |
                   ((uint32_t)B(2) << 8) | B(3);
  if (magic != TC_MAGIC) return TC_ERR_BAD_BINARY;

  uint16_t version = (B(4) << 8) | B(5);
  if (version < 2 || version > TC_VERSION) return TC_ERR_BAD_BINARY;

  uint16_t global_size = (B(6) << 8) | B(7);
  uint16_t entry_point = (B(8) << 8) | B(9);
  uint16_t const_pool_size = (B(10) << 8) | B(11);
  uint16_t heap_decl_size = (B(12) << 8) | B(13);

  // V5=20, V4=18, V3=16, V2=14 bytes header
  uint16_t header_size = (version >= 5) ? 20 : ((version >= 4) ? 18 : ((version >= 3) ? 16 : 14));
  uint16_t func_table_size = (version >= 3 && size >= 16) ? ((B(14) << 8) | B(15)) : 0;
  uint16_t persist_table_size = (version >= 4 && size >= 18) ? ((B(16) << 8) | B(17)) : 0;
  uint16_t globals_table_size = (version >= 5 && size >= 20) ? ((B(18) << 8) | B(19)) : 0;

  if (size < header_size) return TC_ERR_BAD_BINARY;

  uint16_t const_end = header_size + const_pool_size;

  // Pre-scan constant pool to count entries and measure const_data bytes needed
  uint16_t prescan_count = 0;
  uint16_t prescan_data  = 0;
  {
    uint16_t scan = header_size;
    while (scan < const_end && prescan_count < TC_MAX_CONSTANTS) {
      uint8_t type = B(scan); scan++;
      if (type == 1) {  // string
        uint16_t len = (B(scan) << 8) | B(scan + 1);
        scan += 2 + len;
        prescan_data += len + 1;  // +1 for null terminator
      } else if (type == 2) {  // float
        scan += 4;
      }
      prescan_count++;
    }
  }

  // Close any open file handles from previous run
  tc_close_all_files();

  // Free any previously allocated dynamic memory
  tc_free_all_frames(vm);
  tc_heap_free_all(vm);
  if (vm->stack) { free(vm->stack); vm->stack = nullptr; vm->stack_size = 0; }
  if (vm->globals) { free(vm->globals); vm->globals = nullptr; vm->globals_size = 0; }
  if (vm->constants) { free(vm->constants); vm->constants = nullptr; vm->const_capacity = 0; }
  if (vm->const_data) { free(vm->const_data); vm->const_data = nullptr; vm->const_data_size = 0; }
  if (vm->udp_globals) { free(vm->udp_globals); vm->udp_globals = nullptr; vm->udp_global_count = 0; }

  // Allocate stack
  vm->stack = (int32_t *)calloc(TC_STACK_SIZE, sizeof(int32_t));
  if (!vm->stack) return TC_ERR_STACK_OVERFLOW;  // OOM
  vm->stack_size = TC_STACK_SIZE;

  // Allocate globals array based on binary header (minimum 64 slots for small programs)
  uint16_t alloc_globals = global_size < 64 ? 64 : global_size;
  vm->globals = (int32_t *)calloc(alloc_globals, sizeof(int32_t));
  if (!vm->globals) return TC_ERR_STACK_OVERFLOW;  // OOM
  vm->globals_size = alloc_globals;

  // Allocate constants array based on pre-scan (minimum 8 entries)
  uint16_t alloc_consts = prescan_count < 8 ? 8 : prescan_count;
  vm->constants = (TcConstant *)calloc(alloc_consts, sizeof(TcConstant));
  if (!vm->constants) return TC_ERR_STACK_OVERFLOW;  // OOM
  vm->const_capacity = alloc_consts;

  // Allocate const_data buffer based on pre-scan (minimum 64 bytes)
  uint16_t alloc_cdata = prescan_data < 64 ? 64 : prescan_data;
  vm->const_data = (char *)calloc(alloc_cdata, 1);
  if (!vm->const_data) return TC_ERR_STACK_OVERFLOW;  // OOM
  vm->const_data_size = alloc_cdata;

  // Parse constant pool into allocated arrays
  vm->const_count = 0;
  vm->const_data_used = 0;
  uint16_t offset = header_size;

  while (offset < const_end && vm->const_count < vm->const_capacity) {
    uint8_t type = B(offset); offset++;
    TcConstant *c = &vm->constants[vm->const_count];
    c->type = type;
    if (type == 1) {  // string
      uint16_t len = (B(offset) << 8) | B(offset + 1);
      offset += 2;
      if (vm->const_data_used + len + 1 > vm->const_data_size) break;
      c->str.ptr = &vm->const_data[vm->const_data_used];
      c->str.len = len;
      TC_MEMCPY(&vm->const_data[vm->const_data_used], &binary[offset], len);
      vm->const_data[vm->const_data_used + len] = '\0';
      vm->const_data_used += len + 1;
      offset += len;
    } else if (type == 2) {  // float
      int32_t bits = ((int32_t)B(offset) << 24) | ((int32_t)B(offset+1) << 16) |
                     ((int32_t)B(offset+2) << 8) | B(offset+3);
      c->f = i2f(bits);
      offset += 4;
    }
    vm->const_count++;
  }

  // Parse heap declarations and pre-allocate blocks
  uint16_t heap_end = const_end + heap_decl_size;
  if (heap_decl_size > 0) {
    uint8_t count = B(const_end);
    // Compute total heap needed
    uint32_t total_heap = 0;
    for (uint8_t i = 0; i < count; i++) {
      uint16_t sz = ((uint16_t)B(const_end + 1 + i * 3 + 1) << 8) |
                     B(const_end + 1 + i * 3 + 2);
      total_heap += sz;
    }
    if (total_heap > 0) {
      vm->heap_data = (int32_t *)special_calloc(total_heap, sizeof(int32_t));
      if (!vm->heap_data) return TC_ERR_STACK_OVERFLOW;  // OOM
      vm->heap_capacity = total_heap;
      vm->heap_handles = (TcHeapHandle *)calloc(TC_MAX_HEAP_HANDLES, sizeof(TcHeapHandle));
      if (!vm->heap_handles) { free(vm->heap_data); vm->heap_data = nullptr; return TC_ERR_STACK_OVERFLOW; }
      // Pre-allocate each declared block
      for (uint8_t i = 0; i < count; i++) {
        uint8_t handle = B(const_end + 1 + i * 3);
        uint16_t sz = ((uint16_t)B(const_end + 1 + i * 3 + 1) << 8) |
                       B(const_end + 1 + i * 3 + 2);
        if (handle < TC_MAX_HEAP_HANDLES) {
          vm->heap_handles[handle].offset = vm->heap_used;
          vm->heap_handles[handle].size = sz;
          vm->heap_handles[handle].alive = true;
          vm->heap_used += sz;
          if ((uint8_t)(handle + 1) > vm->heap_handle_count) vm->heap_handle_count = handle + 1;
        }
      }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: heap %d handles, %d/%d slots"), count, vm->heap_used, total_heap);
    }
  }

  // Parse function table (V3)
  vm->callback_count = 0;
  uint16_t func_table_start = heap_end;
  uint16_t func_table_end = func_table_start + func_table_size;
  if (func_table_size > 0) {
    uint16_t pos = func_table_start;
    uint8_t count = B(pos); pos++;
    for (uint8_t i = 0; i < count && i < TC_MAX_CALLBACKS && pos < func_table_end; i++) {
      uint8_t name_len = B(pos); pos++;
      if (name_len >= TC_CALLBACK_NAME_MAX) name_len = TC_CALLBACK_NAME_MAX - 1;
      TC_MEMCPY(vm->callbacks[i].name, &binary[pos], name_len);
      vm->callbacks[i].name[name_len] = '\0';
      pos += name_len;  // skip full name even if truncated
      vm->callbacks[i].address = (B(pos) << 8) | B(pos + 1);
      pos += 2;
      vm->callback_count++;
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: callback '%s' @%d"), vm->callbacks[i].name, vm->callbacks[i].address);
    }
  }

  // Parse persist table (V4)
  vm->persist_count = 0;
  uint16_t persist_table_start = func_table_end;
  uint16_t persist_table_end = persist_table_start + persist_table_size;
  if (persist_table_size > 0) {
    uint16_t pos = persist_table_start;
    uint8_t count = B(pos); pos++;
    if (count > TC_MAX_PERSIST) count = TC_MAX_PERSIST;
    for (uint8_t i = 0; i < count && pos + 4 <= persist_table_end; i++) {
      vm->persist[i].index = (B(pos) << 8) | B(pos + 1);
      pos += 2;
      vm->persist[i].count = (B(pos) << 8) | B(pos + 1);
      pos += 2;
      vm->persist_count++;
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: %d persist entries"), vm->persist_count);
  }

  // Parse globals table (V5: UDP auto-update variables) — dynamically allocated
  vm->udp_global_count = 0;
  vm->udp_globals = nullptr;
  uint16_t globals_table_start = persist_table_end;
  uint16_t globals_table_end = globals_table_start + globals_table_size;
  if (globals_table_size > 0) {
    uint16_t pos = globals_table_start;
    uint8_t count = B(pos); pos++;
    if (count > TC_MAX_UDP_GLOBALS) count = TC_MAX_UDP_GLOBALS;
    if (count > 0) {
      vm->udp_globals = (struct TcVM::TcUdpGlobalEntry *)calloc(count, sizeof(struct TcVM::TcUdpGlobalEntry));
      if (!vm->udp_globals) { count = 0; }  // OOM — skip UDP globals
    }
    for (uint8_t i = 0; i < count && pos < globals_table_end; i++) {
      uint8_t name_len = B(pos); pos++;
      if (name_len >= TC_UDP_VAR_NAME_MAX) name_len = TC_UDP_VAR_NAME_MAX - 1;
      for (uint8_t j = 0; j < name_len && pos < globals_table_end; j++) {
        vm->udp_globals[i].name[j] = (char)B(pos); pos++;
      }
      vm->udp_globals[i].name[name_len] = '\0';
      if (pos + 4 <= globals_table_end) {
        vm->udp_globals[i].index = (B(pos) << 8) | B(pos + 1); pos += 2;
        vm->udp_globals[i].slot_count = (B(pos) << 8) | B(pos + 1); pos += 2;
        if (pos < globals_table_end) {
          vm->udp_globals[i].type = B(pos); pos++;
        } else {
          vm->udp_globals[i].type = 0;
        }
        vm->udp_global_count++;
      }
    }
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: %d udp global entries"), vm->udp_global_count);
  }

  #undef B

  vm->code = binary;
  vm->code_offset = globals_table_end;
  vm->code_size = size - globals_table_end;
  vm->pc = vm->code_offset + entry_point;
  vm->sp = 0;
  vm->fp = 0;
  vm->frame_count = 0;
  vm->running = false;
  vm->halted = false;
  vm->delayed = false;
  vm->delay_until = 0;
  vm->error = TC_OK;
  vm->instruction_count = 0;
  vm->ow_pin = -1;
  vm->ow_bus = nullptr;
  // globals and stack already zero from calloc — no memset needed
  vm->persist_file[0] = '\0';  // caller sets this before tc_persist_load()

  // Allocate frame 0 for main() — program starts here without OP_CALL
  if (!tc_frame_alloc(&vm->frames[0])) {
    return TC_ERR_STACK_OVERFLOW;  // OOM
  }

  return TC_OK;
}

// Forward declaration — tc_vm_step is defined below, called by tc_vm_call_callback
static int tc_vm_step(TcVM *vm);

/*********************************************************************************************\
 * VM: Callback invocation — call a named function after main() has halted
 *
 * Execution model:
 *   After main() returns, globals & heap persist. Callbacks temporarily un-halt
 *   the VM, allocate a fresh frame, execute the function synchronously, then
 *   re-halt. Instruction limit prevents runaway callbacks.
 *
 * Thread safety (ESP32):
 *   Callbacks are called from Tasmota's main loop (FUNC_JSON_APPEND,
 *   FUNC_WEB_SENSOR, FUNC_EVERY_SECOND). The FreeRTOS task has already
 *   exited (task_running=false) when main() halted, so there is no
 *   concurrent access to the VM state.
\*********************************************************************************************/

static int tc_vm_call_callback(TcVM *vm, const char *name) {
  // Find callback by name
  int idx = -1;
  for (int i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) { idx = i; break; }
  }
  if (idx < 0) return TC_OK;  // callback not defined, silently skip

  // VM must be halted (main returned) with no error
  if (!vm->halted || vm->error != TC_OK) return vm->error;

  // Save state
  uint8_t saved_frame_count = vm->frame_count;
  uint16_t saved_pc = vm->pc;
  uint16_t saved_sp = vm->sp;

  // Temporarily un-halt and set up callback frame
  vm->halted = false;
  vm->running = true;
  if (vm->frame_count >= TC_MAX_FRAMES) return TC_ERR_FRAME_OVERFLOW;
  TcFrame *frame = &vm->frames[vm->frame_count];
  frame->return_pc = 0;  // detect return by frame_count drop
  if (!tc_frame_alloc(frame)) {
    vm->halted = true;
    vm->running = false;
    return TC_ERR_STACK_OVERFLOW;
  }
  vm->fp = vm->frame_count;
  vm->frame_count++;
  vm->pc = vm->code_offset + vm->callbacks[idx].address;

  // Execute with instruction limit
  uint32_t count = 0;
  while (vm->frame_count > saved_frame_count && !vm->halted && vm->error == TC_OK) {
    int err = tc_vm_step(vm);
    if (err == TC_ERR_PAUSED) {
      // delay() in callback — execute synchronously (short delays only)
      if (vm->delayed && vm->delay_until > millis()) {
        uint32_t wait = vm->delay_until - millis();
        if (wait > 100) wait = 100;  // cap at 100ms to avoid WDT
        delay(wait);
      }
      vm->delayed = false;
      continue;  // resume callback execution after delay
    }
    if (err != TC_OK) {
      vm->error = err;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Callback error %d at PC=%u"), err, vm->pc);
      break;
    }
    vm->instruction_count++;
    if (++count > TC_CALLBACK_MAX_INSTR) {
      vm->error = TC_ERR_INSTRUCTION_LIMIT;
      break;
    }
  }

  // Restore halted state (globals & heap persist)
  vm->halted = true;
  vm->running = false;
  vm->pc = saved_pc;

  // Clean up any leftover frames from the callback
  while (vm->frame_count > saved_frame_count) {
    tc_frame_free(&vm->frames[--vm->frame_count]);
  }
  vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;

  // Flush output to Tasmota
  tc_output_flush();

  return vm->error;
}

// Call a callback with a C string argument (e.g. Event(char json[]))
// Allocates a temp heap buffer, copies the string, pushes ref, calls callback, frees buffer.
static int tc_vm_call_callback_str(TcVM *vm, const char *name, const char *str) {
  // Check callback exists
  int idx = -1;
  for (int i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) { idx = i; break; }
  }
  if (idx < 0) return TC_OK;  // not defined, skip
  if (!vm->halted || vm->error != TC_OK) return vm->error;

  int32_t slen = str ? strlen(str) : 0;
  int32_t slots = slen + 1;  // include null terminator
  if (slots > 512) slots = 512;  // cap at 512 chars

  // Allocate temporary heap buffer
  int handle = tc_heap_alloc(vm, slots);
  if (handle < 0) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Event callback — heap alloc failed (%d slots)"), slots);
    return TC_OK;  // non-fatal, skip callback
  }

  // Copy string into heap buffer (one char per int32_t slot)
  int32_t *buf = &vm->heap_data[vm->heap_handles[handle].offset];
  for (int32_t i = 0; i < slots - 1 && i < slen; i++) {
    buf[i] = (int32_t)(uint8_t)str[i];
  }
  buf[slots - 1] = 0;

  // Push heap ref onto stack for the callback parameter
  int32_t ref = 0xC0000000 | handle;
  TC_PUSH(vm, ref);

  // Call the callback (it will pop the ref as its parameter)
  int err = tc_vm_call_callback(vm, name);

  // Free temp buffer
  tc_heap_free_handle(vm, handle);

  return err;
}

/*********************************************************************************************\
 * VM: Execute single instruction
\*********************************************************************************************/

static int tc_vm_step(TcVM *vm) {
  if (vm->halted || vm->error != TC_OK) return vm->error;

  // Bounds check PC before reading next instruction
  if (vm->pc < vm->code_offset || vm->pc >= vm->code_offset + vm->code_size) {
    vm->error = TC_ERR_BOUNDS;
    return TC_ERR_BOUNDS;
  }

  uint8_t op = tc_read_u8(vm);
  int32_t a, b;
  float fa, fb;
  uint16_t addr;
  uint8_t idx;

  switch (op) {
    case OP_NOP: break;
    case OP_HALT: vm->halted = true; vm->running = false; break;

    // ── Stack ──────────────────────────────
    case OP_PUSH_I32: TC_PUSH(vm, tc_read_i32(vm)); break;
    case OP_PUSH_F32: TC_PUSHF(vm, tc_read_f32(vm)); break;
    case OP_PUSH_I8:  TC_PUSH(vm, (int32_t)tc_read_i8(vm)); break;
    case OP_PUSH_I16: { int16_t sv = (int16_t)tc_read_u16(vm); TC_PUSH(vm, (int32_t)sv); break; }
    case OP_POP:  TC_POP(vm); break;
    case OP_DUP:  a = TC_PEEK(vm); TC_PUSH(vm, a); break;

    // ── Integer arithmetic ─────────────────
    case OP_ADD: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a+b); break;
    case OP_SUB: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a-b); break;
    case OP_MUL: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a*b); break;
    case OP_DIV: b=TC_POP(vm); a=TC_POP(vm); if(!b) return TC_ERR_DIV_ZERO; TC_PUSH(vm, a/b); break;
    case OP_MOD: b=TC_POP(vm); a=TC_POP(vm); if(!b) return TC_ERR_DIV_ZERO; TC_PUSH(vm, a%b); break;
    case OP_NEG: a=TC_POP(vm); TC_PUSH(vm, -a); break;

    // ── Float arithmetic ───────────────────
    case OP_FADD: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSHF(vm, fa+fb); break;
    case OP_FSUB: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSHF(vm, fa-fb); break;
    case OP_FMUL: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSHF(vm, fa*fb); break;
    case OP_FDIV: fb=TC_POPF(vm); fa=TC_POPF(vm); if(fb==0.0f) return TC_ERR_DIV_ZERO; TC_PUSHF(vm, fa/fb); break;
    case OP_FNEG: fa=TC_POPF(vm); TC_PUSHF(vm, -fa); break;

    // ── Bitwise ────────────────────────────
    case OP_BIT_AND: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a&b); break;
    case OP_BIT_OR:  b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a|b); break;
    case OP_BIT_XOR: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a^b); break;
    case OP_BIT_NOT: a=TC_POP(vm); TC_PUSH(vm, ~a); break;
    case OP_SHL: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a<<b); break;
    case OP_SHR: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a>>b); break;

    // ── Integer comparison ─────────────────
    case OP_EQ:  b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a==b?1:0); break;
    case OP_NEQ: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a!=b?1:0); break;
    case OP_LT:  b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a<b?1:0); break;
    case OP_GT:  b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a>b?1:0); break;
    case OP_LTE: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a<=b?1:0); break;
    case OP_GTE: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, a>=b?1:0); break;

    // ── Float comparison ───────────────────
    case OP_FEQ:  fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa==fb?1:0); break;
    case OP_FNEQ: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa!=fb?1:0); break;
    case OP_FLT:  fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa<fb?1:0); break;
    case OP_FGT:  fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa>fb?1:0); break;
    case OP_FLTE: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa<=fb?1:0); break;
    case OP_FGTE: fb=TC_POPF(vm); fa=TC_POPF(vm); TC_PUSH(vm, fa>=fb?1:0); break;

    // ── Logical ────────────────────────────
    case OP_LOGIC_AND: b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, (a&&b)?1:0); break;
    case OP_LOGIC_OR:  b=TC_POP(vm); a=TC_POP(vm); TC_PUSH(vm, (a||b)?1:0); break;
    case OP_LOGIC_NOT: a=TC_POP(vm); TC_PUSH(vm, a?0:1); break;

    // ── Control flow ───────────────────────
    case OP_JMP:
      addr = tc_read_u16(vm);
      vm->pc = vm->code_offset + addr;
      break;
    case OP_JZ:
      addr = tc_read_u16(vm);
      a = TC_POP(vm);
      if (a == 0) vm->pc = vm->code_offset + addr;
      break;
    case OP_JNZ:
      addr = tc_read_u16(vm);
      a = TC_POP(vm);
      if (a != 0) vm->pc = vm->code_offset + addr;
      break;

    case OP_CALL:
      addr = tc_read_u16(vm);
      if (vm->frame_count >= TC_MAX_FRAMES) return TC_ERR_FRAME_OVERFLOW;
      {
        TcFrame *frame = &vm->frames[vm->frame_count];
        frame->return_pc = vm->pc;
        if (!tc_frame_alloc(frame)) {
          return TC_ERR_STACK_OVERFLOW;  // OOM
        }
        vm->fp = vm->frame_count;
        vm->frame_count++;
        vm->pc = vm->code_offset + addr;
      }
      break;

    case OP_RET:
      if (vm->frame_count == 0) { tc_frame_free(&vm->frames[0]); vm->halted = true; vm->running = false; break; }
      { TcFrame *f = &vm->frames[--vm->frame_count];
        vm->pc = f->return_pc;
        tc_frame_free(f);  // free returning frame's locals
        vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0; }
      break;

    case OP_RET_VAL:
      a = TC_POP(vm);
      if (vm->frame_count == 0) { tc_frame_free(&vm->frames[0]); TC_PUSH(vm, a); vm->halted = true; vm->running = false; break; }
      { TcFrame *f = &vm->frames[--vm->frame_count];
        vm->pc = f->return_pc;
        tc_frame_free(f);  // free returning frame's locals
        vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;
        TC_PUSH(vm, a); }
      break;

    // ── Variables (with bounds checks) ─────
    case OP_LOAD_LOCAL:
      idx=tc_read_u8(vm);
      if (idx >= TC_MAX_LOCALS) return TC_ERR_BOUNDS;
      TC_PUSH(vm, vm->frames[vm->fp].locals[idx]); break;
    case OP_STORE_LOCAL:
      idx=tc_read_u8(vm);
      if (idx >= TC_MAX_LOCALS) { TC_POP(vm); return TC_ERR_BOUNDS; }
      vm->frames[vm->fp].locals[idx]=TC_POP(vm); break;
    case OP_LOAD_GLOBAL:
      addr=tc_read_u16(vm);
      if (addr >= vm->globals_size) return TC_ERR_BOUNDS;
      TC_PUSH(vm, vm->globals[addr]); break;
    case OP_STORE_GLOBAL:
      addr=tc_read_u16(vm);
      if (addr >= vm->globals_size) { TC_POP(vm); return TC_ERR_BOUNDS; }
      vm->globals[addr]=TC_POP(vm); break;
    case OP_STORE_GLOBAL_UDP: {
      uint16_t gidx = tc_read_u16(vm);
      uint16_t name_idx = tc_read_u16(vm);
      if (gidx >= vm->globals_size) { TC_POP(vm); return TC_ERR_BOUNDS; }
      int32_t val = TC_POP(vm);
      vm->globals[gidx] = val;
      if (Tinyc && Tinyc->udp_connected && name_idx < vm->const_count && vm->constants[name_idx].type == 1) {
        tc_udp_send(vm->constants[name_idx].str.ptr, i2f(val));
      }
      break;
    }
    case OP_STORE_WATCH: {
      uint16_t var_idx = tc_read_u16(vm);
      uint16_t shadow_idx = tc_read_u16(vm);
      uint16_t written_idx = tc_read_u16(vm);
      int32_t val = TC_POP(vm);
      if (var_idx < vm->globals_size && shadow_idx < vm->globals_size && written_idx < vm->globals_size) {
        vm->globals[shadow_idx] = vm->globals[var_idx];  // save old value
        vm->globals[written_idx] = 1;                     // set written flag
        vm->globals[var_idx] = val;                        // store new value
      }
      break;
    }

    // ── Arrays (with bounds checks) ────────
    case OP_LOAD_LOCAL_ARR:
      idx=tc_read_u8(vm); a=TC_POP(vm);
      if ((uint32_t)(idx+a) >= TC_MAX_LOCALS) return TC_ERR_BOUNDS;
      TC_PUSH(vm, vm->frames[vm->fp].locals[idx+a]); break;
    case OP_STORE_LOCAL_ARR:
      idx=tc_read_u8(vm); b=TC_POP(vm); a=TC_POP(vm);
      if ((uint32_t)(idx+a) >= TC_MAX_LOCALS) return TC_ERR_BOUNDS;
      vm->frames[vm->fp].locals[idx+a]=b; break;
    case OP_LOAD_GLOBAL_ARR:
      addr=tc_read_u16(vm); a=TC_POP(vm);
      if ((uint32_t)(addr+a) >= vm->globals_size) return TC_ERR_BOUNDS;
      TC_PUSH(vm, vm->globals[addr+a]); break;
    case OP_STORE_GLOBAL_ARR:
      addr=tc_read_u16(vm); b=TC_POP(vm); a=TC_POP(vm);
      if ((uint32_t)(addr+a) >= vm->globals_size) return TC_ERR_BOUNDS;
      vm->globals[addr+a]=b; break;

    // ── Type conversion ────────────────────
    case OP_I2F: a=TC_POP(vm); TC_PUSHF(vm, (float)a); break;
    case OP_F2I: fa=TC_POPF(vm); TC_PUSH(vm, (int32_t)fa); break;
    case OP_I2C: a=TC_POP(vm); TC_PUSH(vm, a & 0xFF); break;

    // ── Array address refs (for string functions) ──
    case OP_ADDR_LOCAL:
      idx = tc_read_u8(vm);
      TC_PUSH(vm, tc_make_local_ref(vm->fp, idx));
      break;
    case OP_ADDR_GLOBAL:
      addr = tc_read_u16(vm);
      TC_PUSH(vm, tc_make_global_ref(addr));
      break;

    // ── Heap arrays ───────────────────────
    case OP_LOAD_HEAP_ARR: {
      uint8_t handle = tc_read_u8(vm);
      a = TC_POP(vm);  // index
      if (handle >= TC_MAX_HEAP_HANDLES || !vm->heap_data || !vm->heap_handles ||
          !vm->heap_handles[handle].alive ||
          a < 0 || (uint16_t)a >= vm->heap_handles[handle].size) {
        return TC_ERR_BOUNDS;
      }
      TC_PUSH(vm, vm->heap_data[vm->heap_handles[handle].offset + a]);
      break;
    }
    case OP_STORE_HEAP_ARR: {
      uint8_t handle = tc_read_u8(vm);
      b = TC_POP(vm);  // value
      a = TC_POP(vm);  // index
      if (handle >= TC_MAX_HEAP_HANDLES || !vm->heap_data || !vm->heap_handles ||
          !vm->heap_handles[handle].alive ||
          a < 0 || (uint16_t)a >= vm->heap_handles[handle].size) {
        return TC_ERR_BOUNDS;
      }
      vm->heap_data[vm->heap_handles[handle].offset + a] = b;
      break;
    }
    case OP_ADDR_HEAP: {
      uint8_t handle = tc_read_u8(vm);
      // Pack: 0xC0000000 | handle
      TC_PUSH(vm, (int32_t)(0xC0000000U | handle));
      break;
    }

    // ── Constants ──────────────────────────
    case OP_LOAD_CONST:
      addr = tc_read_u16(vm);
      if (addr < vm->const_count) {
        if (vm->constants[addr].type == 1) TC_PUSH(vm, addr);
        else TC_PUSHF(vm, vm->constants[addr].f);
      }
      break;

    // ── Syscalls ───────────────────────────
    case OP_SYSCALL:
      idx = tc_read_u8(vm);
      return tc_syscall(vm, idx);

    default:
      vm->error = TC_ERR_BAD_OPCODE;
      return TC_ERR_BAD_OPCODE;
  }
  return vm->error;
}

/*********************************************************************************************\
 * VM: Run N instructions (non-blocking slice)
 * Uses computed-goto dispatch on GCC for ~20-30% speedup over switch().
\*********************************************************************************************/

static int tc_vm_run_slice(TcVM *vm, uint32_t max_instr) {
  vm->running = true;

  // Check for non-blocking delay before entering hot loop
  if (vm->delayed) {
    if ((int32_t)(millis() - vm->delay_until) >= 0) {
      vm->delayed = false;
    } else {
      return TC_OK;
    }
  }

#if defined(__GNUC__) || defined(__clang__)
  // ════════════════════════════════════════════════════════════════════
  // Computed-goto (direct-threaded) dispatch — GCC / Clang only
  // Each opcode handler jumps directly to the next, avoiding the
  // branch-prediction penalty of a single switch indirect branch.
  // ════════════════════════════════════════════════════════════════════

  // Dispatch table (0x00 – 0xA5, 166 entries)
  // C++ doesn't support designated initializers, so build at runtime on first call
  static const void *_dispatch[0xA6] = {};
  static bool _dispatch_init = false;
  if (!_dispatch_init) {
    memset((void*)_dispatch, 0, sizeof(_dispatch));
    // Stack
    _dispatch[0x00] = &&_op_nop;   _dispatch[0x01] = &&_op_halt;
    _dispatch[0x02] = &&_op_push_i32; _dispatch[0x03] = &&_op_push_f32;
    _dispatch[0x04] = &&_op_push_i8;  _dispatch[0x05] = &&_op_push_i16;
    _dispatch[0x06] = &&_op_pop;      _dispatch[0x07] = &&_op_dup;
    // Integer arithmetic
    _dispatch[0x10] = &&_op_add;  _dispatch[0x11] = &&_op_sub;
    _dispatch[0x12] = &&_op_mul;  _dispatch[0x13] = &&_op_div;
    _dispatch[0x14] = &&_op_mod;  _dispatch[0x15] = &&_op_neg;
    // Float arithmetic
    _dispatch[0x18] = &&_op_fadd; _dispatch[0x19] = &&_op_fsub;
    _dispatch[0x1A] = &&_op_fmul; _dispatch[0x1B] = &&_op_fdiv;
    _dispatch[0x1C] = &&_op_fneg;
    // Bitwise
    _dispatch[0x20] = &&_op_band; _dispatch[0x21] = &&_op_bor;
    _dispatch[0x22] = &&_op_bxor; _dispatch[0x23] = &&_op_bnot;
    _dispatch[0x24] = &&_op_shl;  _dispatch[0x25] = &&_op_shr;
    // Integer comparison
    _dispatch[0x30] = &&_op_eq;   _dispatch[0x31] = &&_op_neq;
    _dispatch[0x32] = &&_op_lt;   _dispatch[0x33] = &&_op_gt;
    _dispatch[0x34] = &&_op_lte;  _dispatch[0x35] = &&_op_gte;
    // Float comparison
    _dispatch[0x36] = &&_op_feq;  _dispatch[0x37] = &&_op_fneq;
    _dispatch[0x38] = &&_op_flt;  _dispatch[0x39] = &&_op_fgt;
    _dispatch[0x3A] = &&_op_flte; _dispatch[0x3B] = &&_op_fgte;
    // Logical
    _dispatch[0x40] = &&_op_land; _dispatch[0x41] = &&_op_lor;
    _dispatch[0x42] = &&_op_lnot;
    // Control flow
    _dispatch[0x50] = &&_op_jmp;  _dispatch[0x51] = &&_op_jz;
    _dispatch[0x52] = &&_op_jnz;  _dispatch[0x53] = &&_op_call;
    _dispatch[0x54] = &&_op_ret;  _dispatch[0x55] = &&_op_ret_val;
    // Variables
    _dispatch[0x60] = &&_op_load_local;   _dispatch[0x61] = &&_op_store_local;
    _dispatch[0x62] = &&_op_load_global;  _dispatch[0x63] = &&_op_store_global;
    _dispatch[0x64] = &&_op_store_global_udp;
    // Arrays
    _dispatch[0x68] = &&_op_load_local_arr;  _dispatch[0x69] = &&_op_store_local_arr;
    _dispatch[0x6A] = &&_op_load_global_arr; _dispatch[0x6B] = &&_op_store_global_arr;
    // Type conversion
    _dispatch[0x70] = &&_op_i2f;  _dispatch[0x71] = &&_op_f2i;
    _dispatch[0x72] = &&_op_i2c;
    // Address refs
    _dispatch[0x78] = &&_op_addr_local; _dispatch[0x79] = &&_op_addr_global;
    // Syscall & const
    _dispatch[0x80] = &&_op_syscall; _dispatch[0x90] = &&_op_load_const;
    // Heap
    _dispatch[0xA0] = &&_op_load_heap;  _dispatch[0xA1] = &&_op_store_heap;
    _dispatch[0xA2] = &&_op_addr_heap;
    // Watch
    _dispatch[0xA5] = &&_op_store_watch;
    _dispatch_init = true;
  }

  // ── Prologue: load hot VM state into locals ──
  int32_t *_stack  = vm->stack;
  uint16_t _stack_size = vm->stack_size;
  const uint8_t *_code = vm->code;
  uint16_t _sp     = vm->sp;
  uint16_t _pc     = vm->pc;
  uint16_t _coff   = vm->code_offset;
  uint16_t _csz    = vm->code_size;
  uint16_t _gsz    = vm->globals_size;
  int      _err    = TC_OK;
  uint32_t _count  = 0;
  uint32_t _start  = millis();
  int32_t  _a, _b;
  float    _fa, _fb;
  uint16_t _addr;
  uint8_t  _idx;
  uint8_t  _op;

  // ── Inline read helpers using local _pc ──
  #define _RD_U8()  TC_READ_BYTE(&_code[_pc++])
  #define _RD_I8()  ((int8_t)TC_READ_BYTE(&_code[_pc++]))
  #define _RD_U16() ({ uint16_t _v = ((uint16_t)TC_READ_BYTE(&_code[_pc])<<8) | TC_READ_BYTE(&_code[_pc+1]); _pc+=2; _v; })
  #define _RD_I32() ({ int32_t _v = ((int32_t)TC_READ_BYTE(&_code[_pc])<<24) | ((int32_t)TC_READ_BYTE(&_code[_pc+1])<<16) | \
                       ((int32_t)TC_READ_BYTE(&_code[_pc+2])<<8) | TC_READ_BYTE(&_code[_pc+3]); _pc+=4; _v; })
  #define _RD_F32() i2f(_RD_I32())

  // ── Dispatch macro ──
  #define NEXT() do { \
    _count++; \
    if ((_count & 0x3F) == 0 && millis() - _start > 10) goto _vm_yield; \
    if (_pc < _coff || _pc >= _coff + _csz) { _err = TC_ERR_BOUNDS; goto _vm_exit; } \
    _op = _RD_U8(); \
    goto *(_dispatch[_op] ? _dispatch[_op] : &&_vm_bad_op); \
  } while(0)

  // ── First instruction fetch ──
  if (_pc < _coff || _pc >= _coff + _csz) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
  _op = _RD_U8();
  goto *(_dispatch[_op] ? _dispatch[_op] : &&_vm_bad_op);

  // ════ Opcode handlers ════

  // Stack
  _op_nop:   NEXT();
  _op_halt:  vm->halted = true; vm->running = false; goto _vm_exit;
  _op_push_i32: TC_IPUSH(_RD_I32()); NEXT();
  _op_push_f32: TC_IPUSHF(_RD_F32()); NEXT();
  _op_push_i8:  TC_IPUSH((int32_t)_RD_I8()); NEXT();
  _op_push_i16: { int16_t sv = (int16_t)_RD_U16(); TC_IPUSH((int32_t)sv); NEXT(); }
  _op_pop:   _sp--; NEXT();
  _op_dup:   _a = TC_IPEEK(); TC_IPUSH(_a); NEXT();

  // Integer arithmetic
  _op_add: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a+_b); NEXT();
  _op_sub: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a-_b); NEXT();
  _op_mul: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a*_b); NEXT();
  _op_div: _b=TC_IPOP(); _a=TC_IPOP(); if(!_b){_err=TC_ERR_DIV_ZERO;goto _vm_exit;} TC_IPUSH(_a/_b); NEXT();
  _op_mod: _b=TC_IPOP(); _a=TC_IPOP(); if(!_b){_err=TC_ERR_DIV_ZERO;goto _vm_exit;} TC_IPUSH(_a%_b); NEXT();
  _op_neg: _a=TC_IPOP(); TC_IPUSH(-_a); NEXT();

  // Float arithmetic
  _op_fadd: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSHF(_fa+_fb); NEXT();
  _op_fsub: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSHF(_fa-_fb); NEXT();
  _op_fmul: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSHF(_fa*_fb); NEXT();
  _op_fdiv: _fb=TC_IPOPF(); _fa=TC_IPOPF(); if(_fb==0.0f){_err=TC_ERR_DIV_ZERO;goto _vm_exit;} TC_IPUSHF(_fa/_fb); NEXT();
  _op_fneg: _fa=TC_IPOPF(); TC_IPUSHF(-_fa); NEXT();

  // Bitwise
  _op_band: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a&_b); NEXT();
  _op_bor:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a|_b); NEXT();
  _op_bxor: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a^_b); NEXT();
  _op_bnot: _a=TC_IPOP(); TC_IPUSH(~_a); NEXT();
  _op_shl:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a<<_b); NEXT();
  _op_shr:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a>>_b); NEXT();

  // Integer comparison
  _op_eq:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a==_b?1:0); NEXT();
  _op_neq: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a!=_b?1:0); NEXT();
  _op_lt:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a<_b?1:0); NEXT();
  _op_gt:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a>_b?1:0); NEXT();
  _op_lte: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a<=_b?1:0); NEXT();
  _op_gte: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH(_a>=_b?1:0); NEXT();

  // Float comparison
  _op_feq:  _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa==_fb?1:0); NEXT();
  _op_fneq: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa!=_fb?1:0); NEXT();
  _op_flt:  _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa<_fb?1:0); NEXT();
  _op_fgt:  _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa>_fb?1:0); NEXT();
  _op_flte: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa<=_fb?1:0); NEXT();
  _op_fgte: _fb=TC_IPOPF(); _fa=TC_IPOPF(); TC_IPUSH(_fa>=_fb?1:0); NEXT();

  // Logical
  _op_land: _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH((_a&&_b)?1:0); NEXT();
  _op_lor:  _b=TC_IPOP(); _a=TC_IPOP(); TC_IPUSH((_a||_b)?1:0); NEXT();
  _op_lnot: _a=TC_IPOP(); TC_IPUSH(_a?0:1); NEXT();

  // Control flow
  _op_jmp:
    _addr = _RD_U16();
    _pc = _coff + _addr;
    NEXT();
  _op_jz:
    _addr = _RD_U16();
    _a = TC_IPOP();
    if (_a == 0) _pc = _coff + _addr;
    NEXT();
  _op_jnz:
    _addr = _RD_U16();
    _a = TC_IPOP();
    if (_a != 0) _pc = _coff + _addr;
    NEXT();
  _op_call:
    _addr = _RD_U16();
    if (vm->frame_count >= TC_MAX_FRAMES) { _err = TC_ERR_FRAME_OVERFLOW; goto _vm_exit; }
    {
      // Write back pc before frame setup (return_pc must be current)
      vm->sp = _sp; vm->pc = _pc;
      TcFrame *frame = &vm->frames[vm->frame_count];
      frame->return_pc = _pc;
      if (!tc_frame_alloc(frame)) { _err = TC_ERR_STACK_OVERFLOW; goto _vm_exit; }
      vm->fp = vm->frame_count;
      vm->frame_count++;
      _pc = _coff + _addr;
    }
    NEXT();
  _op_ret:
    if (vm->frame_count == 0) {
      tc_frame_free(&vm->frames[0]);
      vm->halted = true; vm->running = false;
      goto _vm_exit;
    }
    {
      TcFrame *f = &vm->frames[--vm->frame_count];
      _pc = f->return_pc;
      tc_frame_free(f);
      vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;
    }
    NEXT();
  _op_ret_val:
    _a = TC_IPOP();
    if (vm->frame_count == 0) {
      tc_frame_free(&vm->frames[0]);
      TC_IPUSH(_a);
      vm->halted = true; vm->running = false;
      goto _vm_exit;
    }
    {
      TcFrame *f = &vm->frames[--vm->frame_count];
      _pc = f->return_pc;
      tc_frame_free(f);
      vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;
      TC_IPUSH(_a);
    }
    NEXT();

  // Variables
  _op_load_local:
    _idx = _RD_U8();
    if (_idx >= TC_MAX_LOCALS) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    TC_IPUSH(vm->frames[vm->fp].locals[_idx]);
    NEXT();
  _op_store_local:
    _idx = _RD_U8();
    if (_idx >= TC_MAX_LOCALS) { _sp--; _err = TC_ERR_BOUNDS; goto _vm_exit; }
    vm->frames[vm->fp].locals[_idx] = TC_IPOP();
    NEXT();
  _op_load_global:
    _addr = _RD_U16();
    if (_addr >= _gsz) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    TC_IPUSH(vm->globals[_addr]);
    NEXT();
  _op_store_global:
    _addr = _RD_U16();
    if (_addr >= _gsz) { _sp--; _err = TC_ERR_BOUNDS; goto _vm_exit; }
    vm->globals[_addr] = TC_IPOP();
    NEXT();
  _op_store_global_udp: {
    uint16_t _gidx = _RD_U16();
    uint16_t _nidx = _RD_U16();
    if (_gidx >= _gsz) { _sp--; _err = TC_ERR_BOUNDS; goto _vm_exit; }
    int32_t _val = TC_IPOP();
    vm->globals[_gidx] = _val;
    if (Tinyc && Tinyc->udp_connected && _nidx < vm->const_count && vm->constants[_nidx].type == 1) {
      tc_udp_send(vm->constants[_nidx].str.ptr, i2f(_val));
    }
    NEXT();
  }

  // Store watch
  _op_store_watch: {
    uint16_t var_idx = _RD_U16();
    uint16_t shadow_idx = _RD_U16();
    uint16_t written_idx = _RD_U16();
    int32_t val = TC_IPOP();
    if (var_idx < _gsz && shadow_idx < _gsz && written_idx < _gsz) {
      vm->globals[shadow_idx] = vm->globals[var_idx];
      vm->globals[written_idx] = 1;
      vm->globals[var_idx] = val;
    }
    NEXT();
  }

  // Arrays
  _op_load_local_arr:
    _idx = _RD_U8(); _a = TC_IPOP();
    if ((uint32_t)(_idx+_a) >= TC_MAX_LOCALS) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    TC_IPUSH(vm->frames[vm->fp].locals[_idx+_a]);
    NEXT();
  _op_store_local_arr:
    _idx = _RD_U8(); _b = TC_IPOP(); _a = TC_IPOP();
    if ((uint32_t)(_idx+_a) >= TC_MAX_LOCALS) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    vm->frames[vm->fp].locals[_idx+_a] = _b;
    NEXT();
  _op_load_global_arr:
    _addr = _RD_U16(); _a = TC_IPOP();
    if ((uint32_t)(_addr+_a) >= _gsz) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    TC_IPUSH(vm->globals[_addr+_a]);
    NEXT();
  _op_store_global_arr:
    _addr = _RD_U16(); _b = TC_IPOP(); _a = TC_IPOP();
    if ((uint32_t)(_addr+_a) >= _gsz) { _err = TC_ERR_BOUNDS; goto _vm_exit; }
    vm->globals[_addr+_a] = _b;
    NEXT();

  // Type conversion
  _op_i2f: _a = TC_IPOP(); TC_IPUSHF((float)_a); NEXT();
  _op_f2i: _fa = TC_IPOPF(); TC_IPUSH((int32_t)_fa); NEXT();
  _op_i2c: _a = TC_IPOP(); TC_IPUSH(_a & 0xFF); NEXT();

  // Address refs
  _op_addr_local:
    _idx = _RD_U8();
    TC_IPUSH(tc_make_local_ref(vm->fp, _idx));
    NEXT();
  _op_addr_global:
    _addr = _RD_U16();
    TC_IPUSH(tc_make_global_ref(_addr));
    NEXT();

  // Constants
  _op_load_const:
    _addr = _RD_U16();
    if (_addr < vm->const_count) {
      if (vm->constants[_addr].type == 1) TC_IPUSH(_addr);
      else TC_IPUSHF(vm->constants[_addr].f);
    }
    NEXT();

  // Heap arrays
  _op_load_heap: {
    uint8_t handle = _RD_U8();
    _a = TC_IPOP();
    if (handle >= TC_MAX_HEAP_HANDLES || !vm->heap_data || !vm->heap_handles ||
        !vm->heap_handles[handle].alive ||
        _a < 0 || (uint16_t)_a >= vm->heap_handles[handle].size) {
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    TC_IPUSH(vm->heap_data[vm->heap_handles[handle].offset + _a]);
    NEXT();
  }
  _op_store_heap: {
    uint8_t handle = _RD_U8();
    _b = TC_IPOP(); _a = TC_IPOP();
    if (handle >= TC_MAX_HEAP_HANDLES || !vm->heap_data || !vm->heap_handles ||
        !vm->heap_handles[handle].alive ||
        _a < 0 || (uint16_t)_a >= vm->heap_handles[handle].size) {
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    vm->heap_data[vm->heap_handles[handle].offset + _a] = _b;
    NEXT();
  }
  _op_addr_heap: {
    uint8_t handle = _RD_U8();
    TC_IPUSH((int32_t)(0xC0000000U | handle));
    NEXT();
  }

  // Syscall — sync locals ↔ vm, call handler, reload
  _op_syscall: {
    _idx = _RD_U8();
    vm->sp = _sp; vm->pc = _pc;  // write back
    int scerr = tc_syscall(vm, _idx);
    _sp = vm->sp; _pc = vm->pc;  // reload (syscall may change sp/pc)
    if (scerr == TC_ERR_PAUSED) goto _vm_yield;
    if (scerr != TC_OK) { _err = scerr; goto _vm_exit; }
    NEXT();
  }

  // ── Exit labels ──
  _vm_bad_op:
    _err = TC_ERR_BAD_OPCODE;
    goto _vm_exit;
  _vm_yield:
    vm->sp = _sp; vm->pc = _pc;
    vm->instruction_count += _count;
    return TC_OK;
  _vm_exit:
    vm->sp = _sp; vm->pc = _pc;
    vm->instruction_count += _count;
    if (_err != TC_OK) vm->error = _err;
    return vm->error;

  // Clean up inner-loop macros
  #undef _RD_U8
  #undef _RD_I8
  #undef _RD_U16
  #undef _RD_I32
  #undef _RD_F32
  #undef NEXT

#else
  // ════════════════════════════════════════════════════════════════════
  // Fallback: original switch-based dispatch for non-GCC compilers
  // ════════════════════════════════════════════════════════════════════
  uint32_t count = 0;
  uint32_t start_ms = millis();

  while (vm->running && !vm->halted && vm->error == TC_OK && count < max_instr) {
    int err = tc_vm_step(vm);
    if (err == TC_ERR_PAUSED) return TC_OK;
    if (err != TC_OK) return err;
    count++;
    vm->instruction_count++;
    if ((count & 0x3F) == 0) {
      if (millis() - start_ms > 10) return TC_OK;
    }
  }
  return vm->error;
#endif // __GNUC__
}

/*********************************************************************************************\
 * VM: FreeRTOS task for ESP32 (runs VM in its own task with real blocking delay)
\*********************************************************************************************/

#ifdef ESP32
static void tc_vm_task(void *param) {
  TcSlot *slot = (TcSlot *)param;
  TcVM *vm = &slot->vm;
  slot->task_running = true;
  vm->running = true;
  tc_current_slot = slot;  // set for output functions

  AddLog(LOG_LEVEL_INFO, PSTR("TCC: VM task started (%s)"), slot->filename);

  // Phase 1: Execute main()
  while (!slot->task_stop && !vm->halted && vm->error == TC_OK) {
    // Handle delay as real RTOS blocking delay (feeds WDT, yields CPU)
    if (vm->delayed) {
      int32_t remaining = (int32_t)(vm->delay_until - millis());
      if (remaining > 0) {
        while (remaining > 0 && !slot->task_stop) {
          int32_t chunk = (remaining > 50) ? 50 : remaining;
          vTaskDelay(chunk / portTICK_PERIOD_MS);
          remaining = (int32_t)(vm->delay_until - millis());
        }
      }
      vm->delayed = false;
      tc_current_slot = slot;  // restore — other tasks may have changed it during delay
      if (slot->task_stop) break;
    }

    // Execute a batch of instructions, then yield
    uint32_t count = 0;
    while (!vm->halted && vm->error == TC_OK && count < 256 && !slot->task_stop) {
      int err = tc_vm_step(vm);
      if (err == TC_ERR_PAUSED) break;
      if (err != TC_OK) {
        vm->error = err;
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Runtime error %d at PC=%u after %u instr"),
          err, vm->pc, vm->instruction_count);
        break;
      }
      count++;
      vm->instruction_count++;
    }

    yield();
  }

  // Cleanup after main() exits
  tc_free_all_frames(vm);
  tc_close_all_files();
  tc_output_flush();

  if (vm->halted && vm->error == TC_OK) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Halted after %u instr, %d callbacks"),
      vm->instruction_count, vm->callback_count);

    // Phase 2: If TaskLoop callback exists, loop calling it in this task
    int tl_idx = -1;
    for (int i = 0; i < vm->callback_count; i++) {
      if (strcmp(vm->callbacks[i].name, "TaskLoop") == 0) { tl_idx = i; break; }
    }
    if (tl_idx >= 0) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: TaskLoop running in task"));
      uint16_t tl_addr = vm->callbacks[tl_idx].address;

      while (!slot->task_stop && vm->error == TC_OK) {
        if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
        tc_current_slot = slot;  // restore after mutex acquire

        uint8_t saved_frame_count = vm->frame_count;
        uint16_t saved_pc = vm->pc;
        vm->halted = false;
        vm->running = true;
        if (vm->frame_count < TC_MAX_FRAMES) {
          TcFrame *frame = &vm->frames[vm->frame_count];
          frame->return_pc = 0;
          if (tc_frame_alloc(frame)) {
            vm->fp = vm->frame_count;
            vm->frame_count++;
            vm->pc = vm->code_offset + tl_addr;

            uint32_t count = 0;
            while (vm->frame_count > saved_frame_count && !vm->halted && vm->error == TC_OK && !slot->task_stop) {
              int err = tc_vm_step(vm);
              if (err == TC_ERR_PAUSED) {
                if (vm->delayed) {
                  vm->halted = true;
                  vm->running = false;
                  tc_current_slot = nullptr;  // clear during delay
                  if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
                  int32_t remaining = (int32_t)(vm->delay_until - millis());
                  while (remaining > 0 && !slot->task_stop) {
                    int32_t chunk = (remaining > 50) ? 50 : remaining;
                    vTaskDelay(chunk / portTICK_PERIOD_MS);
                    remaining = (int32_t)(vm->delay_until - millis());
                  }
                  vm->delayed = false;
                  if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
                  tc_current_slot = slot;  // restore after reacquire
                  vm->halted = false;
                  vm->running = true;
                  if (slot->task_stop) break;
                }
                continue;
              }
              if (err != TC_OK) {
                vm->error = err;
                AddLog(LOG_LEVEL_ERROR, PSTR("TCC: TaskLoop error %d at PC=%u"), err, vm->pc);
                break;
              }
              if (++count > TC_CALLBACK_MAX_INSTR) {
                vm->error = TC_ERR_INSTRUCTION_LIMIT;
                break;
              }
            }

            while (vm->frame_count > saved_frame_count) {
              tc_frame_free(&vm->frames[--vm->frame_count]);
            }
            vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;
          }
        }

        vm->halted = true;
        vm->running = false;
        vm->pc = saved_pc;
        tc_output_flush();

        tc_current_slot = nullptr;
        if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);

        if (vm->error != TC_OK || slot->task_stop) break;

        vTaskDelay(1);
      }
      if (slot->task_stop) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: TaskLoop stopped"));
      } else if (vm->error != TC_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: TaskLoop error: %s"), tc_error_str(vm->error));
      }
    }
  } else if (vm->error != TC_OK) {
    tc_heap_free_all(vm);
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Error: %s (PC=%d)"),
      tc_error_str(vm->error), vm->pc - vm->code_offset);
  } else if (slot->task_stop) {
    tc_heap_free_all(vm);
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Task stopped"));
  }

  slot->running = false;
  vm->running = false;
  slot->task_running = false;
  slot->task_handle = nullptr;
  tc_current_slot = nullptr;
  vTaskDelete(NULL);
}
#endif  // ESP32

/*********************************************************************************************\
 * Slot-level helpers (must be in .h to avoid Arduino IDE auto-prototype issues with TcSlot*)
\*********************************************************************************************/

// Call a named callback on a single slot, with mutex protection and tc_current_slot set
static void tc_slot_callback(TcSlot *s, const char *name) {
  if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) return;
  tc_current_slot = s;
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
  tc_vm_call_callback(&s->vm, name);
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
  tc_current_slot = nullptr;
}

// Helper: derive .pvs persist filename from .tcb filename
// e.g. "/ecotracker.tcb" -> "/ecotracker.pvs", "/autoexec.tcb" -> "/autoexec.pvs"
static void TinyCSetPersistFile(TcSlot *s, const char *tcb_path) {
  if (!s) return;
  char *pf = s->vm.persist_file;
  const size_t pfsz = sizeof(s->vm.persist_file);
  if (!tcb_path || tcb_path[0] == '\0') {
    strlcpy(pf, "/autoexec.pvs", pfsz);
    return;
  }
  strlcpy(pf, tcb_path, pfsz);
  // Replace extension: find last '.' and replace with .pvs
  char *dot = strrchr(pf, '.');
  if (dot && (dot - pf) < (int)(pfsz - 4)) {
    strcpy(dot, ".pvs");
  } else {
    strlcat(pf, ".pvs", pfsz);
  }
}

// Helper: stop the VM in a specific slot
static void TinyCStopVM(TcSlot *s) {
  if (!Tinyc || !s) return;

#ifdef ESP32
  if (s->task_handle) {
    // Signal task to stop via both flags -- vm.error causes tc_vm_step to exit
    s->task_stop = true;
    s->vm.error = TC_ERR_INSTRUCTION_LIMIT;  // causes immediate exit from step loop
    // Wait for task to exit (max 2s)
    for (int i = 0; i < 200 && s->task_running; i++) {
      delay(10);
    }
    if (s->task_running) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Task did not stop in time (%s), abandoned"), s->filename);
      s->task_running = false;
      s->task_handle = nullptr;
    } else {
      s->task_handle = nullptr;
    }
  }
#endif

  // Call OnExit() callback BEFORE teardown — lets driver release resources (I2C, etc.)
  s->vm.error = TC_OK;
  s->vm.running = false;
  s->vm.halted = true;
  tc_current_slot = s;
  tc_vm_call_callback(&s->vm, "OnExit");
  tc_output_flush();
  tc_current_slot = nullptr;

  // Auto-save persist variables before clearing VM
  tc_persist_save(&s->vm);

  s->running = false;
  s->vm.running = false;
  tc_free_all_frames(&s->vm);
  tc_heap_free_all(&s->vm);
  // Only stop UDP if no other slot still needs it
  {
    bool udp_still_needed = false;
    for (int i = 0; i < TC_MAX_VMS; i++) {
      if (Tinyc->slots[i] && Tinyc->slots[i] != s && Tinyc->slots[i]->loaded && Tinyc->slots[i]->vm.udp_global_count > 0) {
        udp_still_needed = true;
        break;
      }
    }
    if (!udp_still_needed) {
      tc_udp_stop();
    }
  }
  tc_spi_cleanup();
  tc_serial_close();
  // Free OneWire bus
  if (s->vm.ow_bus) { delete s->vm.ow_bus; s->vm.ow_bus = nullptr; s->vm.ow_pin = -1; }
  // Free HTTP header arrays
  if (Tinyc) {
    if (Tinyc->http_hdr_name) { free(Tinyc->http_hdr_name); Tinyc->http_hdr_name = nullptr; }
    if (Tinyc->http_hdr_value) { free(Tinyc->http_hdr_value); Tinyc->http_hdr_value = nullptr; }
    Tinyc->http_hdr_count = 0;
  }
  // Clear registered console buttons and command prefix
  if (Tinyc) Tinyc->console_btn_count = 0;
  s->cmd_prefix[0] = '\0';

  // Flush output for this slot
  tc_current_slot = s;
  tc_output_flush();
  tc_current_slot = nullptr;

#ifdef ESP32
  if (s->vm_mutex) {
    vSemaphoreDelete(s->vm_mutex);
    s->vm_mutex = nullptr;
  }
#endif
}

// Helper: start the VM in a specific slot
static bool TinyCStartVM(TcSlot *s) {
  if (!Tinyc || !s || !s->loaded) return false;

  // Reset VM
  int err = tc_vm_load(&s->vm, s->program, s->program_size);
  if (err != TC_OK) return false;

  // Set persist filename and load saved values
  TinyCSetPersistFile(s, s->filename);
  tc_persist_load(&s->vm);

  // Register UDP global variables (V5: auto-update from packets)
  if (s->vm.udp_global_count > 0) {
    if (!Tinyc->udp_used) {
      Tinyc->udp_used = true;
      tc_udp_init();
    }
    for (uint8_t i = 0; i < s->vm.udp_global_count; i++) {
      tc_udp_find_var(s->vm.udp_globals[i].name, true);
    }
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: registered %d UDP global vars"), s->vm.udp_global_count);
  }

  s->output_len = 0;
  s->output[0] = '\0';

#ifdef ESP32
  // Stop any existing task first
  if (s->task_handle) {
    TinyCStopVM(s);
  }

  s->task_stop = false;
  s->task_running = false;

  // Create mutex for VM access serialization (task vs main thread callbacks)
  if (!s->vm_mutex) {
    s->vm_mutex = xSemaphoreCreateMutex();
  }

  // Build task name from slot index
  char taskname[16];
  snprintf(taskname, sizeof(taskname), "tinyc_vm%d", 0);  // find slot index
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    if (Tinyc->slots[i] == s) { snprintf(taskname, sizeof(taskname), "tinyc_vm%d", i); break; }
  }

#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C2)
  // Single-core variants -- no core affinity
  BaseType_t ret = xTaskCreate(tc_vm_task, taskname, 8192, s, 1, &s->task_handle);
#else
  // Dual-core ESP32/S3 -- pin to core 1
  BaseType_t ret = xTaskCreatePinnedToCore(tc_vm_task, taskname, 8192, s, 1, &s->task_handle, 1);
#endif
  if (ret != pdPASS) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Failed to create task for %s"), s->filename);
    s->running = false;
    return false;
  }
#endif  // ESP32

  s->running = true;
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: Program started (%s)"), s->filename);
  return true;
}

#endif  // _XDRV_124_TINYC_VM_H_
