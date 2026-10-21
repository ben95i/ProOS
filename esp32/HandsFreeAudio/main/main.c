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
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "speex/speex_echo.h"
#include "speex/speex_preprocess.h"
#include "speexdsp_alloc.h"

// --- Constants ---
static const char *TAG = "Honda_HR-V";

// --- Touch Panel Pins ---
#define TOUCH_PAD_PLAY       GPIO_NUM_18
#define TOUCH_PAD_PAUSE      GPIO_NUM_4
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

// Samples per mic DMA frame, and per echo canceller frame: 7.5 ms at 16 kHz, 15 ms at
// 8 kHz. A macro rather than a const so it can size static arrays.
#define DMA_FRAME_SIZE_SAMPLES 120u

// OPTIMIZATION: Deep Buffering for A2DP Crackle Prevention
static const uint32_t RING_BUFFER_SIZE_BYTES = 40 * 1024;     
static const uint32_t PRE_ROLL_THRESHOLD_BYTES = 20 * 1024;   
// Uplink queue to Bluedroid. The backlog trim in microphoneTask keeps it at or under
// HFP_UPLINK_MAX_BACKLOG_BYTES (480), so 1 KB is double what it ever holds.
static const uint32_t STREAM_BUFFER_SIZE_BYTES = 1024;
static const uint32_t TRIGGER_LEVEL_BYTES = 128;

static const float MICROPHONE_GAIN = 2.0f;

// FIX: Moved out of bluetoothHandsFreeReceiveCallback(). A #define inside a function
// body is not scoped to that function in C - it leaks into the rest of the translation
// unit for every line that follows. Keeping it here makes that explicit and avoids any
// future accidental redefinition/clash.
// Largest call frame the receive callback takes, in samples. HFP delivers at most
// 120 (one 7.5 ms mSBC frame; CVSD frames are 60), so 240 is twice the largest. The
// stereo copy of it lives on the Bluedroid task's stack: 240 x 2 x 2 = 960 bytes.
#define MAX_HFP_SAMPLES 240

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
#define CALL_TX_DESC_COUNT         6
#define CALL_TX_FRAMES_PER_DESC    60
// Descriptors of silence preloaded before the clock starts. Without this the queue has
// no cushion at all: the SCO source and the I2S sink are rate-matched, so the queue level
// is fixed by whatever it happens to be at call start and can only ever ratchet DOWN as
// jitter eats into it - the producer never gets a chance to run ahead and refill. Measured
// on the simulator, the queue sat empty for 79% of all descriptor fetches, which means the
// caller's voice was shot through with 3.75 ms gaps of auto-cleared silence. Two
// descriptors is 7.5 ms of headroom at 16 kHz, still an order of magnitude below the
// 360 ms the original music-sized buffers imposed.
#define CALL_TX_PRELOAD_DESC       2

// Writes during a call must never wedge the Bluedroid callback thread. A long timeout
// here is worse than dropping a frame - dropped audio is 3.75 ms of silence, a wedged
// BT task is a dead call.
#define CALL_TX_WRITE_TIMEOUT_MS   20

/* * ============================================================================
 * HFP uplink latency control
 * ============================================================================
 *
 * microphoneStreamBuffer was 2048 bytes = 1024 samples = 64 ms at 16 kHz. The original
 * code only discarded audio once that was completely full, so whatever backlog built up
 * at call start stayed there for the rest of the call and was added to the uplink delay.
 * The backlog is now held to a fixed low water mark, discarding the OLDEST whole frames.
 */
#define HFP_UPLINK_MAX_BACKLOG_BYTES  480   // 2 mic frames

// The original 250 ms settle delay costs a quarter of a second of the near-end
// talker's first words. 80 ms is enough for the baseband to settle.
#define HFP_MIC_STARTUP_DELAY_MS      80

// Uplink poke back-off. Each microphone frame ends with
// esp_hf_client_outgoing_data_ready(), and Bluedroid answers by calling
// bluetoothHandsFreeSendCallback(). When the phone ends a call, the SCO link closes
// some time before ESP_HF_CLIENT_AUDIO_STATE_EVT reports it, and every poke in that
// gap fails inside Bluedroid with "bta_hf_client_sco_co_out_data invalid air mode:
// 255". Each of those error lines blocks the Bluetooth task on the UART for a few
// ms, which delays the event further, which allows more pokes: one call end logged
// over 100 of them. So once this many pokes in a row have gone unanswered (60 ms at
// 16 kHz), only every UPLINK_BACKOFF_POKE_INTERVAL-th frame pokes (every 120 ms).
// One answered poke ends the back-off, so a live link loses at most one interval.
// Overridable so the simulator can compare runs with the back-off effectively off:
// make -C sim run S=4 CFLAGS="-O2 -g -DUPLINK_UNANSWERED_POKE_LIMIT=1000000000"
#ifndef UPLINK_UNANSWERED_POKE_LIMIT
#define UPLINK_UNANSWERED_POKE_LIMIT  8
#endif
#define UPLINK_BACKOFF_POKE_INTERVAL  16

/* * ============================================================================
 * Voice-switched gating (half-duplex echo suppression)  -- TUNING BLOCK
 * ============================================================================
 *
 * WHAT THIS IS
 * ------------
 * This is NOT an echo canceller. It does not model the acoustic path and it does
 * not subtract anything. It simply turns the uplink DOWN whenever the far end is
 * talking and the near end is not, so the echo never leaves the box. It cannot
 * diverge, it needs no delay alignment, and it costs about 1% of one core.
 *
 * The price is that the link is half-duplex while the far end is speaking: you
 * cannot interrupt them, and your first syllable after they stop is clipped by
 * whatever AEC_DUCK_RECOVERY_MS is set to.
 *
 * HOW THE DECISION IS MADE, once per microphone frame (7.5 ms at 16 kHz):
 *
 *   farEnvelope  = smoothed level of what we just sent to the DAC
 *   micEnvelope  = smoothed level of the conditioned microphone frame
 *   echoExpected = farEnvelope * AEC_ECHO_COUPLING_DB
 *
 *   far end is talking      <=>  farEnvelope > AEC_FAR_ACTIVE_LEVEL
 *   near end is talking too <=>  micEnvelope > echoExpected * AEC_NEAREND_MARGIN
 *
 *   duck the uplink to AEC_DUCK_DEPTH_DB  <=>  far talking AND near end not talking
 *
 * There is deliberately no delay line aligning the reference to the microphone.
 * The acoustic loop delay here is roughly 20-50 ms (TX DMA queue + DAC + amp +
 * flight time + mic group delay + RX DMA), and AEC_FAR_RELEASE_MS is set long
 * enough to simply span it: the far-end envelope is still elevated when the echo
 * it caused finally arrives. That is why the release is slow and the attack fast.
 *
 * TUNING
 * ------
 * The values below were tuned in the car. The tuning log and calibration
 * assistant that measured them were removed once the gate was scheduled for
 * replacement by SpeexDSP echo cancellation (AEC-PLAN.md); they are in git history.
 */

// Which echo handling the uplink uses (see "SpeexDSP echo cancellation" below):
//   ECHO_MODE_NONE   straight microphone path, for A/B comparison
//   ECHO_MODE_GATE   this voice-switched gate (the old behaviour)
//   ECHO_MODE_SPEEX  SpeexDSP adaptive echo cancellation with residual suppression
// Overridable from the build, e.g. the simulator's CFLAGS="-DECHO_MODE=1".
#define ECHO_MODE_NONE                0
#define ECHO_MODE_GATE                1
#define ECHO_MODE_SPEEX               2
#ifndef ECHO_MODE
#define ECHO_MODE                     ECHO_MODE_SPEEX
#endif
#define AEC_GATE_ENABLED              (ECHO_MODE == ECHO_MODE_GATE)
#define AEC_SPEEX_ENABLED             (ECHO_MODE == ECHO_MODE_SPEEX)

// Far-end level, in mean |sample| units (16-bit full scale = 32767), above which
// the far end counts as talking. Too low and the gate latches shut on line noise;
// too high and quiet talkers get no suppression.
#define AEC_FAR_ACTIVE_LEVEL          500.0f

// Speaker-to-microphone coupling, in dB, MEASURED AT THE POINT THE GATE SEES IT -
// that is, after MICROPHONE_GAIN has already been applied. It is NOT the raw
// acoustic coupling, and it is not necessarily negative.
//
// -20 dB is the value tuned for this car. The untuned starting point was 0.0 dB,
// the arithmetic of this build: a car cabin with the mic a metre or so off-axis
// from the speaker is roughly -6 dB of acoustic coupling, and MICROPHONE_GAIN of
// 2.0 puts +6 dB straight back on. Change MICROPHONE_GAIN and this has to move
// with it, dB for dB.
//
// Higher = the gate assumes a stronger echo and stays shut more readily.
#define AEC_ECHO_COUPLING_DB          (-20.0f)

// How far the microphone must exceed the expected echo level, in dB, before the
// gate accepts it as genuine near-end speech and opens back up.
// Higher = more echo suppression, more half-duplex.
#define AEC_NEAREND_MARGIN            6.0f

// How far down the uplink is pulled while ducked, in dB. Negative.
#define AEC_DUCK_DEPTH_DB             (-32.0f)

// Far-end envelope tracking. Fast attack so the gate shuts before the echo can
// arrive; slow release so it stays shut across the whole 20-50 ms acoustic loop
// delay plus the natural gaps between words. RELEASE is the de facto hangover
// timer - if you hear a short burst of echo at the end of each of their
// sentences, this is the constant to raise.
#define AEC_FAR_ATTACK_MS             2.0f
#define AEC_FAR_RELEASE_MS            180.0f

