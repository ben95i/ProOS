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
 * microphoneStreamBuffer is 2048 bytes = 1024 samples = 64 ms at 16 kHz. The original
 * code only discarded audio once that was completely full, so whatever backlog built up
 * at call start stayed there for the rest of the call and was added to the uplink delay.
 * The backlog is now held to a fixed low water mark, discarding the OLDEST whole frames.
 */
#define HFP_UPLINK_MAX_BACKLOG_BYTES  480   // 2 mic frames

// The original 250 ms settle delay costs a quarter of a second of the near-end
// talker's first words. 80 ms is enough for the baseband to settle.
#define HFP_MIC_STARTUP_DELAY_MS      80

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
 * HOW TO TUNE IT IN THE CAR
 * -------------------------
 * Leave AEC_GATE_LOG_ENABLED at 1 for the first few calls. Once a second the mic
 * task prints:
 *
 *   GATE far=1234 mic=5678 echoRef=617 gain=-32.0dB duck=71%
 *
 *   far     - level of the far end's voice as sent to the DAC
 *   mic     - level at the microphone after HPF/LPF/MICROPHONE_GAIN
 *   echoRef - the near-end threshold the mic level is being compared against
 *   gain    - what the uplink is currently scaled by
 *   duck    - percentage of the last second spent ducked
 *
 * Tune in this order:
 *
 *  1. AEC_FAR_ACTIVE_LEVEL. Sit in a silent call and watch "far" with nobody
 *     speaking. Set this ~3x above whatever idle value you see. Too low and the
 *     gate latches shut on line noise and you are permanently muted; too high and
 *     quiet talkers get no suppression at all.
 *
 *  2. AEC_ECHO_COUPLING_DB. Have the far end talk while you stay silent, and
 *     compare "mic" against "far". mic/far expressed in dB IS your real acoustic
 *     coupling. Put that number here. Every physical change - moving the mic,
 *     changing head unit volume, AEC_SPEAKER_ATTENUATION_DB below - moves it.
 *
 *  3. AEC_NEAREND_MARGIN. Now have both ends talk. If the far end still hears
 *     echo when you both speak, raise it (harder to open the gate, more of a
 *     one-way radio). If your own voice gets chopped when you talk over them,
 *     lower it. This is the single knob that trades echo against duplex feel.
 *
 *  4. AEC_DUCK_DEPTH_DB last, and only if a residue is still audible. -32 dB is
 *     already inaudible under road noise; going to -60 or muting outright makes
 *     the far end think the line has dropped, because they lose your background
 *     noise entirely. Keeping some noise through is what makes it sound alive.
 */

// Master switch. Set to 0 to compile the gate out entirely and get the previous
// straight-through microphone path back for an A/B comparison.
#define AEC_GATE_ENABLED              1

// Far-end level, in mean |sample| units (16-bit full scale = 32767), above which
// the far end counts as talking. See step 1 above.
#define AEC_FAR_ACTIVE_LEVEL          500.0f

// Speaker-to-microphone coupling, in dB, MEASURED AT THE POINT THE GATE SEES IT -
// that is, after MICROPHONE_GAIN has already been applied. It is NOT the raw
// acoustic coupling, and it is not necessarily negative.
//
// The 0.0 dB default is the arithmetic of this specific build: a car cabin with
// the mic a metre or so off-axis from the speaker is roughly -6 dB of acoustic
// coupling, and MICROPHONE_GAIN of 2.0 puts +6 dB straight back on. Net zero.
// Change MICROPHONE_GAIN and this has to move with it, dB for dB.
//
// Higher = the gate assumes a stronger echo and stays shut more readily.
// See step 2 above for how to measure yours instead of trusting this.
#define AEC_ECHO_COUPLING_DB          (-20.0f)

// How far the microphone must exceed the expected echo level, in dB, before the
// gate accepts it as genuine near-end speech and opens back up. See step 3.
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
// echo, at zero CPU cost. 0.0 disables it. Note that lowering this changes your
// real coupling, so re-measure AEC_ECHO_COUPLING_DB (step 2) after touching it.
#define AEC_SPEAKER_ATTENUATION_DB    -2.0f

