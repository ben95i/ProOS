/* ProOS addition: included by os_support.h when OS_SUPPORT_CUSTOM is defined.
   Routes SpeexDSP's allocations through speexdsp_alloc.c, so the firmware can
   choose where the echo canceller's memory comes from and count it.
   speex_realloc() keeps its default: none of the modules built here call it. */
#ifndef OS_SUPPORT_CUSTOM_H
#define OS_SUPPORT_CUSTOM_H

#include "speexdsp_alloc.h"

#define OVERRIDE_SPEEX_ALLOC
static inline void *speex_alloc(int size)
{
   return speexdspAlloc((size_t)size);
}

#define OVERRIDE_SPEEX_ALLOC_SCRATCH
static inline void *speex_alloc_scratch(int size)
{
   return speexdspAlloc((size_t)size);
}

#define OVERRIDE_SPEEX_FREE
static inline void speex_free(void *ptr)
{
   speexdspFree(ptr);
}

#define OVERRIDE_SPEEX_FREE_SCRATCH
static inline void speex_free_scratch(void *ptr)
{
   speexdspFree(ptr);
}

#endif
