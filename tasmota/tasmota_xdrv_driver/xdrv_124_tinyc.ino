/*
  xdrv_124_tinyc.ino - TinyC Bytecode VM for Tasmota

  Copyright (C) 2024  Gerhard Mutz

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

#ifdef USE_TINYC

/*********************************************************************************************\
 * TinyC - Lightweight C-subset bytecode VM for ESP32/ESP8266
 *
 * Multi-VM slot support: up to TC_MAX_VMS simultaneous programs (4 on ESP32, 1 on ESP8266)
 *
 * Commands:
 *   TinyC              - Show VM status (all slots)
 *   TinyCRun [s] [/f]  - Run slot s (default 0), optionally load /f first
 *   TinyCStop [s]      - Stop slot s (default 0)
 *   TinyCReset [s]     - Reset slot s (default 0)
 *   TinyCExec <n>      - Set instructions per tick (default 1000)
 *   TinyCInfo 0|1      - Show/hide VM status rows on main page
 *
 * Web:
 *   /tc            - TinyC console page with upload form (shows all slots)
 *   /tc_upload     - POST endpoint for .tcb binary upload
 *   /tc_api        - GET JSON API (cmd=run|stop|status|autoexec) with CORS, slot= parameter
\*********************************************************************************************/

#define XDRV_124  124

// Forward declarations for custom web handlers (called from SYS_WEB_ON in vm.h)
static void HandleTinyCWebOn1(void);
static void HandleTinyCWebOn2(void);
static void HandleTinyCWebOn3(void);
static void HandleTinyCWebOn4(void);
static void (*const TinyCWebOnHandlers[])(void) = {
  HandleTinyCWebOn1, HandleTinyCWebOn2, HandleTinyCWebOn3, HandleTinyCWebOn4
};

// mDNS support (for SYS_MDNS syscall)
#ifdef ESP32
  #include <ESPmDNS.h>
#else
  #include <ESP8266mDNS.h>
#endif

// UriGlob for port 82 download server wildcard routes
#include <uri/UriGlob.h>

// VM engine is in a separate .h to avoid Arduino IDE auto-prototype issues
#include "include/xdrv_124_tinyc_vm.h"

/*********************************************************************************************\
 * Email body callback — called from script_send_email_body() when TinyC initiated the send
 * Sends pre-registered body text and file attachments via send_message_txt() callback
\*********************************************************************************************/

#if defined(USE_SENDMAIL) || defined(USE_ESP32MAIL)
// Called from script_send_email_body() — returns true if TinyC handled it
bool tinyc_email_body(void(*func)(char *)) {
  if (!Tinyc || !Tinyc->email_active) return false;

  // Send body text
  if (Tinyc->email_body && Tinyc->email_body[0]) {
    func(Tinyc->email_body);
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: email body sent (%d chars)"), strlen(Tinyc->email_body));
  }

  // Send attachments: '@/path' for files, '$N' for webcam picture buffers
  for (uint8_t i = 0; i < Tinyc->email_attach_count; i++) {
    if (Tinyc->email_attach[i]) {
      if (Tinyc->email_attach[i][0] == '$') {
        // Picture buffer — send as-is (email library handles $N)
        func(Tinyc->email_attach[i]);
      } else {
        // File path — prefix with '@' for attach_File mechanism
        char tmp[40];
        snprintf(tmp, sizeof(tmp), "@%s", Tinyc->email_attach[i]);
        func(tmp);
      }
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: email attach: %s"), Tinyc->email_attach[i]);
    }
  }

  return true;  // handled by TinyC
}
#endif  // USE_SENDMAIL

/*********************************************************************************************\
 * HomeKit integration — provides C-linkage functions called from homekit.c
\*********************************************************************************************/
#ifdef USE_HOMEKIT
extern "C" {
  // Return pointer to VM globals of slot 0 (or NULL if not loaded)
  int32_t *tc_hk_get_globals(void) {
    if (!Tinyc || !Tinyc->slots[0] || !Tinyc->slots[0]->loaded) return nullptr;
    return Tinyc->slots[0]->vm.globals;
  }

  // Return Tasmota hostname for HomeKit bridge name
  char *tc_hk_get_hostname(void) {
    return NetworkHostname();
  }

  // Called from homekit.c when Apple Home writes a variable
  // Sets dirty flag for hkReady() polling (called from HAP thread)
  void tc_hk_set_dirty(int16_t global_idx) {
    for (uint8_t i = 0; i < hk_var_count; i++) {
      if (hk_var_gidx[i] == global_idx) {
        hk_var_dirty[i] = 1;
        return;
      }
    }
  }

  // Called from HomeKit HAP thread when Apple Home writes a value
  // Invokes "HomeKitWrite" callback on slot 0 with (dev_index, var_index, value) args
  void tc_hk_write_callback(uint8_t dev_index, uint8_t var_index, int32_t value) {
    if (!Tinyc) return;
    TcSlot *s = Tinyc->slots[0];
    if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) return;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
    tc_current_slot = s;
    TcVM *vm = &s->vm;
    if (vm->sp + 3 <= vm->stack_size) {
      vm->stack[vm->sp++] = (int32_t)dev_index;
      vm->stack[vm->sp++] = (int32_t)var_index;
      vm->stack[vm->sp++] = value;
      tc_vm_call_callback(vm, "HomeKitWrite");
    }
    tc_current_slot = nullptr;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
  }
}
#endif  // USE_HOMEKIT

/*********************************************************************************************\
 * Helpers: slot-aware callback dispatch
\*********************************************************************************************/

// tc_slot_callback() is in vm.h (Arduino auto-prototype workaround)

// Call a named callback on ALL active (loaded + halted + no-error) slots
static void tc_all_callbacks(const char *name) {
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (s) tc_slot_callback(s, name);
  }
}

// Call a named callback with a string argument on all active slots
static void tc_all_callbacks_str(const char *name, const char *str) {
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
    tc_current_slot = s;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
    tc_vm_call_callback_str(&s->vm, name, str);
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
    tc_current_slot = nullptr;
  }
}

// Touch button callback — called from xdrv_55_touch.ino on button/slider events
// Pushes (btn, val) args onto VM stack and calls TouchButton callback on all active slots
void tinyc_touch_button(uint8_t btn, int16_t val) {
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
    tc_current_slot = s;
    // Push args left-to-right: btn first, then val (callee pops in reverse)
    // Note: can't use TC_PUSH macro here (it has 'return int' in overflow check)
    TcVM *vm = &s->vm;
    if (vm->sp + 2 <= vm->stack_size) {
      vm->stack[vm->sp++] = (int32_t)btn;
      vm->stack[vm->sp++] = (int32_t)val;
      tc_vm_call_callback(vm, "TouchButton");
    }
    tc_current_slot = nullptr;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
  }
}

/*********************************************************************************************\
 * Settings file: /tinyc.cfg — persists slot→file mapping + autoexec flags
 *
 * Format: TC_MAX_VMS lines, each line:  filename,autoexec,show_info
 *   /slot0_test.tcb,1
 *   /bresser.tcb,0
 *   ,0
 *   ,0
 *   Last line (optional): _info,<0|1>   (show_info flag)
\*********************************************************************************************/

#define TC_CFG_FILE "/tinyc.cfg"

#ifdef USE_UFILESYS
// Save current slot configuration to /tinyc.cfg
static void TinyCSaveSettings(void) {
  if (!Tinyc) return;
  FS *fs = ufsp ? ufsp : ffsp;
  if (!fs) return;
  File f = fs->open(TC_CFG_FILE, "w");
  if (!f) { AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Cannot write " TC_CFG_FILE)); return; }
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    f.printf("%s,%d\n", Tinyc->slot_config[i].filename, Tinyc->slot_config[i].autoexec ? 1 : 0);
  }
  // Extra line for show_info
  f.printf("_info,%d\n", Tinyc->show_info ? 1 : 0);
  f.close();
  AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: Settings saved"));
}

// Load slot configuration from /tinyc.cfg, load files, auto-run marked slots
static void TinyCLoadSettings(void) {
  if (!Tinyc) return;
  FS *fs = ufsp ? ufsp : ffsp;
  if (!fs) return;
  File f = fs->open(TC_CFG_FILE, "r");
  if (!f) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: No " TC_CFG_FILE " found"));
    return;
  }
  uint8_t slot = 0;
  while (f.available() && slot <= TC_MAX_VMS) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) { slot++; continue; }

    // Parse: filename,autoexec
    int comma = line.indexOf(',');
    String fname = (comma >= 0) ? line.substring(0, comma) : line;
    int autoexec = (comma >= 0 && comma + 1 < (int)line.length()) ? line.substring(comma + 1).toInt() : 0;

    // Special _info line
    if (fname == "_info") {
      Tinyc->show_info = (autoexec != 0);
      continue;
    }

    if (slot >= TC_MAX_VMS) break;

    fname.trim();
    if (fname.length() > 0) {
      // Store config (lightweight — no RAM allocation for the VM)
      strlcpy(Tinyc->slot_config[slot].filename, fname.c_str(), sizeof(Tinyc->slot_config[slot].filename));
      Tinyc->slot_config[slot].autoexec = (autoexec != 0);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: Slot %d: %s (autoexec=%d)"), slot, fname.c_str(), autoexec);
    }
    slot++;
  }
  f.close();

  // Auto-run slots with autoexec flag — lazy-load + start
  if (TasmotaGlobal.no_autoexec) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Boot loop detected — autoexec disabled"));
  } else {
    for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
      if (Tinyc->slot_config[i].autoexec && Tinyc->slot_config[i].filename[0]) {
        if (TinyCLoadFile(Tinyc->slot_config[i].filename, i)) {
          TinyCStartVM(Tinyc->slots[i]);
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: Auto-started slot %d"), i);
          delay(100);  // stagger VM starts to avoid heap/stack exhaustion
        }
      }
    }
  }
}
#endif  // USE_UFILESYS

/*********************************************************************************************\
 * Tasmota: Init
\*********************************************************************************************/

static void TinyCInit(void) {
  uint32_t freeHeap = ESP_getFreeHeap();
  uint32_t needed = sizeof(struct TINYC);
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: Need %d bytes, free heap %d"), needed, freeHeap);

  if (freeHeap < needed + 4096) {  // keep 4KB reserve
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Not enough heap (%d free, need %d+4K)"), freeHeap, needed);
    return;
  }

  Tinyc = (struct TINYC *)calloc(1, sizeof(struct TINYC));
  if (!Tinyc) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Memory allocation failed (%d bytes)"), needed);
    return;
  }
  // calloc() zeroes memory but doesn't call C++ constructors for embedded objects.
  // WiFiUDP (NetworkUDP) needs proper construction or begin() crashes (NULL deref).
  new (&Tinyc->udp) WiFiUDP();
  new (&Tinyc->udp_port) WiFiUDP();
  Tinyc->instr_per_tick = TC_INSTR_PER_TICK;
  // Init SPI CS pins to -1 (unused)
  for (int i = 0; i < TC_SPI_MAX_CS; i++) { Tinyc->spi.cs[i] = -1; }
  Tinyc->spi.sclk = 0;  // 0 = not initialized (valid pins are >0 or <0)

  // Slots allocated on demand (upload, load, run API)

  AddLog(LOG_LEVEL_INFO, PSTR("TCC: TinyC VM initialized (%d bytes, %d free)"), needed, ESP_getFreeHeap());

  // Load slot config from /tinyc.cfg (loads files + auto-runs marked slots)
#ifdef USE_UFILESYS
  TinyCLoadSettings();
#endif
}

// TinyCSetPersistFile(), TinyCStopVM(), TinyCStartVM() are in vm.h
// (Arduino auto-prototype workaround for TcSlot* parameters)

/*********************************************************************************************\
 * Tasmota: Periodic execution (every 50ms)
\*********************************************************************************************/

static void TinyCEvery50ms(void) {
  if (!Tinyc) return;

#ifdef ESP32
  // ESP32: VM runs in its own FreeRTOS task -- monitor all slots for completion
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s) continue;
    if (s->running && !s->task_running) {
      // Task finished -- update state
      s->running = false;
      tc_current_slot = s;
      tc_output_flush();
      tc_current_slot = nullptr;
    }
  }
