/*
  xdrv_125_partedit.ino — in-place partition-scheme converter (old dual-OTA → safeboot)

  Tiny, self-contained driver meant to live INSIDE a SAFEBOOT image so a device that
  can only be reached over the network can convert its OWN flash layout from the old
  dual-OTA scheme (app0 + app1, no factory) to the standard safeboot scheme
  (safeboot/factory + app0 + a big spiffs) — with no serial access — and then stay
  resident in the factory partition for any future partition surgery.

  WHY a separate driver and not chkpt (xdrv_123): safeboot strips USE_BINPLUGINS (and
  USE_UFILESYS), and this must be lean enough to ride along in the ~776 KB safeboot
  image (factory slot is 0xD0000 = 832 KB). It uses only esp_partition_* / esp_flash_*
  — no filesystem.

  THE FLOW (remote device, running its OLD firmware in app0):
    1. OTA this safeboot+partedit image into the INACTIVE slot (app1) via normal /u2,
       boot it. Now running from app1; app0 (0x10000) is idle.
    2. `PartEdit dry`     — preview the table the convert would write (no flash touched).
    3. `PartEdit go DOIT` — the whole conversion in one command: self-copy the running
                          image to 0x10000 if we booted into app1 (else it's already the
                          factory spot), rewrite the table at 0x8000 (read-back verified),
                          erase otadata (→ boot factory), restart.
       OR `PartEdit auto`  — arm that same conversion behind a 20 s abort window (logged
                          countdown; `PartEdit cancel` aborts) — the hands-off path for a
                          remote device: OTA the image, send `auto`, walk away.
       (`PartEdit copy` self-copies standalone if you prefer to stage it separately.)
    4. Boots safeboot/partedit from the factory (0x10000) on the new scheme → console →
       OTA the real firmware into app0 (standard safeboot OTA). Done.

  The ONLY unrecoverable failure is a power cut during the convert's single-sector
  0x8000 erase+write. Everything else lands in a console-capable safeboot. Test on a
  serial-recoverable device first.

  SAFETY: refuses unless the current table is a SIMPLE dual-OTA layout
  (nvs,otadata,app0@0x10000,app1,spiffs, contiguous, filling flash — optionally
  with ONE trailing partition after spiffs, e.g. a BinPlugins "custom", which is
  preserved untouched while the new spiffs is shrunk to stop at it) AND we have a
  standard safeboot target for this flash size (PE_TARGETS: 4 MB + 16 MB so far).
  Anything else (odd offsets, non-contiguous, unknown flash size) is rejected,
  not guessed. Add a flash size by adding one PE_TARGETS row (its stock Tasmota
  safeboot CSV: safeboot 0x10000/0xD0000, app0 0xE0000/<app>, spiffs <off>/<rest>).

  Copyright (C) 2026  Gerhard Mutz / claude  —  GPL v3 (Tasmota's license)
*/

#ifdef ESP32
#ifdef USE_PARTEDIT

#define XDRV_125  125

#include "esp_flash_partitions.h"   // esp_partition_info_t, ESP_PARTITION_MAGIC[_MD5], PART_TYPE_*, PART_SUBTYPE_*, esp_partition_table_verify
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_ota_ops.h"
#include "MD5Builder.h"

// ── FIXED for every flash size: the factory + app0 always start here ──
#define PE_SB_OFF       0x010000   // safeboot/factory partition offset (was old app0)
#define PE_SB_SIZE      0x0D0000   // 832 KB factory (holds the ~756 KB safeboot image)
#define PE_APP0_OFF     0x0E0000   // = PE_SB_OFF + PE_SB_SIZE
#define PE_TABLE_OFFSET 0x8000
#define PE_OTADATA_OFF  0x00e000
#define PE_OTADATA_SIZE 0x002000

// ── per-flash-size STANDARD safeboot target (matches Tasmota's stock CSVs — the
//    app0/spiffs split is NOT a clean formula, it's Tasmota's chosen layout, so a
//    small lookup keeps converted devices byte-aligned with the official scheme).
//    safeboot is always PE_SB_OFF/PE_SB_SIZE; app0(ota_0) always at PE_APP0_OFF. ──
typedef struct { uint32_t flash, app0_size, spiffs_off, spiffs_size; } pe_target_t;
static const pe_target_t PE_TARGETS[] = {
  { 0x0400000, 0x1D0000, 0x2B0000, 0x150000 },  //  4 MB → app1856k_fs1344k  (1.875M app / 1.34M fs)
  { 0x1000000, 0x3D0000, 0x4B0000, 0xB50000 },  // 16 MB → app3904k_fs11584k (3.9M app / 11.58M fs)
};
static const pe_target_t *PartEdit_Target(uint32_t flash) {
  for (unsigned i = 0; i < sizeof(PE_TARGETS) / sizeof(PE_TARGETS[0]); i++)
    if (PE_TARGETS[i].flash == flash) return &PE_TARGETS[i];
  return NULL;
}

