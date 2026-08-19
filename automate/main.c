#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

// --- Constants ---
static const char *TAG = "Honda_HR-V";

// --- Touch Panel Pins ---
#define TOUCH_PAD_PLAY       GPIO_NUM_4
#define TOUCH_PAD_PAUSE      GPIO_NUM_18
#define TOUCH_PAD_NEXT_TRACK GPIO_NUM_19
#define TOUCH_PAD_PREV_TRACK GPIO_NUM_22

// --- I2S Audio Pins ---
#define DAC_I2S_BCLK         GPIO_NUM_23
#define DAC_I2S_WS           GPIO_NUM_26
#define DAC_I2S_DATA         GPIO_NUM_25
#define MIC_I2S_BCLK         GPIO_NUM_21
#define MIC_I2S_WS           GPIO_NUM_32
#define MIC_I2S_DATA         GPIO_NUM_34

static const uint16_t SAMPLE_RATE_8K = 8000;
static const uint16_t SAMPLE_RATE_16K = 16000;
static const uint16_t SAMPLE_RATE_44K = 44100;

static const uint32_t DMA_FRAME_SIZE_SAMPLES = 120;

// OPTIMIZATION: Deep Buffering for A2DP Crackle Prevention
static const uint32_t RING_BUFFER_SIZE_BYTES = 40 * 1024;     
static const uint32_t PRE_ROLL_THRESHOLD_BYTES = 20 * 1024;   
static const uint32_t STREAM_BUFFER_SIZE_BYTES = 2048;
static const uint32_t TRIGGER_LEVEL_BYTES = 128;

static const float MICROPHONE_GAIN = 2.0f;

// FIX: Moved out of bluetoothHandsFreeReceiveCallback(). A #define inside a function
// body is not scoped to that function in C - it leaks into the rest of the translation
// unit for every line that follows. Keeping it here makes that explicit and avoids any
// future accidental redefinition/clash.
#define MAX_HFP_SAMPLES 512

// --- DSP Filter Frequencies ---
#define MATH_PI 3.14159265358979323846f
static const float HIGH_PASS_CUTOFF_HZ = 200.0f;  
static const float LOW_PASS_CUTOFF_HZ = 7000.0f;  

static const char *NVS_NAMESPACE_STRING = "bt_storage";
static const char *NVS_KEY_BLUETOOTH_MAC = "last_bda";

/* * ============================================================================
 * NEW: Speaker (I2S Tx) DMA profiles
 * ============================================================================
 *
 * The single biggest source of "the call feels laggy" was that the speaker
 * channel is sized for A2DP, not for a phone call.
 *
 * i2s_channel_write() does NOT hand a partially-filled DMA descriptor to the
 * hardware - it accumulates into the descriptor and only releases it once it is
 * completely full (dma_buffer_size = dma_frame_num * slot_num * slot_bit_width/8).
 * With the original music settings (dma_desc_num = 6, dma_frame_num = 960) each
 * descriptor holds 960 stereo frames:
 *
 *     44100 Hz -> 21.8 ms per descriptor, ~131 ms queued   (great for music)
 *     16000 Hz -> 60.0 ms per descriptor, ~360 ms queued   (terrible for a call)
 *
 * So during a call the downlink was carrying up to a third of a second of pure
 * buffering delay before the caller's voice ever reached the speaker. That is
 * also fatal for echo cancellation: an adaptive filter has to span the whole
 * echo delay, and nothing that runs on an ESP32 can span 360 ms.
 *
 * These call-mode numbers give 60 stereo frames per descriptor (240 bytes),
 * which is exactly one 8 kHz CVSD HFP frame and exactly half of one 16 kHz
 * mSBC frame, so writes always land on descriptor boundaries:
 *
 *     16000 Hz -> 3.75 ms per descriptor, 15 ms queued
 *      8000 Hz -> 7.50 ms per descriptor, 30 ms queued
 */
#define MUSIC_TX_DESC_COUNT        6
#define MUSIC_TX_FRAMES_PER_DESC   (DMA_FRAME_SIZE_SAMPLES * 8)   // 960, unchanged
#define CALL_TX_DESC_COUNT         4
#define CALL_TX_FRAMES_PER_DESC    60

// Writes during a call must never wedge the Bluedroid callback thread. A long
// timeout here is worse than dropping a frame - dropped audio is 3.75 ms of
// silence, a wedged BT task is a dead call.
#define CALL_TX_WRITE_TIMEOUT_MS   20

/* * ============================================================================
 * NEW: HFP uplink latency control
 * ============================================================================
 *
 * microphoneStreamBuffer is 2048 bytes = 1024 samples = 64 ms at 16 kHz. The
 * original code only discarded audio once that was completely full, so whatever
 * backlog built up at call start stayed there for the rest of the call and was
 * added to the uplink delay. We now hold the backlog to a fixed low water mark
 * and throw away the OLDEST whole frames to stay under it.
 */
#define HFP_UPLINK_MAX_BACKLOG_BYTES  480   // 2 mic frames

// The original 250 ms settle delay costs a quarter of a second of the near-end
// talker's first words. 80 ms is enough for the baseband to settle.
#define HFP_MIC_STARTUP_DELAY_MS      80

/* * ============================================================================
 * NEW: Acoustic Echo Canceller configuration
 * ============================================================================
 *
 * Normalised LMS adaptive filter + double-talk detector + residual suppressor.
 * Runs on the microphone signal, using the decoded downlink (what we just sent
 * to the speaker) as the reference.
 *
 * AEC_FILTER_TAPS is deliberately generous. Rather than trying to measure the
 * exact loudspeaker-to-microphone delay, the filter simply spans the entire
 * plausible range of it (32 ms at 16 kHz), which covers the Tx DMA queue, the
 * DAC/amp group delay, the acoustic flight time across the cabin and the MEMS
 * microphone's own decimation delay. Watch the "peak tap" figure in the
 * periodic AEC log - if it settles well below AEC_FILTER_TAPS you can shave
 * taps off and save CPU.
 *
 * Cost at 512 taps / 16 kHz is roughly 25-30% of one core. The microphone task
 * is pinned to core 1, where audioOutputTask is idle for the whole call, so
 * there is room. Drop to 256 if you need it back.
 */
#define AEC_ENABLED                1
#define AEC_FILTER_TAPS            512
#define AEC_REF_RING_SAMPLES       2048          // must be a power of two
#define AEC_REF_RING_MASK          (AEC_REF_RING_SAMPLES - 1)
#define AEC_STEP_SIZE              0.30f         // NLMS mu, 0.1 (slow/stable) .. 0.5 (fast/twitchy)
#define AEC_POWER_EPSILON          2000.0f       // regularisation, in int16^2 units
#define AEC_FAR_ACTIVE_FLOOR       2000.0f       // mean-square below this = far end is silent
#define AEC_DOUBLETALK_MARGIN      2.0f          // freeze adaptation 6 dB above expected echo
#define AEC_DOUBLETALK_HANGOVER    12            // frames (~90 ms at 16 kHz)
#define AEC_NEAREND_MARGIN         6.0f          // near end declared ~8 dB above expected residual
#define AEC_LEAK_TRUST_THRESHOLD   0.5f          // trust the residual test once the filter earns 3 dB
#define AEC_NEAREND_HANGOVER       24            // frames (~180 ms at 16 kHz)
#define AEC_LAG_TOLERANCE_SAMPLES  192           // re-anchor if streams slip this far apart
#define AEC_REF_GUARD_SAMPLES      384           // zeroed run kept ahead of the write pointer
#define AEC_MAX_WEIGHT_ENERGY      64.0f         // no real echo path has this much gain
#define AEC_NOISE_FLOOR_MAX        1.0e6f        // ceiling on the comfort-noise estimate
#define AEC_SUPPRESS_FLOOR         0.06f         // max attenuation, far-end talking alone
#define AEC_SUPPRESS_FLOOR_DT      0.45f         // max attenuation during double-talk
#define AEC_LOG_INTERVAL_FRAMES    256

// --- Global Hardware and OS Handles ---
static i2s_chan_handle_t txChannel = NULL;
static i2s_chan_handle_t rxChannel = NULL;
static RingbufHandle_t a2dpRingBuffer = NULL;
static StreamBufferHandle_t microphoneStreamBuffer = NULL;

static SemaphoreHandle_t preRollSemaphore = NULL;
static SemaphoreHandle_t txMutex = NULL;
static SemaphoreHandle_t rxMutex = NULL;

// OPTIMIZATION: Task handle to directly wake/sleep the microphone
static TaskHandle_t micTaskHandle = NULL;

// Caches the remote phone's address when the HFP link comes up, for logging and for any
// future use that legitimately needs the peer address. NOT used to initiate SCO setup -
// the Audio Gateway (phone) owns that in HFP.
static esp_bd_addr_t hfpRemoteDeviceAddress = {0};
static volatile bool isHfpRemoteAddressValid = false;