#else
  // ESP8266: slice-based execution in 50ms tick (no FreeRTOS task support)
  // Only slot 0 on ESP8266
  TcSlot *s = Tinyc->slots[0];
  if (s && s->loaded && s->running) {
    if (s->vm.halted || s->vm.error != TC_OK) {
      tc_free_all_frames(&s->vm);
      if (s->vm.halted && s->vm.error == TC_OK) {
        // Normal halt: globals + heap persist for callbacks
        tc_current_slot = s;
        tc_output_flush();
        tc_current_slot = nullptr;
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: Program halted after %u instructions, %d callbacks"),
          s->vm.instruction_count, s->vm.callback_count);
        s->running = false;
      }
      if (s->vm.error != TC_OK) {
        tc_heap_free_all(&s->vm);
        tc_current_slot = s;
        tc_output_flush();
        tc_current_slot = nullptr;
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Error: %s (PC=%d)"),
          tc_error_str(s->vm.error), s->vm.pc - s->vm.code_offset);
        s->running = false;
      }
    } else {
      yield();  // Feed WDT before VM execution
      tc_current_slot = s;
      int err = tc_vm_run_slice(&s->vm, Tinyc->instr_per_tick);
      tc_current_slot = nullptr;
      yield();  // Feed WDT after VM execution

      if (err != TC_OK && err != TC_ERR_PAUSED) {
        tc_free_all_frames(&s->vm);
        tc_heap_free_all(&s->vm);
        tc_current_slot = s;
        tc_output_flush();
        tc_current_slot = nullptr;
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Runtime error: %s (PC=%d, instr=%u)"),
          tc_error_str(err), s->vm.pc - s->vm.code_offset, s->vm.instruction_count);
        s->running = false;
      }
    }
  }
#endif  // ESP32 vs ESP8266

  // Execute deferred commands (audio etc.) only when VM is halted and idle
  // Must not run while VM task is active -- concurrent SD access causes crashes
  // Check slot 0 for deferred exec (shared infrastructure)
  {
    TcSlot *s0 = Tinyc->slots[0];
    if (s0 && s0->loaded && s0->vm.halted && s0->vm.error == TC_OK) {
      tc_deferred_exec();
    }
  }

  // Every50ms callback on all active slots
  tc_all_callbacks("Every50ms");
}

/*********************************************************************************************\
 * Tasmota: Commands
\*********************************************************************************************/

static const char TC_NOT_INIT[] PROGMEM = "Not initialized";

#define D_PRFX_TINYC "TinyC"

const char kTinyCCommands[] PROGMEM = D_PRFX_TINYC "|"
  "|Run|Stop|Reset|Exec|Info";

void (* const TinyCCommand[])(void) PROGMEM = {
  &CmndTinyC, &CmndTinyCRun, &CmndTinyCStop,
  &CmndTinyCReset, &CmndTinyCExec, &CmndTinyCInfo
};

// Query variables by name — scans global name table in binary (zero RAM cost)
// Usage: http://device/cm?cmnd=TinyC ?var1;var2     (slot 0)
//        http://device/cm?cmnd=TinyC ?1 var1;var2   (slot 1)
// Returns: {"TinyC":{"temp":23.5,"count":42,"name":"hello"}}
// Query global variables by index — used with _Q() compile-time macro
// Format: TinyC ?<idx><type>[<size>];<idx><type>[<size>];...
// Types: i=int, f=float, s<n>=char[n], I<n>=int[n], F<n>=float[n]
// Optional slot prefix: TinyC ?2 0f;1i  (query slot 2)
// Response: {"TinyC":[23.5,42,...]}
static void CmndTinyCQuery(char *data, uint16_t len) {
  uint8_t slot = 0;
  char *p = data;
  while (len > 0 && *p == ' ') { p++; len--; }
  if (len >= 2 && p[0] >= '0' && p[0] < ('0' + TC_MAX_VMS) && p[1] == ' ') {
    slot = p[0] - '0';
    p += 2; len -= 2;
  }
  TcSlot *s = (slot < TC_MAX_VMS) ? Tinyc->slots[slot] : nullptr;
  if (!s || !s->loaded) {
    Response_P(PSTR("{\"TinyC\":\"slot %d not loaded\"}"), slot);
    return;
  }
  Response_P(PSTR("{\"TinyC\":["));
  bool first = true;
  while (len > 0) {
    while (len > 0 && *p == ' ') { p++; len--; }
    if (len == 0) break;
    // Parse index
    uint16_t idx = 0;
    while (len > 0 && *p >= '0' && *p <= '9') {
      idx = idx * 10 + (*p - '0');
      p++; len--;
    }
    if (len == 0) break;
    char type = *p; p++; len--;  // i, f, s, I, F
    // Parse optional size for arrays/strings
    uint16_t size = 0;
    while (len > 0 && *p >= '0' && *p <= '9') {
      size = size * 10 + (*p - '0');
      p++; len--;
    }
    if (!first) ResponseAppend_P(PSTR(","));
    first = false;
    if (type == 's' && size > 0) {
      // char[] → JSON string
      char tmp[size + 1];
      uint16_t i = 0;
      while (i < size && idx + i < TC_MAX_GLOBALS && s->vm.globals[idx + i]) {
        tmp[i] = (char)s->vm.globals[idx + i]; i++;
      }
      tmp[i] = 0;
      ResponseAppend_P(PSTR("\"%s\""), tmp);
    } else if ((type == 'I' || type == 'F') && size > 0) {
      // int[]/float[] → JSON array
      ResponseAppend_P(PSTR("["));
      for (uint16_t i = 0; i < size && idx + i < TC_MAX_GLOBALS; i++) {
        if (i) ResponseAppend_P(PSTR(","));
        int32_t v = s->vm.globals[idx + i];
        if (type == 'F') { float f = *(float*)&v; ResponseAppend_P(PSTR("%*_f"), -4, &f); }
        else ResponseAppend_P(PSTR("%d"), v);
      }
      ResponseAppend_P(PSTR("]"));
    } else if (type == 'f') {
      int32_t v = (idx < TC_MAX_GLOBALS) ? s->vm.globals[idx] : 0;
      float f = *(float*)&v;
      ResponseAppend_P(PSTR("%*_f"), -4, &f);
    } else {
      int32_t v = (idx < TC_MAX_GLOBALS) ? s->vm.globals[idx] : 0;
      ResponseAppend_P(PSTR("%d"), v);
    }
    // Skip semicolon separator
    if (len > 0 && *p == ';') { p++; len--; }
  }
  ResponseAppend_P(PSTR("]}"));
}

void CmndTinyC(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }

  // Handle variable query: "TinyC ?var1;var2"
  if (XdrvMailbox.data_len > 0 && XdrvMailbox.data[0] == '?') {
    CmndTinyCQuery(XdrvMailbox.data + 1, XdrvMailbox.data_len - 1);
    return;
  }

  // Show status for all slots
  Response_P(PSTR("{\"TinyC\":{\"Heap\":%d,\"Slots\":["), ESP_getFreeHeap());
  bool first = true;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s) continue;
    if (!first) ResponseAppend_P(PSTR(","));
    first = false;
    ResponseAppend_P(PSTR("{\"Slot\":%d,\"Loaded\":%d,\"Running\":%d,\"Size\":%d,"
      "\"PC\":%d,\"SP\":%d,\"Instr\":%u,\"Error\":\"%s\",\"File\":\"%s\"}"),
      i,
      s->loaded ? 1 : 0,
      s->running ? 1 : 0,
      s->program_size,
      s->vm.pc - s->vm.code_offset,
      s->vm.sp,
      s->vm.instruction_count,
      tc_error_str(s->vm.error),
      s->filename[0] ? s->filename : "");
  }
  ResponseAppend_P(PSTR("]}}"));
}

// Parse optional slot number from command payload: "TinyCRun [slot] [/file]"
// Returns slot number (0-based), advances *pp past the slot digit + space
static uint8_t tc_parse_cmd_slot(char **pp, uint16_t *plen) {
  uint8_t slot = 0;
  char *p = *pp;
  uint16_t len = *plen;
  if (len > 0 && p[0] >= '0' && p[0] < ('0' + TC_MAX_VMS)) {
    slot = p[0] - '0';
    p++; len--;
    if (len > 0 && p[0] == ' ') { p++; len--; }
    *pp = p;
    *plen = len;
  }
  return slot;
}

void CmndTinyCRun(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  // Parse: TinyCRun [slot] [/file.tcb]
  char *p = XdrvMailbox.data;
  uint16_t len = XdrvMailbox.data_len;
  uint8_t slot_num = tc_parse_cmd_slot(&p, &len);
  TcSlot *s = Tinyc->slots[slot_num];
  if (!s) {
    // Auto-allocate slot if needed
    Tinyc->slots[slot_num] = tc_slot_alloc();
    s = Tinyc->slots[slot_num];
  }
  if (!s) { ResponseCmndChar_P(TC_NOT_INIT); return; }
#ifdef USE_UFILESYS
  // If a filename follows (e.g., "TinyCRun /bresser.tcb" or "TinyCRun 2 /bresser.tcb")
  if (len > 0 && p[0] == '/') {
    TinyCStopVM(s);
    if (!TinyCLoadFile(p, slot_num)) {
      ResponseCmndChar_P(PSTR("Load failed"));
      return;
    }
    TinyCSaveSettings();
  }
#endif
  // Lazy-load from config if not yet loaded
  if (!s->loaded && Tinyc->slot_config[slot_num].filename[0]) {
    TinyCLoadFile(Tinyc->slot_config[slot_num].filename, slot_num);
    s = Tinyc->slots[slot_num];
  }
  if (!s || !s->loaded) { ResponseCmndChar_P(PSTR("No program loaded")); return; }
  if (!TinyCStartVM(s)) {
    ResponseCmndChar_P(PSTR("Start failed"));
    return;
  }
  ResponseCmndDone();
}

void CmndTinyCStop(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  char *p = XdrvMailbox.data;
  uint16_t len = XdrvMailbox.data_len;
  uint8_t slot_num = tc_parse_cmd_slot(&p, &len);
  TcSlot *s = Tinyc->slots[slot_num];
  if (!s) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  TinyCStopVM(s);
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: Slot %d stopped"), slot_num);
  ResponseCmndDone();
}

void CmndTinyCReset(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  char *p = XdrvMailbox.data;
  uint16_t len = XdrvMailbox.data_len;
  uint8_t slot_num = tc_parse_cmd_slot(&p, &len);
  TcSlot *s = Tinyc->slots[slot_num];
  if (!s) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  TinyCStopVM(s);  // frees frame locals and heap
  // Free remaining dynamic VM allocations before zeroing struct
  if (s->vm.stack) { free(s->vm.stack); }
  if (s->vm.globals) { free(s->vm.globals); }
  if (s->vm.constants) { free(s->vm.constants); }
  if (s->vm.const_data) { free(s->vm.const_data); }
  memset(&s->vm, 0, sizeof(TcVM));  // zero all fields and pointers
  s->output_len = 0;
  s->output[0] = '\0';
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: Slot %d reset"), slot_num);
  ResponseCmndDone();
}

void CmndTinyCExec(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  if (XdrvMailbox.payload > 0) {
    Tinyc->instr_per_tick = XdrvMailbox.payload;
  }
  ResponseCmndNumber(Tinyc->instr_per_tick);
}

void CmndTinyCInfo(void) {
  if (!Tinyc) { ResponseCmndChar_P(TC_NOT_INIT); return; }
  if (XdrvMailbox.data_len > 0) {
    Tinyc->show_info = (XdrvMailbox.payload != 0);
#ifdef USE_UFILESYS
    TinyCSaveSettings();
#endif
  }
  ResponseCmndNumber(Tinyc->show_info ? 1 : 0);
}

/*********************************************************************************************\
 * Tasmota: Web interface
\*********************************************************************************************/

#ifdef USE_WEBSERVER