static bool pe_copied = false;   // set once `PartEdit copy` verified safeboot @ 0x10000
static int  pe_auto   = 0;       // >0 = auto-convert countdown (s); armed by `PartEdit auto`, aborted by `PartEdit cancel`
#define PE_AUTO_SECS  20         // abort window before an armed auto-convert fires
// 4 KB sector buffers kept in BSS, not on the (small) command-task stack.
static uint8_t pe_buf[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4)));   // the table sector
static uint8_t pe_vrf[SPI_FLASH_SEC_SIZE] __attribute__((aligned(4)));   // read-back verify

// ---- read + verify the on-flash partition table into caller's 4 KB buffer -------------
// Returns entry count, or -1 on read/verify error.
int PartEdit_ReadTable(uint8_t *buf) {
  if (esp_flash_read(NULL, buf, PE_TABLE_OFFSET, SPI_FLASH_SEC_SIZE) != ESP_OK) return -1;
  int num = 0;
  if (esp_partition_table_verify((const esp_partition_info_t *)buf, false, &num) != ESP_OK) return -1;
  return num;
}

// Find an entry by label; returns index or -1.
// NB: these 4 helpers take the table as `void*` (cast to esp_partition_info_t*
// inside) ON PURPOSE. With esp_partition_info_t in the signature, the Arduino
// .ino auto-prototype generator emits a prototype into tasmota.ino BEFORE
// esp_flash_partitions.h is included (gated behind safeboot-only USE_PARTEDIT)
// → "esp_partition_info_t not declared" on every non-safeboot build. void*
// keeps the type out of the generated prototype.
int PartEdit_Find(void *pe_v, int num, const char *label) {
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_v;
  for (int i = 0; i < num; i++) { if (!strcmp((char *)pe[i].label, label)) return i; }
  return -1;
}

// Find an entry whose region starts exactly at `off`; returns index or -1.
// Used to spot a partition sitting directly after spiffs (e.g. a trailing
// BinPlugins "custom" partition) so the conversion can preserve it.
int PartEdit_FindByOffset(void *pe_v, int num, uint32_t off) {
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_v;
  for (int i = 0; i < num; i++) { if (pe[i].pos.offset == off) return i; }
  return -1;
}

// Is the current table a recognized SIMPLE dual-OTA layout for a flash size we have
// a safeboot target for? Strict: nvs,otadata,app0@0x10000, app1 directly after it
// (same size), spiffs after that, and the old app slot is >= the safeboot size.
// The 5-entry form has spiffs filling to the end of flash. A 6-entry form is also
// accepted iff the 6th entry sits directly after spiffs and the two together fill
// flash (e.g. a trailing BinPlugins "custom" partition) — it is PRESERVED and the
// new spiffs is shrunk to stop at it. Anything else (odd offsets, non-contiguous,
// an unknown flash size) is rejected, not guessed.
bool PartEdit_IsConvertibleOld(void *pe_v, int num) {
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_v;
  if (num != 5 && num != 6) return false;
  if (PartEdit_Find(pe, num, "safeboot") >= 0) return false;   // already converted
  int a0 = PartEdit_Find(pe, num, "app0");
  int a1 = PartEdit_Find(pe, num, "app1");
  int sp = PartEdit_Find(pe, num, "spiffs");
  if (a0 < 0 || a1 < 0 || sp < 0) return false;
  uint32_t flash = ESP.getFlashChipSize();
  const pe_target_t *t = PartEdit_Target(flash);
  if (!t) return false;                                        // no safeboot target for this flash
  uint32_t S = pe[a0].pos.size;
  bool base = pe[a0].type == PART_TYPE_APP && pe[a0].pos.offset == PE_SB_OFF &&
              pe[a1].type == PART_TYPE_APP && pe[a1].pos.offset == PE_SB_OFF + S && pe[a1].pos.size == S &&
              pe[sp].pos.offset == PE_SB_OFF + 2 * S &&
              S >= PE_SB_SIZE;                                  // safeboot fits the old app slot
  if (!base) return false;
  uint32_t sp_end = pe[sp].pos.offset + pe[sp].pos.size;
  if (num == 5) {
    return (uint64_t)sp_end == flash;                          // spiffs fills to end of flash
  }
  // num == 6: a trailing partition (e.g. a BinPlugins "custom") sits directly after
  // spiffs and the two fill flash. We KEEP it (the new spiffs is shrunk to stop at
  // it) — so it must leave room for a non-empty spiffs in the converted layout.
  int cu = PartEdit_FindByOffset(pe, num, sp_end);
  if (cu < 0) return false;                                    // 6th entry not contiguous → reject
  return (uint64_t)pe[cu].pos.offset + pe[cu].pos.size == flash &&  // custom fills to flash end
         pe[cu].pos.offset > t->spiffs_off;                         // new spiffs has positive size
}

