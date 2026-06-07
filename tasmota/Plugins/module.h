#ifndef _MODULE_H_
#define _MODULE_H_

#undef  I2C_BUFFER_LENGTH

#include <stdio.h>
#include <stddef.h>
#include <Wire.h>
#include <Stream.h>
#include <HardwareSerial.h>

#ifdef USE_BINPLUGINS

#define AGPIO(x) ((x)<<5)
#define BGPIO(x) ((x)>>5)

#ifndef SerConfu8
#define SerConfu8 uint8_t
#endif
//#include "tasmota_compat.h"
#include "../include/tasmota.h"
#include "../include/i18n.h"
#include "../include/tasmota_globals.h"


#ifndef D_SENSOR_NONE
#include "../language/de_DE.h"
#endif

#include "../include/tasmota_template.h"
#include "../include/tasmota_types.h"

extern TSettings* Settings;

#define SETTINGS TSettings

#ifndef PROGMEM
#define PROGMEM
#endif

#include "modules_def.h"

static int32_t mod_func_execute(uint32_t);
static void end_of_module(void);

#endif

#endif // _MODULE_H_