// Microphone envelope smoothing. Short enough to catch the onset of your own
// voice, long enough not to trigger on a single door slam.
#define AEC_MIC_ENVELOPE_MS           15.0f

// How quickly the uplink gain glides between open and ducked. The glide is
// interpolated per sample across the frame, so these can be short without
// clicking. ONSET should stay fast (the echo is already in flight); RECOVERY is
// what clips the first syllable of your reply, so keep it as short as you can
// stand before the tail of their echo starts leaking through.
#define AEC_DUCK_ONSET_MS             8.0f
#define AEC_DUCK_RECOVERY_MS          120.0f

// Optional fixed trim on the call-audio path to the DAC, in dB. Negative. This is
// the cheapest echo reduction available: every dB off the speaker is a dB off the
// echo, at zero CPU cost. 0.0 disables it. Applied in every ECHO_MODE. With the gate,
// lowering this changes the real coupling, so AEC_ECHO_COUPLING_DB has to move with it.
#define AEC_SPEAKER_ATTENUATION_DB    -2.0f

/* * ============================================================================
 * SpeexDSP echo cancellation (ECHO_MODE_SPEEX)
 * ============================================================================
 *
 * An adaptive filter learns the path from the speaker to the microphone and
 * subtracts the predicted echo from every microphone frame; SpeexDSP's
 * preprocessor then suppresses what is left and the road noise. Nothing to tune:
 * it re-learns continuously, so head unit volume, caller loudness and codec
 * changes need no setting. Design and measurements: AEC-PLAN.md.
 *
 * THE REFERENCE. The canceller needs exactly what the DAC played, sample-aligned
 * with the microphone. speakerSentCallback() (the TX channel's on_sent interrupt)
 * copies each call-audio descriptor as it finishes playing into referenceRing,
 * including the silence of an underrun. TX (I2S0) and RX (I2S1) run from the same
 * PLL at the same rate, so once aligned the two never drift. At call start, and
 * after a codec change, the mic task empties the RX queue and starts reading the
 * ring AEC_REFERENCE_LEAD_SAMPLES behind its newest sample, so the reference always
 * leads the echo it caused. An RX queue overflow drops one 120-sample mic frame;
 * micOverflowCallback() counts it and the same amount of reference is skipped.
 *
 * PER FRAME (microphoneTask): high-pass and low-pass as before, to int16 WITHOUT
 * MICROPHONE_GAIN (so clipping cannot make the echo non-linear), then
 * speex_echo_cancellation(), speex_preprocess_run(), then MICROPHONE_GAIN. Frames
 * are 120 samples at both rates, so no buffering or delay is added.
 *
 * MEMORY. The states are created by the mic task when call audio starts and
 * destroyed when it ends (~58 KB at either rate, measured by sim/speexcheck), so
 * music keeps all its RAM. Creation is skipped, and the call carries on without
 * cancellation, if the heap could not spare that and keep AEC_HEAP_RESERVE_BYTES.
 */

// Echo path the filter covers, in ms: DAC + head unit + speaker + cabin + mic.
#define AEC_TAIL_MS                   120

// Reference ring: 1024 samples (64 ms at 16 kHz). A power of two, so a free-running
// index wraps with a mask. The mic task reads AEC_REFERENCE_LEAD_SAMPLES behind
// the newest sample, which keeps a frame of slack for interrupt timing.
#define AEC_REFERENCE_RING_SAMPLES    1024
#define AEC_REFERENCE_LEAD_SAMPLES    120

// Heap the canceller takes (measured 57.5 KB at 16 kHz, less at 8 kHz), what must
// stay free after it for Bluedroid, and its largest single allocation (~15 KB).
#define AEC_STATE_BYTES               (60 * 1024)
#define AEC_HEAP_RESERVE_BYTES        (20 * 1024)
#define AEC_LARGEST_ALLOCATION_BYTES  (16 * 1024)

// Mean square of a reference frame above which the far end counts as talking, for
// the DIAG aec line's echo reduction estimate only (about -40 dBFS).
#define AEC_FAR_ACTIVE_MEAN_SQUARE    (300.0 * 300.0)


/* * ============================================================================
 * Call-time diagnostics (resource budget for the echo canceller)
 * ============================================================================
 *
 * Measures what a call leaves free on the board, so the SpeexDSP echo canceller
 * (AEC-PLAN.md, step 0) can be sized from numbers rather than estimates. During a
 * call a low-priority task prints, once per DIAG_LOG_PERIOD_MS:
 *
 *   DIAG mic 16k 133 frames avg 412 max 980 of 7500 us (5%/13%) | uplink fetches 133 skipped 0 | heap free 98K largest 61K min 85K | mic stack free 3120 B
 *
 *   frames   microphone frames processed in the window (133 a second at 16 kHz)
 *   avg/max  microphoneTask time from the end of the I2S read to the frame being
 *            queued, against the frame period (the budget); max includes time the
 *            task was preempted
 *   uplink   times Bluedroid fetched uplink audio, and pokes held back because it
 *            had stopped (see UPLINK_UNANSWERED_POKE_LIMIT); ~133 and 0 in a call
 *   heap     free 8-bit heap now, largest single block, lowest free since boot
 *   stack    microphoneTask stack never used since boot (6144 B allocated)
 *
 * With ECHO_MODE_SPEEX it follows each of those with the echo canceller's line
 * (see logEchoCanceller()):
 *
 *   DIAG aec erle 31 dB over 96 far frames | ref short 0 resync 0 | mic overflow 0 | speex allocs 23
 *
 * and once per DIAG_CPU_PERIOD_MS each core's idle share over that window and the
 * busiest tasks, each as a share of one core:
 *
 *   DIAG cpu 10s idle c0 52% c1 91% | BTC_TASK 21% c0, btController 18% c0, ...
 *
 * followed by every task's stack never used since it started, in bytes, tightest first:
 *
 *   DIAG stack free B: BTC_TASK 1180, hciT 1520, BTU_TASK 4310, microphoneTask 4372, ...
 *
 * At boot it prints the heap once, after Bluetooth and the tasks are up.
 *
 * microphoneTask only adds to three counters; all formatting and UART output happens
 * in diagnosticsTask, so the log cannot stall microphone processing. The CPU line
 * needs CONFIG_FREERTOS_USE_TRACE_FACILITY and CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
 * (sdkconfig.defaults). Set to 0 for daily use.
 */
#define DIAG_LOG_ENABLED              1
#define DIAG_LOG_PERIOD_MS            1000
#define DIAG_CPU_PERIOD_MS            10000

// Room for every task in the system (Bluedroid, controller, IPC, timers, idle, ours).
#define DIAG_MAX_TASKS                32
// Number of busiest tasks named on the CPU line.
#define DIAG_TOP_TASK_COUNT           6

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

// Times Bluedroid has called bluetoothHandsFreeSendCallback(), whatever it returned.
// Written only by the Bluedroid task, read by microphoneTask (poke back-off) and the
// diagnostics. A single aligned 32-bit word, so reads need no lock.
static volatile uint32_t uplinkFetchCount = 0;
// Pokes microphoneTask skipped while backing off. Written only by microphoneTask.
static volatile uint32_t uplinkSkippedPokeCount = 0;

// Dynamic filter coefficients based on current sample rate
static float activeLowPassAlpha = 0.0f;
static float activeHighPassAlpha = 0.0f;

/* --- Voice-switched gate: cross-thread state and derived coefficients --- */

// Peak far-end frame level published by bluetoothHandsFreeReceiveCallback (Bluedroid
// thread) and consumed-and-cleared by microphoneTask. A peak hold rather than a plain
// level so a loud downlink frame can never be missed if the two threads happen to run
// at slightly different frame rates. Single aligned 32-bit float, so the read and the
// write are each atomic on Xtensa - no mutex, and no risk of blocking the BT callback.
static volatile float farEndPeakLevel = 0.0f;

// Per-frame one-pole coefficients, recalculated whenever the call sample rate changes
// because the microphone frame period changes with it.
static float gateFarAttackCoefficient   = 0.0f;
static float gateFarReleaseCoefficient  = 0.0f;
static float gateMicCoefficient         = 0.0f;
static float gateDuckOnsetCoefficient   = 0.0f;
static float gateDuckRecoveryCoefficient = 0.0f;

// Constants pre-converted from dB to linear so the per-frame path has no log/exp in it.
static float gateDuckGainLinear    = 1.0f;
static float gateEchoThresholdGain = 1.0f;   // AEC_ECHO_COUPLING_DB + AEC_NEAREND_MARGIN
static float speakerAttenuationLinear = 1.0f;

// NEW: which DMA profile the Tx channel is currently built with
typedef enum {
    TX_PROFILE_MUSIC = 0,
    TX_PROFILE_CALL  = 1
} TransmitDmaProfile;

static TransmitDmaProfile currentTransmitProfile = TX_PROFILE_MUSIC;

// Rate the I2S channels are clocked at, set by updateI2sSampleRate(). Read by the
// diagnostics to work out the microphone frame budget.
static volatile uint32_t activeSampleRate = 0;

#if AEC_SPEEX_ENABLED
// What the DAC played during a call (right channel = call audio), written by
// speakerSentCallback() and read by microphoneTask. referenceWriteIndex counts every
// sample ever written and only grows; the slot is the index masked to the ring. It
// is published with release order after the samples, and read with acquire order,
// because the interrupt and the mic task can run on different cores.
static int16_t referenceRing[AEC_REFERENCE_RING_SAMPLES];
static volatile uint32_t referenceWriteIndex = 0;

// RX queue overflows since boot (each drops one 120-sample mic frame). Interrupt-written.
static volatile uint32_t micOverflowCount = 0;

