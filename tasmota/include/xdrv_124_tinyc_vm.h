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

#ifdef USE_SML_M
uint32_t SML_SetOptions(uint32_t in);
#endif

// Forward declarations for MQTT bridge helpers — defined in xdrv_124_tinyc.ino
// (after this header is included). Needed because tc_syscall() below calls them.
#ifdef USE_MQTT
int tc_mqtt_subscribe(const char *topic);
int tc_mqtt_unsubscribe(const char *topic);
#endif

// Forward declarations for dynamic task spawn — defined in xdrv_124_tinyc.ino
#ifdef ESP32
int tc_spawn_task_create(const char *name, uint16_t stack_kb);
int tc_spawn_task_kill(const char *name);
int tc_spawn_task_running(const char *name);
void tc_spawn_task_cleanup_slot(uint8_t slot_idx);
#endif

#include <OneWire.h>
// Without Scripter, these aren't pulled in transitively. TinyC needs them
// directly for serial port objects and deep-sleep wakeup-pin config.
// Safe to include unconditionally — both have header guards and no side effects.
#include <TasmotaSerial.h>
#if defined(ESP32) && SOC_RTCIO_INPUT_OUTPUT_SUPPORTED
#include "driver/rtc_io.h"
#endif

// Minimal I2S output — standalone, no USE_I2S_AUDIO needed
#if defined(ESP32) && ESP_IDF_VERSION_MAJOR >= 5
#include "driver/i2s_std.h"
#endif

// Crypto primitives for the AES/HMAC/SHA syscalls (360..365). mbedtls ships
// with the ESP-IDF Arduino framework; no extra deps. Used by TinyC scripts
// implementing protocols that need symmetric crypto (Local Tuya, MQTT-TLS
// fingerprinting, encrypted SML decoders, custom signed REST APIs, etc.).
#ifdef ESP32
#include <mbedtls/aes.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md.h>
#endif

#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
#include "esp_camera.h"
#include "img_converters.h"

