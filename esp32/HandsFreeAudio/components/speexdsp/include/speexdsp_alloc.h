/*
 * ProOS addition: the allocator SpeexDSP uses for its echo canceller, preprocessor
 * and FFT tables, plus counters to see what they cost.
 *
 * Every speex_alloc() / speex_free() inside the library lands here (through
 * libspeexdsp/os_support_custom.h). By default that is calloc() / free(); the
 * firmware can swap in its own pair, such as a bump allocator over the A2DP buffer
 * during a call (AEC-PLAN.md, memory plan).
 *
 * Not thread-safe: create and destroy SpeexDSP states from one task only.
 *
 * Example, measuring what an echo canceller costs:
 *   size_t bytesBefore = speexdspAllocatedBytes();
 *   SpeexEchoState *echoState = speex_echo_state_init(120, 1920);
 *   size_t echoStateBytes = speexdspAllocatedBytes() - bytesBefore;   // ~44 KB
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Must return memory that is already zeroed, as calloc() does, or NULL.
typedef void *(*SpeexdspAllocFunction)(size_t sizeBytes);
typedef void (*SpeexdspFreeFunction)(void *pointer);

/**
 * @brief Routes SpeexDSP's allocations to another allocator pair.
 * * Change it only while no SpeexDSP state exists, or a state would be freed by a
 * different allocator than the one that made it. Passing NULL for either restores
 * calloc() / free().
 */
void speexdspSetAllocator(SpeexdspAllocFunction allocFunction, SpeexdspFreeFunction freeFunction);

/** @brief Zeroed allocation through the current allocator; counted. NULL on failure. */
void *speexdspAlloc(size_t sizeBytes);

/** @brief Frees through the current allocator; counted. NULL is ignored. */
void speexdspFree(void *pointer);

/** @brief Allocations made and not yet freed. 0 once every state is destroyed. */
int speexdspLiveAllocations(void);

/** @brief Bytes requested since boot, never decreasing. Take a difference to size a state. */
size_t speexdspAllocatedBytes(void);

/** @brief Allocations that failed since boot. */
int speexdspFailedAllocations(void);

#ifdef __cplusplus
}
#endif