// Alignment events since boot, for the DIAG aec line. Written only by microphoneTask.
static volatile uint32_t referenceShortSampleCount = 0;   // reference samples padded with zeros
static volatile uint32_t referenceResyncCount = 0;        // read position reset to the lead
#endif

#if DIAG_LOG_ENABLED
// Microphone frame timing for the current log window. Written by microphoneTask,
// read and cleared by diagnosticsTask, both under diagMutex.
typedef struct {
    uint32_t frameCount;
    uint64_t totalMicroseconds;
    uint32_t maxMicroseconds;
    // Echo canceller, frames with the far end talking only: energy at the mic and
    // left after cancellation (before residual suppression), for the ERLE estimate.
    uint32_t echoFarFrames;
    uint64_t echoMicEnergy;
    uint64_t echoResidualEnergy;
} MicFrameTiming;

static SemaphoreHandle_t diagMutex = NULL;
static MicFrameTiming micFrameTiming = {0};

// Run-time counters of every task at the start of the CPU window, and the total
// run time then. Static rather than on diagnosticsTask's stack: ~1.3 KB each.
static TaskStatus_t cpuTaskSnapshot[DIAG_MAX_TASKS];
static TaskStatus_t cpuTaskCurrent[DIAG_MAX_TASKS];
static UBaseType_t cpuTaskSnapshotCount = 0;
static configRUN_TIME_COUNTER_TYPE cpuSnapshotTotalTime = 0;
#endif

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
/**
 * @brief Converts a time constant in milliseconds into a one-pole smoothing coefficient
 *        for an update that runs once every stepSeconds.
 * * Standard step response: y += coeff * (x - y), coeff = 1 - e^(-dt/tau). Clamped to
 * 1.0 so a time constant shorter than one step degenerates into an instant jump rather
 * than overshooting, and floored at a real value so a zero/negative tau cannot produce
 * a NaN that would poison the envelope for the rest of the call.
 */
static float onePoleCoefficient(float timeConstantMs, float stepSeconds) {
    float tau = timeConstantMs * 0.001f;
    if (tau <= 0.0f || stepSeconds <= 0.0f) {
        return 1.0f;
    }
    float coefficient = 1.0f - expf(-stepSeconds / tau);
    if (coefficient > 1.0f) return 1.0f;
    if (coefficient < 0.0f) return 0.0f;
    return coefficient;
}

void calculateFilterAlphas(uint32_t sampleRate) {
    if (sampleRate == 0) return;
    
    float dt = 1.0f / (float)sampleRate;
    
    float rcLow = 1.0f / (2.0f * MATH_PI * LOW_PASS_CUTOFF_HZ);
    activeLowPassAlpha = dt / (rcLow + dt);
    
    float rcHigh = 1.0f / (2.0f * MATH_PI * HIGH_PASS_CUTOFF_HZ);
    activeHighPassAlpha = rcHigh / (rcHigh + dt);

    /* --- Voice-switched gate coefficients --- */
    // The gate decides once per microphone frame, not once per sample, so its time
    // constants are converted against the FRAME period, not the sample period. The
    // frame period changes when the call renegotiates between CVSD 8k and mSBC 16k,
    // which is exactly why this lives here rather than being a compile-time constant:
    // a 180 ms hangover has to stay 180 ms at both rates.
    float frameSeconds = (float)DMA_FRAME_SIZE_SAMPLES / (float)sampleRate;

    gateFarAttackCoefficient    = onePoleCoefficient(AEC_FAR_ATTACK_MS,    frameSeconds);
    gateFarReleaseCoefficient   = onePoleCoefficient(AEC_FAR_RELEASE_MS,   frameSeconds);
    gateMicCoefficient          = onePoleCoefficient(AEC_MIC_ENVELOPE_MS,  frameSeconds);
    gateDuckOnsetCoefficient    = onePoleCoefficient(AEC_DUCK_ONSET_MS,    frameSeconds);
    gateDuckRecoveryCoefficient = onePoleCoefficient(AEC_DUCK_RECOVERY_MS, frameSeconds);

    // dB -> linear, done once here so the per-frame decision is pure multiply/compare.
    gateDuckGainLinear       = powf(10.0f, AEC_DUCK_DEPTH_DB / 20.0f);
    gateEchoThresholdGain    = powf(10.0f, (AEC_ECHO_COUPLING_DB + AEC_NEAREND_MARGIN) / 20.0f);
    speakerAttenuationLinear = powf(10.0f, AEC_SPEAKER_ATTENUATION_DB / 20.0f);

    ESP_LOGI(TAG, "--- DSP Updated | Fs: %lu Hz | HP Alpha: %.4f | LP Alpha: %.4f ---", 
             sampleRate, activeHighPassAlpha, activeLowPassAlpha);
#if AEC_GATE_ENABLED
    ESP_LOGI(TAG, "--- Gate | frame %.2f ms | duck %.1f dB | near-end threshold %.3f x far ---",
             frameSeconds * 1000.0f, (float)AEC_DUCK_DEPTH_DB, gateEchoThresholdGain);
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

#if AEC_SPEEX_ENABLED
/**
 * @brief TX on_sent interrupt: copies the call audio of the descriptor the DAC just
 *        finished playing into referenceRing.
 * * Registered on the call-profile TX channel only. The descriptor holds 60 stereo
 * frames; the right channel is the call audio in both stereo modes. With
 * auto_clear (after the callback) an underrun's descriptor is silence, so the ring
 * holds what was really played. Copy only: no float, no logging, nothing that blocks.
 * * @return false: no task is woken.
 */
static bool IRAM_ATTR speakerSentCallback(i2s_chan_handle_t channel, i2s_event_data_t *event, void *userContext) {
    const int16_t *stereoSamples = (const int16_t *)event->dma_buf;
    uint32_t frameCount = (uint32_t)(event->size / (2 * sizeof(int16_t)));
    uint32_t writeIndex = referenceWriteIndex;

    for (uint32_t frame = 0; frame < frameCount; frame++) {
        uint32_t rightChannelIndex = frame * 2 + 1;
        uint32_t ringSlot = writeIndex & (AEC_REFERENCE_RING_SAMPLES - 1);
        referenceRing[ringSlot] = stereoSamples[rightChannelIndex];
        writeIndex++;
    }

    __atomic_store_n(&referenceWriteIndex, writeIndex, __ATOMIC_RELEASE);
    return false;
}

/**
 * @brief RX on_recv_q_ovf interrupt: the mic task fell behind and the oldest mic frame
 *        was dropped. Counted, so microphoneTask skips the matching reference.
 * * @return false: no task is woken.
 */
static bool IRAM_ATTR micOverflowCallback(i2s_chan_handle_t channel, i2s_event_data_t *event, void *userContext) {
    micOverflowCount++;
    return false;
}
#endif

/**
 * @brief NEW: Builds the I2S Tx (speaker) channel with a given DMA geometry.
 * * Factored out of initializeI2sTransmitChannel() so the channel can be rebuilt with
 * call-sized DMA buffers when a call starts and music-sized buffers when it ends.
 * DMA depth is fixed at channel creation - i2s_channel_reconfig_std_clock() only
 * touches the clock - so a rebuild is the only way to change it.
 */
static void createTransmitChannel(uint32_t sampleRate, uint32_t descriptorCount,
                                  uint32_t framesPerDescriptor, uint32_t preloadDescriptors,
                                  bool isReferenceCaptured) {
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

#if AEC_SPEEX_ENABLED
    // The echo canceller's reference: callbacks must be registered before the channel
    // is enabled. Music never needs it.
    if (isReferenceCaptured) {
        i2s_event_callbacks_t transmitCallbacks = { .on_sent = speakerSentCallback };
        i2s_channel_register_event_callback(txChannel, &transmitCallbacks, NULL);
    }
#else
    (void)isReferenceCaptured;
#endif

    // Prime the DMA queue with silence while the channel is still in the READY state.
    // i2s_channel_preload_data() only works before the clock starts, which is exactly the
    // window we need: once running, there is no way to get ahead of a rate-matched source.
    if (preloadDescriptors > 0 && framesPerDescriptor <= CALL_TX_FRAMES_PER_DESC) {
        static int16_t silentDescriptor[CALL_TX_FRAMES_PER_DESC * 2];
        memset(silentDescriptor, 0, sizeof(silentDescriptor));
        for (uint32_t index = 0; index < preloadDescriptors; index++) {
            size_t bytesLoaded = 0;
            i2s_channel_preload_data(txChannel, silentDescriptor,
                                     framesPerDescriptor * 2 * sizeof(int16_t), &bytesLoaded);
        }
    }

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
        createTransmitChannel(sampleRate, CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC, CALL_TX_PRELOAD_DESC, true);
        ESP_LOGI(TAG, "--- Tx DMA -> CALL profile (%d x %d frames) ---",
                 CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC);
    } else {
        createTransmitChannel(sampleRate, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC, 0, false);
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
    activeSampleRate = targetSampleRate;

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
    // In call mode always rebuild rather than just retuning the clock. A bare
    // disable/reconfig/enable empties the DMA queue without re-priming it and without
    // restarting the echo reference's playout clock, so a mid-call codec renegotiation
    // would silently desynchronise the canceller for the rest of the call.
    if (wantedProfile != currentTransmitProfile || wantedProfile == TX_PROFILE_CALL) {
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
 * * * @param audioBuffer Pointer to the raw PCM bytes from the caller.
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

#if AEC_GATE_ENABLED
    // Running sum of |sample| for the voice-switched gate. This is measured on what we
    // are about to hand the DAC - i.e. after any speaker attenuation - because that, not
    // the raw SCO payload, is what physically generates the echo.
    uint32_t farEndLevelAccumulator = 0;
#endif

    for (uint32_t index = 0; index < numberOfSamples; index++) {
        uint32_t leftChannelIndex = index * 2;
        uint32_t rightChannelIndex = leftChannelIndex + 1;
        
        int16_t currentSample = pcm16Data[index];

        // Optional fixed trim on the speaker path (AEC_SPEAKER_ATTENUATION_DB), in every
        // echo mode. Skipped entirely when set to 0 dB, so the samples pass through untouched.
        if (speakerAttenuationLinear < 0.999f) {
            float attenuated = (float)currentSample * speakerAttenuationLinear;
            currentSample = (int16_t)attenuated;
        }

#if AEC_GATE_ENABLED
        int32_t absoluteSample = (currentSample < 0) ? -(int32_t)currentSample
                                                     :  (int32_t)currentSample;
        farEndLevelAccumulator += (uint32_t)absoluteSample;
#endif

        if (enableHfpStereoMode) {
            stereoBuffer[leftChannelIndex] = currentSample;
            stereoBuffer[rightChannelIndex] = currentSample;
        } else {
            stereoBuffer[leftChannelIndex] = 0;
            stereoBuffer[rightChannelIndex] = currentSample;
        }
    }

#if AEC_GATE_ENABLED
    // Publish as a PEAK HOLD, not an assignment. microphoneTask clears this when it
    // consumes it, so if two downlink frames land between two microphone frames the
    // louder one survives. Assigning instead would let a quiet frame mask a loud one
    // and briefly unlatch the gate mid-sentence.
    if (numberOfSamples > 0) {
        float farEndFrameLevel = (float)farEndLevelAccumulator / (float)numberOfSamples;
        if (farEndFrameLevel > farEndPeakLevel) {
            farEndPeakLevel = farEndFrameLevel;
        }
    }
#endif

    xSemaphoreTake(txMutex, portMAX_DELAY);   
    // Short timeout. With the call-mode DMA profile the whole Tx queue is only
    // ~15 ms deep, so a long block here would stall the Bluedroid callback thread for
    // far longer than the audio is worth.
    i2s_channel_write(txChannel, (void*)stereoBuffer, stereoBufferSize, &bytesWritten,
                      pdMS_TO_TICKS(CALL_TX_WRITE_TIMEOUT_MS));
    xSemaphoreGive(txMutex);
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
    // Counted before any early return: being called at all is what shows the SCO
    // link is still taking uplink audio (see UPLINK_UNANSWERED_POKE_LIMIT).
    uplinkFetchCount++;

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
            ESP_LOGI(TAG, "--- Call Ended ---");
            updateI2sSampleRate(SAMPLE_RATE_44K); // Explicitly DISABLES the RX DMA buffer

            // NEW: become visible again now that the SCO link is gone.
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        }
    }
}

