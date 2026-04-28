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

// Default TinyC bytecode repository (used when /tinyc_repo.cfg is not present)
#define TINYC_DEFAULT_REPO "https://raw.githubusercontent.com/gemu2015/Sonoff-Tasmota/universal/tasmota/tinyc/bytecode"

// Global pause flag — set by filesystem upload handler (xdrv_50) to pause VM during uploads
bool tc_global_pause = false;

// Forward declarations for custom web handlers (called from SYS_WEB_ON in vm.h)
static void HandleTinyCWebOn1(void);
static void HandleTinyCWebOn2(void);
static void HandleTinyCWebOn3(void);
static void HandleTinyCWebOn4(void);
static void HandleTinyCWebOn5(void);
static void HandleTinyCWebOn6(void);
static void HandleTinyCWebOn7(void);
static void (*const TinyCWebOnHandlers[])(void) = {
  HandleTinyCWebOn1, HandleTinyCWebOn2, HandleTinyCWebOn3, HandleTinyCWebOn4,
  HandleTinyCWebOn5, HandleTinyCWebOn6, HandleTinyCWebOn7
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
    TcVM *vm = &s->vm;
    if (vm->sp + 3 <= vm->stack_size) {
      // Pin pre-push SP: tc_vm_call_callback_idx's saved_sp/restore would
      // otherwise re-materialise the 3 args the callback consumed.
      uint16_t pre_push_sp = vm->sp;
      vm->stack[vm->sp++] = (int32_t)dev_index;
      vm->stack[vm->sp++] = (int32_t)var_index;
      vm->stack[vm->sp++] = value;
      tc_vm_call_callback(vm, "HomeKitWrite");
      vm->sp = pre_push_sp;   // balance the 3 pushes
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

// ID-based variant for hot-path well-known callbacks (EverySecond, Every50ms, ...).
// Zero strcmp per call — dispatch is a direct array index. If no slot defines
// the callback, each slot fast-exits without even taking the mutex.
// tc_all_callbacks_id() body is in vm.h (Arduino auto-prototype workaround).

// Call a named callback with a string argument on all active slots
static void tc_all_callbacks_str(const char *name, const char *str) {
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s || !s->loaded) continue;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
    if (!s->vm.halted || s->vm.error != TC_OK) {
#ifdef ESP32
      if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
      continue;
    }
    tc_current_slot = s;
    tc_vm_call_callback_str(&s->vm, name, str);
    tc_current_slot = nullptr;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
  }
}

// Touch button callback — called from xdrv_55_touch.ino on button/slider events
// Pushes (btn, val) args onto VM stack and calls TouchButton callback on all active slots
void tinyc_touch_button(uint8_t btn, int16_t val) {
  if (!Tinyc) return;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    TcSlot *s = Tinyc->slots[i];
    if (!s || !s->loaded) continue;
#ifdef ESP32
    if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
    if (!s->vm.halted || s->vm.error != TC_OK) {
#ifdef ESP32
      if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
      continue;
    }
    tc_current_slot = s;
    // Push args left-to-right: btn first, then val (callee pops in reverse).
    // tc_vm_call_callback_idx captures saved_sp AFTER the push and restores
    // it on exit — which would re-materialise both args that the callback
    // already consumed via STORE_LOCAL. Pin pre-push SP and reassert it on
    // return so Every TouchButton invocation is net-zero on the operand stack.
    TcVM *vm = &s->vm;
    if (vm->sp + 2 <= vm->stack_size) {
      uint16_t pre_push_sp = vm->sp;
      vm->stack[vm->sp++] = (int32_t)btn;
      vm->stack[vm->sp++] = (int32_t)val;
      tc_vm_call_callback(vm, "TouchButton");
      vm->sp = pre_push_sp;   // balance the 2 pushes
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
  // TCP client slots — placement-new each (calloc zeroes but doesn't construct C++ objects)
  for (uint8_t s = 0; s < TC_TCP_CLI_SLOTS; s++) {
    new (&Tinyc->tcp_cli_clients[s]) WiFiClient();
  }
  Tinyc->tcp_cli_slot = 0;
  Tinyc->instr_per_tick = TC_INSTR_PER_TICK;
  // Init SPI CS pins to -1 (unused)
  for (int i = 0; i < TC_SPI_MAX_CS; i++) { Tinyc->spi.cs[i] = -1; }
  Tinyc->spi.sclk = 0;  // 0 = not initialized (valid pins are >0 or <0)

  // Slots allocated on demand (upload, load, run API)

  AddLog(LOG_LEVEL_INFO, PSTR("TCC: TinyC v" TC_RELEASE " initialized (%d bytes, %d free)"), needed, ESP_getFreeHeap());

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
  tc_all_callbacks_id(TC_CB_EVERY_50_MSECOND);
}

/*********************************************************************************************\
 * Tasmota: Commands
\*********************************************************************************************/

static const char TC_NOT_INIT[] PROGMEM = "Not initialized";

#define D_PRFX_TINYC "TinyC"

void CmndCheckPartition(void);

const char kTinyCCommands[] PROGMEM = D_PRFX_TINYC "|"
  "|Run|Stop|Reset|Exec|Info"
#ifdef ESP32
  "|Chkpt"
#endif
  ;

void (* const TinyCCommand[])(void) PROGMEM = {
  &CmndTinyC, &CmndTinyCRun, &CmndTinyCStop,
  &CmndTinyCReset, &CmndTinyCExec, &CmndTinyCInfo
#ifdef ESP32
  , &CmndCheckPartition
#endif
};

// --- TinyCChkpt: partition table manager (no USE_BINPLUGINS needed) ---
#ifdef ESP32
#include <MD5Builder.h>

static bool chkpt_scan_ptable(uint8_t *mp, uint32_t num) {
  int num_partitions = num;
  esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
  for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
    AddLog(LOG_LEVEL_INFO, PSTR("partition addr: 0x%06x; size: 0x%06x (%d KB); label: %s"),
           peptr->pos.offset, peptr->pos.size, peptr->pos.size / 1024, peptr->label);
    peptr++;
  }
  esp_err_t ret = esp_partition_table_verify((const esp_partition_info_t *)mp, false, &num_partitions);
  AddLog(LOG_LEVEL_INFO, "partition table status: err: %d - entries: %d", ret, num_partitions);
  return ret;
}

// TinyCChkpt       — show partition table
// TinyCChkpt p      — pack: shrink app0 to fit firmware, expand spiffs
// TinyCChkpt p 2880 — pack with explicit app0 size in KB
void CmndCheckPartition(void) {
  uint32_t new_app_size = 0;
  uint8_t pack = 0;

  if (XdrvMailbox.data_len) {
    char *cp = XdrvMailbox.data;
    while (*cp == ' ') cp++;
    if (*cp == 'p') {
      pack = 1;
      cp++;
      while (*cp == ' ') cp++;
      if (*cp) {
        uint32_t req_kb = strtol(cp, &cp, 10);
        if (req_kb >= 1024 && req_kb <= 3904) {
          new_app_size = req_kb * 1024;
          new_app_size = (new_app_size + 0xFFFF) & ~0xFFFF;
        }
      }
    }
  }

  if (pack) {
    uint32_t sketch_size = ESP.getSketchSize();
    if (!new_app_size) {
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

  #define PART_OFFSET 0x8000

  int num_partitions;
  uint8_t *mp = (uint8_t*)calloc(SPI_FLASH_SEC_SIZE >> 2, 4);
  esp_err_t ret = esp_flash_read(NULL, mp, PART_OFFSET, SPI_FLASH_SEC_SIZE);
  if (ret) {
    AddLog(LOG_LEVEL_INFO, "partition read error: %d", ret);
  } else {
    if (mp[0] != 0xAA || mp[1] != 0x50) {
      AddLog(LOG_LEVEL_INFO, "partition table not valid");
    } else {
      ret = esp_partition_table_verify((const esp_partition_info_t *)mp, false, &num_partitions);
      if (!ret) {
        AddLog(LOG_LEVEL_INFO, "partition table is valid: %d entries", num_partitions);
        int8_t hasspiffs = -1;
        esp_partition_info_t *peptr = (esp_partition_info_t*)mp;
        for (uint32_t cnt = 0; cnt < num_partitions; cnt++) {
          AddLog(LOG_LEVEL_INFO, PSTR("partition addr: 0x%06x; size: 0x%06x (%d KB); label: %s"),
                 peptr->pos.offset, peptr->pos.size, peptr->pos.size / 1024, peptr->label);
          if (!strcmp((char*)peptr->label, "spiffs")) hasspiffs = cnt;
          peptr++;
          if (peptr->magic != ESP_PARTITION_MAGIC) break;
        }

        if (pack) {
          int8_t hasapp0 = -1, hascustom = -1, hassafeboot = -1;
          peptr = (esp_partition_info_t*)mp;
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
            pack = 0;
          } else {
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
              AddLog(LOG_LEVEL_INFO, PSTR("pack: app0 %d KB -> %d KB"),
                peptr[hasapp0].pos.size / 1024, new_app_size / 1024);
              AddLog(LOG_LEVEL_INFO, PSTR("pack: spiffs %d KB @ 0x%06x -> %d KB @ 0x%06x"),
                peptr[hasspiffs].pos.size / 1024, peptr[hasspiffs].pos.offset,
                new_spiffs_size / 1024, new_spiffs_offset);
              peptr[hasapp0].pos.size = new_app_size;
              peptr[hasspiffs].pos.offset = new_spiffs_offset;
              peptr[hasspiffs].pos.size = new_spiffs_size;
            }
          }
        }
      }
    }
  }

  if (pack) {
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

    chkpt_scan_ptable(mp, num_partitions);
    ret = esp_flash_erase_region(NULL, PART_OFFSET, SPI_FLASH_SEC_SIZE);
    ret = esp_flash_write(NULL, mp, PART_OFFSET, SPI_FLASH_SEC_SIZE);
    free(mp);
    ESP_Restart();
    return;
  }

  free(mp);
  ResponseCmndDone();
}
#endif // ESP32

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
  // Internal DRAM first — small programs (the common case) stay in fast RAM.
  // Only when the internal heap can't satisfy (very large .tcb, or DRAM
  // already crowded by canvas/camera/HomeKit) do we spill into PSRAM. The
  // VM step loop reads code byte-by-byte via TC_READ_BYTE; PSRAM's ~10x
  // latency is acceptable at TinyC's tick rates (50 ms / 1 s callbacks)
  // but we prefer to avoid it when possible.
  s->program = (uint8_t *)malloc(fsize);
