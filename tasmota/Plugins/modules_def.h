#ifndef MODULE_TYPE_SENSOR
// MODULE_TYPE_BLIB: pure binary library — no Tasmota lifecycle hooks
// (no FUNC_INIT / FUNC_LOOP / FUNC_COMMAND etc). Only purpose is to
// expose a TC_EXPORT[] table of named native functions for callers
// inside firmware (TinyC scripts and other plugins) to invoke at full
// native speed. See the TC_EXPORT struct + pFUNC_GET_TINYC_EXPORTS
// selector below for the discovery contract.
enum {MODULE_TYPE_SENSOR, MODULE_TYPE_LIGHT, MODULE_TYPE_ENERGY, MODULE_TYPE_DRIVER, MODULE_TYPE_BLIB};
enum {ARCH_ESP8266, ARCH_ESP32,ARCH_ESP32_RV,ARCH_ESP32_P4};
#endif

enum {iD_TEMPERATURE,iD_PRESSURE,iD_HUMIDITY,iD_ABSOLUTE_HUMIDITY,iD_DISTANCE};

#define MODULE_SYNC 0x55aaFC4A

#undef CURR_ARCH
#ifdef ESP8266
#define CURR_ARCH ARCH_ESP8266
#else
#ifdef __riscv
#if defined(CONFIG_IDF_TARGET_ESP32P4)
#define CURR_ARCH ARCH_ESP32_P4      // RV32IMAFC hard-float (ilp32f) — ABI-incompatible with C3/C6 (_32r)
#else
#define CURR_ARCH ARCH_ESP32_RV      // RV32IMC/IMAC soft-float (ilp32) — C3 / C6
#endif
#else
#define CURR_ARCH ARCH_ESP32
#endif
#endif

#define pFUNC_DEINIT 999

// Selector dispatched by the plugin loader to a MODULE_TYPE_BLIB
// module immediately after it's mapped into flash. The blib's
// mod_func_execute returns the address of its TC_EXPORT table (an
// array terminated by a sentinel entry with name == NULL); the loader
// then walks the table, applies EXEC_OFFSET to each fn pointer, and
// registers the entries in a global lookup the TinyC `bcall` syscall
// reads. Kept distinct from the pXsnsFunctions enum range (which
// covers Tasmota lifecycle hooks 0..149 + 200..) so it can never
// collide with future additions there.
#define pFUNC_GET_TINYC_EXPORTS 1000

// ── TinyC-callable export ABI ────────────────────────────────────
// A binary library (xblib_*) exposes a list of named native functions.
// TinyC scripts (and, eventually, other plugins) invoke them via a
// `bcall("name", args...)` syscall that does name-based lookup on
// first call and caches the resolved fn pointer per call site.
//
// Ret/arg type enums are deliberately small (5 + 5 values, fits in a
// uint8_t) so the TC_EXPORT struct stays compact; the marshalling
// shim in the firmware decides how to push and pop values from the
// TinyC VM stack based on these tags.
//
// To add new types later (e.g. struct-by-value, double, callback
// fn-ptr), append to the enum. The blib's exports table is versioned
// implicitly by which type tags it uses — a blib that lists TC_ARG_
// constants the firmware doesn't recognise will be rejected at
// register-time with a clear log line.
typedef enum {
  TC_RET_VOID  = 0,
  TC_RET_INT   = 1,
  TC_RET_FLOAT = 2,
} TC_RET_TYPE;

typedef enum {
  TC_ARG_END   = 0,    // sentinel — pads out unused slots in arg_types[]
  TC_ARG_INT   = 1,
  TC_ARG_FLOAT = 2,
  TC_ARG_BUF   = 3,    // char[] / int[] — passed as (void *ptr, int len)
                       // counts as TWO args at the TinyC call site.
  TC_ARG_REF   = 4,    // int& / float& — out-param, callee writes through.
} TC_ARG_TYPE;

#define TC_MAX_ARGS 8

typedef struct {
  const char *name;                        // e.g. "mb_crc16"; NULL = end-of-list
  void       *fn;                          // native function pointer
  uint8_t     argc;                        // number of TinyC-visible args
  uint8_t     ret_type;                    // TC_RET_*
  uint8_t     arg_types[TC_MAX_ARGS];      // each TC_ARG_*; trailing TC_ARG_END
} TC_EXPORT;

// Global registry slot: one per registered blib export. Populated by
// xdrv_123_plugins.ino's tc_blib_register_module() at module load,
// looked up by name from xdrv_124_tinyc_vm.h's SYS_BLIB_CALL handler.
// Field layout shared across both translation units — declared here
// (in the already-shared modules_def.h) so they don't drift.
typedef struct {
  char       *name;                        // malloc'd DRAM copy, NUL-terminated
  void       *fn;                          // EXEC_OFFSET-corrected native fn pointer
  uint8_t     argc;
  uint8_t     ret_type;
  uint8_t     arg_types[TC_MAX_ARGS];
  uint8_t     module_idx;
} TC_BLIB_REG_ENTRY;

typedef union {
  uint8_t data;
  struct {
    uint8_t spare1 : 1;
    uint8_t spare2 : 1;
    uint8_t spare3 : 1;
    uint8_t spare4 : 1;
    uint8_t every_second : 1;
    uint8_t web_sensor : 1;
    uint8_t json_append : 1;
    uint8_t initialized : 1;
  };
} MOD_FLAGS;

