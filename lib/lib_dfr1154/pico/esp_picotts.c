/* Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache 2.0 license.
 *
 * @author J Mattsson <jmattsson@dius.com.au>
 */
#include "picotts.h"
#include "picoapi.h"
#include "picoapid.h"
#include "esp_picorsrc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <stdlib.h>
#include <stdbool.h>
#include <math.h>

// Yep, that's 1.1MB needed by PicoTTS, not counting the resource files which
// we access directly from flash. Overridable via -DPICO_MEM_SIZE=<bytes> so a
// PSRAM-tight board (e.g. 2 MB ESP32-S3) that loads the ~1.05 MB voice into
// PSRAM rather than mmap'ing it from flash can shrink the engine arena to fit
// voice + arena under its PSRAM ceiling. 1.1 MB is the comfortable default
// (8 MB-PSRAM boards keep it); lower values trade synthesis head-room for fit.
#ifndef PICO_MEM_SIZE
#define PICO_MEM_SIZE 1100000
#endif

#define PICOTASK_EXIT  0x0000001u

#define IDLE_WAIT_COUNT 5

// ─── Tasmota: defensive defaults for upstream Kconfig macros ─────────
// The upstream DiUS port relies on Kconfig (idf.py menuconfig) to set
// these. Under PlatformIO/Tasmota we expose them as build flags; if
// the user only defines USE_PICOTTS without the helper flags the
// library would fail to compile (CONFIG_PICOTTS_INPUT_QUEUE_SIZE) or
// link with garbage partition names. Defaults below match the values
// recommended in platformio_snippet.ini and let -DUSE_PICOTTS work
// standalone.
//
// Override in your env's build_flags to tune:
//   -DCONFIG_PICOTTS_INPUT_QUEUE_SIZE=N      (UTF-8 char queue depth)
//   '-DCONFIG_PICOTTS_TA_PARTITION="..."'    (only used if you wire
//   '-DCONFIG_PICOTTS_SG_PARTITION="..."'     up a real partition mode;
//                                              the runtime override
//                                              path doesn't read these)
#ifndef CONFIG_PICOTTS_INPUT_QUEUE_SIZE
// 256 chars matches the upstream Kconfig default — fine for short
// status announcements, may briefly block picotts_add() on long input.
// Bump to 1024 if you regularly synthesise full sentences and want
// I2STTS to return immediately.
#define CONFIG_PICOTTS_INPUT_QUEUE_SIZE 256
#endif
#ifndef CONFIG_PICOTTS_TA_PARTITION
// Placeholder — only referenced inside the partition-mode #else branch
// of the resource loader. The runtime-override path
// (picotts_set_resources) bypasses partition lookup entirely; if a
// partition with this name doesn't exist the lookup silently returns
// NULL and the override pointer wins.
#define CONFIG_PICOTTS_TA_PARTITION "picotts_ta"
#endif
#ifndef CONFIG_PICOTTS_SG_PARTITION
#define CONFIG_PICOTTS_SG_PARTITION "picotts_sg"
#endif

static picotts_output_fn outputCb;
static picotts_error_notify_fn errorCb;
static picotts_idle_notify_fn idleCb;

static SemaphoreHandle_t exitLock;
static QueueHandle_t textQ;
static TaskHandle_t picoTask;

static void *picoMemArea;

static pico_System   picoSystem;
static pico_Resource picoTaResource;
static pico_Resource picoSgResource;
static pico_Engine   picoEngine;

static const pico_Char voiceName[] = "PicoVoice";
static const char tag[] = "picotts";


// Runtime override (Tasmota addition). When set via
// picotts_set_resources() these win over EMBED/PARTITION modes — used
// for LittleFS-loaded voice data: the firmware stays small (~1.8 MB
// instead of 3.2 MB), and the voice file can be swapped without
// reflashing.
static const void *runtime_ta_ptr = NULL;
static const void *runtime_sg_ptr = NULL;

void picotts_set_resources(const void *ta_ptr, const void *sg_ptr)
{
  runtime_ta_ptr = ta_ptr;
  runtime_sg_ptr = sg_ptr;
}

#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
// Embedded resources (only used if no runtime override is set).
extern const char ta_bin_start[] asm("_binary_picotts_ta_bin_start");
extern const char sg_bin_start[] asm("_binary_picotts_sg_bin_start");

#else

static esp_partition_mmap_handle_t taMmap;
static esp_partition_mmap_handle_t sgMmap;

static const void *find_and_map_partition(
    const char *name, esp_partition_mmap_handle_t *handle)
{
  const esp_partition_t *part =
    esp_partition_find_first(
        ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, name);
  if (!part)
  {
    ESP_LOGE(tag, "Partition '%s' not found", name);
    return NULL;
  }
  else
  {
    const void *ptr = 0;
    esp_err_t ret = esp_partition_mmap(
      part, 0, part->size, ESP_PARTITION_MMAP_DATA, &ptr, handle);
    ESP_ERROR_CHECK_WITHOUT_ABORT(ret);
    ESP_LOGI(tag, "Partition '%s' mmap'd to %p", name, ptr);
    return (ret == ESP_OK) ? ptr : NULL;
  }
}