#ifdef ESP32
  if (!s->program) {
    s->program = (uint8_t *)heap_caps_malloc(fsize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s->program) {
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: %s (%u B) allocated in PSRAM (DRAM full)"),
        path, (unsigned)fsize);
    }
  }
#endif
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
    } else if (cmd == "download" && Webserver->hasArg(F("rfile"))) {
#ifdef USE_UFILESYS
      // Download .tcb from remote repository
      String rfile = Webserver->arg(F("rfile"));
      if (rfile.length() > 0) {
        // Read base URL from /tinyc_repo.cfg, fall back to default repo
        char repo_url[200] = {};
        File rcfg = ufsp->open("/tinyc_repo.cfg", "r");
        if (rcfg) {
          int rl = rcfg.readBytesUntil('\n', repo_url, sizeof(repo_url) - 1);
          rcfg.close();
          while (rl > 0 && (repo_url[rl-1] == '\r' || repo_url[rl-1] == ' ')) { repo_url[--rl] = 0; }
        }
        if (!repo_url[0]) {
          strlcpy(repo_url, TINYC_DEFAULT_REPO, sizeof(repo_url));
        }
        if (repo_url[0]) {
          // Build full URL: base_url/filename
          String url = String(repo_url);
          if (!url.endsWith("/")) url += "/";
          url += rfile;
          // Download to filesystem
          char fpath[48];
          snprintf(fpath, sizeof(fpath), "/%s", rfile.c_str());
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
          HTTPClientLight http;
#else
          WiFiClient http_client;
          HTTPClient http;
#endif
          http.setTimeout(10000);
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
          bool begun = http.begin(url);
#else
          bool begun = http.begin(http_client, url);
#endif
          if (begun) {
            int httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
              File f = ufsp->open(fpath, "w");
              if (f) {
                WiFiClient *stream = http.getStreamPtr();
                int32_t len = http.getSize();
                if (len < 0) len = 999999;
                uint8_t *dbuf = (uint8_t *)malloc(512);
                if (dbuf) {
                  while (http.connected() && (len > 0)) {
                    size_t avail = stream->available();
                    if (avail) {
                      if (avail > 512) avail = 512;
                      int rd = stream->readBytes(dbuf, avail);
                      f.write(dbuf, rd);
                      len -= rd;
                    }
                    delay(1);
                  }
                  free(dbuf);
                }
                int fsize = (int)f.size();
                f.close();
                AddLog(LOG_LEVEL_INFO, PSTR("TCC: Downloaded %s (%d bytes)"), fpath, fsize);
                // Load into selected slot
                TinyCLoadFile(fpath, cmd_slot);
                TinyCSaveSettings();
              }
            } else {
              AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Download failed HTTP %d"), httpCode);
            }
            http.end();
          }
        }
      }
#endif
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

      // --- Remote repository selector ---
      // Read repo URL from /tinyc_repo.cfg, fall back to default repo
      {
        char repo_url[200] = {};
        File rcfg = ufsp->open("/tinyc_repo.cfg", "r");
        if (rcfg) {
          int rl = rcfg.readBytesUntil('\n', repo_url, sizeof(repo_url) - 1);
          rcfg.close();
          while (rl > 0 && (repo_url[rl-1] == '\r' || repo_url[rl-1] == ' ')) { repo_url[--rl] = 0; }
        }
        if (!repo_url[0]) {
          strlcpy(repo_url, TINYC_DEFAULT_REPO, sizeof(repo_url));
        }
        if (repo_url[0]) {
          // Fetch index.txt from repo
          String idx_url = String(repo_url);
          if (!idx_url.endsWith("/")) idx_url += "/";
          idx_url += "index.txt";
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
          HTTPClientLight http;
#else
          WiFiClient http_client;
          HTTPClient http;
#endif
          http.setTimeout(5000);
#if defined(ESP32) && defined(USE_WEBCLIENT_HTTPS)
          bool begun = http.begin(idx_url);
#else
          bool begun = http.begin(http_client, idx_url);
#endif
          if (begun) {
            int httpCode = http.GET();
            if (httpCode == HTTP_CODE_OK) {
              String body = http.getString();
              if (body.length() > 0) {
                WSContentSend_P(PSTR(
                  "<fieldset><legend><b> Repository </b></legend>"
                  "<p><form action='/tc' method='get'>"
                  "<div style='display:flex;gap:8px;align-items:center'>"
                  "<select name='rfile' style='flex:1'>"));
                // Parse index.txt: one filename per line
                int pos = 0;
                while (pos < (int)body.length()) {
                  int nl = body.indexOf('\n', pos);
                  if (nl < 0) nl = body.length();
                  String line = body.substring(pos, nl);
                  line.trim();
                  if (line.length() > 0 && line.endsWith(".tcb")) {
                    WSContentSend_P(PSTR("<option value='%s'>%s</option>"),
                      line.c_str(), line.c_str());
                  }
                  pos = nl + 1;
                }
                WSContentSend_P(PSTR(
                  "</select>"
                  "<select name='slot' style='width:auto'>"));
                for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
                  WSContentSend_P(PSTR("<option value='%d'>Slot %d</option>"), i, i);
                }
                WSContentSend_P(PSTR(
                  "</select></div>"
                  "<br><button name='cmd' value='download' class='button bgrn'>"
                  "Download &amp; Load</button>"
                  "</form></p></fieldset>"));
              }
            }
            http.end();
          }
        }
      }
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
    "<button onclick=\"window.open('/ide','tinyc_ide')\" class='button bgrn'>Open IDE</button>"
    "</p>"
    "<p style='text-align:center;font-size:.85em;opacity:.6'>Served from device filesystem</p>"));
