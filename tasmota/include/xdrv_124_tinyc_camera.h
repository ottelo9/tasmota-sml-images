/*
  xdrv_124_tinyc_camera.h - fork-owned, TinyC-controlled ESP32-P4 MIPI-CSI camera

  Copyright (C) 2025 Gerhard Mutz / Tasmota fork

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.
*/

// ============================================================================
//  TinyC-owned MIPI-CSI camera (ESP32-P4)
// ----------------------------------------------------------------------------
//  A self-contained capture path for the OV5647 (Waveshare-class) MIPI module,
//  copied + trimmed from the upstream CSI webcam driver (xdrv_81_*). The camera
//  is owned ENTIRELY by the TinyC VM:
//    - Tasmota initializes NOTHING at boot — this file registers no Xdrv, has
//      no FUNC_INIT, no auto-stream. The upstream xdrv_81 driver stays OFF and
//      byte-for-byte unmodified.
//    - The pipeline lazy-inits on the first WcCsiCaptureJpeg() call, which is
//      the TinyC camera-capture syscall (camControl sel 10). First capture from
//      a TinyC script = the only thing that brings the hardware up.
//
//  Pulled in from xdrv_124_tinyc.ino. Only compiled when TinyC owns the camera
//  AND the upstream driver is disabled, so there is never a symbol clash and
//  WcCsiCaptureJpeg has exactly one definition.
//
//  Trimmed vs. upstream: no MJPEG processing task / port-81 server, no H.264 /
//  WebRTC / RTSP, no Tasmota commands, no ISP JSON tuning (CCM/gamma/AWB). The
//  ping-pong frame buffers are filled by the CSI ISR and read directly by the
//  one-shot capture — no worker task needed. ISP tuning is a quality follow-up.
// ============================================================================

#if defined(USE_TINYC_CAMERA) && !defined(USE_CSI_WEBCAM) && defined(CONFIG_IDF_TARGET_ESP32P4)

#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "driver/isp.h"
#include "esp_cache.h"
#include "driver/jpeg_encode.h"
#include "esp_ldo_regulator.h"
#include <Wire.h>

// ─────────────────────────────────────────────────────────────────────────────
//  OV5647 SCCB/I2C sensor driver  (verbatim from xdrv_81_0 CSI core, Berry-less)
// ─────────────────────────────────────────────────────────────────────────────
#define OV5647_ADDR     0x36
#define OV5647_CHIP_ID  0x5647

static TwoWire *ov5647_wire   = nullptr;
static bool     ov5647_inited = false;
static uint16_t ov5647_w = 640, ov5647_h = 480;
static uint16_t ov5647_mipi_clock = 291;
static uint8_t  ov5647_fmt = 1, ov5647_bin = 2;