// Helper: send response with PROGMEM body string (saves RAM on ESP8266)
static void WSSend_P(int code, PGM_P content_type, PGM_P body) {
  char ct[32], buf[96];
  strncpy_P(ct, content_type, sizeof(ct) - 1); ct[sizeof(ct) - 1] = '\0';
  strncpy_P(buf, body, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
  Webserver->send(code, (const char*)ct, (const char*)buf);
}
// Shared PROGMEM strings for web responses
static const char TC_CT_JSON[] PROGMEM = "application/json";
static const char TC_CORS_ORIGIN[] PROGMEM = "Access-Control-Allow-Origin";
static const char TC_CORS_METHODS[] PROGMEM = "Access-Control-Allow-Methods";
static const char TC_CORS_HEADERS[] PROGMEM = "Access-Control-Allow-Headers";

// Send CORS headers from PROGMEM (saves ~180 bytes RAM on ESP8266)
static void TCSendCORS(const char *methods_ram) {
  char hdr[40], val[16];
  strncpy_P(hdr, TC_CORS_ORIGIN, sizeof(hdr)); Webserver->sendHeader(hdr, "*");
  strncpy_P(hdr, TC_CORS_METHODS, sizeof(hdr)); Webserver->sendHeader(hdr, methods_ram);
  strncpy_P(hdr, TC_CORS_HEADERS, sizeof(hdr));
  strcpy_P(val, PSTR("Content-Type"));
  Webserver->sendHeader(hdr, val);
}
// Shorthand for JSON responses
#define WSSendJSON_P(code, body) WSSend_P(code, TC_CT_JSON, body)
// Send JSON with pre-filled RAM buffer
static void WSSendJSON(int code, const char *json_buf) {
  char ct[20];
  strncpy_P(ct, TC_CT_JSON, sizeof(ct) - 1); ct[sizeof(ct) - 1] = '\0';
  Webserver->send(code, (const char*)ct, json_buf);
}

// Helper: load a .tcb file from filesystem into a specified slot
#ifdef USE_UFILESYS
static bool TinyCLoadFile(const char *path, uint8_t slot_num) {
  if (!Tinyc) return false;
  if (slot_num >= TC_MAX_VMS) return false;

  // Allocate slot if needed
  if (!Tinyc->slots[slot_num]) {
    Tinyc->slots[slot_num] = tc_slot_alloc();
    if (!Tinyc->slots[slot_num]) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Slot %d alloc failed"), slot_num);
      return false;
    }
  }
  TcSlot *s = Tinyc->slots[slot_num];

  File file;
  if (ufsp) file = ufsp->open(path, "r");
  if (!file && ffsp && ffsp != ufsp) file = ffsp->open(path, "r");
  if (!file) return false;
  uint32_t fsize = file.size();
  if (fsize == 0 || fsize > TC_MAX_PROGRAM) { file.close(); return false; }
  TinyCStopVM(s);
  if (s->program) { free(s->program); s->program = nullptr; }
  s->program = (uint8_t *)malloc(fsize);
  if (!s->program) { file.close(); return false; }
  file.read(s->program, fsize);
  file.close();
  s->program_size = fsize;
  int err = tc_vm_load(&s->vm, s->program, fsize);
  if (err == TC_OK) {
    s->loaded = true;
    strlcpy(s->filename, path, sizeof(s->filename));
    strlcpy(Tinyc->slot_config[slot_num].filename, path, sizeof(Tinyc->slot_config[slot_num].filename));
    TinyCSetPersistFile(s, path);
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Loaded %s (%d bytes) into slot %d"), path, fsize, slot_num);
    return true;
  }
  AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Load %s failed: %s"), path, tc_error_str(err));
  free(s->program);
  s->program = nullptr;
  s->program_size = 0;
  return false;
}
#endif

static void HandleTinyCPage(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  WSContentStart_P(PSTR("TinyC Console"));
  WSContentSendStyle();

  // Handle button commands first (before displaying status)
  // Commands default to slot 0 unless otherwise specified
  if (Tinyc && Webserver->hasArg(F("cmd"))) {
    String cmd = Webserver->arg(F("cmd"));
    uint8_t cmd_slot = 0;
    if (Webserver->hasArg(F("slot"))) {
      cmd_slot = Webserver->arg(F("slot")).toInt();
      if (cmd_slot >= TC_MAX_VMS) cmd_slot = 0;
    }
    TcSlot *cs = Tinyc->slots[cmd_slot];

    if (cmd == "run") {
      // Lazy-load if needed
      if (!cs && Tinyc->slot_config[cmd_slot].filename[0]) {
        TinyCLoadFile(Tinyc->slot_config[cmd_slot].filename, cmd_slot);
        cs = Tinyc->slots[cmd_slot];
      }
      if (cs) TinyCStartVM(cs);
    } else if (cmd == "stop" && cs) {
      TinyCStopVM(cs);
    } else if (cmd == "reset" && cs) {
      TinyCStopVM(cs);
      // Free remaining dynamic VM allocations before zeroing struct
      if (cs->vm.stack) { free(cs->vm.stack); }
      if (cs->vm.globals) { free(cs->vm.globals); }
      if (cs->vm.constants) { free(cs->vm.constants); }
      if (cs->vm.const_data) { free(cs->vm.const_data); }
      memset(&cs->vm, 0, sizeof(TcVM));
      cs->output_len = 0;
      cs->output[0] = '\0';
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: VM slot %d reset (web)"), cmd_slot);
    } else if (cmd == "load" && Webserver->hasArg(F("file"))) {
#ifdef USE_UFILESYS
      String file = Webserver->arg(F("file"));
      if (file.length() > 0) {
        TinyCLoadFile(file.c_str(), cmd_slot);
        TinyCSaveSettings();
      }
#endif
    } else if (cmd == "autoexec") {
      // Set autoexec flag: value=1 enable, value=0 disable, no value = toggle
      if (Webserver->hasArg(F("value"))) {
        Tinyc->slot_config[cmd_slot].autoexec = (Webserver->arg(F("value")).toInt() != 0);
      } else {
        Tinyc->slot_config[cmd_slot].autoexec = !Tinyc->slot_config[cmd_slot].autoexec;
      }
      if (cs) cs->autoexec = Tinyc->slot_config[cmd_slot].autoexec;
      TinyCSaveSettings();
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: Slot %d autoexec=%d"), cmd_slot, Tinyc->slot_config[cmd_slot].autoexec ? 1 : 0);
    } else if (cmd == "delall") {
#ifdef USE_UFILESYS
      // Delete all .tcb files from both filesystems
      int total = 0;
      FS *fss[] = { ufsp, (ffsp && ffsp != ufsp) ? ffsp : nullptr };
      for (int fi = 0; fi < 2; fi++) {
        if (!fss[fi]) continue;
        File dir = fss[fi]->open("/", "r");
        if (!dir) continue;
        char names[16][40];
        int count = 0;
        dir.rewindDirectory();
        while (count < 16) {
          File entry = dir.openNextFile();
          if (!entry) break;
          if (!entry.isDirectory()) {
            char *ep = (char *)entry.name();
            if (*ep == '/') ep++;
            char *lcp = strrchr(ep, '/');
            if (lcp) ep = lcp + 1;
            uint16_t nlen = strlen(ep);
            if (nlen > 4 && strcasecmp(ep + nlen - 4, ".tcb") == 0) {
              snprintf(names[count], sizeof(names[0]), "/%s", ep);
              count++;
            }
          }
          entry.close();
        }
        dir.close();
        for (int i = 0; i < count; i++) {
          fss[fi]->remove(names[i]);
        }
        total += count;
      }
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: Deleted %d .tcb files"), total);
#endif
    }
  }

  // Custom styles for this page
  WSContentSend_P(PSTR(
    "<style>"
    ".tc-run{color:#0a0}.tc-load{color:#fa0}.tc-empty{color:var(--c_txt);opacity:.5}"
    ".tc-err{color:#f44}"
    ".tc-row{display:flex;align-items:center;gap:6px;padding:4px 0;border-bottom:1px solid #333}"
    ".tc-row:last-child{border-bottom:none}"
    ".tc-info{flex:1;font-size:.9em}"
    ".tc-btns{display:flex;gap:4px}"
    ".tc-btns button{width:auto;padding:0 8px;font-size:.85em;line-height:1.8rem}"
    ".tc-out{background:#1a1a1a;color:#0f0;padding:6px 8px;border-radius:.3em;"
    "max-height:120px;overflow:auto;font-family:monospace;font-size:.85em;"
    "white-space:pre-wrap;word-break:break-all;margin:4px 0}"
    ".tc-upload input[type=file]{margin:8px 0}"
    ".tc-ide-url{display:flex;gap:8px;align-items:center}"
    ".tc-ide-url input{flex:1;padding:6px 8px}"
    ".tc-ide-url button{width:auto;padding:0 16px}"
    "</style>"));

  // --- VM Status: compact view with all slots in one fieldset ---
  if (Tinyc) {
    WSContentSend_P(PSTR("<fieldset><legend><b> TinyC VM Slots </b></legend>"));
    for (uint8_t si = 0; si < TC_MAX_VMS; si++) {
      TcSlot *s = Tinyc->slots[si];
      // Show slot if loaded OR has a config file (lazy-load pending)
      if (!s && !Tinyc->slot_config[si].filename[0]) continue;

      // Running = task active OR halted in callback mode (loaded + halted + no error)
      bool active = s && (s->running || (s->loaded && s->vm.halted && s->vm.error == TC_OK));
      const char *state, *sc;
      if (active) { state = "Run"; sc = "tc-run"; }
      else if (s && s->loaded) { state = "Rdy"; sc = "tc-load"; }
      else { state = "---"; sc = "tc-empty"; }

      // Determine filename: from loaded slot or from config
      const char *fname = (s && s->filename[0]) ? s->filename : Tinyc->slot_config[si].filename;

      // Compact row: [dot status] filename (size) | buttons
      WSContentSend_P(PSTR(
        "<div class='tc-row'>"
        "<span class='%s'>&#x25cf;</span>"
        "<span class='tc-info'><b>%d</b> %s %s (%dB)"),
        sc, si, state,
        fname,
        s ? s->program_size : 0);

      if (s && s->vm.error != 0) {
        WSContentSend_P(PSTR(" <span class='tc-err'>%s</span>"), tc_error_str(s->vm.error));
      }
      WSContentSend_P(PSTR("</span>"));

      // Inline buttons: grey out Run when active, Stop when not active
      WSContentSend_P(PSTR(
        "<form action='/tc' method='get' class='tc-btns'>"
        "<input type='hidden' name='slot' value='%d'>"
        "<button name='cmd' value='run' class='button' style='background:%s'>&#x25B6;</button>"
        "<button name='cmd' value='stop' class='button' style='background:%s'>&#x25A0;</button>"
        "<button name='cmd' value='reset' class='button'>&#x21BB;</button>"
        "<button name='cmd' value='autoexec' class='button' style='background:%s' title='Auto-execute on boot'>A</button>"
        "</form></div>"), si,
        active ? "#555" : "#47c266",    // Run: grey when active, green when idle
        active ? "#d43535" : "#555",     // Stop: red when active, grey when idle
        Tinyc->slot_config[si].autoexec ? "#47c266" : "var(--c_btn)");

      // Output log (compact)
      if (s && s->output_len > 0) {
        WSContentSend_P(PSTR("<div class='tc-out'>%s</div>"), s->output);
      }
    }
    WSContentSend_P(PSTR("</fieldset>"));

    // --- File selector with slot chooser ---
#ifdef USE_UFILESYS
    if (ufsp || ffsp) {
      WSContentSend_P(PSTR(
        "<fieldset><legend><b> Load Program </b></legend>"
        "<p><form action='/tc' method='get'>"
        "<div style='display:flex;gap:8px;align-items:center'>"
        "<select name='file' style='flex:1'>"));
      // Scan up to 2 filesystems: ufsp (SD/main) and ffsp (flash) if different
      FS *fss[] = { ufsp, (ffsp && ffsp != ufsp) ? ffsp : nullptr };
      const char *fslabel[] = { "", " [flash]" };
      for (int fi = 0; fi < 2; fi++) {
        if (!fss[fi]) continue;
        File dir = fss[fi]->open("/", "r");
        if (!dir) continue;
        dir.rewindDirectory();
        while (true) {
          File entry = dir.openNextFile();
          if (!entry) break;
          if (entry.isDirectory()) { entry.close(); continue; }
          char *ep = (char *)entry.name();
          if (*ep == '/') ep++;
          char *lcp = strrchr(ep, '/');
          if (lcp) ep = lcp + 1;
          uint16_t nlen = strlen(ep);
          if (nlen > 4 && strcasecmp(ep + nlen - 4, ".tcb") == 0) {
            char fpath[40];
            snprintf(fpath, sizeof(fpath), "/%s", ep);
            WSContentSend_P(PSTR("<option value='%s'>%s (%d B)%s</option>"),
              fpath, ep, entry.size(), fslabel[fi]);
          }
          entry.close();
        }
        dir.close();
      }
      WSContentSend_P(PSTR(
        "</select>"
        "<select name='slot' style='width:auto'>"));
      for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
        WSContentSend_P(PSTR("<option value='%d'>Slot %d</option>"), i, i);
      }
      WSContentSend_P(PSTR(
        "</select></div>"
        "<br><div style='display:flex;gap:8px'>"
        "<button name='cmd' value='load' class='button'>Load into Slot</button>"
        "<button name='cmd' value='delall' class='button bred'"
        " onclick=\"return confirm('Delete all .tcb files?')\">"
        "Delete All .tcb</button>"
        "</div></form></p></fieldset>"));
    }
#endif

  } else {
    WSContentSend_P(PSTR("<fieldset><legend><b> TinyC VM </b></legend>"
      "<p style='text-align:center;opacity:.6'>TinyC not initialized</p>"
      "</fieldset>"));
  }

  // --- Upload Section (with slot selector via JS to set form action) ---
  WSContentSend_P(PSTR(
    "<fieldset><legend><b> Upload Program </b></legend>"
    "<form class='tc-upload' method='POST' action='/tc_upload' enctype='multipart/form-data'"
    " onsubmit=\"this.action='/tc_upload?slot='+this.querySelector('[name=uslot]').value\">"
    "<div style='display:flex;gap:8px;align-items:center'>"
    "<input type='file' name='tcb' accept='.tcb' style='flex:1'>"
    "<select name='uslot' style='width:auto'>"));
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    WSContentSend_P(PSTR("<option value='%d'>Slot %d</option>"), i, i);
  }
  WSContentSend_P(PSTR(
    "</select></div>"
    "<button type='submit' class='button bgrn'>Upload .tcb</button>"
    "</form></fieldset>"));

  // --- IDE Section ---
  WSContentSend_P(PSTR("<fieldset><legend><b> TinyC IDE </b></legend>"));
