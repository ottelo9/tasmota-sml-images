/*
  xdrv_124_tinyc_pcnt.h - TinyC hardware pulse counter (ESP32 PCNT) with a glitch filter

  pcntBegin(unit, pinA, pinB, filter_ns)   start counting on pinA (pinB < 0: one channel, rising edges of pinA; pinB >= 0: quadrature decode
                                           with pinB, x4 resolution and a direction); returns 1 / 0
  pcntRead(unit)                           the 32 bit count (the hardware counter is 16 bit, the driver accumulates the overflow)
  pcntClear(unit)                          count := 0
  pcntEnd(unit)                            stop and release the unit and the pins

  Why: Tasmota's own counters (Counter1..4) count in a GPIO interrupt and filter only in milliseconds. A geared motor with encoder produces
  2000..5000 pulses per second, so a millisecond debounce cannot tell a pulse from a spike that the motor PWM couples into the encoder wire.
  The PCNT counts in hardware (no interrupt, no VM time) and ignores every pulse shorter than filter_ns (up to about 12 us, the hardware limit
  is 1023 APB cycles = 12.8 us at 80 MHz; a larger value is clipped). The pins are used as inputs with the pull-up on (GPIO34..39 have none).

  The pin must be FREE in Tasmota: the same rule as for pinMode() (tc_pin_forbidden), i.e. `Gpio34 0`, not `Gpio34 352` (Counter1).
  Up to TC_PCNT_UNITS units (the chip has 4 to 8). A unit belongs to the slot that opened it; unloading the slot releases it.

  Chips without PCNT (ESP32-C3, ESP8266) and builds with -DTINYC_NO_PCNT: all four calls answer 0.

  Copyright (C) 2026  Gerhard Mutz and Claude. GNU GPL v3 or later, same as the rest of the TinyC driver.
*/

#ifndef XDRV_124_TINYC_PCNT_H
#define XDRV_124_TINYC_PCNT_H

#if defined(ESP32) && !defined(TINYC_NO_PCNT)
#include "soc/soc_caps.h"
#if defined(SOC_PCNT_SUPPORTED)

#define USE_TINYC_PCNT
#include "driver/pulse_cnt.h"

#define TC_PCNT_UNITS 4

struct TcPcnt {
  pcnt_unit_handle_t    unit;
  pcnt_channel_handle_t cha;
  pcnt_channel_handle_t chb;
  void                 *owner;       // the TcSlot that opened the unit: only its unload releases it
  bool                  used;
};
static TcPcnt tc_pcnt[TC_PCNT_UNITS];

static bool tc_pin_forbidden(int32_t pin);   // defined in xdrv_124_tinyc_vm.h

static void tc_pcnt_free(int u) {
  TcPcnt *p = &tc_pcnt[u];
  if (!p->used && !p->unit) { return; }
  if (p->unit) { pcnt_unit_stop(p->unit); pcnt_unit_disable(p->unit); }
  if (p->cha)  { pcnt_del_channel(p->cha); }
  if (p->chb)  { pcnt_del_channel(p->chb); }
  if (p->unit) { pcnt_del_unit(p->unit); }
  memset(p, 0, sizeof(*p));
}