static bool ov5647_wr(uint16_t reg, uint8_t val) {
  if (!ov5647_wire) { return false; }
  ov5647_wire->beginTransmission(OV5647_ADDR);
  ov5647_wire->write((reg >> 8) & 0xFF);
  ov5647_wire->write(reg & 0xFF);
  ov5647_wire->write(val);
  return ov5647_wire->endTransmission() == 0;
}
static int ov5647_rd(uint16_t reg) {           // -1 on error
  if (!ov5647_wire) { return -1; }
  ov5647_wire->beginTransmission(OV5647_ADDR);
  ov5647_wire->write((reg >> 8) & 0xFF);
  ov5647_wire->write(reg & 0xFF);
  if (ov5647_wire->endTransmission(false) != 0) { return -1; }
  if (ov5647_wire->requestFrom((int)OV5647_ADDR, 1) < 1) { return -1; }
  return ov5647_wire->available() ? ov5647_wire->read() : -1;
}
static bool ov5647_probe(TwoWire *w) {
  if (!w) { return false; }
  w->beginTransmission(OV5647_ADDR);
  return w->endTransmission() == 0;
}
static bool ov5647_detect() {
  ov5647_wire = nullptr;
  if (ov5647_probe(&Wire)) { ov5647_wire = &Wire; }
#ifdef USE_I2C_BUS2
  else if (ov5647_probe(&Wire1)) { ov5647_wire = &Wire1; }
#endif
  if (!ov5647_wire) { AddLog(LOG_LEVEL_INFO, PSTR("OV5647: I2C scan failed")); return false; }
  delay(10);
  int idh = ov5647_rd(0x300A), idl = ov5647_rd(0x300B);
  if (idh < 0 || idl < 0) { return false; }
  uint16_t id = (idh << 8) | idl;
  AddLog(LOG_LEVEL_INFO, PSTR("OV5647: Chip ID = 0x%04X"), id);
  return id == OV5647_CHIP_ID;
}
static bool ov5647_common_regs() {            // sleep + soft reset + clock-lane gate
  if (!ov5647_wr(0x0100, 0x00)) { return false; }
  if (!ov5647_wr(0x0103, 0x01)) { return false; }
  delay(10);
  return ov5647_wr(0x4800, 0x01);
}
// Dynamic PLL/geometry register generation (faithful to ov5647.be regs_custom).
static bool ov5647_regs_custom(int x, int y, int w, int h, int bin, int fps, int fmt) {
  w = (w / 8) * 8;  if (w < 16) { w = 16; }
  if (h < 16) { h = 16; }  if (fps < 1) { fps = 1; }
  ov5647_w = w; ov5647_h = h; ov5647_fmt = fmt; ov5647_bin = bin;

  int vts_min = h + 50, pll_mult;
  if (bin == 2) {
    pll_mult = (1896 * vts_min * fps + 417592) / 417593;
    if (pll_mult < 175) { pll_mult = 175; }
    if (pll_mult > 252) { pll_mult = 252; }
    if (pll_mult >= 128) { pll_mult = (pll_mult + 1) & 0xFE; }
    ov5647_mipi_clock = (4 * pll_mult + 1) / 3;
  } else {
    pll_mult = (2500 * vts_min * fps + 839999) / 840000;
    if (pll_mult < 80) { pll_mult = 80; }
    if (pll_mult > 252) { pll_mult = 252; }
    if (pll_mult >= 128) { pll_mult = (pll_mult + 1) & 0xFE; }
    ov5647_mipi_clock = 4 * pll_mult;
  }
  AddLog(LOG_LEVEL_INFO, PSTR("OV5647: CFG %dx%d Bin=%d Fmt=%d FPS=%d PLL=%d MIPI=%dMbps"),
         w, h, bin, fmt, fps, pll_mult, ov5647_mipi_clock);

  bool ok = true;
  if (bin == 2) {
    int start_x = (1296 - w) / 2 + x, start_y = (972 - h) / 2 + y;
    int off_x = 9 + start_x, off_y = start_y, hts = 1896;
    int vts = pll_mult * 417593 / (hts * fps);  if (vts < h + 50) { vts = h + 50; }
    ok &= ov5647_wr(0x3034, fmt == 0 ? 0x18 : 0x1a);
    ok &= ov5647_wr(0x3035, 0x41); ok &= ov5647_wr(0x3036, pll_mult);
    ok &= ov5647_wr(0x303c, 0x11); ok &= ov5647_wr(0x3106, 0xf5);
    ok &= ov5647_wr(0x3814, 0x31); ok &= ov5647_wr(0x3815, 0x31); ok &= ov5647_wr(0x3820, 0x41); ok &= ov5647_wr(0x3821, 0x03);
    ok &= ov5647_wr(0x3827, 0xec); ok &= ov5647_wr(0x370c, 0x0f); ok &= ov5647_wr(0x3612, 0x59); ok &= ov5647_wr(0x3618, 0x00);
    ok &= ov5647_wr(0x5000, 0xff); ok &= ov5647_wr(0x583e, 0xf0); ok &= ov5647_wr(0x583f, 0x20); ok &= ov5647_wr(0x5002, 0x41); ok &= ov5647_wr(0x5003, 0x08); ok &= ov5647_wr(0x5a00, 0x08);
    ok &= ov5647_wr(0x3000, 0x00); ok &= ov5647_wr(0x3001, 0x00); ok &= ov5647_wr(0x3002, 0x00); ok &= ov5647_wr(0x3016, 0x08); ok &= ov5647_wr(0x3017, 0xe0);
    ok &= ov5647_wr(0x3018, 0x44); ok &= ov5647_wr(0x301c, 0xf8); ok &= ov5647_wr(0x301d, 0xf0);
    ok &= ov5647_wr(0x3a18, 0x00); ok &= ov5647_wr(0x3a19, 0xf8); ok &= ov5647_wr(0x3c01, 0x80); ok &= ov5647_wr(0x3c00, 0x40); ok &= ov5647_wr(0x3b07, 0x0c);
    ok &= ov5647_wr(0x380c, 0x07); ok &= ov5647_wr(0x380d, 0x68);
    ok &= ov5647_wr(0x380e, (vts >> 8) & 0xFF); ok &= ov5647_wr(0x380f, vts & 0xFF);
    ok &= ov5647_wr(0x3800, 0x00); ok &= ov5647_wr(0x3801, 0x00); ok &= ov5647_wr(0x3802, 0x00); ok &= ov5647_wr(0x3803, 0x00);
    ok &= ov5647_wr(0x3804, 0x0a); ok &= ov5647_wr(0x3805, 0x3f); ok &= ov5647_wr(0x3806, 0x07); ok &= ov5647_wr(0x3807, 0xa1);
    ok &= ov5647_wr(0x3808, (w >> 8) & 0xFF); ok &= ov5647_wr(0x3809, w & 0xFF);
    ok &= ov5647_wr(0x380a, (h >> 8) & 0xFF); ok &= ov5647_wr(0x380b, h & 0xFF);
    ok &= ov5647_wr(0x3810, (off_x >> 8) & 0xFF); ok &= ov5647_wr(0x3811, off_x & 0xFF);
    ok &= ov5647_wr(0x3812, (off_y >> 8) & 0xFF); ok &= ov5647_wr(0x3813, off_y & 0xFF);
    ok &= ov5647_wr(0x3630, 0x2e); ok &= ov5647_wr(0x3632, 0xe2); ok &= ov5647_wr(0x3633, 0x23); ok &= ov5647_wr(0x3634, 0x44); ok &= ov5647_wr(0x3636, 0x06);
    ok &= ov5647_wr(0x3620, 0x64); ok &= ov5647_wr(0x3621, 0xe0); ok &= ov5647_wr(0x3600, 0x37);
    ok &= ov5647_wr(0x3704, 0xa0); ok &= ov5647_wr(0x3703, 0x5a); ok &= ov5647_wr(0x3715, 0x78); ok &= ov5647_wr(0x3717, 0x01); ok &= ov5647_wr(0x3731, 0x02);
    ok &= ov5647_wr(0x370b, 0x60); ok &= ov5647_wr(0x3705, 0x1a);
    ok &= ov5647_wr(0x3f05, 0x02); ok &= ov5647_wr(0x3f06, 0x10); ok &= ov5647_wr(0x3f01, 0x0a);
    ok &= ov5647_wr(0x3a08, 0x01); ok &= ov5647_wr(0x3a09, 0x27); ok &= ov5647_wr(0x3a0a, 0x00); ok &= ov5647_wr(0x3a0b, 0xf6); ok &= ov5647_wr(0x3a0d, 0x04);
    ok &= ov5647_wr(0x3a0e, 0x03); ok &= ov5647_wr(0x3a0f, 0x58); ok &= ov5647_wr(0x3a10, 0x50); ok &= ov5647_wr(0x3a1b, 0x58); ok &= ov5647_wr(0x3a1e, 0x50);
    ok &= ov5647_wr(0x3a11, 0x60); ok &= ov5647_wr(0x3a1f, 0x28);
    ok &= ov5647_wr(0x4001, 0x02); ok &= ov5647_wr(0x4004, 0x02); ok &= ov5647_wr(0x4000, 0x09);
    ok &= ov5647_wr(0x4837, 0x28); ok &= ov5647_wr(0x4050, 0x6e); ok &= ov5647_wr(0x4051, 0x8f);
  } else {
    int start_x = (2592 - w) / 2 + x, start_y = (1944 - h) / 2 + y;
    int win_x = start_x + 12, win_y = start_y + 2;
    int end_x = win_x + w + 8 - 1, end_y = win_y + h + 8 - 1;
    if (end_x > 2611) { end_x = 2611; }  if (end_y > 1953) { end_y = 1953; }
    int hts = 2500;
    int vts = pll_mult * 840000 / (hts * fps);  if (vts < h + 50) { vts = h + 50; }
    ok &= ov5647_wr(0x3034, fmt == 0 ? 0x18 : 0x1a);
    ok &= ov5647_wr(0x3035, 0x21); ok &= ov5647_wr(0x3036, pll_mult); ok &= ov5647_wr(0x303c, 0x11); ok &= ov5647_wr(0x3106, 0xf5);
    ok &= ov5647_wr(0x3814, 0x11); ok &= ov5647_wr(0x3815, 0x11); ok &= ov5647_wr(0x3820, 0x00); ok &= ov5647_wr(0x3821, 0x02);
    ok &= ov5647_wr(0x3800, (win_x >> 8) & 0xFF); ok &= ov5647_wr(0x3801, win_x & 0xFF);
    ok &= ov5647_wr(0x3802, (win_y >> 8) & 0xFF); ok &= ov5647_wr(0x3803, win_y & 0xFF);
    ok &= ov5647_wr(0x3804, (end_x >> 8) & 0xFF); ok &= ov5647_wr(0x3805, end_x & 0xFF);
    ok &= ov5647_wr(0x3806, (end_y >> 8) & 0xFF); ok &= ov5647_wr(0x3807, end_y & 0xFF);
    ok &= ov5647_wr(0x3808, (w >> 8) & 0xFF); ok &= ov5647_wr(0x3809, w & 0xFF);
    ok &= ov5647_wr(0x380a, (h >> 8) & 0xFF); ok &= ov5647_wr(0x380b, h & 0xFF);
    ok &= ov5647_wr(0x3810, 0x00); ok &= ov5647_wr(0x3811, 0x05); ok &= ov5647_wr(0x3812, 0x00); ok &= ov5647_wr(0x3813, 0x02);
    ok &= ov5647_wr(0x380c, (hts >> 8) & 0xFF); ok &= ov5647_wr(0x380d, hts & 0xFF);
    ok &= ov5647_wr(0x380e, (vts >> 8) & 0xFF); ok &= ov5647_wr(0x380f, vts & 0xFF);
    ok &= ov5647_wr(0x3708, 0x64); ok &= ov5647_wr(0x3709, 0x12); ok &= ov5647_wr(0x3827, 0xec); ok &= ov5647_wr(0x370c, 0x03);
    ok &= ov5647_wr(0x3612, 0x5b); ok &= ov5647_wr(0x3618, 0x04);
    ok &= ov5647_wr(0x3630, 0x2e); ok &= ov5647_wr(0x3632, 0xe2); ok &= ov5647_wr(0x3633, 0x23); ok &= ov5647_wr(0x3634, 0x44); ok &= ov5647_wr(0x3636, 0x06);
    ok &= ov5647_wr(0x3620, 0x64); ok &= ov5647_wr(0x3621, 0xe0); ok &= ov5647_wr(0x3600, 0x37);
    ok &= ov5647_wr(0x3704, 0xa0); ok &= ov5647_wr(0x3703, 0x5a); ok &= ov5647_wr(0x3715, 0x78); ok &= ov5647_wr(0x3717, 0x01); ok &= ov5647_wr(0x3731, 0x02);
    ok &= ov5647_wr(0x370b, 0x60); ok &= ov5647_wr(0x3705, 0x1a);
    ok &= ov5647_wr(0x5000, 0xff); ok &= ov5647_wr(0x583e, 0xf0); ok &= ov5647_wr(0x583f, 0x4f);
    ok &= ov5647_wr(0x5003, 0x08); ok &= ov5647_wr(0x5a00, 0x08);
    ok &= ov5647_wr(0x3a0f, 0x30); ok &= ov5647_wr(0x3a10, 0x28); ok &= ov5647_wr(0x3a1b, 0x30); ok &= ov5647_wr(0x3a1e, 0x26);
    ok &= ov5647_wr(0x3a18, 0xff); ok &= ov5647_wr(0x3a19, 0x00);
    ok &= ov5647_wr(0x3a08, 0x01); ok &= ov5647_wr(0x3a09, 0x4b); ok &= ov5647_wr(0x3a0a, 0x01); ok &= ov5647_wr(0x3a0b, 0x13);
    ok &= ov5647_wr(0x3000, 0x00); ok &= ov5647_wr(0x3001, 0x00); ok &= ov5647_wr(0x3002, 0x00);
    ok &= ov5647_wr(0x3016, 0x08); ok &= ov5647_wr(0x3017, 0xe0);
    ok &= ov5647_wr(0x3018, 0x44); ok &= ov5647_wr(0x301c, 0xf8); ok &= ov5647_wr(0x301d, 0xf0);
    ok &= ov5647_wr(0x3c00, 0x40); ok &= ov5647_wr(0x3b07, 0x0c);
    ok &= ov5647_wr(0x3a11, 0x60); ok &= ov5647_wr(0x3a1f, 0x28);
    ok &= ov5647_wr(0x4001, 0x02); ok &= ov5647_wr(0x4004, 0x04); ok &= ov5647_wr(0x4000, 0x09);
    ok &= ov5647_wr(0x4837, 0x19); ok &= ov5647_wr(0x4800, 0x34);
  }
  return ok;
}
static bool ov5647_stream(bool on) {
  if (!ov5647_wire) { return false; }
  if (on) {
    if (!ov5647_wr(0x4800, 0x14)) { return false; }   // MIPI enable
    if (!ov5647_wr(0x0100, 0x01)) { return false; }   // stream enable
    AddLog(LOG_LEVEL_INFO, PSTR("OV5647: Stream ON"));
  } else {
    if (!ov5647_wr(0x0100, 0x00)) { return false; }
    AddLog(LOG_LEVEL_INFO, PSTR("OV5647: Stream OFF"));
  }
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Slim CSI pipeline state (no worker task, no H.264/RTSP/WebRTC/MJPEG-server)
// ─────────────────────────────────────────────────────────────────────────────
typedef enum { TCAM_IDLE = 0, TCAM_STREAMING, TCAM_FAILED } tcam_state_t;

static struct {
  esp_cam_ctlr_handle_t      cam_handle;
  isp_proc_handle_t          isp_handle;
  esp_ldo_channel_handle_t   ldo_mipi_phy;
  uint8_t                   *fb[2];           // ping-pong YUV422 frame buffers (PSRAM)
  size_t                     fb_size;
  esp_cam_ctlr_trans_t       trans;
  volatile int               write_idx;
  volatile int               read_idx;
  volatile tcam_state_t      state;
  volatile uint32_t          frames;          // incremented by the CSI ISR
  SemaphoreHandle_t          frame_mutex;
  jpeg_encoder_handle_t      jpeg;
  void                      *jpeg_buf;
  size_t                     jpeg_buf_size;
  jpeg_encode_cfg_t          jpeg_cfg;
  SemaphoreHandle_t          jpeg_mutex;
  uint16_t                   width;
  uint16_t                   height;
  uint8_t                    format;          // color_pixel_raw_format_t
  uint8_t                    lanes;
  uint16_t                   mipi_clock;       // Mbps/lane
  uint8_t                    quality;          // JPEG quality
} Tcam = {};

// Default capture geometry. 640x480, 2x2 binning, RAW10, 30fps — the proven
// Waveshare OV5647 startup config. (A future cameraInit() could override.)
#define TCAM_DEF_W      640
#define TCAM_DEF_H      480
#define TCAM_DEF_BIN    2
#define TCAM_DEF_FPS    30
#define TCAM_DEF_FMT    1     // 1 = RAW10
#define TCAM_DEF_QUAL   50

// ── CSI ISR callbacks: ping-pong only, no task to notify (capture polls frames) ──
static bool IRAM_ATTR tcam_on_get_new_vb(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
  if (Tcam.state != TCAM_STREAMING) { return false; }
  int next_idx = (Tcam.write_idx + 1) % 2;
  Tcam.write_idx = next_idx;
  trans->buffer = Tcam.fb[next_idx];
  trans->buflen = Tcam.fb_size;
  return false;
}
static bool IRAM_ATTR tcam_on_trans_finished(esp_cam_ctlr_handle_t handle, esp_cam_ctlr_trans_t *trans, void *user_data) {
  if (Tcam.state != TCAM_STREAMING) { return false; }
  Tcam.read_idx = Tcam.write_idx;   // the buffer just filled is now readable
  Tcam.frames++;
  return false;
}

static void tcam_deinit(void) {
  Tcam.state = TCAM_IDLE;
  if (Tcam.cam_handle) {
    esp_cam_ctlr_stop(Tcam.cam_handle);
    esp_cam_ctlr_disable(Tcam.cam_handle);
    esp_cam_ctlr_del(Tcam.cam_handle);
    Tcam.cam_handle = nullptr;
  }
  ov5647_stream(false);
  if (Tcam.isp_handle) { esp_isp_disable(Tcam.isp_handle); esp_isp_del_processor(Tcam.isp_handle); Tcam.isp_handle = nullptr; }
  if (Tcam.jpeg) { jpeg_del_encoder_engine(Tcam.jpeg); Tcam.jpeg = nullptr; }
  if (Tcam.jpeg_buf) { free(Tcam.jpeg_buf); Tcam.jpeg_buf = nullptr; }
  for (int i = 0; i < 2; i++) { if (Tcam.fb[i]) { free(Tcam.fb[i]); Tcam.fb[i] = nullptr; } }
  if (Tcam.ldo_mipi_phy) { esp_ldo_release_channel(Tcam.ldo_mipi_phy); Tcam.ldo_mipi_phy = nullptr; }
  if (Tcam.frame_mutex) { vSemaphoreDelete(Tcam.frame_mutex); Tcam.frame_mutex = nullptr; }
  if (Tcam.jpeg_mutex)  { vSemaphoreDelete(Tcam.jpeg_mutex);  Tcam.jpeg_mutex  = nullptr; }
}

// Full hardware bring-up: MIPI LDO + OV5647 + CSI ctlr + ISP + JPEG encoder +
// mutexes, then start streaming. Runs once, on the first capture (VM task).
static bool tcam_init(void) {
  if (Tcam.state == TCAM_STREAMING) { return true; }
  esp_err_t ret;
  AddLog(LOG_LEVEL_INFO, PSTR("TCAM: ===== TinyC camera bring-up ====="));

  // 1. MIPI PHY LDO (LDO_VO3, 2.5V)
  esp_ldo_channel_config_t ldo_cfg = { .chan_id = 3, .voltage_mv = 2500 };
  ret = esp_ldo_acquire_channel(&ldo_cfg, &Tcam.ldo_mipi_phy);
  if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: MIPI LDO failed (0x%x)"), ret); goto fail; }

  // 2. OV5647 sensor: detect + soft-reset + geometry/PLL regs
  if (!ov5647_inited) {
    if (!ov5647_detect()) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: OV5647 not found")); goto fail; }
    ov5647_inited = true;
  }
  if (!ov5647_common_regs()) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: OV5647 reset failed")); goto fail; }
  if (!ov5647_regs_custom(0, 0, TCAM_DEF_W, TCAM_DEF_H, TCAM_DEF_BIN, TCAM_DEF_FPS, TCAM_DEF_FMT)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: OV5647 config failed")); goto fail;
  }
  Tcam.width      = ov5647_w;
  Tcam.height     = ov5647_h;
  Tcam.format     = ov5647_fmt;
  Tcam.lanes      = 2;
  Tcam.mipi_clock = ov5647_mipi_clock;
  Tcam.quality    = TCAM_DEF_QUAL;

  // 3. Ping-pong frame buffers (YUV422 = 2 bytes/pixel), PSRAM, 64-byte aligned
  Tcam.fb_size = (size_t)Tcam.width * Tcam.height * 2;
  for (int i = 0; i < 2; i++) {
    Tcam.fb[i] = (uint8_t*)heap_caps_aligned_calloc(64, 1, Tcam.fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!Tcam.fb[i]) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: frame buffer %d alloc failed"), i); goto fail; }
    esp_cache_msync(Tcam.fb[i], Tcam.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  }
  Tcam.write_idx = 0;
  Tcam.read_idx  = 1;

  // 4. CSI controller (RAW in -> YUV422 out for the JPEG encoder)
  {
    esp_cam_ctlr_csi_config_t csi_config = {
      .ctlr_id            = 0,
      .h_res              = Tcam.width,
      .v_res              = Tcam.height,
      .data_lane_num      = Tcam.lanes,
      .lane_bit_rate_mbps = (int)Tcam.mipi_clock,
      .input_data_color_type  = (cam_ctlr_color_t)COLOR_TYPE_ID(COLOR_SPACE_RAW, (color_pixel_raw_format_t)Tcam.format),
      .output_data_color_type = CAM_CTLR_COLOR_YUV422,
      .queue_items        = 1,
      .byte_swap_en       = false,
    };
    ret = esp_cam_new_csi_ctlr(&csi_config, &Tcam.cam_handle);
    if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: CSI init failed (0x%x)"), ret); goto fail; }

    esp_cam_ctlr_evt_cbs_t cbs = { .on_get_new_trans = tcam_on_get_new_vb, .on_trans_finished = tcam_on_trans_finished };
    Tcam.trans.buffer = Tcam.fb[0];
    Tcam.trans.buflen = Tcam.fb_size;
    ret = esp_cam_ctlr_register_event_callbacks(Tcam.cam_handle, &cbs, &Tcam.trans);
    if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: CSI cb reg failed (0x%x)"), ret); goto fail; }
    ret = esp_cam_ctlr_enable(Tcam.cam_handle);
    if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: CSI enable failed (0x%x)"), ret); goto fail; }
  }

  // 5. ISP (RAW -> YUV422 demosaic). No JSON tuning (CCM/gamma/AWB) — follow-up.
  {
    esp_isp_processor_cfg_t isp_config = {
      .clk_hz                 = 120 * 1000 * 1000,
      .input_data_source      = ISP_INPUT_DATA_SOURCE_CSI,
      .input_data_color_type  = (isp_color_t)COLOR_TYPE_ID(COLOR_SPACE_RAW, (color_pixel_raw_format_t)Tcam.format),
      .output_data_color_type = ISP_COLOR_YUV422,
      .h_res                  = Tcam.width,
      .v_res                  = Tcam.height,
    };
    ret = esp_isp_new_processor(&isp_config, &Tcam.isp_handle);
    if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: ISP init failed (0x%x)"), ret); goto fail; }
    esp_isp_enable(Tcam.isp_handle);
  }

  // 6. JPEG encoder engine + output buffer (copied from WcSetupJpegEncoder)
  {
    jpeg_encode_engine_cfg_t jpeg_eng_cfg = { .intr_priority = 0, .timeout_ms = 100 };
    ret = jpeg_new_encoder_engine(&jpeg_eng_cfg, &Tcam.jpeg);
    if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: JPEG enc init failed (0x%x)"), ret); goto fail; }
    jpeg_encode_memory_alloc_cfg_t jpeg_mem_cfg = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t actual_size = 0;
    Tcam.jpeg_buf = jpeg_alloc_encoder_mem((size_t)Tcam.width * Tcam.height / 2, &jpeg_mem_cfg, &actual_size);
    if (!Tcam.jpeg_buf) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: JPEG buffer alloc failed")); goto fail; }
    Tcam.jpeg_buf_size = actual_size;
    Tcam.jpeg_cfg = {
      .height       = Tcam.height,
      .width        = Tcam.width,
      .src_type     = JPEG_ENCODE_IN_FORMAT_YUV422,
      .sub_sample   = JPEG_DOWN_SAMPLING_YUV422,
      .image_quality = Tcam.quality,
    };
  }

  // 7. Mutexes
  Tcam.frame_mutex = xSemaphoreCreateMutex();
  Tcam.jpeg_mutex  = xSemaphoreCreateMutex();
  if (!Tcam.frame_mutex || !Tcam.jpeg_mutex) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: mutex create failed")); goto fail; }

  // 8. Start: CSI controller + sensor stream on
  Tcam.frames = 0;
  ret = esp_cam_ctlr_start(Tcam.cam_handle);
  if (ret != ESP_OK) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: CSI start failed (0x%x)"), ret); goto fail; }
  if (!ov5647_stream(true)) { AddLog(LOG_LEVEL_ERROR, PSTR("TCAM: OV5647 stream-on failed")); goto fail; }
  Tcam.state = TCAM_STREAMING;   // ISR active from here

  // Warm-up: the OV5647 auto-exposure/AWB takes ~1 s of streaming to converge.
  // The very first frame is underexposed = all-zero YUV422, which decodes to a
  // solid-green JPEG. Discard ~1.5 s of frames so the FIRST capture is already a
  // real, exposed image. One-time cost on the init / first-capture path.
  {
    uint32_t t0 = millis();
    while (Tcam.frames < 40 && (millis() - t0) < 1500) { delay(20); }
  }
  AddLog(LOG_LEVEL_INFO, PSTR("TCAM: streaming %dx%d (MIPI %d Mbps/lane, Q%d) — %u warm-up frames"),
         Tcam.width, Tcam.height, Tcam.mipi_clock, Tcam.quality, (unsigned)Tcam.frames);
  return true;

fail:
  tcam_deinit();
  Tcam.state = TCAM_FAILED;
  return false;
}

// Sensor PID accessor for the camControl(8) getSensorPID syscall — returns the
// OV5647 chip id once tcam_init() has detected the sensor, else -1.
static int tcam_sensor_pid(void) { return ov5647_inited ? OV5647_CHIP_ID : -1; }

// ─────────────────────────────────────────────────────────────────────────────
//  WcCsiCaptureJpeg — the one symbol the TinyC VM camControl(10) dispatch calls.
//  Lazy-inits the whole pipeline on first call (Tasmota never does), waits for a
//  fresh frame, HW-encodes it to JPEG. *buf points into Tcam.jpeg_buf (valid only
//  until the next capture) — the VM copies it straight into its PSRAM slot.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t WcCsiCaptureJpeg(uint8_t **buf, uint32_t *len, int *w, int *h) {
  if (buf) { *buf = nullptr; }
  if (len) { *len = 0; }
  if (w)   { *w = 0; }
  if (h)   { *h = 0; }

  if (Tcam.state != TCAM_STREAMING) {
    if (!tcam_init()) { return 0; }
  }
  if (!Tcam.jpeg || !Tcam.jpeg_buf) { return 0; }

  // Wait for the ISR to capture a fresh frame (up to ~600 ms).
  uint32_t start_frames = Tcam.frames;
  uint32_t t0 = millis();
  while (Tcam.frames == start_frames && (millis() - t0) < 600) { delay(5); }
  if (Tcam.frames == start_frames) { return 0; }   // no frame in time

  if (xSemaphoreTake(Tcam.frame_mutex, pdMS_TO_TICKS(200)) != pdTRUE) { return 0; }
  if (xSemaphoreTake(Tcam.jpeg_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    xSemaphoreGive(Tcam.frame_mutex);
    return 0;
  }

  uint8_t *src = Tcam.fb[Tcam.read_idx];
  esp_cache_msync(src, Tcam.fb_size, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

  uint32_t jpeg_size = 0;
  esp_err_t ret = jpeg_encoder_process(Tcam.jpeg, &Tcam.jpeg_cfg,
                                       src, Tcam.fb_size,
                                       (uint8_t*)Tcam.jpeg_buf, Tcam.jpeg_buf_size,
                                       &jpeg_size);
  if (ret == ESP_OK && jpeg_size > 0) {
    if (buf) { *buf = (uint8_t*)Tcam.jpeg_buf; }
    if (len) { *len = jpeg_size; }
    if (w)   { *w = Tcam.width; }
    if (h)   { *h = Tcam.height; }
  } else {
    jpeg_size = 0;
  }

  xSemaphoreGive(Tcam.jpeg_mutex);
  xSemaphoreGive(Tcam.frame_mutex);
  return jpeg_size;
}

#endif  // USE_TINYC_CAMERA && !USE_CSI_WEBCAM && CONFIG_IDF_TARGET_ESP32P4
