/*
  xdrv_54_lvgl.ino - LVLG integration

  Copyright (C) 2021 Stephan Hadinger, Berry language by Guan Wenliang https://github.com/Skiars/berry

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

#ifdef ESP32
#if defined(USE_LVGL) && defined(USE_UNIVERSAL_DISPLAY)

#include <renderer.h>
#include "lvgl.h"
#include "core/lv_global.h"         // needed for LV_GLOBAL_DEFAULT
#include "tasmota_lvgl_assets.h"    // force compilation of assets

#define XDRV_54             54

#define LV_MAGIC    0x564C //LV
#define CHUNK_SIZE  1024

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// callback type when a screen paint is done
typedef void (*lv_paint_cb_t)(int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint8_t *pixels);
// callback type when a stream buffer is ready
typedef void (*lv_stream_cb_t)(const uint8_t *buf, size_t len, bool last);

struct LVGL_Glue {
  lv_display_t *lv_display = nullptr;
  lv_indev_t *lv_indev = nullptr;
  void *lv_pixel_buf = nullptr;
  void *lv_pixel_buf2 = nullptr;
  Ticker tick;
  File * screenshot = nullptr;
  lv_paint_cb_t paint_cb = nullptr;
  lv_stream_cb_t stream_cb = nullptr;
};
LVGL_Glue * lvgl_glue;

// **************************************************
// Logging
// **************************************************
#if LV_USE_LOG
#ifdef USE_BERRY
static void lvbe_debug(lv_log_level_t, const char *msg);
static void lvbe_debug(lv_log_level_t, const char *msg) {
  be_writebuffer("LVG: ", sizeof("LVG: "));
  be_writebuffer(msg, strlen(msg));
}
#endif
#endif

/************************************************************
 * Main screen refresh function
 ************************************************************/
// This is the flush function required for LittlevGL screen updates.
// It receives a bounding rect and an array of pixel data (conveniently
// already in 565 format, so the Earth was lucky there).
void lv_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p);
void lv_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p) {
  uint16_t width = (area->x2 - area->x1 + 1);
  uint16_t height = (area->y2 - area->y1 + 1);

  // check if we are currently doing a screenshot
  if (lvgl_glue->screenshot != nullptr) {
    // save pixels to file
    int32_t btw = (width * height * LV_COLOR_DEPTH + 7) / 8;
    yield();            // ensure WDT does not fire
    while (btw > 0) {
      if (btw > 0) {    // if we had a previous error (ex disk full) don't try to write anymore
        int32_t ret = lvgl_glue->screenshot->write((const uint8_t*) color_p, btw);
        if (ret >= 0) {
          btw -= ret;
        } else {
          btw = 0;  // abort
        }
      }
    }
    lv_disp_flush_ready(disp);
    return; // ok
  }

  uint32_t pixels_len = width * height;
  uint32_t chrono_start = millis();
  renderer->setAddrWindow(area->x1, area->y1, area->x1+width, area->y1+height);
  renderer->pushColors((uint16_t *)color_p, pixels_len, true);
  renderer->setAddrWindow(0,0,0,0);
  renderer->Updateframe();
  uint32_t chrono_time = millis() - chrono_start;

  lv_disp_flush_ready(disp);

  if (pixels_len >= 10000 && (!renderer->lvgl_param.use_dma)) {
    if (HighestLogLevel() >= LOG_LEVEL_DEBUG_MORE) {
      AddLog(LOG_LEVEL_DEBUG_MORE, D_LOG_LVGL "Refreshed %d pixels in %d ms (%i pix/ms)", pixels_len, chrono_time,
              chrono_time > 0 ? pixels_len / chrono_time : -1);
    }
  }
  // if there is a display callback, call it
  if (lvgl_glue->paint_cb != nullptr) {
    lvgl_glue->paint_cb(area->x1, area->y1, area->x2, area->y2, color_p);
  }

  // if there is a stream callback, process it
  if (lvgl_glue->stream_cb != nullptr) {
    lv_process_stream(area->x1, area->y1, width, height, (const uint16_t*)color_p, pixels_len);
  }
}

void lv_set_paint_cb(void* cb);
void lv_set_paint_cb(void* cb) {
  lvgl_glue->paint_cb = (lv_paint_cb_t) cb;
}

void * lv_get_paint_cb(void);
void * lv_get_paint_cb(void) {
  return (void*) lvgl_glue->paint_cb;
}

void lv_set_stream_cb(void* cb);
void lv_set_stream_cb(void* cb) {
  lvgl_glue->stream_cb = (lv_stream_cb_t)cb;
}

void * lv_get_stream_cb(void);
void * lv_get_stream_cb(void) {
  return (void*) lvgl_glue->stream_cb;
}

void lv_process_stream(int32_t x, int32_t y, int32_t width, int32_t height, const uint16_t *pixels, uint32_t len) {
  static uint16_t chunk[CHUNK_SIZE];
  size_t chunk_pos = 0;

  auto emit = [&](uint16_t val) {
      chunk[chunk_pos++] = val;
      if (chunk_pos >= CHUNK_SIZE) {
          chunk_pos = 0;
          lvgl_glue->stream_cb((const uint8_t *)chunk, CHUNK_SIZE * 2, false);
      }
  };

  emit(LV_MAGIC);
  emit((uint16_t)x);
  emit((uint16_t)y);
  emit((uint16_t)width);
  emit((uint16_t)height);

  const uint16_t *p = pixels;
  const uint16_t *end = pixels + len;
  while (p < end) {
    uint16_t run = 1;
    uint16_t limit = min(p + 0x7FFF + 2, end) - p;
    while (run < limit && p[run] == *p) run++;
    if (run >= 2) {
      emit(0x8000 | (run - 2));
      emit(*p);
      p += run;
    } else {
      uint16_t rlen = 0;
      while (p + rlen + 1 < end && p[rlen] != p[rlen + 1]) rlen++;
      if (rlen == 0) rlen++;
      emit(rlen - 1);
      for (uint16_t j = 0; j < rlen; j++)
        emit(*p++);
    }
  }
  if (chunk_pos > 0)
    lvgl_glue->stream_cb((const uint8_t *)chunk, chunk_pos * 2, true);
}