#if DIAG_LOG_ENABLED
/* * ============================================================================
 * Call-time diagnostics
 * ============================================================================
 */

/**
 * @brief Adds one microphone frame's processing time to the current log window.
 * * Called by microphoneTask once per frame. The mutex is held for three additions
 * only, so the diagnostics task can never delay the next frame noticeably.
 * * @param elapsedMicroseconds Time from the end of the I2S read to the frame being queued.
 */
static void recordMicFrameTime(uint32_t elapsedMicroseconds) {
    xSemaphoreTake(diagMutex, portMAX_DELAY);
    micFrameTiming.frameCount++;
    micFrameTiming.totalMicroseconds += elapsedMicroseconds;
    if (elapsedMicroseconds > micFrameTiming.maxMicroseconds) {
        micFrameTiming.maxMicroseconds = elapsedMicroseconds;
    }
    xSemaphoreGive(diagMutex);
}

#if AEC_SPEEX_ENABLED
/**
 * @brief Adds one cancelled frame to the echo reduction estimate, if the far end was
 *        talking in it (reference mean square above AEC_FAR_ACTIVE_MEAN_SQUARE).
 * * Mic and residual energies are summed over such frames; the DIAG aec line shows
 * their ratio as ERLE. Near-end speech in those frames lowers the figure, so it is a
 * floor, not an exact measure.
 */
static void recordEchoFrame(const int16_t *micFrame, const int16_t *referenceFrame,
                            const int16_t *residualFrame, uint32_t sampleCount) {
    uint64_t referenceEnergy = 0;
    uint64_t micEnergy = 0;
    uint64_t residualEnergy = 0;
    for (uint32_t index = 0; index < sampleCount; index++) {
        int32_t referenceSample = referenceFrame[index];
        int32_t micSample = micFrame[index];
        int32_t residualSample = residualFrame[index];
        referenceEnergy += (uint64_t)(referenceSample * referenceSample);
        micEnergy += (uint64_t)(micSample * micSample);
        residualEnergy += (uint64_t)(residualSample * residualSample);
    }

    double referenceMeanSquare = (double)referenceEnergy / (double)sampleCount;
    bool isFarEndTalking = (referenceMeanSquare > AEC_FAR_ACTIVE_MEAN_SQUARE);
    if (!isFarEndTalking) {
        return;
    }

    xSemaphoreTake(diagMutex, portMAX_DELAY);
    micFrameTiming.echoFarFrames++;
    micFrameTiming.echoMicEnergy += micEnergy;
    micFrameTiming.echoResidualEnergy += residualEnergy;
    xSemaphoreGive(diagMutex);
}
#endif

/**
 * @brief Returns the frame timing gathered since the last call and starts a new window.
 * * Example: after one second of a 16 kHz call, { frameCount 133, totalMicroseconds
 * 54796, maxMicroseconds 980 }.
 */
static MicFrameTiming takeMicFrameTiming(void) {
    MicFrameTiming windowTiming;
    xSemaphoreTake(diagMutex, portMAX_DELAY);
    windowTiming = micFrameTiming;
    memset(&micFrameTiming, 0, sizeof(micFrameTiming));
    xSemaphoreGive(diagMutex);
    return windowTiming;
}

/**
 * @brief Logs the free heap, largest free block and lowest free heap since boot.
 * * @param when Short label for the moment measured, e.g. "after init".
 */
static void logHeapUsage(const char *when) {
    size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t minimumFreeBytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "DIAG heap %s: free %uK largest %uK min %uK",
             when,
             (unsigned)(freeBytes / 1024),
             (unsigned)(largestBlockBytes / 1024),
             (unsigned)(minimumFreeBytes / 1024));
}

// Uplink counters at the last log line; only diagnosticsTask touches these.
static uint32_t uplinkFetchCountAtLastLog = 0;
static uint32_t uplinkSkippedPokeCountAtLastLog = 0;

/**
 * @brief Returns how many uplink fetches and skipped pokes happened since the last
 *        call, and starts a new window.
 * * Example, one second of a healthy 16 kHz call: fetches ~133, skipped 0. While the
 * SCO link is closing: fetches 0, skipped climbing.
 * * @param fetchesOut Times Bluedroid called bluetoothHandsFreeSendCallback().
 * @param skippedOut Pokes microphoneTask held back (see UPLINK_UNANSWERED_POKE_LIMIT).
 */
static void takeUplinkCounts(uint32_t *fetchesOut, uint32_t *skippedOut) {
    uint32_t fetchCountNow = uplinkFetchCount;
    uint32_t skippedCountNow = uplinkSkippedPokeCount;
    *fetchesOut = fetchCountNow - uplinkFetchCountAtLastLog;
    *skippedOut = skippedCountNow - uplinkSkippedPokeCountAtLastLog;
    uplinkFetchCountAtLastLog = fetchCountNow;
    uplinkSkippedPokeCountAtLastLog = skippedCountNow;
}

/**
 * @brief Logs the once-a-second call budget line: microphone frame time against the
 *        frame period, uplink fetches and skipped pokes, the heap, and the microphone
 *        task's unused stack.
 */
static MicFrameTiming logCallBudget(void) {
    MicFrameTiming windowTiming = takeMicFrameTiming();
    uint32_t uplinkFetches = 0;
    uint32_t uplinkSkippedPokes = 0;
    takeUplinkCounts(&uplinkFetches, &uplinkSkippedPokes);

    // Frame period in microseconds: 120 samples are 7500 us at 16 kHz, 15000 us at 8 kHz.
    uint32_t sampleRate = activeSampleRate;
    uint32_t frameBudgetMicroseconds = 0;
    if (sampleRate > 0) {
        uint64_t frameSampleMicroseconds = (uint64_t)DMA_FRAME_SIZE_SAMPLES * 1000000ULL;
        frameBudgetMicroseconds = (uint32_t)(frameSampleMicroseconds / sampleRate);
    }

    uint32_t averageMicroseconds = 0;
    if (windowTiming.frameCount > 0) {
        averageMicroseconds = (uint32_t)(windowTiming.totalMicroseconds / windowTiming.frameCount);
    }

    uint32_t averagePercent = 0;
    uint32_t maxPercent = 0;
    if (frameBudgetMicroseconds > 0) {
        uint32_t averageScaled = averageMicroseconds * 100;
        uint32_t maxScaled = windowTiming.maxMicroseconds * 100;
        averagePercent = averageScaled / frameBudgetMicroseconds;
        maxPercent = maxScaled / frameBudgetMicroseconds;
    }

    size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t minimumFreeBytes = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);

    // On ESP-IDF the high water mark is in bytes, not words.
    unsigned micStackFreeBytes = 0;
    if (micTaskHandle != NULL) {
        micStackFreeBytes = (unsigned)uxTaskGetStackHighWaterMark(micTaskHandle);
    }

    ESP_LOGI(TAG, "DIAG mic %luk %lu frames avg %lu max %lu of %lu us (%lu%%/%lu%%) | "
                  "uplink fetches %lu skipped %lu | "
                  "heap free %uK largest %uK min %uK | mic stack free %u B",
             (unsigned long)(sampleRate / 1000),
             (unsigned long)windowTiming.frameCount,
             (unsigned long)averageMicroseconds,
             (unsigned long)windowTiming.maxMicroseconds,
             (unsigned long)frameBudgetMicroseconds,
             (unsigned long)averagePercent,
             (unsigned long)maxPercent,
             (unsigned long)uplinkFetches,
             (unsigned long)uplinkSkippedPokes,
             (unsigned)(freeBytes / 1024),
             (unsigned)(largestBlockBytes / 1024),
             (unsigned)(minimumFreeBytes / 1024),
             micStackFreeBytes);
    return windowTiming;
}