// C-linkage wrapper so camera C library can call Tasmota's AddLog
// Only compiled when TC_CAM_DEBUG is defined (add -DTC_CAM_DEBUG to build_flags)
#ifdef TC_CAM_DEBUG
extern "C" void TcCamLog(const char* fmt, ...) {
    char buf[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    AddLog(2, PSTR("CAM: %s"), buf);
}
#endif
// PSRAM picture slots — capture copies JPEG here, camera fb returned immediately
#define TC_CAM_MAX_SLOTS 4
static struct {
  uint8_t *buf;
  uint32_t len;
  uint16_t width;
  uint16_t height;
  volatile uint8_t writing;   // 1 while memcpy in progress — skip reads
} tc_cam_slot[TC_CAM_MAX_SLOTS] = {};

// ── MJPEG streaming server (port 81) ──
#define TC_CAM_STREAM_PORT 81
#define TC_CAM_BOUNDARY "tc-cam-boundary-0123456789"
static struct {
  ESP8266WebServer *server;
  WiFiClient client;
  uint8_t stream_active;      // 0=off, 1=starting, 2=streaming
  uint8_t pending;            // 1 = init requested but WiFi not ready yet
  uint8_t *send_buf;          // copy of frame for sending (avoids torn reads)
  uint32_t send_len;
  uint32_t send_alloc;        // allocated size of send_buf
} tc_cam_stream = {};

// ── Motion detection ──
static struct {
  uint16_t interval_ms;       // 0 = disabled
  uint32_t last_time;
  uint32_t motion_trigger;    // accumulated pixel diff per 100 pixels
  uint32_t motion_brightness; // average brightness per 100 pixels
  uint8_t *ref_buf;           // previous frame grayscale (PSRAM)
  uint32_t ref_size;          // width*height of ref_buf
  uint8_t triggered;          // 1 if motion exceeded threshold
  uint32_t threshold;         // trigger level (0 = report only)
} tc_cam_motion = {};

// Bridge: provide WcGetPicstore() from tc_cam_slot when USE_WEBCAM is NOT available
// This allows the email driver's $N picture attachment to work with the TinyC camera
#if !defined(USE_WEBCAM)
uint32_t WcGetPicstore(int32_t num, uint8_t **buff) {
  if (num < 0) { return TC_CAM_MAX_SLOTS; }
  if (num >= TC_CAM_MAX_SLOTS) { if (buff) *buff = nullptr; return 0; }
  if (buff) *buff = tc_cam_slot[num].buf;
  return tc_cam_slot[num].len;
}
#endif  // !USE_WEBCAM
#endif

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
  #ifndef TC_MAX_CONSTANTS
    #define TC_MAX_CONSTANTS 64      // constant pool entries
  #endif
  #define TC_MAX_CONST_DATA  512     // string constant bytes (informational only — buffer is dynamically sized)
  #define TC_INSTR_PER_TICK  500     // instructions per 50ms tick
  #define TC_OUTPUT_SIZE     128     // output buffer for MQTT
#else  // ESP32
  // Bytecode buffer is allocated via heap_caps_malloc(MALLOC_CAP_SPIRAM) with
  // DRAM fallback (see TinyCLoadFile in xdrv_124_tinyc.ino). With PSRAM
  // available the only practical ceiling is the .tcb on-disk size; without
  // PSRAM the available DRAM block sets the real limit. 128 KB is enough
  // headroom for the largest current user scripts (~65 KB) to grow without
  // forcing another firmware bump.
  #define TC_MAX_PROGRAM     131072  // max bytecode size (PSRAM-backed)
  #define TC_STACK_SIZE      256     // operand stack (1KB)
  #define TC_MAX_FRAMES      32      // call depth
  #define TC_MAX_LOCALS      256     // locals per frame (1KB) - enough for char arrays
  #define TC_MAX_GLOBALS     512     // global slots (2KB)
  // Constant pool cap. Raised 256→512 in v1.3.12 and 512→1024 in v1.3.18 for
  // large bat_ctrl.tc-style scripts (BMU + Modbus + REST + Speedwire + EEBus)
  // that hit ~440 unique string literals. Encoding supports u16 (≤65535), so
  // the cap is purely a soft RAM-safety bound: each slot is ~12 bytes →
  // 1024 slots → ~12 KB worst case (calloc'd, not always used). Override in
  // user_config_override.h to tune per project.
  #ifndef TC_MAX_CONSTANTS
    #define TC_MAX_CONSTANTS 1024    // constant pool entries (dynamic alloc, uint16_t)
  #endif
  #define TC_MAX_CONST_DATA  8192    // string constant bytes (informational only — buffer is dynamically sized to fit)
  #define TC_INSTR_PER_TICK  1000    // instructions per 50ms tick
  #define TC_OUTPUT_SIZE     128     // output buffer for MQTT (was 512)
#endif

#define TC_MAX_FILE_HANDLES  4      // max simultaneously open files
#define TC_TCP_CLI_SLOTS     4      // outgoing TCP client slots (selected via tcpSelect)

// VM task stack size (bytes). Bumped 8192 → 12288 (2026-04-27) after a
// stack-overflow crash in tc_persist_save → fs::File::write → fwrite →
// __smakebuf_r → littlefs → SPI flash → FreeRTOS critical section reached
// xPortSetInterruptMaskFromISR with a corrupted TCB. Newlib's fwrite path
// alloca's a ~1 KB stdio buffer in __smakebuf_r, the LittleFS / SPI-flash
// chain adds another ~2 KB, plus Xtensa register windows. With the v1.3.20
// build's slightly different code layout (mbedtls headers pulled in for the
// new crypto syscalls) the meter_pin_unlock.tc autoexec — which calls
// saveVars() in main() — went over 8 KB. 12 KB gives ~3 KB headroom.
#ifndef TC_VM_TASK_STACK
  #define TC_VM_TASK_STACK   12288
#endif

// Heap memory for large arrays (> 255 elements)
#ifdef ESP8266
  #define TC_MAX_HEAP           2048   // heap slots (8KB)
  #define TC_MAX_HEAP_HANDLES   8
#else  // ESP32
  #define TC_MAX_HEAP           16384  // heap slots (64KB upper bound; grown dynamically)
  #define TC_MAX_HEAP_HANDLES   128    // max concurrent heap arrays (energy script uses 68+)
#endif

#define TC_MAGIC           0x54434300  // "TCC\0"
#define TC_VERSION         5           // V5: global (UDP auto-update) variables
#define TC_RELEASE         "1.3.20"    // Symmetric crypto syscalls (360–365): aesEcb / aesCbc (AES-128, in-place on TinyC char[] buffers), hmacSha256, sha256, plus hex2bin / bin2hex byte-twiddling helpers. ESP32-only via mbedtls (already linked for HTTPS/MQTT-TLS); ESP8266 path stubs return 0/no-op. Motivating use case: TinyC scripts speaking the Tuya local protocol (v3.3 = AES-128-ECB) so users can drive Smart-Life-controlled devices (pool heat pumps, plugs, switches, dehumidifiers) directly from Tasmota without a cloud round-trip or a separate bridge. Also enables custom signed REST APIs (HMAC-SHA256), encrypted SML decoders not covered by AmsLib, and per-device MQTT-TLS fingerprinting. Buffers follow TinyC convention (one byte per int32 slot, low 8 bits used); lengths are in bytes and must fit the ref's allocated capacity. AES-CBC stack-allocates up to 4 KB per call, falls back to malloc above; HMAC/SHA bounded at 1024 B key / 4 KB data — bigger payloads should be hashed in chunks via repeated SHA-256 of a hash-state buffer (future enhancement). Tuya v3.4 (ECDH+AES-GCM) not exposed yet — most Smart-Life devices are still v3.3. IDE/compiler-side: BUILTINS table + symbol table entries for the 6 functions need to be added in tinyc_ide.html.gz to make them callable from .tc source (next commit). Previous: "1.3.19" — Cross-VM share table + PSRAM-backed bytecode + IDE strcmp/sprintf fixes. (1) New 8-syscall `share*` API (340–347) lets two TinyC slots exchange named scalars/strings via a driver-global 32-entry table (~2.6 KB DRAM, mutex-protected on ESP32). Use this when one program outgrows a single slot and is split across two — e.g. Andreas's BYD/Speedwire/EEBus stack. Missing-key reads return 0/0.0/"" without error; `shareHas`/`shareDelete` complete the model. (2) `TC_MAX_PROGRAM` 65536 → 131072 with PSRAM fallback: `s->program` and `vm->const_data` allocate from internal DRAM first, only spill to `heap_caps_malloc(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` on OOM (ESP32 only). Small/normal programs stay in fast static RAM; only edge-case 100+ KB scripts (or scripts on devices with fragmented heap) reach PSRAM. AddLog INFO line emitted when PSRAM path is taken. (3) IDE: 4-site emitByte-truncation bug fixed — `emit(Op.SYSCALL); emitByte(Syscall.X)` truncated ids ≥256 to `id & 0xFF`, silently rerouting STRCMP_CONST=275 to SYS_MATH_POW=19 (so `strcmp(arr,"literal")` returned NaN bits 0x7FC00000 = 2143289344, breaking every `if (strcmp(...) == 0)` branch). Same bug hit FILE_WRITE_STR=276, LOG_LEVEL=269, LOG_LEVEL_STR=270. All four sites now use the existing `emitSyscall(id)` helper which auto-picks SYSCALL2 (u16) for ids ≥256. (4) IDE: `inferType(CallExpr)` had a hardcoded float-builtin list (sqrt/sin/cos/.../atof) and ignored the symbol-table `returnFloat: true` flag, so `sprintf(buf,"%.2f",shareGetFloat(...))` saw valType='int' and emitted I2F → reinterpreted bits → printed 1056964608.00 (= 0.5f bit-pattern). Now also returns 'float' when `BUILTINS[name].returnFloat === true`; future float-returning builtins work automatically.   Previous: "1.3.18" — Constant pool cap raised 512→1024 on ESP32 (Andreas BYD/Speedwire/EEBus scripts hit 440/512). "1.3.17" — TC_ERR_BOUNDS rich log + RET-time SP-balance check + compiler float→int narrowing warning.
#define TC_FILE_NAME       "/autoexec.tcb"
#define TC_MAX_PERSIST     64          // max persist variable entries
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

// Callback support — covers all 14 TcCallbackId well-known callbacks plus
// the string-keyed ones (HomeKitWrite, TouchButton, UdpCall, WebOn, WebUI,
// JsonCall, WebCall, WebPage, Command, Event). Must be ≥ TC_CB_COUNT (14)
// or load will silently truncate the hot-path callbacks.
#define TC_MAX_CALLBACKS  24
#ifdef ESP8266
  #define TC_CALLBACK_MAX_INSTR 20000  // instruction limit per callback (ESP8266)
#else
  #define TC_CALLBACK_MAX_INSTR 200000 // instruction limit per callback (ESP32)
#endif
#define TC_CALLBACK_NAME_MAX 20        // max callback name length (longest: OnMqttDisconnect=16+1)

// UDP multicast support (Scripter-compatible protocol)
#define TC_UDP_PORT          1999
#define TC_UDP_MAX_VARS      64         // max tracked UDP variable names
#define TC_UDP_VAR_NAME_MAX  16         // max variable name length
#define TC_UDP_BUF_SIZE      320        // receive buffer (max: 2+16+1+2+64*4 = 277)
#define TC_UDP_MAX_ARRAY     64         // max float array elements per UDP variable
#define TC_UDP_TIMEOUT_SEC   60         // default inactivity timeout (seconds), 0 = disabled

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
  OP_SYSCALL      = 0x80,  // u8 syscall_id
  OP_SYSCALL2     = 0x81,  // u16 syscall_id (extended range 256+)
  // Heap arrays (large arrays > 255 elements)
  OP_LOAD_HEAP_ARR  = 0xA0,  // u8 handle; pop idx -> push value
  OP_STORE_HEAP_ARR = 0xA1,  // u8 handle; pop val, pop idx -> store
  OP_ADDR_HEAP      = 0xA2,  // u8 handle -> push ref: 0xC0000000 | handle
  // Runtime array ref (ref params): resolve packed ref stored in local slot
  OP_LOAD_REF_ARR   = 0xA3,  // u8 local_idx; pop idx -> push *(resolveRef(local[local_idx])+idx)
  OP_STORE_REF_ARR  = 0xA4,  // u8 local_idx; pop val, pop idx -> *(resolveRef(local[local_idx])+idx)=val
  // Watch variables (change tracking)
  OP_STORE_WATCH    = 0xA5,  // u16 varIdx, u16 shadowIdx, u16 writtenIdx — store with shadow update
  // Heap ref with slot offset — for strcpy(arr[i].field, ...)
  OP_ADDR_HEAP_OFF  = 0xA6,  // u8 handle; pop offset -> push ref: 0xC0000000 | (offset<<16) | handle
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
  SYS_MATH_POW  = 19,  SYS_MATH_ACOS = 123, // pow(base,exp)->float, acos(f)->float
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
  SYS_LOG_LVL          = 269, // (level, char_ref) -> void — AddLog with explicit level
  SYS_LOG_LVL_STR       = 270, // (level, const_idx) -> void — AddLog string literal with level
  // Minimal I2S output (no USE_I2S_AUDIO needed)
  SYS_I2S_BEGIN         = 271, // (bclk, lrclk, dout, sampleRate) -> int — init I2S TX, returns 0=ok
  SYS_I2S_WRITE         = 272, // (arr_ref, len) -> int — write int16 PCM samples, returns written
  SYS_I2S_STOP          = 273, // () -> void — stop and release I2S
  SYS_FILE_READ_PCM16   = 274, // (handle, arr_ref, max_samples, channels) -> samples_read
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
  SYS_CAM_INIT_PINS   = 206, // (pins_ref, format, framesize, quality) -> int — init camera with custom pins
  // Hardware register peek/poke (ESP32 memory-mapped I/O)
  SYS_PEEK_REG         = 207, // (addr) -> int — read 32-bit from memory-mapped address
  SYS_POKE_REG         = 208, // (addr, val) -> void — write 32-bit to memory-mapped address
  SYS_TASM_GET_STR     = 209, // (sel, buf_ref) -> int — get Tasmota string info into char[]
  SYS_TASM_POWER       = 217, // (index) -> int — power state of relay (0-based)
  SYS_TASM_SWITCH      = 218, // (index) -> int — switch state (0-based)
  SYS_TASM_COUNTER     = 219, // (index) -> int — pulse counter (0-based)
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
  SYS_DSP_LOAD_IMG    = 262, // (filename_const) -> int — load JPG to PSRAM, returns slot (0-3, -1=err)
  SYS_DSP_IMG_RECT    = 263, // (slot, sx, sy, dx, dy, w, h) -> void — push sub-rect from image to screen
  SYS_DSP_IMG_WIDTH   = 264, // (slot) -> int — get image width
  SYS_DSP_IMG_HEIGHT  = 265, // (slot) -> int — get image height
  SYS_DSP_TEXT_WIDTH  = 266, // (len) -> int — get pixel width for len chars in current font
  SYS_DSP_TEXT_HEIGHT = 267, // () -> int — get pixel height for current font
  SYS_DSP_IMG_TEXT    = 268, // (slot, x, y, color, fieldw, align, buf_ref) -> void — composite text on image
  SYS_DSP_LOAD_IMG_CAM  = 277, // (cam_slot) -> int — decode JPEG from cam slot into a new img slot (-1=err)
  SYS_DSP_IMG_TEXT_BURN = 278, // (slot, x, y, color, fieldw, align, buf_ref) -> void — burn text pixels INTO image buffer (no TFT push)
  SYS_DSP_IMG_TO_CAM    = 279, // (img_slot, cam_slot, quality) -> int — re-encode img slot back into cam slot as JPEG, returns size (-1=err)
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
  // TCP client (outgoing connections) — TC_TCP_CLI_SLOTS parallel slots, selected via tcpSelect()
  SYS_TCP_CONNECT     = 290, // (ip_const, port) -> int — connect to remote ip:port (0=ok, -1=fail, -2=no net)
  SYS_TCP_DISCONNECT  = 291, // () -> void — close selected TCP client slot
  SYS_TCP_CONNECTED   = 292, // () -> int — 1 if selected TCP client connected, 0 otherwise
  SYS_TCP_SELECT      = 293, // (slot) -> void — select outgoing TCP slot (0..TC_TCP_CLI_SLOTS-1)
  SYS_TCP_CONNECT_REF = 294, // (ip_ref, port) -> int — connect with IP from runtime char array
  // MQTT subscribe / publish-to-topic (USE_MQTT). Dispatches OnMqttData(topic,payload) callback.
  SYS_MQTT_SUBSCRIBE   = 295, // (topic_const)          -> int slot (0..9, -1=err)
  SYS_MQTT_UNSUBSCRIBE = 296, // (topic_const)          -> int (0=ok, -1=err)
  SYS_MQTT_PUBLISH_TO  = 297, // (topic_const, payload_const) -> int (0=ok, -1=err)
  // Dynamic task spawn (ESP32 only). Runs a named user function on a new FreeRTOS task
  // with full delay() support; shares the spawning VM's globals/heap. killTask is cooperative.
  SYS_SPAWN_TASK       = 298, // (name_const)              -> int pool_idx (0..TC_MAX_SPAWN_TASKS-1), -1=err
  SYS_SPAWN_TASK_STACK = 299, // (name_const, stack_kb)    -> int pool_idx, -1=err (stack_kb clamped 2..12)
  SYS_KILL_TASK        = 300, // (name_const)              -> int 0=signaled, -1=not running
  SYS_TASK_RUNNING     = 301, // (name_const)              -> int 1=running, 0=idle
  // ── TinyUI — tiny LVGL-like layer on top of existing dsp* primitives ───
  // Passive widgets (label/progress/gauge/icon) live in a small separate pool
  // and are auto-redrawn on screen switch. Interactive widgets (checkbox) are
  // thin wrappers over dspTButton — they live in the existing VButton pool.
  SYS_UI_SCREEN        = 310, // (id)                                -> void   // switch visible screen; -1 reads current
  SYS_UI_THEME         = 311, // (bg, accent, text, border)          -> void   // set global theme colors (RGB565)
  SYS_UI_CLEAR_SCREEN  = 312, // ()                                   -> void   // fill viewport with theme.bg
  SYS_UI_LABEL         = 320, // (num,x,y,w,h,text_const,align)      -> void   // static text (align: 0=left,1=center,2=right)
  SYS_UI_LABEL_SET     = 321, // (num, text_ref_or_const)            -> void   // change label text, redraw
  SYS_UI_CHECKBOX      = 322, // (num,x,y,w,h,label_const)            -> void   // toggle-button-backed checkbox, caller-sized hit area
  SYS_UI_PROGRESS      = 323, // (num,x,y,w,h,value,max)             -> void   // horizontal fill bar
  SYS_UI_PROGRESS_SET  = 324, // (num, value)                        -> void   // update progress bar value
  SYS_UI_GAUGE         = 325, // (num,x,y,r,value,vmin,vmax)         -> void   // circular gauge with needle
  SYS_UI_ICON          = 326, // (num,x,y,img_slot)                   -> void   // clickable image (uses existing img pool)
  SYS_UI_BUTTON        = 327, // (num,x,y,w,h,label_const)            -> void   // VButton-backed momentary pushbutton; TouchButton(num,1) on press, (num,0) on release
  // In-memory RGB565 canvases — let dsp* primitives draw into an image slot
  SYS_IMG_CREATE       = 328, // (w, h)                                -> int slot (0..3, -1=err) // alloc blank canvas in PSRAM
  SYS_IMG_BEGIN_DRAW   = 329, // (slot)                                -> void   // redirect all dsp* to the slot's canvas
  SYS_IMG_END_DRAW     = 330, // ()                                    -> void   // restore panel renderer
  SYS_IMG_CLEAR        = 331, // (slot, color_rgb565)                  -> void   // fast fill of canvas
  SYS_IMG_BLIT         = 332, // (dst,src,sx,sy,dx,dy,w,h)             -> void   // canvas->canvas rect copy (clipped)
  SYS_IMG_INVALIDATE   = 333, // (slot,x,y,w,h)                        -> void   // union rect into slot's dirty region
  SYS_IMG_FLUSH        = 334, // (slot,panel_x,panel_y)                -> void   // push dirty region to panel + clear
  // Cross-VM shared key/value store (driver-global, mutex-protected)
  SYS_SHARE_SET_INT    = 340, // (key_const_idx, val)        -> void
  SYS_SHARE_GET_INT    = 341, // (key_const_idx)             -> int  (0 if missing)
  SYS_SHARE_SET_FLT    = 342, // (key_const_idx, val)        -> void
  SYS_SHARE_GET_FLT    = 343, // (key_const_idx)             -> float (0.0 if missing)
  SYS_SHARE_SET_STR    = 344, // (key_const_idx, src_ref)    -> void
  SYS_SHARE_GET_STR    = 345, // (key_const_idx, dst_ref)    -> int  chars copied (0 if missing)
  SYS_SHARE_HAS        = 346, // (key_const_idx)             -> int  0/1
  SYS_SHARE_DELETE     = 347, // (key_const_idx)             -> int  1 if removed, 0 if not present

  // Symmetric crypto (AES + HMAC-SHA256). Buffers are TinyC `char[]` (one byte
  // per int32 slot, low 8 bits used). Lengths in BYTES. All ops are in-place
  // on the data ref. ESP32-only (mbedtls); ESP8266 returns 0 / no-op.
  // Tuya local protocol (v3.3): AES-128-ECB + zero-pad. Tuya v3.4: AES-128-GCM
  // (not exposed yet — add later if needed). Most Smart-Life pool heaters,
  // plugs, switches use v3.3.
  SYS_AES_ECB          = 360, // (key16_ref, data16_ref, enc_flag)              -> int  1=ok 0=err
  SYS_AES_CBC          = 361, // (key16_ref, iv16_ref, data_ref, len, enc_flag) -> int  1=ok 0=err
                              //   len must be a multiple of 16. iv is updated in-place.
  SYS_HMAC_SHA256      = 362, // (key_ref, klen, data_ref, dlen, out32_ref)     -> int  1=ok
  SYS_SHA256           = 363, // (data_ref, dlen, out32_ref)                    -> int  1=ok
  // Hex / binary helpers (purely byte-twiddling, no crypto state). Convenient
  // for parsing keys/IDs from string literals into byte buffers and back.
  SYS_HEX2BIN          = 364, // (hex_ref, hex_len, out_ref)                    -> int  bytes written (hex_len/2)
                              //   hex_ref may be a const string idx OR a char[] ref.
                              //   Skips whitespace; returns -1 on bad nibble.
  SYS_BIN2HEX          = 365, // (bin_ref, bin_len, out_ref)                    -> int  chars written (bin_len*2),
                              //   lowercase, no separators, NUL-terminated.
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
  SYS_FILE_RANGE      = 260, // (handle, min_ref, max_ref) -> rows (first/last timestamp)
  SYS_TASM_CMD_REF   = 248, // (cmd_ref, out_buf_ref) -> int — tasmCmd with char array command
  SYS_I2C_FREE       = 249, // (addr, bus) -> void — release claimed I2C address
  SYS_WEB_CHART_SIZE  = 233, // (width, height) -> void — set chart div size in pixels (0=default)
  SYS_WEB_CHART_TBASE = 261, // (minutes) -> void — set time base offset from "now" for chart x-axis
  SYS_WEB_REPO_PULLDOWN = 280, // (gref, label_c, json_url_c, index_key_c, dest_path_c) -> void — Scripter smlpd()-style remote JSON directory picker
  SYS_SML_APPLY_PINS    = 281, // (path_c, rx, tx, smlf) -> int — idempotent SML descriptor pin substitution (%0?rxpin%/%0?txpin%/%0?smlf%, leading 0 optional). Inserts "; <template>" comment line above each active line on first call; rebuilds active line from template on subsequent calls. Values are substituted verbatim (e.g. tx=-1 becomes the literal "-1" which SML accepts as "no tx pin"); the original placeholder text is preserved only in the template comment. Returns # subs done, 0 = no change, -1 = err.
  SYS_SML_SCRIPTER_LOAD = 282, // (path_c) -> int — extract >F/>S sections from descriptor, compile to bytecode, run on EverySecond/Every100ms ticks. Subset: lnv0..lnv9, +=/-=/*=//=/=, +-*/% < <= > >= == !=, switch/case/ends, if/endif, sml(m,0,baud), sml(m,1,"HEX"). Returns # sections compiled (0..2), -1 = err.
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
  // Extended syscalls (256+, requires OP_SYSCALL2)
  SYS_STRCMP_CONST    = 275, // (arr_ref, const_idx) -> int — strcmp(char[], "literal")
  SYS_SML_COPY        = 256, // (arr_ref, count) -> int — copy SML values to float array
  SYS_ARRAY_FILL      = 257, // (arr_ref, value, count) -> void — fill array with value
  SYS_ARRAY_COPY      = 258, // (dst_ref, src_ref, count) -> void — copy array to array
  SYS_VM_STACK_DEPTH  = 283, // () -> int — current operand-stack depth (diagnostic)
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

// Emit an actionable BOUNDS diagnostic just before we return TC_ERR_BOUNDS
// from one of the array-access opcodes. The generic "Bounds error" in the
// TaskLoop/crash log tells you nothing about WHICH access; this says
//   TCC: BOUNDS <kind> idx=<i> size=<n> pc=<p>
// so you can see whether it was globals[], a local[], or a heap[handle],
// what index was attempted, what bound was in effect, and the PC.
//
// Array names are not available at VM level (the bytecode carries only base
// addresses / handle IDs), so the caller passes a short literal kind label.
// For the motivating bug (bat_ctrl.tc, g_sl overwritten to ~230, then
// pvact[g_sl] with pvact.size == 96), this prints
//   TCC: BOUNDS global[] idx=230 size=96 pc=43306
// which points straight at the line.
static void tc_log_bounds(const char *kind, int32_t idx, uint32_t bound, uint16_t pc) {
  AddLog(LOG_LEVEL_ERROR,
         PSTR("TCC: BOUNDS %s idx=%ld size=%u pc=%u"),
         kind, (long)idx, (unsigned)bound, (unsigned)pc);
}

// Write crash info to /crash.log (append, keeps last entries)
static void tc_crash_log(int err, uint16_t pc, uint32_t instr_count, const char *context) {
#ifdef ESP32
  if (!ufsp) return;
  FS *fs = ufsp;
  File f = fs->open("/crash.log", "a");
  if (!f) return;
  char ts[24];
  snprintf(ts, sizeof(ts), "%s", GetDateAndTime(DT_LOCAL).c_str());
  f.printf("%s  err=%d (%s)  PC=%u  instr=%u  ctx=%s  heap=%u\n",
    ts, err, tc_error_str(err), pc, instr_count, context ? context : "?", ESP.getFreeHeap());
  f.close();
  AddLog(LOG_LEVEL_ERROR, PSTR("TCC: crash logged to /crash.log"));
#endif
}

/*********************************************************************************************\
 * VM Data structures
\*********************************************************************************************/

typedef struct {
  uint16_t return_pc;
  uint16_t saved_sp;    // caller's SP at OP_CALL (including args pushed on top);
                        // used by RET to flag callees that leaked stack slots.
                        // sp > saved_sp at RET is a definite leak (pushed more
                        // than consumed). Not accessed for frame 0 (main/callback).
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

// Hot-path callbacks resolved once at load via integer ID, so dispatch
// avoids strcmp scans on every tick. The ID is just an index into
// vm->cb_index[] which holds the slot in vm->callbacks[] (or -1 if absent).
typedef enum {
  TC_CB_LOOP = 0,             // EveryLoop
  TC_CB_EVERY_50_MSECOND,     // Every50ms
  TC_CB_EVERY_100_MSECOND,    // Every100ms
  TC_CB_EVERY_SECOND,         // EverySecond
  TC_CB_ON_INIT,              // OnInit
  TC_CB_ON_WIFI_CONNECT,      // OnWifiConnect
  TC_CB_ON_WIFI_DISCONNECT,   // OnWifiDisconnect
  TC_CB_ON_MQTT_CONNECT,      // OnMqttConnect
  TC_CB_ON_MQTT_DISCONNECT,   // OnMqttDisconnect
  TC_CB_ON_MQTT_DATA,         // OnMqttData(topic, payload) — dispatched from FUNC_MQTT_DATA
  TC_CB_ON_TIME_SET,          // OnTimeSet
  TC_CB_CLEAN_UP,             // CleanUp
  TC_CB_TASK_LOOP,            // TaskLoop
  TC_CB_ON_EXIT,              // OnExit
  TC_CB_COUNT
} TcCallbackId;

// Name table: index by TcCallbackId. Plain RAM (small, hot data).
// PROGMEM with pointer-in-table is unreliable on ESP32 — see project notes.
static const char * const TC_CB_NAME[TC_CB_COUNT] = {
  "EveryLoop",
  "Every50ms",
  "Every100ms",
  "EverySecond",
  "OnInit",
  "OnWifiConnect",
  "OnWifiDisconnect",
  "OnMqttConnect",
  "OnMqttDisconnect",
  "OnMqttData",
  "OnTimeSet",
  "CleanUp",
  "TaskLoop",
  "OnExit",
};

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
  // Hot-path dispatch cache: cb_index[TcCallbackId] = slot in callbacks[] (or -1).
  // Populated once at load — eliminates strcmp scans on every tick.
  int8_t        cb_index[TC_CB_COUNT];
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
  bool     upload_active;           // true during upload — pauses VM callbacks
  uint8_t *upload_buf;
  uint32_t upload_alloc_size;       // size of upload_buf allocation (may be < TC_MAX_PROGRAM if client sent ?fsz=N)
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
  // UDP socket inactivity watchdog — reset socket if no rx within timeout
  uint32_t udp_last_rx;           // millis() of last received multicast packet
  uint16_t udp_timeout;           // inactivity timeout in seconds (0 = disabled)
  // General-purpose UDP port (Scripter-compatible udp() function)
  WiFiUDP  udp_port;              // general-purpose UDP socket
  uint16_t udp_port_num;          // bound port number
  bool     udp_port_open;         // port is listening
  uint32_t udp_port_last_rx;     // millis() of last received packet on general port
  uint16_t udp_port_timeout;     // inactivity timeout in seconds (0 = disabled)
  IPAddress udp_port_mcast;       // multicast group IP (0.0.0.0 = unicast)
  // WebUI pages (up to 6, set by wLabel(), buttons on main page)
#define TC_MAX_WEB_PAGES 6
  char     page_label[TC_MAX_WEB_PAGES][32];
  uint8_t  page_slot[TC_MAX_WEB_PAGES];   // which VM slot registered each page
  uint8_t  page_count;            // number of registered pages
  uint8_t  current_page;          // current page being rendered (for wPage())
  // Custom web handlers (webOn)
#define TC_MAX_WEB_HANDLERS 7
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
  WiFiClient tcp_client;               // current server-accepted client
  // TCP client state (outgoing connections — independent from server).
  // TC_TCP_CLI_SLOTS parallel slots so a script can talk to multiple endpoints
  // (e.g. BYD BMU + SMA HM2.0 + Wallbox) in parallel. Slot 0 is the default.
  WiFiClient tcp_cli_clients[TC_TCP_CLI_SLOTS];  // outgoing TCP client slots
  uint8_t    tcp_cli_slot;                       // currently selected slot (0..TC_TCP_CLI_SLOTS-1)
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
#ifdef ESP32
  // Minimal I2S output (standalone, no USE_I2S_AUDIO needed)
  i2s_chan_handle_t i2s_tx_handle;   // I2S TX channel handle (nullptr = not active)
  int32_t  i2s_sample_rate;          // current sample rate
  int16_t *i2s_pcm_buf;             // stereo PCM buffer (alloc in i2sBegin, free in i2sStop)
#endif
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
static int32_t  tc_chart_time_base = 0; // time base offset in minutes from "now" (0=now)
#define TC_MAX_SERIAL_PORTS 3
static TasmotaSerial *tc_serial_ports[TC_MAX_SERIAL_PORTS] = {}; // TinyC serial ports (up to 3, one per handle)

// Image store for dspLoadImage / dspPushImageRect (watchface backgrounds etc.)
// Slots from imgCreate() additionally carry a RendererCanvas so that all
// dsp* primitives can be temporarily retargeted to the slot's pixel buffer
// via imgBeginDraw()/imgEndDraw().
#ifdef USE_DISPLAY
  #include <renderer.h>     // for Renderer / RendererCanvas types
  extern Renderer *renderer;
#endif
#define TC_IMG_SLOTS 4
struct TcImgSlot {
  uint16_t       *buf;     // RGB565 pixel data in PSRAM (NULL = free)
  uint16_t        w, h;    // image dimensions
#ifdef USE_DISPLAY
  RendererCanvas *canvas;  // non-null if created via imgCreate() (JPG slots set null)
#endif
};
static TcImgSlot tc_img_store[TC_IMG_SLOTS] = {};

#ifdef USE_DISPLAY
// Saved panel renderer during imgBeginDraw()/imgEndDraw() redirect window.
// Only one redirect active at a time; nested begin calls are ignored.
static Renderer *tc_canvas_saved_renderer = nullptr;
#endif

// Helper: free all image store slots (called on VM stop). If a redirect is
// still active we restore the panel renderer first so we don't leave the
// global pointing at a canvas we're about to delete.
static void tc_img_store_free(void) {
#ifdef USE_DISPLAY
  if (tc_canvas_saved_renderer) {
    renderer = tc_canvas_saved_renderer;
    tc_canvas_saved_renderer = nullptr;
  }
#endif
  for (int i = 0; i < TC_IMG_SLOTS; i++) {
#ifdef USE_DISPLAY
    if (tc_img_store[i].canvas) {
      delete tc_img_store[i].canvas;
      tc_img_store[i].canvas = nullptr;
    }
#endif
    if (tc_img_store[i].buf) {
      free(tc_img_store[i].buf);
      tc_img_store[i].buf = nullptr;
      tc_img_store[i].w = 0;
      tc_img_store[i].h = 0;
    }
  }
}

// Helper: allocate a new slot
static TcSlot *tc_slot_alloc(void) {
  TcSlot *s = (TcSlot *)calloc(1, sizeof(TcSlot));
  if (s) {
    s->extract_handle = -1;
    // cb_index defaults to zeros from calloc, but zero is a valid slot index.
    // Explicitly mark all well-known callbacks as "not present" until load.
    for (int k = 0; k < TC_CB_COUNT; k++) s->vm.cb_index[k] = -1;
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
 * Local ref:   bits 31-30 = 00/01   (fp << 16) | base_index
 * Global ref:  bits 31-30 = 10      0x80000000 | base_index
 * Heap ref:    bits 31-30 = 11      0xC0000000 | (offset << 16) | handle
 *                                   - bit 15 MUST be 0 (distinguishes from const-pool)
 *                                   - bits 29-16 = slot offset within the heap block (0..16383)
 *                                   - bits 7-0   = handle (0..255, TC_MAX_HEAP_HANDLES=128)
 * Const-pool:  bits 31-30 = 11, bit 15 = 1   0xC0008000 | const_idx (15-bit idx)
\*********************************************************************************************/

static inline int32_t tc_make_local_ref(uint8_t fp, uint8_t base) {
  return ((int32_t)fp << 16) | base;
}
static inline int32_t tc_make_global_ref(uint16_t base) {
  return (int32_t)0x80000000 | base;
}
// Pack a heap ref with optional slot offset. offset must be < 16384 (14 bits).
static inline int32_t tc_make_heap_ref(uint8_t handle, uint16_t offset) {
  return (int32_t)(0xC0000000U | (((uint32_t)(offset & 0x3FFF)) << 16) | handle);
}

// Resolve a packed array ref to a pointer into VM memory, returns NULL on error
static int32_t* tc_resolve_ref(TcVM *vm, int32_t ref) {
  uint32_t uref = (uint32_t)ref;
  uint8_t tag = uref >> 30;
  if (tag == 3) {
    // Heap ref with optional slot offset: 0xC0000000 | (offset<<16) | handle
    // (const-pool refs are detected separately via tc_is_const_ref — callers
    //  that need string-literal support check that FIRST; this path assumes
    //  a regular heap ref.)
    uint8_t handle = (uint8_t)(uref & 0xFF);
    uint16_t offset = (uint16_t)((uref >> 16) & 0x3FFF);
    if (handle >= TC_MAX_HEAP_HANDLES) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: heap handle %d >= max %d"), handle, TC_MAX_HEAP_HANDLES);
      return nullptr;
    }
    if (!vm->heap_data || !vm->heap_handles || !vm->heap_handles[handle].alive) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: heap handle %d invalid (data=%d handles=%d alive=%d)"),
             handle, vm->heap_data != nullptr, vm->heap_handles != nullptr,
             vm->heap_handles ? vm->heap_handles[handle].alive : 0);
      return nullptr;
    }
    if (offset >= vm->heap_handles[handle].size) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: heap ref offset %d >= size %d (handle %d)"),
             offset, vm->heap_handles[handle].size, handle);
      return nullptr;
    }
    return &vm->heap_data[vm->heap_handles[handle].offset + offset];
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
    // Heap ref — subtract offset from total size so a sub-ref sees only
    // its own tail of the block.
    uint8_t handle = (uint8_t)(uref & 0xFF);
    uint16_t offset = (uint16_t)((uref >> 16) & 0x3FFF);
    if (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive) {
      uint16_t sz = vm->heap_handles[handle].size;
      return (offset < sz) ? (int32_t)(sz - offset) : 0;
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

// Detect a const-pool ref emitted by emitStringArg: tag=3 (0xC0000000) with
// the 0x8000 bit of the handle set. Compiler uses this encoding to pass a
// string literal where a char[] ref is expected (e.g. user function arg).
static inline bool tc_is_const_ref(int32_t ref) {
  uint32_t uref = (uint32_t)ref;
  return ((uref >> 30) == 3) && ((uref & 0x8000) != 0);
}

// Extract null-terminated C string from VM array ref into char buffer
// Returns number of chars written (excluding null terminator)
static int tc_ref_to_cstr(TcVM *vm, int32_t ref, char *out, int maxOut) {
  if (tc_is_const_ref(ref)) {
    uint16_t idx = (uint16_t)(((uint32_t)ref) & 0x7FFF);
    const char *s = nullptr;
    if (idx < vm->const_count && vm->constants[idx].type == 1) {
      s = vm->constants[idx].str.ptr;
    }
    if (!s) { out[0] = '\0'; return 0; }
    int i;
    for (i = 0; i < maxOut - 1 && s[i]; i++) out[i] = s[i];
    out[i] = '\0';
    return i;
  }
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

/*********************************************************************************************\
 * TinyUI — lightweight retained-mode UI layer on top of TinyC display primitives
 *
 * Design: two parallel widget pools with DIFFERENT index spaces:
 *   - Existing VButton pool (buttons[MAX_TOUCH_BUTTONS]) for INTERACTIVE widgets:
 *     Button/TButton/PButton/Slider/Checkbox/Icon — handled via existing USE_TOUCH_BUTTONS
 *   - New TcUiWidget pool (tc_ui_widgets[]) for PASSIVE widgets:
 *     Label/Progress/Gauge — drawn immediately + stored for screen-switch redraw
 *
 * Screens: uiScreen(id) sets current screen, clears canvas w/ theme.bg, removes all VButtons,
 * then redraws any passive widgets tagged with the new screen id. The user's script re-creates
 * interactive widgets via their Build*() functions on screen change.
\*********************************************************************************************/
#define TC_UI_MAX_WIDGETS        16
#define TC_UI_WIDGET_NONE        0
#define TC_UI_WIDGET_LABEL       1
#define TC_UI_WIDGET_PROGRESS    2
#define TC_UI_WIDGET_GAUGE       3

  typedef struct {
    uint16_t bg;      // screen background
    uint16_t fg;      // default foreground (text)
    uint16_t accent;  // accent / fill / needle
    uint16_t border;  // border / gauge scale
    uint16_t muted;   // secondary text / empty track
    uint8_t  pad;     // default padding inside widgets (px)
    uint8_t  radius;  // corner radius for future use
  } TcUiTheme;

  typedef struct {
    uint8_t  type;      // TC_UI_WIDGET_*
    uint8_t  screen;    // screen id (0 = all)
    int16_t  x, y, w, h;
    int32_t  value;
    int32_t  vmin;
    int32_t  vmax;
    uint16_t fg;
    uint16_t bg;
    uint16_t accent;
    int8_t   align;     // -1 right, 0 centre, 1 left
    char     text_buf[48]; // label text (copied from const string on create / uiLabelSet)
  } TcUiWidget;

  static TcUiTheme tc_ui_theme = {
    /*bg*/     0x0000, /*fg*/     0xFFFF, /*accent*/ 0x07FF,
    /*border*/ 0x39E7, /*muted*/  0x7BEF, /*pad*/    4, /*radius*/ 6
  };
  static uint8_t     tc_ui_current_screen = 0;
  static TcUiWidget  tc_ui_widgets[TC_UI_MAX_WIDGETS];

  // Draw a single passive widget using existing display primitives.
  static void tc_ui_draw_widget(const TcUiWidget *w) {
    if (!renderer || w->type == TC_UI_WIDGET_NONE) return;
    uint16_t saved_fg = fg_color;
    switch (w->type) {
      case TC_UI_WIDGET_LABEL: {
        // Clear label area with theme.bg, then draw text left/centre/right of area.
        renderer->fillRect(w->x, w->y, w->w, w->h, w->bg);
        const char *txt = w->text_buf;
        fg_color = w->fg;
        int16_t tx = w->x + tc_ui_theme.pad;
        if (w->align == 0) {        // centre (rough — full Adafruit textBounds not needed here)
          int16_t tw = (int16_t)(strlen(txt) * 6); // approx 6 px / char for default font
          tx = w->x + (w->w - tw) / 2;
          if (tx < w->x) tx = w->x;
        } else if (w->align == -1) { // right
          int16_t tw = (int16_t)(strlen(txt) * 6);
          tx = w->x + w->w - tw - tc_ui_theme.pad;
          if (tx < w->x) tx = w->x;
        }
        disp_xpos = tx;
        disp_ypos = w->y + tc_ui_theme.pad;
        renderer->setCursor(disp_xpos, disp_ypos);
        renderer->setTextColor(w->fg, w->bg);
        renderer->println(txt);
        break;
      }
      case TC_UI_WIDGET_PROGRESS: {
        // Empty track
        renderer->fillRect(w->x, w->y, w->w, w->h, w->bg);
        // Border
        renderer->drawRect(w->x, w->y, w->w, w->h, tc_ui_theme.border);
        // Filled portion
        int32_t range = w->vmax - w->vmin;
        if (range <= 0) range = 1;
        int32_t v = w->value;
        if (v < w->vmin) v = w->vmin;
        if (v > w->vmax) v = w->vmax;
        int32_t fw = ((int32_t)(w->w - 2) * (v - w->vmin)) / range;
        if (fw > 0) {
          renderer->fillRect(w->x + 1, w->y + 1, (int16_t)fw, w->h - 2, w->accent);
        }
        break;
      }
      case TC_UI_WIDGET_GAUGE: {
        // Semicircular gauge drawn as short line segments.
        // x,y = centre; w = radius; value/vmin/vmax map to -120..+120 degrees arc.
        int16_t cx = w->x, cy = w->y;
        int16_t r  = w->w;
        if (r < 4) r = 4;
        // Erase previous needle: wipe the interior with bg. The scale ring at
        // radius r is preserved (we only clear up to r-3; needle extends to r-4,
        // so the old line is fully inside the wiped disc).
        renderer->fillCircle(cx, cy, r - 3, w->bg);
        const int SEGMENTS = 30;
        const float a_start = -2.0944f; // -120 deg
        const float a_span  =  4.1888f; //  240 deg
        // Scale track (muted)
        for (int i = 0; i < SEGMENTS; i++) {
          float a0 = a_start + a_span * ((float)i       / SEGMENTS);
          float a1 = a_start + a_span * ((float)(i + 1) / SEGMENTS);
          int16_t x0 = cx + (int16_t)((float)r       * cosf(a0));
          int16_t y0 = cy + (int16_t)((float)r       * sinf(a0));
          int16_t x1 = cx + (int16_t)((float)r       * cosf(a1));
          int16_t y1 = cy + (int16_t)((float)r       * sinf(a1));
          renderer->drawLine(x0, y0, x1, y1, tc_ui_theme.border);
        }
        // Needle
        int32_t range = w->vmax - w->vmin;
        if (range <= 0) range = 1;
        int32_t v = w->value;
        if (v < w->vmin) v = w->vmin;
        if (v > w->vmax) v = w->vmax;
        float frac = (float)(v - w->vmin) / (float)range;
        float a = a_start + a_span * frac;
        int16_t nx = cx + (int16_t)((float)(r - 4) * cosf(a));
        int16_t ny = cy + (int16_t)((float)(r - 4) * sinf(a));
        renderer->drawLine(cx, cy, nx, ny, w->accent);
        renderer->fillCircle(cx, cy, 3, w->accent);
        break;
      }
      default: break;
    }
    fg_color = saved_fg;
  }

  // Helper: clear screen by drawing a filled rect of theme.bg covering the display
  static void tc_ui_clear_screen() {
    if (!renderer) return;
    int16_t dw = renderer->width();
    int16_t dh = renderer->height();
    renderer->fillRect(0, 0, dw, dh, tc_ui_theme.bg);
  }

  // Helper: redraw every passive widget that belongs to the current screen (or screen 0 = all)
  static void tc_ui_redraw_all_widgets() {
    for (int i = 0; i < TC_UI_MAX_WIDGETS; i++) {
      const TcUiWidget *w = &tc_ui_widgets[i];
      if (w->type == TC_UI_WIDGET_NONE) continue;
      if (w->screen != 0 && w->screen != tc_ui_current_screen) continue;
      tc_ui_draw_widget(w);
    }
  }

  // Helper: delete all VButtons (used on screen switch)
  static void tc_ui_delete_all_vbuttons() {
#ifdef USE_TOUCH_BUTTONS
    for (uint32_t i = 0; i < MAX_TOUCH_BUTTONS; i++) {
      if (buttons[i]) { delete buttons[i]; buttons[i] = nullptr; }
    }
#endif
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

// Forward declarations — defined later in this file
static int tc_vm_call_callback(TcVM *vm, const char *name);
static int tc_vm_call_callback_id(TcVM *vm, TcCallbackId cid);

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
    Tinyc->udp_last_rx = millis();  // reset watchdog on (re)connect
    if (!Tinyc->udp_timeout) {
      Tinyc->udp_timeout = TC_UDP_TIMEOUT_SEC;  // set default on first init
    }
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP multicast started on port %d (timeout %ds)"), TC_UDP_PORT, Tinyc->udp_timeout);
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
    Tinyc->udp_port_mcast = IPAddress(0,0,0,0);
  }
  // Stop all outgoing TCP client slots
  for (uint8_t _s = 0; _s < TC_TCP_CLI_SLOTS; _s++) {
    if (Tinyc->tcp_cli_clients[_s].connected()) {
      Tinyc->tcp_cli_clients[_s].stop();
    }
  }
  Tinyc->tcp_cli_slot = 0;
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

  // Inactivity watchdog: reset socket if no packet received within timeout
  if (Tinyc->udp_timeout > 0) {
    uint32_t elapsed = millis() - Tinyc->udp_last_rx;
    if (elapsed > (uint32_t)Tinyc->udp_timeout * 1000) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP multicast rx timeout (%ds) — resetting socket"), Tinyc->udp_timeout);
      Tinyc->udp.flush();
      Tinyc->udp.stop();
      Tinyc->udp_connected = false;
      tc_udp_init();  // immediately reconnect
      return;
    }
  }

  bool got_packet = false;
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

    got_packet = true;
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

  // Reset watchdog timer on any received packet
  if (got_packet) {
    Tinyc->udp_last_rx = millis();
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

static void tc_serial_close(int h) {
  if (h >= 0 && h < TC_MAX_SERIAL_PORTS && tc_serial_ports[h]) {
    tc_serial_ports[h]->flush();
    delay(50);
    delete tc_serial_ports[h];
    tc_serial_ports[h] = nullptr;
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial port %d closed"), h);
  }
}

static void tc_serial_close_all(void) {
  for (int i = 0; i < TC_MAX_SERIAL_PORTS; i++) tc_serial_close(i);
}

static inline TasmotaSerial *tc_serial_get(int h) {
  return (h >= 0 && h < TC_MAX_SERIAL_PORTS) ? tc_serial_ports[h] : nullptr;
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
 * SML descriptor pin substitution helpers — used by SYS_SML_APPLY_PINS
 *
 * Recognizes 3 placeholders: %0?rxpin% / %0?txpin% / %0?smlf% (leading 0 optional,
 * matching the IDE's regex). Substitution is idempotent via "; <template>" comment
 * lines kept above the active line.
\*********************************************************************************************/

// Try to match a %placeholder% at p[0..end). Returns chars consumed (>=5) on
// match and writes 0/1/2 (rx/tx/smlf) to *kind. Returns 0 on no match.
static int tc_sml_match_ph(const char *p, const char *end, int *kind) {
  if (p >= end || *p != '%') return 0;
  const char *q = p + 1;
  if (q < end && *q == '0') q++;  // optional leading "0"
  size_t r = end - q;
  if (r >= 6 && memcmp(q, "rxpin%", 6) == 0) { *kind = 0; return (int)((q + 6) - p); }
  if (r >= 6 && memcmp(q, "txpin%", 6) == 0) { *kind = 1; return (int)((q + 6) - p); }
  if (r >= 5 && memcmp(q, "smlf%",  5) == 0) { *kind = 2; return (int)((q + 5) - p); }
  return 0;
}

static bool tc_sml_line_has_ph(const char *p, size_t len) {
  const char *end = p + len;
  for (const char *q = p; q < end; q++) {
    int kind;
    if (tc_sml_match_ph(q, end, &kind)) return true;
  }
  return false;
}

// Copy src→dst substituting placeholders with the literal numeric value (including
// negative values — e.g. -1 becomes "-1" in the output, which SML accepts as "no pin"
// for txpin). The original placeholders stay in the "; <template>" comment line so
// subsequent calls can still re-substitute with different values. Returns bytes
// written.
//
// Rationale: SML's descriptor parser uses strtol() and requires every field to be a
// valid integer. Leaving any "%0xxx%" text in the active line makes SML reject the
// descriptor entirely. The template-comment line preserves the placeholder form for
// later edits, while the active line is always fully substituted.
static size_t tc_sml_subst_line(const char *src, size_t src_len,
                                char *dst, size_t dst_cap,
                                const int values[3]) {
  const char *end = src + src_len;
  const char *p = src;
  size_t out = 0;
  while (p < end && out < dst_cap) {
    int kind;
    int phlen = tc_sml_match_ph(p, end, &kind);
    if (phlen > 0) {
      char numbuf[12];
      int nlen = snprintf(numbuf, sizeof(numbuf), "%d", values[kind]);
      if (nlen > 0 && (size_t)(out + nlen) <= dst_cap) {
        memcpy(dst + out, numbuf, (size_t)nlen);
        out += (size_t)nlen;
      }
      p += phlen;
    } else {
      dst[out++] = *p++;
    }
  }
  return out;
}

/*********************************************************************************************\
 * Mini-Scripter — runs a tiny Tasmota Scripter subset extracted from an SML descriptor's
 *   >F (Every100ms) and >S (EverySecond) sections. Aimed at builds without full Scripter
 *   (e.g. -DTINYC_NO_SCRIPTER) where the user wants the descriptor's IEC mode-A handshake
 *   or similar tick-driven logic to keep working.
 *
 * Subset (anything outside is a compile-time error → load fails, descriptor unaffected):
 *   variables   lnv0..lnv9           (10 int32, volatile, zeroed at load)
 *   operators   = += -= *= /=        + - * / %     < <= > >= == !=
 *   control     switch <lnv> [case <int> ...]+ ends
 *               if <expr> ... endif
 *   syscalls    sml(meter, 0, baud)      → SML_SetBaud
 *               sml(meter, 1, "HEX..")   → SML_Write (send hex bytes)
 *               sml(-meter, 1, "BAUD:8X1") → SML_Write (reconfigure serial framing,
 *                                           handled natively by SML_Write's flag<0 path,
 *                                           e.g. "2400:8E1" to switch to 8E1 for M-Bus)
 *               delay(ms)                → yield-style delay (capped at 1000 ms; see note)
 *               spinm(pin, mode)         → pinMode(pin, mode?OUTPUT:INPUT)
 *               spin(pin, val)           → digitalWrite(pin, val?HIGH:LOW)
 *                                          (used by IR-reading-head descriptors like
 *                                           EasyMeter Q3A to drive the optical TX LED)
 *   sections    >F (Every100ms)   >S (EverySecond)
 *   misc        ; end-of-line comment   ;-prefixed line comment   blank lines OK
 *
 * Compiled to a stack-machine bytecode at smlScripterLoad() time. Runtime cost per tick
 * is dispatch + a handful of arithmetic/cmp ops; no allocations after load.
 *
 * `delay(ms)` is intended for short M-Bus pre-wake pauses (few hundred ms) — it runs
 * inside the Tasmota main loop so long delays will stall other drivers. The exec caps
 * at 1000 ms as a safety net; longer waits should be expressed as `case N` ticks instead.
 *
 * NOTE: If full Tasmota Scripter is also present and parsing the same file, both will run
 * the >F/>S blocks → double work / double SML sends. Use this only when Scripter is off.
\*********************************************************************************************/

#define TC_MSCR_BC_MAX     512      // bytecode bytes per section (>F or >S)
#define TC_MSCR_LNV_COUNT  10
#define TC_MSCR_STK_MAX    16
#define TC_MSCR_HEX_MAX    128      // max payload bytes for sml(m,1,"HEX..") — up to
                                    // 256 hex chars, enough for ~1 s of 0x55 wake at 2400 bd
#define TC_MSCR_DELAY_CAP  1000     // ms — hard cap for delay() to avoid loop starvation

enum TcMsOp {
  MS_OP_END = 0,
  MS_OP_PUSH_LNV,    // [op][var]                    — push lnv[var]
  MS_OP_PUSH_IMM,    // [op][i32 LE]                 — push imm
  MS_OP_POP_LNV,     // [op][var]                    — pop → lnv[var]
  MS_OP_ADD, MS_OP_SUB, MS_OP_MUL, MS_OP_DIV, MS_OP_MOD,
  MS_OP_LT, MS_OP_LE, MS_OP_GT, MS_OP_GE, MS_OP_EQ, MS_OP_NE,
  MS_OP_JMP,         // [op][u16 LE abs]             — unconditional
  MS_OP_JZ,          // [op][u16 LE abs]             — pop, if 0 jump
  MS_OP_SML_BAUD,    // pop baud, pop meter          → SML_SetBaud
  MS_OP_SML_HEX,     // [op][len_lo][len_hi][chars...] pop meter → SML_Write(meter, hexstr)
                     // (for negative meter, the payload is "BAUD:8X1" and SML_Write's
                     //  flag<0 path reconfigures serial framing instead of sending bytes)
  MS_OP_DELAY,       // pop ms → delay(min(ms, TC_MSCR_DELAY_CAP))
  MS_OP_SPIN,        // pop val, pop pin             → digitalWrite(pin, val?HIGH:LOW)
  MS_OP_SPINM,       // pop mode, pop pin            → pinMode(pin, mode?OUTPUT:INPUT)
};

typedef struct TcMiniScripter {
  int32_t  lnv[TC_MSCR_LNV_COUNT];
  uint8_t  bc_f[TC_MSCR_BC_MAX];
  uint8_t  bc_s[TC_MSCR_BC_MAX];
  uint16_t bc_f_len;
  uint16_t bc_s_len;
  uint8_t  loaded;          // 0/1 — bytecode populated?
  uint8_t  has_f;
  uint8_t  has_s;
} TcMiniScripter;

static TcMiniScripter tc_mscr;

// ── Tokenizer ─────────────────────────────────────────────────────
enum TcMsTokKind {
  MTK_END = 0, MTK_NL, MTK_INT, MTK_HEXSTR, MTK_LNV,
  MTK_KW_SWITCH, MTK_KW_CASE, MTK_KW_ENDS, MTK_KW_IF, MTK_KW_ENDIF, MTK_KW_SML,
  MTK_KW_DELAY, MTK_KW_SPIN, MTK_KW_SPINM,
  MTK_KW_FOR, MTK_KW_NEXT,
  MTK_PLUS, MTK_MINUS, MTK_MUL, MTK_DIV, MTK_MOD,
  MTK_LT, MTK_LE, MTK_GT, MTK_GE, MTK_EQ, MTK_NE,
  MTK_ASSIGN, MTK_PLUSEQ, MTK_MINUSEQ, MTK_MULEQ, MTK_DIVEQ,
  MTK_LPAREN, MTK_RPAREN,
  MTK_ERR
};

typedef struct {
  uint8_t k;
  int32_t i;            // int value, lnv idx
  const char *str;      // hex content
  uint16_t slen;
} TcMsTok;

typedef struct {
  const char *p, *end;
  uint8_t *bc;
  uint16_t cap, len;
  uint16_t line;
  uint8_t err;          // 0=ok, 1=overflow, 2=syntax
  TcMsTok cur;
  uint8_t have_cur;
} TcMsCompiler;

static void tc_mscr_skip_inline_ws(TcMsCompiler *c) {
  while (c->p < c->end) {
    char ch = *c->p;
    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == ',') { c->p++; continue; }
    if (ch == ';') {                              // comment to end of line
      while (c->p < c->end && *c->p != '\n') c->p++;
      continue;
    }
    break;
  }
}

static void tc_mscr_lex(TcMsCompiler *c, TcMsTok *t) {
  tc_mscr_skip_inline_ws(c);
  t->str = NULL; t->slen = 0; t->i = 0;
  if (c->p >= c->end) { t->k = MTK_END; return; }
  char ch = *c->p;
  if (ch == '\n') { c->p++; c->line++; t->k = MTK_NL; return; }
  if (ch >= '0' && ch <= '9') {
    int32_t v = 0;
    while (c->p < c->end && *c->p >= '0' && *c->p <= '9') { v = v*10 + (*c->p - '0'); c->p++; }
    t->k = MTK_INT; t->i = v; return;
  }
  if (ch == '"') {
    c->p++;                                       // opening quote
    t->str = c->p;
    while (c->p < c->end && *c->p != '"' && *c->p != '\n') c->p++;
    t->slen = (uint16_t)(c->p - t->str);
    if (c->p < c->end && *c->p == '"') c->p++;    // closing quote
    t->k = MTK_HEXSTR; return;
  }
  // Multi-char operators
  if (ch == '<' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_LE; return; }
  if (ch == '>' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_GE; return; }
  if (ch == '=' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_EQ; return; }
  if (ch == '!' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_NE; return; }
  if (ch == '+' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_PLUSEQ; return; }
  if (ch == '-' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_MINUSEQ; return; }
  if (ch == '*' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_MULEQ; return; }
  if (ch == '/' && c->p+1 < c->end && c->p[1] == '=') { c->p+=2; t->k = MTK_DIVEQ; return; }
  switch (ch) {
    case '+': c->p++; t->k = MTK_PLUS;   return;
    case '-': c->p++; t->k = MTK_MINUS;  return;
    case '*': c->p++; t->k = MTK_MUL;    return;
    case '/': c->p++; t->k = MTK_DIV;    return;
    case '%': c->p++; t->k = MTK_MOD;    return;
    case '<': c->p++; t->k = MTK_LT;     return;
    case '>': c->p++; t->k = MTK_GT;     return;
    case '=': c->p++; t->k = MTK_ASSIGN; return;
    case '(': c->p++; t->k = MTK_LPAREN; return;
    case ')': c->p++; t->k = MTK_RPAREN; return;
  }
  // Identifier / keyword
  if ((ch>='a' && ch<='z') || (ch>='A' && ch<='Z') || ch=='_') {
    const char *s = c->p;
    while (c->p < c->end && ((*c->p>='a'&&*c->p<='z')||(*c->p>='A'&&*c->p<='Z')||(*c->p>='0'&&*c->p<='9')||*c->p=='_')) c->p++;
    size_t n = (size_t)(c->p - s);
    // lnv0..lnv9
    if (n == 4 && (s[0]=='l'||s[0]=='L') && (s[1]=='n'||s[1]=='N') && (s[2]=='v'||s[2]=='V') && s[3]>='0' && s[3]<='9') {
      t->k = MTK_LNV; t->i = s[3] - '0'; return;
    }
    // keywords
    #define KWMATCH(kw,lit) (n==sizeof(lit)-1 && memcmp(s,lit,sizeof(lit)-1)==0 ? (t->k=kw, 1) : 0)
    if (KWMATCH(MTK_KW_SWITCH,"switch")) return;
    if (KWMATCH(MTK_KW_CASE,  "case"))   return;
    if (KWMATCH(MTK_KW_ENDS,  "ends"))   return;
    if (KWMATCH(MTK_KW_IF,    "if"))     return;
    if (KWMATCH(MTK_KW_ENDIF, "endif"))  return;
    if (KWMATCH(MTK_KW_SML,   "sml"))    return;
    if (KWMATCH(MTK_KW_DELAY, "delay"))  return;
    if (KWMATCH(MTK_KW_SPINM, "spinm"))  return;
    if (KWMATCH(MTK_KW_SPIN,  "spin"))   return;
    if (KWMATCH(MTK_KW_FOR,   "for"))    return;
    if (KWMATCH(MTK_KW_NEXT,  "next"))   return;
    #undef KWMATCH
    // Unknown identifier — carry text in str/slen so the block-level
    // dispatcher can emit an actionable diagnostic ("unknown identifier 'X'",
    // soft-skip for 'print', etc.).
    t->str = s;
    t->slen = (uint16_t)n;
    t->k = MTK_ERR; return;
  }
  // Non-identifier garbage (e.g. `#`, `@`, `{`). Stash the byte in `i` so the
  // dispatcher can say "unexpected '#'" instead of "unknown statement".
  t->i = (int32_t)(uint8_t)ch;
  c->p++; t->k = MTK_ERR;
}

static void tc_mscr_peek(TcMsCompiler *c, TcMsTok *out) {
  if (!c->have_cur) { tc_mscr_lex(c, &c->cur); c->have_cur = 1; }
  *out = c->cur;
}
static void tc_mscr_next(TcMsCompiler *c, TcMsTok *out) {
  tc_mscr_peek(c, out);
  c->have_cur = 0;
}
static void tc_mscr_skip_nls(TcMsCompiler *c) {
  TcMsTok t;
  for (;;) { tc_mscr_peek(c, &t); if (t.k != MTK_NL) break; tc_mscr_next(c, &t); }
}

// ── Bytecode emit helpers ─────────────────────────────────────────
static void tc_mscr_emit(TcMsCompiler *c, uint8_t b) {
  if (c->len < c->cap) c->bc[c->len++] = b;
  else { c->err = 1; }
}
static void tc_mscr_emit_i32(TcMsCompiler *c, int32_t v) {
  tc_mscr_emit(c, (uint8_t)v); tc_mscr_emit(c, (uint8_t)(v>>8));
  tc_mscr_emit(c, (uint8_t)(v>>16)); tc_mscr_emit(c, (uint8_t)(v>>24));
}
static uint16_t tc_mscr_emit_jmp(TcMsCompiler *c, uint8_t op) {
  tc_mscr_emit(c, op);
  uint16_t pos = c->len;
  tc_mscr_emit(c, 0); tc_mscr_emit(c, 0);
  return pos;
}
static void tc_mscr_patch(TcMsCompiler *c, uint16_t at, uint16_t v) {
  if ((uint32_t)at + 1 < c->cap) { c->bc[at] = (uint8_t)v; c->bc[at+1] = (uint8_t)(v>>8); }
}
static void tc_mscr_syntax(TcMsCompiler *c, const char *what) {
  if (!c->err) {
    c->err = 2;
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mscr syntax @line %u: %s"), (unsigned)c->line, what);
  }
}

// Actionable error that quotes the offending identifier, e.g.
//   "TCC: mscr @line 7: 'for'/next loops not supported ('for')"
// Truncates identifier to 23 chars to keep stack small.
static void tc_mscr_err_with_ident(TcMsCompiler *c, const char *msg, const TcMsTok *t) {
  if (c->err) return;
  c->err = 2;
  char buf[24];
  size_t n = (t->slen < sizeof(buf) - 1) ? t->slen : sizeof(buf) - 1;
  if (t->str && n) memcpy(buf, t->str, n);
  buf[n] = 0;
  AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mscr @line %u: %s ('%s')"),
         (unsigned)c->line, msg, buf);
}

// Compare the captured identifier in an MTK_ERR token against a C string literal.
static int tc_mscr_ident_eq(const TcMsTok *t, const char *kw) {
  size_t n = 0;
  while (kw[n]) n++;
  return (t->slen == (uint16_t)n && t->str && memcmp(t->str, kw, n) == 0);
}

// Advance the source pointer past the current line (to the next '\n' or EOF).
// Used when soft-skipping an unsupported-but-safe statement like `print`.
// Does not consume the '\n' — the block loop's skip_nls handles that and
// increments c->line, so diagnostics keep their line numbers accurate.
static void tc_mscr_skip_to_eol(TcMsCompiler *c) {
  while (c->p < c->end && *c->p != '\n') c->p++;
  c->have_cur = 0;   // drop any peeked token, it's stale now
}

// ── Expression parser (recursive-descent, four precedence levels) ─
static void tc_mscr_expr_eq(TcMsCompiler *c);

static void tc_mscr_primary(TcMsCompiler *c) {
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k == MTK_INT)       { tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, t.i); return; }
  if (t.k == MTK_LNV)       { tc_mscr_emit(c, MS_OP_PUSH_LNV); tc_mscr_emit(c, (uint8_t)t.i); return; }
  if (t.k == MTK_MINUS) {                                        // unary -
    tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, 0);
    tc_mscr_primary(c);
    tc_mscr_emit(c, MS_OP_SUB);
    return;
  }
  if (t.k == MTK_LPAREN) {
    tc_mscr_expr_eq(c);
    tc_mscr_next(c, &t);
    if (t.k != MTK_RPAREN) tc_mscr_syntax(c, "expected )");
    return;
  }
  tc_mscr_syntax(c, "expected expression");
}
static void tc_mscr_expr_mul(TcMsCompiler *c) {
  tc_mscr_primary(c);
  for (;;) {
    TcMsTok t; tc_mscr_peek(c, &t);
    uint8_t op;
    if      (t.k == MTK_MUL) op = MS_OP_MUL;
    else if (t.k == MTK_DIV) op = MS_OP_DIV;
    else if (t.k == MTK_MOD) op = MS_OP_MOD;
    else break;
    tc_mscr_next(c, &t);
    tc_mscr_primary(c);
    tc_mscr_emit(c, op);
  }
}
static void tc_mscr_expr_add(TcMsCompiler *c) {
  tc_mscr_expr_mul(c);
  for (;;) {
    TcMsTok t; tc_mscr_peek(c, &t);
    uint8_t op;
    if      (t.k == MTK_PLUS)  op = MS_OP_ADD;
    else if (t.k == MTK_MINUS) op = MS_OP_SUB;
    else break;
    tc_mscr_next(c, &t);
    tc_mscr_expr_mul(c);
    tc_mscr_emit(c, op);
  }
}
static void tc_mscr_expr_cmp(TcMsCompiler *c) {
  tc_mscr_expr_add(c);
  for (;;) {
    TcMsTok t; tc_mscr_peek(c, &t);
    uint8_t op;
    if      (t.k == MTK_LT) op = MS_OP_LT;
    else if (t.k == MTK_LE) op = MS_OP_LE;
    else if (t.k == MTK_GT) op = MS_OP_GT;
    else if (t.k == MTK_GE) op = MS_OP_GE;
    else break;
    tc_mscr_next(c, &t);
    tc_mscr_expr_add(c);
    tc_mscr_emit(c, op);
  }
}
static void tc_mscr_expr_eq(TcMsCompiler *c) {
  tc_mscr_expr_cmp(c);
  for (;;) {
    TcMsTok t; tc_mscr_peek(c, &t);
    uint8_t op;
    if      (t.k == MTK_EQ) op = MS_OP_EQ;
    else if (t.k == MTK_NE) op = MS_OP_NE;
    else break;
    tc_mscr_next(c, &t);
    tc_mscr_expr_cmp(c);
    tc_mscr_emit(c, op);
  }
}