// Once-per-second tuning log described above. Set to 0 for production.
#define AEC_GATE_LOG_ENABLED          0
#define AEC_GATE_LOG_PERIOD_MS        1000

/* * ============================================================================
 * Calibration assistant
 * ============================================================================
 *
 * Set AEC_CAL_ENABLED to 1 and drive the car normally with a call in progress.
 * Every AEC_CAL_PERIOD_MS the microphone task MEASURES your real acoustic
 * coupling and prints the value to type into AEC_ECHO_COUPLING_DB, plus a verdict
 * on whether the gate is currently in its useful range.
 *
 * How the measurement works, and why it can be trusted:
 *
 * The microphone envelope contains echo + your voice + road noise. Your voice and
 * road noise can only ever ADD to it, never subtract - so across many frames of
 * far-end speech, the MINIMUM of (mic / far) is the cleanest available estimate of
 * pure echo coupling. That minimum is what gets reported. You do not have to stay
 * silent for it to work, though a quiet stretch makes it converge faster.
 *
 * Frames are only counted when the far end is solidly mid-utterance
 * (AEC_CAL_MIN_FAR_LEVEL, plus a run-length requirement). Frames at the tail of a
 * far-end utterance are excluded deliberately: the far envelope decays over
 * AEC_FAR_RELEASE_MS while the microphone envelope falls in AEC_MIC_ENVELOPE_MS,
 * so the ratio dives toward zero there and would drag the minimum far below the
 * true coupling.
 *
 * The verdict is based on headroom = (configured threshold) - (measured coupling):
 *
 *   headroom < AEC_CAL_HEADROOM_LOW_DB   the gate's near-end threshold sits BELOW
 *                                        your actual echo, so the echo itself
 *                                        keeps opening the gate -> echo leaks
 *   headroom in the band                 IN RANGE
 *   headroom > AEC_CAL_HEADROOM_HIGH_DB  the threshold is so far above the echo
 *                                        that your own voice struggles to clear
 *                                        it -> you get chopped when you talk
 */
#define AEC_CAL_ENABLED               0

// How often to report. Long enough to see several far-end utterances.
#define AEC_CAL_PERIOD_MS             5000

// Only measure coupling on solid far-end speech, not on the edges of it.
#define AEC_CAL_MIN_FAR_LEVEL         (AEC_FAR_ACTIVE_LEVEL * 2.0f)

// Consecutive far-active frames required before a frame counts, so the
// measurement lands inside an utterance rather than at its onset - the echo of a
// given far-end frame does not reach the microphone for 20-50 ms.
#define AEC_CAL_MIN_RUN_FRAMES        8

// Both envelopes must be on a PLATEAU - changing by less than this fraction from
// the previous frame - before a frame counts. This is what makes the measurement
// immune to the 20-50 ms lag between the two: during a sustained vowel both
// envelopes are flat, so it no longer matters that one leads the other. Without
// it the ratio dips at every syllable onset, because the far envelope jumps on a
// 2 ms attack while the echo of that jump has not arrived at the mic yet.
#define AEC_CAL_PLATEAU_TOLERANCE     0.15f

// The far end must also not be in its RELEASE tail. The plateau test alone does
// not catch this: the far envelope decays with AEC_FAR_RELEASE_MS, which is only
// about 4% per frame and passes as "flat", while the microphone has already gone
// silent - giving absurd readings around -40 dB. A frame only counts if the
// instantaneous far level is still holding the envelope up.
#define AEC_CAL_STEADY_RATIO          0.8f

// Minimum qualifying frames before a measurement is reported at all.
#define AEC_CAL_MIN_FRAMES            40

// Coupling is reported as the MEDIAN of the per-frame mic/far ratio, accumulated
// into a 1 dB histogram. A median rather than a minimum because the echo is a
// convolution, not a scaled copy: its level relative to the far end fluctuates
// naturally, and a minimum simply chases the low tail of that fluctuation. A
// median is also robust to the driver talking during the measurement, since
// near-end speech only pushes ratios upward.
#define AEC_CAL_HIST_MIN_DB           (-40)
#define AEC_CAL_HIST_MAX_DB           (40)
#define AEC_CAL_HIST_BINS             (AEC_CAL_HIST_MAX_DB - AEC_CAL_HIST_MIN_DB + 1)