/************************************************************
 * Emulation of stdio for FreeType
 *
 ************************************************************/

#ifdef USE_UFILESYS

#include <FS.h>
#include "ZipReadFS.h"
extern FS *ffsp;
extern FS *ufsp;
FS lv_zip_ufsp(ZipReadFSImplPtr(new ZipReadFSImpl(&ffsp, "/sd/", &ufsp)));

extern "C" {

  typedef void lvbe_FILE;

  // FILE * fopen ( const char * filename, const char * mode );
  lvbe_FILE * lvbe_fopen(const char * filename, const char * mode ) {

    // Add "/" prefix
    String file_path = "/";
    file_path += filename;

    File f = lv_zip_ufsp.open(file_path, mode);
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fopen(%s) -> %i", file_path.c_str(), (int32_t)f);
    // AddLog(LOG_LEVEL_INFO, "LVG: F=%*_H", sizeof(f), &f);
    if (f) {
      File * f_ptr = new File(f);                 // copy to dynamic object
      *f_ptr = f;                                 // TODO is this necessary?
      return f_ptr;
    }
    return nullptr;
  }

  // int fclose ( FILE * stream );
  lv_fs_res_t lvbe_fclose(lvbe_FILE * stream) {
    File * f_ptr = (File*) stream;
    f_ptr->close();
    delete f_ptr;
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fclose(%p)", f_ptr);
    return LV_FS_RES_OK;
  }

  // size_t fread ( void * ptr, size_t size, size_t count, FILE * stream );
  size_t lvbe_fread(void * ptr, size_t size, size_t count, lvbe_FILE * stream) {
    File * f_ptr = (File*) stream;
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fread (%p, %i, %i, %p)", ptr, size, count, f_ptr);

    int32_t ret = f_ptr->read((uint8_t*) ptr, size * count);
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fread -> %i", ret);
    if (ret < 0) {    // error
      ret = 0;
    }
    return ret;
  }

  // int fseek ( FILE * stream, long int offset, int origin );
  int lvbe_fseek(lvbe_FILE * stream, long int offset, int origin ) {
    File * f_ptr = (File*) stream;
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fseek(%p, %i, %i)", f_ptr, offset, origin);

    fs::SeekMode mode = fs::SeekMode::SeekSet;
    if (SEEK_CUR == origin) {
      mode = fs::SeekMode::SeekCur;
    } else if (SEEK_END == origin) {
      mode = fs::SeekMode::SeekEnd;
    }
    bool ok = f_ptr->seek(offset, mode);
    return ok ? 0 : -1;
  }

  // long int ftell ( FILE * stream );
  int lvbe_ftell(lvbe_FILE * stream) {
    File * f_ptr = (File*) stream;
    // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_ftell(%p) -> %i", f_ptr, f_ptr->position());
    return f_ptr->position();
  }

}
#endif // USE_UFILESYS

/************************************************************
 * Callbacks for file system access from LVGL
 *
 * Useful to load fonts or images from file system
 ************************************************************/

#ifdef USE_UFILESYS
static void * lvbe_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode);
static void * lvbe_fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_open(%p, %p, %s, %i) %i", drv, file_p, path, mode, sizeof(File));
  const char * modes = nullptr;
  switch (mode) {
    case LV_FS_MODE_WR:                   modes = "w";    break;
    case LV_FS_MODE_RD:                   modes = "r";    break;
    case LV_FS_MODE_WR | LV_FS_MODE_RD:   modes = "rw";   break;
  }

  if (modes == nullptr) {
    AddLog(LOG_LEVEL_INFO, "LVG: fs_open, unsupported mode %d", mode);
    return nullptr;
  }

  return (void*) lvbe_fopen(path, modes);
}

static lv_fs_res_t lvbe_fs_close(lv_fs_drv_t * drv, void * file_p);
static lv_fs_res_t lvbe_fs_close(lv_fs_drv_t * drv, void * file_p) {
  return lvbe_fclose((void*)file_p);
}

static lv_fs_res_t lvbe_fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br);
static lv_fs_res_t lvbe_fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_read(%p, %p, %p, %i, %p)", drv, file_p, buf, btr, br);
  File * f_ptr = (File*) file_p;
  // AddLog(LOG_LEVEL_INFO, "LVG: F=%*_H", sizeof(File), f_ptr);
  int32_t ret = f_ptr->read((uint8_t*) buf, btr);
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_read -> %i", ret);
  if (ret >= 0) {
    *br = ret;
    return LV_FS_RES_OK;
  } else {
    return LV_FS_RES_UNKNOWN;
  }
}

static lv_fs_res_t lvbe_fs_write(lv_fs_drv_t * drv, void * file_p, const void * buf, uint32_t btw, uint32_t * bw);
static lv_fs_res_t lvbe_fs_write(lv_fs_drv_t * drv, void * file_p, const void * buf, uint32_t btw, uint32_t * bw) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_write(%p, %p, %p, %i, %p)", drv, file_p, buf, btw, bw);
  File * f_ptr = (File*) file_p;
  int32_t ret = f_ptr->write((const uint8_t*) buf, btw);
  if (ret >= 0) {
    *bw = ret;
    return LV_FS_RES_OK;
  } else {
    return LV_FS_RES_UNKNOWN;
  }
}