// --- Global State Variables ---
// OPTIMIZATION: Volatile flags for atomic, lock-free status checking across CPU cores
static volatile bool isAudioPathOpen = false;
static volatile bool isPlaying = false;
static volatile bool enableHfpStereoMode = false;
static volatile bool isMicHardwareEnabled = false; 
static volatile bool isCallActiveOrRinging = false;

// Dynamic filter coefficients based on current sample rate
static float activeLowPassAlpha = 0.0f;
static float activeHighPassAlpha = 0.0f;

// NEW: which DMA profile the Tx channel is currently built with
typedef enum {
    TX_PROFILE_MUSIC = 0,
    TX_PROFILE_CALL  = 1
} TransmitDmaProfile;

static TransmitDmaProfile currentTransmitProfile = TX_PROFILE_MUSIC;

/* * ============================================================================
 * DSP Coefficient Calculations
 * ============================================================================
 */

/**
 * @brief Calculates the alpha (α) coefficients used by the digital High-Pass and Low-Pass IIR filters.
 * * Because filter math depends on time (Δt = 1/Fs), coefficients must be recalculated 
 * every time the Bluetooth sample rate changes. Uses standard RC filter formulas.
 * * @param sampleRate The current active sample rate (8000, 16000, or 44100 Hz).
 */
void calculateFilterAlphas(uint32_t sampleRate) {
    if (sampleRate == 0) return;
    
    float dt = 1.0f / (float)sampleRate;
    
    float rcLow = 1.0f / (2.0f * MATH_PI * LOW_PASS_CUTOFF_HZ);
    activeLowPassAlpha = dt / (rcLow + dt);
    
    float rcHigh = 1.0f / (2.0f * MATH_PI * HIGH_PASS_CUTOFF_HZ);
    activeHighPassAlpha = rcHigh / (rcHigh + dt);
    
    ESP_LOGI(TAG, "--- DSP Updated | Fs: %lu Hz | HP Alpha: %.4f | LP Alpha: %.4f ---", 
             sampleRate, activeHighPassAlpha, activeLowPassAlpha);
}

/* * ============================================================================
 * NEW: Acoustic Echo Canceller
 * ============================================================================
 *
 * Signal flow, all inside the HFP path:
 *
 *   phone --SCO--> bluetoothHandsFreeReceiveCallback()
 *                     |-> I2S Tx -> DAC -> amp -> speakers --.
 *                     '-> echoCancellerPushReference()        | acoustic
 *                                    |                        | coupling
 *                                    v                        v
 *                              reference ring  --------> microphoneTask()
 *                                                            |
 *                                             HPF/LPF -> echoCancellerProcessFrame()
 *                                                            |
 *                                                   gain -> uplink -> phone
 *
 * Three stages:
 *   1. NLMS adaptive filter models the loudspeaker->microphone path and
 *      subtracts a prediction of the echo. This is what preserves quality:
 *      it removes the echo without touching the near-end talker at all.
 *   2. A double-talk detector freezes adaptation whenever the driver is
 *      speaking, so the filter never diverges (the classic failure that makes
 *      cheap car kits sound like they are chewing the audio).
 *   3. A residual suppressor removes what the linear filter cannot - the part
 *      of the echo that came back distorted by a non-linear amp or speaker.
 *      It only bites while the far end is talking and the near end is not, so
 *      in normal conversation it is inaudible to the far end.
 *
 * Nothing here runs when isAudioPathOpen is false, so A2DP is untouched.
 */

static float aecReferenceRing[AEC_REF_RING_SAMPLES];
static float aecWeights[AEC_FILTER_TAPS];

// Written by the Bluedroid callback (core 0), read by microphoneTask (core 1).
static volatile uint32_t aecReferenceWriteIndex = 0;

// Owned exclusively by microphoneTask.
static uint32_t aecMicrophoneIndex = 0;
static uint32_t aecReferenceAnchor = 0;
static float aecEchoReturnLoss = 1.0f;      // estimated |mic| / |reference| coupling
static float aecResidualLeak = 1.0f;        // estimated residual echo / error power
static float aecNoiseFloor = 100.0f;        // for comfort noise
static float aecSuppressionGain = 1.0f;
static int   aecDoubleTalkHangover = 0;
static int   aecNearEndHangover = 0;
static bool  aecNearEndWasActive = false;
static uint32_t aecFrameCounter = 0;
static uint32_t aecNoiseSeed = 0x1234567u;
static volatile bool aecIsPrimed = false;

static inline float aecComfortNoise(void) {
    // xorshift32, cheap and good enough for shaped comfort noise.
    aecNoiseSeed ^= aecNoiseSeed << 13;
    aecNoiseSeed ^= aecNoiseSeed >> 17;
    aecNoiseSeed ^= aecNoiseSeed << 5;
    return ((float)(int32_t)aecNoiseSeed) * (1.0f / 2147483648.0f);
}

/**
 * @brief Throws away everything the filter has learned, without disturbing the
 * reference/microphone alignment. Used both at call start and by the health guard.
 */
static void aecRestartFilter(void) {
    memset(aecWeights, 0, sizeof(aecWeights));
    aecEchoReturnLoss = 1.0f;
    aecResidualLeak = 1.0f;
    aecNoiseFloor = 100.0f;
    aecSuppressionGain = 1.0f;
    aecDoubleTalkHangover = 0;
    aecNearEndHangover = 0;
    aecNearEndWasActive = false;
}

/**
 * @brief Clears all echo canceller state. Called when a call's audio path opens.
 */
static void echoCancellerReset(void) {
    aecIsPrimed = false;
    aecRestartFilter();
    memset(aecReferenceRing, 0, sizeof(aecReferenceRing));
    aecReferenceWriteIndex = 0;
    aecMicrophoneIndex = 0;
    aecReferenceAnchor = 0;
    aecFrameCounter = 0;
}

/**
 * @brief Latches the reference stream position that corresponds to "microphone sample 0".
 * * Called by microphoneTask the instant it starts capturing, after the DMA drain. From
 * here on, microphone sample n is assumed to line up with reference sample
 * (anchor + n), and the adaptive filter's tap span absorbs the real physical delay.
 */
static void echoCancellerAnchor(void) {
    aecReferenceAnchor = aecReferenceWriteIndex;
    aecMicrophoneIndex = 0;
    aecIsPrimed = true;
}

/**
 * @brief Stores downlink samples that were actually accepted by the I2S Tx channel.
 * * Only samples that really made it into the DMA queue are counted. If a write is
 * short (timeout, or the frame was clipped to MAX_HFP_SAMPLES) those samples never
 * reach the speaker, so counting them would permanently skew the alignment.
 * * @param monoSamples Pointer to the mono downlink PCM.
 * @param sampleCount  Number of samples that were successfully queued for playback.
 */
static void echoCancellerPushReference(const int16_t *monoSamples, uint32_t sampleCount) {
    uint32_t writeIndex = aecReferenceWriteIndex;

    for (uint32_t index = 0; index < sampleCount; index++) {
        aecReferenceRing[(writeIndex + index) & AEC_REF_RING_MASK] = (float)monoSamples[index];
    }

    // Keep a zeroed run immediately ahead of the write pointer. The microphone task and
    // the Bluetooth callback are independent real-time streams, so on any given frame the
    // microphone may ask for a reference sample a few hundred samples newer than the last
    // one that arrived. Those slots would otherwise still hold data from 128 ms ago, and
    // the adaptive filter would happily try to fit it. Reading a genuine zero instead
    // contributes nothing to the prediction and nothing to the weight update, which is
    // exactly the desired behaviour.
    for (uint32_t index = 0; index < AEC_REF_GUARD_SAMPLES; index++) {
        aecReferenceRing[(writeIndex + sampleCount + index) & AEC_REF_RING_MASK] = 0.0f;
    }

    // Publish the ring contents before publishing the new index, so the microphone
    // task can never read a slot that has not been filled yet.
    __sync_synchronize();
    aecReferenceWriteIndex = writeIndex + sampleCount;
}

/**
 * @brief Re-aligns the reference and microphone streams if they have slipped apart.
 * * Both streams run at the same I2S clock, so they should stay locked. They can still
 * slip if the Tx DMA underruns (silence is inserted into what the speaker actually
 * played) or if the phone bursts a backlog of SCO packets at us. Rather than resetting
 * and eating a couple of seconds of re-convergence, the tap weights are slid by the
 * same amount, which preserves everything the filter has learned.
 */
static void echoCancellerRealign(int32_t lag) {
    if (lag > 0) {
        uint32_t shift = (uint32_t)lag;
        if (shift >= AEC_FILTER_TAPS) {
            memset(aecWeights, 0, sizeof(aecWeights));
        } else {
            memmove(&aecWeights[shift], &aecWeights[0], (AEC_FILTER_TAPS - shift) * sizeof(float));
            memset(&aecWeights[0], 0, shift * sizeof(float));
        }
    } else {
        uint32_t shift = (uint32_t)(-lag);
        if (shift >= AEC_FILTER_TAPS) {
            memset(aecWeights, 0, sizeof(aecWeights));
        } else {
            memmove(&aecWeights[0], &aecWeights[shift], (AEC_FILTER_TAPS - shift) * sizeof(float));
            memset(&aecWeights[AEC_FILTER_TAPS - shift], 0, shift * sizeof(float));
        }
    }

    aecReferenceAnchor = (uint32_t)((int32_t)aecReferenceAnchor + lag);
    ESP_LOGW(TAG, "--- AEC: re-aligned reference by %ld samples ---", (long)lag);
}