#else
  WSContentSend_P(PSTR(
    "<div class='tc-ide-url'>"
    "<input id='ide_url' value='http://localhost:8080' placeholder='IDE URL'>"
    "<button onclick=\"var u=document.getElementById('ide_url').value;"
    "window.open(u+'?device='+location.hostname,'tinyc_ide')\" class='button bgrn'>Open</button>"
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
  if (renderer && (((uintptr_t)renderer->framebuffer >= 0x3C000000) || ((uintptr_t)renderer->rgb_fb >= 0x3C000000))) {
    WSContentSend_P(PSTR(
      "<fieldset><legend><b> Display </b></legend>"
      "<p style='text-align:center'>"
      "<button onclick=\"window.open('/tc_display','tinyc_display')\" class='button bgrn'>Display Mirror</button>"
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
    // JSON response with CORS headers for browser IDE.
    // IMPORTANT: `s->loaded` can be STALE from a previous successful upload —
    // if THIS upload failed (malloc, bad slot, oversize, load error) the slot
    // still has the previous program loaded. Trust `Web.upload_error` as the
    // authoritative signal for this request.
    TCSendCORS("POST, OPTIONS");
    if (Web.upload_error || !s || !s->loaded) {
      WSSendJSON_P(400, PSTR("{\"ok\":false,\"error\":\"upload failed\"}"));
      return;
    }
    char json[160];
    snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"size\":%d,\"file\":\"%s\",\"slot\":%d}"),
      s->program_size,
      s->filename[0] ? s->filename : "",
      slot_num);
    WSSendJSON(200, json);
    return;
  }

  // Regular HTML response for form-based upload from /tc page.
  // Same rule as the JSON branch: Web.upload_error is authoritative — the
  // slot's own `loaded` flag may reflect the previously-loaded program.
  WSContentStart_P(PSTR("TinyC Upload"));
  WSContentSendStyle();

  if (!Web.upload_error && s && s->loaded) {
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
    // Allocate upload buffer.
    //
    // Sizing: the HTTP client may send `?fsz=N` (matches the `/ufsu` upload
    // convention used by push_tcb.sh) — if present we allocate exactly that
    // (clamped to TC_MAX_PROGRAM). Without the hint we fall back to the full
    // TC_MAX_PROGRAM. This avoids the failure mode where a 3 KB .tcb upload
    // fails because the device's heap is too fragmented to provide a single
    // contiguous 64 KB block.
    //
    // Allocation strategy: PSRAM first on ESP32 (avoids fragmented internal
    // heap on long-running devices with serial scripts etc.), then internal.
    if (Tinyc->upload_buf) { free(Tinyc->upload_buf); Tinyc->upload_buf = nullptr; }
    size_t alloc_size = TC_MAX_PROGRAM;
    if (Webserver->hasArg(F("fsz"))) {
      long fsz = Webserver->arg(F("fsz")).toInt();
      if (fsz > 0) {
        // Add a small headroom (16 B) so an off-by-one in the client's fsz
        // doesn't immediately trip the "too large" branch in UPLOAD_FILE_WRITE.
        size_t hint = (size_t)fsz + 16;
        if (hint < alloc_size) alloc_size = hint;
      }
    }
    Tinyc->upload_alloc_size = alloc_size;
#ifdef ESP32
    Tinyc->upload_buf = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!Tinyc->upload_buf) {
      Tinyc->upload_buf = (uint8_t *)malloc(alloc_size);
    }
#else
    Tinyc->upload_buf = (uint8_t *)malloc(alloc_size);
#endif
    if (!Tinyc->upload_buf) {
      Web.upload_error = 1;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Upload malloc(%u) failed — free heap=%u largest block=%u"),
        (unsigned)alloc_size,
        (unsigned)ESP_getFreeHeap(),
#ifdef ESP32
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)
#else
        (unsigned)ESP.getMaxFreeBlockSize()
#endif
      );
      return;
    }
    Tinyc->upload_received = 0;
    Tinyc->upload_active = true;
  }
  else if (upload.status == UPLOAD_FILE_WRITE) {
    // If a prior phase (malloc fail, slot fail) already marked this upload
    // invalid, silently discard further chunks — don't re-log per chunk
    // with a misleading "too large" message.
    if (Web.upload_error || !Tinyc->upload_buf) {
      return;
    }
    // Bound against the actual allocation size, which may be smaller than
    // TC_MAX_PROGRAM when the client sent ?fsz=N.
    size_t cap = Tinyc->upload_alloc_size ? Tinyc->upload_alloc_size : TC_MAX_PROGRAM;
    if (Tinyc->upload_received + upload.currentSize <= cap) {
      memcpy(Tinyc->upload_buf + Tinyc->upload_received, upload.buf, upload.currentSize);
      Tinyc->upload_received += upload.currentSize;
    } else {
      Web.upload_error = 1;
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: Upload too large (received %u + chunk %u > cap %u)"),
        (unsigned)Tinyc->upload_received, (unsigned)upload.currentSize, (unsigned)cap);
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
    Tinyc->upload_active = false;
  }
}

// ---- API endpoint for browser IDE (JSON + CORS) ----
// GET /tc_api?cmd=run|stop|status&slot=N
static void HandleTinyCApi(void) {
  TCSendCORS("GET, POST, OPTIONS");

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
    const char *run_name = s->filename[0] ? s->filename : "";
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: Program started %s (API, slot %d)"), run_name, slot_num);
    snprintf_P(json, sizeof(json), PSTR("{\"ok\":true,\"running\":true,\"size\":%d,\"slot\":%d,\"file\":\"%s\"}"),
      s->program_size, slot_num, run_name);
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
    Webserver->send(200, F("application/json"), result);
    return;
  }
  else if (cmd == "freegpio") {
    // Return list of free (usable, not flash, not assigned) GPIO pins
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

  // Try SD (ufsp) first, then Flash (ffsp) — lets users drop a newer
  // tinyc_ide.html.gz on SD and have it take precedence without a reflash.
  bool gzipped = false;
  File f;
  if (ufsp) f = ufsp->open("/tinyc_ide.html.gz", "r");
  if (f) {
    gzipped = true;
  } else {
    if (ufsp) f = ufsp->open("/tinyc_ide.html", "r");
    if (!f && ffsp && ffsp != ufsp) {
      f = ffsp->open("/tinyc_ide.html.gz", "r");
      if (f) {
        gzipped = true;
      } else {
        f = ffsp->open("/tinyc_ide.html", "r");
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
        int32_t newval = val.toInt();
        int32_t oldval = s->vm.globals[gidx];
        s->vm.globals[gidx] = newval;
        // Dispatch TouchButton callback when a webButton value changes
        if (newval != oldval) {
          tinyc_touch_button((uint8_t)gidx, (int16_t)newval);
        }
      }
    }
  }
}

// ---- Display framebuffer mirror ----
#ifdef USE_DISPLAY

// Serve raw framebuffer binary: 8-byte header + raw pixel data
static bool tc_mirror_busy = false;

static void HandleTinyCDisplayRaw(void) {
  // Reject if a previous transfer is still in progress
  if (tc_mirror_busy) {
    Webserver->send(503, "text/plain", "Busy");
    return;
  }
  if (!renderer) {
    Webserver->send(503, "text/plain", "No display");
    return;
  }
  tc_mirror_busy = true;
  tc_global_pause = true;   // pause VM during framebuffer read
  delay(0);  // yield to let VM finish current cycle

  int8_t bpp = renderer->disp_bpp;
  uint8_t *fb = renderer->framebuffer;
  uint16_t *rgb = renderer->rgb_fb;
  // Validate pointers — reject garbage values (valid ESP32 heap/PSRAM >= 0x3C000000)
  if (fb && (uintptr_t)fb < 0x3C000000) fb = nullptr;
  if (rgb && (uintptr_t)rgb < 0x3C000000) rgb = nullptr;

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
    tc_mirror_busy = false;
    tc_global_pause = false;
    return;
  }

#ifdef ESP32
  // Large RGB framebuffers (>32KB): downsample + stream from SRAM
  // Scale factor: 2=half (400x240=192KB), 1=full (800x480=768KB)
  #define DISPLAY_MIRROR_SCALE 2
  if (abs_bpp == 16 && rgb && fb_size > 32768) {
    uint16_t dw = rw / DISPLAY_MIRROR_SCALE;
    uint16_t dh = rh / DISPLAY_MIRROR_SCALE;
    uint32_t line_bytes = dw * 2;
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

    // Connection: close ensures client doesn't try keep-alive (Core 3 lwIP fix)
    Webserver->sendHeader(F("Connection"), F("close"));
    Webserver->setContentLength(8 + ds);
    Webserver->send(200, F("application/octet-stream"), "");
    WiFiClient client = Webserver->client();
    client.setNoDelay(true);
    client.setTimeout(5);
    client.write(header, 8);

    // Batch buffer in internal SRAM — 16 lines at a time reduces TCP write count
    // 2:1: 16 lines × 400 pixels × 2 bytes = 12800 bytes
    const uint16_t BATCH = 16;
    uint32_t batch_bytes = line_bytes * BATCH;
    uint8_t *buf = (uint8_t *)heap_caps_malloc(batch_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) { buf = (uint8_t *)malloc(batch_bytes); }
    if (!buf) { tc_mirror_busy = false; tc_global_pause = false; return; }

    uint16_t lines_done = 0;
    for (uint16_t y = 0; y < rh; y += DISPLAY_MIRROR_SCALE) {
      if (!client.connected()) break;
      uint16_t *src = rgb + (uint32_t)y * rw;
      uint16_t *dst = (uint16_t *)(buf + lines_done * line_bytes);
      for (uint16_t x = 0; x < rw; x += DISPLAY_MIRROR_SCALE) {
        *dst++ = src[x];
      }
      lines_done++;
      if (lines_done >= BATCH) {
        client.write(buf, lines_done * line_bytes);
        lines_done = 0;
        delay(5);
      }
    }
    if (lines_done > 0 && client.connected()) {
      client.write(buf, lines_done * line_bytes);
    }
    free(buf);
    delay(50);  // let last write drain
    // SO_LINGER{1,0} sends RST → no TIME_WAIT socket accumulation
    struct linger lin = {1, 0};
    setsockopt(client.fd(), SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
    client.stop();
    tc_mirror_busy = false;
    tc_global_pause = false;
    return;
  }
#endif  // ESP32 — large RGB framebuffers

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
  Webserver->sendHeader(F("Connection"), F("close"));
  Webserver->setContentLength(total);
  Webserver->send(200, F("application/octet-stream"), "");

  WiFiClient client = Webserver->client();
  client.setNoDelay(true);
  client.setTimeout(5);
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
  client.flush();
  delay(50);
  client.stop();
  tc_mirror_busy = false;
  tc_global_pause = false;
}