#if defined(USE_TINYC_IDE) && defined(USE_UFILESYS)
  WSContentSend_P(PSTR(
    "<p style='text-align:center'>"
    "<button onclick=\"window.open('/ide')\" class='button bgrn'>Open IDE</button>"
    "</p>"
    "<p style='text-align:center;font-size:.85em;opacity:.6'>Served from device filesystem</p>"));
#else
  WSContentSend_P(PSTR(
    "<div class='tc-ide-url'>"
    "<input id='ide_url' value='http://localhost:8080' placeholder='IDE URL'>"
    "<button onclick=\"var u=document.getElementById('ide_url').value;"
    "window.open(u+'?device='+location.hostname)\" class='button bgrn'>Open</button>"
    "</div>"
    "<p style='text-align:center;font-size:.85em;opacity:.6'>IDE URL saved in browser</p>"
    "<script>var u=localStorage.getItem('tinyc_ide_url');"
    "if(u)document.getElementById('ide_url').value=u;"
    "document.getElementById('ide_url').onchange=function(){"
    "localStorage.setItem('tinyc_ide_url',this.value)};</script>"));
#endif
  WSContentSend_P(PSTR("</fieldset>"));

  // Listen for IDE "run on device" notifications to auto-refresh
  WSContentSend_P(PSTR(
    "<script>if(window.BroadcastChannel){"
    "var _tc=new BroadcastChannel('tinyc');"
    "_tc.onmessage=function(e){if(e.data=='refresh')location.reload();}}"
    "</script>"));

#ifdef USE_DISPLAY
  if (renderer && (renderer->framebuffer || renderer->rgb_fb)) {
    WSContentSend_P(PSTR(
      "<fieldset><legend><b> Display </b></legend>"
      "<p style='text-align:center'>"
      "<button onclick=\"window.open('/tc_display')\" class='button bgrn'>Display Mirror</button>"
      "</p></fieldset>"));
  }
#endif

  WSContentSpaceButton(BUTTON_MANAGEMENT);
  WSContentEnd();
}

static void HandleTinyCUploadDone(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  uint8_t slot_num = Tinyc ? Tinyc->upload_slot : 0;
  TcSlot *s = (Tinyc && slot_num < TC_MAX_VMS) ? Tinyc->slots[slot_num] : nullptr;

  // Check if this is an API call (from browser IDE) via ?api=1 query parameter
  bool is_api = Webserver->hasArg(F("api"));

  if (is_api) {
    // JSON response with CORS headers for browser IDE
    TCSendCORS("POST, OPTIONS");
    if (s && s->loaded) {
      char json[160];
      snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"size\":%d,\"file\":\"%s\",\"slot\":%d}"),
        s->program_size,
        s->filename[0] ? s->filename : "",
        slot_num);
      WSSendJSON(200, json);
    } else {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"upload failed\"}"));
    }
    return;
  }

  // Regular HTML response for form-based upload from /tc page
  WSContentStart_P(PSTR("TinyC Upload"));
  WSContentSendStyle();

  if (s && s->loaded) {
    WSContentSend_P(PSTR(
      "<fieldset><legend><b> Upload Result </b></legend>"
      "<p style='text-align:center;color:#0a0'><b>&#x2714; Upload successful!</b></p>"
      "<p style='text-align:center'>%s — %d bytes (slot %d)</p>"
      "</fieldset>"),
      s->filename[0] ? s->filename : "program",
      s->program_size, slot_num);
  } else {
    WSContentSend_P(PSTR(
      "<fieldset><legend><b> Upload Result </b></legend>"
      "<p style='text-align:center;color:#f44'><b>&#x2718; Upload failed</b></p>"
      "</fieldset>"));
  }
  WSContentSend_P(HTTP_FORM_BUTTON, PSTR("tc"), PSTR("Back to TinyC"));

  WSContentSpaceButton(BUTTON_MAIN);
  WSContentEnd();
}

// Handle CORS preflight for browser IDE uploads
static void HandleTinyCUploadCORS(void) {
  TCSendCORS("POST, OPTIONS");
  Webserver->send(204);
}

static void HandleTinyCUpload(void) {
  if (!Tinyc) return;

  HTTPUpload& upload = Webserver->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Determine target slot from ?slot=N parameter (default 0)
    Tinyc->upload_slot = 0;
    if (Webserver->hasArg(F("slot"))) {
      uint8_t rs = Webserver->arg(F("slot")).toInt();
      if (rs < TC_MAX_VMS) Tinyc->upload_slot = rs;
    }
    uint8_t slot_num = Tinyc->upload_slot;

    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Upload start: %s (slot %d)"), upload.filename.c_str(), slot_num);

    // Capture uploaded filename (prepend / for filesystem path)
    snprintf(Tinyc->upload_filename, sizeof(Tinyc->upload_filename), "/%s", upload.filename.c_str());

    // Allocate slot if needed
    if (!Tinyc->slots[slot_num]) {
      Tinyc->slots[slot_num] = tc_slot_alloc();
      if (!Tinyc->slots[slot_num]) {
        Web.upload_error = 1;
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Slot %d alloc failed"), slot_num);
        return;
      }
    }
    TcSlot *s = Tinyc->slots[slot_num];

    // Stop any running program in this slot
    TinyCStopVM(s);
    // Allocate upload buffer
    if (Tinyc->upload_buf) { free(Tinyc->upload_buf); Tinyc->upload_buf = nullptr; }
    Tinyc->upload_buf = (uint8_t *)malloc(TC_MAX_PROGRAM);
    if (!Tinyc->upload_buf) {
      Web.upload_error = 1;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Upload malloc failed"));
      return;
    }
    Tinyc->upload_received = 0;
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Tinyc->upload_buf && Tinyc->upload_received + upload.currentSize <= TC_MAX_PROGRAM) {
      memcpy(Tinyc->upload_buf + Tinyc->upload_received, upload.buf, upload.currentSize);
      Tinyc->upload_received += upload.currentSize;
    } else {
      Web.upload_error = 1;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Upload too large (max %d)"), TC_MAX_PROGRAM);
    }
  }
  else if (upload.status == UPLOAD_FILE_END) {
    uint8_t slot_num = Tinyc->upload_slot;
    TcSlot *s = (slot_num < TC_MAX_VMS) ? Tinyc->slots[slot_num] : nullptr;

    if (s && Tinyc->upload_buf && Tinyc->upload_received > 0 && !Web.upload_error) {
      // Free old program in this slot
      if (s->program) { free(s->program); }

      // Use upload buffer as program
      s->program = Tinyc->upload_buf;
      s->program_size = Tinyc->upload_received;
      Tinyc->upload_buf = nullptr;

      // Try to load
      int err = tc_vm_load(&s->vm, s->program, s->program_size);
      if (err == TC_OK) {
        s->loaded = true;
        strlcpy(s->filename, Tinyc->upload_filename, sizeof(s->filename));
        strlcpy(Tinyc->slot_config[slot_num].filename, s->filename, sizeof(Tinyc->slot_config[slot_num].filename));
        TinyCSetPersistFile(s, s->filename);
        AddLog(LOG_LEVEL_INFO, PSTR("TCC: Loaded %d bytes into slot %d"), s->program_size, slot_num);

        // Save to filesystem with uploaded filename
#ifdef USE_UFILESYS
        if (ufsp) {
          const char *saveName = s->filename[0] ? s->filename : TC_FILE_NAME;
#ifdef USE_WEBCAM
          WcInterrupt(0);
#endif
          File f = ufsp->open(saveName, "w");
          if (f) {
            f.write(s->program, s->program_size);
            f.close();
          }
#ifdef USE_WEBCAM
          WcInterrupt(1);
#endif
          AddLog(LOG_LEVEL_INFO, PSTR("TCC: Saved to %s"), saveName);
        }
        TinyCSaveSettings();
#endif
      } else {
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Load error: %s"), tc_error_str(err));
        free(s->program);
        s->program = nullptr;
        s->program_size = 0;
        s->loaded = false;
        Web.upload_error = 1;
      }
    } else {
      if (Tinyc->upload_buf) { free(Tinyc->upload_buf); Tinyc->upload_buf = nullptr; }
    }
  }
}

// ---- API endpoint for browser IDE (JSON + CORS) ----
// GET /tc_api?cmd=run|stop|status&slot=N
static void HandleTinyCApi(void) {
  TCSendCORS("GET, OPTIONS");

  if (!Tinyc) {
    WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"not initialized\"}"));
    return;
  }

  String cmd = Webserver->arg(F("cmd"));
  uint8_t slot_num = 0;
  if (Webserver->hasArg(F("slot"))) {
    slot_num = Webserver->arg(F("slot")).toInt();
    if (slot_num >= TC_MAX_VMS) slot_num = 0;
  }
  char json[384];

  if (cmd == "run") {
    // Allocate slot if needed
    // Lazy-load: if slot has a config file but isn't loaded yet, load now
    if (!Tinyc->slots[slot_num] || !Tinyc->slots[slot_num]->loaded) {
      if (Tinyc->slot_config[slot_num].filename[0]) {
        TinyCLoadFile(Tinyc->slot_config[slot_num].filename, slot_num);
      } else if (!Tinyc->slots[slot_num]) {
        Tinyc->slots[slot_num] = tc_slot_alloc();
      }
    }
    TcSlot *s = Tinyc->slots[slot_num];
    if (!s || !s->loaded) {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"no program loaded\"}"));
      return;
    }
    if (!TinyCStartVM(s)) {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"start failed\"}"));
      return;
    }
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Program started (API, slot %d)"), slot_num);
    snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"running\":true,\"size\":%d,\"slot\":%d}"),
      s->program_size, slot_num);
    WSSendJSON(200, json);
  }
  else if (cmd == "stop") {
    TcSlot *s = Tinyc->slots[slot_num];
    if (s) {
      TinyCStopVM(s);
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: Program stopped (API, slot %d)"), slot_num);
    }
    WSSendJSON_P(200, PSTR("{\"ok\":true,\"running\":false}"));
  }
  else if (cmd == "autoexec") {
    // Set autoexec: /tc_api?cmd=autoexec&slot=N&value=1|0
    if (Webserver->hasArg(F("value"))) {
      Tinyc->slot_config[slot_num].autoexec = (Webserver->arg(F("value")).toInt() != 0);
    } else {
      Tinyc->slot_config[slot_num].autoexec = !Tinyc->slot_config[slot_num].autoexec;
    }
    TcSlot *s = Tinyc->slots[slot_num];
    if (s) s->autoexec = Tinyc->slot_config[slot_num].autoexec;
    TinyCSaveSettings();
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Slot %d autoexec=%d (API)"), slot_num, Tinyc->slot_config[slot_num].autoexec ? 1 : 0);
    snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"slot\":%d,\"autoexec\":%d}"),
      slot_num, Tinyc->slot_config[slot_num].autoexec ? 1 : 0);
    WSSendJSON(200, json);
  }
  else if (cmd == "status") {
    // Return array of all slot statuses
    String result = F("{\"ok\":true,\"slots\":[");
    bool first = true;
    for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
      TcSlot *s = Tinyc->slots[i];
      // Show slot if loaded OR has a config file (lazy-load pending)
      if (!s && !Tinyc->slot_config[i].filename[0]) continue;
      if (!first) result += ',';
      first = false;
      if (s) {
        // Loaded slot — show full stats
        uint32_t vm_ram = sizeof(TcSlot) + s->program_size
          + s->vm.stack_size * sizeof(int32_t)
          + s->vm.globals_size * sizeof(int32_t)
          + s->vm.const_capacity * sizeof(TcConstant)
          + s->vm.const_data_size;
        if (s->vm.heap_data) vm_ram += s->vm.heap_used * sizeof(int32_t);
        if (s->vm.heap_handles) vm_ram += TC_MAX_HEAP_HANDLES * sizeof(TcHeapHandle);
        if (s->vm.udp_globals) vm_ram += s->vm.udp_global_count * sizeof(struct TcVM::TcUdpGlobalEntry);
        snprintf_P(json, sizeof(json),
          PSTR("{\"slot\":%d,\"loaded\":%d,\"running\":%d,\"autoexec\":%d,\"size\":%d,\"file\":\"%s\","
               "\"pc\":%d,\"sp\":%d,\"instr\":%u,\"ram\":%u,\"error\":\"%s\"}"),
          i,
          s->loaded ? 1 : 0,
          s->running ? 1 : 0,
          Tinyc->slot_config[i].autoexec ? 1 : 0,
          s->program_size,
          s->filename[0] ? s->filename : "",
          s->vm.pc - s->vm.code_offset,
          s->vm.sp,
        s->vm.instruction_count,
        vm_ram,
        tc_error_str(s->vm.error));
      } else {
        // Unloaded slot — show config info only (lazy-load pending, 0 RAM used)
        snprintf_P(json, sizeof(json),
          PSTR("{\"slot\":%d,\"loaded\":0,\"running\":0,\"autoexec\":%d,\"size\":0,\"file\":\"%s\","
               "\"pc\":0,\"sp\":0,\"instr\":0,\"ram\":0,\"error\":\"OK\"}"),
          i, Tinyc->slot_config[i].autoexec ? 1 : 0, Tinyc->slot_config[i].filename);
      }
      result += json;
    }
    result += F("],\"heap\":");
    result += String(ESP_getFreeHeap());
    result += '}';
    TCSendCORS("GET, OPTIONS");
    Webserver->send(200, F("application/json"), result);
    return;
  }
  else if (cmd == "freegpio") {
    // Return list of free (usable, not flash, not assigned) GPIO pins
    TCSendCORS("GET, OPTIONS");
    String result = F("{\"ok\":true,\"gpios\":[");
    bool first = true;
    for (uint32_t i = 0; i < MAX_GPIO_PIN; i++) {
      if (FlashPin(i)) continue;                    // skip flash/reserved pins
      if (TasmotaGlobal.gpio_pin[i] > 0) continue;  // skip assigned pins
      if (!first) result += ',';
      result += String(i);
      first = false;
    }
    result += F("]}");
    Webserver->send(200, F("application/json"), result);
    return;
  }