static lv_fs_res_t lvbe_fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p);
static lv_fs_res_t lvbe_fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_tell(%p, %p, %p)", drv, file_p, pos_p);
  File * f_ptr = (File*) file_p;
  *pos_p = f_ptr->position();
  return LV_FS_RES_OK;
}

static lv_fs_res_t lvbe_fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence);
static lv_fs_res_t lvbe_fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_seek(%p, %p, %i)", drv, file_p, pos);
  File * f_ptr = (File*) file_p;
  SeekMode seek;
  switch (whence) {
    case LV_FS_SEEK_SET: seek = SeekSet; break;
    case LV_FS_SEEK_CUR: seek = SeekCur; break;
    case LV_FS_SEEK_END: seek = SeekEnd; break;
    default: return LV_FS_RES_UNKNOWN;
  }

  if (f_ptr->seek(pos, seek)) {
    return LV_FS_RES_OK;
  } else {
    return LV_FS_RES_UNKNOWN;
  }
}

static lv_fs_res_t lvbe_fs_size(lv_fs_drv_t * drv, void * file_p, uint32_t * size_p);
static lv_fs_res_t lvbe_fs_size(lv_fs_drv_t * drv, void * file_p, uint32_t * size_p) {
  // AddLog(LOG_LEVEL_INFO, "LVG: lvbe_fs_size(%p, %p, %p)", drv, file_p, size_p);
  File * f_ptr = (File*) file_p;
  *size_p = f_ptr->size();
  return LV_FS_RES_OK;
}
#endif // USE_UFILESYS

/*********************************************************************************************\
 * Memory handler
 * Use PSRAM if available
\*********************************************************************************************/
extern "C" {
  /*
  Use the following

  extern void *lvbe_malloc(size_t size);
  extern void  lvbe_free(void *ptr);
  extern void *lvbe_realloc(void *ptr, size_t size);
  extern void *lvbe_calloc(size_t num, size_t size);
  */
  void *lvbe_malloc(uint32_t size);
  void *lvbe_realloc(void *ptr, size_t size);
  void *lvbe_calloc(size_t num, size_t size);
#ifdef USE_BERRY_PSRAM
  void *lvbe_malloc(uint32_t size) {
    return special_malloc(size);
  }
  void *lvbe_realloc(void *ptr, size_t size) {
    return special_realloc(ptr, size);
  }
  void *lvbe_calloc(size_t num, size_t size) {
    return special_calloc(num, size);
  }
#else // USE_BERRY_PSRAM
  void *lvbe_malloc(uint32_t size) {
    return malloc(size);
  }
  void *lvbe_realloc(void *ptr, size_t size) {
    return realloc(ptr, size);
  }
  void *lvbe_calloc(size_t num, size_t size) {
    return calloc(num, size);
  }
#endif // USE_BERRY_PSRAM

  void lvbe_free(void *ptr) {
    free(ptr);
  }

#ifdef USE_LVGL_PNG_DECODER
  // for PNG decoder, use same allocators as LVGL
  void* lodepng_malloc(size_t size) { return lvbe_malloc(size); }
  void* lodepng_realloc(void* ptr, size_t new_size) { return lvbe_realloc(ptr, new_size); }
  void lodepng_free(void* ptr) { lvbe_free(ptr); }
#endif // USE_LVGL_PNG_DECODER

#if defined(USE_TINYC_LVGL) && !defined(USE_BERRY)
  // LVGL custom memory backend (LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM). Normally provided by
  // lv_binding_berry/lv_mem_core_berry.c; supplied here for the Berry-free TinyC build, routing to
  // the lvbe_malloc/realloc/free above (PSRAM when USE_BERRY_PSRAM). Mirrors the LVGL 9 lv_mem API.
  void lv_mem_init(void) { }
  void lv_mem_deinit(void) { }
  lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
  void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }
  void *lv_malloc_core(size_t size) { return lvbe_malloc(size); }
  void *lv_realloc_core(void *p, size_t new_size) { return lvbe_realloc(p, new_size); }
  void lv_free_core(void *p) { lvbe_free(p); }
  void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }
  lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
#endif // USE_TINYC_LVGL && !USE_BERRY

}

// ARCHITECTURE-SPECIFIC TIMER STUFF ---------------------------------------

extern void lv_flush_callback(lv_display_t *disp, const lv_area_t *area, uint8_t * px_map);

// Tick interval for LittlevGL internal timekeeping; 1 to 10 ms recommended
static const int lv_tick_interval_ms = 5;

static void lv_tick_handler(void) { lv_tick_inc(lv_tick_interval_ms); }

// TOUCHSCREEN STUFF -------------------------------------------------------

uint32_t Touch_Status(int32_t sel);

#if defined(USE_TINYC_LVGL) && !defined(USE_BERRY)
// Touchscreen calibration record — normally provided by the Berry LVGL binding
// (lv_binding_berry/lv_berry.c). On the Berry-free TinyC build, define it here so
// lvgl_touchscreen_read() can stash the last touch for calibration.
typedef struct lv_ts_calibration_t {
  lv_coord_t        raw_x;
  lv_coord_t        raw_y;
  lv_coord_t        x;
  lv_coord_t        y;
  lv_indev_state_t  state;
} lv_ts_calibration_t;
lv_ts_calibration_t lv_ts_calibration = { 0, 0, 0, 0, LV_INDEV_STATE_RELEASED };
#endif