/**
 * @brief Cancels loudspeaker echo from one frame of microphone audio, in place.
 * * @param microphoneSamples Linear (post HPF/LPF, pre-gain) microphone samples.
 * @param sampleCount        Number of samples in the frame.
 */
static void echoCancellerProcessFrame(float *microphoneSamples, uint32_t sampleCount) {
#if !AEC_ENABLED
    (void)microphoneSamples;
    (void)sampleCount;
    return;
#else
    if (!aecIsPrimed || sampleCount == 0) {
        return;
    }

    uint32_t referenceWritten = aecReferenceWriteIndex;
    __sync_synchronize();

    // How far ahead of us is the reference writer? Should hover near zero.
    int32_t lag = (int32_t)(referenceWritten - (aecReferenceAnchor + aecMicrophoneIndex));
    if (lag > AEC_LAG_TOLERANCE_SAMPLES || lag < -AEC_LAG_TOLERANCE_SAMPLES) {
        echoCancellerRealign(lag);
    }

    uint32_t referenceBase = aecReferenceAnchor + aecMicrophoneIndex;

    // Only bail if the microphone has run so far ahead of the reference that even the
    // zero guard cannot cover the read. After a re-align above this cannot happen.
    if ((int32_t)(referenceWritten + AEC_REF_GUARD_SAMPLES - (referenceBase + sampleCount)) < 0) {
        aecMicrophoneIndex += sampleCount;
        return;
    }

    /* ---- Pass 1: frame energies, so the double-talk decision is made up front ---- */
    // The microphone energy comes from this frame. The far-end energy is measured on the
    // most recently ARRIVED reference samples rather than the aligned ones: it is only a
    // gross "is the far end talking" gate, it must never be skewed by where the streams
    // happen to sit relative to each other, and the few milliseconds of lead it carries
    // is absorbed by the double-talk hangover.
    float microphoneEnergy = 0.0f;
    float referenceEnergy = 0.0f;
    uint32_t energyBase = referenceWritten - sampleCount;

    for (uint32_t index = 0; index < sampleCount; index++) {
        float micValue = microphoneSamples[index];
        float refValue = aecReferenceRing[(energyBase + index) & AEC_REF_RING_MASK];
        microphoneEnergy += micValue * micValue;
        referenceEnergy  += refValue * refValue;
    }
    microphoneEnergy /= (float)sampleCount;
    referenceEnergy  /= (float)sampleCount;

    bool isFarEndActive = (referenceEnergy > AEC_FAR_ACTIVE_FLOOR);

    // Double-talk test, self-calibrating. aecEchoReturnLoss tracks how loud the echo
    // normally is for a given reference level, so this works even in a car where the
    // speaker is close enough to the microphone that the coupling is near 0 dB.
    bool isDoubleTalk = false;
    if (isFarEndActive) {
        float expectedEchoLevel = aecEchoReturnLoss * sqrtf(referenceEnergy);
        if (sqrtf(microphoneEnergy) > AEC_DOUBLETALK_MARGIN * expectedEchoLevel) {
            aecDoubleTalkHangover = AEC_DOUBLETALK_HANGOVER;
        }
        if (aecDoubleTalkHangover > 0) {
            aecDoubleTalkHangover--;
            isDoubleTalk = true;
        }
    } else {
        aecDoubleTalkHangover = 0;
    }

    // Adaptation is gated by BOTH tests. The pre-filter test above catches an obviously
    // loud near-end talker immediately; the post-filter test carried over from the
    // previous frame catches a quiet one, who would otherwise sit invisible underneath the
    // echo, be adapted into the filter, and drag the residual-leak estimate up with him -
    // which in turn blinds the near-end detector and makes the suppressor duck the very
    // voice it is supposed to protect. One frame of lag here is 7.5 ms and is covered by
    // the hangover on both tests.
    bool allowAdaptation = (isFarEndActive && !isDoubleTalk && !aecNearEndWasActive);

    /* ---- Pass 2: NLMS filter and update ---- */
    float errorEnergy = 0.0f;
    float predictionEnergy = 0.0f;

    // Exact energy of the reference samples sitting inside the tap window right now.
    //
    // BUG FIX: this used to be an exponentially-weighted average fed from the NEWEST tap
    // slot only. That is wrong in a way that never shows up in a lockstep simulation and
    // always shows up on hardware. The Bluetooth receive callback and the microphone task
    // are independent real-time streams, so their relative phase drifts; whenever the
    // microphone drew level with or ahead of the reference writer, the newest tap slot was
    // part of the zero guard, the average decayed to nothing, and the normaliser pinned
    // itself to AEC_POWER_EPSILON while the real window energy was in the billions. The
    // NLMS step size is inversely proportional to this number, so the filter started
    // taking steps around a million times too large and blew up to infinity within a few
    // samples of the next loud far-end onset. Infinity minus infinity is NaN, NaN fails
    // every comparison so none of the clamps could catch it, and the float-to-int
    // conversion at the end of microphoneTask turned it into a constant - which is why
    // the caller heard nothing at all, permanently, until the SCO link was torn down and
    // rebuilt. Measuring the window directly cannot drift out of step with what the
    // filter is actually convolving against.
    float tapWindowPower = 0.0f;
    {
        uint32_t position = referenceBase & AEC_REF_RING_MASK;
        for (uint32_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
            float value = aecReferenceRing[position];
            tapWindowPower += value * value;
            position = (position - 1) & AEC_REF_RING_MASK;
        }
    }

    for (uint32_t index = 0; index < sampleCount; index++) {
        uint32_t head = (referenceBase + index) & AEC_REF_RING_MASK;

        if (index > 0) {
            // Slide the window on by one: newest reference in, oldest reference out.
            float entering = aecReferenceRing[head];
            float leaving = aecReferenceRing[(head - AEC_FILTER_TAPS) & AEC_REF_RING_MASK];
            tapWindowPower += entering * entering - leaving * leaving;
            if (tapWindowPower < 0.0f) {
                tapWindowPower = 0.0f;   // guard against float rounding drift
            }
        }

        float prediction = 0.0f;
        uint32_t tapPosition = head;
        for (uint32_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
            prediction += aecWeights[tap] * aecReferenceRing[tapPosition];
            tapPosition = (tapPosition - 1) & AEC_REF_RING_MASK;
        }

        float desired = microphoneSamples[index];
        float error = desired - prediction;

        if (allowAdaptation) {
            float normaliser = tapWindowPower + AEC_POWER_EPSILON;
            float stepGain = (AEC_STEP_SIZE * error) / normaliser;

            tapPosition = head;
            for (uint32_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
                aecWeights[tap] += stepGain * aecReferenceRing[tapPosition];
                tapPosition = (tapPosition - 1) & AEC_REF_RING_MASK;
            }
        }

        microphoneSamples[index] = error;
        errorEnergy += error * error;
        predictionEnergy += prediction * prediction;
    }

    errorEnergy /= (float)sampleCount;
    predictionEnergy /= (float)sampleCount;
    aecMicrophoneIndex += sampleCount;

    /* ---- Health guard ---- */
    // Belt and braces for the failure above. A single non-finite value in the filter
    // state is fatal and silent: NaN compares false against everything, so every clamp
    // in this file waves it through, and it then propagates into every sample forever.
    // One check per frame costs nothing and turns a dead microphone into a 7.5 ms gap.
    float weightEnergy = 0.0f;
    uint32_t peakTap = 0;
    float peakWeight = 0.0f;
    for (uint32_t tap = 0; tap < AEC_FILTER_TAPS; tap++) {
        float weight = aecWeights[tap];
        weightEnergy += weight * weight;
        float magnitude = fabsf(weight);
        if (magnitude > peakWeight) {
            peakWeight = magnitude;
            peakTap = tap;
        }
    }

    if (!isfinite(weightEnergy) || !isfinite(errorEnergy) || !isfinite(predictionEnergy) ||
        weightEnergy > AEC_MAX_WEIGHT_ENERGY) {
        ESP_LOGW(TAG, "--- AEC: filter unstable (weight energy %.3g), restarting ---", weightEnergy);
        aecRestartFilter();
        for (uint32_t index = 0; index < sampleCount; index++) {
            microphoneSamples[index] = 0.0f;
        }
        return;
    }

    /* ---- Statistics used by the double-talk detector and the suppressor ---- */
    if (allowAdaptation && referenceEnergy > 0.0f) {
        float instantCoupling = sqrtf(microphoneEnergy / referenceEnergy);
        aecEchoReturnLoss += 0.02f * (instantCoupling - aecEchoReturnLoss);
        if (aecEchoReturnLoss < 0.05f) aecEchoReturnLoss = 0.05f;
        if (aecEchoReturnLoss > 8.0f)  aecEchoReturnLoss = 8.0f;

        if (predictionEnergy > AEC_FAR_ACTIVE_FLOOR) {
            float instantLeak = errorEnergy / predictionEnergy;
            // Symmetric and slow: this must represent the TYPICAL residual, not the best
            // frame ever seen. It is only updated on frames the gate below has already
            // certified as far-end-only, which is what keeps it clean.
            aecResidualLeak += 0.05f * (instantLeak - aecResidualLeak);
            if (aecResidualLeak < 0.0001f) aecResidualLeak = 0.0001f;
            if (aecResidualLeak > 1.0f)    aecResidualLeak = 1.0f;
        }
    }

    if (!isFarEndActive) {
        // Slow minimum tracker for the microphone noise floor, used to shape
        // comfort noise so the far end never hears the channel go completely dead.
        if (errorEnergy < aecNoiseFloor) {
            aecNoiseFloor = errorEnergy;
        } else {
            aecNoiseFloor *= 1.002f;
        }
        if (aecNoiseFloor < 1.0f) aecNoiseFloor = 1.0f;
        // Without a ceiling this ramps by 1.002 per frame for as long as the near end
        // keeps talking while the far end listens, and the comfort noise injected later
        // ends up louder than the speech it is meant to hide.
        if (aecNoiseFloor > AEC_NOISE_FLOOR_MAX) aecNoiseFloor = AEC_NOISE_FLOOR_MAX;
    }

    /* ---- Residual echo suppression ---- */
    // Deciding whether the driver is talking is done AFTER the adaptive filter, not
    // before it. The pre-filter test above has to compare the raw microphone against the
    // raw echo, so a driver who is quieter than the speakers hides underneath it - that
    // was measured at about 10 dB of unwanted ducking on the near-end talker. The
    // adaptive filter has already stripped 20 dB or more of echo out of the residual, so
    // testing the residual against the expected leakage is far more sensitive, and the
    // near-end voice comes through the suppressor essentially untouched.
    //
    // Before the filter has converged the prediction is meaningless, so fall back to the
    // pre-filter test - during those first seconds hard suppression is the right answer
    // anyway, because the linear filter is not yet removing anything.
    // The residual test is only meaningful once the linear filter is actually cancelling
    // something. Until then aecResidualLeak is still at its pessimistic starting value and
    // the test would fire on everything, so the pre-filter test stands in.
    bool residualTestIsTrustworthy = (predictionEnergy > AEC_FAR_ACTIVE_FLOOR) &&
                                     (aecResidualLeak < AEC_LEAK_TRUST_THRESHOLD);

    bool isNearEndPresent;
    if (isFarEndActive && residualTestIsTrustworthy) {
        float expectedResidual = aecResidualLeak * predictionEnergy;
        if (errorEnergy > AEC_NEAREND_MARGIN * expectedResidual) {
            aecNearEndHangover = AEC_NEAREND_HANGOVER;
        }
        isNearEndPresent = (aecNearEndHangover > 0);
        if (aecNearEndHangover > 0) {
            aecNearEndHangover--;
        }
    } else {
        isNearEndPresent = isDoubleTalk;
        if (!isFarEndActive) {
            aecNearEndHangover = 0;
        }
    }

    float suppressionFloor = 1.0f;
    if (isFarEndActive) {
        suppressionFloor = isNearEndPresent ? AEC_SUPPRESS_FLOOR_DT : AEC_SUPPRESS_FLOOR;
    }

    float targetGain = 1.0f;
    if (isFarEndActive && errorEnergy > 0.0f) {
        // Wiener-style gain: how much of what is left is genuinely near-end audio
        // rather than residual echo.
        float residualEcho = aecResidualLeak * predictionEnergy;
        targetGain = (errorEnergy - residualEcho) / errorEnergy;
    }
    if (targetGain < suppressionFloor) targetGain = suppressionFloor;
    if (targetGain > 1.0f) targetGain = 1.0f;

    aecNearEndWasActive = isNearEndPresent;

    float previousGain = aecSuppressionGain;
    // Both directions are quick. Coming down fast kills echo bursts; going back up fast
    // means the front of the driver's next word is not clipped off.
    float smoothing = (targetGain < aecSuppressionGain) ? 0.55f : 0.40f;
    aecSuppressionGain += smoothing * (targetGain - aecSuppressionGain);

    if (previousGain < 0.999f || aecSuppressionGain < 0.999f) {
        float gainStep = (aecSuppressionGain - previousGain) / (float)sampleCount;
        float comfortAmplitude = sqrtf(aecNoiseFloor);

        for (uint32_t index = 0; index < sampleCount; index++) {
            float gain = previousGain + gainStep * (float)index;
            float shaped = microphoneSamples[index] * gain;

            float missing = 1.0f - gain;
            if (missing > 0.02f) {
                shaped += aecComfortNoise() * comfortAmplitude * missing;
            }
            microphoneSamples[index] = shaped;
        }
    }

    /* ---- Diagnostics ---- */
    aecFrameCounter++;
    if ((aecFrameCounter % AEC_LOG_INTERVAL_FRAMES) == 0) {
        float erleDb = 0.0f;
        if (errorEnergy > 0.0f && microphoneEnergy > 0.0f) {
            erleDb = 10.0f * log10f(microphoneEnergy / errorEnergy);
        }
        ESP_LOGI(TAG, "--- AEC | peak tap: %lu/%d | ERLE: %.1f dB | coupling: %.2f | leak: %.4f | lag: %ld ---",
                 (unsigned long)peakTap, AEC_FILTER_TAPS, erleDb,
                 aecEchoReturnLoss, aecResidualLeak, (long)lag);
    }
#endif
}