#if AEC_SPEEX_ENABLED
// Alignment counters at the last DIAG aec line; only diagnosticsTask touches these.
static uint32_t referenceShortAtLastLog = 0;
static uint32_t referenceResyncAtLastLog = 0;
static uint32_t micOverflowAtLastLog = 0;

/**
 * @brief Starts the alignment counters' window afresh (call start).
 */
static void resetEchoCounters(void) {
    referenceShortAtLastLog = referenceShortSampleCount;
    referenceResyncAtLastLog = referenceResyncCount;
    micOverflowAtLastLog = micOverflowCount;
}

/**
 * @brief Logs the once-a-second echo canceller line, from the window logCallBudget()
 *        just took. Example:
 *   DIAG aec erle 31 dB over 96 far frames | ref short 0 resync 0 | mic overflow 0 | speex allocs 23
 * * erle      echo reduction by the canceller alone, while the far end talks ("--" if
 *             it did not); residual suppression removes more after it
 * * ref short reference samples that had not arrived yet (padded with silence)
 * * resync    times the reference was re-aligned because the mic task fell behind
 * * overflow  mic frames dropped by the RX DMA queue (the reference skips them too)
 * * allocs    SpeexDSP allocations alive: about 20 in a call, 0 between calls
 * * @param windowTiming The window logCallBudget() took.
 */
static void logEchoCanceller(const MicFrameTiming *windowTiming) {
    uint32_t shortNow = referenceShortSampleCount;
    uint32_t resyncNow = referenceResyncCount;
    uint32_t overflowNow = micOverflowCount;
    uint32_t shortInWindow = shortNow - referenceShortAtLastLog;
    uint32_t resyncInWindow = resyncNow - referenceResyncAtLastLog;
    uint32_t overflowInWindow = overflowNow - micOverflowAtLastLog;
    referenceShortAtLastLog = shortNow;
    referenceResyncAtLastLog = resyncNow;
    micOverflowAtLastLog = overflowNow;

    char erleText[16];
    bool hasFarFrames = (windowTiming->echoFarFrames > 0 && windowTiming->echoMicEnergy > 0);
    if (hasFarFrames) {
        double residualEnergy = (windowTiming->echoResidualEnergy > 0) ? (double)windowTiming->echoResidualEnergy : 1.0;
        double energyRatio = (double)windowTiming->echoMicEnergy / residualEnergy;
        double erleDb = 10.0 * log10(energyRatio);
        snprintf(erleText, sizeof(erleText), "%d dB", (int)lround(erleDb));
    } else {
        snprintf(erleText, sizeof(erleText), "--");
    }

    ESP_LOGI(TAG, "DIAG aec erle %s over %lu far frames | ref short %lu resync %lu | mic overflow %lu | speex allocs %d",
             erleText,
             (unsigned long)windowTiming->echoFarFrames,
             (unsigned long)shortInWindow,
             (unsigned long)resyncInWindow,
             (unsigned long)overflowInWindow,
             speexdspLiveAllocations());
}
#endif

/**
 * @brief Records every task's run-time counter as the start of a CPU window.
 */
static void takeCpuSnapshot(void) {
    cpuTaskSnapshotCount = uxTaskGetSystemState(cpuTaskSnapshot, DIAG_MAX_TASKS, &cpuSnapshotTotalTime);
    if (cpuTaskSnapshotCount == 0) {
        ESP_LOGW(TAG, "DIAG cpu: more than %d tasks, raise DIAG_MAX_TASKS", DIAG_MAX_TASKS);
    }
}

/**
 * @brief Run time a task used since the snapshot, or all of it for a task started since.
 * * @param currentTask The task's status now.
 * @return Counter ticks used in this window.
 */
static configRUN_TIME_COUNTER_TYPE taskRunTimeSinceSnapshot(const TaskStatus_t *currentTask) {
    for (UBaseType_t index = 0; index < cpuTaskSnapshotCount; index++) {
        bool isSameTask = (cpuTaskSnapshot[index].xHandle == currentTask->xHandle);
        if (isSameTask) {
            return currentTask->ulRunTimeCounter - cpuTaskSnapshot[index].ulRunTimeCounter;
        }
    }
    return currentTask->ulRunTimeCounter;
}

/**
 * @brief Logs every task's stack that has never been used since it started, tightest
 *        first, as one line printed with the CPU line.
 * * Shows which stacks (Bluetooth's in particular) could give RAM back, and which are
 * close to overflowing. On ESP-IDF the figure is in bytes. Example:
 *   DIAG stack free B: BTC_TASK 1180, BTU_TASK 4310, microphoneTask 4372, ...
 * * @param taskCount Number of valid entries in cpuTaskCurrent.
 */
static void logTaskStacks(UBaseType_t taskCount) {
    char stacksText[448];
    size_t textLength = 0;
    stacksText[0] = '\0';

    bool isListed[DIAG_MAX_TASKS] = { false };
    for (UBaseType_t rank = 0; rank < taskCount; rank++) {
        int tightestIndex = -1;
        for (UBaseType_t index = 0; index < taskCount; index++) {
            if (isListed[index]) {
                continue;
            }
            bool isTighter = (tightestIndex < 0 ||
                              cpuTaskCurrent[index].usStackHighWaterMark <
                              cpuTaskCurrent[tightestIndex].usStackHighWaterMark);
            if (isTighter) {
                tightestIndex = (int)index;
            }
        }
        isListed[tightestIndex] = true;

        size_t roomLeft = sizeof(stacksText) - textLength;
        int written = snprintf(stacksText + textLength, roomLeft, "%s%s %lu",
                               (rank == 0) ? "" : ", ",
                               cpuTaskCurrent[tightestIndex].pcTaskName,
                               (unsigned long)cpuTaskCurrent[tightestIndex].usStackHighWaterMark);
        if (written < 0 || (size_t)written >= roomLeft) {
            break;
        }
        textLength += (size_t)written;
    }

    ESP_LOGI(TAG, "DIAG stack free B: %s", stacksText);
}

/**
 * @brief Logs each core's idle share and the busiest tasks since the last snapshot,
 *        then starts a new window.
 * * Shares are of one core: FreeRTOS's total run time is the time elapsed, not
 * elapsed x cores, so a task using all of core 0 reads 100%. Example:
 *   DIAG cpu 10s idle c0 52% c1 91% | BTC_TASK 21% c0, btController 18% c0
 * * @param windowMilliseconds Length of the window, for the label.
 */
static void logCpuUsage(uint32_t windowMilliseconds) {
    configRUN_TIME_COUNTER_TYPE totalTimeNow = 0;
    UBaseType_t currentCount = uxTaskGetSystemState(cpuTaskCurrent, DIAG_MAX_TASKS, &totalTimeNow);
    configRUN_TIME_COUNTER_TYPE totalTimeDelta = totalTimeNow - cpuSnapshotTotalTime;

    if (currentCount == 0 || totalTimeDelta == 0) {
        takeCpuSnapshot();
        return;
    }

    TaskHandle_t idleTaskCore0 = xTaskGetIdleTaskHandleForCore(0);
    TaskHandle_t idleTaskCore1 = xTaskGetIdleTaskHandleForCore(1);
    uint32_t idlePercentCore0 = 0;
    uint32_t idlePercentCore1 = 0;

    // Per-task share of one core this window; idle tasks are reported apart.
    uint32_t taskPercent[DIAG_MAX_TASKS];
    for (UBaseType_t index = 0; index < currentCount; index++) {
        configRUN_TIME_COUNTER_TYPE taskDelta = taskRunTimeSinceSnapshot(&cpuTaskCurrent[index]);
        uint64_t taskDeltaScaled = (uint64_t)taskDelta * 100ULL;
        uint32_t percent = (uint32_t)(taskDeltaScaled / (uint64_t)totalTimeDelta);

        bool isIdleCore0 = (cpuTaskCurrent[index].xHandle == idleTaskCore0);
        bool isIdleCore1 = (cpuTaskCurrent[index].xHandle == idleTaskCore1);
        if (isIdleCore0) {
            idlePercentCore0 = percent;
            percent = 0;
        } else if (isIdleCore1) {
            idlePercentCore1 = percent;
            percent = 0;
        }
        taskPercent[index] = percent;
    }

    // Name the busiest tasks, highest first, skipping any under 1%.
    char topTasksText[192];
    size_t textLength = 0;
    topTasksText[0] = '\0';
    for (int rank = 0; rank < DIAG_TOP_TASK_COUNT; rank++) {
        int busiestIndex = -1;
        for (UBaseType_t index = 0; index < currentCount; index++) {
            bool isBusier = (busiestIndex < 0 || taskPercent[index] > taskPercent[busiestIndex]);
            if (taskPercent[index] > 0 && isBusier) {
                busiestIndex = (int)index;
            }
        }
        if (busiestIndex < 0) {
            break;
        }

        BaseType_t coreId = xTaskGetCoreID(cpuTaskCurrent[busiestIndex].xHandle);
        char coreLabel = (coreId == tskNO_AFFINITY) ? '*' : (char)('0' + coreId);
        size_t roomLeft = sizeof(topTasksText) - textLength;
        int written = snprintf(topTasksText + textLength, roomLeft, "%s%s %lu%% c%c",
                               (rank == 0) ? "" : ", ",
                               cpuTaskCurrent[busiestIndex].pcTaskName,
                               (unsigned long)taskPercent[busiestIndex],
                               coreLabel);
        if (written < 0 || (size_t)written >= roomLeft) {
            break;
        }
        textLength += (size_t)written;
        taskPercent[busiestIndex] = 0;
    }

    ESP_LOGI(TAG, "DIAG cpu %lus idle c0 %lu%% c1 %lu%% | %s",
             (unsigned long)(windowMilliseconds / 1000),
             (unsigned long)idlePercentCore0,
             (unsigned long)idlePercentCore1,
             topTasksText);
    logTaskStacks(currentCount);

    memcpy(cpuTaskSnapshot, cpuTaskCurrent, sizeof(TaskStatus_t) * currentCount);
    cpuTaskSnapshotCount = currentCount;
    cpuSnapshotTotalTime = totalTimeNow;
}