typedef struct {
  void (*pvTaskCode)(void*);
  const char *constpcName;
  uint32_t usStackDepth;
  void *constpvParameters;
  uint32_t uxPriority;
  void *constpvCreatedTask;
  uint32_t xCoreID;
} TASKPARS;

typedef struct {
  int8_t mclk;
  int8_t bclk;
  int8_t ws;
  int8_t dout;
  int8_t din;
  uint8_t bmode;
  uint8_t channels;
  int16_t *dptr;
  uint16_t dlen; // + frequency
  uint32_t txhandle;
  uint32_t rxhandle;
  void *cbp;
  uint16_t timeout;
  uint16_t error;
  int8_t pdm_clk;
} I2S_PARS;

typedef struct {
  void *mod_addr;
  void (* const *jt)(void);
  void *mod_memory;
  uint16_t mem_size;
 // uint32_t execution_offset;
  MOD_FLAGS flags;
} MODULES_TABLE;

typedef struct {
    int8_t rxpin;
    int8_t txpin;
    int8_t hwfb;
    int32_t nwmode;
    uint16_t bsize;
    uint32_t speed;
    int8_t invert;
} TSPARS;

typedef struct {
  uint32_t cnt_last_ts;
  uint32_t counter_ltime;
  uint32_t counter_lfalltime;
  uint32_t counter_pulsewidth;
  uint16_t debounce;
  uint8_t cnt_updated;
  uint8_t cnt_debounce;
  uint8_t cnt_old_state;
  int8_t srcpin;
  uint8_t pinstate;
} PLUGIN_COUNTER;


typedef struct {
  uint16_t (*sd)(uint8_t, uint8_t);
  uint8_t meter;
  uint8_t *key;
  uint8_t *auth;
  uint8_t **out;
  uint16_t *size;
  uint8_t flags;
} HP_PARS;

#define MD_TYPE uint32_t

#define MOD_STORE_NAMESIZE 8

typedef struct {
  char name[MOD_STORE_NAMESIZE];
  volatile MD_TYPE value;
} MODULE_STORE;


enum {
  temperature_celsius = 0, tele_period, global_update, humidity, uptime, rel_inverted, devices_present 
};


#define FUNC_QUERY_LOW 0x80000000
#define FUNC_QUERY_HIGH 0xffffffff

//slow RTC MEM
#define GLOB_MOD_REG 0x50001ff0
//#define GLOB_MOD_REG RTC_SLOW_MEM

enum pXsnsFunctions { pFUNC_SETTINGS_OVERRIDE, pFUNC_SETUP_RING1, pFUNC_SETUP_RING2, pFUNC_PRE_INIT, pFUNC_INIT, pFUNC_ACTIVE, pFUNC_ABOUT_TO_RESTART,
                     pFUNC_LOOP, pFUNC_SLEEP_LOOP, pFUNC_EVERY_50_MSECOND, pFUNC_EVERY_100_MSECOND, pFUNC_EVERY_200_MSECOND, pFUNC_EVERY_250_MSECOND, pFUNC_EVERY_SECOND,
                     pFUNC_RESET_SETTINGS, pFUNC_RESTORE_SETTINGS, pFUNC_SAVE_SETTINGS, pFUNC_SAVE_AT_MIDNIGHT, pFUNC_SAVE_BEFORE_RESTART, pFUNC_INTERRUPT_STOP, pFUNC_INTERRUPT_START,
                     pFUNC_AFTER_TELEPERIOD, pFUNC_JSON_APPEND, pFUNC_WEB_SENSOR, pFUNC_WEB_COL_SENSOR,
                     pFUNC_MQTT_SUBSCRIBE, pFUNC_MQTT_INIT,
                     pFUNC_SET_POWER, pFUNC_SHOW_SENSOR, pFUNC_ANY_KEY, pFUNC_LED_LINK,
                     pFUNC_ENERGY_EVERY_SECOND, pFUNC_ENERGY_RESET,
                     pFUNC_TELEPERIOD_RULES_PROCESS, pFUNC_FREE_MEM,
                     pFUNC_WEB_ADD_BUTTON, pFUNC_WEB_ADD_CONSOLE_BUTTON, pFUNC_WEB_ADD_MANAGEMENT_BUTTON, pFUNC_WEB_ADD_MAIN_BUTTON,
                     pFUNC_WEB_GET_ARG, pFUNC_WEB_ADD_HANDLER, pFUNC_SET_SCHEME, pFUNC_HOTPLUG_SCAN, pFUNC_TIME_SYNCED,
                     pFUNC_DEVICE_GROUP_ITEM,
                     pFUNC_NETWORK_UP, pFUNC_NETWORK_DOWN,
                     pFUNC_return_result = 200,  // Insert function WITHOUT return results before here. Following functions return results
                     pFUNC_PIN_STATE, pFUNC_MODULE_INIT, pFUNC_ADD_BUTTON, pFUNC_ADD_SWITCH, pFUNC_BUTTON_PRESSED, pFUNC_BUTTON_MULTI_PRESSED,
                     pFUNC_SET_DEVICE_POWER,
                     pFUNC_MQTT_DATA, pFUNC_SERIAL,
                     pFUNC_COMMAND, pFUNC_COMMAND_SENSOR, pFUNC_COMMAND_DRIVER,
                     pFUNC_RULES_PROCESS,
                     pFUNC_SET_CHANNELS,
                     pFUNC_last_function         // Insert functions WITH return results before here
                     };