/* * ============================================================================
 * Non-Volatile Storage (NVS) Operations
 * ============================================================================
 */

/**
 * @brief Saves the MAC address of the most recently connected Bluetooth device to flash memory.
 * * Opens the NVS namespace, writes the blob data under the key 'last_bda', and commits 
 * the changes so the device can auto-reconnect if the car is turned off and back on.
 * * @param bluetoothAddress A 6-byte array containing the Bluetooth MAC address.
 */
void saveBluetoothAddressToNvs(esp_bd_addr_t bluetoothAddress) {
    nvs_handle_t nvsHandle;
    esp_err_t openResult = nvs_open(NVS_NAMESPACE_STRING, NVS_READWRITE, &nvsHandle);
    bool isOpenSuccess = (openResult == ESP_OK);

    if (isOpenSuccess) {
        esp_err_t setResult = nvs_set_blob(nvsHandle, NVS_KEY_BLUETOOTH_MAC, bluetoothAddress, sizeof(esp_bd_addr_t));
        bool isSetSuccess = (setResult == ESP_OK);

        if (isSetSuccess) {
            nvs_commit(nvsHandle);
            ESP_LOGI(TAG, "--- Saved Bluetooth MAC to NVS ---");
        }
        
        nvs_close(nvsHandle);
    } else {
        ESP_LOGE(TAG, "--- Error opening NVS to save MAC ---");
    }
}

/**
 * @brief Retrieves the saved MAC address from flash memory upon boot-up.
 * * @param bluetoothAddress A pre-allocated 6-byte array to store the retrieved MAC address.
 * @return true if a saved address was found, false otherwise.
 */
bool getBluetoothAddressFromNvs(esp_bd_addr_t bluetoothAddress) {
    nvs_handle_t nvsHandle;
    esp_err_t openResult = nvs_open(NVS_NAMESPACE_STRING, NVS_READONLY, &nvsHandle);
    bool isOpenSuccess = (openResult == ESP_OK);

    if (isOpenSuccess) {
        size_t requiredSize = sizeof(esp_bd_addr_t);
        esp_err_t getResult = nvs_get_blob(nvsHandle, NVS_KEY_BLUETOOTH_MAC, bluetoothAddress, &requiredSize);
        nvs_close(nvsHandle);

        bool isGetSuccess = (getResult == ESP_OK);
        if (isGetSuccess) {
            return true;
        }
    }
    return false;
}

/* * ============================================================================
 * Hardware & Audio Reconfiguration Operations
 * ============================================================================
 */

/**
 * @brief NEW: Builds the I2S Tx (speaker) channel with a given DMA geometry.
 * * Factored out of initializeI2sTransmitChannel() so the channel can be rebuilt with
 * call-sized DMA buffers when a call starts and music-sized buffers when it ends.
 * DMA depth is fixed at channel creation - i2s_channel_reconfig_std_clock() only
 * touches the clock - so a rebuild is the only way to change it.
 */
static void createTransmitChannel(uint32_t sampleRate, uint32_t descriptorCount, uint32_t framesPerDescriptor) {
    i2s_chan_config_t transmitConfiguration = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    transmitConfiguration.dma_desc_num = descriptorCount;
    transmitConfiguration.dma_frame_num = framesPerDescriptor;
    transmitConfiguration.auto_clear = true;
    i2s_new_channel(&transmitConfiguration, &txChannel, NULL);

    i2s_std_config_t transmitStandardConfiguration = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = DAC_I2S_BCLK,
            .ws   = DAC_I2S_WS,
            .dout = DAC_I2S_DATA,
            .din  = I2S_GPIO_UNUSED
        },
    };
    transmitStandardConfiguration.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    i2s_channel_init_std_mode(txChannel, &transmitStandardConfiguration);
    i2s_channel_enable(txChannel);
}