#ifdef USE_UFILESYS
  else if (cmd == "listfiles") {
    // List files on device filesystem (both ufsp and ffsp)
    if (!ufsp && !ffsp) {
      WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"no filesystem\"}"));
      return;
    }
    String result = F("{\"ok\":true,\"files\":[");
    bool first = true;
    FS *fss[] = { ufsp, (ffsp && ffsp != ufsp) ? ffsp : nullptr };
    for (int fi = 0; fi < 2; fi++) {
      if (!fss[fi]) continue;
      File root = fss[fi]->open("/", "r");
      if (!root) continue;
      File f = root.openNextFile();
      while (f) {
        if (!f.isDirectory()) {
          const char *fname = f.name();
          if (*fname == '/') fname++;
          const char *lcp = strrchr(fname, '/');
          if (lcp) fname = lcp + 1;
          if (!first) result += ',';
          result += F("{\"name\":\"");
          result += fname;
          result += F("\",\"size\":");
          result += String(f.size());
          if (fi == 1) result += F(",\"fs\":\"flash\"");
          result += '}';
          first = false;
        }
        f = root.openNextFile();
      }
      root.close();
    }
    result += F("]}");
    TCSendCORS("GET, OPTIONS");
    Webserver->send(200, F("application/json"), result);
    return;
  }
  else if (cmd == "deletefile") {
    // Delete a file: /tc_api?cmd=deletefile&path=/filename (tries ufsp then ffsp)
    String fpath = Webserver->arg(F("path"));
    if (fpath.length() == 0 || fpath[0] != '/') {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"missing path\"}"));
      return;
    }
    if (!ufsp && !ffsp) {
      WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"no filesystem\"}"));
      return;
    }
    bool deleted = false;
    if (ufsp) deleted = ufsp->remove(fpath.c_str());
    if (!deleted && ffsp && ffsp != ufsp) deleted = ffsp->remove(fpath.c_str());
    if (deleted) {
      WSSendJSON_P(200, PSTR("{\"ok\":true}"));
    } else {
      WSSendJSON_P(404, PSTR("{\"ok\":false,\"error\":\"delete failed\"}"));
    }
    return;
  }
  else if (cmd == "readfile") {
    // Read a text file from filesystem: /tc_api?cmd=readfile&path=/sml_meter.def
    // Supports time-range filter: /tc_api?cmd=readfile&path=/data.csv@1.2.22-00:00_12.2.22-00:00
    String fpath = Webserver->arg(F("path"));
    if (fpath.length() == 0 || fpath[0] != '/') {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"missing path\"}"));
      return;
    }
    if (!ufsp) {
      WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"no filesystem\"}"));
      return;
    }

    // Check for time-range filter: path@from_to
    char pathbuf[128];
    strlcpy(pathbuf, fpath.c_str(), sizeof(pathbuf));
    uint32_t cmp_from = 0, cmp_to = 0;
    char *atp = strchr(pathbuf, '@');
    if (atp) {
      *atp = 0;
      atp++;
      // from_to separated by underscore
      char *tp = strchr(atp, '_');
      if (tp) {
        *tp = 0;
        tp++;
        cmp_from = tc_ts_cmp(atp);
        cmp_to = tc_ts_cmp(tp);
      }
    }

    File f = ufsp->open(pathbuf, "r");
    if (!f) {
      WSSendJSON_P(404, PSTR("{\"ok\":false,\"error\":\"file not found\"}"));
      return;
    }

    TCSendCORS("GET, POST, OPTIONS");

    if (cmp_from && cmp_to && cmp_to > cmp_from) {
      // -- Time-filtered file serving --
      Webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
      Webserver->send(200, F("text/plain"), "");

      char *lbuf = (char*)malloc(512);
      if (!lbuf) { f.close(); return; }

      // 1. Send header line (first line of CSV)
      uint16_t li = 0;
      while (f.available() && li < 510) {
        uint8_t c;
        f.read(&c, 1);
        lbuf[li++] = c;
        if (c == '\n') break;
      }
      lbuf[li] = 0;
      if (li > 0) Webserver->sendContent(lbuf);
      uint32_t header_end = f.position();

      // 2. Try index file for fast seek
      uint32_t seek_pos = 0;
      {
        char indpath[128];
        strlcpy(indpath, pathbuf, sizeof(indpath));
        char *dot = strrchr(indpath, '.');
        if (dot) {
          strcpy(dot, ".ind");
        } else {
          strcat(indpath, ".ind");
        }
        File ind = ufsp->open(indpath, "r");
        if (ind) {
          // Skip index header line
          while (ind.available()) {
            uint8_t c; ind.read(&c, 1);
            if (c == '\n') break;
          }
          // Scan index lines: timestamp\tbyte_offset
          uint32_t last_good_pos = 0;
          uint16_t ycnt = 0;
          while (ind.available()) {
            li = 0;
            while (ind.available() && li < 510) {
              uint8_t c; ind.read(&c, 1);
              lbuf[li++] = c;
              if (c == '\n') break;
            }
            lbuf[li] = 0;
            uint32_t cmp = tc_ts_cmp(lbuf);
            if (cmp == 0) continue;
            if (cmp >= cmp_from) break;  // past our start
            char *tab = strchr(lbuf, '\t');
            if (tab) {
              last_good_pos = strtoul(tab + 1, NULL, 10);
            }
            if (++ycnt >= 100) { ycnt = 0; yield(); }
          }
          seek_pos = last_good_pos;
          ind.close();
        } else {
          // No index: estimated seek (like Scripter's opt_fext)
          li = 0;
          while (f.available() && li < 31) {
            uint8_t c; f.read(&c, 1);
            if (c == '\t' || c == '\n') break;
            lbuf[li++] = c;
          }
          lbuf[li] = 0;
          uint32_t ts_first = tc_ts_cmp(lbuf);

          // Find last line timestamp
          uint32_t fsize = f.size();
          uint32_t back = (fsize > 256) ? fsize - 256 : 0;
          f.seek(back, SeekSet);
          uint32_t last_nl = 0;
          while (f.available()) {
            uint8_t c; f.read(&c, 1);
            if (c == '\n') last_nl = f.position();
          }
          if (last_nl > back) {
            f.seek(last_nl, SeekSet);
            li = 0;
            while (f.available() && li < 31) {
              uint8_t c; f.read(&c, 1);
              if (c == '\t' || c == '\n') break;
              lbuf[li++] = c;
            }
            lbuf[li] = 0;
            uint32_t ts_last = tc_ts_cmp(lbuf);
            if (ts_last > ts_first && cmp_from > ts_first) {
              float perc = (float)(cmp_from - ts_first) / (float)(ts_last - ts_first) * 0.8f;
              if (perc < 0) perc = 0;
              if (perc > 1) perc = 1;
              seek_pos = (uint32_t)(perc * fsize);
            }
          }
          // Skip partial line at seek position
          if (seek_pos > 0) {
            f.seek(seek_pos, SeekSet);
            while (f.available()) {
              uint8_t c; f.read(&c, 1);
              if (c == '\n') break;
            }
            seek_pos = f.position();
          }
        }
      }

      // 3. Seek to start position and stream matching lines
      if (seek_pos > 0) {
        f.seek(seek_pos, SeekSet);
      } else {
        f.seek(header_end, SeekSet);
      }

      while (f.available()) {
        li = 0;
        while (f.available() && li < 510) {
          uint8_t c; f.read(&c, 1);
          lbuf[li++] = c;
          if (c == '\n') break;
        }
        lbuf[li] = 0;

        // Extract timestamp from first column (before first tab)
        char saved = 0;
        char *tab = strchr(lbuf, '\t');
        if (tab) { saved = *tab; *tab = 0; }
        uint32_t cmp = tc_ts_cmp(lbuf);
        if (tab) *tab = saved;

        if (cmp == 0) continue;         // skip invalid/header
        if (cmp > cmp_to) break;        // past end, done (data is chronological)
        if (cmp >= cmp_from) {
          Webserver->sendContent(lbuf);
        }
        yield();  // feed WDT during streaming
      }
      // Signal end of chunked response
      Webserver->sendContent("");

      free(lbuf);
      f.close();
      return;
    }

    // Normal full-file streaming (no time filter)
    Webserver->setContentLength(f.size());
    Webserver->send(200, F("text/plain"), "");
    uint8_t buf[256];
    while (f.available()) {
      int n = f.read(buf, sizeof(buf));
      if (n > 0) Webserver->client().write(buf, n);
    }
    f.close();
    return;
  }
  else if (cmd == "writefile") {
    // Write text to a file: /tc_api?cmd=writefile&path=/sml_meter.def  (POST body = content)
    String fpath = Webserver->arg(F("path"));
    if (fpath.length() == 0 || fpath[0] != '/') {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"missing path\"}"));
      return;
    }
    if (!ufsp) {
      WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"no filesystem\"}"));
      return;
    }
    String body = Webserver->arg(F("plain"));
    File f = ufsp->open(fpath.c_str(), "w");
    if (!f) {
      WSSendJSON_P(500, PSTR("{\"ok\":false,\"error\":\"write failed\"}"));
      return;
    }
    f.print(body);
    f.close();
    snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"size\":%d}"), body.length());
    WSSendJSON(200, json);
    return;
  }
#endif
  else {
    // Default: status for a specific slot (backward-compatible single-slot response)
    TcSlot *s = Tinyc->slots[slot_num];
    if (s) {
      snprintf_P(json, sizeof(json),
        PSTR("{\"ok\":true,\"slot\":%d,\"loaded\":%d,\"running\":%d,\"size\":%d,\"file\":\"%s\","
             "\"pc\":%d,\"sp\":%d,\"instr\":%u,\"error\":\"%s\",\"heap\":%d}"),
        slot_num,
        s->loaded ? 1 : 0,
        s->running ? 1 : 0,
        s->program_size,
        s->filename[0] ? s->filename : "",
        s->vm.pc - s->vm.code_offset,
        s->vm.sp,
        s->vm.instruction_count,
        tc_error_str(s->vm.error),
        ESP_getFreeHeap());
    } else {
      snprintf_P(json, sizeof(json),
        PSTR("{\"ok\":true,\"slot\":%d,\"loaded\":0,\"running\":0,\"size\":0,\"heap\":%d}"),
        slot_num, ESP_getFreeHeap());
    }
    WSSendJSON(200, json);
  }
}

// CORS preflight for /tc_api
static void HandleTinyCApiCORS(void) {
  TCSendCORS("GET, OPTIONS");
  Webserver->send(204);
}

// ---- HomeKit QR Code pairing page (/hk) ----
#ifdef USE_HOMEKIT
extern "C" {
  extern const char *homekit_get_uri(void);
  extern const char *homekit_get_code(void);
}