// Fraction of qualifying far-end frames allowed to exceed the gate's near-end
// threshold before the verdict calls it a leak. This is measured directly rather
// than modelled: it IS how often the echo will prise the gate open.
#define AEC_CAL_MAX_LEAK_PERCENT      15

// The healthy headroom band, and the point the recommendation aims at.
#define AEC_CAL_HEADROOM_LOW_DB       3.0f
#define AEC_CAL_HEADROOM_HIGH_DB      12.0f
#define AEC_CAL_TARGET_HEADROOM_DB    6.0f

// AEC_FAR_ACTIVE_LEVEL should sit this far above the quietest far-end level seen,
// or it will latch on line noise and mute you for the whole call.
#define AEC_CAL_FAR_IDLE_RATIO        3.0f

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

// Number of microphone frames between tuning log lines.
static uint32_t gateLogPeriodFrames = 0;
// Number of microphone frames between calibration reports.
static uint32_t gateCalPeriodFrames = 0;

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

    gateLogPeriodFrames = (uint32_t)((AEC_GATE_LOG_PERIOD_MS * 0.001f) / frameSeconds);
    if (gateLogPeriodFrames == 0) {
        gateLogPeriodFrames = 1;
    }

    gateCalPeriodFrames = (uint32_t)((AEC_CAL_PERIOD_MS * 0.001f) / frameSeconds);
    if (gateCalPeriodFrames == 0) {
        gateCalPeriodFrames = 1;
    }

    ESP_LOGI(TAG, "--- DSP Updated | Fs: %lu Hz | HP Alpha: %.4f | LP Alpha: %.4f ---", 
             sampleRate, activeHighPassAlpha, activeLowPassAlpha);
    ESP_LOGI(TAG, "--- Gate | frame %.2f ms | duck %.1f dB | near-end threshold %.3f x far ---",
             frameSeconds * 1000.0f, (float)AEC_DUCK_DEPTH_DB, gateEchoThresholdGain);
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
static void createTransmitChannel(uint32_t sampleRate, uint32_t descriptorCount,
                                  uint32_t framesPerDescriptor, uint32_t preloadDescriptors) {
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
        createTransmitChannel(sampleRate, CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC, CALL_TX_PRELOAD_DESC);
        ESP_LOGI(TAG, "--- Tx DMA -> CALL profile (%d x %d frames) ---",
                 CALL_TX_DESC_COUNT, CALL_TX_FRAMES_PER_DESC);
    } else {
        createTransmitChannel(sampleRate, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC, 0);
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

    // Running sum of |sample| for the voice-switched gate. This is measured on what we
    // are about to hand the DAC - i.e. after any speaker attenuation - because that, not
    // the raw SCO payload, is what physically generates the echo.
    uint32_t farEndLevelAccumulator = 0;

    for (uint32_t index = 0; index < numberOfSamples; index++) {
        uint32_t leftChannelIndex = index * 2;
        uint32_t rightChannelIndex = leftChannelIndex + 1;
        
        int16_t currentSample = pcm16Data[index];

#if AEC_GATE_ENABLED
        // Optional fixed trim on the speaker path (AEC_SPEAKER_ATTENUATION_DB). Skipped
        // entirely when set to 0 dB so the default build is bit-identical to before.
        if (speakerAttenuationLinear < 0.999f) {
            float attenuated = (float)currentSample * speakerAttenuationLinear;
            currentSample = (int16_t)attenuated;
        }

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
    uint32_t gateFrameCounter = 0;
    uint32_t gateDuckedFrames = 0;

#if AEC_CAL_ENABLED
    // Calibration accumulators. All reset at the end of every reporting window so
    // each report describes the last AEC_CAL_PERIOD_MS, not the whole call.
    uint16_t calRatioHistogram[AEC_CAL_HIST_BINS];   // 1 dB bins of mic/far, 162 bytes
    uint32_t calQualifyingFrames = 0;     // frames that met every measurement condition
    uint32_t calLeakFrames = 0;           // qualifying frames that cleared the gate threshold
    float    calFarQuietestLevel = 0.0f;  // lowest far-end envelope seen this window
    uint32_t calFarActiveFrames = 0;
    uint32_t calFarActiveDucked = 0;
    uint32_t calFrameCounter = 0;
    uint32_t calFarActiveRun = 0;         // consecutive frames the far end has been active
    float    calPreviousFarEnvelope = 0.0f;
    float    calPreviousMicEnvelope = 0.0f;
#endif
#endif

    while (true) {
        if (!isAudioPathOpen) {
            // OPTIMIZATION: Put this task to sleep INDEFINITELY when not in a call.
            lowPassFilterState = 0.0f;
            previousHighPassInput = 0;
            previousHighPassOutput = 0.0f;

#if AEC_GATE_ENABLED
            // Reset the gate to fully OPEN between calls. Waking up mid-duck would mute
            // the first words of the next call, and a stale far-end envelope from the
            // previous call would keep it shut for AEC_FAR_RELEASE_MS after answering.
            farEndEnvelope = 0.0f;
            microphoneEnvelope = 0.0f;
            gateGain = 1.0f;
            previousGateGain = 1.0f;
            gateFrameCounter = 0;
            gateDuckedFrames = 0;
            farEndPeakLevel = 0.0f;
#if AEC_CAL_ENABLED
            memset(calRatioHistogram, 0, sizeof(calRatioHistogram));
            calQualifyingFrames = 0;
            calLeakFrames = 0;
            calFarQuietestLevel = 1.0e9f;   // sentinel: first real frame replaces it
            calFarActiveFrames = 0;
            calFarActiveDucked = 0;
            calFrameCounter = 0;
            calFarActiveRun = 0;
            calPreviousFarEnvelope = 0.0f;
            calPreviousMicEnvelope = 0.0f;
#endif
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

#if AEC_GATE_ENABLED
            float microphoneLevelAccumulator = 0.0f;
#endif

            // --- Pass 1: microphone conditioning: high-pass, low-pass, gain, clip ---
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

                float amplifiedResult = lowPassResult * MICROPHONE_GAIN;

                // Clamp in the FLOAT domain, before the conversion. Converting a NaN or an
                // out-of-range float to int32_t is undefined behaviour, and on Xtensa it
                // produces a value the integer-side clamps cannot recognise - one bad
                // sample becomes a constant, which the far end hears as dead silence
                // rather than as a glitch. The compound test below is false for NaN as
                // well as for out-of-range values, so both land in the recovery branch,
                // and NaN (neither > 0 nor < 0) resolves to zero.
                if (!(amplifiedResult >= -32768.0f && amplifiedResult <= 32767.0f)) {
                    amplifiedResult = (amplifiedResult > 0.0f) ?  32767.0f
                                    : (amplifiedResult < 0.0f) ? -32768.0f
                                    : 0.0f;
                }

#if AEC_GATE_ENABLED
                // Defer the int16 conversion to pass 2 - the gain to apply is not known
                // until the whole frame's level has been measured.
                conditionedFrame[index] = amplifiedResult;
                microphoneLevelAccumulator += fabsf(amplifiedResult);
#else
                processedVoiceBuffer[index] = (int16_t)amplifiedResult;
#endif
            }

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

#if AEC_CAL_ENABLED
            /* ---- Calibration assistant ---- */

            // Track how long the far end has been continuously active, so the coupling
            // measurement can be restricted to the middle of an utterance.
            if (isFarEndTalking) {
                calFarActiveRun++;
                calFarActiveFrames++;
                if (targetGain < 1.0f) {
                    calFarActiveDucked++;
                }
            } else {
                calFarActiveRun = 0;
            }

            // Lowest far-end envelope this window - used to check that
            // AEC_FAR_ACTIVE_LEVEL sits safely above the idle line level.
            if (farEndEnvelope < calFarQuietestLevel) {
                calFarQuietestLevel = farEndEnvelope;
            }

            // A frame qualifies for the coupling measurement only if the far end is
            // loud, has been talking for a while, and is NOT in its decay tail. The
            // decay test is the important one: the far envelope falls over
            // AEC_FAR_RELEASE_MS but the mic envelope falls over AEC_MIC_ENVELOPE_MS,
            // so at the end of every utterance the ratio collapses toward zero and
            // would drag the minimum well below the true coupling.
            bool isFarLoudEnough = (farEndEnvelope > AEC_CAL_MIN_FAR_LEVEL);
            bool isFarSustained  = (calFarActiveRun > AEC_CAL_MIN_RUN_FRAMES);
            bool isFarNotDecaying = (farEndFrameLevel > AEC_CAL_STEADY_RATIO * farEndEnvelope);

            // Plateau test on BOTH envelopes - see AEC_CAL_PLATEAU_TOLERANCE.
            bool isFarOnPlateau = (calPreviousFarEnvelope > 0.0f) &&
                (fabsf(farEndEnvelope - calPreviousFarEnvelope)
                     <= AEC_CAL_PLATEAU_TOLERANCE * calPreviousFarEnvelope);
            bool isMicOnPlateau = (calPreviousMicEnvelope > 0.0f) &&
                (fabsf(microphoneEnvelope - calPreviousMicEnvelope)
                     <= AEC_CAL_PLATEAU_TOLERANCE * calPreviousMicEnvelope);

            calPreviousFarEnvelope = farEndEnvelope;
            calPreviousMicEnvelope = microphoneEnvelope;

            if (isFarLoudEnough && isFarSustained && isFarNotDecaying &&
                isFarOnPlateau && isMicOnPlateau && microphoneEnvelope > 0.0f) {

                float couplingDb = 20.0f * log10f(microphoneEnvelope / farEndEnvelope);

                int bin = (int)lroundf(couplingDb) - AEC_CAL_HIST_MIN_DB;
                if (bin < 0) bin = 0;
                if (bin >= AEC_CAL_HIST_BINS) bin = AEC_CAL_HIST_BINS - 1;
                if (calRatioHistogram[bin] < 0xFFFF) {
                    calRatioHistogram[bin]++;
                }

                // Did the echo alone clear the gate's near-end threshold on this
                // frame? If so the gate opened on its own echo. Counted directly.
                if (microphoneEnvelope > nearEndThreshold) {
                    calLeakFrames++;
                }
                calQualifyingFrames++;
            }

            calFrameCounter++;
            if (calFrameCounter >= gateCalPeriodFrames) {
                if (calQualifyingFrames >= AEC_CAL_MIN_FRAMES) {
                    // Median of the histogram.
                    uint32_t half = calQualifyingFrames / 2;
                    uint32_t running = 0;
                    int medianBin = 0;
                    for (int b = 0; b < AEC_CAL_HIST_BINS; b++) {
                        running += calRatioHistogram[b];
                        if (running >= half) { medianBin = b; break; }
                    }
                    float measuredCouplingDb = (float)(medianBin + AEC_CAL_HIST_MIN_DB);
                    unsigned long leakPercent = (100UL * calLeakFrames) / calQualifyingFrames;

                    float configuredThresholdDb = AEC_ECHO_COUPLING_DB + AEC_NEAREND_MARGIN;
                    float headroomDb = configuredThresholdDb - measuredCouplingDb;

                    // Aim the recommendation at the middle of the healthy band. With
                    // AEC_NEAREND_MARGIN at its default of 6.0 this reduces to
                    // "set AEC_ECHO_COUPLING_DB to the measured coupling".
                    float recommendedCouplingDb = measuredCouplingDb
                                                + AEC_CAL_TARGET_HEADROOM_DB
                                                - AEC_NEAREND_MARGIN;

                    if (leakPercent > AEC_CAL_MAX_LEAK_PERCENT) {
                        ESP_LOGW(TAG, "GATE CAL  coupling %+.0f dB | headroom %+.1f dB | leak %lu%% | "
                                      "ECHO LEAKING -> set AEC_ECHO_COUPLING_DB to %+.0f",
                                 measuredCouplingDb, headroomDb, leakPercent, recommendedCouplingDb);
                    } else if (headroomDb > AEC_CAL_HEADROOM_HIGH_DB) {
                        ESP_LOGW(TAG, "GATE CAL  coupling %+.0f dB | headroom %+.1f dB | leak %lu%% | "
                                      "OVER-DUCKING -> set AEC_ECHO_COUPLING_DB to %+.0f",
                                 measuredCouplingDb, headroomDb, leakPercent, recommendedCouplingDb);
                    } else {
                        ESP_LOGI(TAG, "GATE CAL  coupling %+.0f dB | headroom %+.1f dB | leak %lu%% | "
                                      "*** IN RANGE *** (headroom band %.0f to %.0f dB, leak under %d%%)",
                                 measuredCouplingDb, headroomDb, leakPercent,
                                 (float)AEC_CAL_HEADROOM_LOW_DB, (float)AEC_CAL_HEADROOM_HIGH_DB,
                                 (int)AEC_CAL_MAX_LEAK_PERCENT);
                    }
                } else {
                    ESP_LOGI(TAG, "GATE CAL  not enough steady far-end speech to measure "
                                  "(%lu/%d qualifying frames) - keep the call going",
                             (unsigned long)calQualifyingFrames, (int)AEC_CAL_MIN_FRAMES);
                }

                // Far-end activity threshold sanity check, reported every window
                // because it is the constant that silently mutes the whole call if
                // it latches onto line noise.
                if (calFarQuietestLevel > 1.0e8f) {
                    calFarQuietestLevel = 0.0f;   // no frames seen this window
                }
                float farIdleHeadroom = (calFarQuietestLevel > 1.0f)
                                      ? (AEC_FAR_ACTIVE_LEVEL / calFarQuietestLevel)
                                      : 999.0f;
                unsigned long duckPercent = calFarActiveFrames
                                          ? (100UL * calFarActiveDucked) / calFarActiveFrames
                                          : 0UL;

                if (farIdleHeadroom < AEC_CAL_FAR_IDLE_RATIO) {
                    ESP_LOGW(TAG, "GATE CAL  far idle %.0f vs threshold %.0f (%.1fx) | "
                                  "THRESHOLD TOO LOW -> raise AEC_FAR_ACTIVE_LEVEL to %.0f | duck %lu%%",
                             calFarQuietestLevel, (float)AEC_FAR_ACTIVE_LEVEL, farIdleHeadroom,
                             calFarQuietestLevel * AEC_CAL_FAR_IDLE_RATIO, duckPercent);
                } else {
                    ESP_LOGI(TAG, "GATE CAL  far idle %.0f vs threshold %.0f (%.1fx) OK | duck %lu%% of far-active",
                             calFarQuietestLevel, (float)AEC_FAR_ACTIVE_LEVEL, farIdleHeadroom, duckPercent);
                }

                calFrameCounter = 0;
                calQualifyingFrames = 0;
                calLeakFrames = 0;
                memset(calRatioHistogram, 0, sizeof(calRatioHistogram));
                calFarActiveFrames = 0;
                calFarActiveDucked = 0;
                calFarQuietestLevel = 1.0e9f;
            }
#endif  /* AEC_CAL_ENABLED */

#if AEC_GATE_LOG_ENABLED
            gateFrameCounter++;
            if (targetGain < 1.0f) {
                gateDuckedFrames++;
            }
            if (gateFrameCounter >= gateLogPeriodFrames) {
                float reportedGainDb = 20.0f * log10f((gateGain > 1e-6f) ? gateGain : 1e-6f);
                ESP_LOGI(TAG, "GATE far=%.0f mic=%.0f echoRef=%.0f gain=%.1fdB duck=%lu%%",
                         farEndEnvelope, microphoneEnvelope, nearEndThreshold, reportedGainDb,
                         (unsigned long)((100UL * gateDuckedFrames) / gateFrameCounter));
                gateFrameCounter = 0;
                gateDuckedFrames = 0;
            }
#endif
#endif  /* AEC_GATE_ENABLED */

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
    createTransmitChannel(SAMPLE_RATE_44K, MUSIC_TX_DESC_COUNT, MUSIC_TX_FRAMES_PER_DESC, 0);
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