/**
 * @brief NEW: Tears down and rebuilds the Tx channel under a different DMA profile.
 * * Caller must already hold txMutex.
 */
static void switchTransmitProfile(TransmitDmaProfile profile, uint32_t sampleRate) {
    if (txChannel != NULL) {
        i2s_channel_disable(txChannel);
        i2s_del_channel(txChannel);
        txChannel = NULL;
    }

    if (profile == TX_PROFILE_CALL) {
        createTransmitChannel(sampleRate, CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC);
        ESP_LOGI(TAG, "--- Tx DMA -> CALL profile (%d x %d frames) ---",
                 CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC);
    } else {
        createTransmitChannel(sampleRate, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC);
        ESP_LOGI(TAG, "--- Tx DMA -> MUSIC profile (%d x %d frames) ---",
                 MUSIC_TX_DESC_COUNT, (int)MUSIC_TX_FRAMES_PER_DESC);
    }

    currentTransmitProfile = profile;
}

/**
 * @brief Dynamically reconfigures the I2S hardware clocks on the fly.
 * * Switches between Music (A2DP @ 44.1kHz), Standard Calls (HFP @ 8kHz), and HD Calls 
 * (HFP mSBC @ 16kHz). Disables both I2S channels, updates the clock config, recalculates 
 * DSP math, and re-enables the Tx (Speaker) channel. 
 * OPTIMIZATION: Only re-enables the Rx (Microphone) channel if a phone call is actually active.
 * NEW: also swaps the Tx DMA depth between the music and call profiles.
 * * @param targetSampleRate The desired frequency in Hz.
 */
void updateI2sSampleRate(uint32_t targetSampleRate) {
    bool isRateZero = (targetSampleRate == 0);
    bool isTxChannelNull = (txChannel == NULL);
    bool isRxChannelNull = (rxChannel == NULL);

    if (isRateZero || isTxChannelNull || isRxChannelNull) {
        return;
    }

    bool isPhoneCallActive = isAudioPathOpen;
    TransmitDmaProfile wantedProfile = isPhoneCallActive ? TX_PROFILE_CALL : TX_PROFILE_MUSIC;

    ESP_LOGI(TAG, "--- Switching Clock -> %lu Hz ---", targetSampleRate);

    xSemaphoreTake(txMutex, portMAX_DELAY);
    xSemaphoreTake(rxMutex, portMAX_DELAY);
    
    calculateFilterAlphas(targetSampleRate);

    // FIX: Only disable the mic if it's currently running. 
    // This prevents the "i2s_channel_disable: the channel has not been enabled yet" error.
    if (isMicHardwareEnabled) {
        i2s_channel_disable(rxChannel);
        isMicHardwareEnabled = false;
    }

    // FIX: Use Default PLL (I2S_CLK_SRC_DEFAULT), NOT I2S_CLK_SRC_APLL.
    // The APLL locks to a single frequency and refuses to switch from 44.1k to 16k, 
    // which caused the microphone to flood the Bluetooth stack and crash the system.
    i2s_std_clk_config_t clockConfiguration = I2S_STD_CLK_DEFAULT_CONFIG(targetSampleRate);
    clockConfiguration.clk_src = I2S_CLK_SRC_DEFAULT; 
    
    i2s_channel_reconfig_std_clock(rxChannel, &clockConfiguration);

    // NEW: rebuilding the Tx channel applies both the new DMA depth and the new clock,
    // so it replaces the disable/reconfig/enable dance when the profile changes.
    if (wantedProfile != currentTransmitProfile) {
        switchTransmitProfile(wantedProfile, targetSampleRate);
    } else {
        i2s_channel_disable(txChannel);
        i2s_channel_reconfig_std_clock(txChannel, &clockConfiguration);
        i2s_channel_enable(txChannel);
    }

    // OPTIMIZATION: Always enable TX (Speakers). Only enable RX (Mic DMA) if in a call!
    if (isPhoneCallActive) {
        i2s_channel_enable(rxChannel);
        isMicHardwareEnabled = true;
    }

    xSemaphoreGive(rxMutex);
    xSemaphoreGive(txMutex);
}

/* * ============================================================================
 * Callbacks: A2DP (Music)
 * ============================================================================
 */

/**
 * @brief Receives decoded PCM audio chunks from the Bluetooth stack when streaming music.
 * * Drops incoming audio into a2dpRingBuffer. Uses a non-blocking xRingbufferSend (wait time = 0) 
 * to prevent Bluetooth stack starvation. If the buffer is full, it drops the packet rather 
 * than blocking the CPU. Signals preRollSemaphore once enough audio builds up.
 * * @param audioData Pointer to the raw PCM bytes.
 * @param dataLength Size of the audio chunk in bytes.
 */
void bluetoothA2dpSinkDataCallback(const uint8_t *audioData, uint32_t dataLength) {
    if (isAudioPathOpen || a2dpRingBuffer == NULL) {
        return;
    }

    // OPTIMIZATION: Change timeout to 0 (non-blocking). If the ringbuffer is full, we must
    // drop the packet instantly rather than stalling the Bluedroid BT Stack.
    BaseType_t sendResult = xRingbufferSend(a2dpRingBuffer, (void *)audioData, dataLength, 0);
    
    bool isBufferFull = (sendResult != pdTRUE);

    if (isBufferFull) {
        return;
    }

    if (!isPlaying) {
        size_t currentFilledBytes = 0;
        vRingbufferGetInfo(a2dpRingBuffer, NULL, NULL, NULL, NULL, &currentFilledBytes);

        bool isPreRollSatisfied = (currentFilledBytes >= PRE_ROLL_THRESHOLD_BYTES);
        
        if (isPreRollSatisfied) {
            isPlaying = true;
            xSemaphoreGive(preRollSemaphore);
        }
    }
}

/**
 * @brief Handles A2DP control events (Connection, Audio Config).
 * * When connected, triggers saving the MAC address to NVS. When the SBC codec is 
 * negotiated, commands the I2S hardware to switch to 44.1kHz.
 * * @param event The A2DP callback event type.
 * @param parameter Pointer to event-specific parameters.
 */
void bluetoothAppA2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *parameter) {
    bool isConnectionStateEvent = (event == ESP_A2D_CONNECTION_STATE_EVT);
    bool isAudioConfigEvent = (event == ESP_A2D_AUDIO_CFG_EVT);

    if (isConnectionStateEvent) {
        bool isConnected = (parameter->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED);
        
        if (isConnected) {
            ESP_LOGI(TAG, "--- A2DP Device Connected ---");
            saveBluetoothAddressToNvs(parameter->conn_stat.remote_bda);
        }
    } else if (isAudioConfigEvent) {
        ESP_LOGI(TAG, "--- A2DP Configuration Event---");
        bool isSbcCodec = (parameter->audio_cfg.mcc.type == ESP_A2D_MCT_SBC);
        
        if (isSbcCodec) {
            updateI2sSampleRate(SAMPLE_RATE_44K);
        }
    }
}

/* * ============================================================================
 * Callbacks: Hands-Free Protocol (Phone Calls)
 * ============================================================================
 */

/**
 * @brief Receives audio from the remote caller to be played on the car speakers.
 * * 1. Mono-to-Stereo: Routes mono call samples based on the enableHfpStereoMode flag
 * and pushes them directly to the I2S Tx hardware.
 * * 2. NEW: hands the same mono samples to the echo canceller as its reference, but only
 * the ones the I2S channel actually accepted - anything dropped never reaches the
 * speaker, so counting it would skew the echo path alignment for the rest of the call.
 * * @param audioBuffer Pointer to the raw PCM bytes from the caller.
 * @param bufferLength Size of the audio chunk in bytes.
 */
void bluetoothHandsFreeReceiveCallback(const uint8_t *audioBuffer, uint32_t bufferLength) {
    size_t bytesWritten = 0;
    int16_t *pcm16Data = (int16_t *)audioBuffer;
    
    uint32_t numberOfSamples = bufferLength / 2;
    // Safe maximum buffer size based on standard HFP frames (see MAX_HFP_SAMPLES above)
    if (numberOfSamples > MAX_HFP_SAMPLES) {
        numberOfSamples = MAX_HFP_SAMPLES; // Prevent overrun
    }
    uint32_t stereoBufferSize = numberOfSamples * 2 * sizeof(int16_t);
    int16_t stereoBuffer[MAX_HFP_SAMPLES * 2]; // FIX: Fixed-size allocation

    for (uint32_t index = 0; index < numberOfSamples; index++) {
        uint32_t leftChannelIndex = index * 2;
        uint32_t rightChannelIndex = leftChannelIndex + 1;
        
        int16_t currentSample = pcm16Data[index];

        if (enableHfpStereoMode) {
            stereoBuffer[leftChannelIndex] = currentSample;
            stereoBuffer[rightChannelIndex] = currentSample;
        } else {
            stereoBuffer[leftChannelIndex] = 0;
            stereoBuffer[rightChannelIndex] = currentSample;
        }
    }

    xSemaphoreTake(txMutex, portMAX_DELAY);   
    // NEW: short timeout. With the call-mode DMA profile the whole Tx queue is only
    // ~15 ms deep, so a long block here would stall the Bluedroid callback thread for
    // far longer than the audio is worth.
    i2s_channel_write(txChannel, (void*)stereoBuffer, stereoBufferSize, &bytesWritten,
                      pdMS_TO_TICKS(CALL_TX_WRITE_TIMEOUT_MS));
    xSemaphoreGive(txMutex);

    // 4 bytes per stereo frame -> one mono reference sample each.
    uint32_t playedSamples = (uint32_t)bytesWritten / 4;
    if (playedSamples > 0) {
        echoCancellerPushReference(pcm16Data, playedSamples);
    }
}

