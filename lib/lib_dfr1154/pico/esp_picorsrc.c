/* Copyright (C) 2024 DiUS Computing Pty Ltd.
 * Licensed under the Apache 2.0 license.
 *
 * @author J Mattsson <jmattsson@dius.com.au>
 */
#include "picoapi.h"
#include "picoapid.h"
#include "picorsrc.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
// We need access to the internal definitions in order to implement an
// alternative resource loader, hence this unorthodox include.
// Path is relative to esp_picorsrc.c's location (lib/libesp32_div/pico/).
#include "lib/picorsrc.c"

#pragma GCC diagnostic pop


static uint16_t esp_pico_load_pi_u16(const char *raw, unsigned offs)
{
  uint16_t a = raw[offs];
  uint16_t b = raw[offs+1];
  return (b << 8 | a);
}

static uint32_t esp_pico_load_pi_u32(const char *raw, unsigned offs)
{
  uint32_t a = raw[offs];
  uint32_t b = raw[offs+1];
  uint32_t c = raw[offs+2];
  uint32_t d = raw[offs+3];
  // BUG fix 2026-05-10: was `(d << 24 || c << 16 | b << 8 | a)` — `||`
  // (logical) instead of `|` (bitwise) reduces the whole expression to
  // 0 or 1, so the function returned ~0/1 instead of the actual 32-bit
  // little-endian value. Used to read voice-resource length at line 93
  // of esp_pico_loadResource. Worked accidentally for first init (some
  // SVOX paths don't strictly enforce len) but may have contributed to
  // resource-state corruption on second init.
  return (d << 24) | (c << 16) | (b << 8) | a;
}


pico_status_t verify_svox_header(const char *raw)
{
  const char marker[] = " (C) SVOX AG ";
  for (unsigned i = 0; i < sizeof(marker) -1; ++i)
  {
    if (raw[i] != (marker[i] - 0x20))
      return PICO_EXC_FILE_CORRUPT;
  }
  return PICO_OK;
}


pico_status_t esp_pico_loadResource(pico_System sys, const void *raw, pico_Resource *outResource)
{
  picorsrc_Resource *resource = (picorsrc_Resource *)outResource;
  picorsrc_ResourceManager this = sys->rm;
  picorsrc_Resource res = picorsrc_newResource(this->common->mm);
  if (res == NULL)
    return picoos_emRaiseException(this->common->em, PICO_EXC_OUT_OF_MEM, NULL, NULL);

  if (this->numResources >= PICO_MAX_NUM_RESOURCES)
  {
    picoos_deallocate(this->common->mm, (void *)&res);
    return picoos_emRaiseException(this->common->em, PICO_EXC_MAX_NUM_EXCEED,
      NULL, (picoos_char *)"no more than %i resources", PICO_MAX_NUM_RESOURCES);
  }

  enum {
    SVOXHDR_OFFS = 0, // first 13 bytes = " (C) SVOX AG " downshifted by 0x20
    HEADER_LEN_OFFS = 13, // header length read as le u16 after that
    DATA_OFFS = HEADER_LEN_OFFS + 2,
  };
  pico_status_t status = verify_svox_header(raw + SVOXHDR_OFFS);
  if (status != PICO_OK)
    return picoos_emRaiseException(this->common->em, PICO_EXC_FILE_CORRUPT, NULL, NULL);
  unsigned hdrlen1 = esp_pico_load_pi_u16(raw, HEADER_LEN_OFFS);

  const char *data = (const char *)raw + DATA_OFFS;

  // This effectively covers the readHeader() step in picorsrc_loadResource()
  picoos_file_header_t header;
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
  status = picoos_hdrParseHeader(&header, data);
  #pragma GCC diagnostic pop
  data += hdrlen1;

  if (status == PICO_OK &&
      isResourceLoaded(this, header.field[PICOOS_HEADER_NAME].value))
    status = PICO_WARN_RESOURCE_DOUBLE_LOAD;

  uint32_t len = 0;
  if (status == PICO_OK)
  {
    len = esp_pico_load_pi_u32(data, 0);
    data += 4;

    // We fib things here and /don't/ copy into a PICOOS_ALIGN_SIZE memory area,
    // but we get away with it due to the files data areas already being
    // align(4), and on the esp that's all we need.
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wdiscarded-qualifiers"
    res->raw_mem = data;
    #pragma GCC diagnostic pop
    res->start = res->raw_mem;

    // Note resource unique name
    int n = picoos_strlcpy(
      res->name, header.field[PICOOS_HEADER_NAME].value,
      PICORSRC_MAX_RSRC_NAME_SIZ);
    if (n > PICORSRC_MAX_RSRC_NAME_SIZ)
    {
      status = PICO_ERR_INDEX_OUT_OF_RANGE;
      picoos_emRaiseException(
          this->common->em, PICO_ERR_INDEX_OUT_OF_RANGE, NULL,
          (picoos_char *)"resource %s", res->name);
    }
  }

  if (status == PICO_OK)
  {
    const uint8_t *ctype = header.field[PICOOS_HEADER_CONTENT_TYPE].value;
    if (picoos_strcmp(ctype, PICORSRC_FIELD_VALUE_TEXTANA) == 0)
      res->type = PICORSRC_TYPE_TEXTANA;
    else if (picoos_strcmp(ctype, PICORSRC_FIELD_VALUE_SIGGEN) == 0)
      res->type = PICORSRC_TYPE_SIGGEN;
    else
      res->type = PICORSRC_TYPE_OTHER;

    // Create kb list from resource
    status = picorsrc_getKbList(this, res->start, len, &res->kbList);
  }

  if (status == PICO_OK)
  {
    res->next = this->resources;
    this->resources = res;
    this->numResources++;
    *resource = res;
  }
  else {
    picorsrc_disposeResource(this->common->mm, &res);
  }
  return status;
};


pico_status_t esp_pico_unloadResource(pico_System sys, pico_Resource *inResource)
{
  picorsrc_Resource *resource = (picorsrc_Resource *)inResource;
  picorsrc_ResourceManager this = sys->rm;

  picorsrc_Resource r1, r2, rsrc;

  if (resource == NULL)
    return PICO_ERR_NULLPTR_ACCESS;
  else
    rsrc = *resource;

  if (rsrc->lockCount > 0)
    return PICO_EXC_RESOURCE_BUSY;

  r1 = NULL;
  r2 = this->resources;
  while (r2 != NULL && r2 != rsrc) {
    r1 = r2;
    r2 = r2->next;
  }
  if (NULL == r1)
    this->resources = rsrc->next;
  else if (NULL == r2)
    return PICO_ERR_OTHER;    /* didn't find resource in rm! */
  else
    r1->next = rsrc->next;

  if (NULL != rsrc->kbList)
    picorsrc_releaseKbList(this, &rsrc->kbList);

  picoos_deallocate(this->common->mm,(void **)resource);
  this->numResources--;

  return PICO_OK;
}