static void HandleHomeKitQR(void) {
  const char *uri = homekit_get_uri();
  const char *code = homekit_get_code();

  if (!uri || !uri[0]) {
    WSContentStart_P(PSTR("HomeKit"));
    WSContentSendStyle();
    WSContentSend_P(PSTR("<p>HomeKit not running.</p>"));
    WSContentSpaceButton(BUTTON_MAIN);
    WSContentEnd();
    return;
  }

  WSContentStart_P(PSTR("HomeKit Pairing"));
  WSContentSendStyle();
  // Use qrcode.js from CDN — the iPhone viewing this page has internet access
  WSContentSend_P(PSTR(
    "<div style='text-align:center'>"
    "<h2>HomeKit Pairing</h2>"
    "<div id='qr' style='display:inline-block;padding:8px;background:#fff'></div>"
    "<p style='font-size:24px;font-family:monospace;letter-spacing:4px'><b>%s</b></p>"
    "<p>Scan with iPhone Camera or Home app</p>"
    "<p id='uri' style='font-size:10px;color:#888'></p>"
    "</div>"
    "<script src='https://cdn.jsdelivr.net/npm/qrcodejs@1.0.0/qrcode.min.js'></script>"
    "<script>"
    "var u='%s';"
    "document.getElementById('uri').textContent=u;"
    "try{new QRCode(document.getElementById('qr'),{text:u,width:200,height:200,"
    "correctLevel:QRCode.CorrectLevel.M});}catch(e){"
    "document.getElementById('qr').innerHTML='<p>QR library failed: '+e.message+'</p>';}"
    "</script>"
  ), code, uri);
  WSContentSpaceButton(BUTTON_MAIN);
  WSContentEnd();
}
#endif  // USE_HOMEKIT

// ---- Self-hosted IDE (optional -- #define USE_TINYC_IDE) ----
// Serves /tinyc_ide.html (or .gz) from filesystem at /ide
#ifdef USE_TINYC_IDE
#ifdef USE_UFILESYS
static void HandleTinyCIde(void) {
  if (!ffsp && !ufsp) {
    WSSend_P(503, PSTR("text/plain"), PSTR("Filesystem not available"));
    return;
  }

  // Try gzipped version first on ffsp (flash), then ufsp (SD)
  bool gzipped = false;
  File f;
  if (ffsp) f = ffsp->open("/tinyc_ide.html.gz", "r");
  if (f) {
    gzipped = true;
  } else {
    if (ffsp) f = ffsp->open("/tinyc_ide.html", "r");
    if (!f && ufsp && ufsp != ffsp) {
      f = ufsp->open("/tinyc_ide.html.gz", "r");
      if (f) {
        gzipped = true;
      } else {
        f = ufsp->open("/tinyc_ide.html", "r");
      }
    }
  }

  if (!f) {
    WSSend_P(404, PSTR("text/plain"), PSTR("TinyC IDE not found. Upload tinyc_ide.html.gz to filesystem."));
    return;
  }

  uint32_t fsize = f.size();
  if (gzipped) {
    Webserver->sendHeader(F("Content-Encoding"), F("gzip"));
  }
  Webserver->setContentLength(fsize);
  WSSend_P(200, PSTR("text/html"), PSTR(""));

  // Stream file in chunks (smaller buffer on ESP8266 to save stack)
  uint8_t buf[256];
  while (f.available()) {
    int n = f.read(buf, sizeof(buf));
    if (n > 0) {
      Webserver->client().write(buf, n);
    }
    yield();
  }
  f.close();
}

#endif  // USE_UFILESYS
#endif  // USE_TINYC_IDE

// ---- WebUI: shared sv= parameter handler ----

// Process sv= widget value updates from AJAX requests
// Format: sv=gidx_value | sv=gidx_s_string | sv=gidx_t_HH:MM
// Uses slot 0 for globals access (WebUI is bound to slot 0)
static void TinyC_WebSetVar(void) {
  if (!Tinyc) return;
  TcSlot *s = Tinyc->slots[0];
  if (!s || !s->loaded) return;
  if (!Webserver->hasArg(F("sv"))) return;

  String sv = Webserver->arg(F("sv"));
  int sep = sv.indexOf('_');
  if (sep > 0) {
    int32_t gidx = sv.substring(0, sep).toInt();
    String val = sv.substring(sep + 1);
    if (gidx >= 0 && gidx < TC_MAX_GLOBALS) {
      if (val.startsWith("s_")) {
        // String value: write chars as int32 into globals[gidx..]
        const char *str = val.c_str() + 2;
        int32_t maxLen = TC_MAX_GLOBALS - gidx - 1;
        int i;
        for (i = 0; i < maxLen && str[i]; i++) {
          s->vm.globals[gidx + i] = (int32_t)(uint8_t)str[i];
        }
        s->vm.globals[gidx + i] = 0;  // null terminate
      } else if (val.startsWith("t_")) {
        // Time value: HH:MM -> HHMM integer
        const char *ts = val.c_str() + 2;
        int hh = 0, mm = 0;
        sscanf(ts, "%d:%d", &hh, &mm);
        s->vm.globals[gidx] = hh * 100 + mm;
      } else {
        s->vm.globals[gidx] = val.toInt();
      }
    }
  }
}

// ---- Display framebuffer mirror ----
#ifdef USE_DISPLAY

// Serve raw framebuffer binary: 8-byte header + raw pixel data
static void HandleTinyCDisplayRaw(void) {
  int8_t bpp = renderer->disp_bpp;
  uint8_t *fb = renderer->framebuffer;
  uint16_t *rgb = renderer->rgb_fb;

  // Derive raw (unrotated) dimensions from rotated width/height + rotation
  uint8_t rot = renderer->getRotation();
  uint16_t rw, rh;  // raw framebuffer dimensions
  if (rot == 0 || rot == 2) {
    rw = renderer->width();
    rh = renderer->height();
  } else {
    rw = renderer->height();
    rh = renderer->width();
  }

  // Compute framebuffer data size
  uint32_t fb_size = 0;
  uint8_t *data_ptr = nullptr;
  int8_t abs_bpp = abs(bpp);

  if (abs_bpp == 16 && rgb) {
    fb_size = (uint32_t)rw * rh * 2;
    data_ptr = (uint8_t *)rgb;
  } else if (abs_bpp == 4 && fb) {
    fb_size = (uint32_t)rw * rh / 2;
    data_ptr = fb;
  } else if (abs_bpp == 1 && fb) {
    if (bpp == -1) {
      fb_size = (uint32_t)rw * ((rh + 7) / 8);  // column-major
    } else {
      fb_size = ((uint32_t)rw * rh + 7) / 8;     // row-major
    }
    data_ptr = fb;
  }

  if (!data_ptr || fb_size == 0) {
    Webserver->send(503, "text/plain", "Unsupported bpp");
    return;
  }

  // Large RGB framebuffers (>32KB): stream line-by-line from SRAM to avoid PSRAM/LCD DMA contention
  // DISPLAY_MIRROR_HALF: 2:1 downsample (800x480 → 400x240 = 192KB) — better for slow WiFi
  // Full resolution: 800x480 = 768KB — best quality, needs stable connection
  #define DISPLAY_MIRROR_HALF
  if (abs_bpp == 16 && rgb && fb_size > 32768) {
#ifdef DISPLAY_MIRROR_HALF
    uint16_t dw = rw / 2;
    uint16_t dh = rh / 2;
    uint32_t line_bytes = dw * 2;
#else
    uint16_t dw = rw;
    uint16_t dh = rh;
    uint32_t line_bytes = rw * 2;
#endif
    uint32_t ds = (uint32_t)dw * dh * 2;

    uint8_t header[8];
    header[0] = dw & 0xFF;
    header[1] = (dw >> 8) & 0xFF;
    header[2] = dh & 0xFF;
    header[3] = (dh >> 8) & 0xFF;
    header[4] = (uint8_t)bpp;
    header[5] = (renderer->lvgl_param.swap_color ? 1 : 0) | ((rot & 3) << 2);
    header[6] = 0;
    header[7] = 0;

    Webserver->setContentLength(8 + ds);
    Webserver->send(200, F("application/octet-stream"), "");
    WiFiClient client = Webserver->client();
    client.setNoDelay(true);
    client.setTimeout(5);
    client.write(header, 8);

    // Line buffer in internal SRAM
    uint8_t *line = (uint8_t *)heap_caps_malloc(line_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!line) { line = (uint8_t *)malloc(line_bytes); }
    if (!line) { client.stop(); return; }

#ifdef DISPLAY_MIRROR_HALF
    for (uint16_t y = 0; y < rh; y += 2) {
      if (!client.connected()) break;
      uint16_t *src = rgb + (uint32_t)y * rw;
      uint16_t *dst = (uint16_t *)line;
      for (uint16_t x = 0; x < rw; x += 2) {
        *dst++ = src[x];
      }
      client.write(line, line_bytes);
      delay(1);
    }
#else
    for (uint16_t y = 0; y < rh; y++) {
      if (!client.connected()) break;
      // Copy one full row from PSRAM to SRAM, then send from SRAM
      memcpy(line, (uint8_t *)(rgb + (uint32_t)y * rw), line_bytes);
      client.write(line, line_bytes);
      delay(1);
    }
#endif
    free(line);
    client.stop();
    return;
  }

  // Small framebuffers (EPD, OLED) — send full resolution
  // 8-byte header
  uint8_t header[8];
  header[0] = rw & 0xFF;
  header[1] = (rw >> 8) & 0xFF;
  header[2] = rh & 0xFF;
  header[3] = (rh >> 8) & 0xFF;
  header[4] = (uint8_t)bpp;
  header[5] = (renderer->lvgl_param.swap_color ? 1 : 0) | ((rot & 3) << 2);
  header[6] = 0;
  header[7] = 0;

  uint32_t total = 8 + fb_size;
  Webserver->setContentLength(total);
  Webserver->send(200, F("application/octet-stream"), "");

  WiFiClient client = Webserver->client();
  client.setNoDelay(true);
  client.setTimeout(5);  // 5s write timeout
  client.write(header, 8);

  // Stream in chunks with yield, checking client is still connected
  uint32_t sent = 0;
  while (sent < fb_size) {
    if (!client.connected()) break;
    uint32_t chunk = fb_size - sent;
    if (chunk > 512) chunk = 512;
    client.write(data_ptr + sent, chunk);
    sent += chunk;
    yield();
  }
  client.stop();
}