void PartEdit_DumpTable(const char *tag, void *pe_v, int num) {
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_v;
  AddLog(LOG_LEVEL_INFO, PSTR("PE: %s — %d entries:"), tag, num);
  for (int i = 0; i < num; i++) {
    AddLog(LOG_LEVEL_INFO, PSTR("PE:   %-10s t=%02x st=%02x  @0x%06x  0x%06x (%uK)"),
           pe[i].label, pe[i].type, pe[i].subtype, pe[i].pos.offset, pe[i].pos.size,
           pe[i].pos.size / 1024);
  }
}

// Rewrite buf in place: app0→safeboot(factory), app1→app0(ota_0), move spiffs. Refresh md5.
// Caller must have passed PartEdit_IsConvertibleOld (so the target exists).
void PartEdit_BuildNew(void *pe_v, int num) {
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_v;
  const pe_target_t *t = PartEdit_Target(ESP.getFlashChipSize());
  if (!t) return;
  int a0 = PartEdit_Find(pe, num, "app0");
  int a1 = PartEdit_Find(pe, num, "app1");
  int sp = PartEdit_Find(pe, num, "spiffs");
  // old app0 -> safeboot / factory (same 0x10000 offset, shrunk to 0xD0000)
  memset(pe[a0].label, 0, sizeof(pe[a0].label));
  strcpy((char *)pe[a0].label, "safeboot");
  pe[a0].subtype  = PART_SUBTYPE_FACTORY;
  pe[a0].pos.size = PE_SB_SIZE;
  // old app1 -> app0 / ota_0 (moved down to 0xE0000, standard app size for this flash)
  memset(pe[a1].label, 0, sizeof(pe[a1].label));
  strcpy((char *)pe[a1].label, "app0");
  pe[a1].subtype    = PART_SUBTYPE_OTA_FLAG;     // 0x10 = ota_0
  pe[a1].pos.offset = PE_APP0_OFF;
  pe[a1].pos.size   = t->app0_size;
  // spiffs grows + moves (type/subtype/label kept). If a trailing partition (e.g. a
  // BinPlugins "custom") sits right after the OLD spiffs, preserve it untouched and
  // shrink the new spiffs to stop exactly at it; otherwise spiffs fills to the end
  // of flash per the target.
  uint32_t old_sp_end = pe[sp].pos.offset + pe[sp].pos.size;
  int cu = PartEdit_FindByOffset(pe, num, old_sp_end);   // -1 → no trailing partition (5-entry)
  pe[sp].pos.offset = t->spiffs_off;
  pe[sp].pos.size   = (cu >= 0) ? (pe[cu].pos.offset - t->spiffs_off) : t->spiffs_size;
  // (the trailing custom entry, if any, is left exactly as-is → preserved)
  // refresh the ESP_PARTITION_MAGIC_MD5 trailer (esp_partition_table_verify checks it)
  MD5Builder md5; md5.begin();
  md5.add((uint8_t *)pe, num * sizeof(esp_partition_info_t));
  md5.calculate();
  uint8_t digest[16]; md5.getBytes(digest);
  uint8_t *end = (uint8_t *)pe + num * sizeof(esp_partition_info_t);
  end[0] = 0xeb; end[1] = 0xeb;
  memmove(end + 16, digest, 16);
}

