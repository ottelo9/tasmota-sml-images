/* esp_picorsrc.h — prototypes for the embedded-friendly resource loader.
 *
 * Upstream DiUS/esp-picotts ships these in their include/ dir but the
 * file isn't checked in to GitHub (only picotts.h is). Synthesised here
 * from the function definitions in esp_picorsrc.c.
 */
#ifndef ESP_PICORSRC_H
#define ESP_PICORSRC_H

#include "picoapi.h"

#ifdef __cplusplus
extern "C" {
#endif

pico_status_t esp_pico_loadResource(
    pico_System sys, const void *raw, pico_Resource *outResource);

pico_status_t esp_pico_unloadResource(
    pico_System sys, pico_Resource *inResource);

#ifdef __cplusplus
}
#endif
#endif