/**
 * @brief Provides microphone data to the Bluetooth stack to transmit to the remote caller.
 * * Reads processed and filtered audio from the microphoneStreamBuffer 
 * and hands it to the Bluetooth radio.
 * * @param outputBuffer Pointer to the buffer where outgoing audio should be written.
 * @param bufferSize The number of bytes requested by the Bluetooth stack.
 * @return The actual number of bytes written to the output buffer.
 */
uint32_t bluetoothHandsFreeSendCallback(uint8_t *outputBuffer, uint32_t bufferSize) {
    if (microphoneStreamBuffer == NULL || !isAudioPathOpen) { 
        return 0; 
    }

    // STRICT FRAME ENFORCEMENT
    // Never return partial frames to the mSBC encoder. 
    if (xStreamBufferBytesAvailable(microphoneStreamBuffer) < bufferSize) {
        return 0;
    }

    size_t bytesRead = xStreamBufferReceive(microphoneStreamBuffer, (void *)outputBuffer, bufferSize, 0);

    // PREVENT AUDIO HALVING
    // Recursively alert stack if more valid audio is waiting.
    if (xStreamBufferBytesAvailable(microphoneStreamBuffer) >= bufferSize) {
        esp_hf_client_outgoing_data_ready();
    }

    return (uint32_t)bytesRead;
}

/**
 * @brief Manages the state machine of phone calls (Ringing, Connected, Hang up).
 * * When a call connects, checks if it is wideband (16kHz) or narrowband (8kHz), updates 
 * I2S hardware clock, and wakes up the sleeping microphoneTask via xTaskNotifyGive. 
 * When the call ends, switches back to 44.1kHz.
 * * @param event The HFP client callback event type.
 * @param parameter Pointer to event-specific parameters.
 */
void bluetoothHandsFreeClientCallback(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *parameter) {
    bool isAudioStateEvent = (event == ESP_HF_CLIENT_AUDIO_STATE_EVT);
    bool isCallEvent = (event == ESP_HF_CLIENT_CIND_CALL_EVT);
    bool isCallSetupEvent = (event == ESP_HF_CLIENT_CIND_CALL_SETUP_EVT);
    bool isConnectionStateEvent = (event == ESP_HF_CLIENT_CONNECTION_STATE_EVT);

    // --- Catch HFP link Connect/Disconnect ---
    // Caches the remote phone's address. Kept for logging/diagnostics and so any future
    // code that legitimately needs the peer address has the correct one available - but
    // note that this is deliberately NOT used to drive SCO setup (see CIND_CALL below).
    if (isConnectionStateEvent) {
        if (parameter->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED ||
            parameter->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            memcpy(hfpRemoteDeviceAddress, parameter->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            isHfpRemoteAddressValid = true;
            ESP_LOGI(TAG, "HFP: Link connected, remote address cached");
        } else if (parameter->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
            isHfpRemoteAddressValid = false;
        }
    }
    // --- Catch Incoming/Outgoing Ringing ---
    if (isCallSetupEvent) {
        // NONE means setup finished (call answered or rejected). 
        // Any other status (INCOMING, DIALING, ALERTING) means it's ringing.
        if (parameter->call_setup.status != ESP_HF_CALL_SETUP_STATUS_IDLE) {
            isCallActiveOrRinging = true;
            ESP_LOGI(TAG, "HFP: Phone is Ringing/Dialing");
        } else {
            ESP_LOGI(TAG, "HFP: Call Setup Finished");
        }
    } 
    // --- Catch Call Connect/Disconnect ---
    else if (isCallEvent) {
        if (parameter->call.status == ESP_HF_CALL_STATUS_NO_CALLS) {
            isCallActiveOrRinging = false;
            ESP_LOGI(TAG, "HFP: Call Ended/Disconnected");
        } 
        else if (parameter->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            isCallActiveOrRinging = true;
            ESP_LOGI(TAG, "HFP: Call is Active");
            // NOTE: Deliberately do NOT call esp_hf_client_connect_audio() here.
            //
            // REGRESSION FIX: In the original code this call was passed
            // esp_bt_dev_get_address() - this device's OWN address - so the stack
            // rejected it and it was a dead no-op. The audio path that actually
            // worked was always the one the phone (Audio Gateway) sets up itself;
            // the AG owns SCO/eSCO establishment in HFP, and the HF unit is not
            // supposed to race it.
            //
            // Passing the correct remote address turned that dormant no-op into a
            // real, redundant SCO connect request issued at CIND_CALL time - which
            // arrives while the AG is already establishing (or has just established)
            // its own SCO link. The AG responds by tearing the link down and
            // renegotiating, which fires ESP_HF_CLIENT_AUDIO_STATE_EVT with a
            // DISCONNECTED state mid-call. That handler sets isAudioPathOpen = false
            // and calls updateI2sSampleRate(SAMPLE_RATE_44K), so:
            //   - the speaker ends up clocked at 44.1kHz while 8/16kHz call audio is
            //     still being written to it -> playback sounds sped up and distorted
            //   - isAudioPathOpen == false makes bluetoothHandsFreeSendCallback()
            //     return 0 unconditionally, and updateI2sSampleRate() disables the
            //     RX DMA channel -> complete microphone silence
            // Both reported symptoms, one cause.
        }
    } else if (isAudioStateEvent) {
        bool isNarrowBandConnected = (parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED);
        bool isWideBandConnected = (parameter->audio_stat.state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC);
        bool isAnyConnectionActive = (isNarrowBandConnected || isWideBandConnected);

        if (isAnyConnectionActive) {
            isAudioPathOpen = true;
            isPlaying = false;

            uint32_t activeCallSampleRate = SAMPLE_RATE_8K;
            if (isWideBandConnected) {
                activeCallSampleRate = SAMPLE_RATE_16K;
            }

            ESP_LOGI(TAG, "--- Call Connected (%s) ---", isWideBandConnected ? "mSBC 16k" : "CVSD 8k");

            // NEW: stop advertising and stop accepting new pages for the duration of the
            // call. Inquiry scan and page scan steal radio slots from the reserved eSCO
            // windows, which shows up as jitter, retransmissions and extra latency on a
            // link that has no elasticity to absorb them.
            esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);

            // NEW: clear all echo canceller state before any audio moves.
            echoCancellerReset();

            // Explicitly flush stream buffer immediately on connection 
            // to ensure absolute purity of audio data going to the encoder
            if (microphoneStreamBuffer != NULL) {
                xStreamBufferReset(microphoneStreamBuffer);
            }

            updateI2sSampleRate(activeCallSampleRate); // Explicitly ENABLES the RX DMA buffer
            
            // OPTIMIZATION: Instantly wake up the dormant Microphone CPU Task via notification
            if (micTaskHandle != NULL) {
                xTaskNotifyGive(micTaskHandle);
            }
            
        } else {
            isAudioPathOpen = false;
            isPlaying = false;
            isCallActiveOrRinging = false; // Fallback sync on audio disconnect
            aecIsPrimed = false;
            ESP_LOGI(TAG, "--- Call Ended ---");
            updateI2sSampleRate(SAMPLE_RATE_44K); // Explicitly DISABLES the RX DMA buffer

            // NEW: become visible again now that the SCO link is gone.
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
    }
}

/* * ============================================================================
 * Background Tasks
 * ============================================================================
 */

 // Handles TTP224 inputs and sends AVRCP control commands
void touchPanelTask(void *taskParameters) {
    bool previousPad1 = false, previousPad2 = false, previousPad3 = false, previousPad4 = false;

    while (true) {
        bool currentPad1 = gpio_get_level(TOUCH_PAD_PAUSE);
        bool currentPad2 = gpio_get_level(TOUCH_PAD_NEXT_TRACK);
        bool currentPad3 = gpio_get_level(TOUCH_PAD_PREV_TRACK);
        bool currentPad4 = gpio_get_level(TOUCH_PAD_PLAY);

        // Pad 1: Pause / End Call
        if (currentPad1 && !previousPad1) {

            if (isCallActiveOrRinging) {
                // 1. If a call is active or ringing, end it.
                // This sends the AT+CHUP command to hang up or reject the call.
                esp_hf_client_reject_call();
                ESP_LOGI(TAG, "--- HFP: Call Ended/Rejected via Touch Pad ---");
            } else {
                esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_PRESSED);
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STATE_RELEASED);
                ESP_LOGI(TAG, "--- AVRCP: Pausing ---");
            }
        }

        // Pad 2: Next Track
        if (currentPad2 && !previousPad2) {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
            ESP_LOGI(TAG, "--- AVRCP: Next Track ---");
        }

        // Pad 3: Previous Track
        if (currentPad3 && !previousPad3) {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_PRESSED);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_BACKWARD, ESP_AVRC_PT_CMD_STATE_RELEASED);
            ESP_LOGI(TAG, "--- AVRCP: Previous Track ---");
        }

        // Pad 4: Play
        if (currentPad4 && !previousPad4) {
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_PRESSED);
            vTaskDelay(pdMS_TO_TICKS(50));
            esp_avrc_ct_send_passthrough_cmd(0, ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_STATE_RELEASED);
            ESP_LOGI(TAG, "--- AVRCP: Playing ---");
        }

        previousPad1 = currentPad1;
        previousPad2 = currentPad2;
        previousPad3 = currentPad3;
        previousPad4 = currentPad4;

        // Polling interval acts as a natural hardware debounce
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}