// Self-copy the running (app1) image into the idle app0 partition (→ future factory).
bool PartEdit_CopyToFactory(void) {
  const esp_partition_t *run = esp_ota_get_running_partition();
  const esp_partition_t *app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  (esp_partition_subtype_t)(PART_SUBTYPE_OTA_FLAG), "app0");
  const esp_partition_t *app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
                                  (esp_partition_subtype_t)(PART_SUBTYPE_OTA_FLAG | 1), "app1");
  if (!run || !app0 || !app1) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: copy — run/app0/app1 not found")); return false; }
  if (run->address != app1->address) {   // must run from app1 so app0 (=0x10000) is idle
    AddLog(LOG_LEVEL_ERROR, PSTR("PE: copy REFUSED — must run from app1 (0x%06x), running @0x%06x"),
           (unsigned)app1->address, (unsigned)run->address);
    return false;
  }
  uint8_t *sec = (uint8_t *)malloc(SPI_FLASH_SEC_SIZE);
  if (!sec) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: copy OOM")); return false; }
  AddLog(LOG_LEVEL_INFO, PSTR("PE: copying running image (app1) -> 0x%06x (%uK)…"),
         (unsigned)app0->address, PE_SB_SIZE / 1024);
  bool ok = true;
  for (uint32_t off = 0; off < PE_SB_SIZE; off += SPI_FLASH_SEC_SIZE) {
    if (esp_partition_read(run, off, sec, SPI_FLASH_SEC_SIZE) != ESP_OK) { ok = false; break; }
    if (esp_partition_erase_range(app0, off, SPI_FLASH_SEC_SIZE) != ESP_OK) { ok = false; break; }
    if (esp_partition_write(app0, off, sec, SPI_FLASH_SEC_SIZE) != ESP_OK) { ok = false; break; }
    if ((off & 0x1FFFF) == 0) delay(1);   // yield every 128 KB (safeboot WDT is off, IIWDT isn't)
  }
  if (ok) {
    uint8_t hdr[4]; esp_partition_read(app0, 0, hdr, 4);
    if (hdr[0] != 0xE9) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: copy — no app magic 0xE9 at 0x10000 (got 0x%02x)"), hdr[0]); ok = false; }
  }
  free(sec);
  if (ok) { pe_copied = true; AddLog(LOG_LEVEL_INFO, PSTR("PE: copy OK — safeboot present at 0x10000")); }
  return ok;
}

// Full conversion, shared by `PartEdit go DOIT` and the auto-convert countdown:
//   1. ensure the running safeboot image sits at 0x10000 (self-copy if we booted into
//      app1; if we booted at 0x10000 it's already the factory spot),
//   2. write the new table at 0x8000 with read-back verify (3 tries),
//   3. erase otadata (→ boot factory), restart.
// Returns false on any failure (and does NOT restart — recoverable, no boot-loop);
// on success it restarts and never returns.
bool PartEdit_Convert(void) {
  int num = PartEdit_ReadTable(pe_buf);
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_buf;
  if (num < 0 || !PartEdit_IsConvertibleOld(pe, num)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("PE: convert REFUSED — table is not a convertible dual-OTA"));
    return false;
  }
  const esp_partition_t *run = esp_ota_get_running_partition();
  if (run && run->address != PE_SB_OFF) {              // booted from app1 → self-copy to 0x10000
    if (!PartEdit_CopyToFactory()) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: convert — self-copy failed")); return false; }
  } else {                                             // booted at 0x10000 → must be a valid app there
    uint8_t hdr0 = 0; esp_flash_read(NULL, &hdr0, PE_SB_OFF, 1);
    if (hdr0 != 0xE9) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: convert — no app image (0xE9) at 0x10000")); return false; }
  }
  PartEdit_BuildNew(pe, num);
  AddLog(LOG_LEVEL_INFO, PSTR("PE: COMMITTING new table — do not power off"));
  bool ok = false;
  for (int attempt = 0; attempt < 3 && !ok; attempt++) {
    esp_flash_erase_region(NULL, PE_TABLE_OFFSET, SPI_FLASH_SEC_SIZE);
    esp_flash_write(NULL, pe_buf, PE_TABLE_OFFSET, SPI_FLASH_SEC_SIZE);
    esp_flash_read(NULL, pe_vrf, PE_TABLE_OFFSET, SPI_FLASH_SEC_SIZE);
    ok = (0 == memcmp(pe_buf, pe_vrf, SPI_FLASH_SEC_SIZE));
    AddLog(LOG_LEVEL_INFO, PSTR("PE: table write attempt %d verify=%d"), attempt + 1, ok);
  }
  if (!ok) { AddLog(LOG_LEVEL_ERROR, PSTR("PE: TABLE VERIFY FAILED — NOT restarting (serial recovery may be needed)")); return false; }
  esp_flash_erase_region(NULL, PE_OTADATA_OFF, PE_OTADATA_SIZE);   // last: → boot factory
  AddLog(LOG_LEVEL_INFO, PSTR("PE: converted to safeboot scheme — restarting into factory"));
  delay(200);
  ESP_Restart();
  return true;   // unreached
}