// Serve self-contained HTML/JS display snapshot page
// Single fetch on page load, manual Refresh button, CSS scale for viewing
static void HandleTinyCDisplayHTML(void) {
  Webserver->setContentLength(CONTENT_LENGTH_UNKNOWN);
  Webserver->send(200, F("text/html"), "");

  Webserver->sendContent_P(PSTR(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width'>"
    "<title>Display Mirror</title>"
    "<style>"
    "body{background:#111;color:#ddd;font-family:sans-serif;margin:20px;text-align:center}"
    "canvas{border:1px solid #444}"
    ".ctrl{margin:10px;display:flex;justify-content:center;gap:10px;align-items:center;flex-wrap:wrap}"
    "button{padding:6px 16px;border:none;border-radius:4px;cursor:pointer;background:#47c266;color:#fff}"
    "select{padding:4px 8px;border-radius:4px;border:1px solid #444;background:#222;color:#ddd}"
    "#st{font-size:.85em;opacity:.7;margin:8px}"
    "</style></head><body>"
    "<h2>Display Mirror</h2>"
    "<canvas id='c'></canvas>"
    "<div id='st'>Loading...</div>"
    "<div class='ctrl'>"
    "<button onclick='go()'>Refresh</button>"
    "<label>Scale: <select id='sc' onchange='rsc()'>"
    "<option value='1'>1x</option>"
    "<option value='2' selected>2x</option>"
    "<option value='4'>4x</option>"
    "</select></label>"
    "</div>"
  ));

  // JavaScript
  Webserver->sendContent_P(PSTR(
    "<script>"
    "var cv=document.getElementById('c'),cx=cv.getContext('2d');"
    "function rsc(){"
    "if(cv.width&&cv.height){var s=+document.getElementById('sc').value;"
    "cv.style.width=(cv.width*s)+'px';cv.style.height=(cv.height*s)+'px';}}"
  ));

  // Pixel decoders per bpp
  Webserver->sendContent_P(PSTR(
    "function src(dx,dy,w,h,rot){"
    "if(rot==0)return[dx,dy];"
    "if(rot==1)return[w-1-dy,dx];"
    "if(rot==2)return[w-1-dx,h-1-dy];"
    "return[dy,h-1-dx];}"

    "function d1c(px,raw,w,h,dw,dh,rot,inv){"
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var v=(raw[sx+(sy>>3)*w]>>(sy&7))&1;"
    "if(inv)v=1-v;v=v?255:0;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"

    "function d1r(px,raw,w,h,dw,dh,rot,inv){"
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var i=sx+sy*w,v=(raw[i>>3]>>(7-(i&7)))&1;"
    "if(inv)v=1-v;v=v?255:0;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"
  ));

  Webserver->sendContent_P(PSTR(
    "function d4(px,raw,w,h,dw,dh,rot){"
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var bi=Math.floor((sy*w+sx)/2),v;"
    "if((sx+sy*w)%2==0)v=raw[bi]&0xf;else v=(raw[bi]>>4)&0xf;"
    "v=v*17;var p=(dy*dw+dx)*4;"
    "px[p]=v;px[p+1]=v;px[p+2]=v;px[p+3]=255;}}"

    "function d16(px,raw,w,h,dw,dh,rot,sw){"
    "for(var dy=0;dy<dh;dy++)for(var dx=0;dx<dw;dx++){"
    "var s=src(dx,dy,w,h,rot),sx=s[0],sy=s[1];"
    "var oi=(sy*w+sx)*2,lo=raw[oi],hi=raw[oi+1],c;"
    "if(sw)c=(lo<<8)|hi;else c=(hi<<8)|lo;"
    "var p=(dy*dw+dx)*4;"
    "px[p]=((c>>11)&0x1f)<<3;px[p+1]=((c>>5)&0x3f)<<2;"
    "px[p+2]=(c&0x1f)<<3;px[p+3]=255;}}"
  ));

  // Single fetch — runs on page load, Refresh button calls go() again
  Webserver->sendContent_P(PSTR(
    "async function go(){"
    "document.getElementById('st').textContent='Loading...';"
    "try{"
    "var ac=new AbortController();"
    "var to=setTimeout(function(){ac.abort();},30000);"
    "var r=await fetch('/tc_display?raw=1',{signal:ac.signal});"
    "clearTimeout(to);"
    "if(!r.ok)throw 'HTTP '+r.status;"
    "var ab=await r.arrayBuffer(),dv=new DataView(ab);"
    "var w=dv.getUint16(0,1),h=dv.getUint16(2,1),"
    "bpp=dv.getInt8(4),fl=dv.getUint8(5);"
    "var sw=fl&1,rot=(fl>>2)&3;"
    "var dw,dh;"
    "if(rot==1||rot==3){dw=h;dh=w;}else{dw=w;dh=h;}"
    "cv.width=dw;cv.height=dh;"
    "var id=cx.createImageData(dw,dh),px=id.data;"
    "var raw=new Uint8Array(ab,8);"
    "if(bpp==-1)d1c(px,raw,w,h,dw,dh,rot,0);"
    "else if(bpp==1)d1r(px,raw,w,h,dw,dh,rot,0);"
    "else if(Math.abs(bpp)==4)d4(px,raw,w,h,dw,dh,rot);"
    "else if(bpp==16)d16(px,raw,w,h,dw,dh,rot,sw);"
    "cx.putImageData(id,0,0);"
    "rsc();"
    "document.getElementById('st').textContent="
    "dw+'x'+dh+' bpp='+bpp;"
    "}catch(e){"
    "document.getElementById('st').textContent='Error: '+e;}}"
    "go();"
    "</script></body></html>"
  ));

  Webserver->sendContent("");
}

