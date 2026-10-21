/*
 * SpeexDSP check for the PC: builds the vendored fixed-point SpeexDSP exactly as the
 * firmware does (same sources, same defines, see the Makefile) and reports, for each
 * configuration AEC-PLAN.md considers:
 *
 *   - the bytes the echo canceller and the preprocessor allocate, and that every
 *     allocation is freed again
 *   - echo reduction (ERLE) on a synthetic echo, for the canceller alone and with the
 *     preprocessor's residual echo suppression
 *   - host time per frame (a PC is far faster than the ESP32; for comparison only)
 *
 * The echo is white noise from the "speaker" passed through a fixed room response
 * (a bulk delay, a few reflections and a decaying tail), plus a little mic noise.
 * It checks the library works in this build; the realistic car model is step 3.
 *
 *   make speexcheck
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "speexdsp_alloc.h"

#define MAX_FRAME_SAMPLES   120
#define ROOM_TAPS           1024
#define RUN_SECONDS         20
#define MEASURE_SECONDS     5       /* ERLE over the last seconds, once converged */

typedef struct {
    int sampleRate;
    int frameSamples;
    int tailMilliseconds;
} SpeexConfiguration;

static const SpeexConfiguration CONFIGURATIONS[] = {
    { 16000, 120, 120 },    /* chosen: mSBC, 120 ms tail */
    { 16000, 120,  90 },    /* shorter tail, if step 6 allows it */
    {  8000, 120, 120 },    /* CVSD: the firmware keeps 120-sample frames (15 ms) at 8 kHz */
};

/* xorshift32: deterministic noise, so runs compare */
static uint32_t noiseState = 2463534242u;

/**
 * @brief Next pseudo-random value in -1.0 .. 1.0.
 */
static float nextNoise(void) {
    noiseState ^= noiseState << 13;
    noiseState ^= noiseState >> 17;
    noiseState ^= noiseState << 5;
    float scaled = (float)(int32_t)noiseState / 2147483648.0f;
    return scaled;
}

/**
 * @brief Fills a room response: bulk delay, three reflections and an exponential tail.
 * * @param response   ROOM_TAPS taps to fill.
 * @param sampleRate Rate the taps are at, so the delay and decay are in real time.
 */
static void buildRoomResponse(float *response, int sampleRate) {
    memset(response, 0, sizeof(float) * ROOM_TAPS);
    int bulkDelaySamples = (sampleRate * 25) / 1000;               /* 25 ms */
    response[bulkDelaySamples] = 0.6f;
    response[bulkDelaySamples + sampleRate / 1000] = -0.3f;          /* +1 ms */
    response[bulkDelaySamples + (sampleRate * 3) / 1000] = 0.2f;     /* +3 ms */
    response[bulkDelaySamples + (sampleRate * 7) / 1000] = -0.15f;   /* +7 ms */

    float decayPerSample = expf(-1.0f / (0.015f * (float)sampleRate));   /* 15 ms time constant */
    float tailLevel = 0.08f;
    int tailStart = bulkDelaySamples + (sampleRate * 8) / 1000;
    for (int tap = tailStart; tap < ROOM_TAPS; tap++) {
        response[tap] += tailLevel * nextNoise();
        tailLevel *= decayPerSample;
    }
}

/**
 * @brief Clamps a float sample into int16 range.
 */
static int16_t toSample(float value) {
    if (value > 32767.0f) return 32767;
    if (value < -32768.0f) return -32768;
    return (int16_t)value;
}

/**
 * @brief Runs one configuration and prints its memory, ERLE and time per frame.
 * * @return 0 when every allocation was freed and nothing failed, 1 otherwise.
 */