// Serve self-contained HTML/JS display viewer page
static void HandleTinyCDisplayHTML(void) {
  Webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
  Webserver->send(200, F("text/html"), "");

  Webserver->sendContent_P(PSTR(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width'>"
    "<title>Display Mirror</title>"
    "<style>"
    "body{background:#111;color:#ddd;font-family:sans-serif;margin:20px;text-align:center}"
    "canvas{border:1px solid #444;image-rendering:pixelated;image-rendering:crisp-edges}"
    ".ctrl{margin:10px;display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap}"
    "button{padding:6px 16px;border:none;border-radius:4px;cursor:pointer;background:#47c266;color:#fff}"
    "button.stop{background:#d43535}"
    "select{padding:4px 8px;border-radius:4px;border:1px solid #444;background:#222;color:#ddd}"
    "#st{font-size:.85em;opacity:.7;margin:8px}"
    "</style></head><body>"
    "<h2>Display Mirror</h2>"
    "<canvas id='c'></canvas>"
    "<div id='st'></div>"
    "<div class='ctrl'>"
    "<button id='btn' onclick='toggle()'>Start</button>"
    "<label>Interval: <select id='iv'>"
    "<option value='2000'>2s</option>"
    "<option value='5000' selected>5s</option>"
    "<option value='10000'>10s</option>"
    "<option value='30000'>30s</option>"
    "</select></label>"
    "<label>Scale: <select id='sc' onchange='rsc()'>"
    "<option value='1'>1x</option>"
    "<option value='2' selected>2x</option>"
    "<option value='4'>4x</option>"
    "</select></label>"
    "</div>"
  ));

  // JavaScript decoder
  Webserver->sendContent_P(PSTR(
    "<script>"
    "var cv=document.getElementById('c'),cx=cv.getContext('2d'),"
    "tid=0,cnt=0,on=0,W=0,H=0;"
    "function toggle(){on=!on;"
    "document.getElementById('btn').textContent=on?'Stop':'Start';"
    "document.getElementById('btn').className=on?'stop':'';"
    "if(on)go();else if(tid){clearTimeout(tid);tid=0;}}"
    "function rsc(){"
    "if(W&&H){var s=+document.getElementById('sc').value;"
    "cv.style.width=(W*s)+'px';cv.style.height=(H*s)+'px';}}"
  ));

  // Pixel decoders per bpp
  Webserver->sendContent_P(PSTR(
    "function src(dx,dy,w,h,rot){"
    "if(rot==0)return[dx,dy];"
    "if(rot==1)return[w-1-dy,dx];"
    "if(rot==2)return[w-1-dx,h-1-dy];"
    "return[dy,h-1-dx];}"

    "function d1c(px,raw,w,h,dw,dh,rot,inv){"  // bpp=-1 column-major
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var v=(raw[sx+(sy>>3)*w]>>(sy&7))&1;"
    "if(inv)v=1-v;v=v?255:0;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"

    "function d1r(px,raw,w,h,dw,dh,rot,inv){"  // bpp=1 row-major
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var i=sx+sy*w,v=(raw[i>>3]>>(7-(i&7)))&1;"
    "if(inv)v=1-v;v=v?255:0;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"
  ));

  Webserver->sendContent_P(PSTR(
    "function d4(px,raw,w,h,dw,dh,rot){"  // bpp=4 grayscale
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var bi=Math.floor((sy*w+sx)/2),v;"
    "if((sx+sy*w)%2==0)v=raw[bi]&0xf;else v=(raw[bi]>>4)&0xf;"
    "v=v*17;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"

    "function d16(px,raw,w,h,dw,dh,rot,sw){"  // bpp=16 RGB565
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var oi=(sy*w+sx)*2,lo=raw[oi],hi=raw[oi+1],c;"
    "if(sw)c=(lo<<8)|hi;else c=(hi<<8)|lo;"
    "var p=(dy*dw+dx)*4;"
    "px[p]=((c>>11)&0x1f)<<3;px[p+1]=((c>>5)&0x3f)<<2;"
    "px[p+2]=(c&0x1f)<<3;px[p+3]=255;}}"
  ));

  // Main fetch + render loop with AbortController timeout
  Webserver->sendContent_P(PSTR(
    "var busy=0;"
    "async function go(){"
    "if(!on||busy)return;"
    "busy=1;"
    "var ac=new AbortController();"
    "var to=setTimeout(function(){ac.abort();},15000);"
    "try{"
    "var r=await fetch('/tc_display?raw=1',{signal:ac.signal});"
    "clearTimeout(to);"
    "if(!r.ok)throw r.status;"
    "var ab=await r.arrayBuffer(),dv=new DataView(ab);"
    "var w=dv.getUint16(0,1),h=dv.getUint16(2,1),"
    "bpp=dv.getInt8(4),fl=dv.getUint8(5);"
    "var sw=fl&1,rot=(fl>>2)&3;"
    "var dw,dh;"
    "if(rot==1||rot==3){dw=h;dh=w;}else{dw=w;dh=h;}"
    "if(dw!=W||dh!=H){W=dw;H=dh;cv.width=W;cv.height=H;rsc();}"
    "var id=cx.createImageData(W,H),px=id.data;"
    "var raw=new Uint8Array(ab,8);"
    "if(bpp==-1)d1c(px,raw,w,h,dw,dh,rot,0);"
    "else if(bpp==1)d1r(px,raw,w,h,dw,dh,rot,0);"
    "else if(Math.abs(bpp)==4)d4(px,raw,w,h,dw,dh,rot);"
    "else if(bpp==16)d16(px,raw,w,h,dw,dh,rot,sw);"
    "cx.putImageData(id,0,0);"
    "cnt++;document.getElementById('st').textContent="
    "dw+'x'+dh+' bpp='+bpp+' #'+cnt;"
    "}catch(e){"
    "clearTimeout(to);"
    "if(e.name!='AbortError')"
    "document.getElementById('st').textContent='Error: '+e;}"
    "busy=0;"
    "if(on)tid=setTimeout(go,+document.getElementById('iv').value);}"
    "</script></body></html>"
  ));

  Webserver->sendContent("");
}

// Dispatch: HTML viewer page or raw binary data
static void HandleTinyCDisplay(void) {
  if (!HttpCheckPriviledgedAccess()) return;

  if (!renderer) {
    Webserver->send(503, "text/plain", "No display");
    return;
  }
  if (!renderer->framebuffer && !renderer->rgb_fb) {
    Webserver->send(503, "text/plain", "No framebuffer");
    return;
  }

  if (Webserver->hasArg(F("raw"))) {
    HandleTinyCDisplayRaw();
  } else {
    HandleTinyCDisplayHTML();
  }
}

#endif  // USE_DISPLAY

// ---- Custom web handlers (webOn) -- uses slot 0 ----

static void HandleTinyCWebOn(uint8_t handler_num) {
  if (!Tinyc) { Webserver->send(503, "text/plain", "TinyC not ready"); return; }
  TcSlot *s = Tinyc->slots[0];
  if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) {
    Webserver->send(503, "text/plain", "TinyC not ready");
    return;
  }
  Tinyc->current_web_handler = handler_num;
  // CORS + chunked response -- callback uses webSend() to emit content
  TCSendCORS("GET, POST, OPTIONS");
  WSContentBegin(200, CT_HTML);
  tc_slot_callback(s, "WebOn");
  WSContentEnd();
  Tinyc->current_web_handler = 0;
}

static void HandleTinyCWebOn1(void) { HandleTinyCWebOn(1); }
static void HandleTinyCWebOn2(void) { HandleTinyCWebOn(2); }
static void HandleTinyCWebOn3(void) { HandleTinyCWebOn(3); }
static void HandleTinyCWebOn4(void) { HandleTinyCWebOn(4); }

// ---- WebUI: interactive widget page (/tc_ui) -- uses slot 0 ----

static void HandleTinyCUI(void) {
  if (!HttpCheckPriviledgedAccess()) return;
  if (!Tinyc) { Webserver->send(503, "text/plain", "TinyC not ready"); return; }
  TcSlot *s = Tinyc->slots[0];
  if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) {
    Webserver->send(503, "text/plain", "TinyC not ready");
    return;
  }

  // Read page number from ?p= parameter (0-5, default 0)
  uint8_t page = 0;
  if (Webserver->hasArg(F("p"))) {
    page = Webserver->arg(F("p")).toInt();
    if (page >= TC_MAX_WEB_PAGES) page = 0;
  }
  Tinyc->current_page = page;

  // Handle sv= parameter -- widget value update
  TinyC_WebSetVar();

  // Reset WebChart state before rendering
  tc_chart_seq = 0;
  tc_chart_lib_sent = false;

  // AJAX mode (m=1): just re-render widgets via WebUI() callback
  if (Webserver->hasArg(F("m"))) {
    WSContentBegin(200, CT_HTML);
    tc_slot_callback(s, "WebUI");
    WSContentEnd();
    return;
  }

  // Full page: HTML skeleton with JavaScript for AJAX refresh
  const char *title = (page < Tinyc->page_count && Tinyc->page_label[page][0])
                    ? Tinyc->page_label[page] : "TinyC UI";
  WSContentStart_P(title);
  WSContentSendStyle();
  WSContentSend_P(PSTR(
    "<script>"
    "var rfsh=1,x=null,lt;"
    "function la(p){"
      "var a=p||'';"
      "if(p)clearTimeout(lt);"
      "if(x)x.abort();"
      "x=new XMLHttpRequest();"
      "x.onreadystatechange=function(){"
        "if(x.readyState==4&&x.status==200){"
          "document.getElementById('ui').innerHTML=x.responseText;"
        "}"
      "};"
      "if(rfsh){"
        "x.open('GET','./tc_ui?p=%d&m=1'+a,true);"
        "x.send();"
        "lt=setTimeout(la,2000);"
      "}"
    "}"
    "function seva(v,i){rfsh=1;la('&sv='+i+'_'+v);rfsh=0;}"
    "function siva(v,i){rfsh=1;la('&sv='+i+'_s_'+v);rfsh=0;}"
    "function sivat(v,i){rfsh=1;la('&sv='+i+'_t_'+v);rfsh=0;}"
    "function pr(f){if(f){lt=setTimeout(la,2000);rfsh=1;}else{clearTimeout(lt);rfsh=0;}}"
    "window.onload=la;"
    "</script>"
  ), page);
  WSContentSend_P(PSTR("<div id='ui'>"));
  tc_slot_callback(s, "WebUI");
  WSContentSend_P(PSTR("</div>"));
  WSContentSpaceButton(BUTTON_MAIN);
  WSContentEnd();
}

#endif  // USE_WEBSERVER

/*********************************************************************************************\
 * Tasmota: JSON telemetry
\*********************************************************************************************/

static void TinyCShow(bool json) {
  if (!Tinyc) return;

  if (json) {
    // Iterate all slots for JSON output
    for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
      TcSlot *s = Tinyc->slots[i];
      if (!s) continue;
      if (i == 0) {
        // Slot 0 uses backward-compatible key "TinyC"
        ResponseAppend_P(PSTR(",\"TinyC\":{\"Running\":%d,\"Loaded\":%d,\"Size\":%d,\"Instr\":%u}"),
          s->running ? 1 : 0,
          s->loaded ? 1 : 0,
          s->program_size,
          s->vm.instruction_count);
      } else {
        ResponseAppend_P(PSTR(",\"TinyC%d\":{\"Running\":%d,\"Loaded\":%d,\"Size\":%d,\"Instr\":%u}"),
          i,
          s->running ? 1 : 0,
          s->loaded ? 1 : 0,
          s->program_size,
          s->vm.instruction_count);
      }
      // Call user's JsonCall() on this slot (skip the slot that triggered sensorGet)
      if (s->loaded && s->vm.halted && s->vm.error == TC_OK && s != tc_sensor_get_slot) {
        tc_slot_callback(s, "JsonCall");
      }
    }
  }
#ifdef USE_WEBSERVER
  else {
    // Web sensor rows for all slots
    for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
      TcSlot *s = Tinyc->slots[i];
      if (!s) continue;
      // Debug info rows (switchable via TinyCInfo command)
      if (Tinyc->show_info) {
        char status[10];
        if (s->running) { strcpy_P(status, PSTR("Running")); }
        else if (s->loaded) { strcpy_P(status, PSTR("Loaded")); }
        else { strcpy_P(status, PSTR("Empty")); }
        if (i == 0) {
          WSContentSend_PD(PSTR("{s}TinyC{m}%s (%d bytes){e}"),
            status, s->program_size);
        } else {
          WSContentSend_PD(PSTR("{s}TinyC[%d]{m}%s (%d bytes){e}"),
            i, status, s->program_size);
        }
      }
      // Call user's WebCall() on this slot (always active)
      if (s->loaded && s->vm.halted && s->vm.error == TC_OK) {
        tc_slot_callback(s, "WebCall");
      }
    }
  }
#endif
}

/*********************************************************************************************\
 * Port 82 Download Server -- background task for large file serving (ESP32 only)
 * Serves /ufs/<filename> with optional @from_to time-range filtering
 * Downloads run in a FreeRTOS task so main loop stays responsive
\*********************************************************************************************/

#ifdef ESP32