// ── Statement / block parser ──────────────────────────────────────
static void tc_mscr_block(TcMsCompiler *c, uint8_t term1, uint8_t term2);

static void tc_mscr_stmt_assign(TcMsCompiler *c, int lnv_idx, uint8_t opkind) {
  // opkind: MTK_ASSIGN/PLUSEQ/MINUSEQ/MULEQ/DIVEQ
  if (opkind == MTK_ASSIGN) {
    tc_mscr_expr_eq(c);
  } else {
    uint8_t op = MS_OP_ADD;
    switch (opkind) {
      case MTK_PLUSEQ:  op = MS_OP_ADD; break;
      case MTK_MINUSEQ: op = MS_OP_SUB; break;
      case MTK_MULEQ:   op = MS_OP_MUL; break;
      case MTK_DIVEQ:   op = MS_OP_DIV; break;
    }
    tc_mscr_emit(c, MS_OP_PUSH_LNV); tc_mscr_emit(c, (uint8_t)lnv_idx);
    tc_mscr_expr_eq(c);
    tc_mscr_emit(c, op);
  }
  tc_mscr_emit(c, MS_OP_POP_LNV); tc_mscr_emit(c, (uint8_t)lnv_idx);
}

static void tc_mscr_stmt_sml(TcMsCompiler *c) {
  // Already consumed "sml" keyword. Expect "(" int sub  rest ")"
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k != MTK_LPAREN) { tc_mscr_syntax(c, "sml expected ("); return; }
  tc_mscr_expr_eq(c);                              // meter on stack
  TcMsTok ts; tc_mscr_next(c, &ts);
  if (ts.k != MTK_INT) { tc_mscr_syntax(c, "sml subop must be literal int"); return; }
  if (ts.i == 0) {                                 // sml(m, 0, baud) → SetBaud
    tc_mscr_expr_eq(c);                            // baud on stack
    tc_mscr_next(c, &t);
    if (t.k != MTK_RPAREN) { tc_mscr_syntax(c, "sml expected )"); return; }
    tc_mscr_emit(c, MS_OP_SML_BAUD);
  } else if (ts.i == 1) {                          // sml(m, 1, "HEX..") → SML_Write hex
    tc_mscr_next(c, &t);
    if (t.k != MTK_HEXSTR) { tc_mscr_syntax(c, "sml(.,1,..) needs \"…\" literal"); return; }
    if (t.slen == 0 || t.slen > TC_MSCR_HEX_MAX*2) {
      tc_mscr_syntax(c, "sml string empty or too long"); return;
    }
    TcMsTok tc2; tc_mscr_next(c, &tc2);
    if (tc2.k != MTK_RPAREN) { tc_mscr_syntax(c, "sml expected )"); return; }
    // encoding: [op][len_lo][len_hi][chars…]
    // Note: string contents are not hex-validated at compile time. SML_Write
    // itself parses the payload: positive meter = hex bytes; negative meter
    // = "BAUD:8X1" serial reconfigure (M-Bus mid-stream parity switch).
    tc_mscr_emit(c, MS_OP_SML_HEX);
    tc_mscr_emit(c, (uint8_t)(t.slen & 0xFF));
    tc_mscr_emit(c, (uint8_t)(t.slen >> 8));
    for (uint16_t k = 0; k < t.slen; k++) tc_mscr_emit(c, (uint8_t)t.str[k]);
  } else {
    tc_mscr_syntax(c, "sml subop must be 0 or 1"); return;
  }
}

static void tc_mscr_stmt_delay(TcMsCompiler *c) {
  // "delay" already consumed. Expect: ( <expr> )
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k != MTK_LPAREN) { tc_mscr_syntax(c, "delay expected ("); return; }
  tc_mscr_expr_eq(c);                              // ms on stack
  tc_mscr_next(c, &t);
  if (t.k != MTK_RPAREN) { tc_mscr_syntax(c, "delay expected )"); return; }
  tc_mscr_emit(c, MS_OP_DELAY);
}

// Shared parser for spin(pin, val) / spinm(pin, mode). Both forms take two
// int expressions; op selects digitalWrite vs pinMode at runtime.
static void tc_mscr_stmt_spin_like(TcMsCompiler *c, uint8_t op) {
  // "spin" or "spinm" already consumed. Expect: ( <expr> , <expr> )
  // (commas are lexer whitespace → `spin(pin val)` also accepted.)
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k != MTK_LPAREN) { tc_mscr_syntax(c, "spin expected ("); return; }
  tc_mscr_expr_eq(c);                              // pin on stack
  tc_mscr_expr_eq(c);                              // val/mode on stack
  tc_mscr_next(c, &t);
  if (t.k != MTK_RPAREN) { tc_mscr_syntax(c, "spin expected )"); return; }
  tc_mscr_emit(c, op);
}

static void tc_mscr_stmt_switch(TcMsCompiler *c) {
  // "switch" already consumed. Expect: <lnv> NL  [case <int> NL block]+ ends
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k != MTK_LNV) { tc_mscr_syntax(c, "switch needs lnvN"); return; }
  uint8_t var = (uint8_t)t.i;
  // Expect newline
  TcMsTok nl; tc_mscr_next(c, &nl);
  if (nl.k != MTK_NL && nl.k != MTK_END) { tc_mscr_syntax(c, "switch: expected newline"); return; }
  tc_mscr_skip_nls(c);
  uint16_t end_jumps[16]; uint8_t end_jumps_n = 0;
  for (;;) {
    TcMsTok kw; tc_mscr_peek(c, &kw);
    if (kw.k == MTK_KW_ENDS) { tc_mscr_next(c, &kw); break; }
    if (kw.k == MTK_END)     { tc_mscr_syntax(c, "switch: missing ends"); return; }
    if (kw.k != MTK_KW_CASE) { tc_mscr_syntax(c, "switch: expected case"); return; }
    tc_mscr_next(c, &kw);
    TcMsTok ci; tc_mscr_next(c, &ci);
    if (ci.k != MTK_INT) { tc_mscr_syntax(c, "case needs int"); return; }
    TcMsTok cnl; tc_mscr_next(c, &cnl);
    if (cnl.k != MTK_NL && cnl.k != MTK_END) { tc_mscr_syntax(c, "case: expected newline"); return; }
    tc_mscr_skip_nls(c);
    // emit:  PUSH_LNV var; PUSH_IMM ci.i; EQ; JZ next_case
    tc_mscr_emit(c, MS_OP_PUSH_LNV); tc_mscr_emit(c, var);
    tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, ci.i);
    tc_mscr_emit(c, MS_OP_EQ);
    uint16_t jz_pos = tc_mscr_emit_jmp(c, MS_OP_JZ);
    // body until next case or ends
    tc_mscr_block(c, MTK_KW_CASE, MTK_KW_ENDS);
    // jump to end-of-switch
    if (end_jumps_n < 16) {
      end_jumps[end_jumps_n++] = tc_mscr_emit_jmp(c, MS_OP_JMP);
    } else {
      tc_mscr_syntax(c, "switch too many cases"); return;
    }
    tc_mscr_patch(c, jz_pos, c->len);
  }
  // patch all end-of-switch jumps
  for (uint8_t i = 0; i < end_jumps_n; i++) tc_mscr_patch(c, end_jumps[i], c->len);
}

static void tc_mscr_stmt_if(TcMsCompiler *c) {
  // "if" already consumed. Expect: <expr> NL  body  endif
  tc_mscr_expr_eq(c);
  TcMsTok nl; tc_mscr_next(c, &nl);
  if (nl.k != MTK_NL && nl.k != MTK_END) { tc_mscr_syntax(c, "if: expected newline"); return; }
  tc_mscr_skip_nls(c);
  uint16_t jz_pos = tc_mscr_emit_jmp(c, MS_OP_JZ);
  tc_mscr_block(c, MTK_KW_ENDIF, MTK_KW_ENDIF);
  TcMsTok endif; tc_mscr_next(c, &endif);
  if (endif.k != MTK_KW_ENDIF) { tc_mscr_syntax(c, "if: missing endif"); return; }
  tc_mscr_patch(c, jz_pos, c->len);
}

// Consume an optional unary '-' then an int literal. Returns 1 on success,
// 0 on parse failure (calls tc_mscr_syntax with `ctx` on failure).
static int tc_mscr_read_signed_int(TcMsCompiler *c, int32_t *out, const char *ctx) {
  TcMsTok t; tc_mscr_next(c, &t);
  int negate = 0;
  if (t.k == MTK_MINUS) { negate = 1; tc_mscr_next(c, &t); }
  if (t.k != MTK_INT) { tc_mscr_syntax(c, ctx); return 0; }
  *out = negate ? -t.i : t.i;
  return 1;
}

// for <lnv>  <start>  <end>  <step>
//   <body>
// next
//
// Scripter-compatible integer range loop. Bounds and step must be signed
// int literals so the direction test (LE vs GE) is decidable at compile
// time and we don't need a separate backward-loop opcode.
//
// Compiles to (reusing existing opcodes, no new VM additions):
//     PUSH_IMM start; POP_LNV var          ; init
//   LOOP:
//     PUSH_LNV var; PUSH_IMM end; (LE|GE)
//     JZ DONE                              ; falls through if in range
//     <body>                               ; recursive block, term=MTK_KW_NEXT
//     PUSH_LNV var; PUSH_IMM step; ADD; POP_LNV var
//     JMP LOOP
//   DONE:
//
// Typical cost vs. unrolling: a 53-iteration M-Bus wake preamble
// (`for lnv1 1 53 1 ; sml(1 1 "55…") ; next`) compiles to ~60 B of bytecode
// versus ~1100 B when the loop is manually decomposed — the difference
// between fitting inside TC_MSCR_BC_MAX (512 B) and silently overflowing.
static void tc_mscr_stmt_for(TcMsCompiler *c) {
  // "for" already consumed.
  TcMsTok t; tc_mscr_next(c, &t);
  if (t.k != MTK_LNV) { tc_mscr_syntax(c, "for needs lnvN"); return; }
  uint8_t var = (uint8_t)t.i;
  int32_t start, end, step;
  if (!tc_mscr_read_signed_int(c, &start, "for: start must be int literal")) return;
  if (!tc_mscr_read_signed_int(c, &end,   "for: end must be int literal"))   return;
  if (!tc_mscr_read_signed_int(c, &step,  "for: step must be int literal"))  return;
  if (step == 0) { tc_mscr_syntax(c, "for: step must not be 0"); return; }
  TcMsTok nl; tc_mscr_next(c, &nl);
  if (nl.k != MTK_NL && nl.k != MTK_END) { tc_mscr_syntax(c, "for: expected newline"); return; }
  tc_mscr_skip_nls(c);

  // init: var = start
  tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, start);
  tc_mscr_emit(c, MS_OP_POP_LNV);  tc_mscr_emit(c, var);

  // loop top: PUSH var; PUSH end; (LE|GE); JZ done
  uint16_t loop_top = c->len;
  tc_mscr_emit(c, MS_OP_PUSH_LNV); tc_mscr_emit(c, var);
  tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, end);
  tc_mscr_emit(c, step > 0 ? MS_OP_LE : MS_OP_GE);
  uint16_t done_jz = tc_mscr_emit_jmp(c, MS_OP_JZ);

  // body (block reads until `next`)
  tc_mscr_block(c, MTK_KW_NEXT, MTK_KW_NEXT);
  TcMsTok nx; tc_mscr_next(c, &nx);
  if (nx.k != MTK_KW_NEXT) { tc_mscr_syntax(c, "for: missing next"); return; }

  // increment: var = var + step
  tc_mscr_emit(c, MS_OP_PUSH_LNV); tc_mscr_emit(c, var);
  tc_mscr_emit(c, MS_OP_PUSH_IMM); tc_mscr_emit_i32(c, step);
  tc_mscr_emit(c, MS_OP_ADD);
  tc_mscr_emit(c, MS_OP_POP_LNV);  tc_mscr_emit(c, var);

  // unconditional back-jump to loop_top (MS_OP_JMP is [op][u16 LE abs])
  tc_mscr_emit(c, MS_OP_JMP);
  tc_mscr_emit(c, (uint8_t)(loop_top & 0xFF));
  tc_mscr_emit(c, (uint8_t)(loop_top >> 8));

  // patch exit-jump target to here
  tc_mscr_patch(c, done_jz, c->len);
}

static void tc_mscr_block(TcMsCompiler *c, uint8_t term1, uint8_t term2) {
  for (;;) {
    if (c->err) return;
    tc_mscr_skip_nls(c);
    TcMsTok t; tc_mscr_peek(c, &t);
    if (t.k == MTK_END) return;
    if (t.k == term1 || t.k == term2) return;
    if (t.k == MTK_LNV) {
      tc_mscr_next(c, &t);
      int v = t.i;
      TcMsTok op; tc_mscr_next(c, &op);
      if (op.k != MTK_ASSIGN && op.k != MTK_PLUSEQ && op.k != MTK_MINUSEQ &&
          op.k != MTK_MULEQ  && op.k != MTK_DIVEQ) {
        tc_mscr_syntax(c, "expected = += -= *= /="); return;
      }
      tc_mscr_stmt_assign(c, v, op.k);
    } else if (t.k == MTK_KW_SML) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_sml(c);
    } else if (t.k == MTK_KW_DELAY) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_delay(c);
    } else if (t.k == MTK_KW_SPIN) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_spin_like(c, MS_OP_SPIN);
    } else if (t.k == MTK_KW_SPINM) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_spin_like(c, MS_OP_SPINM);
    } else if (t.k == MTK_KW_SWITCH) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_switch(c);
    } else if (t.k == MTK_KW_IF) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_if(c);
    } else if (t.k == MTK_KW_FOR) {
      tc_mscr_next(c, &t);
      tc_mscr_stmt_for(c);
    } else if (t.k == MTK_ERR && t.slen > 0) {
      // Unknown identifier — check for known-unsupported Scripter constructs
      // and give actionable hints so users know which rewrite to apply.
      // ── Soft-skip: `print …` is diagnostic output only, always safe to drop.
      if (tc_mscr_ident_eq(&t, "print")) {
        tc_mscr_next(c, &t);
        tc_mscr_skip_to_eol(c);
        AddLog(LOG_LEVEL_INFO,
               PSTR("TCC: mscr @line %u: 'print' line dropped (diagnostic only)"),
               (unsigned)c->line);
        continue;  // retry at next statement
      }
      // ── Hard failures with rewrite hints (see tinyc/examples/sml/README.md).
      // NOTE: `for`/`next` are real keywords since v1.3.16 — they lex as
      // MTK_KW_FOR/MTK_KW_NEXT and never reach this branch. No diagnostic
      // needed.
      if (tc_mscr_ident_eq(&t, "mins")   || tc_mscr_ident_eq(&t, "secs") ||
          tc_mscr_ident_eq(&t, "hours")  || tc_mscr_ident_eq(&t, "upsecs") ||
          tc_mscr_ident_eq(&t, "upmins") || tc_mscr_ident_eq(&t, "tstamp") ||
          tc_mscr_ident_eq(&t, "time")) {
        tc_mscr_err_with_ident(c,
          "builtin time variable not supported — use a local lnv tick counter", &t);
        return;
      }
      if (tc_mscr_ident_eq(&t, "sb") || tc_mscr_ident_eq(&t, "st") ||
          tc_mscr_ident_eq(&t, "sl")) {
        tc_mscr_err_with_ident(c,
          "string slicing not supported in mini-scripter", &t);
        return;
      }
      // Generic: name the identifier so the user knows what to look for.
      tc_mscr_err_with_ident(c, "unknown identifier", &t);
      return;
    } else if (t.k == MTK_ERR && t.slen == 0) {
      // Bad character (not an identifier). Most common case: `#` at line start
      // of a `#subname` subroutine definition.
      if (t.i == '#') {
        tc_mscr_syntax(c,
          "'#' found — =# subroutines not supported, inline the body at call site");
      } else if (t.i >= 32 && t.i < 127) {
        char msg[40];
        snprintf(msg, sizeof(msg), "unexpected '%c'", (char)t.i);
        tc_mscr_syntax(c, msg);
      } else {
        tc_mscr_syntax(c, "unexpected byte in source");
      }
      return;
    } else if (t.k == MTK_ASSIGN) {
      // Bare `=` at statement start (no lnv in front) — almost certainly a
      // Scripter `=#subname` call site, which we don't support.
      tc_mscr_syntax(c,
        "'=' at statement start — =#subroutine calls not supported, inline the body");
      return;
    } else {
      tc_mscr_syntax(c, "unknown statement"); return;
    }
    // consume statement-terminating newline (one or more)
    tc_mscr_skip_nls(c);
  }
}

static int tc_mscr_compile(const char *src, size_t src_len, uint8_t *bc, uint16_t cap, uint16_t *out_len) {
  TcMsCompiler c;
  memset(&c, 0, sizeof(c));
  c.p = src; c.end = src + src_len;
  c.bc = bc; c.cap = cap;
  c.line = 1;
  tc_mscr_block(&c, MTK_END, MTK_END);
  if (c.err) { *out_len = 0; return -1; }
  tc_mscr_emit(&c, MS_OP_END);
  if (c.err) { *out_len = 0; return -1; }
  *out_len = c.len;
  return 0;
}

// ── VM (stack interpreter) ────────────────────────────────────────
static int tc_mscr_hexnibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void tc_mscr_exec(uint8_t *bc, uint16_t bc_len, int32_t *lnv) {
  if (!bc || !bc_len) return;
  int32_t stk[TC_MSCR_STK_MAX];
  int sp = 0;
  uint16_t ip = 0;
  uint32_t guard = 0;
  while (ip < bc_len && guard++ < 4096) {
    uint8_t op = bc[ip++];
    switch (op) {
      case MS_OP_END: return;
      case MS_OP_PUSH_LNV: {
        if (ip >= bc_len) return;
        uint8_t v = bc[ip++];
        if (sp >= TC_MSCR_STK_MAX || v >= TC_MSCR_LNV_COUNT) return;
        stk[sp++] = lnv[v];
        break;
      }
      case MS_OP_PUSH_IMM: {
        if (ip + 4 > bc_len) return;
        int32_t v = (int32_t)(bc[ip] | (bc[ip+1]<<8) | (bc[ip+2]<<16) | (bc[ip+3]<<24));
        ip += 4;
        if (sp >= TC_MSCR_STK_MAX) return;
        stk[sp++] = v;
        break;
      }
      case MS_OP_POP_LNV: {
        if (ip >= bc_len || sp <= 0) return;
        uint8_t v = bc[ip++];
        if (v >= TC_MSCR_LNV_COUNT) return;
        lnv[v] = stk[--sp];
        break;
      }
      case MS_OP_ADD: case MS_OP_SUB: case MS_OP_MUL: case MS_OP_DIV: case MS_OP_MOD:
      case MS_OP_LT:  case MS_OP_LE:  case MS_OP_GT:  case MS_OP_GE:  case MS_OP_EQ: case MS_OP_NE: {
        if (sp < 2) return;
        int32_t b = stk[--sp];
        int32_t a = stk[--sp];
        int32_t r = 0;
        switch (op) {
          case MS_OP_ADD: r = a + b; break;
          case MS_OP_SUB: r = a - b; break;
          case MS_OP_MUL: r = a * b; break;
          case MS_OP_DIV: r = (b==0) ? 0 : a / b; break;
          case MS_OP_MOD: r = (b==0) ? 0 : a % b; break;
          case MS_OP_LT:  r = (a <  b); break;
          case MS_OP_LE:  r = (a <= b); break;
          case MS_OP_GT:  r = (a >  b); break;
          case MS_OP_GE:  r = (a >= b); break;
          case MS_OP_EQ:  r = (a == b); break;
          case MS_OP_NE:  r = (a != b); break;
        }
        stk[sp++] = r;
        break;
      }
      case MS_OP_JMP: {
        if (ip + 2 > bc_len) return;
        uint16_t tgt = (uint16_t)(bc[ip] | (bc[ip+1]<<8));
        ip = tgt;
        break;
      }
      case MS_OP_JZ: {
        if (ip + 2 > bc_len || sp <= 0) return;
        uint16_t tgt = (uint16_t)(bc[ip] | (bc[ip+1]<<8));
        ip += 2;
        int32_t v = stk[--sp];
        if (!v) ip = tgt;
        break;
      }
      case MS_OP_SML_BAUD: {
        if (sp < 2) return;
        int32_t baud  = stk[--sp];
        int32_t meter = stk[--sp];
#if defined(USE_SML_M) || defined(USE_SML)
        SML_SetBaud((uint32_t)meter, (uint32_t)baud);
#else
        // SML driver not compiled in — consume args, no-op. Lets pre-compiled
        // .tas bytecode load without crashing the VM, but the meter call is
        // silently dropped.
        (void)meter; (void)baud;
#endif
        break;
      }
      case MS_OP_SML_HEX: {
        if (ip + 2 > bc_len || sp < 1) return;
        uint16_t hlen = (uint16_t)(bc[ip] | (bc[ip+1] << 8));
        ip += 2;
        if (ip + hlen > bc_len) return;
        // Build a NUL-terminated string for SML_Write. Positive meter → hex
        // bytes; negative meter → "BAUD:8X1" serial-reconfigure (parsed by
        // SML_Write's flag<0 path).
        char hbuf[TC_MSCR_HEX_MAX*2 + 4];
        if (hlen >= sizeof(hbuf)) return;
        memcpy(hbuf, bc + ip, hlen);
        hbuf[hlen] = 0;
        ip += hlen;
        int32_t meter = stk[--sp];
#if defined(USE_SML_M) || defined(USE_SML)
        SML_Write((int32_t)meter, hbuf);
#else
        // SML driver not compiled in — consume args, no-op (see MS_OP_SML_BAUD).
        (void)meter; (void)hbuf;
#endif
        break;
      }
      case MS_OP_DELAY: {
        if (sp < 1) return;
        int32_t ms = stk[--sp];
        if (ms < 0) ms = 0;
        if (ms > TC_MSCR_DELAY_CAP) ms = TC_MSCR_DELAY_CAP;
        if (ms > 0) delay((uint32_t)ms);
        break;
      }
      case MS_OP_SPIN: {
        // spin(pin, val) → digitalWrite(pin, val ? HIGH : LOW)
        if (sp < 2) return;
        int32_t val = stk[--sp];
        int32_t pin = stk[--sp];
        if (pin >= 0 && pin < MAX_GPIO_PIN) {
          digitalWrite((uint8_t)pin, val ? HIGH : LOW);
        }
        break;
      }
      case MS_OP_SPINM: {
        // spinm(pin, mode) → pinMode(pin, mode ? OUTPUT : INPUT)
        // Tasmota Scripter semantics: 1 = OUTPUT, 0 = INPUT.
        if (sp < 2) return;
        int32_t mode = stk[--sp];
        int32_t pin  = stk[--sp];
        if (pin >= 0 && pin < MAX_GPIO_PIN) {
          pinMode((uint8_t)pin, mode ? OUTPUT : INPUT);
        }
        break;
      }
      default: return;                              // unknown opcode → bail
    }
  }
}

// ── Tick dispatcher (called from tc_all_callbacks_id) ─────────────
static inline void tc_mscr_tick_f(void) {
  if (tc_mscr.loaded && tc_mscr.has_f) tc_mscr_exec(tc_mscr.bc_f, tc_mscr.bc_f_len, tc_mscr.lnv);
}
static inline void tc_mscr_tick_s(void) {
  if (tc_mscr.loaded && tc_mscr.has_s) tc_mscr_exec(tc_mscr.bc_s, tc_mscr.bc_s_len, tc_mscr.lnv);
}

// ── Section extractor ─────────────────────────────────────────────
// Find the first line beginning with `marker` (e.g. ">F"). Returns pointer to
// the line AFTER the marker (i.e. start of body), and writes the body length up
// to the next ">X" line or end-of-buffer to *out_len. Returns NULL if not found.
static const char *tc_mscr_find_section(const char *buf, size_t buf_len, const char *marker, size_t *out_len) {
  size_t mlen = strlen(marker);
  const char *p = buf;
  const char *end = buf + buf_len;
  while (p < end) {
    // Is this line the marker?
    size_t rem = (size_t)(end - p);
    if (rem >= mlen && memcmp(p, marker, mlen) == 0) {
      // Verify it's followed by NL/space/EOL (so ">F" doesn't match ">FOO")
      char nx = (p + mlen < end) ? p[mlen] : '\n';
      if (nx == '\n' || nx == '\r' || nx == ' ' || nx == '\t') {
        // skip to next line
        const char *body = (const char*)memchr(p, '\n', rem);
        body = body ? body + 1 : end;
        // body extends until next line starting with '>' or EOF
        const char *q = body;
        while (q < end) {
          const char *eol = (const char*)memchr(q, '\n', (size_t)(end - q));
          const char *next_line_start = eol ? eol + 1 : end;
          if (next_line_start < end && *next_line_start == '>') {
            *out_len = (size_t)(next_line_start - body);
            return body;
          }
          q = next_line_start;
        }
        *out_len = (size_t)(end - body);
        return body;
      }
    }
    // advance to next line
    const char *eol = (const char*)memchr(p, '\n', rem);
    p = eol ? eol + 1 : end;
  }
  *out_len = 0;
  return NULL;
}

// Public load entry — read file at `path`, extract >F/>S, compile both. Returns:
//   number of sections successfully compiled (0..2), -1 = error (open/size).
static int tc_mscr_load(const char *path) {
#ifdef USE_UFILESYS
  memset(&tc_mscr, 0, sizeof(tc_mscr));
  char fpath[128]; strlcpy(fpath, path, sizeof(fpath));
  FS *fsp = tc_file_path(fpath);
  if (!fsp) return -1;
  File f = fsp->open(fpath, "r");
  if (!f) return -1;
  size_t fsize = (size_t)f.size();
  if (fsize == 0 || fsize > 8192) { f.close(); return -1; }
  char *buf = (char*)malloc(fsize + 1);
  if (!buf) { f.close(); return -1; }
  size_t got = (size_t)f.read((uint8_t*)buf, fsize);
  buf[got] = 0;
  f.close();
  int ok = 0;
  size_t sec_len = 0;
  const char *body;
  body = tc_mscr_find_section(buf, got, ">F", &sec_len);
  if (body) {
    if (tc_mscr_compile(body, sec_len, tc_mscr.bc_f, TC_MSCR_BC_MAX, &tc_mscr.bc_f_len) == 0) {
      tc_mscr.has_f = 1; ok++;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: mscr compiled >F %u src→%u bc"), (unsigned)sec_len, (unsigned)tc_mscr.bc_f_len);
    } else {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mscr >F compile failed"));
    }
  }
  body = tc_mscr_find_section(buf, got, ">S", &sec_len);
  if (body) {
    if (tc_mscr_compile(body, sec_len, tc_mscr.bc_s, TC_MSCR_BC_MAX, &tc_mscr.bc_s_len) == 0) {
      tc_mscr.has_s = 1; ok++;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: mscr compiled >S %u src→%u bc"), (unsigned)sec_len, (unsigned)tc_mscr.bc_s_len);
    } else {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: mscr >S compile failed"));
    }
  }
  free(buf);
  if (ok > 0) tc_mscr.loaded = 1;
  return ok;
#else
  (void)path;
  return -1;
#endif
}

/*********************************************************************************************\
 * Cross-VM shared key/value store
 *
 * A small driver-global table (mutex-protected on ESP32) that lets multiple
 * TinyC slots share named scalars/strings — useful when a single program
 * outgrows TC_MAX_PROGRAM and is split across two or more slots.
 *
 *   shareSetInt("key", v);   int   v = shareGetInt("key");
 *   shareSetFloat("key", f); float f = shareGetFloat("key");
 *   shareSetStr("key", buf); shareGetStr("key", outBuf);
 *   shareHas("key");         shareDelete("key");
 *
 * Keys are string literals (compile-time constants). A get on a missing key
 * returns 0 / 0.0 / "" — never errors — so reader code stays simple.
 * Caps: TC_SHARE_MAX entries × (TC_SHARE_KEY_LEN+1 key + TC_SHARE_STR_LEN+1
 * value) ≈ 32 × 81 B ≈ 2.6 KB worst case. Storage is not persisted across
 * reboots; if a script needs persistence it can write its own pvars.
\*********************************************************************************************/

#ifndef TC_SHARE_MAX
  #define TC_SHARE_MAX     32     // total named entries across all slots
#endif
#ifndef TC_SHARE_KEY_LEN
  #define TC_SHARE_KEY_LEN 16     // max key length (excluding NUL)
#endif
#ifndef TC_SHARE_STR_LEN
  #define TC_SHARE_STR_LEN 64     // max string-value length (excluding NUL)
#endif

#define TC_SHARE_TYPE_NONE 0
#define TC_SHARE_TYPE_INT  1
#define TC_SHARE_TYPE_FLT  2
#define TC_SHARE_TYPE_STR  3

typedef struct TcShareEntry {
  char     key[TC_SHARE_KEY_LEN + 1];
  uint8_t  type;                                // TC_SHARE_TYPE_*
  union { int32_t i; float f; } v;
  char     s[TC_SHARE_STR_LEN + 1];             // populated only when type == STR
} TcShareEntry;

static TcShareEntry tc_share_table[TC_SHARE_MAX] = { };

#ifdef ESP32
static SemaphoreHandle_t tc_share_mutex = nullptr;
static inline void tc_share_lock(void) {
  if (!tc_share_mutex) tc_share_mutex = xSemaphoreCreateMutex();
  if (tc_share_mutex)  xSemaphoreTake(tc_share_mutex, portMAX_DELAY);
}
static inline void tc_share_unlock(void) {
  if (tc_share_mutex) xSemaphoreGive(tc_share_mutex);
}
#else
static inline void tc_share_lock(void)   {}
static inline void tc_share_unlock(void) {}
#endif

// Returns table index for `key`, or -1 if not present. Caller holds lock.
static int tc_share_find(const char *key) {
  if (!key || !*key) return -1;
  for (int i = 0; i < TC_SHARE_MAX; i++) {
    if (tc_share_table[i].type != TC_SHARE_TYPE_NONE &&
        strncmp(tc_share_table[i].key, key, TC_SHARE_KEY_LEN) == 0) {
      return i;
    }
  }
  return -1;
}

// Returns existing slot for `key`, or first free slot (claimed by writing the
// key); -1 if the table is full. Caller holds lock.
static int tc_share_find_or_alloc(const char *key) {
  int idx = tc_share_find(key);
  if (idx >= 0) return idx;
  for (int i = 0; i < TC_SHARE_MAX; i++) {
    if (tc_share_table[i].type == TC_SHARE_TYPE_NONE) {
      strlcpy(tc_share_table[i].key, key, sizeof(tc_share_table[i].key));
      tc_share_table[i].s[0] = '\0';
      return i;
    }
  }
  return -1;
}

/*********************************************************************************************\
 * VM: Syscall dispatch
\*********************************************************************************************/

static int tc_syscall(TcVM *vm, uint16_t id) {
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
    case SYS_VM_STACK_DEPTH: {
      // Diagnostic: report current operand-stack depth *before* this syscall's return push.
      // Useful for detecting SP leaks in user scripts / callback chains.
      int32_t depth = (int32_t)vm->sp;
      TC_PUSH(vm, depth);
      break;
    }
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
      // serialBegin(rxpin, txpin, baud, config, bufsize) -> int (handle 0..2, or -1 on error)
      int32_t bufsize = TC_POP(vm);
      int32_t config  = TC_POP(vm);
      int32_t baud    = TC_POP(vm);
      int32_t txpin   = TC_POP(vm);
      int32_t rxpin   = TC_POP(vm);
      // find a free slot
      int slot = -1;
      for (int i = 0; i < TC_MAX_SERIAL_PORTS; i++) {
        if (!tc_serial_ports[i]) { slot = i; break; }
      }
      if (slot < 0) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: serialBegin — all %d ports in use"), TC_MAX_SERIAL_PORTS);
        TC_PUSH(vm, -1);
        break;
      }
      if (bufsize < 64) bufsize = 64;
      if (bufsize > 2048) bufsize = 2048;
      if (config < 0 || config > 23) config = 3;  // default 8N1
#ifdef ESP32
      if (Is_gpio_used(rxpin) || Is_gpio_used(txpin)) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial warning — pins %d/%d may be in use"), rxpin, txpin);
      }