// Dispatch: HTML viewer page or raw binary data
static void HandleTinyCDisplay(void) {
  if (!HttpCheckPriviledgedAccess()) return;

  Renderer *r = renderer;  // snapshot pointer
  if (!r) {
    Webserver->send(503, "text/plain", "No display");
    return;
  }
  // Validate framebuffer pointers — rgb_fb may contain uninitialized garbage
  // on displays where the LCD peripheral manages DMA buffers directly.
  // Valid ESP32 heap pointers are >= 0x3C000000.
  uint8_t *fb = r->framebuffer;
  uint16_t *rgb = r->rgb_fb;
  bool fb_ok = fb && ((uintptr_t)fb >= 0x3C000000);
  bool rgb_ok = rgb && ((uintptr_t)rgb >= 0x3C000000);
  if (!fb_ok && !rgb_ok) {
    Webserver->send(503, "text/plain", "No framebuffer (display uses HW DMA)");
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
static void HandleTinyCWebOn5(void) { HandleTinyCWebOn(5); }
static void HandleTinyCWebOn6(void) { HandleTinyCWebOn(6); }
static void HandleTinyCWebOn7(void) { HandleTinyCWebOn(7); }

// ---- Camera JPEG endpoint: /tc_cam?slot=N — serve PSRAM slot directly ----
#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
static void HandleTinyCCam(void) {
  int slot = 1;  // default slot 1
  if (Webserver->hasArg("slot")) {
    slot = Webserver->arg("slot").toInt();
  }
  if (slot < 1 || slot > TC_CAM_MAX_SLOTS) {
    Webserver->send(400, "text/plain", "invalid slot");
    return;
  }
  int idx = slot - 1;
  if (!tc_cam_slot[idx].buf || tc_cam_slot[idx].len == 0 || tc_cam_slot[idx].writing) {
    Webserver->send(503, "text/plain", "no image");
    return;
  }
  Webserver->sendHeader("Cache-Control", "no-cache, no-store");
  Webserver->send_P(200, "image/jpeg", (const char*)tc_cam_slot[idx].buf, tc_cam_slot[idx].len);
}

// ---- MJPEG streaming server on port 81 ----

static void TC_CamStreamHandler(void) {
  tc_cam_stream.stream_active = 1;
  tc_cam_stream.client = tc_cam_stream.server->client();
  AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: stream client connected"));
}

static void TC_CamStreamRoot(void) {
  tc_cam_stream.server->sendHeader("Location", "/cam.mjpeg");
  tc_cam_stream.server->send(302, "", "");
}

static void TC_CamStreamTask(void) {
  if (!tc_cam_stream.client.connected()) {
    tc_cam_stream.stream_active = 0;
    return;
  }
  if (1 == tc_cam_stream.stream_active) {
    tc_cam_stream.client.flush();
    tc_cam_stream.client.setNoDelay(true);
    tc_cam_stream.client.setTimeout(1);
    tc_cam_stream.client.print("HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace;boundary=" TC_CAM_BOUNDARY "\r\n"
      "\r\n");
    tc_cam_stream.stream_active = 2;
  }
  if (2 == tc_cam_stream.stream_active) {
    // Copy frame to send buffer first (slot may be overwritten during slow TCP send)
    if (!tc_cam_slot[0].buf || tc_cam_slot[0].len == 0 || tc_cam_slot[0].writing) return;
    uint32_t len = tc_cam_slot[0].len;
    // (Re)allocate send buffer if needed
    if (!tc_cam_stream.send_buf || tc_cam_stream.send_alloc < len) {
      if (tc_cam_stream.send_buf) free(tc_cam_stream.send_buf);
      tc_cam_stream.send_buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      tc_cam_stream.send_alloc = tc_cam_stream.send_buf ? len : 0;
    }
    if (!tc_cam_stream.send_buf) return;
    memcpy(tc_cam_stream.send_buf, tc_cam_slot[0].buf, len);
    tc_cam_stream.send_len = len;
    // Send from the copy — safe even if slot gets overwritten now
    tc_cam_stream.client.print("--" TC_CAM_BOUNDARY "\r\n");
    tc_cam_stream.client.printf("Content-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n",
      (int)tc_cam_stream.send_len);
    tc_cam_stream.client.write(tc_cam_stream.send_buf, tc_cam_stream.send_len);
    tc_cam_stream.client.print("\r\n");
  }
  if (0 == tc_cam_stream.stream_active) {
    tc_cam_stream.client.flush();
    tc_cam_stream.client.stop();
    AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: stream client disconnected"));
  }
}

void TC_CamStreamInit(void) {
  if (tc_cam_stream.server) return;  // already running
  // Defer if WiFi not ready (early boot autoexec)
  if (!WifiHasIP()) {
    tc_cam_stream.pending = 1;
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: stream server deferred (no WiFi)"));
    return;
  }
  tc_cam_stream.pending = 0;
  tc_cam_stream.stream_active = 0;
  tc_cam_stream.server = new ESP8266WebServer(TC_CAM_STREAM_PORT);
  if (tc_cam_stream.server) {
    tc_cam_stream.server->on("/", TC_CamStreamRoot);
    tc_cam_stream.server->on("/cam.mjpeg", TC_CamStreamHandler);
    tc_cam_stream.server->on("/cam.jpg", TC_CamStreamHandler);
    tc_cam_stream.server->on("/stream", TC_CamStreamHandler);
    tc_cam_stream.server->begin();
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: MJPEG stream server on port %d"), TC_CAM_STREAM_PORT);
  }
}

void TC_CamStreamStop(void) {
  if (tc_cam_stream.server) {
    tc_cam_stream.stream_active = 0;
    tc_cam_stream.server->stop();
    delete tc_cam_stream.server;
    tc_cam_stream.server = nullptr;
    if (tc_cam_stream.send_buf) { free(tc_cam_stream.send_buf); tc_cam_stream.send_buf = nullptr; }
    tc_cam_stream.send_len = 0;
    tc_cam_stream.send_alloc = 0;
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: stream server stopped"));
  }
}

static void TC_CamStreamLoop(void) {
  // Deferred init: create server once WiFi is ready
  if (tc_cam_stream.pending && !tc_cam_stream.server && WifiHasIP()) {
    TC_CamStreamInit();
  }
  if (tc_cam_stream.server) {
    tc_cam_stream.server->handleClient();
    if (tc_cam_stream.stream_active) { TC_CamStreamTask(); }
  }
}

// ---- Motion detection (frame-diff from PSRAM slot 0) ----

static void TC_CamMotionDetect(void) {
  if (!tc_cam_motion.interval_ms) return;
  if ((millis() - tc_cam_motion.last_time) < tc_cam_motion.interval_ms) return;
  tc_cam_motion.last_time = millis();

  // Need a valid JPEG in slot 0 with known dimensions, not currently being written
  if (!tc_cam_slot[0].buf || tc_cam_slot[0].len == 0 ||
      tc_cam_slot[0].width == 0 || tc_cam_slot[0].height == 0 ||
      tc_cam_slot[0].writing) return;

  uint32_t w = tc_cam_slot[0].width;
  uint32_t h = tc_cam_slot[0].height;
  uint32_t pixels = w * h;

  // Decode JPEG to RGB888 in temp buffer
  uint8_t *rgb = (uint8_t*)heap_caps_malloc(pixels * 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rgb) return;

  if (!fmt2rgb888(tc_cam_slot[0].buf, tc_cam_slot[0].len, PIXFORMAT_JPEG, rgb)) {
    free(rgb);
    return;
  }

  // Allocate reference buffer if needed
  if (!tc_cam_motion.ref_buf || tc_cam_motion.ref_size != pixels) {
    if (tc_cam_motion.ref_buf) free(tc_cam_motion.ref_buf);
    tc_cam_motion.ref_buf = (uint8_t*)heap_caps_malloc(pixels, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    tc_cam_motion.ref_size = pixels;
    if (tc_cam_motion.ref_buf) {
      // First frame — fill reference, no comparison
      uint8_t *pxi = rgb;
      for (uint32_t i = 0; i < pixels; i++) {
        tc_cam_motion.ref_buf[i] = (pxi[0] + pxi[1] + pxi[2]) / 3;
        pxi += 3;
      }
    }
    free(rgb);
    return;
  }

  // Compare with reference
  uint64_t accu = 0;
  uint64_t bright = 0;
  uint8_t *pxi = rgb;
  uint8_t *pxr = tc_cam_motion.ref_buf;
  for (uint32_t i = 0; i < pixels; i++) {
    int32_t gray = (pxi[0] + pxi[1] + pxi[2]) / 3;
    int32_t lgray = pxr[0];
    pxr[0] = gray;
    pxi += 3;
    pxr++;
    accu += abs(gray - lgray);
    bright += gray;
  }
  free(rgb);

  uint32_t divider = pixels / 100;
  if (divider == 0) divider = 1;
  tc_cam_motion.motion_trigger = (uint32_t)(accu / divider);
  tc_cam_motion.motion_brightness = (uint32_t)(bright / divider);
  tc_cam_motion.triggered = (tc_cam_motion.threshold > 0 && tc_cam_motion.motion_trigger > tc_cam_motion.threshold) ? 1 : 0;
}

#endif // USE_WEBCAM || USE_TINYC_CAMERA

// ---- WebUI: interactive widget page (/tc_ui) ----

static void HandleTinyCUI(void) {
  if (!HttpCheckPriviledgedAccess()) return;
  if (!Tinyc) { Webserver->send(503, "text/plain", "TinyC not ready"); return; }

  // Read page number from ?p= parameter (0-5, default 0)
  uint8_t page = 0;
  if (Webserver->hasArg(F("p"))) {
    page = Webserver->arg(F("p")).toInt();
    if (page >= TC_MAX_WEB_PAGES) page = 0;
  }
  Tinyc->current_page = page;

  // Find the slot that registered this page
  uint8_t si = (page < Tinyc->page_count) ? Tinyc->page_slot[page] : 0;
  TcSlot *s = Tinyc->slots[si];
  if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) {
    Webserver->send(503, "text/plain", "TinyC not ready");
    return;
  }

  // Handle sv= parameter -- widget value update
  TinyC_WebSetVar();

  // Reset WebChart state before rendering
  tc_chart_seq = 0;
  tc_chart_lib_sent = false;
  tc_chart_time_base = 0;

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
          "var r=x.responseText;"
          "if(r.indexOf('script src=')>=0){location.reload();return;}"
          "var el=document.getElementById('ui');"
          "el.innerHTML=r;"
          "var ss=el.getElementsByTagName('script');"
          "for(var i=0;i<ss.length;i++){eval(ss[i].textContent);}"
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
        ResponseAppend_P(PSTR(",\"TinyC\":{\"Running\":%d,\"Loaded\":%d,\"Size\":%d,\"Instr\":%u,\"Heap\":\"%u/%u\"}"),
          s->running ? 1 : 0,
          s->loaded ? 1 : 0,
          s->program_size,
          s->vm.instruction_count,
          s->vm.heap_used,
          s->vm.heap_capacity);
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

#ifdef USE_MQTT
/*********************************************************************************************\
 * MQTT subscribe / publish bridge for TinyC scripts
 * Scripts subscribe to external topics via mqttSubscribe("foo/#") and receive
 * data in a user-defined callback: void OnMqttData(char topic[], char payload[]).
 * '#' wildcard matches any suffix after the prefix (MQTT spec multi-level).
\*********************************************************************************************/

#define TC_MAX_MQTT_SUBS   10
#define TC_MQTT_TOPIC_LEN  128

struct TcMqttSub {
  char topic[TC_MQTT_TOPIC_LEN];
  bool active;
  bool wildcard;       // true if topic ends in '#' (multi-level) or contains '+'
  uint8_t prefix_len;  // bytes that must match verbatim before the wildcard
};

static TcMqttSub tc_mqtt_subs[TC_MAX_MQTT_SUBS];
static uint8_t   tc_mqtt_sub_count = 0;

static bool tc_mqtt_topic_match(const char *sub_topic, const char *recv_topic,
                                bool is_wildcard, uint8_t prefix_len) {
  if (!is_wildcard) return (strcmp(sub_topic, recv_topic) == 0);
  return (strncmp(sub_topic, recv_topic, prefix_len) == 0);
}

int tc_mqtt_subscribe(const char *topic) {
  if (!topic || !topic[0]) return -1;
  // Already subscribed? return existing slot
  for (uint8_t i = 0; i < tc_mqtt_sub_count; i++) {
    if (tc_mqtt_subs[i].active && strcmp(tc_mqtt_subs[i].topic, topic) == 0) return i;
  }
  int slot = -1;
  for (uint8_t i = 0; i < TC_MAX_MQTT_SUBS; i++) {
    if (!tc_mqtt_subs[i].active) { slot = i; break; }
  }
  if (slot < 0) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: MQTT sub failed, max %d"), TC_MAX_MQTT_SUBS);
    return -1;
  }
  strlcpy(tc_mqtt_subs[slot].topic, topic, TC_MQTT_TOPIC_LEN);
  tc_mqtt_subs[slot].active = true;
  const char *hash = strchr(topic, '#');
  if (hash) {
    tc_mqtt_subs[slot].wildcard = true;
    tc_mqtt_subs[slot].prefix_len = (uint8_t)(hash - topic);
  } else {
    tc_mqtt_subs[slot].wildcard = false;
    tc_mqtt_subs[slot].prefix_len = strlen(topic);
  }
  if ((uint8_t)slot >= tc_mqtt_sub_count) tc_mqtt_sub_count = slot + 1;
  MqttSubscribe(topic);
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: MQTT sub [%d] '%s'%s"),
         slot, topic, hash ? " (wildcard)" : "");
  return slot;
}

int tc_mqtt_unsubscribe(const char *topic) {
  if (!topic) return -1;
  for (uint8_t i = 0; i < tc_mqtt_sub_count; i++) {
    if (tc_mqtt_subs[i].active && strcmp(tc_mqtt_subs[i].topic, topic) == 0) {
      MqttUnsubscribe(topic);
      tc_mqtt_subs[i].active = false;
      tc_mqtt_subs[i].topic[0] = 0;
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: MQTT unsub [%d] '%s'"), i, topic);
      return 0;
    }
  }
  return -1;
}

// Re-send MQTT SUBSCRIBE packets after (re)connect. Hooked on FUNC_MQTT_INIT.
static void tc_mqtt_resubscribe(void) {
  for (uint8_t i = 0; i < tc_mqtt_sub_count; i++) {
    if (tc_mqtt_subs[i].active) {
      MqttSubscribe(tc_mqtt_subs[i].topic);
      AddLog(LOG_LEVEL_DEBUG, PSTR("TCC: MQTT resub [%d] '%s'"), i, tc_mqtt_subs[i].topic);
    }
  }
}

// Dispatch incoming MQTT message to matching subs → fires OnMqttData on each slot.
// Uses the cached well-known callback index (TC_CB_ON_MQTT_DATA) when present.
static bool tc_mqtt_data_handler(void) {
  if (tc_mqtt_sub_count == 0) return false;
  char *topic = XdrvMailbox.topic;
  char *data  = XdrvMailbox.data;
  if (!topic || !data) return false;

  bool any = false;
  for (uint8_t i = 0; i < tc_mqtt_sub_count; i++) {
    if (!tc_mqtt_subs[i].active) continue;
    if (!tc_mqtt_topic_match(tc_mqtt_subs[i].topic, topic,
                             tc_mqtt_subs[i].wildcard,
                             tc_mqtt_subs[i].prefix_len)) continue;

    // Variable-length local null-terminated copy of the payload
    uint32_t plen = XdrvMailbox.data_len;
    if (plen > 1024) plen = 1024;
    char payload[plen + 1];
    memcpy(payload, data, plen);
    payload[plen] = 0;

    for (uint8_t sidx = 0; sidx < TC_MAX_VMS; sidx++) {
      TcSlot *slot = Tinyc->slots[sidx];
      if (!slot || !slot->loaded) continue;
      if (!slot->vm.halted || slot->vm.error != TC_OK) continue;
      // Skip if script didn't define OnMqttData — fast cache check
      if (slot->vm.cb_index[TC_CB_ON_MQTT_DATA] < 0) continue;
      tc_current_slot = slot;
#ifdef ESP32
      if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
#endif
      tc_vm_call_callback_str2(&slot->vm, "OnMqttData", topic, payload);
#ifdef ESP32
      if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
#endif
      tc_current_slot = nullptr;
    }
    any = true;
    break;  // first matching sub wins — don't double-fire per broker message
  }
  return any;
}

// NOTE: no tc_mqtt_clear() — subs persist across script reloads so in-flight
// messages aren't lost during a TinyCRun. Scripts that want a clean slate
// should call mqttUnsubscribe() for their topics in OnExit().
#endif  // USE_MQTT

/*********************************************************************************************\
 * Dynamic task spawn — spawnTask / killTask / taskRunning (ESP32 only)
 *
 * Runs a named user function on a dedicated FreeRTOS task, sharing the spawning VM's
 * globals/heap. Mirrors the existing TaskLoop mutex-discipline: hold vm_mutex while
 * executing VM instructions, release during delay(). Cooperative kill via a flag
 * polled in the exec loop.
 *
 * The pool is global (spans all VM slots) — up to TC_MAX_SPAWN_TASKS live tasks at
 * a time. Task names are scoped per-VM-slot, so two slots can each have a task
 * named "Worker" without colliding.
\*********************************************************************************************/

#ifdef ESP32

#define TC_MAX_SPAWN_TASKS   4
#define TC_SPAWN_NAME_LEN    24
// Default native-task stack. tc_vm_step is a giant switch with lots of
// per-case locals and AddLog() uses vprintf — 3 KB crashes with
// StoreProhibited on first worker that logs. 5 KB is the minimum that
// works reliably. Users can still override per-call via the 2-arg form:
//   spawnTask("Worker", 8)  // KB
#define TC_SPAWN_STACK_DEF   5     // KB, 5*1024 = 5120
#define TC_SPAWN_STACK_MAX   16    // KB upper clamp
#define TC_SPAWN_STACK_MIN   3     // KB lower clamp (works for simple blink-only workers)
#define TC_SPAWN_WAIT_MAIN   5000  // ms to wait for main() to halt before giving up

struct TcSpawnTask {
  char          name[TC_SPAWN_NAME_LEN];  // "" = pool entry free
  TaskHandle_t  handle;
  TcSlot       *slot;
  uint8_t       slot_idx;         // 0..TC_MAX_VMS-1
  volatile uint8_t stop_requested;
  volatile uint8_t running;       // 1 while FreeRTOS task alive
};

static TcSpawnTask tc_spawn_pool[TC_MAX_SPAWN_TASKS];
static SemaphoreHandle_t tc_spawn_pool_mutex = nullptr;

static void tc_spawn_pool_lock(void) {
  if (!tc_spawn_pool_mutex) tc_spawn_pool_mutex = xSemaphoreCreateMutex();
  if (tc_spawn_pool_mutex) xSemaphoreTake(tc_spawn_pool_mutex, portMAX_DELAY);
}
static void tc_spawn_pool_unlock(void) {
  if (tc_spawn_pool_mutex) xSemaphoreGive(tc_spawn_pool_mutex);
}

// FreeRTOS task body — executes the user function to completion or stop-request.
// Re-resolves callback index by name at start (robust against VM reloads).
// Wrapped in a do/while(0) so `break` substitutes for goto-to-cleanup (C++
// forbids goto crossing initializations).
static void tc_spawn_task_body(void *param) {
  TcSpawnTask *entry = (TcSpawnTask *)param;
  TcSlot *slot = entry->slot;
  TcVM   *vm   = &slot->vm;
  const char *name = entry->name;

  do {
    // Wait for main() to halt (if this was spawned from inside main).
    // Most callers are in callback context where halted==true already.
    uint32_t waited = 0;
    while (!entry->stop_requested && slot->loaded && !vm->halted && waited < TC_SPAWN_WAIT_MAIN) {
      vTaskDelay(pdMS_TO_TICKS(10));
      waited += 10;
    }
    if (entry->stop_requested || !slot->loaded) break;
    if (!vm->halted) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask('%s') aborted — main never halted"), name);
      break;
    }

    // Re-resolve callback name → index (VM could have been reloaded between spawn + wake)
    int cb_idx = -1;
    for (uint8_t i = 0; i < vm->callback_count; i++) {
      if (strcmp(vm->callbacks[i].name, name) == 0) { cb_idx = i; break; }
    }
    if (cb_idx < 0) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask('%s') — function gone after wait"), name);
      break;
    }

    // Execute the user function under vm_mutex. The pattern follows TaskLoop:
    // release mutex during delay() so Tasmota callbacks can interleave.
    if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
    tc_current_slot = slot;

    uint8_t  saved_frame_count      = vm->frame_count;
    uint16_t saved_pc                = vm->pc;
    uint16_t saved_sp                = vm->sp;
    uint16_t saved_heap_used         = vm->heap_used;
    uint8_t  saved_heap_handle_count = vm->heap_handle_count;

    if (vm->frame_count >= TC_MAX_FRAMES) {
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask('%s') frame overflow"), name);
      tc_current_slot = nullptr;
      if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
      break;
    }

    vm->halted = false;
    vm->running = true;
    TcFrame *frame = &vm->frames[vm->frame_count];
    frame->return_pc = 0;
    if (!tc_frame_alloc(frame)) {
      vm->halted = true; vm->running = false;
      tc_current_slot = nullptr;
      if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
      AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask('%s') stack alloc fail"), name);
      break;
    }
    vm->fp = vm->frame_count;
    vm->frame_count++;
    vm->pc = vm->code_offset + vm->callbacks[cb_idx].address;

    AddLog(LOG_LEVEL_INFO, PSTR("TCC: spawnTask('%s') running on slot %d"), name, entry->slot_idx);

    uint32_t count = 0;
    while (vm->frame_count > saved_frame_count
           && !vm->halted
           && vm->error == TC_OK
           && !entry->stop_requested
           && slot->loaded) {
      int err = tc_vm_step(vm);
      if (err == TC_ERR_PAUSED) {
        if (vm->delayed) {
          // Release mutex during the actual sleep so other work can proceed.
          vm->halted = true;
          vm->running = false;
          tc_current_slot = nullptr;
          if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
          int32_t remaining = (int32_t)(vm->delay_until - millis());
          while (remaining > 0 && !entry->stop_requested && slot->loaded) {
            int32_t chunk = (remaining > 50) ? 50 : remaining;
            vTaskDelay(pdMS_TO_TICKS(chunk));
            remaining = (int32_t)(vm->delay_until - millis());
          }
          vm->delayed = false;
          if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
          tc_current_slot = slot;
          if (entry->stop_requested || !slot->loaded) break;
          vm->halted = false;
          vm->running = true;
        }
        continue;
      }
      if (err != TC_OK) {
        vm->error = err;
        AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask('%s') err %d at PC=%u"), name, err, vm->pc);
        tc_crash_log(err, vm->pc, vm->instruction_count, name);
        break;
      }
      count++;
      vm->instruction_count++;
      // Yield periodically to feed WDT and let other work run.
      if ((count & 0xFFFF) == 0) {
        vm->halted = true; vm->running = false;
        if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
        vTaskDelay(1);
        if (slot->vm_mutex) xSemaphoreTake(slot->vm_mutex, portMAX_DELAY);
        tc_current_slot = slot;
        if (entry->stop_requested || !slot->loaded) break;
        vm->halted = false; vm->running = true;
      }
    }

    // Clean up callback frame
    while (vm->frame_count > saved_frame_count) {
      tc_frame_free(&vm->frames[--vm->frame_count]);
    }
    vm->fp = vm->frame_count > 0 ? vm->frame_count - 1 : 0;

    vm->halted = true;
    vm->running = false;
    vm->pc = saved_pc;
    vm->sp = saved_sp;
    if (vm->heap_handles && vm->heap_handle_count > saved_heap_handle_count) {
      for (uint8_t i = saved_heap_handle_count; i < vm->heap_handle_count; i++) {
        vm->heap_handles[i].alive = false;
      }
      vm->heap_handle_count = saved_heap_handle_count;
    }
    vm->heap_used = saved_heap_used;
    tc_output_flush();

    tc_current_slot = nullptr;
    if (slot->vm_mutex) xSemaphoreGive(slot->vm_mutex);
  } while (0);

  AddLog(LOG_LEVEL_INFO, PSTR("TCC: spawnTask('%s') finished%s"),
         name, entry->stop_requested ? " (killed)" : "");
  tc_spawn_pool_lock();
  entry->handle = nullptr;
  entry->running = 0;
  entry->stop_requested = 0;
  entry->slot = nullptr;
  entry->slot_idx = 0xFF;
  entry->name[0] = 0;
  tc_spawn_pool_unlock();
  vTaskDelete(NULL);
}

