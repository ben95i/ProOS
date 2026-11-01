/*
 * Local addition to the vendored SpeexDSP (README.md): copies the echo canceller's
 * learned filter out of a state and back into a new one, so a call can start with
 * what the last call learned instead of from zero. The functions are appended to
 * libspeexdsp/mdf.c, since the state's fields are private to it.
 *
 *   size_t bytes = speexEchoFilterBytes(echoState);        // 23 KB at 120 / 1920
 *   void *saved = malloc(bytes);
 *   speexEchoSaveFilter(echoState, saved);                  // before destroying it
 *   ...
 *   bool isLoaded = speexEchoLoadFilter(newEchoState, saved, bytes);
 *
 * A filter only loads into a state of the same frame size, tail, channel count
 * and sampling rate; otherwise the load does nothing and returns false.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "speex/speex_echo.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes speexEchoSaveFilter() writes for this state. */
size_t speexEchoFilterBytes(const SpeexEchoState *state);

/** Copies the learned filter into buffer (speexEchoFilterBytes() bytes). */
void speexEchoSaveFilter(const SpeexEchoState *state, void *buffer);

/** Loads a filter saved from a state of the same shape and rate; false (and the
    state untouched) if bufferBytes or the shape differ. */
bool speexEchoLoadFilter(SpeexEchoState *state, const void *buffer, size_t bufferBytes);

#ifdef __cplusplus
}
#endif