static int checkConfiguration(const SpeexConfiguration *configuration) {
    int sampleRate = configuration->sampleRate;
    int frameSamples = configuration->frameSamples;
    int tailSamples = (sampleRate * configuration->tailMilliseconds) / 1000;

    size_t bytesBeforeEcho = speexdspAllocatedBytes();
    SpeexEchoState *echoState = speex_echo_state_init(frameSamples, tailSamples);
    speex_echo_ctl(echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
    size_t echoBytes = speexdspAllocatedBytes() - bytesBeforeEcho;

    size_t bytesBeforePreprocess = speexdspAllocatedBytes();
    SpeexPreprocessState *preprocessState = speex_preprocess_state_init(frameSamples, sampleRate);
    speex_preprocess_ctl(preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE, echoState);
    size_t preprocessBytes = speexdspAllocatedBytes() - bytesBeforePreprocess;

    if (echoState == NULL || preprocessState == NULL) {
        printf("  %5d Hz %3d ms: state creation failed\n", sampleRate, configuration->tailMilliseconds);
        return 1;
    }

    static float roomResponse[ROOM_TAPS];
    static float speakerHistory[ROOM_TAPS];
    buildRoomResponse(roomResponse, sampleRate);
    memset(speakerHistory, 0, sizeof(speakerHistory));
    int historyPosition = 0;

    int totalFrames = (RUN_SECONDS * sampleRate) / frameSamples;
    int measureFromFrame = totalFrames - (MEASURE_SECONDS * sampleRate) / frameSamples;
    double micEchoEnergy = 0.0;
    double cancelledEnergy = 0.0;
    double suppressedEnergy = 0.0;
    double processingSeconds = 0.0;

    int16_t speakerFrame[MAX_FRAME_SAMPLES];
    int16_t micFrame[MAX_FRAME_SAMPLES];
    int16_t outputFrame[MAX_FRAME_SAMPLES];

    for (int frame = 0; frame < totalFrames; frame++) {
        for (int sample = 0; sample < frameSamples; sample++) {
            float speakerValue = 6000.0f * nextNoise();
            speakerFrame[sample] = toSample(speakerValue);
            speakerHistory[historyPosition] = (float)speakerFrame[sample];

            float echoValue = 0.0f;
            for (int tap = 0; tap < ROOM_TAPS; tap++) {
                int historyIndex = (historyPosition - tap + ROOM_TAPS) % ROOM_TAPS;
                echoValue += roomResponse[tap] * speakerHistory[historyIndex];
            }
            historyPosition = (historyPosition + 1) % ROOM_TAPS;

            float micNoise = 20.0f * nextNoise();
            micFrame[sample] = toSample(echoValue + micNoise);
        }

        struct timespec startTime, endTime;
        clock_gettime(CLOCK_MONOTONIC, &startTime);
        speex_echo_cancellation(echoState, micFrame, speakerFrame, outputFrame);
        bool isMeasuring = (frame >= measureFromFrame);
        if (isMeasuring) {
            for (int sample = 0; sample < frameSamples; sample++) {
                double micValue = micFrame[sample];
                double cancelledValue = outputFrame[sample];
                micEchoEnergy += micValue * micValue;
                cancelledEnergy += cancelledValue * cancelledValue;
            }
        }
        speex_preprocess_run(preprocessState, outputFrame);
        clock_gettime(CLOCK_MONOTONIC, &endTime);

        double elapsedSeconds = (double)(endTime.tv_sec - startTime.tv_sec)
                              + (double)(endTime.tv_nsec - startTime.tv_nsec) / 1e9;
        processingSeconds += elapsedSeconds;

        if (isMeasuring) {
            for (int sample = 0; sample < frameSamples; sample++) {
                double suppressedValue = outputFrame[sample];
                suppressedEnergy += suppressedValue * suppressedValue;
            }
        }
    }

    double cancelErleDb = 10.0 * log10(micEchoEnergy / (cancelledEnergy > 1.0 ? cancelledEnergy : 1.0));
    double totalErleDb = 10.0 * log10(micEchoEnergy / (suppressedEnergy > 1.0 ? suppressedEnergy : 1.0));
    double microsecondsPerFrame = processingSeconds * 1e6 / (double)totalFrames;

    speex_preprocess_state_destroy(preprocessState);
    speex_echo_state_destroy(echoState);
    int liveAfterDestroy = speexdspLiveAllocations();

    printf("  %5d Hz %3d ms tail: echo %5.1f KB + preprocess %4.1f KB = %5.1f KB | "
           "ERLE cancel %5.1f dB, with suppression %5.1f dB | %5.1f us/frame on this PC | live allocs %d\n",
           sampleRate, configuration->tailMilliseconds,
           echoBytes / 1024.0, preprocessBytes / 1024.0, (echoBytes + preprocessBytes) / 1024.0,
           cancelErleDb, totalErleDb, microsecondsPerFrame, liveAfterDestroy);

    bool isClean = (liveAfterDestroy == 0 && speexdspFailedAllocations() == 0);
    return isClean ? 0 : 1;
}

int main(void) {
    printf("SpeexDSP 1.2.1 fixed point (KISS FFT), %d s of synthetic echo per configuration:\n", RUN_SECONDS);
    int failures = 0;
    int configurationCount = (int)(sizeof(CONFIGURATIONS) / sizeof(CONFIGURATIONS[0]));
    for (int index = 0; index < configurationCount; index++) {
        failures += checkConfiguration(&CONFIGURATIONS[index]);
    }
    printf(failures == 0 ? "  OK\n" : "  FAILED: leaked or failed allocations\n");
    return failures == 0 ? 0 : 1;
}