// Called from SYS_SPAWN_TASK / SYS_SPAWN_TASK_STACK syscalls.
// stack_kb=0 → use default. Returns pool index (0..N-1) or -1.
int tc_spawn_task_create(const char *name, uint16_t stack_kb) {
  if (!name || !name[0]) return -1;
  if (strlen(name) >= TC_SPAWN_NAME_LEN) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask: name '%s' too long (max %d)"),
           name, TC_SPAWN_NAME_LEN - 1);
    return -1;
  }
  if (!tc_current_slot) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask: no active slot"));
    return -1;
  }

  // Find slot index of the caller
  uint8_t sidx = 0xFF;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    if (Tinyc->slots[i] == tc_current_slot) { sidx = i; break; }
  }
  if (sidx == 0xFF) return -1;

  // Verify function is defined — fail early so users catch typos
  TcVM *vm = &tc_current_slot->vm;
  int cb_idx = -1;
  for (uint8_t i = 0; i < vm->callback_count; i++) {
    if (strcmp(vm->callbacks[i].name, name) == 0) { cb_idx = i; break; }
  }
  if (cb_idx < 0) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask: function '%s' not defined"), name);
    return -1;
  }

  tc_spawn_pool_lock();
  // Already running on same slot? Refuse — user must killTask first.
  for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
    if (tc_spawn_pool[i].running
        && tc_spawn_pool[i].slot_idx == sidx
        && strcmp(tc_spawn_pool[i].name, name) == 0) {
      tc_spawn_pool_unlock();
      AddLog(LOG_LEVEL_INFO, PSTR("TCC: spawnTask('%s') already running on slot %d"), name, sidx);
      return -1;
    }
  }

  int free_idx = -1;
  for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
    if (!tc_spawn_pool[i].running && !tc_spawn_pool[i].handle) { free_idx = i; break; }
  }
  if (free_idx < 0) {
    tc_spawn_pool_unlock();
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask: pool full (max %d)"), TC_MAX_SPAWN_TASKS);
    return -1;
  }

  TcSpawnTask *entry = &tc_spawn_pool[free_idx];
  strlcpy(entry->name, name, TC_SPAWN_NAME_LEN);
  entry->slot = tc_current_slot;
  entry->slot_idx = sidx;
  entry->stop_requested = 0;
  entry->running = 1;
  entry->handle = nullptr;
  tc_spawn_pool_unlock();

  uint16_t stack_bytes = (stack_kb ? stack_kb : TC_SPAWN_STACK_DEF) * 1024;
  char tname[24];
  snprintf(tname, sizeof(tname), "tc_spawn_%u", (unsigned)free_idx);
  BaseType_t rc = xTaskCreatePinnedToCore(
    tc_spawn_task_body, tname, stack_bytes, entry,
    tskIDLE_PRIORITY + 1, &entry->handle, 1);
  if (rc != pdPASS) {
    tc_spawn_pool_lock();
    entry->running = 0;
    entry->handle = nullptr;
    entry->name[0] = 0;
    tc_spawn_pool_unlock();
    AddLog(LOG_LEVEL_ERROR, PSTR("TCC: spawnTask: xTaskCreate failed"));
    return -1;
  }
  return free_idx;
}