//typedef void (*lv_indev_read_cb_t)(lv_indev_t * indev, lv_indev_data_t * data);
void lvgl_touchscreen_read(lv_indev_t *indev_drv, lv_indev_data_t *data);
void lvgl_touchscreen_read(lv_indev_t *indev_drv, lv_indev_data_t *data) {
  data->point.x = Touch_Status(1); // Last-pressed coordinates
  data->point.y = Touch_Status(2);
  data->state = Touch_Status(0) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->continue_reading = false; /*No buffering now so no more data read*/
  // keep data for TS calibration
  lv_ts_calibration.state = data->state;
  if (data->state == LV_INDEV_STATE_PRESSED) {    // if not pressed, the data may be invalid
    lv_ts_calibration.x = data->point.x;
    lv_ts_calibration.y = data->point.y;
    lv_ts_calibration.raw_x = Touch_Status(-1);
    lv_ts_calibration.raw_y = Touch_Status(-2);
  }
}

// Actual RAM usage will be 2X these figures, since using 2 DMA buffers...
#define LV_BUFFER_ROWS 60 // Most others have a bit more space

/************************************************************
 * Initialize the display / touchscreen drivers then launch lvgl
 *
 * We use our own simplified mapping on top of Universal display
 ************************************************************/
extern Renderer *Init_uDisplay(const char *desc);

void start_lvgl(const char * uconfig);
void start_lvgl(const char * uconfig) {

  if (lvgl_glue != nullptr) {
    AddLog(LOG_LEVEL_DEBUG_MORE, D_LOG_LVGL "LVGL was already initialized");
    return;
  }

  if (!renderer || uconfig) {
    renderer  = Init_uDisplay((char*)uconfig);
    AddLog(LOG_LEVEL_ERROR, "LVG: Could not start Universal Display");
    if (!renderer) return;
  }

  renderer->DisplayOnff(true);

  // **************************************************
  // Initialize LVGL
  // **************************************************
  lvgl_glue = new LVGL_Glue;

  // Initialize lvgl_glue, passing in address of display & touchscreen
  lv_init();

  // Allocate LvGL display buffer (x2 because DMA double buffering)
  bool status_ok = true;
  size_t lvgl_buffer_size;
  do {
    uint32_t flushlines = renderer->lvgl_pars()->flushlines;
    if (0 == flushlines) flushlines = LV_BUFFER_ROWS;

    lvgl_buffer_size = renderer->width() * flushlines;
    if (renderer->lvgl_pars()->use_dma) {
      lvgl_buffer_size /= 2;
      if (lvgl_buffer_size < 1000000) {
        // allocate preferably in internal memory which is faster than PSRAM
        AddLog(LOG_LEVEL_DEBUG, "LVG: Allocating buffer2 %i bytes in main memory (flushlines %i)", (lvgl_buffer_size * (LV_COLOR_DEPTH / 8)) / 1024, flushlines);
        lvgl_glue->lv_pixel_buf2 = heap_caps_malloc_prefer(lvgl_buffer_size * (LV_COLOR_DEPTH / 8), 2, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT);
      }
      if (!lvgl_glue->lv_pixel_buf2) {
        status_ok = false;
        break;
      }
    }

    // allocate preferably in internal memory which is faster than PSRAM
    AddLog(LOG_LEVEL_DEBUG, "LVG: Allocating buffer1 %i KB in main memory (flushlines %i)", (lvgl_buffer_size * (LV_COLOR_DEPTH / 8)) / 1024, flushlines);
    lvgl_glue->lv_pixel_buf = heap_caps_malloc_prefer(lvgl_buffer_size * (LV_COLOR_DEPTH / 8), 2, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL, MALLOC_CAP_8BIT);
    if (!lvgl_glue->lv_pixel_buf) {
      status_ok = false;
      break;
    }
  } while (0);

  if (!status_ok) {
    if (lvgl_glue->lv_pixel_buf) {
      free(lvgl_glue->lv_pixel_buf);
      lvgl_glue->lv_pixel_buf = NULL;
    }
    if (lvgl_glue->lv_pixel_buf2) {
      free(lvgl_glue->lv_pixel_buf2);
      lvgl_glue->lv_pixel_buf2 = NULL;
    }
    delete lvgl_glue;
    lvgl_glue = nullptr;
    AddLog(LOG_LEVEL_ERROR, "LVG: Could not allocate buffers");
    return;
  }

  // Initialize LvGL display driver
  lvgl_glue->lv_display = lv_display_create(renderer->width(), renderer->height());
  lv_display_set_dpi(lvgl_glue->lv_display, 160);          // set display to 160 DPI instead of default 130 DPI to avoid some rounding in styles
  lv_display_set_flush_cb(lvgl_glue->lv_display, lv_flush_callback);
  lv_display_set_buffers(lvgl_glue->lv_display, lvgl_glue->lv_pixel_buf, lvgl_glue->lv_pixel_buf2, lvgl_buffer_size * (LV_COLOR_DEPTH / 8), LV_DISPLAY_RENDER_MODE_PARTIAL);

  // Initialize LvGL input device (touchscreen already started)
  lvgl_glue->lv_indev = lv_indev_create();
  lv_indev_set_type(lvgl_glue->lv_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(lvgl_glue->lv_indev, lvgl_touchscreen_read);

  // ESP 32------------------------------------------------
  lvgl_glue->tick.attach_ms(lv_tick_interval_ms, lv_tick_handler);
  // -----------------------------------------

  // Set the default background color of the display
  // This is normally overriden by an opaque screen on top
#ifdef USE_BERRY
  // By default set the display color to black and opacity to 100%
  lv_obj_t * background = lv_layer_bottom();
  lv_obj_set_style_bg_color(background, lv_color_hex(USE_LVGL_BG_DEFAULT), static_cast<uint32_t>(LV_PART_MAIN) | static_cast<uint32_t>(LV_STATE_DEFAULT));
  lv_obj_set_style_bg_opa(background, LV_OPA_COVER, static_cast<uint32_t>(LV_PART_MAIN) | static_cast<uint32_t>(LV_STATE_DEFAULT));
  // lv_disp_set_bg_color(NULL, lv_color_from_uint32(USE_LVGL_BG_DEFAULT));
  // lv_disp_set_bg_opa(NULL, LV_OPA_COVER);
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(USE_LVGL_BG_DEFAULT), static_cast<uint32_t>(LV_PART_MAIN) | static_cast<uint32_t>(LV_STATE_DEFAULT));
  lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, static_cast<uint32_t>(LV_PART_MAIN) | static_cast<uint32_t>(LV_STATE_DEFAULT));