// 1 ok / 0 failed (unit or pin invalid, pin claimed by Tasmota, driver error: see the log)
static int tc_pcnt_begin(int u, int pa, int pb, int filter_ns, void *owner) {
  if (u < 0 || u >= TC_PCNT_UNITS) { return 0; }
  if (pa < 0 || tc_pin_forbidden(pa)) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin pin %d forbidden or claimed by Tasmota (set it to None first)"), pa);
    return 0;
  }
  if (pb >= 0 && (pb == pa || tc_pin_forbidden(pb))) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin pin %d forbidden or claimed by Tasmota (set it to None first)"), pb);
    return 0;
  }
  tc_pcnt_free(u);                                  // reconfigure
  TcPcnt *p = &tc_pcnt[u];

  pcnt_unit_config_t uc = {};
  uc.low_limit  = -32768;
  uc.high_limit = 32767;
  uc.flags.accum_count = 1;                         // the driver adds the overflow: a 32 bit count
  esp_err_t e = pcnt_new_unit(&uc, &p->unit);
  if (e != ESP_OK) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin unit %d: %s"), u, esp_err_to_name(e)); memset(p, 0, sizeof(*p)); return 0; }
  p->used  = true;
  p->owner = owner;

  if (filter_ns > 0) {
    pcnt_glitch_filter_config_t fc = {};
    fc.max_glitch_ns = (uint32_t)filter_ns;
    e = pcnt_unit_set_glitch_filter(p->unit, &fc);
    if (e != ESP_OK) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin filter %d ns: %s"), filter_ns, esp_err_to_name(e)); tc_pcnt_free(u); return 0; }
  }

  pcnt_chan_config_t ca = {};
  ca.edge_gpio_num  = pa;
  ca.level_gpio_num = (pb >= 0) ? pb : -1;
  e = pcnt_new_channel(p->unit, &ca, &p->cha);
  if (e != ESP_OK) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin pin %d: %s"), pa, esp_err_to_name(e)); tc_pcnt_free(u); return 0; }
  if (pb >= 0) {                                    // quadrature, x4 (the example of the IDF "rotary encoder")
    pcnt_chan_config_t cb = {};
    cb.edge_gpio_num  = pb;
    cb.level_gpio_num = pa;
    e = pcnt_new_channel(p->unit, &cb, &p->chb);
    if (e != ESP_OK) { AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin pin %d: %s"), pb, esp_err_to_name(e)); tc_pcnt_free(u); return 0; }
    pcnt_channel_set_edge_action(p->cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(p->cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(p->chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(p->chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  } else {                                          // one channel: count the rising edges of pinA
    pcnt_channel_set_edge_action(p->cha, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
  }
  if (pcnt_unit_enable(p->unit) != ESP_OK || pcnt_unit_clear_count(p->unit) != ESP_OK || pcnt_unit_start(p->unit) != ESP_OK) {
    AddLog(LOG_LEVEL_INFO, PSTR("TCC: pcntBegin unit %d: enable/start failed"), u);
    tc_pcnt_free(u);
    return 0;
  }
  AddLog(LOG_LEVEL_INFO, PSTR("TCC: PCNT unit %d on GPIO%d%s, filter %d ns"), u, pa, pb >= 0 ? " + quadrature" : "", filter_ns);
  return 1;
}

static int32_t tc_pcnt_read(int u) {
  if (u < 0 || u >= TC_PCNT_UNITS || !tc_pcnt[u].used) { return 0; }
  int v = 0;
  pcnt_unit_get_count(tc_pcnt[u].unit, &v);
  return (int32_t)v;
}

static int tc_pcnt_clear(int u) {
  if (u < 0 || u >= TC_PCNT_UNITS || !tc_pcnt[u].used) { return 0; }
  return pcnt_unit_clear_count(tc_pcnt[u].unit) == ESP_OK ? 1 : 0;
}

static int tc_pcnt_end(int u) {
  if (u < 0 || u >= TC_PCNT_UNITS || !tc_pcnt[u].used) { return 0; }
  tc_pcnt_free(u);
  return 1;
}

// slot unload: release the units this slot opened (nullptr = all)
static void tc_pcnt_close_all(void *unloading_slot) {
  for (int u = 0; u < TC_PCNT_UNITS; u++) {
    if (!tc_pcnt[u].used) { continue; }
    if (unloading_slot && tc_pcnt[u].owner != unloading_slot) { continue; }
    tc_pcnt_free(u);
  }
}

#endif  // SOC_PCNT_SUPPORTED
#endif  // ESP32 && !TINYC_NO_PCNT
#endif  // XDRV_124_TINYC_PCNT_H