/**
 * @brief Low-priority task that prints the call budget lines while a call is active.
 * * Wakes every DIAG_LOG_PERIOD_MS. At the start of a call it discards timing from
 * before the call and takes a CPU snapshot; during the call it logs the budget line
 * every period and the CPU line every DIAG_CPU_PERIOD_MS. Silent between calls.
 * * @param taskParameters FreeRTOS task parameters (unused).
 */
static void diagnosticsTask(void *taskParameters) {
    bool wasCallActive = false;
    uint32_t millisecondsSinceCpuLog = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(DIAG_LOG_PERIOD_MS));

        bool isCallActive = isAudioPathOpen;
        bool isCallStarting = (isCallActive && !wasCallActive);

        if (isCallStarting) {
            uint32_t discardedFetches = 0;
            uint32_t discardedSkippedPokes = 0;
            takeMicFrameTiming();
            takeUplinkCounts(&discardedFetches, &discardedSkippedPokes);
#if AEC_SPEEX_ENABLED
            resetEchoCounters();
#endif
            takeCpuSnapshot();
            millisecondsSinceCpuLog = 0;
            logHeapUsage("call start");
        } else if (isCallActive) {
            MicFrameTiming windowTiming = logCallBudget();
#if AEC_SPEEX_ENABLED
            logEchoCanceller(&windowTiming);
#else
            (void)windowTiming;
#endif

            millisecondsSinceCpuLog += DIAG_LOG_PERIOD_MS;
            if (millisecondsSinceCpuLog >= DIAG_CPU_PERIOD_MS) {
                logCpuUsage(millisecondsSinceCpuLog);
                millisecondsSinceCpuLog = 0;
            }
        }

        wasCallActive = isCallActive;
    }
}
#endif  /* DIAG_LOG_ENABLED */

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

// Poke back-off state, owned by microphoneTask.
typedef struct {
    uint32_t fetchCountAtLastPoke;   // uplinkFetchCount when the last frame was handled
    uint32_t unansweredPokeCount;    // frames in a row with no fetch since
} UplinkPokeState;

/**
 * @brief Starts the back-off state afresh, as if the last poke had been answered.
 * * Called between calls, so a new call always starts poking every frame.
 */
static void resetUplinkPokeState(UplinkPokeState *pokeState) {
    pokeState->fetchCountAtLastPoke = uplinkFetchCount;
    pokeState->unansweredPokeCount = 0;
}

/**
 * @brief Tells Bluedroid uplink audio is ready, unless it has stopped fetching.
 * * Every frame while Bluedroid keeps calling bluetoothHandsFreeSendCallback(). After
 * UPLINK_UNANSWERED_POKE_LIMIT frames with no call, every
 * UPLINK_BACKOFF_POKE_INTERVAL-th frame only, until a call is seen again.
 * Example at 16 kHz: link closes at frame 0; frames 1-8 still poke, frames 9-23
 * skip, frame 24 pokes, frames 25-39 skip, and so on.
 * * @param pokeState microphoneTask's back-off state.
 */
static void pokeUplink(UplinkPokeState *pokeState) {
    uint32_t fetchCountNow = uplinkFetchCount;
    bool wasFetchedSinceLastPoke = (fetchCountNow != pokeState->fetchCountAtLastPoke);
    pokeState->fetchCountAtLastPoke = fetchCountNow;

    if (wasFetchedSinceLastPoke) {
        pokeState->unansweredPokeCount = 0;
    } else {
        pokeState->unansweredPokeCount++;
    }

    bool isBackingOff = (pokeState->unansweredPokeCount >= UPLINK_UNANSWERED_POKE_LIMIT);
    uint32_t framesIntoBackoff = pokeState->unansweredPokeCount - UPLINK_UNANSWERED_POKE_LIMIT;
    bool isProbeFrame = ((framesIntoBackoff % UPLINK_BACKOFF_POKE_INTERVAL) == 0);

    if (isBackingOff && !isProbeFrame) {
        uplinkSkippedPokeCount++;
        return;
    }
    esp_hf_client_outgoing_data_ready();
}

/**
 * @brief Clamps a sample into int16 range in the float domain, NaN to 0.
 * * Converting a NaN or an out-of-range float to an integer is undefined behaviour,
 * and on Xtensa it yields a value no integer-side clamp recognises: one bad sample
 * becomes a constant the far end hears as dead silence. The compound test is false
 * for NaN as well as for out-of-range values, so both land in the recovery branch.
 * Example: 40000.0 -> 32767.0, -1e9 -> -32768.0, NaN -> 0.0, 123.4 -> 123.4.
 */
static float clampToSampleRange(float value) {
    bool isInRange = (value >= -32768.0f && value <= 32767.0f);
    if (isInRange) {
        return value;
    }
    if (value > 0.0f) {
        return 32767.0f;
    }
    if (value < 0.0f) {
        return -32768.0f;
    }
    return 0.0f;
}

/**
 * @brief Reads mic frames until the RX DMA queue is empty (at most 20), so the next
 *        read is a frame captured from now on.
 * * Used at call start and, with the echo canceller, before re-aligning the reference
 * after a codec change.
 * * @param scratchBuffer Somewhere to read into, one frame long.
 */
static void drainMicQueue(int32_t *scratchBuffer, size_t scratchBytes) {
    size_t bytesRead = 0;
    int drainLimit = 20;

    xSemaphoreTake(rxMutex, portMAX_DELAY);
    while (drainLimit > 0 &&
           i2s_channel_read(rxChannel, scratchBuffer, scratchBytes, &bytesRead, 0) == ESP_OK &&
           bytesRead > 0) {
        drainLimit--;
    }
    xSemaphoreGive(rxMutex);
}

#if AEC_SPEEX_ENABLED
// The mic task's echo canceller. Both states are NULL when it is not running (no
// call, or the heap could not spare it), and the frames then pass through unchanged.
typedef struct {
    SpeexEchoState *echoState;
    SpeexPreprocessState *preprocessState;
    uint32_t sampleRate;            // rate it was made for (or last tried); 0 = none
    uint32_t referenceReadIndex;    // next referenceRing sample, same count as referenceWriteIndex
    uint32_t overflowCountSeen;     // micOverflowCount already accounted for
} EchoCanceller;

/**
 * @brief Destroys the canceller's states, if any. Safe to call when none exist.
 */
static void destroyEchoCanceller(EchoCanceller *canceller) {
    if (canceller->preprocessState != NULL) {
        speex_preprocess_state_destroy(canceller->preprocessState);
        canceller->preprocessState = NULL;
    }
    if (canceller->echoState != NULL) {
        speex_echo_state_destroy(canceller->echoState);
        canceller->echoState = NULL;
    }
    canceller->sampleRate = 0;
}

/**
 * @brief Creates the echo canceller and preprocessor for a call rate.
 * * Checks the heap first: SpeexDSP does not check its own allocations, so a failed
 * one would crash rather than return NULL. On a heap too small it logs once and
 * leaves the states NULL; the call goes on without cancellation.
 * Example log: "--- AEC on | 16000 Hz | 120 ms tail | 57 KB | heap left 31K ---"
 * * @param sampleRate 16000 (mSBC) or 8000 (CVSD).
 */