#if LV_USE_LOG
  lv_log_register_print_cb(lvbe_debug);
#endif // LV_USE_LOG
#endif

#ifdef USE_UFILESYS
  // Add file system mapping
  static lv_fs_drv_t drv;      // LVGL8, needs to be static and not on stack
  lv_fs_drv_init(&drv);                     /*Basic initialization*/

  drv.letter = 'A';                         /*An uppercase letter to identify the drive */
  drv.ready_cb = nullptr;               /*Callback to tell if the drive is ready to use */
  drv.open_cb = &lvbe_fs_open;                 /*Callback to open a file */
  drv.close_cb = &lvbe_fs_close;               /*Callback to close a file */
  drv.read_cb = &lvbe_fs_read;                 /*Callback to read a file */
  drv.write_cb = &lvbe_fs_write;               /*Callback to write a file */
  drv.seek_cb = &lvbe_fs_seek;                 /*Callback to seek in a file (Move cursor) */
  drv.tell_cb = &lvbe_fs_tell;                 /*Callback to tell the cursor position  */

  drv.dir_open_cb = nullptr;         /*Callback to open directory to read its content */
  drv.dir_read_cb = nullptr;         /*Callback to read a directory's content */
  drv.dir_close_cb = nullptr;       /*Callback to close a directory */
  // drv.user_data = nullptr;             /*Any custom data if required*/

  lv_fs_drv_register(&drv);                 /*Finally register the drive*/

#endif // USE_UFILESYS

#ifdef USE_LVGL_FREETYPE
  // initialize the FreeType renderer
  lv_freetype_init(USE_LVGL_FREETYPE_MAX_FACES);
  // lv_freetype_init(USE_LVGL_FREETYPE_MAX_FACES,
  //                  USE_LVGL_FREETYPE_MAX_SIZES,
  //                  UsePSRAM() ? USE_LVGL_FREETYPE_MAX_BYTES_PSRAM : USE_LVGL_FREETYPE_MAX_BYTES);
#endif
#ifdef USE_LVGL_PNG_DECODER
  lv_lodepng_init();
#endif // USE_LVGL_PNG_DECODER

  // TODO check later about cache size
  if (UsePSRAM()) {
    lv_cache_set_max_size(LV_GLOBAL_DEFAULT()->img_cache, LV_IMG_CACHE_DEF_SIZE_PSRAM, nullptr);
  } else {
    lv_cache_set_max_size(LV_GLOBAL_DEFAULT()->img_cache, LV_IMG_CACHE_DEF_SIZE_NOPSRAM, nullptr);
  }

  AddLog(LOG_LEVEL_INFO, PSTR(D_LOG_LVGL "LVGL initialized"));
}

/*********************************************************************************************\
 * Callable from Berry
\*********************************************************************************************/
bool lvgl_started(void);
bool lvgl_started(void) {
  return (lvgl_glue != nullptr);
}

void lvgl_set_screenshot_file(File * file);
void lvgl_set_screenshot_file(File * file) {
  lvgl_glue->screenshot = file;
}

void lvgl_reset_screenshot_file(void);
void lvgl_reset_screenshot_file(void) {
  lvgl_glue->screenshot = nullptr;
}

File * lvgl_get_screenshot_file(void);
File * lvgl_get_screenshot_file(void) {
  return lvgl_glue->screenshot;
}

/*********************************************************************************************\
 * TinyC <-> LVGL glue (Berry-free). Lets a TinyC program drive LVGL on the device panel.
 * Declared in xdrv_124_tinyc_vm.h (earlier in the .ino TU) and called from the lvgl*
 * syscalls there. LVGL is not reentrant, so a single recursive mutex guards lv_task_handler
 * (main task, FUNC_LOOP below) against the syscalls (VM task).
\*********************************************************************************************/
#ifdef USE_TINYC_LVGL
static SemaphoreHandle_t tc_lvgl_mtx = nullptr;

void tc_lvgl_lock(void) { if (tc_lvgl_mtx) { xSemaphoreTakeRecursive(tc_lvgl_mtx, portMAX_DELAY); } }
void tc_lvgl_unlock(void) { if (tc_lvgl_mtx) { xSemaphoreGiveRecursive(tc_lvgl_mtx); } }

int tc_lvgl_active(void) { return lvgl_started() ? 1 : 0; }

