/* ProOS addition: replaces the speexdsp_config_types.h that SpeexDSP's configure
   script generates from speexdsp_config_types.h.in. Fixed-width types from
   stdint.h, the same on the ESP32 and on the PC simulator. */
#ifndef __SPEEX_TYPES_H__
#define __SPEEX_TYPES_H__

#include <stdint.h>

typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;

#endif