static void createEchoCanceller(EchoCanceller *canceller, uint32_t sampleRate) {
    destroyEchoCanceller(canceller);
    canceller->sampleRate = sampleRate;

    // Call rates only: a call that ends while this runs leaves the music rate behind.
    bool isCallSampleRate = (sampleRate == SAMPLE_RATE_8K || sampleRate == SAMPLE_RATE_16K);
    if (!isCallSampleRate) {
        return;
    }

    size_t freeBytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largestBlockBytes = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t requiredBytes = AEC_STATE_BYTES + AEC_HEAP_RESERVE_BYTES;
    bool isHeapEnough = (freeBytes >= requiredBytes && largestBlockBytes >= AEC_LARGEST_ALLOCATION_BYTES);
    if (!isHeapEnough) {
        ESP_LOGW(TAG, "--- AEC off: heap free %uK largest %uK, needs %uK and a %uK block ---",
                 (unsigned)(freeBytes / 1024), (unsigned)(largestBlockBytes / 1024),
                 (unsigned)(requiredBytes / 1024), (unsigned)(AEC_LARGEST_ALLOCATION_BYTES / 1024));
        return;
    }

    int frameSamples = (int)DMA_FRAME_SIZE_SAMPLES;
    int tailSamples = (int)((sampleRate * AEC_TAIL_MS) / 1000);
    int speexSampleRate = (int)sampleRate;
    size_t bytesBefore = speexdspAllocatedBytes();

    canceller->echoState = speex_echo_state_init(frameSamples, tailSamples);
    speex_echo_ctl(canceller->echoState, SPEEX_ECHO_SET_SAMPLING_RATE, &speexSampleRate);
    canceller->preprocessState = speex_preprocess_state_init(frameSamples, speexSampleRate);
    speex_preprocess_ctl(canceller->preprocessState, SPEEX_PREPROCESS_SET_ECHO_STATE, canceller->echoState);

    size_t stateBytes = speexdspAllocatedBytes() - bytesBefore;
    ESP_LOGI(TAG, "--- AEC on | %lu Hz | %d ms tail | %uK | heap left %uK ---",
             (unsigned long)sampleRate, AEC_TAIL_MS, (unsigned)(stateBytes / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_8BIT) / 1024));
}

/**
 * @brief Aligns the reference with the next mic frame: reads start
 *        AEC_REFERENCE_LEAD_SAMPLES behind the newest played sample.
 * * Call right after drainMicQueue(), so the next mic frame is captured from now on
 * and the reference leads it. Earlier overflows are forgotten.
 */
static void syncEchoReference(EchoCanceller *canceller) {
    uint32_t writeIndex = __atomic_load_n(&referenceWriteIndex, __ATOMIC_ACQUIRE);
    canceller->referenceReadIndex = writeIndex - AEC_REFERENCE_LEAD_SAMPLES;
    canceller->overflowCountSeen = micOverflowCount;
}

/**
 * @brief Takes the reference samples that belong to the mic frame being processed.
 * * Skips one mic frame of reference per RX overflow. If the ring has not yet got
 * a sample, it is padded with 0 but still counted, so the alignment holds. If the
 * reader has fallen so far behind that the interrupt is overwriting what it would
 * read, it re-aligns at the lead (the canceller then re-converges).
 * * @param referenceFrame Filled with sampleCount samples.
 */
static void takeEchoReference(EchoCanceller *canceller, int16_t *referenceFrame, uint32_t sampleCount) {
    uint32_t overflowCountNow = micOverflowCount;
    uint32_t newOverflows = overflowCountNow - canceller->overflowCountSeen;
    canceller->overflowCountSeen = overflowCountNow;
    uint32_t droppedMicSamples = newOverflows * DMA_FRAME_SIZE_SAMPLES;
    canceller->referenceReadIndex += droppedMicSamples;

    uint32_t writeIndex = __atomic_load_n(&referenceWriteIndex, __ATOMIC_ACQUIRE);
    int32_t availableSamples = (int32_t)(writeIndex - canceller->referenceReadIndex);
    int32_t overwriteMargin = AEC_REFERENCE_RING_SAMPLES - (int32_t)(2 * DMA_FRAME_SIZE_SAMPLES);
    if (availableSamples > overwriteMargin) {
        canceller->referenceReadIndex = writeIndex - AEC_REFERENCE_LEAD_SAMPLES;
        referenceResyncCount++;
    }

    for (uint32_t index = 0; index < sampleCount; index++) {
        int32_t samplesAhead = (int32_t)(writeIndex - canceller->referenceReadIndex);
        bool isWritten = (samplesAhead > 0);
        if (isWritten) {
            uint32_t ringSlot = canceller->referenceReadIndex & (AEC_REFERENCE_RING_SAMPLES - 1);
            referenceFrame[index] = referenceRing[ringSlot];
        } else {
            referenceFrame[index] = 0;
            referenceShortSampleCount++;
        }
        canceller->referenceReadIndex++;
    }
}

/**
 * @brief Removes the speaker's echo from one mic frame.
 * * micFrame and outputFrame are int16 before MICROPHONE_GAIN. Without states (no call
 * yet, heap too small) or with a frame of another size, the mic passes through; the
 * reference is consumed either way so alignment holds.
 * * @param sampleCount Samples in the frame; DMA_FRAME_SIZE_SAMPLES normally.
 */
static void cancelEcho(EchoCanceller *canceller, const int16_t *micFrame, int16_t *outputFrame, uint32_t sampleCount) {
    static int16_t referenceFrame[DMA_FRAME_SIZE_SAMPLES];
    takeEchoReference(canceller, referenceFrame, sampleCount);

    bool isCancellerRunning = (canceller->echoState != NULL && canceller->preprocessState != NULL);
    bool isWholeFrame = (sampleCount == DMA_FRAME_SIZE_SAMPLES);
    if (!isCancellerRunning || !isWholeFrame) {
        memcpy(outputFrame, micFrame, sampleCount * sizeof(int16_t));
        return;
    }

    speex_echo_cancellation(canceller->echoState, micFrame, referenceFrame, outputFrame);

#if DIAG_LOG_ENABLED
    recordEchoFrame(micFrame, referenceFrame, outputFrame, sampleCount);
#endif

    speex_preprocess_run(canceller->preprocessState, outputFrame);
}
#endif  /* AEC_SPEEX_ENABLED */

/**
 * @brief Dedicated CPU thread that reads, processes, and queues Microphone data.
 * * 1. CPU Sleep: If no call is active, sleeps indefinitely (0% CPU) via ulTaskNotifyTake.
 * 2. DSP Pipeline: Reads raw 32-bit hardware samples, applies High-Pass (removes rumble) 
 * and Low-Pass (removes hiss) filters, applies gain and hard clipping, and converts
 * to 16-bit.
 * 3. Queue: Pushes final audio to microphoneStreamBuffer, holding the backlog low so the
 * uplink does not accumulate delay.
 * * @param taskParameters FreeRTOS task parameters (unused).
 */