// Branded startup splash, shown once when LVGL starts. Drawn on the TOP layer so it
// sits above every screen and survives screen switches (the program can build/load its
// own screens underneath immediately). Auto-removes after 5 s — fully non-blocking.
static void tc_lvgl_splash(void) {
  lv_obj_t *sp = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(sp);
  lv_obj_set_size(sp, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(sp, lv_color_hex(0x0b1c2c), 0);
  lv_obj_set_style_bg_opa(sp, LV_OPA_COVER, 0);
  lv_obj_t *t = lv_label_create(sp);
  lv_label_set_text(t, "TinyC LVGL");
  lv_obj_set_style_text_color(t, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_tasmota_28, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -10);
  lv_obj_t *s = lv_label_create(sp);
  lv_label_set_text(s, "starting ...");
  lv_obj_set_style_text_color(s, lv_color_hex(0x66aadd), 0);
  lv_obj_align(s, LV_ALIGN_CENTER, 0, 22);
  lv_obj_delete_delayed(sp, 5000);
}

// Idempotent. Reuses the already-running Universal Display (renderer != null) by passing
// nullptr to start_lvgl(). Holds the lock across start_lvgl so FUNC_LOOP can't run
// lv_task_handler on a half-initialised lvgl_glue. Shows the splash on the first start.
int tc_lvgl_init(void) {
  if (!tc_lvgl_mtx) { tc_lvgl_mtx = xSemaphoreCreateRecursiveMutex(); }
  tc_lvgl_lock();
  bool fresh = !lvgl_started();
  if (fresh) { start_lvgl(nullptr); }
  if (fresh && lvgl_started()) { tc_lvgl_splash(); }
  tc_lvgl_unlock();
  return lvgl_started() ? 1 : 0;
}

/* ── Phase 1: object handle table + widgets + event poll ──────────────────────
   TinyC addresses LVGL objects by integer HANDLE (1..TC_LV_MAX). Handle 0 means
   "the active screen" (so a TinyC program can style/parent onto the screen). Every
   created object gets an LV_EVENT_DELETE cb that nulls its slot, so handles can never
   dangle when LVGL auto-deletes children. Events use a poll ring (TinyC has no
   C->VM callback) drained by lvglEvent()/lvglEventObj()/lvglEventCode().
   All LVGL access is under tc_lvgl_lock() (VM task) vs lv_task_handler (main task). */
#define TC_LV_MAX   128
#define TC_LV_EVQ   16
static lv_obj_t  *tc_lv_obj_tab[TC_LV_MAX];
static struct { uint16_t handle; uint16_t code; } tc_lv_evq[TC_LV_EVQ];
static volatile uint8_t tc_lv_evq_head = 0, tc_lv_evq_tail = 0;
static portMUX_TYPE tc_lv_evq_mux = portMUX_INITIALIZER_UNLOCKED;
static struct { int handle; int code; } tc_lv_ev_cur = { 0, 0 };

// Explicit forward declarations for the lv_*-typed helpers, so Tasmota's .ino
// auto-prototype generator does NOT emit its own at the top of tasmota.ino (where
// lv_event_t/lv_obj_t aren't in scope yet) — same pattern as lvgl_touchscreen_read.
static lv_obj_t *tc_lv_resolve(int h);
static void tc_lv_delete_cb(lv_event_t *e);
static int tc_lv_store(lv_obj_t *o);
static void tc_lv_event_cb(lv_event_t *e);

static lv_obj_t *tc_lv_resolve(int h) {
  if (h == 0) { return lv_screen_active(); }
  if (h < 1 || h > TC_LV_MAX) { return nullptr; }
  return tc_lv_obj_tab[h - 1];
}
static void tc_lv_delete_cb(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  for (int i = 0; i < TC_LV_MAX; i++) { if (tc_lv_obj_tab[i] == o) { tc_lv_obj_tab[i] = nullptr; break; } }
}
static int tc_lv_store(lv_obj_t *o) {
  if (!o) { return 0; }
  for (int i = 0; i < TC_LV_MAX; i++) {
    if (!tc_lv_obj_tab[i]) {
      tc_lv_obj_tab[i] = o;
      lv_obj_add_event_cb(o, tc_lv_delete_cb, LV_EVENT_DELETE, nullptr);
      return i + 1;
    }
  }
  lv_obj_delete(o);                 // table full — don't leak the orphan
  return 0;
}
static void tc_lv_event_cb(lv_event_t *e) {            // main task, FUNC_LOOP lock held
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  int code = (int)lv_event_get_code(e);
  int h = 0;
  for (int i = 0; i < TC_LV_MAX; i++) { if (tc_lv_obj_tab[i] == o) { h = i + 1; break; } }
  if (!h) { return; }
  portENTER_CRITICAL(&tc_lv_evq_mux);
  uint8_t nxt = (tc_lv_evq_head + 1) % TC_LV_EVQ;
  if (nxt != tc_lv_evq_tail) {                          // drop if ring full
    tc_lv_evq[tc_lv_evq_head].handle = (uint16_t)h;
    tc_lv_evq[tc_lv_evq_head].code   = (uint16_t)code;
    tc_lv_evq_head = nxt;
  }
  portEXIT_CRITICAL(&tc_lv_evq_mux);
}

// create (parent handle, 0 = active screen) -> new handle, 0 on failure
int tc_lv_obj(int parent)    { tc_lvgl_lock(); int h = tc_lv_store(lv_obj_create(tc_lv_resolve(parent)));    tc_lvgl_unlock(); return h; }
int tc_lv_label(int parent)  { tc_lvgl_lock(); int h = tc_lv_store(lv_label_create(tc_lv_resolve(parent)));  tc_lvgl_unlock(); return h; }
int tc_lv_button(int parent) { tc_lvgl_lock(); int h = tc_lv_store(lv_button_create(tc_lv_resolve(parent))); tc_lvgl_unlock(); return h; }

// common props (no-op on a bad handle)
void tc_lv_set_pos(int h, int x, int y)   { lv_obj_t *o = tc_lv_resolve(h); if (o) { tc_lvgl_lock(); lv_obj_set_pos(o, x, y);   tc_lvgl_unlock(); } }
void tc_lv_set_size(int h, int w, int ht) { lv_obj_t *o = tc_lv_resolve(h); if (o) { tc_lvgl_lock(); lv_obj_set_size(o, w, ht); tc_lvgl_unlock(); } }
void tc_lv_align(int h, int a, int dx, int dy) { lv_obj_t *o = tc_lv_resolve(h); if (o) { tc_lvgl_lock(); lv_obj_align(o, (lv_align_t)a, dx, dy); tc_lvgl_unlock(); } }
void tc_lv_set_text(int h, const char *s) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || !s) { return; }
  tc_lvgl_lock();
  if (lv_obj_check_type(o, &lv_checkbox_class)) { lv_checkbox_set_text(o, s); }  // checkbox label
  else { lv_label_set_text(o, s); }
  tc_lvgl_unlock();
}
void tc_lv_set_bg_color(int h, int rgb) {
  lv_obj_t *o = tc_lv_resolve(h);
  if (o) { tc_lvgl_lock();
    lv_obj_set_style_bg_color(o, lv_color_hex((uint32_t)rgb), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    tc_lvgl_unlock(); }
}
void tc_lv_set_text_color(int h, int rgb) {
  lv_obj_t *o = tc_lv_resolve(h);
  if (o) { tc_lvgl_lock();
    lv_obj_set_style_text_color(o, lv_color_hex((uint32_t)rgb), LV_PART_MAIN | LV_STATE_DEFAULT);
    tc_lvgl_unlock(); }
}

// events (poll). filter = lv_event_code_t (0 = LV_EVENT_ALL).
void tc_lv_event_enable(int h, int filter) {
  lv_obj_t *o = tc_lv_resolve(h);
  if (o) { tc_lvgl_lock(); lv_obj_add_event_cb(o, tc_lv_event_cb, (lv_event_code_t)filter, nullptr); tc_lvgl_unlock(); }
}
int tc_lv_event_next(void) {
  int got = 0;
  portENTER_CRITICAL(&tc_lv_evq_mux);
  if (tc_lv_evq_tail != tc_lv_evq_head) {
    tc_lv_ev_cur.handle = tc_lv_evq[tc_lv_evq_tail].handle;
    tc_lv_ev_cur.code   = tc_lv_evq[tc_lv_evq_tail].code;
    tc_lv_evq_tail = (tc_lv_evq_tail + 1) % TC_LV_EVQ;
    got = 1;
  }
  portEXIT_CRITICAL(&tc_lv_evq_mux);
  return got;
}
int tc_lv_event_obj(void)  { return tc_lv_ev_cur.handle; }
int tc_lv_event_code(void) { return tc_lv_ev_cur.code; }

// lifecycle (cannot delete the screen, h>=1)
int tc_lv_del(int h)   { lv_obj_t *o = tc_lv_resolve(h); if (o && h >= 1) { tc_lvgl_lock(); lv_obj_delete(o); tc_lvgl_unlock(); return 1; } return 0; }
int tc_lv_clean(int h) { lv_obj_t *o = tc_lv_resolve(h); if (o) { tc_lvgl_lock(); lv_obj_clean(o); tc_lvgl_unlock(); return 1; } return 0; }

/* ── Phase 2: value widgets (slider/bar/arc/switch/checkbox) + value/range/state + style ── */
int tc_lv_slider(int parent)   { tc_lvgl_lock(); int h = tc_lv_store(lv_slider_create(tc_lv_resolve(parent)));   tc_lvgl_unlock(); return h; }
int tc_lv_bar(int parent)      { tc_lvgl_lock(); int h = tc_lv_store(lv_bar_create(tc_lv_resolve(parent)));      tc_lvgl_unlock(); return h; }
int tc_lv_arc(int parent)      { tc_lvgl_lock(); int h = tc_lv_store(lv_arc_create(tc_lv_resolve(parent)));      tc_lvgl_unlock(); return h; }
int tc_lv_switch(int parent)   { tc_lvgl_lock(); int h = tc_lv_store(lv_switch_create(tc_lv_resolve(parent)));   tc_lvgl_unlock(); return h; }
int tc_lv_checkbox(int parent) { tc_lvgl_lock(); int h = tc_lv_store(lv_checkbox_create(tc_lv_resolve(parent))); tc_lvgl_unlock(); return h; }

// value: dispatch by widget type (slider/bar take an anim flag; arc does not)
void tc_lv_set_value(int h, int v, int anim) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  lv_anim_enable_t a = anim ? LV_ANIM_ON : LV_ANIM_OFF;
  tc_lvgl_lock();
  if      (lv_obj_check_type(o, &lv_slider_class)) { lv_slider_set_value(o, v, a); }
  else if (lv_obj_check_type(o, &lv_bar_class))    { lv_bar_set_value(o, v, a); }
  else if (lv_obj_check_type(o, &lv_arc_class))    { lv_arc_set_value(o, v); }
  tc_lvgl_unlock();
}
int tc_lv_get_value(int h) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return 0; }
  int v = 0;
  tc_lvgl_lock();
  if      (lv_obj_check_type(o, &lv_slider_class)) { v = (int)lv_slider_get_value(o); }
  else if (lv_obj_check_type(o, &lv_bar_class))    { v = (int)lv_bar_get_value(o); }
  else if (lv_obj_check_type(o, &lv_arc_class))    { v = (int)lv_arc_get_value(o); }
  tc_lvgl_unlock();
  return v;
}
void tc_lv_set_range(int h, int mn, int mx) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  if      (lv_obj_check_type(o, &lv_slider_class)) { lv_slider_set_range(o, mn, mx); }
  else if (lv_obj_check_type(o, &lv_bar_class))    { lv_bar_set_range(o, mn, mx); }
  else if (lv_obj_check_type(o, &lv_arc_class))    { lv_arc_set_range(o, mn, mx); }
  tc_lvgl_unlock();
}
// checked state — switch, checkbox, or any object's LV_STATE_CHECKED
void tc_lv_set_checked(int h, int on) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  if (on) { lv_obj_add_state(o, LV_STATE_CHECKED); } else { lv_obj_remove_state(o, LV_STATE_CHECKED); }
  tc_lvgl_unlock();
}
int tc_lv_is_checked(int h) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return 0; }
  tc_lvgl_lock(); int r = lv_obj_has_state(o, LV_STATE_CHECKED) ? 1 : 0; tc_lvgl_unlock();
  return r;
}
// generic integer local style on LV_PART_MAIN|LV_STATE_DEFAULT (prop = lv_style_prop_t:
// 120=RADIUS, 56=BORDER_WIDTH, 112=OPA, etc.)
void tc_lv_set_style_int(int h, int prop, int val) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  lv_style_value_t sv; sv.num = val;
  tc_lvgl_lock();
  lv_obj_set_local_style_prop(o, (lv_style_prop_t)prop, sv, LV_PART_MAIN | LV_STATE_DEFAULT);
  tc_lvgl_unlock();
}