#endif
      tc_serial_ports[slot] = new TasmotaSerial(rxpin, txpin, HARDWARE_FALLBACK, 0, bufsize);
      if (tc_serial_ports[slot]) {
        if (tc_serial_ports[slot]->begin(baud, ConvertSerialConfig(config))) {
          if (tc_serial_ports[slot]->hardwareSerial()) {
            ClaimSerial();
          }
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: serial[%d] opened rx=%d tx=%d baud=%d cfg=%d buf=%d hw=%d"),
                 slot, rxpin, txpin, baud, config, bufsize, tc_serial_ports[slot]->hardwareSerial());
          TC_PUSH(vm, slot);
        } else {
          delete tc_serial_ports[slot];
          tc_serial_ports[slot] = nullptr;
          AddLog(LOG_LEVEL_ERROR, PSTR("TCC: serial begin failed"));
          TC_PUSH(vm, -1);
        }
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: serial alloc failed"));
        TC_PUSH(vm, -1);
      }
      break;
    }
    case SYS_SERIAL_CLOSE: {
      a = TC_POP(vm);  // handle
      tc_serial_close(a);
      break;
    }
    case SYS_SERIAL_PRINT: {
      a = TC_POP(vm);  // const_id
      b = TC_POP(vm);  // handle
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        const char *s = vm->constants[a].str.ptr;
        TasmotaSerial *p = tc_serial_get(b);
        if (p) { p->write(s, strlen(s)); } else { tc_output_string(s); }
      }
      break;
    }
    case SYS_SERIAL_PRINT_INT: {
      a = TC_POP(vm);  // val
      b = TC_POP(vm);  // handle
      char ibuf[16];
      itoa(a, ibuf, 10);
      TasmotaSerial *p = tc_serial_get(b);
      if (p) { p->write(ibuf, strlen(ibuf)); } else { tc_output_int(a); }
      break;
    }
    case SYS_SERIAL_PRINT_FLT: {
      fa = TC_POPF(vm);  // val
      b  = TC_POP(vm);   // handle
      char fbuf[24];
      dtostrf(fa, 1, 2, fbuf);
      TasmotaSerial *p = tc_serial_get(b);
      if (p) { p->write(fbuf, strlen(fbuf)); } else { tc_output_float(fa); }
      break;
    }
    case SYS_SERIAL_PRINTLN: {
      a = TC_POP(vm);  // const_id
      b = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(b);
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        const char *s = vm->constants[a].str.ptr;
        if (p) { p->write(s, strlen(s)); p->write("\r\n", 2); }
        else   { tc_output_string(s); tc_output_char('\n'); }
      } else {
        if (p) { p->write("\r\n", 2); } else { tc_output_char('\n'); }
      }
      break;
    }
    case SYS_SERIAL_READ: {
      a = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(a);
      TC_PUSH(vm, (p && p->available()) ? p->read() : -1);
      break;
    }
    case SYS_SERIAL_AVAILABLE: {
      a = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(a);
      TC_PUSH(vm, p ? p->available() : 0);
      break;
    }
    case SYS_SERIAL_WRITE_BYTE: {
      a = TC_POP(vm);  // byte
      b = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(b);
      if (p) p->write((uint8_t)(a & 0xFF));
      break;
    }
    case SYS_SERIAL_WRITE_STR: {
      a = TC_POP(vm);  // buf_ref
      b = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(b);
      if (p) {
        char tbuf[256];
        int len = tc_ref_to_cstr(vm, a, tbuf, sizeof(tbuf));
        if (len > 0) p->write(tbuf, len);
      }
      break;
    }
    case SYS_SERIAL_WRITE_BUF: {
      // serialWriteBytes(h, buf_ref, len) — write exactly len bytes (binary safe)
      b = TC_POP(vm);  // len
      a = TC_POP(vm);  // buf_ref
      int hh = TC_POP(vm);  // handle
      TasmotaSerial *p = tc_serial_get(hh);
      if (p && b > 0 && b <= 256) {
        int32_t *buf = tc_resolve_ref(vm, a);
        if (buf) {
          int32_t maxLen = tc_ref_maxlen(vm, a);
          if (b > maxLen) b = maxLen;
          uint8_t tbuf[256];
          for (int i = 0; i < b; i++) tbuf[i] = (uint8_t)(buf[i] & 0xFF);
          p->write(tbuf, b);
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
    case SYS_MATH_POW: {
      float fb = TC_POPF(vm); fa = TC_POPF(vm); TC_PUSHF(vm, powf(fa, fb)); break;
    }
    case SYS_MATH_ACOS:
      fa = TC_POPF(vm); TC_PUSHF(vm, acosf(fa)); break;
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
      if (!dst) break;
      int32_t max = tc_ref_maxlen(vm, dst_ref) - 1;
      // Source may be a const-pool string ref (e.g. string literal passed
      // through a function param). Detect and use char* path; otherwise
      // treat as an int32 VM array.
      if (tc_is_const_ref(src_ref)) {
        uint16_t ci = (uint16_t)(((uint32_t)src_ref) & 0x7FFF);
        if (ci < vm->const_count && vm->constants[ci].type == 1) {
          const char *s = vm->constants[ci].str.ptr;
          int32_t i = 0;
          while (s && s[i] != 0 && i < max) { dst[i] = (int32_t)(uint8_t)s[i]; i++; }
          dst[i] = 0;
        } else {
          dst[0] = 0;
        }
      } else {
        int32_t *src = tc_resolve_ref(vm, src_ref);
        if (src) {
          int32_t i = 0;
          while (src[i] != 0 && i < max) { dst[i] = src[i]; i++; }
          dst[i] = 0;
        }
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
    case SYS_STRCMP_CONST: {
      // strcmp(char[] arr, "literal") — arr_ref pushed first, const_idx second
      int32_t ci = TC_POP(vm);
      int32_t a_ref = TC_POP(vm);
      int32_t *pa = tc_resolve_ref(vm, a_ref);
      int32_t result = 0;
      if (pa && ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *pb = vm->constants[ci].str.ptr;
        int32_t max_a = tc_ref_maxlen(vm, a_ref);
        int32_t i = 0;
        while (pa[i] != 0 && pb[i] != 0 && pa[i] == (int32_t)(uint8_t)pb[i] && i < max_a) i++;
        int32_t bv = (i < max_a) ? (int32_t)(uint8_t)pb[i] : 0;
        result = pa[i] - bv;
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
    //
    // Buffer sizing: the variadic-sprintf compile-time splitter can hand
    // segments with arbitrarily-long literal prefixes to these syscalls
    // (e.g. webSend-style HTML strings of 60–200 chars). A 64-byte tmp[]
    // would silently truncate the value at the end. TC_SPRINTF_TMP_SIZE
    // is the working buffer used by snprintf() for one segment — at 256 B
    // it covers every realistic single-format emission and still costs
    // <0.5 KB of stack on ESP32 task workers (4–16 KB total). ESP8266 is
    // not affected — it does not run the variadic path on long literals.
    case SYS_SPRINTF_INT: {
      int32_t val = TC_POP(vm);          // int argument
      int32_t ci  = TC_POP(vm);          // format string const index
      int32_t dst_ref = TC_POP(vm);      // destination array ref
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      const char *fmt = tc_get_const_str(vm, ci);
      if (!dst || !fmt) { TC_PUSH(vm, -1); break; }
      int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
      char tmp[256];
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
      char tmp[256];
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
      char srcbuf[256];
      int32_t si = 0;
      while (src[si] != 0 && si < srcMax && si < (int32_t)sizeof(srcbuf) - 1) {
        srcbuf[si] = (char)(src[si] & 0xFF);
        si++;
      }
      srcbuf[si] = '\0';
      char tmp[256];
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
      char tmp[256];
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
      char tmp[256];
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
      char srcbuf[256];
      int32_t si = 0;
      while (src[si] != 0 && si < srcMax && si < (int32_t)sizeof(srcbuf) - 1) {
        srcbuf[si] = (char)(src[si] & 0xFF); si++;
      }
      srcbuf[si] = '\0';
      char tmp[256];
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
      char tmp[256];
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
      char tmp[256];
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

    case SYS_FILE_RANGE: {
      // fileRange(handle, min_ref, max_ref) -> total_rows
      // Reads a tab-delimited log file, skips header, returns first and last timestamps
#ifdef USE_UFILESYS
      int32_t max_ref = TC_POP(vm);
      int32_t min_ref = TC_POP(vm);
      int32_t h = TC_POP(vm);
      if (h < 0 || h >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[h]) {
        TC_PUSH(vm, 0);
        break;
      }
      // Save and seek to start
      tc_file_handles[h].seek(0, SeekSet);

      char line[512];
      int32_t rows = 0;
      bool headerSkipped = false;
      char first_ts[24] = {0};
      char last_ts[24] = {0};

      // Buffered read
      const int FBUF_SZ = 4096;
      uint8_t *fbuf = (uint8_t*)malloc(FBUF_SZ);
      if (!fbuf) { TC_PUSH(vm, 0); break; }
      int fbuf_len = 0, fbuf_pos = 0;

      while (true) {
        int llen = 0;
        bool got_line = false;
        while (true) {
          if (fbuf_pos >= fbuf_len) {
            fbuf_len = tc_file_handles[h].read(fbuf, FBUF_SZ);
            fbuf_pos = 0;
            if (fbuf_len <= 0) break;
          }
          char ch = (char)fbuf[fbuf_pos++];
          if (ch == '\n') { got_line = true; break; }
          if (ch == '\r') continue;
          if (llen < (int)sizeof(line) - 1) line[llen++] = ch;
        }
        line[llen] = 0;
        if (!got_line && llen == 0) break;

        if (!headerSkipped) { headerSkipped = true; continue; }

        // Extract timestamp (first column before tab)
        char *tab = line;
        while (*tab && *tab != '\t') tab++;
        int tslen = tab - line;
        if (tslen > 0 && tslen < 24) {
          if (first_ts[0] == 0) {
            memcpy(first_ts, line, tslen);
            first_ts[tslen] = 0;
          }
          memcpy(last_ts, line, tslen);
          last_ts[tslen] = 0;
          rows++;
        }
      }
      free(fbuf);

      // Write timestamps to output buffers
      int32_t *dst_min = tc_resolve_ref(vm, min_ref);
      int32_t min_max = tc_ref_maxlen(vm, min_ref);
      int32_t len = strlen(first_ts);
      if (len >= min_max) len = min_max - 1;
      for (int i = 0; i < len; i++) dst_min[i] = (int32_t)(uint8_t)first_ts[i];
      dst_min[len] = 0;

      int32_t *dst_max = tc_resolve_ref(vm, max_ref);
      int32_t max_max = tc_ref_maxlen(vm, max_ref);
      len = strlen(last_ts);
      if (len >= max_max) len = max_max - 1;
      for (int i = 0; i < len; i++) dst_max[i] = (int32_t)(uint8_t)last_ts[i];
      dst_max[len] = 0;

      TC_PUSH(vm, rows);
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
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
      char *url = (char *)malloc(256);
      if (!url) { TC_PUSH(vm, -1); break; }
      tc_ref_to_cstr(vm, urlRef, url, 256);
      if (!cpath) { free(url); TC_PUSH(vm, -1); break; }
      char *path = (char *)malloc(128);
      if (!path) { free(url); TC_PUSH(vm, -1); break; }
      strlcpy(path, cpath, 128);
      FS *fsp = tc_file_path(path);
      if (!fsp) { free(url); free(path); TC_PUSH(vm, -1); break; }
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
      HTTPClientLight http;
#else
      WiFiClient http_client;
      HTTPClient http;
#endif
      http.setTimeout(10000);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: fileDownload urlRef=%d len=%d url='%s'"), urlRef, strlen(url), url);
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
      bool begun = http.begin(UrlEncode(url));
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: fileDownload begin=%d (HTTPClientLight)"), begun);
#else
      bool begun = http.begin(http_client, UrlEncode(url));
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: fileDownload begin=%d (WiFiClient)"), begun);
#endif
      free(url);
      if (!begun) { http.end(); free(path); TC_PUSH(vm, -1); break; }
      int httpCode = http.GET();
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        File f = fsp->open(path, "w");
        if (f) {
          WiFiClient *stream = http.getStreamPtr();
          int32_t len = http.getSize();
          if (len < 0) len = 99999999;  // unknown size
          uint8_t *buf = (uint8_t *)malloc(512);
          if (buf) {
            while (http.connected() && (len > 0)) {
              size_t avail = stream->available();
              if (avail) {
                if (avail > 512) avail = 512;
                int rd = stream->readBytes(buf, avail);
                f.write(buf, rd);
                len -= rd;
              }
              delay(1);
            }
            free(buf);
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
      free(path);
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
      bool dflg = (accum < 0);                  // delta mode for absolute columns
      int32_t accumN = (accum < 0) ? -accum : (accum > 0 ? accum : 1);
      // Per-column flags: bit0=absolute(delta), bit1=average(_a), bit7=has previous value
      uint8_t mflg[32];
      memset(mflg, 0, sizeof(mflg));
      float lastv[32];                           // previous values for delta computation
      memset(lastv, 0, sizeof(lastv));
      float summs[32];                           // accumulation sums for averaging
      memset(summs, 0, sizeof(summs));
      uint16_t accnt[32];                        // per-column accumulation counters
      memset(accnt, 0, sizeof(accnt));
      bool headerParsed = false;

      // Lambda: read one line from buffered file
      #define TC_READ_LINE() \
        { llen = 0; got_line = false; \
          while (true) { \
            if (fbuf_pos >= fbuf_len) { \
              fbuf_len = tc_file_handles[h].read(fbuf, FBUF_SZ); \
              fbuf_pos = 0; \
              if (fbuf_len <= 0) break; \
            } \
            char ch = (char)fbuf[fbuf_pos++]; \
            if (ch == '\n') { got_line = true; break; } \
            if (ch == '\r') continue; \
            if (llen < (int)sizeof(line) - 1) line[llen++] = ch; \
          } \
          line[llen] = 0; \
        }

      while (true) {
        // ── Read next line from buffer ──
        int llen = 0;
        bool got_line = false;
        TC_READ_LINE();
        if (!got_line && llen == 0) break;  // EOF

        // ── Parse header line: detect _a suffix on column names ──
        if (!headerParsed) {
          headerParsed = true;
          // Parse header columns to detect _a suffix
          // col_offs matches Scripter: colpos 0=timestamp, 1=first data col
          // curpos = colpos - col_offs → array index
          char *hp = line;
          int hcol = 0;
          // Skip timestamp column (colpos=0)
          while (*hp && *hp != '\t') hp++;
          if (*hp == '\t') hp++;
          hcol = 1;
          // Iterate data columns
          while (*hp) {
            char *start = hp;
            while (*hp && *hp != '\t') hp++;
            int clen = hp - start;
            if (hcol >= col_offs) {
              int curpos = hcol - col_offs;
              if (curpos < numArrays) {
                if (dflg) {
                  mflg[curpos] = 1;  // default: absolute (delta mode)
                  if (clen >= 2 && start[clen-2] == '_' && start[clen-1] == 'a') {
                    mflg[curpos] |= 2;  // _a suffix: average value, skip delta
                  }
                }
              }
            }
            if (*hp == '\t') hp++;
            hcol++;
          }
          continue;  // skip header, read next line
        }

        // ── Parse timestamp in-place (first column before tab) ──
        char *tab = line;
        while (*tab && *tab != '\t') tab++;
        char saved = *tab;
        *tab = 0;

        uint32_t cmp = tc_ts_cmp(line);  // fast: no mktime, no sscanf
        *tab = saved;  // restore

        if (cmp == 0) continue;         // skip invalid
        if (cmp < cmp_from) continue;   // before range
        if (cmp > cmp_to) break;        // past range — done (data is chronological)

        // ── Skip timestamp column, then parse data columns ──
        // col_offs matches Scripter: colpos=1 is first data col
        // curpos = colpos - col_offs → array index
        char *p = (saved == '\t') ? tab + 1 : tab;
        int colpos = 1;  // first data column

        // ── Parse float values into destination arrays ──
        // Following Scripter logic: per-column delta + per-column accumulation
        while (*p) {
          float val = strtof(p, &p);
          if (*p == '\t') p++;

          if (colpos < col_offs) { colpos++; continue; }
          int c = colpos - col_offs;
          colpos++;
          if (c >= numArrays) break;

          float fval = val;
          bool flg = true;  // true = this value contributes to output

          if (mflg[c] & 1) {
            // Delta mode active for this column
            if (!(mflg[c] & 2)) {
              // No _a suffix: absolute counter → compute delta
              if (!(mflg[c] & 0x80)) {
                // First value: just store, no output yet
                lastv[c] = val;
                mflg[c] |= 0x80;
                flg = false;
              } else {
                float tmp = val;
                fval = val - lastv[c];
                if (fval < 0) fval = 0;  // clamp negative deltas
                lastv[c] = tmp;
              }
            }
            // _a suffix (mflg & 2): fval stays as-is (current value, no delta)
          }

          // Accumulate into per-column sum and store when count reaches accumN
          if (flg && arrBase[c]) {
            summs[c] += fval;
            accnt[c]++;
            if (accnt[c] >= accumN) {
              if (rowCount < arrMax[c]) {
                float avg = summs[c] / (float)accumN;
                memcpy(&arrBase[c][rowCount], &avg, sizeof(float));
              }
              summs[c] = 0;
              accnt[c] = 0;
            }
          }
        }

        // Advance row counter when first column's accumulator fires
        // Use column 0 as the reference (matches Scripter behavior where
        // all _a columns accumulate in sync, and absolute columns may lag by 1)
        // Find first active column to determine row advancement
        {
          bool advanced = false;
          for (int c = 0; c < numArrays; c++) {
            if (arrBase[c] && accnt[c] == 0 && rowCount < arrMax[c]) {
              // This column just stored a value (counter reset to 0)
              // Check if it actually had data (not just initialized)
              if (mflg[c] & 0x80 || !(mflg[c] & 1)) {
                advanced = true;
                break;
              }
            }
          }
          if (advanced) rowCount++;
        }
      }

      // Partial accumulation: store remainder
      {
        bool has_partial = false;
        for (int c = 0; c < numArrays; c++) {
          if (accnt[c] > 0) { has_partial = true; break; }
        }
        if (has_partial) {
          for (int c = 0; c < numArrays; c++) {
            if (accnt[c] > 0 && arrBase[c] && rowCount < arrMax[c]) {
              float avg = summs[c] / (float)accnt[c];
              memcpy(&arrBase[c][rowCount], &avg, sizeof(float));
            }
          }
          rowCount++;
        }
      }

      #undef TC_READ_LINE

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
    case SYS_LOG_LVL: {
      a = TC_POP(vm);  // char array ref
      int32_t level = TC_POP(vm);  // log level (1=ERROR, 2=INFO, 3=DEBUG, 4=DEBUG_MORE)
      if (level < 1) level = 1;
      if (level > 4) level = 4;
      char tmp[TC_OUTPUT_SIZE];
      if (tc_ref_to_cstr(vm, a, tmp, sizeof(tmp)) > 0) {
        AddLog(level, PSTR("TCC: %s"), tmp);
      }
      break;
    }
    case SYS_LOG_LVL_STR: {
      a = TC_POP(vm);  // constant pool index
      int32_t level = TC_POP(vm);  // log level
      if (level < 1) level = 1;
      if (level > 4) level = 4;
      if (a >= 0 && a < vm->const_count && vm->constants[a].type == 1) {
        AddLog(level, PSTR("TCC: %s"), vm->constants[a].str.ptr);
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
            Tinyc->udp_port_mcast = IPAddress(0,0,0,0);  // plain unicast
            if (Tinyc->udp_port.begin(port)) {
              Tinyc->udp_port_num = port;
              Tinyc->udp_port_open = true;
              Tinyc->udp_port_last_rx = millis();  // reset watchdog
              if (!Tinyc->udp_port_timeout) {
                Tinyc->udp_port_timeout = TC_UDP_TIMEOUT_SEC;
              }
              AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP port %d opened (timeout %ds)"), port, Tinyc->udp_port_timeout);
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
        case 1: { // udp(1, buf) → read string from UDP (works for unicast + multicast)
          int32_t buf_ref = TC_POP(vm);
          int32_t result = 0;
          if (Tinyc->udp_port_open && !TasmotaGlobal.global_state.network_down) {
            // Inactivity watchdog: reset socket if no packet within timeout
            if (Tinyc->udp_port_timeout > 0) {
              uint32_t elapsed = millis() - Tinyc->udp_port_last_rx;
              if (elapsed > (uint32_t)Tinyc->udp_port_timeout * 1000) {
                AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP port %d rx timeout (%ds) — resetting"),
                       Tinyc->udp_port_num, Tinyc->udp_port_timeout);
                uint16_t saved_port = Tinyc->udp_port_num;
                Tinyc->udp_port.stop();
                if ((uint32_t)Tinyc->udp_port_mcast != 0) {
#ifdef ESP8266
                  Tinyc->udp_port.beginMulticast(WiFi.localIP(), Tinyc->udp_port_mcast, saved_port);
#else
                  Tinyc->udp_port.beginMulticast(Tinyc->udp_port_mcast, saved_port);
#endif
                } else {
                  Tinyc->udp_port.begin(saved_port);
                }
                Tinyc->udp_port_last_rx = millis();
              }
            }
            int32_t plen = Tinyc->udp_port.parsePacket();
            if (plen > 0) {
              Tinyc->udp_port_last_rx = millis();  // packet received — reset watchdog
              // 1024 bytes covers SMA Speedwire (~600B) and most binary UDP telegrams.
              char packet[1024];
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
        case 8: { // udp(8, which, seconds) → set inactivity timeout
          // which: 0 = multicast globalvars, 1 = general-purpose port
          // seconds: timeout in seconds (0 = disable watchdog)
          int32_t seconds = TC_POP(vm);
          int32_t which = TC_POP(vm);
          if (seconds < 0) seconds = 0;
          if (seconds > 3600) seconds = 3600;
          if (which == 0) {
            Tinyc->udp_timeout = (uint16_t)seconds;
            Tinyc->udp_last_rx = millis();  // reset watchdog now
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP multicast timeout set to %ds"), seconds);
          } else {
            Tinyc->udp_port_timeout = (uint16_t)seconds;
            Tinyc->udp_port_last_rx = millis();
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP port timeout set to %ds"), seconds);
          }
          TC_PUSH(vm, 1);
          break;
        }
        case 9: { // udp(9, mcast_ip_ref, port) → join multicast group on udp_port
          int32_t port = TC_POP(vm);
          int32_t ip_ref = TC_POP(vm);
          char ip_str[32];
          tc_ref_to_cstr(vm, ip_ref, ip_str, sizeof(ip_str));
          IPAddress mcast;
          if (!mcast.fromString(ip_str) || port <= 0 || port >= 65536) {
            TC_PUSH(vm, 0);
            break;
          }
          Tinyc->udp_port.stop();
#ifdef ESP8266
          bool ok = Tinyc->udp_port.beginMulticast(WiFi.localIP(), mcast, (uint16_t)port);
#else
          bool ok = Tinyc->udp_port.beginMulticast(mcast, (uint16_t)port);
#endif
          if (ok) {
            Tinyc->udp_port_num = (uint16_t)port;
            Tinyc->udp_port_open = true;
            Tinyc->udp_port_mcast = mcast;
            Tinyc->udp_port_last_rx = millis();
            if (!Tinyc->udp_port_timeout) {
              Tinyc->udp_port_timeout = TC_UDP_TIMEOUT_SEC;
            }
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP joined %s:%d"), ip_str, port);
            TC_PUSH(vm, 1);
          } else {
            Tinyc->udp_port_open = false;
            Tinyc->udp_port_mcast = IPAddress(0,0,0,0);
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: UDP multicast join failed %s:%d"), ip_str, port);
            TC_PUSH(vm, 0);
          }
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
      // If text starts with '{', write as-is (caller provides JSON)
      // Otherwise wrap as {"COMMAND":"text"} via ResponseCmndChar
      a = TC_POP(vm);  // buf ref
      int32_t *arr = tc_resolve_ref(vm, a);
      int32_t maxLen = tc_ref_maxlen(vm, a);
      if (arr && maxLen > 0) {
        char tmpbuf[256];
        int32_t n = (maxLen < 255) ? maxLen : 255;
        int32_t i;
        for (i = 0; i < n && arr[i]; i++) { tmpbuf[i] = (char)(arr[i] & 0xFF); }
        tmpbuf[i] = '\0';
        if (tmpbuf[0] == '{') {
          Response_P(PSTR("%s"), tmpbuf);
        } else {
          ResponseCmndChar(tmpbuf);
        }
      }
      break;
    }
    case SYS_RESPONSE_CMND_STR: {
      // responseCmnd("literal") — send const string as console response
      // If text starts with '{', write as-is; otherwise wrap via ResponseCmndChar
      int32_t ci = TC_POP(vm);
      if (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1) {
        const char *s = vm->constants[ci].str.ptr;
        if (s[0] == '{') {
          Response_P(PSTR("%s"), s);
        } else {
          ResponseCmndChar(s);
        }
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
      a = TC_POP(vm);            // meter index (negative = full-precision numeric-as-string)
#if defined(USE_SML_M) || defined(USE_SML)
      char sbuf[FLOATSZ];
      const char *sval;
      if (a < 0) {
        // Scripter smls[-x] equivalent: full-precision numeric value as string
        dtostrfd(SML_GetVal((uint32_t)(-a)), 4, sbuf);
        sval = sbuf;
      } else {
        sval = SML_GetSVal((uint32_t)a);
      }
      if (sval && ref) {
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

    // ── SML bulk copy ──────────────────────────────────
    case SYS_SML_COPY: {
      // smlCopy(int arr[], int count) -> int — copy SML decoder values to float array
      int32_t count = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);
      int32_t copied = 0;
#if defined(USE_SML_M) || defined(USE_SML)
      int32_t *arr = tc_resolve_ref(vm, arr_ref);
      if (arr) {
        int32_t nvals = (int32_t)SML_GetVal(0);  // index 0 = count
        if (count > nvals) count = nvals;
        for (int32_t i = 0; i < count; i++) {
          float fv = (float)SML_GetVal(i + 1);  // 1-based
          uint32_t fi; memcpy(&fi, &fv, 4);
          arr[i] = (int32_t)fi;
          copied++;
        }
      }
#endif
      TC_PUSH(vm, copied);
      break;
    }
    // ── Array fill / copy ─────────────────────────────
    case SYS_ARRAY_FILL: {
      // arrayFill(int arr[], int value, int count) -> void
      int32_t count = TC_POP(vm);
      int32_t value = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);
      int32_t *arr = tc_resolve_ref(vm, arr_ref);
      if (arr) {
        for (int32_t i = 0; i < count; i++) {
          arr[i] = value;
        }
      }
      break;
    }
    case SYS_ARRAY_COPY: {
      // arrayCopy(int dst[], int src[], int count) -> void
      int32_t count = TC_POP(vm);
      int32_t src_ref = TC_POP(vm);
      int32_t dst_ref = TC_POP(vm);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      if (dst && src) {
        for (int32_t i = 0; i < count; i++) {
          dst[i] = src[i];
        }
      }
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
    //  20 = tasm_pheap      (ro) free PSRAM in bytes (ESP32 only)
    //  21 = tasm_smlj       (rw) SML JSON output enable/disable
    //  22 = tasm_npwr       (ro) number of power devices
    //  23 = tasm_rule       (rw) rule1 enabled (bit 0 of Settings->rule_enabled)
    //  24 = tasm_lat        (rw) latitude in decimal degrees (float)
    //  25 = tasm_lon        (rw) longitude in decimal degrees (float)
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
#ifdef USE_SML_M
        case 21: val = (int32_t)SML_SetOptions(0); break;  // tasm_smlj
#endif
        case 22: val = (int32_t)TasmotaGlobal.devices_present; break;  // tasm_npwr
        case 23: val = bitRead(Settings->rule_enabled, 0); break;  // tasm_rule
        case 24: {  // tasm_lat — float degrees
          float latf = (float)((double)Settings->latitude / 1000000.0);
          uint32_t lati; memcpy(&lati, &latf, 4);
          TC_PUSH(vm, (int32_t)lati);
          goto tasm_get_done;
        }
        case 25: {  // tasm_lon — float degrees
          float lonf = (float)((double)Settings->longitude / 1000000.0);
          uint32_t loni; memcpy(&loni, &lonf, 4);
          TC_PUSH(vm, (int32_t)loni);
          goto tasm_get_done;
        }
        default: break;
      }
      TC_PUSH(vm, val);
      tasm_get_done:
      break;
    }
    // ── Tasmota string info getter ─────────────────────
    case SYS_TASM_GET_STR: {
      // tasmInfo(sel, char buf[]) -> int strlen
      // sel: 0=topic, 1=mac, 2=ip, 3=friendly_name, 4=device_name, 5=group_topic
      int32_t buf_ref = TC_POP(vm);
      int32_t sel = TC_POP(vm);
      const char *src = "";
      char tmp[64];
      tmp[0] = 0;
      switch (sel) {
        case 0: src = TasmotaGlobal.mqtt_topic; break;
        case 1: strlcpy(tmp, NetworkUniqueId().c_str(), sizeof(tmp)); src = tmp; break;
        case 2: strlcpy(tmp, IPGetListeningAddressStr().c_str(), sizeof(tmp)); src = tmp; break;
        case 3: src = SettingsText(SET_FRIENDLYNAME1); break;
        case 4: src = SettingsText(SET_DEVICENAME); break;
#ifdef USE_MQTT
        case 5: src = SettingsText(SET_MQTT_GRP_TOPIC); break;
#endif
        case 6: strlcpy(tmp, GetResetReason().c_str(), sizeof(tmp)); src = tmp; break;
        default: break;
      }
      tc_cstr_to_ref(vm, buf_ref, src);
      TC_PUSH(vm, (int32_t)strlen(src));
      break;
    }
    // ── Indexed Tasmota state getters ─────────────────────
    case SYS_TASM_POWER: {
      // tasmPower(index) -> int — power state of relay (0-based)
      a = TC_POP(vm);
      int32_t val = 0;
      if (a >= 0 && a < (int32_t)TasmotaGlobal.devices_present) {
        val = (TasmotaGlobal.power >> a) & 1;
      }
      TC_PUSH(vm, val);
      break;
    }
    case SYS_TASM_SWITCH: {
      // tasmSwitch(index) -> int — switch state (0-based, Switch1=index 0)
      a = TC_POP(vm);
      int32_t val = -1;
      if (a >= 0 && a < MAX_SWITCHES) {
        val = SwitchGetState(a);
      }
      TC_PUSH(vm, val);
      break;
    }
    case SYS_TASM_COUNTER: {
      // tasmCounter(index) -> int — pulse counter (0-based, Counter1=index 0)
      a = TC_POP(vm);
      int32_t val = 0;
#ifdef USE_COUNTER
      if (a >= 0 && a < MAX_COUNTERS) {
        val = (int32_t)RtcSettings.pulse_counter[a];
      }
#endif
      TC_PUSH(vm, val);
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
#ifdef USE_SML_M
        case 21: SML_SetOptions(0x100 | (uint8_t)val); break;  // tasm_smlj
#endif
        case 23:  // tasm_rule
          if (val) {
            bitSet(Settings->rule_enabled, 0);
          } else {
            bitClear(Settings->rule_enabled, 0);
          }
          break;
        case 24: {  // tasm_lat — float degrees
          float latf; memcpy(&latf, &val, 4);
          Settings->latitude = (int)((double)latf * 1000000.0);
          break;
        }
        case 25: {  // tasm_lon — float degrees
          float lonf; memcpy(&lonf, &val, 4);
          Settings->longitude = (int)((double)lonf * 1000000.0);
          break;
        }
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
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
      HTTPClientLight http;
      http.setTimeout(5000);
      http.begin(url);
#else
      WiFiClient http_client;
      HTTPClient http;
      http.setTimeout(5000);
      http.begin(http_client, url);
#endif
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
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
      HTTPClientLight http;
      http.setTimeout(5000);
      http.begin(url);
#else
      WiFiClient http_client;
      HTTPClient http;
      http.setTimeout(5000);
      http.begin(http_client, url);
#endif
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
    case SYS_SML_APPLY_PINS: {
      // smlApplyPins("/path", rx, tx, smlf) — idempotent template-comment-preserving pin
      // substitution. Recognized placeholders: %0?rxpin%, %0?txpin%, %0?smlf%.
      // First call inserts "; <original>" template line above the active line and substitutes
      // placeholders in the active copy. Subsequent calls rebuild the active line from the
      // template comment (so re-edits always derive from the original markers, never from
      // already-substituted values). Pass -1 to leave a placeholder untouched.
      int32_t smlf = TC_POP(vm);
      int32_t txp  = TC_POP(vm);
      int32_t rxp  = TC_POP(vm);
      int32_t pi   = TC_POP(vm);
#ifdef USE_UFILESYS
      const char *cpath = tc_get_const_str(vm, pi);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      char path[128];
      strlcpy(path, cpath, sizeof(path));
      FS *fsp = tc_file_path(path);
      if (!fsp) { TC_PUSH(vm, -1); break; }
      File f = fsp->open(path, "r");
      if (!f) { TC_PUSH(vm, -1); break; }
      size_t fsize = (size_t)f.size();
      if (fsize == 0 || fsize > 8192) {  // hard cap — SML descriptors are tiny
        f.close();
        AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: smlApplyPins '%s' size %u out of range"), path, (unsigned)fsize);
        TC_PUSH(vm, -1);
        break;
      }
      char *in_buf = (char*)malloc(fsize + 1);
      if (!in_buf) { f.close(); TC_PUSH(vm, -1); break; }
      size_t got = (size_t)f.read((uint8_t*)in_buf, fsize);
      in_buf[got] = 0;
      f.close();
      // Worst case: every line gets a duplicated template comment → 2x.
      size_t out_cap = got * 2 + 64;
      char *out_buf = (char*)malloc(out_cap);
      if (!out_buf) { free(in_buf); TC_PUSH(vm, -1); break; }
      size_t out_len = 0;
      int subs = 0;
      const int values[3] = { (int)rxp, (int)txp, (int)smlf };
      const char *p = in_buf;
      const char *end = in_buf + got;
      while (p < end) {
        const char *eol = (const char*)memchr(p, '\n', (size_t)(end - p));
        size_t line_len = (size_t)((eol ? eol : end) - p);
        // Strip trailing \r so CRLF files become LF after a write.
        size_t content_len = line_len;
        if (content_len > 0 && p[content_len - 1] == '\r') content_len--;

        // Skip leading whitespace to test for ';' template marker.
        size_t lead = 0;
        while (lead < content_len && (p[lead] == ' ' || p[lead] == '\t')) lead++;
        bool is_comment = (lead < content_len && p[lead] == ';');
        bool has_ph = tc_sml_line_has_ph(p, content_len);

        if (is_comment && has_ph) {
          // Template line — emit as-is, then emit a freshly-substituted active line.
          memcpy(out_buf + out_len, p, content_len);
          out_len += content_len;
          out_buf[out_len++] = '\n';
          // Template body = content after the ';' (and any leading spaces after it).
          const char *body = p + lead + 1;  // skip ;
          while (body < p + content_len && (*body == ' ' || *body == '\t')) body++;
          size_t body_len = (size_t)((p + content_len) - body);
          size_t s = tc_sml_subst_line(body, body_len, out_buf + out_len, out_cap - out_len - 1, values);
          out_len += s;
          out_buf[out_len++] = '\n';
          subs++;
          // Discard the next existing line (the previous active copy) — convention says
          // the line directly below a "; ...%placeholder%..." template is the substituted
          // active version we just regenerated.
          if (eol && eol + 1 < end) {
            const char *next = eol + 1;
            const char *next_eol = (const char*)memchr(next, '\n', (size_t)(end - next));
            size_t next_len = (size_t)((next_eol ? next_eol : end) - next);
            size_t next_lead = 0;
            while (next_lead < next_len && (next[next_lead] == ' ' || next[next_lead] == '\t')) next_lead++;
            // Skip if it's a non-empty, non-comment line (i.e. an active candidate).
            if (next_lead < next_len && next[next_lead] != ';') {
              p = next_eol ? next_eol + 1 : end;
              continue;
            }
          }
          p = eol ? eol + 1 : end;
          continue;
        }

        if (!is_comment && has_ph) {
          // Active line with placeholders, no template above yet → insert template, then sub.
          out_buf[out_len++] = ';';
          out_buf[out_len++] = ' ';
          memcpy(out_buf + out_len, p, content_len);
          out_len += content_len;
          out_buf[out_len++] = '\n';
          size_t s = tc_sml_subst_line(p, content_len, out_buf + out_len, out_cap - out_len - 1, values);
          out_len += s;
          if (eol) out_buf[out_len++] = '\n';
          subs++;
          p = eol ? eol + 1 : end;
          continue;
        }

        // Plain line — copy as-is (without the \r if present).
        memcpy(out_buf + out_len, p, content_len);
        out_len += content_len;
        if (eol) out_buf[out_len++] = '\n';
        p = eol ? eol + 1 : end;
      }

      // Idempotency: skip the write if nothing actually changed.
      bool changed = (out_len != got) || (memcmp(in_buf, out_buf, got) != 0);
      int rc = 0;
      if (changed) {
        File w = fsp->open(path, "w");
        if (!w) { rc = -1; }
        else {
          size_t wn = w.write((const uint8_t*)out_buf, out_len);
          w.close();
          if (wn != out_len) {
            AddLog(LOG_LEVEL_ERROR, PSTR("TCC: smlApplyPins short write %u/%u"), (unsigned)wn, (unsigned)out_len);
            rc = -1;
          } else {
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: smlApplyPins '%s' rx=%d tx=%d smlf=%d → %d sub(s), %u→%u bytes"),
                   path, (int)rxp, (int)txp, (int)smlf, subs, (unsigned)got, (unsigned)out_len);
            rc = subs;
          }
        }
      }
      free(in_buf);
      free(out_buf);
      TC_PUSH(vm, rc);
#else
      (void)smlf; (void)txp; (void)rxp; (void)pi;
      TC_PUSH(vm, -1);
#endif
      break;
    }
    case SYS_SML_SCRIPTER_LOAD: {
      // smlScripterLoad("/path") — read the SML descriptor, extract any >F (Every100ms)
      // and >S (EverySecond) Scripter sections, compile each to a tiny bytecode that the
      // tick dispatcher runs without touching the full Scripter engine. Subset support:
      // lnv0..9, arithmetic+cmp, switch/case/ends, if/endif, sml(m,0,baud), sml(m,1,"HEX").
      // Returns # sections compiled (0..2), or -1 on file/open error.
      int32_t pi = TC_POP(vm);
      const char *cpath = tc_get_const_str(vm, pi);
      if (!cpath) { TC_PUSH(vm, -1); break; }
      int rc = tc_mscr_load(cpath);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: smlScripterLoad('%s') = %d (>F=%d >S=%d)"),
             cpath, rc, (int)tc_mscr.has_f, (int)tc_mscr.has_s);
      TC_PUSH(vm, rc);
      break;
    }
    case SYS_WEB_REPO_PULLDOWN: {
      // webRepoPulldown(&sel, "label", "json_url", "index_key", "/dest_path")
      //   Fetches {json_url} (a Scripter-smlpd-compatible directory JSON of the form
      //     {"<index_key>":[{"label":"...","filename":"..."}, ...]} ),
      //   renders a <select> whose options are the entries, pre-selects the current
      //   global's value, on-change writes the new index back to the global via
      //   the standard seva(value,idx) path, then (if dest_path non-empty) downloads
      //   the selected {base}/{filename} and POSTs it to /tc_api?cmd=writefile&path={dest_path}.
      //   base = json_url with the trailing '/basename' stripped.
      int32_t dpi  = TC_POP(vm);   // dest_path const idx ("" = no download)
      int32_t ki   = TC_POP(vm);   // index_key const idx
      int32_t ji   = TC_POP(vm);   // json_url const idx
      int32_t li   = TC_POP(vm);   // label const idx
      int32_t gref = TC_POP(vm);
      uint16_t idx = ((uint32_t)gref) & 0xFFFF;
      int32_t *p = tc_resolve_ref(vm, gref);
      int32_t val = p ? *p : 0;
      const char *label    = tc_get_const_str(vm, li);
      const char *json_url = tc_get_const_str(vm, ji);
      const char *key      = tc_get_const_str(vm, ki);
      const char *dest     = tc_get_const_str(vm, dpi);
      if (!label)    label    = "";
      if (!json_url) json_url = "";
      if (!key)      key      = "files";
      if (!dest)     dest     = "";
      // Render shell
      if (label[0]) {
        WSContentSend_P(PSTR("<div><label><b>%s</b> "), label);
      } else {
        WSContentSend_P(PSTR("<div>"));
      }
      WSContentSend_P(PSTR("<select id='rp%d' onfocusin='pr(0)' onfocusout='pr(1)' onchange='rpCh%d(this.value)'>"
                           "<option value='-1'>loading...</option></select>"),
                      idx, idx);
      if (label[0]) {
        WSContentSend_P(PSTR("</label></div>"));
      } else {
        WSContentSend_P(PSTR("</div>"));
      }
      // Inline JS — own handler per idx so we can capture the entry list
      WSContentSend_P(PSTR(
        "<script>"
        "(function(){"
          "var U='%s',K='%s',D='%s',V=%d,ID='rp%d',IDX=%d;"
          "var base=U.replace(/\\/[^\\/]+$/,'');"
          "window._rp=window._rp||{};"
          "window['rpCh'+IDX]=function(v){"
            "var i=parseInt(v,10);"
            "if(isNaN(i)||i<0){seva(i,IDX);return;}"
            "seva(i,IDX);"
            "if(!D||!window._rp[IDX])return;"
            "var e=window._rp[IDX][i];if(!e||!e.filename)return;"
            "fetch(base+'/'+e.filename.split('/').map(encodeURIComponent).join('/'))"
              ".then(function(r){return r.text();})"
              ".then(function(t){"
                "return fetch('/tc_api?cmd=writefile&path='+encodeURIComponent(D),"
                  "{method:'POST',headers:{'Content-Type':'text/plain'},body:t});"
              "})"
              ".then(function(r){return r.json();})"
              ".then(function(j){console.log('[repoPulldown] saved',D,j);})"
              ".catch(function(e){console.error('[repoPulldown]',e);});"
          "};"
          "function paint(L){"
            "var sel=document.getElementById(ID);if(!sel)return;"
            "var h='';for(var i=0;i<L.length;i++){"
              "h+='<option value=\"'+i+'\"'+(i===V?' selected':'')+'>'+"
                 "(L[i].label||L[i].filename||('#'+i))+'</option>';"
            "}"
            "sel.innerHTML=h;"
          "}"
          "if(window._rp[IDX]){paint(window._rp[IDX]);return;}"    // cache hit — no GitHub fetch
          "fetch(U).then(function(r){return r.json();}).then(function(j){"
            "var L=j[K]||[];window._rp[IDX]=L;paint(L);"
          "}).catch(function(e){"
            "var sel=document.getElementById(ID);"
            "if(sel)sel.innerHTML='<option value=\"-1\">load error</option>';"
            "console.error('[repoPulldown]',e);"
          "});"
        "})();"
        "</script>"),
        json_url, key, dest, val, idx, idx);
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
        // track which slot registered this page
        for (uint8_t si = 0; si < TC_MAX_VMS; si++) {
          if (Tinyc->slots[si] && tc_current_slot == Tinyc->slots[si]) {
            Tinyc->page_slot[pn] = si;
            break;
          }
        }
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
        if (Webserver) {
          Webserver->on(Tinyc->web_handler_url[hn - 1], TinyCWebOnHandlers[hn - 1]);
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: webOn(%d, \"%s\") registered"), hn, url);
        } else {
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: webOn(%d, \"%s\") deferred"), hn, url);
        }
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

    case SYS_WEB_CHART_TBASE: {
      // WebChartTimeBase(minutes) — set time base offset from "now" for chart x-axis
      // 0 = anchored to "now" (default), negative = past (e.g., -1440 = yesterday midnight)
      tc_chart_time_base = TC_POP(vm);
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
      int32_t pos      = TC_POP(vm);
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
      // decimals bits: 0-2 = decimal places (0-6), bit 3 = smooth (curveType:'function')
      bool smooth = (decimals & 8) != 0;
      decimals = decimals & 7;
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
          "function _tcN(ci,t,u,tp,mn,mx,sm){"
            "var lb=null;"
            "if(tp==116){var p=t.indexOf('|');if(p>=0){lb=t.substring(p+1).split('|');t=t.substring(0,p);}}"
            "_tcC[ci]={t:t,u:u,tp:tp,mn:mn,mx:mx,s:[],lb:lb,sm:sm||0};"
          "}"
          "function _tcD(){"
            "var N=new Date();"
            "for(var i=0;i<_tcC.length;i++){"
              "var c=_tcC[i];if(!c)continue;"
              "var dt=new google.visualization.DataTable();"
              "var tp=c.tp,el=document.getElementById('tc'+i);"
              "if(tp==116){"                                                               // table
                "if(c.lb&&c.s.length==1&&c.s[0].d.length>1){"
                  // Transposed table: labels as column headers, one row per series
                  "dt.addColumn('string','Tag');"
                  "for(var k=0;k<c.s[0].d.length;k++){"
                    "var lb=c.lb[k]||''+(k+1);"
                    "dt.addColumn('number',lb);}"
                  "var r=[c.s[0].l];"
                  "for(var k=0;k<c.s[0].d.length;k++)r.push(c.s[0].d[k][1]);"
                  "dt.addRows([r]);"
                "}else{"
                  // Multi-series table: series as columns, data points as rows
                  "dt.addColumn('string',c.t||'');"
                  "for(var j=0;j<c.s.length;j++)dt.addColumn('number',c.s[j].l);"
                  "var rows=[];"
                  "if(c.s.length>0)for(var k=0;k<c.s[0].d.length;k++){"
                    "var lb=c.lb&&c.lb[k]?c.lb[k]:''+(k+1);"
                    "var r=[lb];"
                    "for(var j=0;j<c.s.length;j++)r.push(c.s[j].d[k][1]);"
                    "rows.push(r);}"
                  "dt.addRows(rows);"
                "}"
                "new google.visualization.Table(el).draw(dt,{showRowNumber:true,width:'100%%'});"
              "}else{"                                                                     // charts
                "var isCol=(tp==98||tp==99||tp==1||tp==104||tp==115);"
                "if(isCol){dt.addColumn('string','X');}else{dt.addColumn('datetime','Time');}"
                "for(var j=0;j<c.s.length;j++)dt.addColumn('number',c.s[j].l);"
                "var rows=[];"
                "if(c.s.length>0)for(var k=0;k<c.s[0].d.length;k++){"
                  "var m=c.s[0].d[k][0];"
                  "var xl;"
                  "if(isCol){if(tp==115&&c.s[0].d.length>7){xl=''+k;}else{var td=new Date(N.getTime()+m*60000);if(c.s[0].d.length==1){xl='';}else if(c.s[0].d.length<=7){xl=['So','Mo','Di','Mi','Do','Fr','Sa'][td.getDay()];}else{xl=('0'+td.getDate()).slice(-2)+'.'+('0'+(td.getMonth()+1)).slice(-2)+'.';};}}else{xl=new Date(N.getTime()+m*60000);}"
                  "var r=[xl];"
                  "for(var j=0;j<c.s.length;j++)r.push(c.s[j].d[k][1]);"
                  "rows.push(r);}"
                "dt.addRows(rows);"
                "var colors=c.s.map(function(x){return x.c;});"
                "var va={title:c.u};"
                "if(c.mn<c.mx){va.viewWindow={min:c.mn,max:c.mx};}"
                "var dual=false,sr={},vx={};"
                "vx[0]=va;"
                "for(var j=0;j<c.s.length;j++){"
                  "var s=c.s[j];"
                  "if(j>0&&(s.mn!=c.s[0].mn||s.mx!=c.s[0].mx)){"
                    "dual=true;sr[j]={targetAxisIndex:1};"
                    "var a2={title:s.u};"
                    "if(s.mn<s.mx){a2.viewWindow={min:s.mn,max:s.mx};}"
                    "vx[1]=a2;"
                  "}else{sr[j]={targetAxisIndex:0};}"
                "}"
                "if(dual&&c.s[0].mn<c.s[0].mx){vx[0]={title:c.s[0].u,viewWindow:{min:c.s[0].mn,max:c.s[0].mx}};}"
                "var o={title:c.t,colors:colors,chartArea:{width:'75%%',height:'65%%'}};"
                "if(!isCol){"
                  "var dw=c.s[0].d[0]?c.s[0].d[0][0]*60000:-86400000;"
                  "var dwe=c.s[0].d.length>0?c.s[0].d[c.s[0].d.length-1][0]*60000:0;"
                  "o.curveType=c.sm?'function':'none';"
                  "o.hAxis={format:'HH:mm',viewWindow:{min:new Date(N.getTime()+dw-900000),max:new Date(N.getTime()+dwe+900000)}};"
                  "if((dwe-dw)>172800000){var tks=[];var nd=c.s[0].d.length;if(nd>10&&(dwe-dw)>6048e5){for(var ti=0;ti<nd;ti++){tks.push({v:new Date(N.getTime()+c.s[0].d[ti][0]*60000),f:''+ti});}o.hAxis.ticks=tks;}else{var wd=['So','Mo','Di','Mi','Do','Fr','Sa'];var ds=new Date(N.getTime()+dw);ds.setHours(0,0,0,0);ds.setDate(ds.getDate()+1);var de=new Date(N.getTime()+dwe);while(ds<=de){tks.push({v:new Date(ds),f:wd[ds.getDay()]});ds=new Date(ds.getTime()+86400000);}o.hAxis.ticks=tks;}}"
                  "o.lineWidth=1;o.pointSize=0;"
                "}"
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
          strcpy(div_style, "width:960px;height:300px");
        }
        if (fixed_range) {
          char ymin_s[16], ymax_s[16];
          dtostrf(ymin, 1, 1, ymin_s);
          dtostrf(ymax, 1, 1, ymax_s);
          WSContentSend_P(PSTR(
            "<div id=\"tc%d\" style=\"%s\"></div>"
            "<script>_tcN(%d,'%s','%s',%d,%s,%s,%d);</script>"
          ), chart_id, div_style, chart_id, title, axis_unit, type, ymin_s, ymax_s, smooth ? 1 : 0);
        } else {
          WSContentSend_P(PSTR(
            "<div id=\"tc%d\" style=\"%s\"></div>"
            "<script>_tcN(%d,'%s','%s',%d,0,0,%d);</script>"
          ), chart_id, div_style, chart_id, title, axis_unit, type, smooth ? 1 : 0);
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
        int32_t mins_ago = -((count - 1 - i) * interval) + tc_chart_time_base;
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

    // ── Webcam — camControl(sel, p1, p2) ───────────────
    case SYS_CAM_CONTROL: {
      int32_t p2  = TC_POP(vm);
      int32_t p1  = TC_POP(vm);
      int32_t sel = TC_POP(vm);
      int32_t res = -1;

      switch (sel) {
#if defined(ESP32) && defined(USE_WEBCAM)
        // ── Tasmota webcam wrapper (sel 0-7, requires USE_WEBCAM) ──
        case 0: res = WcSetup(p1); break;                  // init(resolution)
        case 1: res = WcGetFrame(p1); break;               // capture(bufnum) -> framesize
        case 2: res = WcSetOptions(p1, p2); break;         // options(sel, val)
        case 3: res = WcGetWidth(); break;                 // width()
        case 4: res = WcGetHeight(); break;                // height()
        case 5: res = WcSetStreamserver(p1); break;        // stream(on/off)
        case 6: res = WcSetMotionDetect(p1); break;        // motion(param)
#endif // USE_WEBCAM

#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
        case 7: {
          // savePic(bufnum, filehandle) — write picture from RAM buffer to open file
          // Uses WcGetPicstore() — bridged to tc_cam_slot when USE_TINYC_CAMERA
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
#endif // USE_WEBCAM || USE_TINYC_CAMERA

#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
        // ── Direct esp_camera API (sel 8-13) ──
        case 8: {
          // getSensorPID() -> PID value (e.g. 0x3660 for OV3660)
          sensor_t *s = esp_camera_sensor_get();
          res = s ? (int32_t)s->id.PID : -1;
          break;
        }
        case 9: {
          // sensorSet(param, value) -> 0=ok, -1=err
          sensor_t *s = esp_camera_sensor_get();
          if (s) {
            switch (p1) {
              case 0:  res = s->set_vflip(s, p2); break;
              case 1:  res = s->set_brightness(s, p2); break;
              case 2:  res = s->set_saturation(s, p2); break;
              case 3:  res = s->set_hmirror(s, p2); break;
              case 4:  res = s->set_contrast(s, p2); break;
              case 5:  res = s->set_framesize(s, (framesize_t)p2); break;
              case 6:  res = s->set_quality(s, p2); break;
              case 7:  res = s->set_sharpness(s, p2); break;
              case 8:  res = s->set_special_effect(s, p2); break;
              case 9:  res = s->set_whitebal(s, p2); break;
              case 10: res = s->set_awb_gain(s, p2); break;
              case 11: res = s->set_wb_mode(s, p2); break;
              case 12: res = s->set_exposure_ctrl(s, p2); break;
              case 13: res = s->set_aec2(s, p2); break;
              case 14: res = s->set_ae_level(s, p2); break;
              case 15: res = s->set_aec_value(s, p2); break;
              case 16: res = s->set_gain_ctrl(s, p2); break;
              case 17: res = s->set_agc_gain(s, p2); break;
              case 18: res = s->set_gainceiling(s, (gainceiling_t)p2); break;
              case 19: res = s->set_lenc(s, p2); break;
              case 20: res = s->set_raw_gma(s, p2); break;
              default: res = -1; break;
            }
          } else { res = -1; }
          break;
        }
        case 10: {
          // capture(slot) -> size in bytes — capture to PSRAM slot (1-based), returns size
          int32_t slot = p1;
          if (slot < 1 || slot > TC_CAM_MAX_SLOTS) { res = -1; break; }
          slot--;  // 0-based internally
#ifdef ESP32
          // esp_camera_fb_get() blocks — must only run from VM task thread
          // If task_handle is NULL (task exited) or we're on a different thread, skip
          if (!tc_current_slot || !tc_current_slot->task_handle ||
              xTaskGetCurrentTaskHandle() != tc_current_slot->task_handle) {
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: capture SKIPPED — wrong thread (slot=%d, task=%p, cur=%p)"),
              slot + 1, tc_current_slot ? tc_current_slot->task_handle : nullptr, xTaskGetCurrentTaskHandle());
            res = 0; break;  // skip — use TaskLoop for camera captures
          }
#endif
          camera_fb_t *fb = esp_camera_fb_get();
          if (!fb) {
            AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: cam fb_get timeout (slot %d)"), slot + 1);
            res = 0; break;
          }
          // (Re)allocate PSRAM slot if needed
          if (tc_cam_slot[slot].buf && tc_cam_slot[slot].len < fb->len) {
            free(tc_cam_slot[slot].buf);
            tc_cam_slot[slot].buf = nullptr;
          }
          if (!tc_cam_slot[slot].buf) {
            tc_cam_slot[slot].buf = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          }
          if (tc_cam_slot[slot].buf) {
            tc_cam_slot[slot].writing = 1;
            memcpy(tc_cam_slot[slot].buf, fb->buf, fb->len);
            tc_cam_slot[slot].len = fb->len;
            tc_cam_slot[slot].width = fb->width;
            tc_cam_slot[slot].height = fb->height;
            tc_cam_slot[slot].writing = 0;
            res = (int32_t)fb->len;
          } else {
            tc_cam_slot[slot].len = 0;
            res = -1;
            AddLog(LOG_LEVEL_ERROR, PSTR("TCC: cam slot %d PSRAM alloc failed (%d bytes)"), slot + 1, fb->len);
          }
          esp_camera_fb_return(fb);  // return camera fb immediately
          break;
        }
        case 11: {
          // saveSlot(slot, filehandle) -> bytes written — save PSRAM slot to file
          int32_t slot = p1;
          if (slot < 1 || slot > TC_CAM_MAX_SLOTS) { res = -1; break; }
          slot--;
          if (tc_cam_slot[slot].buf && tc_cam_slot[slot].len > 0 &&
              p2 >= 0 && p2 < TC_MAX_FILE_HANDLES && Tinyc->file_used[p2]) {
            res = tc_file_handles[p2].write(tc_cam_slot[slot].buf, tc_cam_slot[slot].len);
          } else { res = -1; }
          break;
        }
        case 12: {
          // freeSlot(slot) — free PSRAM slot (0 = free all)
          if (p1 == 0) {
            for (int i = 0; i < TC_CAM_MAX_SLOTS; i++) {
              if (tc_cam_slot[i].buf) { free(tc_cam_slot[i].buf); tc_cam_slot[i].buf = nullptr; }
              tc_cam_slot[i].len = 0;
            }
          } else if (p1 >= 1 && p1 <= TC_CAM_MAX_SLOTS) {
            int i = p1 - 1;
            if (tc_cam_slot[i].buf) { free(tc_cam_slot[i].buf); tc_cam_slot[i].buf = nullptr; }
            tc_cam_slot[i].len = 0;
          }
          res = 0;
          break;
        }
        case 13: {
          // deinit() -> 0=ok — deinit camera + free all slots + stop stream + free motion
          for (int i = 0; i < TC_CAM_MAX_SLOTS; i++) {
            if (tc_cam_slot[i].buf) { free(tc_cam_slot[i].buf); tc_cam_slot[i].buf = nullptr; }
            tc_cam_slot[i].len = 0;
            tc_cam_slot[i].width = 0;
            tc_cam_slot[i].height = 0;
          }
          // Stop stream server
          if (tc_cam_stream.server) {
            tc_cam_stream.stream_active = 0;
            tc_cam_stream.server->stop();
            delete tc_cam_stream.server;
            tc_cam_stream.server = nullptr;
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: stream server stopped"));
          }
          // Free motion buffers
          if (tc_cam_motion.ref_buf) { free(tc_cam_motion.ref_buf); tc_cam_motion.ref_buf = nullptr; }
          tc_cam_motion.ref_size = 0;
          tc_cam_motion.interval_ms = 0;
          tc_cam_motion.triggered = 0;
          // Note: NOT calling esp_camera_deinit() here — OV3660 doesn't recover
          // from deinit without power cycle. Camera stays initialized for reuse.
          // cameraInit() will deinit+reinit if called again (via tc_cam_inited flag).
          res = 0;
          break;
        }
        case 14: {
          // slotSize(slot) -> size in bytes (0 if empty)
          int32_t slot = p1;
          if (slot >= 1 && slot <= TC_CAM_MAX_SLOTS) {
            res = (int32_t)tc_cam_slot[slot - 1].len;
          } else { res = -1; }
          break;
        }
        case 15: {
          // streamControl(on_off) — start/stop MJPEG stream server on port 81
          // Actual server creation is in xdrv_124_tinyc.ino (TC_CamStreamInit/Stop)
          // This case just signals the request; the functions are called externally
          extern void TC_CamStreamInit(void);
          extern void TC_CamStreamStop(void);
          if (p1) {
            TC_CamStreamInit();
          } else {
            TC_CamStreamStop();
          }
          res = 0;
          break;
        }
        case 16: {
          // motionControl(interval_ms, threshold) — enable/disable motion detection
          tc_cam_motion.interval_ms = (uint16_t)p1;
          tc_cam_motion.threshold = (uint32_t)p2;
          tc_cam_motion.last_time = millis();
          if (p1 == 0) {
            // Disable — free reference buffer
            if (tc_cam_motion.ref_buf) { free(tc_cam_motion.ref_buf); tc_cam_motion.ref_buf = nullptr; }
            tc_cam_motion.ref_size = 0;
            tc_cam_motion.triggered = 0;
          }
          res = 0;
          break;
        }
        case 17: {
          // getMotionValue(sel) — read motion detection results
          switch (p1) {
            case 0: res = (int32_t)tc_cam_motion.motion_trigger; break;
            case 1: res = (int32_t)tc_cam_motion.motion_brightness; break;
            case 2: res = (int32_t)tc_cam_motion.triggered; break;
            case 3: res = (int32_t)tc_cam_motion.interval_ms; break;
            default: res = -1; break;
          }
          break;
        }
        case 18: {
          // motionFree() — free motion reference buffer
          if (tc_cam_motion.ref_buf) { free(tc_cam_motion.ref_buf); tc_cam_motion.ref_buf = nullptr; }
          tc_cam_motion.ref_size = 0;
          tc_cam_motion.interval_ms = 0;
          tc_cam_motion.triggered = 0;
          tc_cam_motion.motion_trigger = 0;
          tc_cam_motion.motion_brightness = 0;
          res = 0;
          break;
        }
        case 19: {
          // readSensorReg(addr, mask, 0) — read sensor register
          // p1=register address (e.g. 0x3820), p2=mask (0xff for full byte)
          sensor_t *s19 = esp_camera_sensor_get();
          if (s19) {
            res = s19->get_reg(s19, p1, p2 ? p2 : 0xff);
          } else { res = -1; }
          break;
        }
        case 20: {
          // writeSensorReg(addr, val, mask) — write sensor register
          // p1=register address, p2=value, p3 used as mask (passed via sel hack)
          // Usage from TinyC: camControl(20, addr, value)
          // writes full byte (mask=0xff)
          sensor_t *s20 = esp_camera_sensor_get();
          if (s20) {
            res = s20->set_reg(s20, p1, 0xff, p2);
          } else { res = -1; }
          break;
        }
#endif // USE_WEBCAM || USE_TINYC_CAMERA
        default:
          AddLog(LOG_LEVEL_ERROR, PSTR("TCC: camControl unknown sel=%d"), sel);
          res = -1;
          break;
      }
      TC_PUSH(vm, res);
      break;
    }

    // ── Hardware register peek/poke ─────────────────────
    case SYS_PEEK_REG: {
      // peekReg(addr) -> int — read 32-bit from memory-mapped address
      uint32_t addr = (uint32_t)TC_POP(vm);
      // Safety: only allow peripheral address ranges (0x3FF00000-0x3FFFFFFF, 0x60000000-0x600FFFFF)
      int32_t val = 0;
      if ((addr >= 0x3FF00000 && addr <= 0x3FFFFFFF) || (addr >= 0x60000000 && addr <= 0x600FFFFF)) {
        val = *(volatile uint32_t*)addr;
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: peekReg invalid addr 0x%08x"), addr);
        val = -1;
      }
      TC_PUSH(vm, val);
      break;
    }
    case SYS_POKE_REG: {
      // pokeReg(addr, val) -> void — write 32-bit to memory-mapped address
      int32_t val  = TC_POP(vm);
      uint32_t addr = (uint32_t)TC_POP(vm);
      if ((addr >= 0x3FF00000 && addr <= 0x3FFFFFFF) || (addr >= 0x60000000 && addr <= 0x600FFFFF)) {
        *(volatile uint32_t*)addr = (uint32_t)val;
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: pokeReg invalid addr 0x%08x"), addr);
      }
      break;
    }

    // ── Camera init with custom pins ─────────────────────
#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
    case SYS_CAM_INIT_PINS: {
      // cameraInit(pins[], format, framesize, quality, xclk_freq, fb_count, grab_mode, fb_loc) -> int
      // xclk_freq: Hz (0 = 20MHz default)
      // fb_count:  0 = auto (1 no PSRAM, 2 with PSRAM), >0 = explicit
      // grab_mode: -1 = auto, 0 = GRAB_WHEN_EMPTY, 1 = GRAB_LATEST
      // fb_loc:    0 = auto (PSRAM if available), 1 = force DRAM
      int32_t fb_loc_arg    = TC_POP(vm);
      int32_t grab_mode_arg = TC_POP(vm);
      int32_t fb_count_arg  = TC_POP(vm);
      int32_t xclk_arg      = TC_POP(vm);
      int32_t quality   = TC_POP(vm);
      int32_t framesize = TC_POP(vm);
      int32_t format    = TC_POP(vm);
      int32_t pins_ref  = TC_POP(vm);
      int32_t *pins = tc_resolve_ref(vm, pins_ref);

      camera_config_t config = {};
      config.pin_pwdn     = pins[0];
      config.pin_reset    = pins[1];
      config.pin_xclk     = pins[2];
      config.pin_sccb_sda = pins[3];
      config.pin_sccb_scl = pins[4];

      // Share I2C bus 2 if Tasmota already initialized it (other sensors on same bus)
      if (TasmotaGlobal.i2c_enabled[1]) {
        config.sccb_i2c_port = 1;           // reuse Wire1
        config.pin_sccb_sda  = -1;          // tell esp_camera to NOT install its own I2C driver
        config.pin_sccb_scl  = -1;
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: sharing I2C bus 2 for SCCB"));
      }

      config.pin_d7       = pins[5];
      config.pin_d6       = pins[6];
      config.pin_d5       = pins[7];
      config.pin_d4       = pins[8];
      config.pin_d3       = pins[9];
      config.pin_d2       = pins[10];
      config.pin_d1       = pins[11];
      config.pin_d0       = pins[12];
      config.pin_vsync    = pins[13];
      config.pin_href     = pins[14];
      config.pin_pclk     = pins[15];

      config.xclk_freq_hz  = (xclk_arg > 0) ? xclk_arg : 20000000;
      // Use high LEDC channel/timer to avoid conflict with Tasmota PWM (which uses TIMER_0/CHANNEL_0)
      config.ledc_channel   = LEDC_CHANNEL_4;
      config.ledc_timer     = LEDC_TIMER_2;
      config.pixel_format   = (pixformat_t)format;
      config.frame_size     = (framesize_t)framesize;
      config.jpeg_quality   = quality;
      // fb_location: 0=auto, 1=force DRAM
      bool use_psram = psramFound() && (fb_loc_arg != 1);
      config.fb_location = use_psram ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

      // fb_count: 0=auto, >0=explicit
      if (fb_count_arg > 0) {
        config.fb_count = fb_count_arg;
      } else if (use_psram) {
        config.fb_count = 2;
      } else {
        config.fb_count = 1;
      }

      // grab_mode: -1=auto, 0=WHEN_EMPTY, 1=LATEST
      // GRAB_LATEST keeps camera continuously capturing — queue always has fresh frame.
      // GRAB_WHEN_EMPTY stops camera when queue is full, causing GDMA freeze issues on ESP32-S3.
      if (grab_mode_arg >= 0) {
        config.grab_mode = (camera_grab_mode_t)grab_mode_arg;
      } else {
        config.grab_mode = CAMERA_GRAB_LATEST;
      }

      if (!use_psram && config.frame_size > FRAMESIZE_SVGA) {
        config.frame_size = FRAMESIZE_SVGA;
      }

      static bool tc_cam_inited = false;
      if (tc_cam_inited) {
        esp_camera_deinit();
        delay(100);
      }

      esp_err_t err = esp_camera_init(&config);
      if (err == ESP_OK) tc_cam_inited = true;
      int32_t res = (err == ESP_OK) ? 0 : -1;
      if (err != ESP_OK) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: cameraInit failed 0x%x"), err);
      } else {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: camera initialized fmt=%d fs=%d q=%d xclk=%d fb=%d grab=%d loc=%d"),
          format, framesize, quality, config.xclk_freq_hz, config.fb_count, config.grab_mode, config.fb_location);
        sensor_t *s = esp_camera_sensor_get();
        if (s) {
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: sensor PID=0x%x"), s->id.PID);
          if (s->id.PID == OV3660_PID) {
            s->set_vflip(s, 1);
            s->set_brightness(s, 1);
            s->set_saturation(s, -2);
            // Enable 50/60Hz banding filter to eliminate light flicker stripes
            // Register 0x3a00 bit 2 = banding filter enable (exposed as "aec2" / "Night Mode" in demo UI)
            // Default init leaves this OFF (0x3a = 0011_1010, bit 2 = 0)
            // Band width steps (0x3a08-0b) and max bands (0x3a0d-0e) are already configured by sensor init
            s->set_aec2(s, 1);
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: OV3660 banding filter enabled (aec2=1)"));
            // Dump banding/AEC registers for diagnostics
            uint8_t r3a00 = s->get_reg(s, 0x3a00, 0xff);  // AEC ctrl: bit2=banding enable
            uint8_t r3c00 = s->get_reg(s, 0x3c00, 0xff);  // banding filter ctrl (50/60Hz)
            uint8_t r3c01 = s->get_reg(s, 0x3c01, 0xff);  // banding auto-detect
            uint8_t r3a09 = s->get_reg(s, 0x3a09, 0xff);  // 50Hz band step L
            uint8_t r3a0b = s->get_reg(s, 0x3a0b, 0xff);  // 60Hz band step L
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: banding r3a00=%02x r3c00=%02x r3c01=%02x step50=%02x step60=%02x"),
              r3a00, r3c00, r3c01, r3a09, r3a0b);
            // Dump key timing/PLL registers
            uint8_t r3820 = s->get_reg(s, 0x3820, 0xff);  // TIMING_TC_REG20 (vflip/binning)
            uint8_t r3821 = s->get_reg(s, 0x3821, 0xff);  // TIMING_TC_REG21 (hmirror/compress)
            uint8_t r3814 = s->get_reg(s, 0x3814, 0xff);  // X_INCREMENT
            uint8_t r3815 = s->get_reg(s, 0x3815, 0xff);  // Y_INCREMENT
            uint8_t pll1 = s->get_reg(s, 0x303b, 0xff);   // SC_PLLS_CTRL1 (multiplier)
            uint8_t pll3 = s->get_reg(s, 0x303d, 0xff);   // SC_PLLS_CTRL3 (pre_div/root2x/seld5)
            uint8_t pclk = s->get_reg(s, 0x3824, 0xff);   // PCLK_RATIO
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: OV3660 r3820=%02x r3821=%02x xinc=%02x yinc=%02x pll_mult=%02x pll_prediv=%02x pclk=%02x"),
              r3820, r3821, r3814, r3815, pll1, pll3, pclk);
          }
        }
      }
      TC_PUSH(vm, res);
      break;
    }
#else
    case SYS_CAM_INIT_PINS: {
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
      break;
    }
#endif // USE_WEBCAM || USE_TINYC_CAMERA

    // ── Image store for dspLoadImage / dspPushImageRect ──
#if defined(USE_DISPLAY) && defined(ESP32) && defined(JPEG_PICTS)
    case SYS_DSP_LOAD_IMG: {
      // dspLoadImage("file.jpg") -> slot (0-3, -1 on error)
      int32_t ci = TC_POP(vm);
      if (!renderer) { TC_PUSH(vm, -1); break; }
      const char *fname = tc_get_const_str(vm, ci);
      if (!fname) { TC_PUSH(vm, -1); break; }
      // find free slot
      int slot = -1;
      for (int i = 0; i < TC_IMG_SLOTS; i++) {
        if (!tc_img_store[i].buf) { slot = i; break; }
      }
      if (slot < 0) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: img no free slot")); TC_PUSH(vm, -1); break; }
      // load file
      File fp = ufsp->open(fname, FS_FILE_READ);
      if (!fp) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: img file '%s' not found"), fname); TC_PUSH(vm, -1); break; }
      uint32_t size = fp.size();
      uint8_t *mem = (uint8_t *)special_malloc(size + 4);
      if (!mem) { fp.close(); TC_PUSH(vm, -1); break; }
      fp.read(mem, size);
      fp.close();
      if (mem[0] != 0xff || mem[1] != 0xd8) {
        free(mem); TC_PUSH(vm, -1); break;
      }
      uint16_t xsize, ysize;
      get_jpeg_size(mem, size, &xsize, &ysize);
      if (!xsize || !ysize) { free(mem); TC_PUSH(vm, -1); break; }
      uint32_t outsize = xsize * ysize * 2;
      uint16_t *out_buf = (uint16_t *)special_malloc(outsize + 4);
      if (!out_buf) { free(mem); TC_PUSH(vm, -1); break; }
      esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = mem,
        .indata_size = size,
        .outbuf = (uint8_t*)out_buf,
        .outbuf_size = outsize,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 }
      };
      esp_jpeg_image_output_t outimg;
      OsWatchLoop();
      esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &outimg);
      OsWatchLoop();
      free(mem);
      if (err != ESP_OK) { free(out_buf); TC_PUSH(vm, -1); break; }
      tc_img_store[slot].buf = out_buf;
      tc_img_store[slot].w = xsize;
      tc_img_store[slot].h = ysize;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: img slot %d loaded %dx%d (%d KB)"),
             slot, xsize, ysize, outsize / 1024);
      TC_PUSH(vm, slot);
      break;
    }
    case SYS_DSP_IMG_RECT: {
      // dspPushImageRect(slot, sx, sy, dx, dy, w, h)
      int32_t h  = TC_POP(vm);
      int32_t w  = TC_POP(vm);
      int32_t dy = TC_POP(vm);
      int32_t dx = TC_POP(vm);
      int32_t sy = TC_POP(vm);
      int32_t sx = TC_POP(vm);
      int32_t slot = TC_POP(vm);
      if (!renderer) break;
      if (slot < 0 || slot >= TC_IMG_SLOTS) break;
      if (!tc_img_store[slot].buf) break;
      uint16_t *img = tc_img_store[slot].buf;
      uint16_t img_w = tc_img_store[slot].w;
      uint16_t img_h = tc_img_store[slot].h;
      // clamp to image bounds
      if (sx < 0) sx = 0;
      if (sy < 0) sy = 0;
      if (sx + w > img_w) w = img_w - sx;
      if (sy + h > img_h) h = img_h - sy;
      if (w <= 0 || h <= 0) break;
      // Always use single setAddrWindow + pushColors (like Draw_RGB_Bitmap)
      // Row-by-row push freezes SPI displays (tested on ESP32 + ILI9341)
      if (sx == 0 && w == img_w) {
        // full-width: contiguous in buffer, single push
        renderer->setAddrWindow(dx, dy, dx + w, dy + h);
        renderer->pushColors(&img[sy * img_w], w * h, true);
      } else {
        // sub-rect: gather rows into contiguous temp buffer, single push
        uint32_t total = w * h;
        uint16_t *tmp = (uint16_t *)special_malloc(total * 2);
        if (tmp) {
          for (int row = 0; row < h; row++) {
            memcpy(&tmp[row * w], &img[(sy + row) * img_w + sx], w * 2);
          }
          renderer->setAddrWindow(dx, dy, dx + w, dy + h);
          renderer->pushColors(tmp, total, true);
          free(tmp);
        }
      }
      renderer->setAddrWindow(0, 0, 0, 0);
      break;
    }
    case SYS_DSP_IMG_WIDTH: {
      int32_t slot = TC_POP(vm);
      if (slot >= 0 && slot < TC_IMG_SLOTS && tc_img_store[slot].buf) {
        TC_PUSH(vm, tc_img_store[slot].w);
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }
    case SYS_DSP_IMG_HEIGHT: {
      int32_t slot = TC_POP(vm);
      if (slot >= 0 && slot < TC_IMG_SLOTS && tc_img_store[slot].buf) {
        TC_PUSH(vm, tc_img_store[slot].h);
      } else {
        TC_PUSH(vm, 0);
      }
      break;
    }

    // ── Canvas-backed image slots: draw procedurally, blit via dspPushImageRect ──
    case SYS_IMG_CREATE: {
      // imgCreate(w, h) -> slot (0..TC_IMG_SLOTS-1, -1 on error)
      // Allocates a blank RGB565 buffer in PSRAM plus a RendererCanvas around
      // it. The slot behaves exactly like a JPG-loaded slot except you can
      // also call imgBeginDraw() to make dsp* primitives target it.
      int32_t h = TC_POP(vm);
      int32_t w = TC_POP(vm);
      if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { TC_PUSH(vm, -1); break; }
      int slot = -1;
      for (int i = 0; i < TC_IMG_SLOTS; i++) {
        if (!tc_img_store[i].buf) { slot = i; break; }
      }
      if (slot < 0) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: img no free slot")); TC_PUSH(vm, -1); break; }
      uint32_t bytes = (uint32_t)w * (uint32_t)h * 2;
      uint16_t *buf = (uint16_t *)special_malloc(bytes + 4);
      if (!buf) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: img %dx%d OOM (%u B)"), w, h, bytes); TC_PUSH(vm, -1); break; }
      memset(buf, 0, bytes);  // start black
      RendererCanvas *cv = new RendererCanvas(buf, (uint16_t)w, (uint16_t)h);
      if (!cv) { free(buf); TC_PUSH(vm, -1); break; }
      tc_img_store[slot].buf    = buf;
      tc_img_store[slot].w      = (uint16_t)w;
      tc_img_store[slot].h      = (uint16_t)h;
      tc_img_store[slot].canvas = cv;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: img slot %d canvas %dx%d (%u KB)"),
             slot, w, h, bytes / 1024);
      TC_PUSH(vm, slot);
      break;
    }
    case SYS_IMG_BEGIN_DRAW: {
      // imgBeginDraw(slot) -> void
      // Swap the global `renderer` to the slot's canvas so subsequent dsp*
      // calls draw into its pixel buffer. Nested calls are ignored (Phase 1:
      // single redirect at a time). Must be paired with imgEndDraw().
      int32_t slot = TC_POP(vm);
      if (slot < 0 || slot >= TC_IMG_SLOTS) break;
      if (!tc_img_store[slot].canvas) break;         // not a canvas slot (JPG-only)
      if (tc_canvas_saved_renderer) break;            // already redirected
      tc_canvas_saved_renderer = renderer;
      renderer = tc_img_store[slot].canvas;
      break;
    }
    case SYS_IMG_END_DRAW: {
      // imgEndDraw() -> void. No-op if no redirect is active.
      if (tc_canvas_saved_renderer) {
        renderer = tc_canvas_saved_renderer;
        tc_canvas_saved_renderer = nullptr;
      }
      break;
    }
    case SYS_IMG_CLEAR: {
      // imgClear(slot, color) -> void. Fast memset-based fill.
      int32_t color = TC_POP(vm);
      int32_t slot  = TC_POP(vm);
      if (slot < 0 || slot >= TC_IMG_SLOTS) break;
      if (!tc_img_store[slot].buf)       break;
      uint32_t pixels = (uint32_t)tc_img_store[slot].w * tc_img_store[slot].h;
      uint16_t c16 = (uint16_t)color;
      uint8_t  hi = c16 >> 8, lo = c16 & 0xff;
      if (hi == lo) {
        memset(tc_img_store[slot].buf, lo, pixels * 2);
      } else {
        uint16_t *p = tc_img_store[slot].buf;
        for (uint32_t i = 0; i < pixels; i++) p[i] = c16;
      }
      // fillScreen-equivalent: whole canvas is now dirty
      if (tc_img_store[slot].canvas) {
        tc_img_store[slot].canvas->markDirty(0, 0, tc_img_store[slot].w, tc_img_store[slot].h);
      }
      break;
    }
    case SYS_IMG_BLIT: {
      // imgBlit(dst, src, sx, sy, dx, dy, w, h) -> void
      // Row-major memcpy between two canvas slots, with full clipping on
      // both source and destination rects. Unions the touched dest rect
      // into the dest canvas's dirty region.
      int32_t h  = TC_POP(vm);
      int32_t w  = TC_POP(vm);
      int32_t dy = TC_POP(vm);
      int32_t dx = TC_POP(vm);
      int32_t sy = TC_POP(vm);
      int32_t sx = TC_POP(vm);
      int32_t src = TC_POP(vm);
      int32_t dst = TC_POP(vm);
      if (dst < 0 || dst >= TC_IMG_SLOTS) break;
      if (src < 0 || src >= TC_IMG_SLOTS) break;
      TcImgSlot *ds = &tc_img_store[dst];
      TcImgSlot *ss = &tc_img_store[src];
      if (!ds->buf || !ss->buf) break;
      // clip against source
      if (sx < 0) { dx -= sx; w += sx; sx = 0; }
      if (sy < 0) { dy -= sy; h += sy; sy = 0; }
      if (sx + w > (int32_t)ss->w) w = (int32_t)ss->w - sx;
      if (sy + h > (int32_t)ss->h) h = (int32_t)ss->h - sy;
      // clip against destination
      if (dx < 0) { sx -= dx; w += dx; dx = 0; }
      if (dy < 0) { sy -= dy; h += dy; dy = 0; }
      if (dx + w > (int32_t)ds->w) w = (int32_t)ds->w - dx;
      if (dy + h > (int32_t)ds->h) h = (int32_t)ds->h - dy;
      if (w <= 0 || h <= 0) break;
      // same-buffer overlap: iterate bottom-up if dy > sy to avoid trash
      bool same = (ds->buf == ss->buf);
      if (same && dy > sy) {
        for (int32_t y = h - 1; y >= 0; y--) {
          memmove(ds->buf + (uint32_t)(dy + y) * ds->w + dx,
                  ss->buf + (uint32_t)(sy + y) * ss->w + sx,
                  (size_t)w * 2);
        }
      } else {
        for (int32_t y = 0; y < h; y++) {
          memmove(ds->buf + (uint32_t)(dy + y) * ds->w + dx,
                  ss->buf + (uint32_t)(sy + y) * ss->w + sx,
                  (size_t)w * 2);
        }
      }
      if (ds->canvas) ds->canvas->markDirty((int16_t)dx, (int16_t)dy, (int16_t)w, (int16_t)h);
      break;
    }
    case SYS_IMG_INVALIDATE: {
      // imgInvalidate(slot, x, y, w, h) -> void. Union rect into dirty region.
      int32_t ih = TC_POP(vm);
      int32_t iw = TC_POP(vm);
      int32_t iy = TC_POP(vm);
      int32_t ix = TC_POP(vm);
      int32_t slot = TC_POP(vm);
      if (slot < 0 || slot >= TC_IMG_SLOTS) break;
      RendererCanvas *cv = tc_img_store[slot].canvas;
      if (!cv) break;
      cv->markDirty((int16_t)ix, (int16_t)iy, (int16_t)iw, (int16_t)ih);
      break;
    }
    case SYS_IMG_FLUSH: {
      // imgFlush(slot, panel_x, panel_y) -> void.
      // Push the slot's dirty region to (panel_x+dx, panel_y+dy), then clear
      // the dirty region. Must NOT be inside an imgBeginDraw redirect (panel
      // renderer needed). No-op if dirty is empty.
      //
      // Implementation mirrors SYS_DSP_IMG_RECT: gather the sub-rect into a
      // contiguous temp buffer and send via ONE setAddrWindow + pushColors
      // call. Row-by-row pushColors freezes/scrambles SPI displays.
      int32_t py = TC_POP(vm);
      int32_t px = TC_POP(vm);
      int32_t slot = TC_POP(vm);
      if (slot < 0 || slot >= TC_IMG_SLOTS) break;
      TcImgSlot *s = &tc_img_store[slot];
      if (!s->buf || !s->canvas) break;
      if (!renderer) break;
      RendererCanvas *cv = s->canvas;
      if (!cv->hasDirty()) break;
      int16_t dx = cv->dirtyX(), dy = cv->dirtyY();
      int16_t dw = cv->dirtyW(), dh = cv->dirtyH();
      if (dw <= 0 || dh <= 0) { cv->clearDirty(); break; }
      uint16_t *img = s->buf;
      uint16_t img_w = s->w;
      if (dx == 0 && dw == (int16_t)img_w) {
        // full-width dirty: rows are contiguous, single push directly
        renderer->setAddrWindow((uint16_t)(px),
                                (uint16_t)(py + dy),
                                (uint16_t)(px + dw),
                                (uint16_t)(py + dy + dh));
        renderer->pushColors(&img[(uint32_t)dy * img_w], (uint32_t)dw * dh, true);
      } else {
        uint32_t total = (uint32_t)dw * dh;
        uint16_t *tmp = (uint16_t *)special_malloc(total * 2);
        if (tmp) {
          for (int16_t row = 0; row < dh; row++) {
            memcpy(&tmp[(uint32_t)row * dw],
                   &img[(uint32_t)(dy + row) * img_w + dx],
                   (size_t)dw * 2);
          }
          renderer->setAddrWindow((uint16_t)(px + dx),
                                  (uint16_t)(py + dy),
                                  (uint16_t)(px + dx + dw),
                                  (uint16_t)(py + dy + dh));
          renderer->pushColors(tmp, total, true);
          free(tmp);
        }
      }
      renderer->setAddrWindow(0, 0, 0, 0);
      cv->clearDirty();
      break;
    }

    // ── Bridge: camera JPEG slot  <->  display image slot (RGB565) ──
#if defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA)
    case SYS_DSP_LOAD_IMG_CAM: {
      // dspLoadImageFromCam(cam_slot) -> img_slot (0..TC_IMG_SLOTS-1, -1 on error)
      // Decodes a JPEG already captured into tc_cam_slot[cam-1] into a free RGB565
      // image slot, making it editable with dspImgTextBurn / re-encodable with
      // dspImageToCam. Non-destructive: the source cam slot is untouched.
      int32_t cam = TC_POP(vm);
      if (cam < 1 || cam > TC_CAM_MAX_SLOTS) { TC_PUSH(vm, -1); break; }
      cam--;
      if (!tc_cam_slot[cam].buf || tc_cam_slot[cam].len < 4) { TC_PUSH(vm, -1); break; }
      uint8_t *jpg  = tc_cam_slot[cam].buf;
      uint32_t jlen = tc_cam_slot[cam].len;
      if (jpg[0] != 0xff || jpg[1] != 0xd8) { TC_PUSH(vm, -1); break; }

      // find free img slot
      int slot = -1;
      for (int i = 0; i < TC_IMG_SLOTS; i++) {
        if (!tc_img_store[i].buf) { slot = i; break; }
      }
      if (slot < 0) {
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: img no free slot (cam %d)"), cam + 1);
        TC_PUSH(vm, -1); break;
      }

      uint16_t xsize = 0, ysize = 0;
      get_jpeg_size(jpg, jlen, &xsize, &ysize);
      if (!xsize || !ysize) { TC_PUSH(vm, -1); break; }
      uint32_t outsize = (uint32_t)xsize * ysize * 2;
      uint16_t *out_buf = (uint16_t *)special_malloc(outsize + 4);
      if (!out_buf) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: img %dx%d RGB565 alloc failed (%u KB)"),
               xsize, ysize, outsize / 1024);
        TC_PUSH(vm, -1); break;
      }
      esp_jpeg_image_cfg_t jpeg_cfg = {
        .indata = jpg,
        .indata_size = jlen,
        .outbuf = (uint8_t*)out_buf,
        .outbuf_size = outsize,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = { .swap_color_bytes = 0 }
      };
      esp_jpeg_image_output_t outimg;
      OsWatchLoop();
      esp_err_t err = esp_jpeg_decode(&jpeg_cfg, &outimg);
      OsWatchLoop();
      if (err != ESP_OK) {
        free(out_buf);
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: JPEG decode failed (cam %d, err=%d)"), cam + 1, err);
        TC_PUSH(vm, -1); break;
      }
      tc_img_store[slot].buf = out_buf;
      tc_img_store[slot].w = xsize;
      tc_img_store[slot].h = ysize;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: img slot %d from cam %d: %dx%d (%u KB)"),
             slot, cam + 1, xsize, ysize, outsize / 1024);
      TC_PUSH(vm, slot);
      break;
    }

    case SYS_DSP_IMG_TO_CAM: {
      // dspImageToCam(img_slot, cam_slot, quality) -> bytes written (-1 on error)
      // Re-encodes RGB565 image slot back into a cam slot as JPEG using
      // esp32-camera's fmt2jpg(). Quality is esp_camera's range (1..63,
      // lower = better). 12 is a good default (~ JPEG Q=85).
      int32_t q        = TC_POP(vm);
      int32_t cam      = TC_POP(vm);
      int32_t img_slot = TC_POP(vm);
      if (img_slot < 0 || img_slot >= TC_IMG_SLOTS || !tc_img_store[img_slot].buf) {
        TC_PUSH(vm, -1); break;
      }
      if (cam < 1 || cam > TC_CAM_MAX_SLOTS) { TC_PUSH(vm, -1); break; }
      if (q < 1)  q = 12;
      if (q > 63) q = 63;
      cam--;

      uint16_t w = tc_img_store[img_slot].w;
      uint16_t h = tc_img_store[img_slot].h;
      uint8_t *out_buf = nullptr;
      size_t   out_len = 0;
      OsWatchLoop();
      bool ok = fmt2jpg((uint8_t*)tc_img_store[img_slot].buf,
                        (size_t)w * h * 2,
                        w, h,
                        PIXFORMAT_RGB565,
                        (uint8_t)q,
                        &out_buf, &out_len);
      OsWatchLoop();
      if (!ok || !out_buf || !out_len) {
        if (out_buf) free(out_buf);
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: fmt2jpg failed (%dx%d q=%d)"), w, h, (int)q);
        TC_PUSH(vm, -1); break;
      }

      // (Re)allocate PSRAM cam slot buffer
      if (tc_cam_slot[cam].buf && tc_cam_slot[cam].len < out_len) {
        free(tc_cam_slot[cam].buf);
        tc_cam_slot[cam].buf = nullptr;
      }
      if (!tc_cam_slot[cam].buf) {
        tc_cam_slot[cam].buf = (uint8_t*)heap_caps_malloc(out_len,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      }
      if (!tc_cam_slot[cam].buf) {
        free(out_buf);
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: cam slot %d PSRAM alloc failed (%u bytes)"),
               cam + 1, (uint32_t)out_len);
        TC_PUSH(vm, -1); break;
      }
      tc_cam_slot[cam].writing = 1;
      memcpy(tc_cam_slot[cam].buf, out_buf, out_len);
      tc_cam_slot[cam].len    = out_len;
      tc_cam_slot[cam].width  = w;
      tc_cam_slot[cam].height = h;
      tc_cam_slot[cam].writing = 0;
      free(out_buf);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: cam slot %d from img %d: %dx%d -> %u bytes (q=%d)"),
             cam + 1, img_slot, w, h, (uint32_t)out_len, (int)q);
      TC_PUSH(vm, (int32_t)out_len);
      break;
    }
#endif // USE_WEBCAM || USE_TINYC_CAMERA

#else
    case SYS_DSP_LOAD_IMG: {
      TC_POP(vm); // filename
      TC_PUSH(vm, -1); // not available
      break;
    }
    case SYS_DSP_IMG_RECT: {
      for (int i = 0; i < 7; i++) TC_POP(vm); // consume args
      break;
    }
    case SYS_DSP_IMG_WIDTH:
    case SYS_DSP_IMG_HEIGHT: {
      TC_POP(vm); // slot
      TC_PUSH(vm, 0);
      break;
    }
    case SYS_IMG_CREATE: {
      TC_POP(vm); TC_POP(vm);  // w, h
      TC_PUSH(vm, -1);
      break;
    }
    case SYS_IMG_BEGIN_DRAW: {
      TC_POP(vm); // slot
      break;
    }
    case SYS_IMG_END_DRAW: {
      break;
    }
    case SYS_IMG_CLEAR: {
      TC_POP(vm); TC_POP(vm);  // slot, color
      break;
    }
    case SYS_IMG_BLIT: {
      for (int i = 0; i < 8; i++) TC_POP(vm);  // dst,src,sx,sy,dx,dy,w,h
      break;
    }
    case SYS_IMG_INVALIDATE: {
      for (int i = 0; i < 5; i++) TC_POP(vm);  // slot,x,y,w,h
      break;
    }
    case SYS_IMG_FLUSH: {
      TC_POP(vm); TC_POP(vm); TC_POP(vm);  // slot,panel_x,panel_y
      break;
    }
    case SYS_DSP_LOAD_IMG_CAM: {
      TC_POP(vm); // cam_slot
      TC_PUSH(vm, -1); // not available
      break;
    }
    case SYS_DSP_IMG_TO_CAM: {
      TC_POP(vm); TC_POP(vm); TC_POP(vm); // img_slot, cam_slot, quality
      TC_PUSH(vm, -1);
      break;
    }
#endif // USE_DISPLAY && ESP32 && JPEG_PICTS

#if !defined(USE_WEBCAM) && !defined(USE_TINYC_CAMERA) && \
     defined(USE_DISPLAY) && defined(ESP32) && defined(JPEG_PICTS)
    // Fallback when display/JPEG is available but no camera support compiled in
    case SYS_DSP_LOAD_IMG_CAM: {
      TC_POP(vm); TC_PUSH(vm, -1); break;
    }
    case SYS_DSP_IMG_TO_CAM: {
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_PUSH(vm, -1); break;
    }
#endif

    // ── Display drawing (direct renderer calls) ──────
#ifdef USE_DISPLAY
    case SYS_DSP_TEXT: {
      int32_t ref = TC_POP(vm);
      if (!renderer) break;  // display not yet initialized
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
      if (!renderer) break;
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
      if (renderer) DisplayOnOff(on);
      break;
    }
    case SYS_DSP_UPDATE:
      if (renderer) renderer->Updateframe();
      break;
    case SYS_DSP_PICTURE: {
      int32_t scale = TC_POP(vm);
      int32_t ci = TC_POP(vm);
      if (!renderer) break;
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
      if (!renderer) break;
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

    case SYS_DSP_TEXT_WIDTH: {
      // Get pixel width for N chars in current font/size
      int32_t len = TC_POP(vm);
      if (!renderer) { TC_PUSH(vm, 0); break; }
      int cw = 0;
      uint8_t csize = renderer->getTextSize();
#ifdef USE_EPD_FONTS
      if (renderer->getFont() > 0 && renderer->getFont() < 5 && renderer->getSelectedFont()) {
        cw = renderer->getSelectedFont()->Width;
      } else
#endif
      {
        cw = 6;  // GFX default: 5px char + 1px gap
      }
      TC_PUSH(vm, len * cw * csize);
      break;
    }
    case SYS_DSP_TEXT_HEIGHT: {
      // Get pixel height for current font/size
      if (!renderer) { TC_PUSH(vm, 0); break; }
      int ch = 0;
      uint8_t csize = renderer->getTextSize();
#ifdef USE_EPD_FONTS
      if (renderer->getFont() > 0 && renderer->getFont() < 5 && renderer->getSelectedFont()) {
        ch = renderer->getSelectedFont()->Height;
      } else
#endif
      {
        ch = 8;  // GFX default: 8px
      }
      TC_PUSH(vm, ch * csize);
      break;
    }

    case SYS_DSP_IMG_TEXT: {
      // dspImgText(slot, x, y, color, fieldWidth, align, text_buf)
      // Composite text onto image sub-rect in RAM, then push once — flicker-free
      // fieldWidth: total field width in chars (0 = auto from text length)
      // align: 0=left, 1=right, 2=center
      int32_t ref    = TC_POP(vm);
      int32_t align  = TC_POP(vm);
      int32_t fieldw = TC_POP(vm);
      int32_t color  = TC_POP(vm);
      int32_t y      = TC_POP(vm);
      int32_t x      = TC_POP(vm);
      int32_t slot   = TC_POP(vm);
      if (!renderer) break;
      if (slot < 0 || slot >= TC_IMG_SLOTS || !tc_img_store[slot].buf) break;

      char text[128];
      tc_ref_to_cstr(vm, ref, text, sizeof(text));
      int tlen = strlen(text);
      if (tlen == 0 && fieldw == 0) break;

      uint16_t *img = tc_img_store[slot].buf;
      uint16_t img_w = tc_img_store[slot].w;
      uint16_t img_h = tc_img_store[slot].h;

      // determine character dimensions
      int cw = 6, ch_h = 8;  // GFX defaults
      uint8_t csize = renderer->getTextSize();
#ifdef USE_EPD_FONTS
      sFONT *fnt = nullptr;
      if (renderer->getFont() > 0 && renderer->getFont() < 5 && renderer->getSelectedFont()) {
        fnt = renderer->getSelectedFont();
        cw = fnt->Width;
        ch_h = fnt->Height;
      }
#endif
      // field width in chars determines the pixel rect width
      int nchars = (fieldw > 0) ? fieldw : tlen;
      int tw = nchars * cw * csize;
      int th = ch_h * csize;

      // clamp to image bounds
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      if (x + tw > img_w) tw = img_w - x;
      if (y + th > img_h) th = img_h - y;
      if (tw <= 0 || th <= 0) break;

      // allocate temp buffer and copy image rect
      uint32_t total = tw * th;
      uint16_t *buf = (uint16_t *)special_malloc(total * 2);
      if (!buf) break;
      for (int row = 0; row < th; row++) {
        memcpy(&buf[row * tw], &img[(y + row) * img_w + x], tw * 2);
      }

      // calculate text offset for alignment within field
      int text_pw = tlen * cw * csize;  // actual text pixel width
      int x_off = 0;  // pixel offset within field rect
      if (align == 1) {        // right
        x_off = tw - text_pw;
      } else if (align == 2) { // center
        x_off = (tw - text_pw) / 2;
      }
      if (x_off < 0) x_off = 0;

      // render text characters into buf
      uint16_t fg = (uint16_t)color;
#ifdef USE_EPD_FONTS
      if (fnt) {
        for (int ci = 0; ci < tlen; ci++) {
          int cx = x_off + ci * cw * csize;
          if (cx >= tw) break;
          char ascii_char = text[ci];
          if (ascii_char < ' ') continue;
          unsigned int char_offset = (ascii_char - ' ') * fnt->Height * (fnt->Width / 8 + (fnt->Width % 8 ? 1 : 0));
          const unsigned char *ptr = &fnt->table[char_offset];
          for (int fj = 0; fj < fnt->Height; fj++) {
            for (int fi = 0; fi < fnt->Width; fi++) {
              if (pgm_read_byte(ptr) & (0x80 >> (fi % 8))) {
                // foreground pixel — write into buf for each scaled pixel
                for (int sy = 0; sy < csize; sy++) {
                  for (int sx = 0; sx < csize; sx++) {
                    int px = cx + fi * csize + sx;
                    int py = fj * csize + sy;
                    if (px < tw && py < th) {
                      buf[py * tw + px] = fg;
                    }
                  }
                }
              }
              // background pixels: keep image data (already in buf)
              if (fi % 8 == 7) ptr++;
            }
            if (fnt->Width % 8 != 0) ptr++;
          }
        }
      }
#endif

      // push composited result in one SPI transaction
      renderer->setAddrWindow(x, y, x + tw, y + th);
      renderer->pushColors(buf, total, true);
      renderer->setAddrWindow(0, 0, 0, 0);
      free(buf);
      break;
    }

    case SYS_DSP_IMG_TEXT_BURN: {
      // dspImgTextBurn(slot, x, y, color, fieldWidth, align, text_buf)
      // Same args / font logic as SYS_DSP_IMG_TEXT, but burns text pixels INTO
      // the image buffer. Does NOT push anything to the TFT. Use this when you
      // need to modify a captured camera frame before re-encoding to JPEG
      // (dspImageToCam). The image slot is mutated in place.
      //
      // NOTE: camera boards usually compile USE_DISPLAY but never initialize
      // renderer (no physical panel). We therefore DO NOT require renderer —
      // we read the EPD font table (PROGMEM constant Font12) directly. If a
      // real display IS attached and a font/size is selected, we honor it.
      int32_t ref    = TC_POP(vm);
      int32_t align  = TC_POP(vm);
      int32_t fieldw = TC_POP(vm);
      int32_t color  = TC_POP(vm);
      int32_t y      = TC_POP(vm);
      int32_t x      = TC_POP(vm);
      int32_t slot   = TC_POP(vm);
      if (slot < 0 || slot >= TC_IMG_SLOTS || !tc_img_store[slot].buf) break;

      char text[128];
      tc_ref_to_cstr(vm, ref, text, sizeof(text));
      int tlen = strlen(text);
      if (tlen == 0 && fieldw == 0) break;

      uint16_t *img  = tc_img_store[slot].buf;
      uint16_t img_w = tc_img_store[slot].w;
      uint16_t img_h = tc_img_store[slot].h;

#ifdef USE_EPD_FONTS
      // Default font when no display is wired up / selected: Font12 @ size 1.
      // Always linked as long as USE_DISPLAY is compiled (ESP32-CAM boards).
      sFONT *fnt   = &Font12;
      uint8_t csize = 1;
      if (renderer) {
        csize = renderer->getTextSize();
        if (csize < 1) csize = 1;
        if (renderer->getFont() > 0 && renderer->getFont() < 5 &&
            renderer->getSelectedFont()) {
          fnt = renderer->getSelectedFont();
        }
      }
      int cw   = fnt->Width;
      int ch_h = fnt->Height;

      int nchars = (fieldw > 0) ? fieldw : tlen;
      int tw = nchars * cw * csize;
      int th = ch_h  * csize;
      if (x < 0) x = 0;
      if (y < 0) y = 0;
      if (x + tw > img_w) tw = img_w - x;
      if (y + th > img_h) th = img_h - y;
      if (tw <= 0 || th <= 0) break;

      int text_pw = tlen * cw * csize;
      int x_off = 0;
      if      (align == 1) x_off = tw - text_pw;
      else if (align == 2) x_off = (tw - text_pw) / 2;
      if (x_off < 0) x_off = 0;

      uint16_t fg = (uint16_t)color;
      for (int ci = 0; ci < tlen; ci++) {
        int cx = x_off + ci * cw * csize;
        if (cx >= tw) break;
        char ascii_char = text[ci];
        if (ascii_char < ' ') continue;
        unsigned int char_offset = (ascii_char - ' ') * fnt->Height *
                                   (fnt->Width / 8 + (fnt->Width % 8 ? 1 : 0));
        const unsigned char *ptr = &fnt->table[char_offset];
        for (int fj = 0; fj < fnt->Height; fj++) {
          for (int fi = 0; fi < fnt->Width; fi++) {
            if (pgm_read_byte(ptr) & (0x80 >> (fi % 8))) {
              for (int sy_i = 0; sy_i < csize; sy_i++) {
                for (int sx_i = 0; sx_i < csize; sx_i++) {
                  int px = x + cx + fi * csize + sx_i;
                  int py = y + fj * csize + sy_i;
                  if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                    img[py * img_w + px] = fg;
                  }
                }
              }
            }
            if (fi % 8 == 7) ptr++;
          }
          if (fnt->Width % 8 != 0) ptr++;
        }
      }
#else
      (void)renderer; (void)img; (void)img_w; (void)img_h;
      (void)x; (void)y; (void)color; (void)fieldw; (void)align; (void)text;
#endif
      break;
    }

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

    // ── TinyUI ──────────────────────────────────────────
    case SYS_UI_SCREEN: { // (id) -> void  switch screen
      int32_t id = TC_POP(vm);
      if (id < 0) id = 0; if (id > 255) id = 255;
      tc_ui_current_screen = (uint8_t)id;
      tc_ui_clear_screen();
      tc_ui_delete_all_vbuttons();
      tc_ui_redraw_all_widgets();
      break;
    }
    case SYS_UI_THEME: { // (bg, accent, text, border) -> void
      int32_t border = TC_POP(vm);
      int32_t text   = TC_POP(vm);
      int32_t accent = TC_POP(vm);
      int32_t bg     = TC_POP(vm);
      tc_ui_theme.bg     = (uint16_t)bg;
      tc_ui_theme.fg     = (uint16_t)text;
      tc_ui_theme.accent = (uint16_t)accent;
      tc_ui_theme.border = (uint16_t)border;
      break;
    }
    case SYS_UI_CLEAR_SCREEN: { // () -> void
      tc_ui_clear_screen();
      break;
    }
    case SYS_UI_LABEL: { // (num,x,y,w,h,text_const,align) -> void
      int32_t align = TC_POP(vm);
      int32_t tci   = TC_POP(vm);
      int32_t h     = TC_POP(vm);
      int32_t w     = TC_POP(vm);
      int32_t y     = TC_POP(vm);
      int32_t x     = TC_POP(vm);
      int32_t num   = TC_POP(vm);
      if (num >= 0 && num < TC_UI_MAX_WIDGETS) {
        TcUiWidget *W = &tc_ui_widgets[num];
        W->type   = TC_UI_WIDGET_LABEL;
        W->screen = tc_ui_current_screen;
        W->x = (int16_t)x; W->y = (int16_t)y;
        W->w = (int16_t)w; W->h = (int16_t)h;
        W->fg = tc_ui_theme.fg;
        W->bg = tc_ui_theme.bg;
        W->accent = tc_ui_theme.accent;
        if (align < -1) align = -1; if (align > 1) align = 1;
        W->align = (int8_t)align;
        const char *s = tc_get_const_str(vm, tci);
        if (s) { strlcpy(W->text_buf, s, sizeof(W->text_buf)); }
        else   { W->text_buf[0] = 0; }
        tc_ui_draw_widget(W);
      }
      break;
    }
    case SYS_UI_LABEL_SET: { // (num, text_ref_or_const) -> void
      int32_t tci = TC_POP(vm);
      int32_t num = TC_POP(vm);
      if (num >= 0 && num < TC_UI_MAX_WIDGETS) {
        TcUiWidget *W = &tc_ui_widgets[num];
        if (W->type == TC_UI_WIDGET_LABEL) {
          // tc_ref_to_cstr handles both const refs and int32-array refs
          tc_ref_to_cstr(vm, tci, W->text_buf, sizeof(W->text_buf));
          tc_ui_draw_widget(W);
        }
      }
      break;
    }
    case SYS_UI_CHECKBOX: { // (num,x,y,w,h,label_const) -> void   — VButton-backed toggle
      int32_t lci = TC_POP(vm);
      int32_t h   = TC_POP(vm);
      int32_t w   = TC_POP(vm);
      int32_t y   = TC_POP(vm);
      int32_t x   = TC_POP(vm);
      int32_t num = TC_POP(vm);
      num = ((num % MAX_TOUCH_BUTTONS) + MAX_TOUCH_BUTTONS) % MAX_TOUCH_BUTTONS;
      const char *label = tc_get_const_str(vm, lci);
      if (!label) label = "";
      // Clamp sizes so a 0 or negative value still produces a usable hit area
      if (w < 8)  w = 8;
      if (h < 8)  h = 8;
      if (renderer) {
        if (buttons[num]) { delete buttons[num]; buttons[num] = nullptr; }
        buttons[num] = new VButton();
        if (buttons[num]) {
          char lbl[32];
          strlcpy(lbl, label, sizeof(lbl));
          // Match dspTButton init order exactly (slider before init, virtual flags after)
          buttons[num]->vpower.data = 0;           // zero all flags (esp. 'disable')
          buttons[num]->vpower.slider = 0;
          buttons[num]->xinitButtonUL(renderer, (int16_t)x, (int16_t)y,
            (uint16_t)w, (uint16_t)h,
            tc_ui_theme.border, tc_ui_theme.bg, tc_ui_theme.fg, lbl, 1);
          buttons[num]->vpower.is_virtual    = 1;
          buttons[num]->vpower.is_pushbutton = 0;  // toggle
          buttons[num]->xdrawButton(buttons[num]->vpower.on_off);
        }
      }
      break;
    }
    case SYS_UI_BUTTON: { // (num,x,y,w,h,label_const) -> void   — VButton-backed momentary pushbutton
      int32_t lci = TC_POP(vm);
      int32_t h   = TC_POP(vm);
      int32_t w   = TC_POP(vm);
      int32_t y   = TC_POP(vm);
      int32_t x   = TC_POP(vm);
      int32_t num = TC_POP(vm);
      num = ((num % MAX_TOUCH_BUTTONS) + MAX_TOUCH_BUTTONS) % MAX_TOUCH_BUTTONS;
      const char *label = tc_get_const_str(vm, lci);
      if (!label) label = "";
      if (w < 8)  w = 8;
      if (h < 8)  h = 8;
      if (renderer) {
        if (buttons[num]) { delete buttons[num]; buttons[num] = nullptr; }
        buttons[num] = new VButton();
        if (buttons[num]) {
          char lbl[32];
          strlcpy(lbl, label, sizeof(lbl));
          buttons[num]->vpower.data = 0;
          buttons[num]->vpower.slider = 0;
          buttons[num]->xinitButtonUL(renderer, (int16_t)x, (int16_t)y,
            (uint16_t)w, (uint16_t)h,
            tc_ui_theme.border, tc_ui_theme.bg, tc_ui_theme.fg, lbl, 1);
          buttons[num]->vpower.is_virtual    = 1;
          buttons[num]->vpower.is_pushbutton = 1;  // momentary — TouchButton(num,1) press, (num,0) release
          buttons[num]->xdrawButton(buttons[num]->vpower.on_off);
        }
      }
      break;
    }
    case SYS_UI_PROGRESS: { // (num,x,y,w,h,value,max) -> void
      int32_t vmax  = TC_POP(vm);
      int32_t value = TC_POP(vm);
      int32_t h     = TC_POP(vm);
      int32_t w     = TC_POP(vm);
      int32_t y     = TC_POP(vm);
      int32_t x     = TC_POP(vm);
      int32_t num   = TC_POP(vm);
      if (num >= 0 && num < TC_UI_MAX_WIDGETS) {
        TcUiWidget *W = &tc_ui_widgets[num];
        W->type   = TC_UI_WIDGET_PROGRESS;
        W->screen = tc_ui_current_screen;
        W->x = (int16_t)x; W->y = (int16_t)y;
        W->w = (int16_t)w; W->h = (int16_t)h;
        W->value = value;
        W->vmin  = 0;
        W->vmax  = (vmax > 0) ? vmax : 100;
        W->fg = tc_ui_theme.fg;
        W->bg = tc_ui_theme.muted;
        W->accent = tc_ui_theme.accent;
        tc_ui_draw_widget(W);
      }
      break;
    }
    case SYS_UI_PROGRESS_SET: { // (num, value) -> void
      int32_t value = TC_POP(vm);
      int32_t num   = TC_POP(vm);
      if (num >= 0 && num < TC_UI_MAX_WIDGETS) {
        TcUiWidget *W = &tc_ui_widgets[num];
        if (W->type == TC_UI_WIDGET_PROGRESS) {
          W->value = value;
          tc_ui_draw_widget(W);
        }
      }
      break;
    }
    case SYS_UI_GAUGE: { // (num,x,y,r,value,vmin,vmax) -> void
      int32_t vmax  = TC_POP(vm);
      int32_t vmin  = TC_POP(vm);
      int32_t value = TC_POP(vm);
      int32_t r     = TC_POP(vm);
      int32_t y     = TC_POP(vm);
      int32_t x     = TC_POP(vm);
      int32_t num   = TC_POP(vm);
      if (num >= 0 && num < TC_UI_MAX_WIDGETS) {
        TcUiWidget *W = &tc_ui_widgets[num];
        W->type   = TC_UI_WIDGET_GAUGE;
        W->screen = tc_ui_current_screen;
        W->x = (int16_t)x; W->y = (int16_t)y;
        W->w = (int16_t)r; W->h = (int16_t)r;
        W->value = value;
        W->vmin  = vmin;
        W->vmax  = (vmax > vmin) ? vmax : (vmin + 1);
        W->fg = tc_ui_theme.fg;
        W->bg = tc_ui_theme.bg;
        W->accent = tc_ui_theme.accent;
        tc_ui_draw_widget(W);
      }
      break;
    }
    case SYS_UI_ICON: { // (num,x,y,img_slot) -> void  (image button wrapper)
      int32_t slot = TC_POP(vm);
      int32_t y    = TC_POP(vm);
      int32_t x    = TC_POP(vm);
      int32_t num  = TC_POP(vm);
      // For now draw via dspPicture-equivalent by calling renderer directly via the img slot path.
      // TODO: wire to the existing image-slot lookup once that path is exposed.
      (void)slot; (void)x; (void)y; (void)num;
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
    // TinyUI stubs (no-touch display build)
    case SYS_UI_SCREEN:         TC_POP(vm); break;
    case SYS_UI_THEME:          TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_CLEAR_SCREEN:   break;
    case SYS_UI_LABEL:          for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_LABEL_SET:      TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_CHECKBOX:       for (int i = 0; i < 6; i++) TC_POP(vm); break;
    case SYS_UI_PROGRESS:       for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_PROGRESS_SET:   TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_GAUGE:          for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_ICON:           for (int i = 0; i < 4; i++) TC_POP(vm); break;
    case SYS_UI_BUTTON:         for (int i = 0; i < 6; i++) TC_POP(vm); break;
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
    case SYS_DSP_TEXT_WIDTH:
      TC_POP(vm); TC_PUSH(vm, 0); break;
    case SYS_DSP_TEXT_HEIGHT:
      TC_PUSH(vm, 0); break;
    case SYS_DSP_IMG_TEXT:
    case SYS_DSP_IMG_TEXT_BURN:
      // 7 args: slot, x, y, color, fieldw, align, buf_ref
      for (int i = 0; i < 7; i++) TC_POP(vm); break;
    // TinyUI stubs (no-display build)
    case SYS_UI_SCREEN:         TC_POP(vm); break;
    case SYS_UI_THEME:          TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_CLEAR_SCREEN:   break;
    case SYS_UI_LABEL:          for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_LABEL_SET:      TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_CHECKBOX:       for (int i = 0; i < 6; i++) TC_POP(vm); break;
    case SYS_UI_PROGRESS:       for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_PROGRESS_SET:   TC_POP(vm); TC_POP(vm); break;
    case SYS_UI_GAUGE:          for (int i = 0; i < 7; i++) TC_POP(vm); break;
    case SYS_UI_ICON:           for (int i = 0; i < 4; i++) TC_POP(vm); break;
    case SYS_UI_BUTTON:         for (int i = 0; i < 6; i++) TC_POP(vm); break;
    // NOTE: Canvas stubs (SYS_IMG_CREATE..SYS_IMG_FLUSH) intentionally NOT
    // duplicated here. They are provided by the earlier
    //   #if defined(USE_DISPLAY) && defined(ESP32) && defined(JPEG_PICTS)
    //   #else
    // block (~line 8015), which fires whenever *any* of those three macros
    // is undefined — including the !USE_DISPLAY case this #else covers.
    // Adding them here would produce duplicate-case-value errors.
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

    // ── Minimal I2S output (standalone, no USE_I2S_AUDIO needed) ──
#ifdef ESP32
    case SYS_I2S_BEGIN: {
      // i2sBegin(bclk, lrclk, dout, sampleRate) -> 0=ok, -1=error
      int32_t sample_rate = TC_POP(vm);
      int32_t dout_pin    = TC_POP(vm);
      int32_t lrclk_pin   = TC_POP(vm);
      int32_t bclk_pin    = TC_POP(vm);

      // Stop any previous I2S session
      if (Tinyc->i2s_tx_handle) {
        i2s_channel_disable(Tinyc->i2s_tx_handle);
        i2s_del_channel(Tinyc->i2s_tx_handle);
        Tinyc->i2s_tx_handle = nullptr;
      }
      if (Tinyc->i2s_pcm_buf) {
        free(Tinyc->i2s_pcm_buf);
        Tinyc->i2s_pcm_buf = nullptr;
      }

      if (sample_rate < 8000) sample_rate = 8000;
      if (sample_rate > 48000) sample_rate = 48000;

      i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
      chan_cfg.dma_desc_num = 8;
      chan_cfg.dma_frame_num = 512;

      esp_err_t err = i2s_new_channel(&chan_cfg, &Tinyc->i2s_tx_handle, NULL);
      if (err != ESP_OK) {
        Tinyc->i2s_tx_handle = nullptr;
        TC_PUSH(vm, -1);
        break;
      }

      i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
          .mclk = I2S_GPIO_UNUSED,
          .bclk = (gpio_num_t)bclk_pin,
          .ws   = (gpio_num_t)lrclk_pin,
          .dout = (gpio_num_t)dout_pin,
          .din  = I2S_GPIO_UNUSED,
          .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
      };

      err = i2s_channel_init_std_mode(Tinyc->i2s_tx_handle, &std_cfg);
      if (err != ESP_OK) {
        i2s_del_channel(Tinyc->i2s_tx_handle);
        Tinyc->i2s_tx_handle = nullptr;
        TC_PUSH(vm, -1);
        break;
      }

      i2s_channel_enable(Tinyc->i2s_tx_handle);
      Tinyc->i2s_sample_rate = sample_rate;
      // Allocate stereo PCM buffer: 512 mono samples → 1024 stereo int16
      if (Tinyc->i2s_pcm_buf) free(Tinyc->i2s_pcm_buf);
      Tinyc->i2s_pcm_buf = (int16_t *)malloc(1024 * sizeof(int16_t));
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: I2S TX started bclk=%d lrclk=%d dout=%d rate=%d"),
             bclk_pin, lrclk_pin, dout_pin, sample_rate);
      TC_PUSH(vm, 0);
      break;
    }

    case SYS_I2S_WRITE: {
      // i2sWrite(buf, len) -> samples_written
      // buf is int[] with 16-bit signed mono PCM samples
      // Output is stereo: each mono sample is duplicated to L+R channels
      // Uses static buffer to avoid malloc/free per call
      int32_t len     = TC_POP(vm);
      int32_t arr_ref = TC_POP(vm);

      if (!Tinyc->i2s_tx_handle || !Tinyc->i2s_pcm_buf) {
        TC_PUSH(vm, -1);
        break;
      }

      int32_t *arr = tc_resolve_ref(vm, arr_ref);
      int32_t maxLen = tc_ref_maxlen(vm, arr_ref);
      if (!arr || len <= 0) { TC_PUSH(vm, 0); break; }
      if (len > maxLen) len = maxLen;
      if (len > 512) len = 512;  // cap to buffer size

      int16_t *i2s_pcm_buf = Tinyc->i2s_pcm_buf;

      for (int32_t i = 0; i < len; i++) {
        int32_t s = arr[i];
        if (s > 32767) s = 32767;
        if (s < -32768) s = -32768;
        i2s_pcm_buf[i * 2]     = (int16_t)s;  // left
        i2s_pcm_buf[i * 2 + 1] = (int16_t)s;  // right
      }

      size_t bytes_written = 0;
      i2s_channel_write(Tinyc->i2s_tx_handle, i2s_pcm_buf, len * 2 * sizeof(int16_t),
                        &bytes_written, portMAX_DELAY);
      TC_PUSH(vm, (int32_t)(bytes_written / (2 * sizeof(int16_t))));
      break;
    }

    case SYS_I2S_STOP: {
      // i2sStop() -> void
      if (Tinyc->i2s_tx_handle) {
        i2s_channel_disable(Tinyc->i2s_tx_handle);
        i2s_del_channel(Tinyc->i2s_tx_handle);
        Tinyc->i2s_tx_handle = nullptr;
      }
      if (Tinyc->i2s_pcm_buf) {
        free(Tinyc->i2s_pcm_buf);
        Tinyc->i2s_pcm_buf = nullptr;
      }
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: I2S TX stopped"));
      break;
    }
#else
    // ESP8266: no I2S support
    case SYS_I2S_BEGIN: { TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_PUSH(vm, -1); break; }
    case SYS_I2S_WRITE: { TC_POP(vm); TC_POP(vm); TC_PUSH(vm, -1); break; }
    case SYS_I2S_STOP:  { break; }
#endif

    // ── fileReadPCM16: read 16-bit LE PCM from file into int[] (native speed) ──
    case SYS_FILE_READ_PCM16: {
#ifdef USE_UFILESYS
      // fileReadPCM16(handle, arr, max_samples, channels) -> samples_read
      // channels: 1=mono, 2=stereo (downmixed to mono)
      // Reads raw bytes from file, converts int16 LE to int32 in arr[]
      int32_t channels    = TC_POP(vm);
      int32_t max_samples = TC_POP(vm);
      int32_t ref         = TC_POP(vm);
      int32_t handle      = TC_POP(vm);

      if (handle < 0 || handle >= TC_MAX_FILE_HANDLES || !Tinyc->file_used[handle]) {
        TC_PUSH(vm, -1); break;
      }
      int32_t *arr = tc_resolve_ref(vm, ref);
      int32_t arr_len = tc_ref_maxlen(vm, ref);
      if (!arr || max_samples <= 0) { TC_PUSH(vm, 0); break; }
      if (max_samples > arr_len) max_samples = arr_len;

      File &f = tc_file_handles[handle];
      int bytes_per_frame = (channels == 2) ? 4 : 2;
      int32_t read_bytes = max_samples * bytes_per_frame;
      // Use a temp buffer on stack (max 2048 bytes = 512 stereo frames or 1024 mono frames)
      if (read_bytes > 2048) { max_samples = 2048 / bytes_per_frame; read_bytes = max_samples * bytes_per_frame; }
      uint8_t tmpbuf[2048];
      int32_t got = f.read(tmpbuf, read_bytes);
      if (got <= 0) { TC_PUSH(vm, 0); break; }
      int32_t frames = got / bytes_per_frame;

      if (channels == 2) {
        // Stereo: average L+R to mono
        for (int32_t i = 0; i < frames; i++) {
          int32_t lo = tmpbuf[i * 4];
          int32_t hi = tmpbuf[i * 4 + 1];
          int32_t left = (int16_t)((hi << 8) | lo);
          lo = tmpbuf[i * 4 + 2];
          hi = tmpbuf[i * 4 + 3];
          int32_t right = (int16_t)((hi << 8) | lo);
          arr[i] = (left + right) / 2;
        }
      } else {
        // Mono
        for (int32_t i = 0; i < frames; i++) {
          int32_t lo = tmpbuf[i * 2];
          int32_t hi = tmpbuf[i * 2 + 1];
          arr[i] = (int16_t)((hi << 8) | lo);
        }
      }
      TC_PUSH(vm, frames);
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    // ── TCP: server-accepted or outgoing client (selected via tcpSelect) ──
    // Outgoing client is selected via tcp_cli_slot (0..TC_TCP_CLI_SLOTS-1).
    // Slot 0 additionally falls back to server-accepted client for backward
    // compat with the Scripter ws* server-only API.
    #define TC_TCP_OUT_CLIENT() (&Tinyc->tcp_cli_clients[Tinyc->tcp_cli_slot])
    #define TC_TCP_ACTIVE_CLIENT() \
      (TC_TCP_OUT_CLIENT()->connected() ? TC_TCP_OUT_CLIENT() : \
       (Tinyc->tcp_cli_slot == 0 && Tinyc->tcp_server && Tinyc->tcp_client.connected()) ? &Tinyc->tcp_client : nullptr)

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
      // Server mode: accept new clients into tcp_client
      if (Tinyc->tcp_server && Tinyc->tcp_server->hasClient()) {
        Tinyc->tcp_client = Tinyc->tcp_server->available();
      }
      // Check active client (outgoing has priority over server-accepted)
      WiFiClient *_tc = TC_TCP_ACTIVE_CLIENT();
      if (_tc) avail = _tc->available();
      TC_PUSH(vm, avail);
      break;
    }
    case SYS_TCP_READ_STR: {  // wsrs(buf)
      int32_t ref = TC_POP(vm);
      int32_t count = 0;
      WiFiClient *_tc = TC_TCP_ACTIVE_CLIENT();
      if (_tc) {
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base) {
          uint16_t slen = _tc->available();
          if (slen > 254) slen = 254;  // cap to reasonable char[] size
          for (uint16_t i = 0; i < slen; i++) {
            base[i] = _tc->read();
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
      WiFiClient *_tc = TC_TCP_ACTIVE_CLIENT();
      if (_tc) {
        char buf[256];
        tc_ref_to_cstr(vm, ref, buf, sizeof(buf));
        _tc->write(buf, strlen(buf));
      }
      break;
    }
    case SYS_TCP_READ_ARR: {  // wsra(arr)
      int32_t ref = TC_POP(vm);
      int32_t count = 0;
      WiFiClient *_tc = TC_TCP_ACTIVE_CLIENT();
      if (_tc) {
        int32_t *base = tc_resolve_ref(vm, ref);
        if (base) {
          uint16_t slen = _tc->available();
          for (uint16_t i = 0; i < slen; i++) {
            base[i] = _tc->read();
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
      WiFiClient *_tc = TC_TCP_ACTIVE_CLIENT();
      if (_tc) {
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
            _tc->write(abf, dlen);
            free(abf);
          }
        }
      }
      break;
    }

    // ── TCP client (outgoing connections) ──────────────
    case SYS_TCP_CONNECT: {  // tcpConnect("ip", port) -> int (operates on currently selected slot)
      int32_t port = TC_POP(vm);
      int32_t ci   = TC_POP(vm);
      if (TasmotaGlobal.global_state.network_down) {
        TC_PUSH(vm, -2);
      } else {
        const char *ip = (ci >= 0 && ci < vm->const_count && vm->constants[ci].type == 1)
                         ? vm->constants[ci].str.ptr : nullptr;
        if (!ip) {
          TC_PUSH(vm, -1);
        } else {
          WiFiClient *_oc = TC_TCP_OUT_CLIENT();
          _oc->stop();  // close any previous connection on this slot
          if (_oc->connect(ip, port)) {
            _oc->setNoDelay(true);
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP client[%d] connected to %s:%d"), Tinyc->tcp_cli_slot, ip, port);
            TC_PUSH(vm, 0);
          } else {
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP client[%d] connect failed %s:%d"), Tinyc->tcp_cli_slot, ip, port);
            TC_PUSH(vm, -1);
          }
        }
      }
      break;
    }
    case SYS_TCP_CONNECT_REF: {  // tcpConnect(ip_char_array, port) -> int
      // _REF variant: IP string from runtime char array rather than literal.
      // Enables dynamic IP configuration from e.g. persist int[4] + sprintf.
      int32_t port = TC_POP(vm);
      int32_t ref  = TC_POP(vm);
      if (TasmotaGlobal.global_state.network_down) {
        TC_PUSH(vm, -2);
      } else {
        char ip_tmp[48];
        if (tc_ref_to_cstr(vm, ref, ip_tmp, sizeof(ip_tmp)) <= 0) {
          TC_PUSH(vm, -1);
        } else {
          WiFiClient *_oc = TC_TCP_OUT_CLIENT();
          _oc->stop();
          if (_oc->connect(ip_tmp, port)) {
            _oc->setNoDelay(true);
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP client[%d] connected to %s:%d"), Tinyc->tcp_cli_slot, ip_tmp, port);
            TC_PUSH(vm, 0);
          } else {
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP client[%d] connect failed %s:%d"), Tinyc->tcp_cli_slot, ip_tmp, port);
            TC_PUSH(vm, -1);
          }
        }
      }
      break;
    }
    case SYS_TCP_DISCONNECT: {  // tcpDisconnect()
      WiFiClient *_oc = TC_TCP_OUT_CLIENT();
      if (_oc->connected()) {
        _oc->stop();
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: TCP client[%d] disconnected"), Tinyc->tcp_cli_slot);
      }
      break;
    }
    case SYS_TCP_CONNECTED: {  // tcpConnected() -> int
      TC_PUSH(vm, TC_TCP_OUT_CLIENT()->connected() ? 1 : 0);
      break;
    }
    case SYS_TCP_SELECT: {  // tcpSelect(slot) — select outgoing TCP client slot
      int32_t slot = TC_POP(vm);
      if (slot < 0) slot = 0;
      if (slot >= TC_TCP_CLI_SLOTS) slot = TC_TCP_CLI_SLOTS - 1;
      Tinyc->tcp_cli_slot = (uint8_t)slot;
      break;
    }

    // ── MQTT Subscribe/Publish ────────────────────────
#ifdef USE_MQTT
    case SYS_MQTT_SUBSCRIBE: {  // mqttSubscribe("topic") -> int slot or -1
      int32_t ci = TC_POP(vm);
      const char *topic = tc_get_const_str(vm, ci);
      TC_PUSH(vm, topic ? tc_mqtt_subscribe(topic) : -1);
      break;
    }
    case SYS_MQTT_UNSUBSCRIBE: {  // mqttUnsubscribe("topic") -> int 0=ok or -1
      int32_t ci = TC_POP(vm);
      const char *topic = tc_get_const_str(vm, ci);
      TC_PUSH(vm, topic ? tc_mqtt_unsubscribe(topic) : -1);
      break;
    }
    case SYS_MQTT_PUBLISH_TO: {  // mqttPublish("topic", "payload") -> int 0=ok or -1
      int32_t pci = TC_POP(vm);   // payload const index
      int32_t tci = TC_POP(vm);   // topic const index
      const char *topic = tc_get_const_str(vm, tci);
      const char *payload = tc_get_const_str(vm, pci);
      if (topic && payload) {
        MqttPublishPayload(topic, payload);
        TC_PUSH(vm, 0);
      } else {
        TC_PUSH(vm, -1);
      }
      break;
    }
#else
    case SYS_MQTT_SUBSCRIBE:
    case SYS_MQTT_UNSUBSCRIBE:
    case SYS_MQTT_PUBLISH_TO:
      TC_POP(vm);
      if (id == SYS_MQTT_PUBLISH_TO) TC_POP(vm);
      TC_PUSH(vm, -1);
      break;
#endif  // USE_MQTT

    // ── Dynamic task spawn (ESP32 only) ───────────────
#ifdef ESP32
    case SYS_SPAWN_TASK: {  // spawnTask("name") -> int
      int32_t ci = TC_POP(vm);
      const char *name = tc_get_const_str(vm, ci);
      TC_PUSH(vm, name ? tc_spawn_task_create(name, 0) : -1);
      break;
    }
    case SYS_SPAWN_TASK_STACK: {  // spawnTask("name", stack_kb) -> int
      int32_t kb = TC_POP(vm);
      int32_t ci = TC_POP(vm);
      const char *name = tc_get_const_str(vm, ci);
      // 3 KB minimum (even simpler workers using AddLog need this much)
      // 16 KB upper clamp — plenty for HTTP/TLS clients.
      if (kb < 3)  kb = 3;
      if (kb > 16) kb = 16;
      TC_PUSH(vm, name ? tc_spawn_task_create(name, (uint16_t)kb) : -1);
      break;
    }
    case SYS_KILL_TASK: {  // killTask("name") -> int (0=signaled, -1=not running)
      int32_t ci = TC_POP(vm);
      const char *name = tc_get_const_str(vm, ci);
      TC_PUSH(vm, name ? tc_spawn_task_kill(name) : -1);
      break;
    }
    case SYS_TASK_RUNNING: {  // taskRunning("name") -> int (0/1)
      int32_t ci = TC_POP(vm);
      const char *name = tc_get_const_str(vm, ci);
      TC_PUSH(vm, name ? tc_spawn_task_running(name) : 0);
      break;
    }
#else
    case SYS_SPAWN_TASK:
    case SYS_KILL_TASK:
    case SYS_TASK_RUNNING:
      TC_POP(vm);
      TC_PUSH(vm, -1);
      break;
    case SYS_SPAWN_TASK_STACK:
      TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, -1);
      break;
#endif  // ESP32

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

    // ── Cross-VM shared key/value store ───────────────
    // All handlers: pop key as a const-pool index, look up key string, do
    // mutex-protected table op, push result if any. Missing-key reads return
    // 0 / 0.0 / "" so polling loops stay simple. See section header above.
    case SYS_SHARE_SET_INT: {
      int32_t val = TC_POP(vm);
      int32_t ki  = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      if (!key) break;
      tc_share_lock();
      int idx = tc_share_find_or_alloc(key);
      if (idx >= 0) { tc_share_table[idx].type = TC_SHARE_TYPE_INT; tc_share_table[idx].v.i = val; }
      tc_share_unlock();
      break;
    }
    case SYS_SHARE_GET_INT: {
      int32_t ki = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      int32_t out = 0;
      if (key) {
        tc_share_lock();
        int idx = tc_share_find(key);
        if (idx >= 0) {
          if      (tc_share_table[idx].type == TC_SHARE_TYPE_INT) out = tc_share_table[idx].v.i;
          else if (tc_share_table[idx].type == TC_SHARE_TYPE_FLT) out = (int32_t)tc_share_table[idx].v.f;
          // STR → leave 0 (caller used wrong getter)
        }
        tc_share_unlock();
      }
      TC_PUSH(vm, out);
      break;
    }
    case SYS_SHARE_SET_FLT: {
      float val   = TC_POPF(vm);
      int32_t ki  = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      if (!key) break;
      tc_share_lock();
      int idx = tc_share_find_or_alloc(key);
      if (idx >= 0) { tc_share_table[idx].type = TC_SHARE_TYPE_FLT; tc_share_table[idx].v.f = val; }
      tc_share_unlock();
      break;
    }
    case SYS_SHARE_GET_FLT: {
      int32_t ki = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      float out = 0.0f;
      if (key) {
        tc_share_lock();
        int idx = tc_share_find(key);
        if (idx >= 0) {
          if      (tc_share_table[idx].type == TC_SHARE_TYPE_FLT) out = tc_share_table[idx].v.f;
          else if (tc_share_table[idx].type == TC_SHARE_TYPE_INT) out = (float)tc_share_table[idx].v.i;
        }
        tc_share_unlock();
      }
      TC_PUSHF(vm, out);
      break;
    }
    case SYS_SHARE_SET_STR: {
      int32_t src_ref = TC_POP(vm);
      int32_t ki      = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      int32_t *src = tc_resolve_ref(vm, src_ref);
      if (!key || !src) break;
      // Pull source bytes out of the int32 array into a local buffer.
      int32_t srcMax = tc_ref_maxlen(vm, src_ref);
      char buf[TC_SHARE_STR_LEN + 1];
      int32_t si = 0;
      while (src[si] != 0 && si < srcMax && si < (int32_t)sizeof(buf) - 1) {
        buf[si] = (char)(src[si] & 0xFF);
        si++;
      }
      buf[si] = '\0';
      tc_share_lock();
      int idx = tc_share_find_or_alloc(key);
      if (idx >= 0) {
        tc_share_table[idx].type = TC_SHARE_TYPE_STR;
        strlcpy(tc_share_table[idx].s, buf, sizeof(tc_share_table[idx].s));
      }
      tc_share_unlock();
      break;
    }
    case SYS_SHARE_GET_STR: {
      int32_t dst_ref = TC_POP(vm);
      int32_t ki      = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      int32_t *dst = tc_resolve_ref(vm, dst_ref);
      int32_t copied = 0;
      if (key && dst) {
        int32_t maxSlots = tc_ref_maxlen(vm, dst_ref);
        char buf[TC_SHARE_STR_LEN + 1];
        buf[0] = '\0';
        tc_share_lock();
        int idx = tc_share_find(key);
        if (idx >= 0 && tc_share_table[idx].type == TC_SHARE_TYPE_STR) {
          strlcpy(buf, tc_share_table[idx].s, sizeof(buf));
        }
        tc_share_unlock();
        copied = tc_sprintf_to_ref(dst, maxSlots, buf);
      }
      TC_PUSH(vm, copied);
      break;
    }
    case SYS_SHARE_HAS: {
      int32_t ki = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      int32_t hit = 0;
      if (key) {
        tc_share_lock();
        hit = (tc_share_find(key) >= 0) ? 1 : 0;
        tc_share_unlock();
      }
      TC_PUSH(vm, hit);
      break;
    }
    case SYS_SHARE_DELETE: {
      int32_t ki = TC_POP(vm);
      const char *key = tc_get_const_str(vm, ki);
      int32_t removed = 0;
      if (key) {
        tc_share_lock();
        int idx = tc_share_find(key);
        if (idx >= 0) {
          tc_share_table[idx].type = TC_SHARE_TYPE_NONE;
          tc_share_table[idx].key[0] = '\0';
          tc_share_table[idx].s[0] = '\0';
          removed = 1;
        }
        tc_share_unlock();
      }
      TC_PUSH(vm, removed);
      break;
    }

    // ── Crypto: AES-128 / SHA-256 / HMAC-SHA256 ──────
    // Helper lambda-style macros local to this block. Pull a TinyC char[] ref
    // off the stack and copy to a stack-local C byte buffer. Returns 0 if the
    // ref is bogus or capacity is exceeded. Caller checks with `if (!_ok) ...`.
    case SYS_AES_ECB: {
#ifdef ESP32
      int32_t enc_flag  = TC_POP(vm);
      int32_t data_ref  = TC_POP(vm);
      int32_t key_ref   = TC_POP(vm);
      int32_t *key_arr  = tc_resolve_ref(vm, key_ref);
      int32_t *data_arr = tc_resolve_ref(vm, data_ref);
      if (!key_arr || !data_arr ||
          tc_ref_maxlen(vm, key_ref) < 16 || tc_ref_maxlen(vm, data_ref) < 16) {
        TC_PUSH(vm, 0); break;
      }
      uint8_t k[16], in[16], out[16];
      for (int i = 0; i < 16; i++) { k[i]  = (uint8_t)(key_arr[i]  & 0xFF); }
      for (int i = 0; i < 16; i++) { in[i] = (uint8_t)(data_arr[i] & 0xFF); }
      mbedtls_aes_context ctx;
      mbedtls_aes_init(&ctx);
      int rc = enc_flag ? mbedtls_aes_setkey_enc(&ctx, k, 128)
                        : mbedtls_aes_setkey_dec(&ctx, k, 128);
      if (rc == 0) {
        rc = mbedtls_aes_crypt_ecb(&ctx,
              enc_flag ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
              in, out);
      }
      mbedtls_aes_free(&ctx);
      if (rc == 0) {
        for (int i = 0; i < 16; i++) data_arr[i] = (int32_t)out[i];
        TC_PUSH(vm, 1);
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_AES_CBC: {
#ifdef ESP32
      int32_t enc_flag = TC_POP(vm);
      int32_t len      = TC_POP(vm);
      int32_t data_ref = TC_POP(vm);
      int32_t iv_ref   = TC_POP(vm);
      int32_t key_ref  = TC_POP(vm);
      int32_t *key_arr  = tc_resolve_ref(vm, key_ref);
      int32_t *iv_arr   = tc_resolve_ref(vm, iv_ref);
      int32_t *data_arr = tc_resolve_ref(vm, data_ref);
      if (!key_arr || !iv_arr || !data_arr ||
          (len & 15) != 0 || len <= 0 ||
          tc_ref_maxlen(vm, key_ref) < 16 ||
          tc_ref_maxlen(vm, iv_ref)  < 16 ||
          tc_ref_maxlen(vm, data_ref) < len) {
        TC_PUSH(vm, 0); break;
      }
      // Copy in. Stack alloc is fine for small TinyC payloads — the script
      // task has 5 KB+ stack and CBC operates a chunk at a time. For very
      // large payloads (> 4 KB) we malloc instead.
      uint8_t k[16], iv[16];
      uint8_t *buf = nullptr;
      bool heap = (len > 4096);
      uint8_t  stackbuf[4096];
      buf = heap ? (uint8_t*)malloc(len) : stackbuf;
      if (!buf) { TC_PUSH(vm, 0); break; }
      for (int i = 0; i < 16;  i++) k[i]  = (uint8_t)(key_arr[i] & 0xFF);
      for (int i = 0; i < 16;  i++) iv[i] = (uint8_t)(iv_arr[i]  & 0xFF);
      for (int i = 0; i < len; i++) buf[i] = (uint8_t)(data_arr[i] & 0xFF);
      mbedtls_aes_context ctx;
      mbedtls_aes_init(&ctx);
      int rc = enc_flag ? mbedtls_aes_setkey_enc(&ctx, k, 128)
                        : mbedtls_aes_setkey_dec(&ctx, k, 128);
      if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx,
              enc_flag ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
              len, iv, buf, buf);
      }
      mbedtls_aes_free(&ctx);
      if (rc == 0) {
        for (int i = 0; i < len; i++) data_arr[i] = (int32_t)buf[i];
        for (int i = 0; i < 16;  i++) iv_arr[i]   = (int32_t)iv[i];   // updated IV
        TC_PUSH(vm, 1);
      } else {
        TC_PUSH(vm, 0);
      }
      if (heap) free(buf);
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_HMAC_SHA256: {
#ifdef ESP32
      int32_t out_ref  = TC_POP(vm);
      int32_t dlen     = TC_POP(vm);
      int32_t data_ref = TC_POP(vm);
      int32_t klen     = TC_POP(vm);
      int32_t key_ref  = TC_POP(vm);
      int32_t *key_arr  = tc_resolve_ref(vm, key_ref);
      int32_t *data_arr = tc_resolve_ref(vm, data_ref);
      int32_t *out_arr  = tc_resolve_ref(vm, out_ref);
      if (!key_arr || !data_arr || !out_arr ||
          klen <= 0 || dlen < 0 ||
          tc_ref_maxlen(vm, key_ref) < klen ||
          tc_ref_maxlen(vm, data_ref) < dlen ||
          tc_ref_maxlen(vm, out_ref) < 32) {
        TC_PUSH(vm, 0); break;
      }
      // Stack-local copies; HMAC keys/data are usually small. For pathological
      // multi-KB key/data, callers can compute SHA-256 over chunks externally
      // — for now require sane sizes (≤ 1024 each).
      if (klen > 1024 || dlen > 4096) { TC_PUSH(vm, 0); break; }
      uint8_t kbuf[1024], dbuf[4096], out[32];
      for (int i = 0; i < klen; i++) kbuf[i] = (uint8_t)(key_arr[i]  & 0xFF);
      for (int i = 0; i < dlen; i++) dbuf[i] = (uint8_t)(data_arr[i] & 0xFF);
      const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
      int rc = mbedtls_md_hmac(info, kbuf, klen, dbuf, dlen, out);
      if (rc == 0) {
        for (int i = 0; i < 32; i++) out_arr[i] = (int32_t)out[i];
        TC_PUSH(vm, 1);
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_SHA256: {
#ifdef ESP32
      int32_t out_ref  = TC_POP(vm);
      int32_t dlen     = TC_POP(vm);
      int32_t data_ref = TC_POP(vm);
      int32_t *data_arr = tc_resolve_ref(vm, data_ref);
      int32_t *out_arr  = tc_resolve_ref(vm, out_ref);
      if (!data_arr || !out_arr || dlen < 0 ||
          tc_ref_maxlen(vm, data_ref) < dlen ||
          tc_ref_maxlen(vm, out_ref) < 32 ||
          dlen > 4096) {
        TC_PUSH(vm, 0); break;
      }
      uint8_t dbuf[4096], out[32];
      for (int i = 0; i < dlen; i++) dbuf[i] = (uint8_t)(data_arr[i] & 0xFF);
      // mbedtls_sha256(buf, len, out, is_sha224=0) — return 0 on success.
      int rc = mbedtls_sha256(dbuf, dlen, out, 0);
      if (rc == 0) {
        for (int i = 0; i < 32; i++) out_arr[i] = (int32_t)out[i];
        TC_PUSH(vm, 1);
      } else {
        TC_PUSH(vm, 0);
      }
#else
      TC_POP(vm); TC_POP(vm); TC_POP(vm);
      TC_PUSH(vm, 0);
#endif
      break;
    }

    case SYS_HEX2BIN: {
      // (hex_ref_or_const, hex_len, out_ref) → int bytes written (hex_len/2),
      // or -1 on bad input. Whitespace in hex is silently skipped.
      int32_t out_ref = TC_POP(vm);
      int32_t hex_len = TC_POP(vm);
      int32_t hex_ref = TC_POP(vm);
      int32_t *out_arr = tc_resolve_ref(vm, out_ref);
      if (!out_arr || hex_len < 0) { TC_PUSH(vm, -1); break; }
      // Accept either a const string (when caller passed a literal) or a TinyC
      // char[] ref. Copy hex chars into a local buffer first.
      char src[1024];
      int32_t src_len = 0;
      if (tc_is_const_ref(hex_ref)) {
        const char *cs = tc_get_const_str(vm, hex_ref);
        if (!cs) { TC_PUSH(vm, -1); break; }
        while (cs[src_len] && src_len < (int32_t)sizeof(src) - 1) {
          src[src_len] = cs[src_len]; src_len++;
        }
        if (hex_len > 0 && hex_len < src_len) src_len = hex_len;
      } else {
        int32_t *hex_arr = tc_resolve_ref(vm, hex_ref);
        if (!hex_arr) { TC_PUSH(vm, -1); break; }
        int32_t cap = tc_ref_maxlen(vm, hex_ref);
        if (hex_len > cap) hex_len = cap;
        if (hex_len > (int32_t)sizeof(src)) hex_len = sizeof(src);
        for (int i = 0; i < hex_len; i++) src[i] = (char)(hex_arr[i] & 0xFF);
        src_len = hex_len;
      }
      // Parse pairs of nibbles, skipping whitespace. Bail on malformed.
      int32_t out_cap = tc_ref_maxlen(vm, out_ref);
      int32_t written = 0;
      uint8_t hi = 0; int have_hi = 0;
      for (int i = 0; i < src_len; i++) {
        char c = src[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ':' || c == '-') continue;
        uint8_t v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else { TC_PUSH(vm, -1); break; }
        if (!have_hi) { hi = v; have_hi = 1; }
        else {
          if (written >= out_cap) break;
          out_arr[written++] = (int32_t)((hi << 4) | v);
          have_hi = 0;
        }
      }
      // If we broke from the loop with `-1` already pushed, we'd have consumed
      // the stack — but since `break` only exits the for-loop here, push now.
      TC_PUSH(vm, written);
      break;
    }

    case SYS_BIN2HEX: {
      // (bin_ref, bin_len, out_ref) → int chars written (bin_len*2). Lowercase.
      int32_t out_ref = TC_POP(vm);
      int32_t bin_len = TC_POP(vm);
      int32_t bin_ref = TC_POP(vm);
      int32_t *bin_arr = tc_resolve_ref(vm, bin_ref);
      int32_t *out_arr = tc_resolve_ref(vm, out_ref);
      if (!bin_arr || !out_arr || bin_len < 0) { TC_PUSH(vm, 0); break; }
      int32_t bin_cap = tc_ref_maxlen(vm, bin_ref);
      int32_t out_cap = tc_ref_maxlen(vm, out_ref);
      if (bin_len > bin_cap) bin_len = bin_cap;
      if (bin_len * 2 + 1 > out_cap) bin_len = (out_cap - 1) / 2;
      static const char hex[] = "0123456789abcdef";
      int32_t w = 0;
      for (int i = 0; i < bin_len; i++) {
        uint8_t b = (uint8_t)(bin_arr[i] & 0xFF);
        out_arr[w++] = (int32_t)hex[b >> 4];
        out_arr[w++] = (int32_t)hex[b & 0x0F];
      }
      if (w < out_cap) out_arr[w] = 0;  // NUL-terminate when room
      TC_PUSH(vm, w);
      break;
    }

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
 * Format v2: ['P','V','H'] [layout_hash u32 LE] [count u8]
 *            [index u16 LE, slotCount u16 LE, data: slotCount×4 bytes LE]
 * Legacy v1: ['P','V'] [count u8] ... — discarded on load because raw indexes
 *            cannot be safely mapped to a new binary's persist table.
 * The layout hash fingerprints (persist_count, [(index, slotCount)]). A layout
 * change (add/remove/reorder/resize persist vars) changes the hash → old file
 * auto-discarded → no more manual `UfsDelete /tinyc_pvars.bin` after edits.
\*********************************************************************************************/

// FNV-1a 32-bit hash over the persist table shape. Must produce the same value
// on save and on load for an unchanged layout.
static uint32_t tc_persist_layout_hash(TcVM *vm) {
  uint32_t h = 0x811c9dc5u;
  #define TC_FNV_BYTE(b) do { h ^= (uint8_t)(b); h *= 0x01000193u; } while(0)
  TC_FNV_BYTE(vm->persist_count);
  for (uint8_t i = 0; i < vm->persist_count; i++) {
    uint16_t idx = vm->persist[i].index;
    uint16_t cnt = vm->persist[i].count;
    TC_FNV_BYTE(idx & 0xff);
    TC_FNV_BYTE((idx >> 8) & 0xff);
    TC_FNV_BYTE(cnt & 0xff);
    TC_FNV_BYTE((cnt >> 8) & 0xff);
  }
  #undef TC_FNV_BYTE
  return h;
}

static void tc_persist_save(TcVM *vm) {
  if (vm->persist_count == 0 || vm->persist_file[0] == '\0') return;
#ifdef USE_UFILESYS
  // Calculate total size: 3 (magic 'P','V','H') + 4 (layout hash u32) + 1 (count) + entries
  uint16_t total = 8;
  for (uint8_t i = 0; i < vm->persist_count; i++) {
    total += 4 + vm->persist[i].count * 4;  // index(2) + slotCount(2) + data
  }
  uint8_t *buf = (uint8_t *)malloc(total);
  if (!buf) return;

  uint32_t hash = tc_persist_layout_hash(vm);
  uint16_t pos = 0;
  buf[pos++] = 'P';
  buf[pos++] = 'V';
  buf[pos++] = 'H';  // v2 magic — layout-fingerprinted
  buf[pos++] = hash & 0xFF;
  buf[pos++] = (hash >> 8) & 0xFF;
  buf[pos++] = (hash >> 16) & 0xFF;
  buf[pos++] = (hash >> 24) & 0xFF;
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
  if (fsize < 8) { f.close(); return; }  // min = 'P' 'V' 'H' hash[4] count

  uint8_t *buf = (uint8_t *)malloc(fsize);
  if (!buf) { f.close(); return; }
  f.read(buf, fsize);
  f.close();

  if (buf[0] != 'P' || buf[1] != 'V') { free(buf); return; }
  if (buf[2] != 'H') {
    // Legacy format (pre-layout-hash) — raw indexes can't be safely mapped
    // across layout changes. Discard rather than risk corrupting state.
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: legacy persist file — resetting %s"), vm->persist_file);
    free(buf);
    if (ufsp) ufsp->remove(vm->persist_file);
    return;
  }
  uint32_t stored_hash = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8)
                       | ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);
  uint32_t expected_hash = tc_persist_layout_hash(vm);
  if (stored_hash != expected_hash) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: persist layout changed (%08X->%08X) — resetting %s"),
           stored_hash, expected_hash, vm->persist_file);
    free(buf);
    if (ufsp) ufsp->remove(vm->persist_file);
    return;
  }
  uint8_t count = buf[7];
  uint16_t pos = 8;

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

  // Reset hot-path dispatch cache first thing — ensures no stale indices
  // from a previous load survive if we early-return on a bad header.
  for (int k = 0; k < TC_CB_COUNT; k++) vm->cb_index[k] = -1;

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

  // Allocate const_data buffer based on pre-scan (minimum 64 bytes).
  // Internal DRAM first; PSRAM only as fallback so small scripts stay fast.
  uint16_t alloc_cdata = prescan_data < 64 ? 64 : prescan_data;
  vm->const_data = (char *)calloc(alloc_cdata, 1);
#ifdef ESP32
  if (!vm->const_data) {
    vm->const_data = (char *)heap_caps_malloc(alloc_cdata, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (vm->const_data) memset(vm->const_data, 0, alloc_cdata);
  }
#endif
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
  // Problem #1: warn if constant pool was truncated (more entries in binary than TC_MAX_CONSTANTS)
  if (offset < const_end) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: constant pool truncated — loaded %d/%d entries (max %d). Recompile with fewer string literals."),
           vm->const_count, prescan_count, TC_MAX_CONSTANTS);
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
      // Problem #2: log descriptive error before OOM return
      if (total_heap > TC_MAX_HEAP) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: heap OOM — need %u slots (%u KB), max %u (%u KB). Reduce array sizes."),
               total_heap, (unsigned)(total_heap * 4 / 1024), (unsigned)TC_MAX_HEAP, (unsigned)(TC_MAX_HEAP * 4 / 1024));
        return TC_ERR_STACK_OVERFLOW;
      }
      vm->heap_data = (int32_t *)special_calloc(total_heap, sizeof(int32_t));
      if (!vm->heap_data) {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: heap alloc failed (%u slots, %u KB) — not enough free heap."),
               total_heap, (unsigned)(total_heap * 4 / 1024));
        return TC_ERR_STACK_OVERFLOW;
      }
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
    // Warn if the bytecode declares more callbacks than we can hold.
    // Previously silent truncation caused callbacks past index 10 to never
    // fire (e.g. EverySecond if it came after 10 other well-known callbacks).
    if (count > TC_MAX_CALLBACKS) {
      AddLog(LOG_LEVEL_ERROR,
        PSTR("TCC: bytecode has %d callbacks but TC_MAX_CALLBACKS=%d — %d dropped"),
        count, TC_MAX_CALLBACKS, count - TC_MAX_CALLBACKS);
    }
  }

  // Build hot-path callback cache (one strcmp pass, replaces per-tick strcmps).
  // After this, dispatch from FUNC_LOOP / FUNC_EVERY_*_MSECOND / FUNC_EVERY_SECOND
  // is a direct array index instead of a linear strcmp scan.
  for (int k = 0; k < TC_CB_COUNT; k++) {
    vm->cb_index[k] = -1;
    for (uint8_t i = 0; i < vm->callback_count; i++) {
      if (strcmp(vm->callbacks[i].name, TC_CB_NAME[k]) == 0) {
        vm->cb_index[k] = (int8_t)i;
        break;
      }
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

// Internal: invoke callback at known index. Caller must have already validated
// that idx is in range and that vm is halted/error-free. `name` is used only
// for log/crash messages.
// The full PAUSED handler MUST stay in this body — see project_tinyc notes.
static int tc_vm_call_callback_idx(TcVM *vm, int idx, const char *name) {
  // Save state
  uint8_t saved_frame_count = vm->frame_count;
  uint16_t saved_pc = vm->pc;
  uint16_t saved_sp = vm->sp;
  // Bug #2 fix: reclaim any heap allocations made during the callback
  // (bump allocator doesn't rewind heap_used even on tc_heap_free_handle).
  // Snapshot heap position + handle count; restore on exit.
  uint16_t saved_heap_used = vm->heap_used;
  uint8_t  saved_heap_handle_count = vm->heap_handle_count;

  // Temporarily un-halt and set up callback frame
  vm->halted = false;
  vm->running = true;
  if (vm->frame_count >= TC_MAX_FRAMES) {
    // Keep VM in a consistent halted state on frame overflow
    vm->halted = true;
    vm->running = false;
    return TC_ERR_FRAME_OVERFLOW;
  }
  TcFrame *frame = &vm->frames[vm->frame_count];
  frame->return_pc = 0;  // detect return by frame_count drop
  frame->saved_sp = vm->sp;  // host-pushed args live on top; must balance at RET
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
      tc_crash_log(err, vm->pc, vm->instruction_count, name);
      break;
    }
    vm->instruction_count++;
    if (++count > TC_CALLBACK_MAX_INSTR) {
      vm->error = TC_ERR_INSTRUCTION_LIMIT;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Instruction limit in '%s' at PC=%u (%u instr)"), name, vm->pc, count);
      tc_crash_log(TC_ERR_INSTRUCTION_LIMIT, vm->pc, count, name);
      break;
    }
  }

  // Restore halted state (globals persist)
  vm->halted = true;
  vm->running = false;
  vm->pc = saved_pc;
  // Bug #1 fix: restore SP. Without this, every callback that left anything on
  // the stack (args not consumed, RET_VAL's return value, partial push on error)
  // leaks a slot per call → SP drifts → stack corruption after hours/days.
  vm->sp = saved_sp;

  // Clean up any leftover frames from the callback
  while (vm->frame_count > saved_frame_count) {
    tc_frame_free(&vm->frames[--vm->frame_count]);
  }
  vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;

  // Bug #2 fix: reclaim heap allocations made during the callback.
  // Mark handles created during the callback as dead so the handle table
  // doesn't grow unbounded, then rewind the bump pointer.
  if (vm->heap_handles && vm->heap_handle_count > saved_heap_handle_count) {
    for (uint8_t i = saved_heap_handle_count; i < vm->heap_handle_count; i++) {
      vm->heap_handles[i].alive = false;
    }
    vm->heap_handle_count = saved_heap_handle_count;
  }
  vm->heap_used = saved_heap_used;

  // Flush output to Tasmota
  tc_output_flush();

  return vm->error;
}

