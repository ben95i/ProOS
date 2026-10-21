/* ProOS addition: SpeexDSP's allocator hook and counters (see include/speexdsp_alloc.h). */
#include <stdbool.h>
#include <stdlib.h>
#include "speexdsp_alloc.h"

/**
 * @brief Default allocator: zeroed memory from the C heap.
 */
static void *defaultAlloc(size_t sizeBytes) {
    return calloc(1, sizeBytes);
}

/**
 * @brief Default free: back to the C heap.
 */
static void defaultFree(void *pointer) {
    free(pointer);
}

static SpeexdspAllocFunction currentAllocFunction = defaultAlloc;
static SpeexdspFreeFunction currentFreeFunction = defaultFree;

static int liveAllocationCount = 0;
static size_t allocatedBytesTotal = 0;
static int failedAllocationCount = 0;

void speexdspSetAllocator(SpeexdspAllocFunction allocFunction, SpeexdspFreeFunction freeFunction) {
    bool isEitherMissing = (allocFunction == NULL || freeFunction == NULL);
    if (isEitherMissing) {
        currentAllocFunction = defaultAlloc;
        currentFreeFunction = defaultFree;
        return;
    }
    currentAllocFunction = allocFunction;
    currentFreeFunction = freeFunction;
}

void *speexdspAlloc(size_t sizeBytes) {
    void *pointer = currentAllocFunction(sizeBytes);
    if (pointer == NULL) {
        failedAllocationCount++;
        return NULL;
    }
    liveAllocationCount++;
    allocatedBytesTotal += sizeBytes;
    return pointer;
}

void speexdspFree(void *pointer) {
    if (pointer == NULL) {
        return;
    }
    liveAllocationCount--;
    currentFreeFunction(pointer);
}

int speexdspLiveAllocations(void) {
    return liveAllocationCount;
}

size_t speexdspAllocatedBytes(void) {
    return allocatedBytesTotal;
}

int speexdspFailedAllocations(void) {
    return failedAllocationCount;
}