static void unmap_partitions(void)
{
  if (taMmap)
  {
    esp_partition_munmap(taMmap);
    taMmap = 0;
  }
  if (sgMmap)
  {
    esp_partition_munmap(sgMmap);
    sgMmap = 0;
  }
}
#endif


static const void *find_ta_bin_start()
{
  if (runtime_ta_ptr) return runtime_ta_ptr;
#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
  return ta_bin_start;
#else
  return find_and_map_partition(CONFIG_PICOTTS_TA_PARTITION, &taMmap);
#endif
}


static const void *find_sg_bin_start()
{
  if (runtime_sg_ptr) return runtime_sg_ptr;
#ifdef CONFIG_PICOTTS_RESOURCE_MODE_EMBED
  return sg_bin_start;
#else
  return find_and_map_partition(CONFIG_PICOTTS_SG_PARTITION, &sgMmap);
#endif
}


// Use regular exp() function as the picotty hack doesn't work on Xtensa
picoos_double picoos_quick_exp(const picoos_double y)
{
  return exp(y);
}


static void esp_pico_err_print(const char *what, int code)
{
  pico_Retstring msg;
  pico_getSystemStatusMessage(picoSystem, code, msg);
  ESP_LOGE(tag, "%s (%i): %s", what, code, msg);
}