int tc_spawn_task_kill(const char *name) {
  if (!name || !name[0]) return -1;
  if (!tc_current_slot) return -1;
  uint8_t sidx = 0xFF;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    if (Tinyc->slots[i] == tc_current_slot) { sidx = i; break; }
  }
  if (sidx == 0xFF) return -1;

  int found = -1;
  tc_spawn_pool_lock();
  for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
    if (tc_spawn_pool[i].running
        && tc_spawn_pool[i].slot_idx == sidx
        && strcmp(tc_spawn_pool[i].name, name) == 0) {
      tc_spawn_pool[i].stop_requested = 1;
      found = 0;
      break;
    }
  }
  tc_spawn_pool_unlock();
  if (found == 0) AddLog(LOG_LEVEL_INFO, PSTR("TCC: killTask('%s') signaled"), name);
  return found;
}

int tc_spawn_task_running(const char *name) {
  if (!name || !name[0]) return 0;
  if (!tc_current_slot) return 0;
  uint8_t sidx = 0xFF;
  for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
    if (Tinyc->slots[i] == tc_current_slot) { sidx = i; break; }
  }
  if (sidx == 0xFF) return 0;

  int r = 0;
  tc_spawn_pool_lock();
  for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
    if (tc_spawn_pool[i].running
        && tc_spawn_pool[i].slot_idx == sidx
        && strcmp(tc_spawn_pool[i].name, name) == 0) {
      r = 1;
      break;
    }
  }
  tc_spawn_pool_unlock();
  return r;
}