/**
 * @brief Dedicated CPU thread that continuously feeds the I2S Tx (Speaker) DMA hardware.
 * * If a call is active, it yields (calls bypass the ringbuffer). If playing A2DP music, 
 * waits for preRollSemaphore to ensure a deep buffer is filled before starting. 
 * Pulls data from a2dpRingBuffer and writes it to the txChannel.
 * * @param taskParameters FreeRTOS task parameters (unused).
 */
void audioOutputTask(void *taskParameters) {
    size_t itemSize = 0;

    while (true) {
        if (isAudioPathOpen) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!isPlaying) {
            xSemaphoreTake(preRollSemaphore, portMAX_DELAY);
        }

        uint8_t *dataItem = (uint8_t *)xRingbufferReceive(a2dpRingBuffer, &itemSize, pdMS_TO_TICKS(100));
        bool hasData = (dataItem != NULL);

        if (hasData) {
            size_t bytesWritten = 0;
            
            xSemaphoreTake(txMutex, portMAX_DELAY);
            // FIX: Change portMAX_DELAY to 500ms to prevent thread lock
            i2s_channel_write(txChannel, dataItem, itemSize, &bytesWritten, pdMS_TO_TICKS(500));
            xSemaphoreGive(txMutex);

            vRingbufferReturnItem(a2dpRingBuffer, (void *)dataItem);
        } else {
            // Buffer underflow occurred! 
            ESP_LOGW(TAG, "--- Audio Out Buffer Underflow! Re-buffering... ---");
            isPlaying = false;
        }
    }
}

/**
 * @brief Dedicated CPU thread that reads, processes, and queues Microphone data.
 * * 1. CPU Sleep: If no call is active, sleeps indefinitely (0% CPU) via ulTaskNotifyTake.
 * 2. DSP Pipeline: Reads raw 32-bit hardware samples, applies High-Pass (removes rumble) 
 * and Low-Pass (removes hiss) filters, cancels loudspeaker echo, applies gain and hard
 * clipping, and converts to 16-bit.
 * 3. Queue: Pushes final audio to microphoneStreamBuffer, holding the backlog low so the
 * uplink does not accumulate delay.
 * * NOTE ON ORDERING: the echo canceller sits AFTER the HPF/LPF and BEFORE the gain and
 * the clipper. The HPF/LPF are linear and time-invariant, so the adaptive filter simply
 * learns them as part of the echo path - and having DC and road rumble already removed
 * helps it converge. The gain is a constant the filter also absorbs. The clipper must
 * stay last, because a non-linearity in front of the canceller is exactly the thing a
 * linear canceller cannot model.
 * * @param taskParameters FreeRTOS task parameters (unused).
 */
void microphoneTask(void *taskParameters) {
    size_t bytesReadFromHardware;
    
    int32_t rawHardwareData[DMA_FRAME_SIZE_SAMPLES];
    int16_t processedVoiceBuffer[DMA_FRAME_SIZE_SAMPLES];
    float   linearVoiceBuffer[DMA_FRAME_SIZE_SAMPLES];

    float lowPassFilterState = 0.0f;
    float previousHighPassOutput = 0.0f;
    int32_t previousHighPassInput = 0;

    while (true) {
        if (!isAudioPathOpen) {
            // OPTIMIZATION: Put this task to sleep INDEFINITELY when not in a call.
            lowPassFilterState = 0.0f;
            previousHighPassInput = 0;
            previousHighPassOutput = 0.0f;

            bool isStreamBufferValid = (microphoneStreamBuffer != NULL);
            if (isStreamBufferValid) {
                xStreamBufferReset(microphoneStreamBuffer);
            }

            // Blocks task without eating CPU. Awakened by xTaskNotifyGive in BT HFP Callback.
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY); 

            // Short settle delay so the baseband has the physical link up. Was 250 ms,
            // which threw away a quarter second of the near end's first words.
            vTaskDelay(pdMS_TO_TICKS(HFP_MIC_STARTUP_DELAY_MS));

            // ABSOLUTE DMA DRAIN
            size_t dummyBytes;
            int drainLimit = 20; // FIX: Prevent infinite loops
            
            xSemaphoreTake(rxMutex, portMAX_DELAY);
            while (i2s_channel_read(rxChannel, rawHardwareData, sizeof(rawHardwareData), &dummyBytes, 0) == ESP_OK && dummyBytes > 0 && drainLimit > 0) {
                drainLimit--; // FIX: Decrement limit
            }
            xSemaphoreGive(rxMutex);

            // Ensure the software stream buffer is completely fresh before starting
            if (microphoneStreamBuffer != NULL) {
                xStreamBufferReset(microphoneStreamBuffer);
            }

            // NEW: the microphone is about to capture its first live sample, so this is
            // the moment that defines the reference/microphone alignment. Everything the
            // echo canceller does hangs off this call.
            echoCancellerAnchor();

            // Proceed directly into the active read loop now that the pipeline is clean
            continue; 
        }

        xSemaphoreTake(rxMutex, portMAX_DELAY);
        esp_err_t readResult = i2s_channel_read(rxChannel, rawHardwareData, sizeof(rawHardwareData), &bytesReadFromHardware, pdMS_TO_TICKS(100));
        
        // FIX: Safely cache the dynamic alphas to local variables while the mutex is held
        float localHighPassAlpha = activeHighPassAlpha;
        float localLowPassAlpha = activeLowPassAlpha;
        
        xSemaphoreGive(rxMutex);

        bool isReadSuccessful = (readResult == ESP_OK);
        bool hasBytes = (bytesReadFromHardware > 0);

        if (isReadSuccessful && hasBytes) {
            uint32_t numberOfSamples = bytesReadFromHardware / 4; 

            // --- Stage 1: linear conditioning (unchanged maths, float output) ---
            for (uint32_t index = 0; index < numberOfSamples; index++) {
                
                int32_t rawSampleData = rawHardwareData[index];
                int32_t shiftedSampleData = rawSampleData >> 16;
                int32_t currentInputSample = shiftedSampleData;

                float currentInputFloat = (float)currentInputSample;
                float previousInputFloat = (float)previousHighPassInput;
                
                float inputDifference = currentInputFloat - previousInputFloat;
                float accumulatedFilterState = previousHighPassOutput + inputDifference;
                
                // FIX: Use the cached thread-safe variables for math
                float highPassResult = localHighPassAlpha * accumulatedFilterState;

                previousHighPassInput = currentInputSample;
                previousHighPassOutput = highPassResult;

                // FIX: Use the cached thread-safe variables for math
                float lowPassComponentOne = localLowPassAlpha * highPassResult;
                float inverseLowPassAlpha = 1.0f - localLowPassAlpha;
                float lowPassComponentTwo = inverseLowPassAlpha * lowPassFilterState;
                
                float lowPassResult = lowPassComponentOne + lowPassComponentTwo;
                lowPassFilterState = lowPassResult;

                linearVoiceBuffer[index] = lowPassResult;
            }

            // --- Stage 2: NEW - remove the car speakers from the microphone signal ---
            echoCancellerProcessFrame(linearVoiceBuffer, numberOfSamples);

            // --- Stage 3: gain, clip, convert to 16-bit ---
            for (uint32_t index = 0; index < numberOfSamples; index++) {
                float amplifiedResult = linearVoiceBuffer[index] * MICROPHONE_GAIN;

                // Clamp in the FLOAT domain, before the conversion. Converting a NaN or
                // an out-of-range float to int32_t is undefined behaviour, and on Xtensa
                // it produces a value that the old integer-side clamps could not
                // recognise - a single bad sample became a constant, which the far end
                // hears as dead silence rather than as a glitch. The compound test below
                // is false for NaN as well as for out-of-range values, so both land in
                // the recovery branch, and NaN (neither > 0 nor < 0) resolves to zero.
                if (!(amplifiedResult >= -32768.0f && amplifiedResult <= 32767.0f)) {
                    amplifiedResult = (amplifiedResult > 0.0f) ?  32767.0f
                                    : (amplifiedResult < 0.0f) ? -32768.0f
                                    : 0.0f;
                }

                processedVoiceBuffer[index] = (int16_t)amplifiedResult;
            }

            uint32_t bufferPayloadSizeBytes = numberOfSamples * sizeof(int16_t);

            // NEW: hold the uplink backlog at a fixed low water mark instead of waiting
            // for the 2 KB stream buffer to fill. Whatever sits in this buffer is pure
            // added mouth-to-ear delay on the near-end talker, and the original code let
            // any startup backlog persist for the entire call. Dropping the OLDEST whole
            // frames keeps sample alignment intact and always favours fresh audio.
            //
            // rawHardwareData is reused as the discard sink - its frame has already been
            // converted by this point, and it is comfortably larger than one payload.
            size_t backlogBytes = xStreamBufferBytesAvailable(microphoneStreamBuffer);
            while ((backlogBytes + bufferPayloadSizeBytes) > HFP_UPLINK_MAX_BACKLOG_BYTES &&
                   backlogBytes >= bufferPayloadSizeBytes) {
                xStreamBufferReceive(microphoneStreamBuffer, (void *)rawHardwareData, bufferPayloadSizeBytes, 0);
                backlogBytes = xStreamBufferBytesAvailable(microphoneStreamBuffer);
            }

            // Ensure the whole frame fits before committing it. xStreamBufferSend() with a
            // 0 timeout will otherwise accept a PARTIAL write, and because this is an
            // unframed byte stream a partial write of a 16-bit-sample chunk permanently
            // shifts sample alignment for everything after it (heard as garbling until
            // the next call start/end resets the buffer).
            size_t availableSpaceBytes = xStreamBufferSpacesAvailable(microphoneStreamBuffer);

            if (availableSpaceBytes >= bufferPayloadSizeBytes) {
                xStreamBufferSend(microphoneStreamBuffer, (void *)processedVoiceBuffer, bufferPayloadSizeBytes, 0);
            }

            // Always poke the stack, even if we had to drop this frame. Skipping this is
            // what stalled the pipeline permanently.
            esp_hf_client_outgoing_data_ready();
        }
    }
}