void microphoneTask(void *taskParameters) {
    size_t bytesReadFromHardware;
    
    int32_t rawHardwareData[DMA_FRAME_SIZE_SAMPLES];
    int16_t processedVoiceBuffer[DMA_FRAME_SIZE_SAMPLES];

    float lowPassFilterState = 0.0f;
    float previousHighPassOutput = 0.0f;
    int32_t previousHighPassInput = 0;

    UplinkPokeState uplinkPokeState = {0};

#if AEC_SPEEX_ENABLED
    // Mic frame before MICROPHONE_GAIN and the canceller's output, static to keep
    // them off the task stack (this task is their only user).
    EchoCanceller echoCanceller = {0};
    static int16_t echoMicFrame[DMA_FRAME_SIZE_SAMPLES];
    static int16_t echoOutputFrame[DMA_FRAME_SIZE_SAMPLES];
#endif

#if AEC_GATE_ENABLED
    // Conditioned samples are held in float between the two passes: the gate decision
    // needs the level of the WHOLE frame before any sample can be scaled, so the
    // int16 conversion has to be deferred until after the decision. 480 bytes of the
    // task's 6144-byte stack.
    float conditionedFrame[DMA_FRAME_SIZE_SAMPLES];

    float farEndEnvelope = 0.0f;
    float microphoneEnvelope = 0.0f;
    float gateGain = 1.0f;          // where the glide is heading this frame
    float previousGateGain = 1.0f;  // where it ended last frame, for per-sample interpolation
#endif

    while (true) {
        if (!isAudioPathOpen) {
            // OPTIMIZATION: Put this task to sleep INDEFINITELY when not in a call.
            lowPassFilterState = 0.0f;
            previousHighPassInput = 0;
            previousHighPassOutput = 0.0f;
            resetUplinkPokeState(&uplinkPokeState);

#if AEC_GATE_ENABLED
            // Reset the gate to fully OPEN between calls. Waking up mid-duck would mute
            // the first words of the next call, and a stale far-end envelope from the
            // previous call would keep it shut for AEC_FAR_RELEASE_MS after answering.
            farEndEnvelope = 0.0f;
            microphoneEnvelope = 0.0f;
            gateGain = 1.0f;
            previousGateGain = 1.0f;
            farEndPeakLevel = 0.0f;
#endif
#if AEC_SPEEX_ENABLED
            // Its memory goes back to the heap, for music, until the next call.
            destroyEchoCanceller(&echoCanceller);
#endif

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
            drainMicQueue(rawHardwareData, sizeof(rawHardwareData));

#if AEC_SPEEX_ENABLED
            // After the drain, so the reference is aligned with the next mic frame. Only
            // if the call is still up: a notification left pending by a mid-call codec
            // change also wakes this task once the call has ended, at the music rate.
            if (isAudioPathOpen) {
                createEchoCanceller(&echoCanceller, activeSampleRate);
                syncEchoReference(&echoCanceller);
            }
#endif

            // Ensure the software stream buffer is completely fresh before starting
            if (microphoneStreamBuffer != NULL) {
                xStreamBufferReset(microphoneStreamBuffer);
            }

            // Proceed directly into the active read loop now that the pipeline is clean
            continue; 
        }

#if AEC_SPEEX_ENABLED
        // A codec change mid-call (mSBC <-> CVSD) reclocks both channels and rebuilds
        // the TX one: start the canceller again at the new rate and re-align.
        uint32_t sampleRateNow = activeSampleRate;
        bool isSampleRateChanged = (echoCanceller.sampleRate != sampleRateNow);
        if (isSampleRateChanged && isAudioPathOpen) {
            drainMicQueue(rawHardwareData, sizeof(rawHardwareData));
            createEchoCanceller(&echoCanceller, sampleRateNow);
            syncEchoReference(&echoCanceller);
        }
#endif

        xSemaphoreTake(rxMutex, portMAX_DELAY);
        esp_err_t readResult = i2s_channel_read(rxChannel, rawHardwareData, sizeof(rawHardwareData), &bytesReadFromHardware, pdMS_TO_TICKS(100));
        
        // FIX: Safely cache the dynamic alphas to local variables while the mutex is held
        float localHighPassAlpha = activeHighPassAlpha;
        float localLowPassAlpha = activeLowPassAlpha;
        
        xSemaphoreGive(rxMutex);

        bool isReadSuccessful = (readResult == ESP_OK);
        bool hasBytes = (bytesReadFromHardware > 0);

        if (isReadSuccessful && hasBytes) {
#if DIAG_LOG_ENABLED
            int64_t frameStartMicroseconds = esp_timer_get_time();
#endif
            uint32_t numberOfSamples = bytesReadFromHardware / 4;

#if AEC_GATE_ENABLED
            float microphoneLevelAccumulator = 0.0f;
#endif

            // --- Pass 1: microphone conditioning: high-pass, low-pass, then gain and clip
            // (with the echo canceller, gain comes after it, in pass 2) ---
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

                float lowPassComponentOne = localLowPassAlpha * highPassResult;
                float inverseLowPassAlpha = 1.0f - localLowPassAlpha;
                float lowPassComponentTwo = inverseLowPassAlpha * lowPassFilterState;

                float lowPassResult = lowPassComponentOne + lowPassComponentTwo;
                lowPassFilterState = lowPassResult;

#if AEC_SPEEX_ENABLED
                // The canceller gets the mic before MICROPHONE_GAIN: clipping here would
                // make the echo non-linear, and a linear echo is what it can remove.
                float clampedMicSample = clampToSampleRange(lowPassResult);
                echoMicFrame[index] = (int16_t)clampedMicSample;
#else
                float amplifiedSample = lowPassResult * MICROPHONE_GAIN;
                // Clamp in the FLOAT domain, before the conversion (see clampToSampleRange).
                float amplifiedResult = clampToSampleRange(amplifiedSample);
#endif

#if AEC_GATE_ENABLED
                // Defer the int16 conversion to pass 2 - the gain to apply is not known
                // until the whole frame's level has been measured.
                conditionedFrame[index] = amplifiedResult;
                microphoneLevelAccumulator += fabsf(amplifiedResult);
#elif !AEC_SPEEX_ENABLED
                processedVoiceBuffer[index] = (int16_t)amplifiedResult;
#endif
            }

#if AEC_SPEEX_ENABLED
            /* ---- Pass 2: echo cancellation, residual suppression, then gain ---- */
            cancelEcho(&echoCanceller, echoMicFrame, echoOutputFrame, numberOfSamples);

            for (uint32_t index = 0; index < numberOfSamples; index++) {
                float amplifiedSample = (float)echoOutputFrame[index] * MICROPHONE_GAIN;
                float clampedSample = clampToSampleRange(amplifiedSample);
                processedVoiceBuffer[index] = (int16_t)clampedSample;
            }
#endif

#if AEC_GATE_ENABLED
            /* ---- Pass 2: voice-switched gate ----
             *
             * Runs once per frame. Total cost is a handful of multiplies plus one pass
             * over the frame, well under 1% of a core at 16 kHz.
             */

            // Consume and CLEAR the peak the downlink callback published. Clearing is
            // what makes the next frame's reading fresh rather than a stale high-water
            // mark that would hold the gate shut forever after one loud frame.
            float farEndFrameLevel = farEndPeakLevel;
            farEndPeakLevel = 0.0f;

            // Fast attack, slow release. The slow release is doing double duty: it is the
            // hangover across the gaps between the far end's words, AND it is what spans
            // the 20-50 ms acoustic loop delay. That is the whole reason this design needs
            // no delay line aligning the reference to the microphone - by the time the
            // echo of a given far-end frame reaches the mic, the envelope that frame
            // raised is still elevated, so the gate is already shut.
            float farCoefficient = (farEndFrameLevel > farEndEnvelope)
                                 ? gateFarAttackCoefficient
                                 : gateFarReleaseCoefficient;
            farEndEnvelope += farCoefficient * (farEndFrameLevel - farEndEnvelope);

            float microphoneFrameLevel = microphoneLevelAccumulator / (float)numberOfSamples;
            microphoneEnvelope += gateMicCoefficient * (microphoneFrameLevel - microphoneEnvelope);

            // The level the microphone has to beat before the gate believes it is hearing
            // real near-end speech rather than its own echo: expected echo level
            // (far x coupling) pushed up by the near-end margin.
            float nearEndThreshold = farEndEnvelope * gateEchoThresholdGain;

            bool isFarEndTalking  = (farEndEnvelope > AEC_FAR_ACTIVE_LEVEL);
            bool isNearEndTalking = (microphoneEnvelope > nearEndThreshold);

            float targetGain = (isFarEndTalking && !isNearEndTalking) ? gateDuckGainLinear
                                                                     : 1.0f;

            float glideCoefficient = (targetGain < gateGain) ? gateDuckOnsetCoefficient
                                                             : gateDuckRecoveryCoefficient;
            gateGain += glideCoefficient * (targetGain - gateGain);

            // Interpolate the gain ACROSS the frame rather than stepping it at the frame
            // boundary. A 32 dB step applied instantaneously every 7.5 ms is an audible
            // click on the far end; ramping it costs one add per sample and removes it.
            float gainStep = (gateGain - previousGateGain) / (float)numberOfSamples;

            for (uint32_t index = 0; index < numberOfSamples; index++) {
                float instantaneousGain = previousGateGain + gainStep * (float)index;
                // conditionedFrame[] was already clamped to int16 range in pass 1 and the
                // gate gain is never above 1.0, so this product cannot overflow. No
                // second clamp is needed here.
                processedVoiceBuffer[index] = (int16_t)(conditionedFrame[index] * instantaneousGain);
            }

            previousGateGain = gateGain;
#endif  /* AEC_GATE_ENABLED */

            uint32_t bufferPayloadSizeBytes = numberOfSamples * sizeof(int16_t);

            // NEW: hold the uplink backlog at a fixed low water mark instead of waiting
            // for the stream buffer to fill. Whatever sits in this buffer is pure
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
            // what stalled the pipeline permanently. The only exception is the back-off
            // once Bluedroid has stopped fetching (see UPLINK_UNANSWERED_POKE_LIMIT).
            pokeUplink(&uplinkPokeState);

#if DIAG_LOG_ENABLED
            int64_t frameEndMicroseconds = esp_timer_get_time();
            int64_t frameElapsedMicroseconds = frameEndMicroseconds - frameStartMicroseconds;
            recordMicFrameTime((uint32_t)frameElapsedMicroseconds);
#endif
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
    createTransmitChannel(SAMPLE_RATE_44K, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC, 0, false);
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
    receiveConfiguration.dma_desc_num = 4;   // was 3; the echo canceller lengthened the mic task, and an RX overflow silently drops samples, which walks the echo path out of alignment 
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

#if AEC_SPEEX_ENABLED
    // An overflow drops a mic frame; the echo canceller has to know, to stay aligned.
    // Registered once: the RX channel is reclocked, never rebuilt.
    i2s_event_callbacks_t receiveCallbacks = { .on_recv_q_ovf = micOverflowCallback };
    i2s_channel_register_event_callback(rxChannel, &receiveCallbacks, NULL);
#endif
}

/**
 * @brief Bootstraps the Espressif Bluedroid Bluetooth stack.
 * * Initializes controller, enables Classic Bluetooth, sets device name, registers 
 * A2DP and HFP callbacks, and makes the device discoverable for pairing.
 */
void initializeBluetoothStack(void) {
    // Hand the controller's BLE-only RAM back to the heap. This device is Classic
    // only, and the release has to happen before esp_bt_controller_init(); after it,
    // the memory is reserved for good. The DIAG heap line shows what it gained.
    esp_err_t bleReleaseResult = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (bleReleaseResult != ESP_OK) {
        ESP_LOGW(TAG, "--- BLE controller memory not released (error %d) ---", (int)bleReleaseResult);
    }

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

#if DIAG_LOG_ENABLED
    diagMutex = xSemaphoreCreateMutex();
    if (diagMutex == NULL) {
        ESP_LOGE(TAG, "--- FATAL: Failed to allocate the diagnostics mutex, halting ---");
        abort();
    }
#endif

    initializeTouchPanel();

    initializeI2sTransmitChannel();
    initializeI2sReceiveChannel();
    
    calculateFilterAlphas(SAMPLE_RATE_16K);

    initializeBluetoothStack();
     
    // Start Tasks
    // NEW: microphoneTask stack raised from 4096 - it now carries an extra float frame
    // buffer and runs the echo canceller's float maths.
    // touchPanelTask and audioOutputTask are sized from the DIAG stack readings
    // (2026-10-19): they used ~0.4 KB and ~1.7 KB, so each keeps over 1 KB spare.
    xTaskCreatePinnedToCore(touchPanelTask, "touchPanelTask", 1536, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(microphoneTask, "microphoneTask", 6144, NULL, 10, &micTaskHandle, 1);
    xTaskCreatePinnedToCore(audioOutputTask, "audioOutputTask", 3072, NULL, 15, NULL, 1);

#if DIAG_LOG_ENABLED
    // Lowest priority of ours, so its UART output only runs when the audio tasks are idle.
    xTaskCreatePinnedToCore(diagnosticsTask, "diagnosticsTask", 4096, NULL, 2, NULL, 1);
    logHeapUsage("after init");
#endif

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