// Called from TinyCStopVM to kill all spawned tasks owned by a VM slot
// before the VM is torn down. Each task observes stop_requested and exits
// cleanly at its next instruction boundary or delay boundary.
void tc_spawn_task_cleanup_slot(uint8_t slot_idx) {
  tc_spawn_pool_lock();
  int kills = 0;
  for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
    if (tc_spawn_pool[i].running && tc_spawn_pool[i].slot_idx == slot_idx) {
      tc_spawn_pool[i].stop_requested = 1;
      kills++;
    }
  }
  tc_spawn_pool_unlock();
  if (!kills) return;
  // Wait up to ~2 s for tasks to observe the flag and self-delete.
  for (int wait = 0; wait < 200; wait++) {
    vTaskDelay(pdMS_TO_TICKS(10));
    tc_spawn_pool_lock();
    int still = 0;
    for (uint8_t i = 0; i < TC_MAX_SPAWN_TASKS; i++) {
      if (tc_spawn_pool[i].running && tc_spawn_pool[i].slot_idx == slot_idx) still++;
    }
    tc_spawn_pool_unlock();
    if (!still) return;
  }
  AddLog(LOG_LEVEL_ERROR, PSTR("TCC: cleanup: %d spawn task(s) still alive on slot %d"), kills, slot_idx);
}

#endif  // ESP32

/*********************************************************************************************\
 * Tasmota: Driver entry point
\*********************************************************************************************/

// Event callback edge-detection flags
static bool tc_init_done = false;
static bool tc_wifi_up = false;

bool Xdrv124(uint32_t function) {
  bool result = false;

  if (FUNC_INIT == function) {
    TinyCInit();
    return false;
  }

  if (!Tinyc) { return false; }

  // Auto-pause VM callbacks during OTA updates and filesystem uploads
  // to prevent timeouts and upload failures
  bool tc_paused = (TasmotaGlobal.ota_state_flag != 0) ||
                   (TasmotaGlobal.restart_flag != 0) ||
                   Tinyc->upload_active ||
                   tc_global_pause;

  switch (function) {
    case FUNC_LOOP:
      if (tc_paused) { break; }
      // Poll UDP multicast for incoming variables
      tc_udp_poll();
#ifdef ESP32
      // Lazy-init port 82 download server once WiFi is connected
      if (!Tinyc->dl_server && WifiHasIP()) {
        TC_DLServerInit();
      }
      // Poll port 82 download server for incoming file requests
      TC_DLServerLoop();
#if defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA)
      // Poll port 81 MJPEG stream server
      TC_CamStreamLoop();
      // Run motion detection if enabled
      TC_CamMotionDetect();
#endif
#endif
      // Call user's EveryLoop() callback on all active slots
      tc_all_callbacks_id(TC_CB_LOOP);
      break;
    case FUNC_EVERY_50_MSECOND:
      if (tc_paused) { break; }
      TinyCEvery50ms();
      if (TasmotaGlobal.rules_flag.mqtt_disconnected) {
        TasmotaGlobal.rules_flag.mqtt_disconnected = 0;
        tc_all_callbacks_id(TC_CB_ON_MQTT_DISCONNECT);
      }
      break;
    case FUNC_EVERY_100_MSECOND:
      if (tc_paused) { break; }
      tc_all_callbacks_id(TC_CB_EVERY_100_MSECOND);
      break;
    case FUNC_EVERY_SECOND:
      if (tc_paused) { break; }
      // Call user's EverySecond() callback on all active slots
      tc_all_callbacks_id(TC_CB_EVERY_SECOND);
      break;
    case FUNC_NETWORK_UP:
      if (!tc_init_done) {
        tc_init_done = true;
        tc_all_callbacks_id(TC_CB_ON_INIT);
      }
      if (!tc_wifi_up) {
        tc_wifi_up = true;
        tc_all_callbacks_id(TC_CB_ON_WIFI_CONNECT);
      }
      break;
    case FUNC_NETWORK_DOWN:
      if (tc_wifi_up) {
        tc_wifi_up = false;
        tc_all_callbacks_id(TC_CB_ON_WIFI_DISCONNECT);
      }
      break;
    case FUNC_MQTT_INIT:
      tc_all_callbacks_id(TC_CB_ON_MQTT_CONNECT);
#ifdef USE_MQTT
      tc_mqtt_resubscribe();
#endif
      break;
#ifdef USE_MQTT
    case FUNC_MQTT_DATA:
      result = tc_mqtt_data_handler();
      break;
#endif
    case FUNC_TIME_SYNCED:
      tc_all_callbacks_id(TC_CB_ON_TIME_SET);
      break;
    case FUNC_COMMAND:
      result = DecodeCommand(kTinyCCommands, TinyCCommand);
      if (!result && Tinyc) {
        // Check each slot for registered command prefix match
        for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
          TcSlot *s = Tinyc->slots[i];
          if (!s || !s->loaded) continue;
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
#ifdef ESP32
            if (s->vm_mutex) xSemaphoreTake(s->vm_mutex, portMAX_DELAY);
#endif
            if (!s->vm.halted || s->vm.error != TC_OK) {
#ifdef ESP32
              if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
              continue;
            }
            tc_current_slot = s;
            tc_vm_call_callback_str(&s->vm, "Command", cmd_str);
            tc_current_slot = nullptr;
#ifdef ESP32
            if (s->vm_mutex) xSemaphoreGive(s->vm_mutex);
#endif
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
#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
      // Show MJPEG stream from port 81 — rendered once, not AJAX-refreshed
      // onerror retry reconnects if stream drops (e.g. client disconnect)
      if ((tc_cam_stream.server || tc_cam_stream.pending) && tc_cam_slot[0].len > 0) {
        // Use onload script to set src AFTER page is fully rendered
        // This prevents the stream request from blocking page load
        WSContentSend_P(PSTR("<p></p><center>"
          "<img id='tccam' onerror='setTimeout(()=>{this.src=\"http://%_I:%d/stream\";},2000)' "
          "alt='TinyC Camera' style='width:99%%;'>"
          "</center><p></p>"
          "<script>window.addEventListener('load',()=>{document.getElementById('tccam').src="
          "'http://%_I:%d/stream';});</script>"),
          (uint32_t)WiFi.localIP(), TC_CAM_STREAM_PORT,
          (uint32_t)WiFi.localIP(), TC_CAM_STREAM_PORT);
      }
#endif
      // Reset WebChart state before calling WebPage()
      tc_chart_seq = 0;
      tc_chart_lib_sent = false;
      tc_chart_width = 0;
      tc_chart_height = 0;
      tc_chart_time_base = 0;
      // Wrap charts in block container to prevent inline-block cascading width expansion
      WSContentSend_P(PSTR("<div style='display:block;width:100%%;overflow:hidden'>"));
      // Call user's WebPage() on all active slots
      for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
        TcSlot *s = Tinyc->slots[i];
        if (!s || !s->loaded || !s->vm.halted || s->vm.error != TC_OK) continue;
        tc_slot_callback(s, "WebPage");
      }
      WSContentSend_P(PSTR("</div>"));
      // Inject JavaScript for widget interactions on main page (all slots)
      {
        bool js_sent = false;
        bool has_webui = false;
        for (uint8_t i = 0; i < TC_MAX_VMS; i++) {
          TcSlot *si = Tinyc->slots[i];
          if (!si || !si->loaded || !si->vm.halted || si->vm.error != TC_OK) continue;
          if (!js_sent && tc_has_callback(&si->vm, "WebCall")) {
            WSContentSend_P(PSTR(
              "<script>"
              "function seva(v,i){rfsh=1;la('&sv='+i+'_'+v);rfsh=0;}"
              "function siva(v,i){la('&sv='+i+'_s_'+v);}"
              "function sivat(v,i){la('&sv='+i+'_t_'+v);}"
              "function pr(f){if(f){lt=setTimeout(la,%d);}else{clearTimeout(lt);clearTimeout(ft);}}"
              "</script>"
            ), Settings->web_refresh);
            js_sent = true;
          }
          if (tc_has_callback(&si->vm, "WebUI")) {
            has_webui = true;
          }
        }
        // Add buttons to /tc_ui pages if any slot has WebUI callback
        if (has_webui) {
          if (Tinyc->page_count > 0) {
            for (uint8_t p = 0; p < Tinyc->page_count; p++) {
              if (Tinyc->page_label[p][0]) {
                WSContentSend_P(PSTR("<p></p><form action='tc_ui' method='get'>"
                  "<input type='hidden' name='p' value='%d'>"
                  "<button>%s</button></form>"), p, Tinyc->page_label[p]);
              }
            }
          } else {
            WSContentSend_P(PSTR("<p></p><form action='tc_ui' method='get'><button>TinyC UI</button></form>"));
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
#if defined(ESP32) && (defined(USE_WEBCAM) || defined(USE_TINYC_CAMERA))
      WebServer_on(PSTR("/tc_cam"), HandleTinyCCam);
#endif
      // Register any webOn handlers that were set up before web server started
      if (Tinyc) {
        for (int i = 0; i < Tinyc->web_handler_count && i < TC_MAX_WEB_HANDLERS; i++) {
          if (Tinyc->web_handler_url[i][0]) {
            Webserver->on(Tinyc->web_handler_url[i], TinyCWebOnHandlers[i]);
            AddLog(LOG_LEVEL_INFO, PSTR("TCC: webOn(%d, \"%s\") registered (deferred)"), i + 1, Tinyc->web_handler_url[i]);
          }
        }
      }
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
      tc_all_callbacks_id(TC_CB_CLEAN_UP);
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