/* * ============================================================================
 * Initialization Routines
 * ============================================================================
 */

 void initializeTouchPanel(void) {
    gpio_config_t touchPanelConfiguration = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << TOUCH_PAD_PAUSE) | 
                        (1ULL << TOUCH_PAD_NEXT_TRACK) | 
                        (1ULL << TOUCH_PAD_PREV_TRACK) | 
                        (1ULL << TOUCH_PAD_PLAY),
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE
    };
    gpio_config(&touchPanelConfiguration);
}

/**
 * @brief Sets up the I2S hardware driver for the Digital-to-Analog Converter (Speakers).
 * * Uses an expanded DMA buffer size to prevent audio popping if the CPU 
 * is momentarily busy. Configures pins 23, 26, and 25.
 */
void initializeI2sTransmitChannel(void) {
    createTransmitChannel(SAMPLE_RATE_44K, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC);
    currentTransmitProfile = TX_PROFILE_MUSIC;
}

/**
 * @brief Sets up the I2S hardware driver for the microphone ADC.
 * * Configures hardware to read 32-bit mono data. NOTE: i2s_channel_enable is omitted 
 * here by design to keep the DMA peripheral powered off to save bandwidth until a call starts.
 * Configures pins 21, 32, and 34.
 */
void initializeI2sReceiveChannel(void) {
    i2s_chan_config_t receiveConfiguration = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    receiveConfiguration.dma_desc_num = 3; 
    receiveConfiguration.dma_frame_num = DMA_FRAME_SIZE_SAMPLES;
    receiveConfiguration.auto_clear = true; 
    i2s_new_channel(&receiveConfiguration, NULL, &rxChannel);
    i2s_std_config_t receiveStandardConfiguration = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_16K),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode      = I2S_SLOT_MODE_MONO,
            .slot_mask      = I2S_STD_SLOT_LEFT,
            .ws_width       = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol         = false,
            .bit_shift      = true,
            .msb_right      = false
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_I2S_BCLK,   
            .ws   = MIC_I2S_WS,      
            .dout = I2S_GPIO_UNUSED,     
            .din  = MIC_I2S_DATA
        },
    };
    receiveStandardConfiguration.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT; // FIX: Use Default PLL to allow dynamic clock switching
    i2s_channel_init_std_mode(rxChannel, &receiveStandardConfiguration);
}

/**
 * @brief Bootstraps the Espressif Bluedroid Bluetooth stack.
 * * Initializes controller, enables Classic Bluetooth, sets device name, registers 
 * A2DP and HFP callbacks, and makes the device discoverable for pairing.
 */
void initializeBluetoothStack(void) {
    esp_bt_controller_config_t bluetoothControllerConfiguration = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bt_controller_init(&bluetoothControllerConfiguration);
    esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);

    esp_bluedroid_init();
    esp_bluedroid_enable();

    esp_bt_gap_set_device_name(TAG);

    // Initialize AVRCP Controller
    esp_avrc_ct_init();

    esp_hf_client_register_callback(bluetoothHandsFreeClientCallback);
    esp_hf_client_register_data_callback(bluetoothHandsFreeReceiveCallback, bluetoothHandsFreeSendCallback);
    esp_hf_client_init();

    esp_a2d_register_callback(bluetoothAppA2dpCallback);
    esp_a2d_sink_register_data_callback(bluetoothA2dpSinkDataCallback);
    esp_a2d_sink_init();
    
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

/* * ============================================================================
 * Application Entry
 * ============================================================================
 */

/**
 * @brief The standard main entry point for ESP-IDF applications.
 * * Initializes NVS (Flash), creates OS primitives (Mutexes, Semaphores, Buffers), 
 * triggers hardware and BT initialization, spins up core audio tasks, and attempts 
 * to auto-connect to the last paired phone.
 */
void app_main(void) {
    // FIX: Standard erase-and-retry handling. If the NVS partition has no free pages
    // (common after many write cycles) or was formatted by a different NVS library
    // version (common after an OTA/firmware update), nvs_flash_init() fails and every
    // subsequent nvs_open() call in this file will silently fail too - Bluetooth MAC
    // auto-reconnect would quietly stop working with no visible error.
    esp_err_t nvsInitResult = nvs_flash_init();
    if (nvsInitResult == ESP_ERR_NVS_NO_FREE_PAGES || nvsInitResult == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvsInitResult = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvsInitResult);

    txMutex = xSemaphoreCreateMutex();
    rxMutex = xSemaphoreCreateMutex();
    preRollSemaphore = xSemaphoreCreateBinary();

    microphoneStreamBuffer = xStreamBufferCreate(STREAM_BUFFER_SIZE_BYTES, TRIGGER_LEVEL_BYTES);
    a2dpRingBuffer = xRingbufferCreate(RING_BUFFER_SIZE_BYTES, RINGBUF_TYPE_BYTEBUF);

    // FIX: These allocations can fail under memory pressure. Every mutex/buffer here is
    // relied on with no further NULL checks anywhere else in the file (e.g.
    // xSemaphoreTake(txMutex, ...) on a NULL handle), so a silent allocation failure here
    // would surface later as a hard-to-diagnose crash deep in an ISR/callback context.
    // Fail loudly and immediately instead.
    if (txMutex == NULL || rxMutex == NULL || preRollSemaphore == NULL ||
        microphoneStreamBuffer == NULL || a2dpRingBuffer == NULL) {
        ESP_LOGE(TAG, "--- FATAL: Failed to allocate one or more OS primitives, halting ---");
        abort();
    }

    initializeTouchPanel();

    initializeI2sTransmitChannel();
    initializeI2sReceiveChannel();
    
    calculateFilterAlphas(SAMPLE_RATE_16K);

    initializeBluetoothStack();
     
    // Start Tasks
    // NEW: microphoneTask stack raised from 4096 - it now carries an extra float frame
    // buffer and runs the echo canceller's float maths.
    xTaskCreatePinnedToCore(touchPanelTask, "touchPanelTask", 2048, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(microphoneTask, "microphoneTask", 6144, NULL, 10, &micTaskHandle, 1);
    xTaskCreatePinnedToCore(audioOutputTask, "audioOutputTask", 4096, NULL, 15, NULL, 1);

    esp_bd_addr_t lastConnectedAddress;
    bool hasSavedDevice = getBluetoothAddressFromNvs(lastConnectedAddress);

    if (hasSavedDevice) {
        ESP_LOGI(TAG, "--- Found saved device in NVS, attempting to reconnect... ---");
        vTaskDelay(pdMS_TO_TICKS(1000)); 
        esp_a2d_sink_connect(lastConnectedAddress);
    } else {
        ESP_LOGI(TAG, "--- No saved device found. Waiting for pairing... ---");
    }
}