void CmndPartEdit(void) {
  char *cmd = XdrvMailbox.data;
  while (*cmd == ' ') cmd++;

  int num = PartEdit_ReadTable(pe_buf);
  if (num < 0) { ResponseCmndChar_P(PSTR("table read/verify failed")); return; }
  esp_partition_info_t *pe = (esp_partition_info_t *)pe_buf;

  if (!*cmd || !strncasecmp(cmd, "info", 4)) {
    const esp_partition_t *run = esp_ota_get_running_partition();
    AddLog(LOG_LEVEL_INFO, PSTR("PE: running @0x%06x sub=%02x  convertible=%d copied=%d"),
           run ? run->address : 0, run ? run->subtype : 0,
           PartEdit_IsConvertibleOld(pe, num), pe_copied);
    PartEdit_DumpTable("current", pe, num);
    ResponseCmndDone();
    return;
  }

  if (!strncasecmp(cmd, "cancel", 6)) {          // always works — abort a pending auto-convert
    if (pe_auto) { pe_auto = 0; AddLog(LOG_LEVEL_INFO, PSTR("PE: auto-convert CANCELLED")); }
    ResponseCmndChar_P(PSTR("auto cancelled"));
    return;
  }

  if (!PartEdit_IsConvertibleOld(pe, num)) {
    AddLog(LOG_LEVEL_ERROR, PSTR("PE: REFUSED — current table is not a convertible dual-OTA layout"));
    PartEdit_DumpTable("current", pe, num);
    ResponseCmndChar_P(PSTR("not convertible"));
    return;
  }

  if (!strncasecmp(cmd, "copy", 4)) {
    bool ok = PartEdit_CopyToFactory();
    ResponseCmndChar_P(ok ? PSTR("copied") : PSTR("copy failed"));
    return;
  }

  if (!strncasecmp(cmd, "dry", 3)) {
    PartEdit_BuildNew(pe, num);       // edit the local copy only — NOTHING written
    PartEdit_DumpTable("WOULD write", pe, num);
    ResponseCmndChar_P(PSTR("dry-run only"));
    return;
  }

  if (!strncasecmp(cmd, "auto", 4)) {            // arm the abort-windowed auto-convert
    pe_auto = PE_AUTO_SECS;
    AddLog(LOG_LEVEL_INFO, PSTR("PE: AUTO-CONVERT armed — fires in %d s; 'PartEdit cancel' to abort"), pe_auto);
    ResponseCmndChar_P(PSTR("auto-convert armed"));
    return;
  }

  if (!strncasecmp(cmd, "go", 2)) {              // immediate manual convert
    char *tok = cmd + 2; while (*tok == ' ') tok++;
    if (strncmp(tok, "DOIT", 4)) { ResponseCmndChar_P(PSTR("need: PartEdit go DOIT")); return; }
    ResponseCmndChar_P(PSTR("converting"));
    if (!PartEdit_Convert()) ResponseCmndChar_P(PSTR("convert failed — halted"));
    return;   // on success PartEdit_Convert() restarted
  }

  ResponseCmndChar_P(PSTR("PartEdit info|dry|copy|go DOIT|auto|cancel"));
}

const char kPartEditCommands[] PROGMEM = "|" "PartEdit";
void (* const PartEditCommand[])(void) PROGMEM = { &CmndPartEdit };

bool Xdrv125(uint32_t function) {
  bool result = false;
  switch (function) {
    case FUNC_EVERY_SECOND:
      if (pe_auto > 0) {
        pe_auto--;
        if (pe_auto == 0) {
          AddLog(LOG_LEVEL_INFO, PSTR("PE: AUTO-CONVERT firing now"));
          PartEdit_Convert();   // restarts on success; on failure logs + leaves pe_auto=0 (no re-fire/loop)
        } else if (pe_auto % 5 == 0 || pe_auto <= 3) {
          AddLog(LOG_LEVEL_INFO, PSTR("PE: auto-convert in %d s ('PartEdit cancel' to abort)"), pe_auto);
        }
      }
      break;
    case FUNC_COMMAND:
      result = DecodeCommand(kPartEditCommands, PartEditCommand);
      break;
  }
  return result;
}

#endif  // USE_PARTEDIT
#endif  // ESP32
