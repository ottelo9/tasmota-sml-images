#ifndef PICOTTS_H
#define PICOTTS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PICOTTS_SAMPLE_FREQ_HZ 16000
#define PICOTTS_SAMPLE_BITS 16

typedef void (*picotts_output_fn)(int16_t *samples, unsigned count);

/**
 * Set runtime resource pointers (Tasmota / PlatformIO addition; not in
 * upstream DiUS). When non-NULL these supersede both EMBED-mode binary
 * symbols and PARTITION-mode mmap. Use case: voice data loaded into
 * PSRAM from LittleFS at boot, so the firmware stays small and the
 * voice can be swapped without reflashing. Pass NULL to revert to the
 * compile-time mode. MUST be called before picotts_init().
 */
void picotts_set_resources(const void *ta_ptr, const void *sg_ptr);

/**
 * Initialises the PicoTTS engine and prepares to receive TTS requests.
 * A new task is launched, which is used to run the TTS engine.
 * @param prio The priority of the TTS task.
 * @param output_cb Callback function which gets invoked directly from the
 *   TTS task with a buffer of samples generated.
 * @param core The core number to bind the TTS task to, or -1 for no fixed
 *   core affinity.
 * @returns True on success, false on failure.
 */
bool picotts_init(unsigned prio, picotts_output_fn output_cb, int core);

/**
 * Adds text to be synthesised. The TTS engine will wait until it sees
 * an appropriate stop (e.g. sentence stop, \0) before commencing the
 * speech generation.
 *
 * A queue is used to transfer the text to the TTS engine. The queue
 * size may be configured via Kconfig. Adding more text while the queue
 * is full will cause this function to block.
 *
 * @param txt The pointer to the text to be spoken, in UTF8 format. The
 *   text is copied, so the pointer may be invalidated immediately upon
 *   return from this call.
 * @param len The number of bytes available in @c text.
 */
void picotts_add(const char *txt, unsigned len);

/**
 * Stops the TTS engine task and frees the used memory resources.
 * Call @c picotts_init() again to reinitialise, if needed.
 */
void picotts_shutdown();


typedef void (*picotts_error_notify_fn)(void);

/**
 * Sets a callback function to be invoked if picotts encounters and error
 * and aborts.
 * @param cb The callback handler. Invoked from the TTS task prior to said
 *   task exiting. To resume TTS operation the callback should schedule a
 *   reinitialisation of PicoTTS, i.e @c picotts_shutdown() followed by
 *   @c picotts_init(). Pass NULL to unregister a set callback function.
 */
void picotts_set_error_notify(picotts_error_notify_fn cb);


typedef void (*picotts_idle_notify_fn)(void);

/**
 * Sets a callback function which gets called when the TTS engines enters
 * idle state again after having generated data. May be used for resource
 * arbitration.
 * @param cb The callback handler. Invoked from the TTS task when the
 *   engine goes idle. Pass NULL to unregister a set callback function.
 */
void picotts_set_idle_notify(picotts_idle_notify_fn cb);

#ifdef __cplusplus
}
#endif
#endif
