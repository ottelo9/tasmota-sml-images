/*
 * xdrv_54_lvgl_tinyc.inc - FORK-OWNED TinyC<->LVGL glue (gemu fork).
 *
 * #included into xdrv_54_lvgl.ino inside `#ifdef USE_TINYC_LVGL` so it shares that
 * translation unit (the static handle table, tc_lvgl_lock(), the LVGL display
 * context). Kept OUT of the upstream xdrv_54_lvgl.ino to minimise fork divergence:
 * ALL TinyC LVGL bindings (the tc_lv_* backends for the lvgl* builtins, dispatched
 * from SYS_LVGL_* in xdrv_124_tinyc_vm.h) belong here, not in the upstream file.
 */
static SemaphoreHandle_t tc_lvgl_mtx = nullptr;

void tc_lvgl_lock(void) { if (tc_lvgl_mtx) { xSemaphoreTakeRecursive(tc_lvgl_mtx, portMAX_DELAY); } }
void tc_lvgl_unlock(void) { if (tc_lvgl_mtx) { xSemaphoreGiveRecursive(tc_lvgl_mtx); } }

int tc_lvgl_active(void) { return lvgl_started() ? 1 : 0; }

// ── /tc_display LVGL screen mirror ──────────────────────────────────────────
// LVGL renders in PARTIAL mode (small flush buffers) — there is no full-screen
// framebuffer to read, and on HW-DMA panels renderer->framebuffer is null. So when
// LVGL is driving the screen the mirror snapshots the active screen to a fresh
// full-screen RGB565 buffer (lv_snapshot, LV_USE_SNAPSHOT). On-demand only: the
// caller streams *data (sw*sh*2 bytes) then MUST call tc_lvgl_snapshot_free(handle).
// The buffer is allocated via LVGL's allocator (PSRAM on PSRAM boards).
int tc_lvgl_snapshot(uint8_t **data, uint16_t *w, uint16_t *h, void **handle) {
  if (!tc_lvgl_active() || !data || !w || !h || !handle) { return 0; }
  tc_lvgl_lock();
  lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  tc_lvgl_unlock();
  if (!snap || !snap->data) {
    if (snap) { tc_lvgl_lock(); lv_draw_buf_destroy(snap); tc_lvgl_unlock(); }
    return 0;
  }
  *data   = (uint8_t *)snap->data;
  *w      = (uint16_t)snap->header.w;
  *h      = (uint16_t)snap->header.h;
  *handle = (void *)snap;
  return 1;
}
void tc_lvgl_snapshot_free(void *handle) {
  if (!handle) { return; }
  tc_lvgl_lock();
  lv_draw_buf_destroy((lv_draw_buf_t *)handle);
  tc_lvgl_unlock();
}

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
#define TC_LV_MAX   256   // was 128; a 1024x600 dashboard (moritz_dash = 9 thermostats + 9 windows + alarm) already sits at ~125 objects, leaving no room. 256 pointers = ~1KB, trivial.
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
  if      (lv_obj_check_type(o, &lv_checkbox_class)) { lv_checkbox_set_text(o, s); }  // checkbox label
  else if (lv_obj_check_type(o, &lv_label_class))    { lv_label_set_text(o, s); }
  // else: not a text widget — e.g. handle 0 (= screen) returned after a TC_LV_MAX overflow.
  // No-op rather than lv_label_set_text() on a non-label, which corrupts it -> hard reset.
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
// set a label's text font by pixel size. Montserrat (proportional, Tasmota subset
// 10/14/20/28) for normal UI sizes; unscii (crisp 8/16 px pixel/monospace, already
// compiled via LV_FONT_UNSCII_8/16=1 in tasmota_lv_conf.h) for the small end where
// Montserrat goes fuzzy — ideal for dense numeric axis labels on small displays.
// The style lands on LV_PART_MAIN, which labels use for their text; on a button,
// pass its child label handle. Existing apps only use 14/20/28 → unchanged.
void tc_lv_set_font(int h, int size) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  const lv_font_t *f;
  if (size >= 28)      { f = &lv_font_montserrat_tasmota_28; }
  else if (size >= 20) { f = &lv_font_montserrat_tasmota_20; }
  else if (size >= 16) { f = &lv_font_unscii_16; }            // 16-19: crisp 16 px mono
  else if (size >= 14) { f = &lv_font_montserrat_tasmota_14; }
  else if (size >= 10) { f = &lv_font_montserrat_tasmota_10; }
  else                 { f = &lv_font_unscii_8; }             // <10: crisp 8 px mono (small displays)
  tc_lvgl_lock();
  lv_obj_set_style_text_font(o, f, LV_PART_MAIN | LV_STATE_DEFAULT);
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
// ⭐ Schiebe- oder Umlaufmodus. Nicht Kosmetik, sondern der Unterschied zwischen
// "geht" und "geht nicht", sobald die Kurve schnell laeuft:
//
//   SHIFT    (LVGL-Vorgabe) -- alle Punkte ruecken bei JEDEM neuen Wert nach links,
//            also wird die GANZE Diagrammflaeche ungueltig. Auf einem 800x1280-Panel
//            sind das bei einer 760x300-Flaeche 228.000 Pixel je Wert.
//   CIRCULAR -- der neue Wert ueberschreibt den aeltesten an Ort und Stelle, ein
//            wandernder Schreibstrich wie auf einem Krankenhausmonitor. Ungueltig wird
//            nur eine schmale Spalte, bei 250 Punkten auf 760 px rund 900 Pixel.
//
// Das ist Faktor ~250. Fuer eine EKG-Kurve mit 250 Hz ist SHIFT aussichtslos und
// CIRCULAR unproblematisch.
void tc_lv_chart_update_mode(int h, int mode) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  lv_chart_set_update_mode(o, mode ? LV_CHART_UPDATE_MODE_CIRCULAR : LV_CHART_UPDATE_MODE_SHIFT);
  tc_lvgl_unlock();
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
// A live RGB565 frame is rendered through a real lv_image (an lv_image_dsc_t wrapping the
// PSRAM buffer), NOT an lv_canvas: lv_image_set_scale/rotation do NOT render on a canvas in
// this LVGL build (canvas shows raw at 1:1 but blanks under any transform), whereas a plain
// lv_image supports scale + rotation fully.
int tc_lv_canvas(int parent) { tc_lvgl_lock(); int h = tc_lv_store(lv_image_create(tc_lv_resolve(parent))); tc_lvgl_unlock(); return h; }
// Point the image at a PSRAM RGB565 buffer (zero-copy) and redraw. The buffer (an image slot
// from dspLoadImageFromCam) must stay valid until this is called again with a new buffer; the
// caller frees the previous slot with dspFreeImage. One static dsc => one live image-from-
// buffer at a time, which is all a camera view needs.
static lv_image_dsc_t tc_cam_dsc;
void tc_lv_canvas_set_rgb565(int h, uint16_t *buf, int w, int hgt) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || !buf || w <= 0 || hgt <= 0) { return; }
  tc_lvgl_lock();
  tc_cam_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
  tc_cam_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
  tc_cam_dsc.header.flags  = 0;
  tc_cam_dsc.header.w      = (uint32_t)w;
  tc_cam_dsc.header.h      = (uint32_t)hgt;
  tc_cam_dsc.header.stride = (uint32_t)w * 2;
  tc_cam_dsc.data_size     = (uint32_t)w * (uint32_t)hgt * 2;
  tc_cam_dsc.data          = (const uint8_t *)buf;
  lv_image_cache_drop(&tc_cam_dsc);          // drop any cached decode so the new pixels render
  lv_image_set_src(o, &tc_cam_dsc);
  lv_obj_invalidate(o);
  tc_lvgl_unlock();
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
// per-axis image zoom. sx/sy in 1/256 units (256 = 100%, 512 = 2x, 128 = 50%). Drives
// beat-pulsing covers (sx=sy) and length-modulated bar images (one axis) from TinyC.
void tc_lv_image_scale(int h, int sx, int sy) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  if (sx < 1) { sx = 1; } if (sy < 1) { sy = 1; }
  tc_lvgl_lock();
  lv_image_set_scale_x(o, (uint32_t)sx);
  lv_image_set_scale_y(o, (uint32_t)sy);
  tc_lvgl_unlock();
}