// Public: name-based dispatch (legacy, still used for non-hot-path callbacks
// like "JsonCall", "WebUI", "WebCall", "TouchButton", "HomeKitWrite", "WebOn",
// "UdpCall", "WebPage", "Command", etc.). Does one strcmp scan.
static int tc_vm_call_callback(TcVM *vm, const char *name) {
  int idx = -1;
  for (int i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) { idx = i; break; }
  }
  if (idx < 0) return TC_OK;  // callback not defined, silently skip
  if (!vm->halted || vm->error != TC_OK) return vm->error;
  return tc_vm_call_callback_idx(vm, idx, name);
}

// Public: ID-based dispatch for hot-path callbacks. Zero strcmp — the cache
// was populated once at tc_vm_load(). If the callback isn't defined, returns
// immediately without even checking halted/error state (saves a memory read).
//
// The idx < callback_count check is belt-and-suspenders: a freshly-calloc'd
// slot has cb_index all zeros (valid-looking indices) but callback_count == 0.
// TinyCStopVM fires OnExit via this path on never-loaded slots, which without
// this guard would jump to callbacks[0].address == 0 → bounds error at PC=0.
static int tc_vm_call_callback_id(TcVM *vm, TcCallbackId cid) {
  if ((unsigned)cid >= TC_CB_COUNT) return TC_OK;
  int8_t idx = vm->cb_index[cid];
  if (idx < 0 || (uint8_t)idx >= vm->callback_count) return TC_OK;
  if (!vm->halted || vm->error != TC_OK) return vm->error;
  return tc_vm_call_callback_idx(vm, idx, TC_CB_NAME[cid]);
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

  // Save heap position so we can reclaim the temp buffer after the callback.
  // tc_heap_free_handle() only marks alive=false but never rewinds heap_used
  // (bump allocator), so without this every Command call leaks 'slots' permanently.
  uint16_t saved_heap_used = vm->heap_used;

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

  // Push heap ref onto stack for the callback parameter.
  // Capture SP *before* the push: tc_vm_call_callback_idx captures its own
  // saved_sp at entry (= post-push) and restores to it on exit, which would
  // re-materialise the arg the callback already consumed via STORE_LOCAL.
  // Pin vm->sp back to pre-push here so the net effect across the call is 0.
  uint16_t pre_push_sp = vm->sp;
  int32_t ref = 0xC0000000 | handle;
  TC_PUSH(vm, ref);

  // Call the callback (it will pop the ref as its parameter)
  int err = tc_vm_call_callback(vm, name);

  // Balance the pre-call push (covers both the success path — idx restored
  // to post-push — and the early-return path where the arg was never consumed).
  vm->sp = pre_push_sp;

  // Free temp buffer and rewind bump allocator to reclaim the space
  tc_heap_free_handle(vm, handle);
  vm->heap_used = saved_heap_used;

  return err;
}