// Background task: streams file to client then exits
static void tc_download_task(void *param) {
  char *path = (char*)param;

  // Parse @from_to time range from path
  uint32_t cmp_from = 0, cmp_to = 0;
  char *atp = strchr(path, '@');
  if (atp) {
    *atp = 0;
    atp++;
    char *tp = strchr(atp, '_');
    if (tp) {
      *tp = 0;
      tp++;
      cmp_from = tc_ts_cmp(atp);
      cmp_to = tc_ts_cmp(tp);
    }
  }

  File file = ufsp->open(path, "r");
  if (!file) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: DL file not found: %s"), path);
    free(path);
    Tinyc->dl_busy = false;
    vTaskDelete(NULL);
    return;
  }

  // Determine content type
  char ctype[32];
  if (strstr_P(path, PSTR(".csv")) || strstr_P(path, PSTR(".txt"))) {
    strcpy_P(ctype, PSTR("text/plain"));
  } else if (strstr_P(path, PSTR(".html"))) {
    strcpy_P(ctype, PSTR("text/html"));
  } else if (strstr_P(path, PSTR(".json"))) {
    strcpy_P(ctype, PSTR("application/json"));
  } else {
    strcpy_P(ctype, PSTR("application/octet-stream"));
  }

  WiFiClient client = Tinyc->dl_server->client();
  uint32_t fsize = file.size();

  // Extract just the filename for Content-Disposition
  char *fname = strrchr(path, '/');
  if (fname) fname++; else fname = path;

  if (cmp_from && cmp_to && cmp_to > cmp_from) {
    // -- Time-filtered download --
    client.printf_P(PSTR("HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n"), ctype, fname);

    char *lbuf = (char*)malloc(512);
    if (lbuf) {
      // Send header line
      uint16_t li = 0;
      while (file.available() && li < 510) {
        uint8_t c; file.read(&c, 1);
        lbuf[li++] = c;
        if (c == '\n') break;
      }
      if (li > 0) {
        // Chunked encoding: send size + data + CRLF
        client.printf("%x\r\n", li);
        client.write((uint8_t*)lbuf, li);
        client.print("\r\n");
      }
      uint32_t header_end = file.position();

      // Try index file for fast seek
      uint32_t seek_pos = 0;
      {
        char indpath[128];
        strlcpy(indpath, path, sizeof(indpath));
        char *dot = strrchr(indpath, '.');
        if (dot) strcpy(dot, ".ind");
        else strcat(indpath, ".ind");

        File ind = ufsp->open(indpath, "r");
        if (ind) {
          // Skip header
          while (ind.available()) {
            uint8_t c; ind.read(&c, 1);
            if (c == '\n') break;
          }
          uint32_t last_good_pos = 0;
          while (ind.available()) {
            li = 0;
            while (ind.available() && li < 510) {
              uint8_t c; ind.read(&c, 1);
              lbuf[li++] = c;
              if (c == '\n') break;
            }
            lbuf[li] = 0;
            uint32_t cmp = tc_ts_cmp(lbuf);
            if (cmp == 0) continue;
            if (cmp >= cmp_from) break;
            char *tab = strchr(lbuf, '\t');
            if (tab) last_good_pos = strtoul(tab + 1, NULL, 10);
          }
          seek_pos = last_good_pos;
          ind.close();
        } else {
          // No index: estimated seek (opt_fext approach)
          li = 0;
          while (file.available() && li < 31) {
            uint8_t c; file.read(&c, 1);
            if (c == '\t' || c == '\n') break;
            lbuf[li++] = c;
          }
          lbuf[li] = 0;
          uint32_t ts_first = tc_ts_cmp(lbuf);

          uint32_t back = (fsize > 256) ? fsize - 256 : 0;
          file.seek(back, SeekSet);
          uint32_t last_nl = 0;
          while (file.available()) {
            uint8_t c; file.read(&c, 1);
            if (c == '\n') last_nl = file.position();
          }
          if (last_nl > back) {
            file.seek(last_nl, SeekSet);
            li = 0;
            while (file.available() && li < 31) {
              uint8_t c; file.read(&c, 1);
              if (c == '\t' || c == '\n') break;
              lbuf[li++] = c;
            }
            lbuf[li] = 0;
            uint32_t ts_last = tc_ts_cmp(lbuf);
            if (ts_last > ts_first && cmp_from > ts_first) {
              float perc = (float)(cmp_from - ts_first) / (float)(ts_last - ts_first) * 0.8f;
              if (perc < 0) perc = 0;
              if (perc > 1) perc = 1;
              seek_pos = (uint32_t)(perc * fsize);
            }
          }
          if (seek_pos > 0) {
            file.seek(seek_pos, SeekSet);
            while (file.available()) {
              uint8_t c; file.read(&c, 1);
              if (c == '\n') break;
            }
            seek_pos = file.position();
          }
        }
      }

      // Stream matching lines
      if (seek_pos > 0) file.seek(seek_pos, SeekSet);
      else file.seek(header_end, SeekSet);

      while (file.available()) {
        li = 0;
        while (file.available() && li < 510) {
          uint8_t c; file.read(&c, 1);
          lbuf[li++] = c;
          if (c == '\n') break;
        }
        lbuf[li] = 0;

        char saved = 0;
        char *tab = strchr(lbuf, '\t');
        if (tab) { saved = *tab; *tab = 0; }
        uint32_t cmp = tc_ts_cmp(lbuf);
        if (tab) *tab = saved;

        if (cmp == 0) continue;
        if (cmp > cmp_to) break;
        if (cmp >= cmp_from) {
          client.printf("%x\r\n", li);
          client.write((uint8_t*)lbuf, li);
          client.print("\r\n");
        }
        yield();  // feed WDT during long transfers
      }
      // Chunked encoding terminator
      client.print("0\r\n\r\n");
      free(lbuf);
    }
  } else {
    // -- Full file download --
    client.printf_P(PSTR("HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
      "Content-Disposition: attachment; filename=\"%s\"\r\n"
      "Content-Length: %d\r\n"
      "Connection: close\r\n\r\n"), ctype, fname, fsize);

    uint8_t buf[512];
    while (fsize > 0) {
      uint16_t len = (fsize < sizeof(buf)) ? fsize : sizeof(buf);
      file.read(buf, len);
      client.write(buf, len);
      fsize -= len;
      yield();  // feed WDT during long transfers
    }
  }

  file.close();
  client.stop();
  free(path);
  Tinyc->dl_busy = false;
  AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: DL task done"));
  vTaskDelete(NULL);
}

// HTTP handler for /ufs/* requests on port 82
static void TC_DLServeFile(void) {
  String uri = Tinyc->dl_server->uri();
  const char *cp = strstr_P(uri.c_str(), PSTR("/ufs/"));
  if (!cp) {
    Tinyc->dl_server->send(404, F("text/plain"), F("Not found"));
    return;
  }
  cp += 4;  // skip "/ufs" -> keep leading "/"

  if (!ufsp) {
    Tinyc->dl_server->send(500, F("text/plain"), F("No filesystem"));
    return;
  }

  if (Tinyc->dl_busy) {
    Tinyc->dl_server->send(503, F("text/plain"), F("Download busy"));
    return;
  }

  Tinyc->dl_busy = true;
  char *path = (char*)malloc(128);
  if (!path) {
    Tinyc->dl_busy = false;
    Tinyc->dl_server->send(500, F("text/plain"), F("Out of memory"));
    return;
  }
  strlcpy(path, cp, 128);
  xTaskCreatePinnedToCore(tc_download_task, "TCDL", 6000, (void*)path, 3, NULL, 1);
}

// Root handler
static void TC_DLRoot(void) {
  Tinyc->dl_server->send(200, F("text/plain"), F("TinyC File Server"));
}

// Initialize port 82 download server
static void TC_DLServerInit(void) {
  if (!Tinyc || Tinyc->dl_server) return;  // already initialized
  Tinyc->dl_server = new ESP8266WebServer(TC_DLPORT);
  if (Tinyc->dl_server) {
    Tinyc->dl_server->on(UriGlob("/ufs/*"), HTTP_GET, TC_DLServeFile);
    Tinyc->dl_server->on("/", HTTP_GET, TC_DLRoot);
    Tinyc->dl_server->begin();
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Download server started on port %d"), TC_DLPORT);
  }
}

// Poll for incoming connections (called from FUNC_LOOP)
static void TC_DLServerLoop(void) {
  if (Tinyc && Tinyc->dl_server) {
    Tinyc->dl_server->handleClient();
  }
}

#endif // ESP32

/*********************************************************************************************\
 * Tasmota: Driver entry point
\*********************************************************************************************/

bool Xdrv124(uint32_t function) {
  bool result = false;

  if (FUNC_INIT == function) {
    TinyCInit();
    return false;
  }

  if (!Tinyc) { return false; }

  switch (function) {
    case FUNC_LOOP:
      // Poll UDP multicast for incoming variables
      tc_udp_poll();
#ifdef ESP32
      // Lazy-init port 82 download server once WiFi is connected
      if (!Tinyc->dl_server && WifiHasIP()) {
        TC_DLServerInit();
      }
      // Poll port 82 download server for incoming file requests
      TC_DLServerLoop();
#endif
      // Call user's EveryLoop() callback on all active slots
      tc_all_callbacks("EveryLoop");
      break;
    case FUNC_EVERY_50_MSECOND:
      TinyCEvery50ms();
      break;
    case FUNC_EVERY_SECOND:
      // Call user's EverySecond() callback on all active slots
      tc_all_callbacks("EverySecond");
      break;
    case FUNC_COMMAND:
      result = DecodeCommand(kTinyCCommands, TinyCCommand);
      if (!result && Tinyc) {
        // Check each slot for registered command prefix match
        for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
          TcSlot *s = Tinyc->slots[i];
          if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
          if (!s->cmd_prefix[0]) continue;
          int plen = strlen(s->cmd_prefix);
          if (strncasecmp(XdrvMailbox.topic, s->cmd_prefix, plen) == 0) {
            // Build command string: "{subcommand} {data}"
            char cmd_str[256];
            const char *sub = XdrvMailbox.topic + plen;
            if (XdrvMailbox.data_len > 0) {
              snprintf(cmd_str, sizeof(cmd_str), "%s %s", sub, XdrvMailbox.data);
            } else {
              snprintf(cmd_str, sizeof(cmd_str), "%s", sub);
            }
            tc_current_slot = s;
#ifdef ESP32
            if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
            tc_vm_call_callback_str(&s->vm, "Command", cmd_str);
#ifdef ESP32
            if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
            tc_current_slot = nullptr;
            result = true;
            break;
          }
        }
      }
      break;
    case FUNC_RULES_PROCESS:
      // Call user's Event(char json[]) callback with the event JSON data
      if (ResponseLength()) {
        tc_all_callbacks_str("Event", ResponseData());
      }
      break;
    case FUNC_JSON_APPEND:
      TinyCShow(true);
      break;
#ifdef USE_WEBSERVER
    case FUNC_WEB_GET_ARG:
      // Process sv= widget value updates from main page AJAX (slot 0)
      TinyC_WebSetVar();
      break;
    case FUNC_WEB_SENSOR:
      TinyCShow(false);
      break;
    case FUNC_WEB_ADD_MAIN_BUTTON:
      // Reset WebChart state before calling WebPage()
      tc_chart_seq = 0;
      tc_chart_lib_sent = false;
      tc_chart_width = 0;
      tc_chart_height = 0;
      // Wrap charts in block container to prevent inline-block cascading width expansion
      WSContentSend_P(PSTR("<div style='display:block;width:100%%;overflow:hidden'>"));
      // Call user's WebPage() on all active slots
      for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
        TcSlot *s = Tinyc->slots[i];
        if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
        tc_slot_callback(s, "WebPage");
      }
      WSContentSend_P(PSTR("</div>"));
      // Inject JavaScript for widget interactions on main page (slot 0 only)
      {
        TcSlot *s0 = Tinyc->slots[0];
        if (s0 && s0->loaded && s0->vm.halted && s0->vm.error == TC_OK) {
          if (tc_has_callback(&s0->vm, "WebCall")) {
            WSContentSend_P(PSTR(
              "<script>"
              "function seva(v,i){rfsh=1;la('&sv='+i+'_'+v);rfsh=0;}"
              "function siva(v,i){la('&sv='+i+'_s_'+v);}"
              "function sivat(v,i){la('&sv='+i+'_t_'+v);}"
              "function pr(f){if(f){lt=setTimeout(la,%d);}else{clearTimeout(lt);clearTimeout(ft);}}"
              "</script>"
            ), Settings->web_refresh);
          }
          // Add buttons to /tc_ui pages if WebUI callback is defined
          if (tc_has_callback(&s0->vm, "WebUI")) {
            if (Tinyc->page_count > 0) {
              // Multiple pages registered via wLabel()
              for (uint8_t p = 0; p < Tinyc->page_count; p++) {
                if (Tinyc->page_label[p][0]) {
                  WSContentSend_P(PSTR("<p></p><form action='tc_ui' method='get'>"
                    "<input type='hidden' name='p' value='%d'>"
                    "<button>%s</button></form>"), p, Tinyc->page_label[p]);
                }
              }
            } else {
              // No wLabel() called -- single default button
              WSContentSend_P(PSTR("<p></p><form action='tc_ui' method='get'><button>TinyC UI</button></form>"));
            }
          }
        }
      }
      break;
    case FUNC_WEB_ADD_CONSOLE_BUTTON:
      if (XdrvMailbox.index) {
        XdrvMailbox.index++;
      } else {
        WSContentSend_P(HTTP_FORM_BUTTON, PSTR("tc"), PSTR("TinyC Console"));
        // Emit script-registered console buttons
        if (Tinyc) {
          for (uint8_t i = 0; i < Tinyc->console_btn_count; i++) {
            WSContentSend_P(HTTP_FORM_BUTTON,
              Tinyc->console_btn_url[i],
              Tinyc->console_btn_label[i]);
          }
        }
      }
      break;
    case FUNC_WEB_ADD_HANDLER:
      WebServer_on(PSTR("/tc"), HandleTinyCPage);
      Webserver->on("/tc_upload", HTTP_POST, HandleTinyCUploadDone, HandleTinyCUpload);
      Webserver->on("/tc_upload", HTTP_OPTIONS, HandleTinyCUploadCORS);
      WebServer_on(PSTR("/tc_api"), HandleTinyCApi);
      Webserver->on("/tc_api", HTTP_OPTIONS, HandleTinyCApiCORS);
      WebServer_on(PSTR("/tc_ui"), HandleTinyCUI);
#if defined(USE_TINYC_IDE) && defined(USE_UFILESYS)
      WebServer_on(PSTR("/ide"), HandleTinyCIde);
#endif
#ifdef USE_HOMEKIT
      WebServer_on(PSTR("/hk"), HandleHomeKitQR);
#endif
#ifdef USE_DISPLAY
      WebServer_on(PSTR("/tc_display"), HandleTinyCDisplay);
#endif
      break;
#endif
    case FUNC_SAVE_BEFORE_RESTART:
      // Call user's CleanUp() callback on all active slots (like scripter's >R section)
      tc_all_callbacks("CleanUp");
      // Save persist variables for all loaded slots
      for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
        TcSlot *s = Tinyc->slots[i];
        if (s && s->loaded) {
          tc_persist_save(&s->vm);
        }
      }
      break;
    case FUNC_ACTIVE:
      result = true;
      break;
  }
  return result;
}

#endif  // USE_TINYC