/* ── Phase 3: chart (+series) + image-from-FS ─────────────────────────────────
   Chart series are lv_chart_series_t* (not lv_obj_t*), so they get their own small
   handle table. NOTE: series handles are not auto-cleared when the chart is deleted
   (series are freed with the chart) — fine for typical "chart lives for the program"
   use; don't delete a chart and keep reusing old series handles. */
#define TC_LV_SER_MAX 16
static lv_chart_series_t *tc_lv_ser_tab[TC_LV_SER_MAX];

int tc_lv_chart(int parent) { tc_lvgl_lock(); int h = tc_lv_store(lv_chart_create(tc_lv_resolve(parent))); tc_lvgl_unlock(); return h; }
void tc_lv_chart_type(int h, int type) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock(); lv_chart_set_type(o, (lv_chart_type_t)type); tc_lvgl_unlock();
}
int tc_lv_chart_series(int h, int rgb) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return 0; }
  int sh = 0;
  tc_lvgl_lock();
  lv_chart_series_t *s = lv_chart_add_series(o, lv_color_hex((uint32_t)rgb), LV_CHART_AXIS_PRIMARY_Y);
  if (s) { for (int i = 0; i < TC_LV_SER_MAX; i++) { if (!tc_lv_ser_tab[i]) { tc_lv_ser_tab[i] = s; sh = i + 1; break; } } }
  tc_lvgl_unlock();
  return sh;
}
void tc_lv_chart_next(int h, int ser, int v) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  if (ser < 1 || ser > TC_LV_SER_MAX) { return; }
  lv_chart_series_t *s = tc_lv_ser_tab[ser - 1]; if (!s) { return; }
  tc_lvgl_lock(); lv_chart_set_next_value(o, s, v); tc_lvgl_unlock();
}
void tc_lv_chart_range(int h, int axis, int mn, int mx) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock(); lv_chart_set_axis_range(o, (lv_chart_axis_t)axis, mn, mx); tc_lvgl_unlock();
}
void tc_lv_chart_count(int h, int n) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock(); lv_chart_set_point_count(o, (uint32_t)(n < 1 ? 1 : n)); tc_lvgl_unlock();
}
int tc_lv_image(int parent) { tc_lvgl_lock(); int h = tc_lv_store(lv_image_create(tc_lv_resolve(parent))); tc_lvgl_unlock(); return h; }
void tc_lv_image_src(int h, const char *path) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || !path) { return; }
  tc_lvgl_lock(); lv_image_set_src(o, path); tc_lvgl_unlock();
}
// rotate an image around its pivot. angle in 0.1° units (3600 = 360°), wrapped to 0..3599.
void tc_lv_image_angle(int h, int angle) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  angle %= 3600; if (angle < 0) { angle += 3600; }
  tc_lvgl_lock(); lv_image_set_rotation(o, angle); tc_lvgl_unlock();
}
// set the rotation pivot (px, relative to the image's top-left). Set this so a hand
// rotates around the watch centre rather than its own middle.
void tc_lv_image_pivot(int h, int x, int y) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock(); lv_image_set_pivot(o, x, y); tc_lvgl_unlock();
}