static void esp_pico_run(void *)
{
  ESP_LOGI(tag, "Task started");
  bool error = false;
  enum {
    WAITING_FOR_BYTES, WAITING_FOR_OUTPUT
  } state = WAITING_FOR_BYTES;
  unsigned idles = 0;

  while(!error)
  {
    uint32_t flags;
    if (xTaskNotifyWait(0, ~0, &flags, 0) == pdPASS)
    {
      if (flags & PICOTASK_EXIT)
        break;
    }

    uint8_t c;
    while (xQueuePeek(textQ, &c, 0) == pdPASS)
    {
      int16_t processed = 0;
      int ret = pico_putTextUtf8(picoEngine, &c, 1, &processed);
      if (ret)
      {
        esp_pico_err_print("Put text failed, stopping TTS", ret);
        error = true;
        break;
      }
      else
      {
        if (processed)
        {
          xQueueReceive(textQ, &c, 0);
          if (state == WAITING_FOR_BYTES)
            state = WAITING_FOR_OUTPUT;
        }
      }
    }

    switch (state)
    {
      case WAITING_FOR_BYTES:
        if (idles < IDLE_WAIT_COUNT)
        {
          if (++idles == IDLE_WAIT_COUNT && idleCb)
            idleCb();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        break;
      case WAITING_FOR_OUTPUT:
      {
        int status;
        do {
          int16_t outbuf[128];
          int16_t bytes = 0, type = 0;
          // Note: Only PICO_DATA_PCM_16BIT is defined as output type, so we
          // don't propagate that information. Rather, it's a fixed property.
          status =
            pico_getData(picoEngine, outbuf, sizeof(outbuf), &bytes, &type);
          if (bytes > 0)
            outputCb(outbuf, bytes/2);
        } while (status == PICO_STEP_BUSY);
        if (status != PICO_STEP_IDLE)
        {
          esp_pico_err_print("Get data failed, stopping TTS", status);
          error = true;
        }
        else
        {
          state = WAITING_FOR_BYTES;
          idles = 0;
        }
        break;
       }
    }
  }
  ESP_LOGI(tag, "Exiting task");
  xSemaphoreGive(exitLock);
  vTaskDelete(NULL);

  if (error && errorCb)
    errorCb();
}


static void esp_pico_cleanup(void)
{
  if (picoTask)
  {
    xTaskNotify(picoTask, PICOTASK_EXIT, eSetBits);
    xSemaphoreTake(exitLock, portMAX_DELAY);
    picoTask = NULL;
  }

  if (picoEngine)
  {
    pico_disposeEngine(picoSystem, &picoEngine);
    pico_releaseVoiceDefinition(picoSystem, voiceName);
    picoEngine = NULL;
  }

  if (picoSgResource)
  {
    esp_pico_unloadResource(picoSystem, &picoSgResource);
    picoSgResource = NULL;
  }

  if (picoTaResource)
  {
    esp_pico_unloadResource(picoSystem, &picoTaResource);
    picoTaResource = NULL;
  }

  if (picoSystem)
  {
    pico_terminate(&picoSystem);
    picoSystem = NULL;
  }

  free(picoMemArea);
  picoMemArea = NULL;

  if (textQ)
  {
    vQueueDelete(textQ);
    textQ = NULL;
  }

  // Clear callback function pointers so a second picotts_init doesn't
  // see stale addresses. picotts_init resets outputCb itself but leaves
  // errorCb/idleCb untouched — the BinPlugin caller in xdrv_42_i2s.cpp
  // re-calls picotts_set_idle_notify / picotts_set_error_notify before
  // each init, so this should already be covered. Zeroing here is a
  // belt-and-braces defense against the InstrFetchProhibited crash on
  // i2sttslang <lang> (wild EPC e.g. 0x1075b555 / 0x150f0d56) — if any
  // code path fires errorCb or idleCb between cleanup and the second
  // notify-set, a stale plugin address (post-relocate) would be wild.
  outputCb = NULL;
  errorCb = NULL;
  idleCb = NULL;
}


bool picotts_init(unsigned prio, picotts_output_fn cb, int core)
{
  if (!exitLock)
    exitLock = xSemaphoreCreateBinary();

  outputCb = cb;

  if (picoMemArea)
  {
    ESP_LOGE(tag, "already initialized");
    return false;
  }
  // The 1.1 MB SVOX engine arena does not fit the internal heap on a
  // 4 MB ESP32-S3 (~200 KB free), so plain malloc() failed here and
  // picotts_init() returned "insufficient memory". Route it through
  // Tasmota's PSRAM-aware special_malloc() so the arena lands in PSRAM
  // (the voice resources already do). pico_arena_malloc is a C-linkage
  // shim around special_malloc defined in xdrv_123_plugins.ino under the
  // same USE_PICOTTS+ESP32 gate; on a no-PSRAM board special_malloc()
  // falls back to plain malloc(). free() works on either.
#ifdef USE_PICOTTS
  extern void *pico_arena_malloc(size_t size);
  picoMemArea = pico_arena_malloc(PICO_MEM_SIZE);
#else
  picoMemArea = malloc(PICO_MEM_SIZE);
#endif
  if (!picoMemArea)
  {
    ESP_LOGE(tag, "insufficient memory to initialize picotts");
    return false;
  }

  #define PICO_INIT_CHECK(msg) \
    if (ret != 0) \
    { \
      esp_pico_err_print(msg, ret); \
      esp_pico_cleanup(); \
      return false; \
    }

  int ret = pico_initialize(picoMemArea, PICO_MEM_SIZE, &picoSystem);
  PICO_INIT_CHECK("init failed");

  const void *ta = find_ta_bin_start();
  if (!ta)
  {
    ESP_LOGE(tag, "Unable to find text analysis resource");
    return false;
  }
  else
    ESP_LOGI(tag, "Loading text analysis resource from %p", ta);

  ret = esp_pico_loadResource(picoSystem, ta, &picoTaResource);
  PICO_INIT_CHECK("Text analysis load failed");

  const void *sg = find_sg_bin_start();
  if (!sg)
  {
    ESP_LOGE(tag, "Unable to find signal generator resource");
    return false;
  }
  else
    ESP_LOGI(tag, "Loading signal generator resource from %p", sg);

  ret = esp_pico_loadResource(picoSystem, sg, &picoSgResource);
  PICO_INIT_CHECK("Signal generator load failed");

  ret = pico_createVoiceDefinition(picoSystem, voiceName);
  PICO_INIT_CHECK("Voice creation failed");

  pico_Retstring str;

  ret = pico_getResourceName(picoSystem, picoTaResource, str);
  PICO_INIT_CHECK("TA resource name error");
  ret = pico_addResourceToVoiceDefinition(
    picoSystem, voiceName, (const pico_Char *)str);
  PICO_INIT_CHECK("TA resource add failed");

  ret = pico_getResourceName(picoSystem, picoSgResource, str);
  PICO_INIT_CHECK("SG resource name error");
  ret = pico_addResourceToVoiceDefinition(
    picoSystem, voiceName, (const pico_Char *)str);
  PICO_INIT_CHECK("SG resource add failed");

  ret = pico_newEngine(picoSystem, voiceName, &picoEngine);
  PICO_INIT_CHECK("Engine creation failed");

  #undef PICO_INIT_CHECK

  textQ = xQueueCreate(CONFIG_PICOTTS_INPUT_QUEUE_SIZE, sizeof(char));
  if (!textQ)
  {
    ESP_LOGE(tag, "Failed to create input queue");
    esp_pico_cleanup();
    return false;
  }

  if (xTaskCreatePinnedToCore(esp_pico_run, "picotts", 8192, NULL,
        prio, &picoTask, core == -1 ? tskNO_AFFINITY : core)
      != pdPASS)
  {
    ESP_LOGE(tag, "Failed to create task");
    esp_pico_cleanup();
    return false;
  }

  return true;
}


void picotts_add(const char *text, unsigned len)
{
  while(len--)
    xQueueSendToBack(textQ, text++, portMAX_DELAY);
}


void picotts_shutdown(void)
{
  esp_pico_cleanup();

#if CONFIG_PICOTTS_RESOURCE_MODE_PARTITION
  unmap_partitions();
#endif
}


void picotts_set_error_notify(picotts_error_notify_fn cb)
{
  errorCb = cb;
}


void picotts_set_idle_notify(picotts_idle_notify_fn cb)
{
  idleCb = cb;
}