// Call a callback with TWO char-array string args: cb(char a[], char b[]).
// Used for OnMqttData(topic, payload). Both strings copied into temp heap
// buffers, refs pushed left-to-right (first param bottom of stack), then
// temp buffers freed and heap bump rewound.
static int tc_vm_call_callback_str2(TcVM *vm, const char *name,
                                     const char *str1, const char *str2) {
  int idx = -1;
  for (int i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) { idx = i; break; }
  }
  if (idx < 0) return TC_OK;
  if (!vm->halted || vm->error != TC_OK) return vm->error;

  int32_t len1 = str1 ? strlen(str1) : 0;
  int32_t len2 = str2 ? strlen(str2) : 0;
  int32_t slots1 = (len1 < 511 ? len1 : 511) + 1;
  int32_t slots2 = (len2 < 511 ? len2 : 511) + 1;

  uint16_t saved_heap_used = vm->heap_used;

  int h1 = tc_heap_alloc(vm, slots1);
  int h2 = tc_heap_alloc(vm, slots2);
  if (h1 < 0 || h2 < 0) {
    vm->heap_used = saved_heap_used;
    return TC_OK;
  }

  int32_t *buf1 = &vm->heap_data[vm->heap_handles[h1].offset];
  for (int32_t i = 0; i < slots1 - 1; i++) buf1[i] = (int32_t)(uint8_t)str1[i];
  buf1[slots1 - 1] = 0;

  int32_t *buf2 = &vm->heap_data[vm->heap_handles[h2].offset];
  for (int32_t i = 0; i < slots2 - 1; i++) buf2[i] = (int32_t)(uint8_t)str2[i];
  buf2[slots2 - 1] = 0;

  // Push in declaration order: topic first (bottom), payload second (top).
  // TinyC frame builder reads params left-to-right from the pushed stack.
  // Pin pre-push SP so tc_vm_call_callback_idx's saved_sp restore doesn't
  // re-materialise the 2 args the callback consumed (see _str variant).
  uint16_t pre_push_sp = vm->sp;
  TC_PUSH(vm, (int32_t)(0xC0000000 | h1));  // first  param: topic
  TC_PUSH(vm, (int32_t)(0xC0000000 | h2));  // second param: payload

  int err = tc_vm_call_callback(vm, name);
  vm->sp = pre_push_sp;   // balance the 2 pushes

  tc_heap_free_handle(vm, h2);
  tc_heap_free_handle(vm, h1);
  vm->heap_used = saved_heap_used;

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
        frame->saved_sp = vm->sp;       // caller's SP (with args on top)
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
        // Post-RET SP should be ≤ caller-at-CALL SP (callee consumed its args,
        // may or may not have left them unused). Higher SP means the callee
        // pushed something it never popped — an unbounded leak over many calls.
        if (vm->sp > f->saved_sp) {
          AddLog(LOG_LEVEL_ERROR,
                 PSTR("TCC: SP leak at void return: sp=%u expected<=%u ret_pc=%u"),
                 (unsigned)vm->sp, (unsigned)f->saved_sp, (unsigned)f->return_pc);
        }
        vm->pc = f->return_pc;
        tc_frame_free(f);  // free returning frame's locals
        vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0; }
      break;

    case OP_RET_VAL:
      a = TC_POP(vm);
      if (vm->frame_count == 0) { tc_frame_free(&vm->frames[0]); TC_PUSH(vm, a); vm->halted = true; vm->running = false; break; }
      { TcFrame *f = &vm->frames[--vm->frame_count];
        // Same leak check as void RET — return value is already popped into `a`,
        // so the remaining stack must not have grown past the CALL-time SP.
        if (vm->sp > f->saved_sp) {
          AddLog(LOG_LEVEL_ERROR,
                 PSTR("TCC: SP leak at value return: sp=%u expected<=%u ret_pc=%u"),
                 (unsigned)vm->sp, (unsigned)f->saved_sp, (unsigned)f->return_pc);
        }
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
      if ((uint32_t)(idx+a) >= TC_MAX_LOCALS) {
        tc_log_bounds("local[]", idx+a, TC_MAX_LOCALS, vm->pc);
        return TC_ERR_BOUNDS;
      }
      TC_PUSH(vm, vm->frames[vm->fp].locals[idx+a]); break;
    case OP_STORE_LOCAL_ARR:
      idx=tc_read_u8(vm); b=TC_POP(vm); a=TC_POP(vm);
      if ((uint32_t)(idx+a) >= TC_MAX_LOCALS) {
        tc_log_bounds("local[]", idx+a, TC_MAX_LOCALS, vm->pc);
        return TC_ERR_BOUNDS;
      }
      vm->frames[vm->fp].locals[idx+a]=b; break;
    case OP_LOAD_GLOBAL_ARR:
      addr=tc_read_u16(vm); a=TC_POP(vm);
      if ((uint32_t)(addr+a) >= vm->globals_size) {
        tc_log_bounds("global[]", addr+a, vm->globals_size, vm->pc);
        return TC_ERR_BOUNDS;
      }
      TC_PUSH(vm, vm->globals[addr+a]); break;
    case OP_STORE_GLOBAL_ARR:
      addr=tc_read_u16(vm); b=TC_POP(vm); a=TC_POP(vm);
      if ((uint32_t)(addr+a) >= vm->globals_size) {
        tc_log_bounds("global[]", addr+a, vm->globals_size, vm->pc);
        return TC_ERR_BOUNDS;
      }
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
        uint32_t sz = (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive)
                      ? vm->heap_handles[handle].size : 0;
        char kind[16]; snprintf(kind, sizeof(kind), "heap[%u]", (unsigned)handle);
        tc_log_bounds(kind, a, sz, vm->pc);
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
        uint32_t sz = (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive)
                      ? vm->heap_handles[handle].size : 0;
        char kind[16]; snprintf(kind, sizeof(kind), "heap[%u]", (unsigned)handle);
        tc_log_bounds(kind, a, sz, vm->pc);
        return TC_ERR_BOUNDS;
      }
      vm->heap_data[vm->heap_handles[handle].offset + a] = b;
      break;
    }
    case OP_ADDR_HEAP: {
      uint8_t handle = tc_read_u8(vm);
      // Pack: 0xC0000000 | handle (offset = 0)
      TC_PUSH(vm, tc_make_heap_ref(handle, 0));
      break;
    }
    case OP_ADDR_HEAP_OFF: {
      uint8_t handle = tc_read_u8(vm);
      int32_t off = TC_POP(vm);   // slot offset (int32, treated unsigned 14-bit)
      if (off < 0) off = 0;
      if (off > 0x3FFF) off = 0x3FFF;
      TC_PUSH(vm, tc_make_heap_ref(handle, (uint16_t)off));
      break;
    }

    // ── Runtime array ref (ref params) ──
    case OP_LOAD_REF_ARR: {
      idx = tc_read_u8(vm);
      a = TC_POP(vm);  // index
      if ((uint32_t)idx >= TC_MAX_LOCALS) return TC_ERR_BOUNDS;
      int32_t ref = vm->frames[vm->fp].locals[idx];
      int32_t *buf = tc_resolve_ref(vm, ref);
      if (!buf) return TC_ERR_BOUNDS;
      int32_t maxLen = tc_ref_maxlen(vm, ref);
      if (a < 0 || a >= maxLen) return TC_ERR_BOUNDS;
      TC_PUSH(vm, buf[a]);
      break;
    }
    case OP_STORE_REF_ARR: {
      idx = tc_read_u8(vm);
      b = TC_POP(vm);  // value
      a = TC_POP(vm);  // index
      if ((uint32_t)idx >= TC_MAX_LOCALS) return TC_ERR_BOUNDS;
      int32_t ref = vm->frames[vm->fp].locals[idx];
      int32_t *buf = tc_resolve_ref(vm, ref);
      if (!buf) return TC_ERR_BOUNDS;
      int32_t maxLen = tc_ref_maxlen(vm, ref);
      if (a < 0 || a >= maxLen) return TC_ERR_BOUNDS;
      buf[a] = b;
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
    case OP_SYSCALL2: {
      uint16_t idx2 = tc_read_u16(vm);
      return tc_syscall(vm, idx2);
    }

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
    // Heap address with offset
    _dispatch[0xA6] = &&_op_addr_heap_off;
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
      frame->saved_sp = _sp;                        // caller SP w/ args on top
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
      if (_sp > f->saved_sp) {                      // SP-leak detection (see switch)
        AddLog(LOG_LEVEL_ERROR,
               PSTR("TCC: SP leak at void return: sp=%u expected<=%u ret_pc=%u"),
               (unsigned)_sp, (unsigned)f->saved_sp, (unsigned)f->return_pc);
      }
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
      if (_sp > f->saved_sp) {                      // SP-leak detection (see switch)
        AddLog(LOG_LEVEL_ERROR,
               PSTR("TCC: SP leak at value return: sp=%u expected<=%u ret_pc=%u"),
               (unsigned)_sp, (unsigned)f->saved_sp, (unsigned)f->return_pc);
      }
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
    if ((uint32_t)(_idx+_a) >= TC_MAX_LOCALS) {
      tc_log_bounds("local[]", _idx+_a, TC_MAX_LOCALS, _pc);
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    TC_IPUSH(vm->frames[vm->fp].locals[_idx+_a]);
    NEXT();
  _op_store_local_arr:
    _idx = _RD_U8(); _b = TC_IPOP(); _a = TC_IPOP();
    if ((uint32_t)(_idx+_a) >= TC_MAX_LOCALS) {
      tc_log_bounds("local[]", _idx+_a, TC_MAX_LOCALS, _pc);
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    vm->frames[vm->fp].locals[_idx+_a] = _b;
    NEXT();
  _op_load_global_arr:
    _addr = _RD_U16(); _a = TC_IPOP();
    if ((uint32_t)(_addr+_a) >= _gsz) {
      tc_log_bounds("global[]", _addr+_a, _gsz, _pc);
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    TC_IPUSH(vm->globals[_addr+_a]);
    NEXT();
  _op_store_global_arr:
    _addr = _RD_U16(); _b = TC_IPOP(); _a = TC_IPOP();
    if ((uint32_t)(_addr+_a) >= _gsz) {
      tc_log_bounds("global[]", _addr+_a, _gsz, _pc);
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
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
      uint32_t sz = (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive)
                    ? vm->heap_handles[handle].size : 0;
      char kind[16]; snprintf(kind, sizeof(kind), "heap[%u]", (unsigned)handle);
      tc_log_bounds(kind, _a, sz, _pc);
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
      uint32_t sz = (handle < TC_MAX_HEAP_HANDLES && vm->heap_handles && vm->heap_handles[handle].alive)
                    ? vm->heap_handles[handle].size : 0;
      char kind[16]; snprintf(kind, sizeof(kind), "heap[%u]", (unsigned)handle);
      tc_log_bounds(kind, _a, sz, _pc);
      _err = TC_ERR_BOUNDS; goto _vm_exit;
    }
    vm->heap_data[vm->heap_handles[handle].offset + _a] = _b;
    NEXT();
  }
  _op_addr_heap: {
    uint8_t handle = _RD_U8();
    TC_IPUSH(tc_make_heap_ref(handle, 0));
    NEXT();
  }
  _op_addr_heap_off: {
    uint8_t handle = _RD_U8();
    int32_t off = TC_IPOP();
    if (off < 0) off = 0;
    if (off > 0x3FFF) off = 0x3FFF;
    TC_IPUSH(tc_make_heap_ref(handle, (uint16_t)off));
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
        tc_crash_log(err, vm->pc, vm->instruction_count, "main");
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

    // Phase 2: If TaskLoop callback exists, loop calling it in this task.
    // Use the load-time cache — no strcmp needed.
    int tl_idx = vm->cb_index[TC_CB_TASK_LOOP];
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
          frame->saved_sp = vm->sp;   // see tc_vm_call_callback_idx — needed for RET leak check
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
                tc_crash_log(err, vm->pc, vm->instruction_count, "TaskLoop");
                break;
              }
              count++;
              vm->instruction_count++;
              // Yield periodically to feed WDT (no instruction limit in TaskLoop)
              if ((count & 0xFFFF) == 0) {
                vm->halted = true;
                vm->running = false;
                if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
                vTaskDelay(1);
                if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
                tc_current_slot = slot;
                vm->halted = false;
                vm->running = true;
                if (slot->task_stop) break;
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
// Bug #1 fix: mutex-first. Checking halted/error without the mutex is a TOCTOU race
// on dual-core ESP32 — Core 1's TaskLoop can flip halted=false between our check and
// the mutex take, causing concurrent VM execution (PC=0 crash, frame corruption).
static void tc_slot_callback(TcSlot *s, const char *name) {
  if (!s || !s->loaded) return;
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
  if (!s->vm.halted || s->vm.error != TC_OK) {
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
    return;
  }
  tc_current_slot = s;
  tc_vm_call_callback(&s->vm, name);
  tc_current_slot = nullptr;
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
}

// Forward decl — defined further below after Tinyc state struct is in scope.
// Kept in vm.h because TcCallbackId argument confuses Arduino auto-prototyper.
static void tc_all_callbacks_id(TcCallbackId cid);

// ID-based variant for hot-path well-known callbacks (EverySecond, EveryLoop, ...).
// Fast-exit without taking the mutex if the callback isn't defined — this is the
// common case (most scripts use 1-2 of the 13 well-known callbacks), so 50-ms-tick
// sweeps across all slots avoid per-slot mutex churn entirely.
static void tc_slot_callback_id(TcSlot *s, TcCallbackId cid) {
  if (!s || !s->loaded) return;
  if ((unsigned)cid >= TC_CB_COUNT) return;
  // Fast path: script doesn't define this callback — skip mutex + VM state read.
  // cb_index is populated once at load and never mutated at runtime, so this
  // read is race-free (a concurrent tc_vm_load would imply the slot isn't
  // "loaded" yet, which we already gated above).
  if (s->vm.cb_index[cid] < 0) return;
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
  if (!s->vm.halted || s->vm.error != TC_OK) {
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
    return;
  }
  tc_current_slot = s;
  tc_vm_call_callback_id(&s->vm, cid);
  tc_current_slot = nullptr;
#ifdef ESP32
  if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
}

// Sweep all slots, calling the ID'd callback on each. Zero strcmps.
// Defined here (not in .ino) because TcCallbackId param confuses Arduino's
// auto-prototyper (same reason tc_slot_callback* lives in vm.h).
static void tc_all_callbacks_id(TcCallbackId cid) {
  // Mini-Scripter ticks first (cheap, no slot lookup needed). Bytecode is
  // populated only after a successful smlScripterLoad() — otherwise no-op.
  if (cid == TC_CB_EVERY_100_MSECOND) tc_mscr_tick_f();
  else if (cid == TC_CB_EVERY_SECOND) tc_mscr_tick_s();
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (s) tc_slot_callback_id(s, cid);
  }
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
  // Kill any spawned (shared-VM) tasks FIRST so they stop touching the VM
  // before the main task is torn down and the heap is freed. Tasks observe
  // their stop_requested flag at the next instruction boundary or delay.
  {
    uint8_t sidx = 0xFF;
    for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
      if (Tinyc->slots[i] == s) { sidx = i; break; }
    }
    if (sidx != 0xFF) {
      tc_spawn_task_cleanup_slot(sidx);
    }
  }

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
  tc_vm_call_callback_id(&s->vm, TC_CB_ON_EXIT);
  tc_output_flush();
  tc_current_slot = nullptr;

  // Auto-save persist variables before clearing VM
  tc_persist_save(&s->vm);

  s->running = false;
  s->vm.running = false;
  tc_free_all_frames(&s->vm);
  tc_heap_free_all(&s->vm);
  s->vm.halted = false;  // Problem 12: prevent callbacks from firing on freed heap
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
  tc_serial_close_all();
  tc_img_store_free();
  // Clear mini-scripter state — otherwise the >F/>S bytecode from the previous
  // .tcb keeps firing on EverySecond/Every100ms ticks and (e.g.) re-emits the
  // IEC 62056-21 wake-up handshake even after the script that loaded it is
  // gone. Next smlScripterLoad() in a new .tcb repopulates; scripts that
  // don't call it get a silent mscr (loaded=0 → tick no-ops).
  memset(&tc_mscr, 0, sizeof(tc_mscr));
#ifdef ESP32
  // Stop I2S output if active
  if (Tinyc->i2s_tx_handle) {
    i2s_channel_disable(Tinyc->i2s_tx_handle);
    i2s_del_channel(Tinyc->i2s_tx_handle);
    Tinyc->i2s_tx_handle = nullptr;
  }
  if (Tinyc->i2s_pcm_buf) {
    free(Tinyc->i2s_pcm_buf);
    Tinyc->i2s_pcm_buf = nullptr;
  }
#endif
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
  BaseType_t ret = xTaskCreate(tc_vm_task, taskname, TC_VM_TASK_STACK, s, 1, &s->task_handle);
#else
  // Dual-core ESP32/S3 -- pin to core 1
  BaseType_t ret = xTaskCreatePinnedToCore(tc_vm_task, taskname, TC_VM_TASK_STACK, s, 1, &s->task_handle, 1);
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