// ── lv_line: dynamic per-line points (malloc'd, freed on object delete) ──────────
// lv_line keeps a POINTER to its points (no copy), so each line owns a heap buffer
// stashed in user_data; an LV_EVENT_DELETE cb frees it. No fixed cap, no leak across
// runs — lvglClean()/lvglDelete() delete the objects, which frees their buffers.
// A "bar" is just a thick rounded 2-point line; lvglLinePoly() grows the buffer to N
// points so a whole polyline (series / gridline / arc) is ONE object.
static void tc_lv_line_del_cb(lv_event_t *e) {
  lv_obj_t *o = (lv_obj_t *)lv_event_get_target(e);
  void *p = lv_obj_get_user_data(o);
  if (p) { free(p); lv_obj_set_user_data(o, nullptr); }
}
int tc_lv_line(int parent) {
  tc_lvgl_lock();
  lv_obj_t *o = lv_line_create(tc_lv_resolve(parent));
  if (o) {
    lv_point_precise_t *pts = (lv_point_precise_t *)calloc(2, sizeof(lv_point_precise_t));
    lv_obj_set_user_data(o, pts);
    if (pts) { lv_line_set_points(o, pts, 2); }
    lv_obj_set_pos(o, 0, 0);                          // points are absolute screen px
    lv_obj_set_style_line_rounded(o, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(o, tc_lv_line_del_cb, LV_EVENT_DELETE, nullptr);
  }
  int h = tc_lv_store(o);
  tc_lvgl_unlock();
  return h;
}
void tc_lv_line_points(int h, int x1, int y1, int x2, int y2) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  lv_point_precise_t *pts = (lv_point_precise_t *)lv_obj_get_user_data(o);
  if (!pts) { pts = (lv_point_precise_t *)calloc(2, sizeof(lv_point_precise_t)); lv_obj_set_user_data(o, pts); }
  if (pts) {
    pts[0].x = x1; pts[0].y = y1; pts[1].x = x2; pts[1].y = y2;
    lv_line_set_points(o, pts, 2);                    // re-set -> invalidates -> repaints
  }
  tc_lvgl_unlock();
}
// one lv_line draws a whole N-point polyline (series / gridline / gauge arc). Grows
// the line's points buffer to N via realloc; xs/ys are TinyC int32 arrays (absolute px).
// Params are int32_t* (not int*) to match tc_resolve_ref's return on toolchains where
// int32_t == long int (e.g. this S3 core) — otherwise the call is a hard -fpermissive error.
void tc_lv_line_poly(int h, int32_t *xs, int32_t *ys, int n) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o || !xs || !ys || n < 2) { return; }
  tc_lvgl_lock();
  lv_point_precise_t *pts = (lv_point_precise_t *)realloc(lv_obj_get_user_data(o), (size_t)n * sizeof(lv_point_precise_t));
  if (pts) {
    lv_obj_set_user_data(o, pts);
    for (int i = 0; i < n; i++) { pts[i].x = xs[i]; pts[i].y = ys[i]; }
    lv_line_set_points(o, pts, n);
  }
  tc_lvgl_unlock();
}
void tc_lv_line_style(int h, int rgb, int width) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  lv_obj_set_style_line_color(o, lv_color_hex((uint32_t)rgb), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_line_width(o, width, LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_line_rounded(o, true, LV_PART_MAIN | LV_STATE_DEFAULT);
  tc_lvgl_unlock();
}

// ── arc helpers (gauges) + rotate-any-object (vertical y-axis titles) ────────────
// LVGL angle convention: 0°=right (3 o'clock), 90°=bottom, 180°=left, 270°=top.
// 270° dial = bg_angles(135,45); top semicircle = bg_angles(180,360).
void tc_lv_arc_bg_angles(int h, int start, int end) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  if (lv_obj_check_type(o, &lv_arc_class)) { lv_arc_set_bg_angles(o, start, end); }
  tc_lvgl_unlock();
}
// colour + thickness of an arc PART: 0=MAIN (background track), 1=INDICATOR (value), 2=KNOB.
// This is the missing piece for zoned gauges + coloured value arcs (lvglSetBgColor/StyleInt
// only reach MAIN). width <= 0 leaves the part's thickness unchanged.
void tc_lv_arc_style(int h, int part, int color, int width) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  lv_part_t pt = (part == 1) ? LV_PART_INDICATOR : (part == 2) ? LV_PART_KNOB : LV_PART_MAIN;
  tc_lvgl_lock();
  lv_obj_set_style_arc_color(o, lv_color_hex((uint32_t)color), pt | LV_STATE_DEFAULT);
  if (width > 0) { lv_obj_set_style_arc_width(o, width, pt | LV_STATE_DEFAULT); }
  tc_lvgl_unlock();
}
// rotate any object about its centre via LVGL transform — for a vertical y-axis title.
// deci_deg in 0.1° units (900 = 90°, 2700 = 270°).
void tc_lv_rotate(int h, int deci_deg) {
  lv_obj_t *o = tc_lv_resolve(h); if (!o) { return; }
  tc_lvgl_lock();
  lv_obj_set_style_transform_pivot_x(o, lv_pct(50), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_transform_pivot_y(o, lv_pct(50), LV_PART_MAIN | LV_STATE_DEFAULT);
  lv_obj_set_style_transform_rotation(o, deci_deg, LV_PART_MAIN | LV_STATE_DEFAULT);
  tc_lvgl_unlock();
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