// screen management — a screen is an object with no parent (lv_obj_create(NULL))
int tc_lv_screen_create(void) { tc_lvgl_lock(); int h = tc_lv_store(lv_obj_create(NULL)); tc_lvgl_unlock(); return h; }
void tc_lv_screen_load(int h) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || h < 1) { return; }
  tc_lvgl_lock(); lv_screen_load(o); tc_lvgl_unlock();
}
void tc_lv_screen_load_anim(int h, int anim, int ms) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || h < 1) { return; }
  tc_lvgl_lock();
  lv_screen_load_anim(o, (lv_screen_load_anim_t)anim, (uint32_t)(ms < 0 ? 0 : ms), 0, false);
  tc_lvgl_unlock();
}
#endif // USE_TINYC_LVGL

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv54(uint32_t function) {
  bool result = false;

  switch (function) {
    case FUNC_LOOP:
      if (lvgl_glue) {
        if (TasmotaGlobal.sleep > USE_LVGL_MAX_SLEEP) {
          TasmotaGlobal.sleep = USE_LVGL_MAX_SLEEP;   // sleep is max 10ms
        }
#ifdef USE_TINYC_LVGL
        tc_lvgl_lock();           // serialise vs the lvgl* syscalls on the VM task
        lv_task_handler();
        tc_lvgl_unlock();
#else
        lv_task_handler();
#endif
      }
      break;
    case FUNC_ACTIVE:
      result = true;
      break;

  }
  return result;
}

#endif  // defined(USE_LVGL) && defined(USE_UNIVERSAL_DISPLAY)
#endif  // ESP32
