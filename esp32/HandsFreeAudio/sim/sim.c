/*
 * Host simulator for the HandsFreeAudio firmware.
 *
 * Compiles ../main/main.c unmodified against the mocks in stubs/ (FreeRTOS tasks as
 * pthreads, I2S with a DMA descriptor model, Bluedroid callbacks, NVS) and drives it
 * with a simulated I2S clock, a car-cabin echo model, a phone (Audio Gateway) and two
 * talkers. Time runs simSpeedup times faster than real time.
 *
 * Build and run from this folder (see the Makefile):
 *   make            builds ./build/sim
 *   make test       runs every scenario
 *   ./build/sim <scenario> [speedup] [verbose]
 *     scenario  1 2 3 4 5 9 10 (listed in printUsage below)
 *     speedup   simulated seconds per real second, default 20
 *     verbose   any third argument prints every firmware log line to stderr
 */
#define _GNU_SOURCE
#include "espmock.h"
#include <pthread.h>
#include <math.h>

/* The firmware under test, compiled unmodified. */
#include "main.c"


/* ============================================================
 * Acoustic model of the car cabin
 * ============================================================ */
#define ECHO_TAPS      140
#define PLAYED_FIFO    32768
static float echoImpulse[ECHO_TAPS];
static int   echoDelaySamples = 180;
static float playedFifo[PLAYED_FIFO];
static uint32_t playedWrite = 0, playedRead = 0;
static float    echoLine[PLAYED_FIFO];
static uint32_t echoLineIdx = 0;

static uint32_t rngState = 2463534242u;
static float frand(void) {
    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
    return ((float)(int32_t)rngState) * (1.0f / 2147483648.0f);
}

/* ============================================================
 * Speech generators and the talking schedule
 * ============================================================ */
static volatile int farTalking = 0, nearTalking = 0;
static volatile int nearEndEnabled = 1;
static long sampleClock = 0;

static float speech(float amp, int active, float *s1, float *s2, long n) {
    float r = frand();
    *s1 += 0.25f * (r - *s1);
    *s2 += 0.02f * (*s1 - *s2);
    float env = active ? (0.55f + 0.45f * sinf((float)n * 0.0011f)) : 0.0f;
    return (*s1 - *s2) * 4.0f * amp * env;
}

/* ============================================================
 * Instrumentation
 * ============================================================ */
typedef struct {
    long   uplinkFrames, uplinkZeroReturns;
    double uplinkEnergy;      long uplinkSamples;
    double micEchoEnergy;     long micSamples;   /* echo present at the microphone */
    long   deadWindows, worstDeadRunMs;
    long   uplinkGapWorstMs, uplinkStarvedIntervals;
    double farOnlyUplinkEnergy; long farOnlySamples;
    double farOnlyEchoEnergy;   long farOnlyEchoSamples;
    long   downlinkFrames;
    double txQueueMaxMs, txQueueSumMs; long txQueueSamples;
    long   callCount;
} Stats;
static Stats st;

static uint64_t lastUplinkUs = 0;
static double   windowEnergy = 0; static long windowSamples = 0;
static int      windowNearTalking = 0;
static uint64_t windowStartUs = 0;
static long     deadRunMs = 0;

/* ============================================================
 * Simulated I2S hardware clock: consumes the DAC, produces the mic
 * ============================================================ */
static void *hardwareThread(void *arg) {
    (void)arg;
    static uint8_t txBuf[65536], rxBuf[65536];
    double txAccum = 0, rxAccum = 0;
    uint64_t last = simNowUs();
    float e1 = 0, e2 = 0;

    while (!mockShutdown) {
        simSleepUs(500);
        uint64_t now = simNowUs();
        double dt = (double)(now - last) / 1e6;
        last = now;
        if (dt > 0.2) dt = 0.2;

        /* ---- DAC: pull whole descriptors and "play" them ---- */
        i2s_chan_handle_t tx = mockTxChan;
        if (tx && mockI2sEnabled(tx)) {
            uint32_t rate = mockI2sRate(tx);
            int descBytes = mockI2sDescBytes(tx);
            int descFrames = descBytes / 4;                 /* stereo 16-bit */
            txAccum += dt * rate;
            while (txAccum >= descFrames && descFrames > 0) {
                txAccum -= descFrames;
                int got = mockI2sTxPop(tx, txBuf);
                int16_t *s = (int16_t *)txBuf;
                for (int f = 0; f < descFrames; f++) {
                    /* An underrun plays auto-cleared zeros: exactly what the DAC emits. */
                    float v = (got > 0) ? (float)s[f * 2 + 1] : 0.0f;   /* right channel = call audio */
                    playedFifo[playedWrite & (PLAYED_FIFO - 1)] = v;
                    playedWrite++;
                }
                size_t q = mockI2sTxQueuedBytes(tx);
                double ms = rate ? (double)(q / 4) * 1000.0 / rate : 0;
                if (ms > st.txQueueMaxMs) st.txQueueMaxMs = ms;
                st.txQueueSumMs += ms; st.txQueueSamples++;
            }
        } else {
            txAccum = 0;
        }

        /* ---- Microphone: echo + near-end voice + noise ---- */
        /* TX consumption and RX generation both happen in this one thread, so the
           played-sample delay line needs no locking and no per-sample copying. */
        i2s_chan_handle_t rx = mockRxChan;
        if (rx && mockI2sEnabled(rx)) {
            uint32_t rate = mockI2sRate(rx);
            int descBytes = mockI2sDescBytes(rx);
            int descFrames = descBytes / 4;                 /* mono 32-bit */
            rxAccum += dt * rate;
            while (rxAccum >= descFrames && descFrames > 0) {
                rxAccum -= descFrames;
                int32_t *d = (int32_t *)rxBuf;
                for (int f = 0; f < descFrames; f++) {
                    float played = 0.0f;
                    if ((int32_t)(playedWrite - playedRead) > 0) {
                        played = playedFifo[playedRead & (PLAYED_FIFO - 1)];
                        playedRead++;
                    }
                    echoLine[echoLineIdx & (PLAYED_FIFO - 1)] = played;
                    echoLineIdx++;

                    float echo = 0;
                    uint32_t base = echoLineIdx - 1 - (uint32_t)echoDelaySamples;
                    for (int k = 0; k < ECHO_TAPS; k++)
                        echo += echoImpulse[k] * echoLine[(base - k) & (PLAYED_FIFO - 1)];
                    echo += 0.00002f * echo * fabsf(echo);

                    float near = speech(3200.0f, nearTalking, &e1, &e2, sampleClock++);
                    float v = echo + near + frand() * 25.0f;
                    if (v > 32000) v = 32000;
                    if (v < -32000) v = -32000;
                    d[f] = ((int32_t)v) << 16;

                    st.micEchoEnergy += (double)echo * echo; st.micSamples++;
                    if (farTalking && !nearTalking) { st.farOnlyEchoEnergy += (double)echo * echo; st.farOnlyEchoSamples++; }
                }
                mockI2sRxPush(rx, rxBuf);
            }
        } else {
            rxAccum = 0;
            playedRead = playedWrite;
        }
    }
    return NULL;
}

/* ============================================================
 * Simulated phone (Audio Gateway). Single-threaded, like the BTC task:
 * a blocking I2S write inside the receive callback really does delay the
 * outgoing-data fetch, exactly as it would on the device.
 * ============================================================ */
static volatile int scoActive = 0, scoWideband = 1;
static double btPpm = 0.0;

static void *phoneThread(void *arg) {
    (void)arg;
    static int16_t downlink[256];
    static uint8_t uplink[512];
    float f1 = 0, f2 = 0;
    long n = 0;
    uint64_t next = simNowUs();

    while (!mockShutdown) {
        double interval = 7500.0 * (1.0 + btPpm / 1e6);
        uint64_t now = simNowUs();
        if (now < next) { simSleepUs(next - now > 2000 ? 2000 : next - now); continue; }
        next += (uint64_t)interval;
        /* Do NOT clamp to now on every overrun - that silently discards accumulated
           lateness and makes the simulated phone deliver below line rate. Only resync
           if we have fallen grossly behind. */
        if (now > next + 100000) next = now;

        if (!scoActive) { simSleepUs(2000); next = simNowUs(); lastUplinkUs = 0;
                          windowEnergy = 0; windowSamples = 0; windowNearTalking = 0;
                          windowStartUs = simNowUs(); deadRunMs = 0; continue; }

        int rate = scoWideband ? 16000 : 8000;
        int frameSamples = rate * 75 / 10000;             /* 7.5 ms: 120 @16k, 60 @8k */
        for (int i = 0; i < frameSamples; i++) {
            float v = speech(6500.0f, farTalking, &f1, &f2, n++);
            if (v > 32000) v = 32000;
            if (v < -32000) v = -32000;
            downlink[i] = (int16_t)v;
        }
        if (mockHfRecvCb) { mockHfRecvCb((const uint8_t *)downlink, (uint32_t)frameSamples * 2); st.downlinkFrames++; }

        /* An eSCO link carries exactly one frame per interval no matter how many
           times the application says data is ready, so pull at most one. */
        long pend = __sync_fetch_and_add(&mockOutgoingReadyPending, 0);
        if (pend > 4) __sync_fetch_and_sub(&mockOutgoingReadyPending, pend - 4);
        if (pend > 0 && mockHfSendCb) {
            __sync_fetch_and_sub(&mockOutgoingReadyPending, 1);
            uint32_t got = mockHfSendCb(uplink, (uint32_t)frameSamples * 2);
            if (got == 0) st.uplinkZeroReturns++;
            else {
                st.uplinkFrames++;
                uint64_t t = simNowUs();
                if (lastUplinkUs) {
                    long gap = (long)((t - lastUplinkUs) / 1000);
                    if (gap > st.uplinkGapWorstMs) st.uplinkGapWorstMs = gap;
                }
                lastUplinkUs = t;
                int16_t *sp = (int16_t *)uplink;
                for (uint32_t i = 0; i < got / 2; i++) {
                    double v = sp[i];
                    st.uplinkEnergy += v * v; st.uplinkSamples++;
                    windowEnergy += v * v; windowSamples++;
                    if (farTalking && !nearTalking) { st.farOnlyUplinkEnergy += v * v; st.farOnlySamples++; }
                }
            }
        } else if (pend == 0) {
            st.uplinkStarvedIntervals++;
        }

        /* 200 ms "can the caller hear me" windows */
        if (nearTalking && scoActive) windowNearTalking = 1;
        if (simNowUs() - windowStartUs > 200000) {
            if (windowNearTalking && windowSamples > 100) {
                double rms = sqrt(windowEnergy / windowSamples);
                if (rms < 30.0) { st.deadWindows++; deadRunMs += 200; if (deadRunMs > st.worstDeadRunMs) st.worstDeadRunMs = deadRunMs; }
                else deadRunMs = 0;
            } else if (windowNearTalking && windowSamples <= 100) {
                st.deadWindows++; deadRunMs += 200;
                if (deadRunMs > st.worstDeadRunMs) st.worstDeadRunMs = deadRunMs;
            } else {
                deadRunMs = 0;
            }
            windowEnergy = 0; windowSamples = 0; windowNearTalking = 0; windowStartUs = simNowUs();
        }
    }
    return NULL;
}

/* ============================================================
 * Call / A2DP control
 * ============================================================ */
static void hf(esp_hf_client_cb_event_t e, esp_hf_client_cb_param_t p) { if (mockHfCb) mockHfCb(e, &p); }

static void startCall(int wideband) {
    esp_hf_client_cb_param_t p;
    memset(&p, 0, sizeof(p));
    p.call_setup.status = ESP_HF_CALL_SETUP_STATUS_INCOMING;
    hf(ESP_HF_CLIENT_CIND_CALL_SETUP_EVT, p);
    simSleepUs(300000);
    memset(&p, 0, sizeof(p)); p.call_setup.status = ESP_HF_CALL_SETUP_STATUS_IDLE;
    hf(ESP_HF_CLIENT_CIND_CALL_SETUP_EVT, p);
    memset(&p, 0, sizeof(p)); p.call.status = ESP_HF_CALL_STATUS_CALL_IN_PROGRESS;
    hf(ESP_HF_CLIENT_CIND_CALL_EVT, p);
    scoWideband = wideband;
    memset(&p, 0, sizeof(p));
    p.audio_stat.state = wideband ? ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC : ESP_HF_CLIENT_AUDIO_STATE_CONNECTED;
    hf(ESP_HF_CLIENT_AUDIO_STATE_EVT, p);
    scoActive = 1;
    st.callCount++;
}

static void endCall(void) {
    esp_hf_client_cb_param_t p;
    scoActive = 0;
    simSleepUs(20000);
    memset(&p, 0, sizeof(p)); p.audio_stat.state = ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;
    hf(ESP_HF_CLIENT_AUDIO_STATE_EVT, p);
    memset(&p, 0, sizeof(p)); p.call.status = ESP_HF_CALL_STATUS_NO_CALLS;
    hf(ESP_HF_CLIENT_CIND_CALL_EVT, p);
}

static volatile int a2dpStreaming = 0;
static void *a2dpThread(void *arg) {
    (void)arg;
    static uint8_t pcm[512];
    while (!mockShutdown) {
        if (a2dpStreaming && mockA2dDataCb) {
            int16_t *s = (int16_t *)pcm;
            for (int i = 0; i < 128; i++) { s[i] = (int16_t)(frand() * 8000.0f); }
            mockA2dDataCb(pcm, 512);
            simSleepUs(2900);            /* 128 stereo frames at 44.1 kHz */
        } else {
            simSleepUs(5000);
        }
    }
    return NULL;
}

/* alternate who is talking */
static void *talkThread(void *arg) {
    (void)arg;
    long t = 0;
    while (!mockShutdown) {
        simSleepUs(250000);
        t++;
        farTalking  = ((t / 8) % 4) != 3;              /* far end talks 3 s in every 4 */
        nearTalking = nearEndEnabled && ((t % 28) > 16);   /* near end talks 3 s in every 7 */
    }
    return NULL;
}

/* ============================================================ */
static void resetStats(void) { memset(&st, 0, sizeof(st)); lastUplinkUs = 0; deadRunMs = 0; mockResetMessages(); mockI2sTxUnderruns = 0; mockI2sRxOverruns = 0; }

static void report(const char *name, double simSeconds) {
    double upRms = st.farOnlySamples ? sqrt(st.farOnlyUplinkEnergy / st.farOnlySamples) : 0;
    double echoRms = st.farOnlyEchoSamples ? sqrt(st.farOnlyEchoEnergy / st.farOnlyEchoSamples) : 0;
    double expectFrames = simSeconds / 0.0075;
    printf("  %-26s calls=%-4ld uplink=%3.0f%% gapMax=%4ldms deadRun=%4ldms "
           "txQ~%4.1f/%4.1fms under=%-5ld over=%-4ld chans=%d\n",
           name, st.callCount,
           expectFrames > 0 ? 100.0 * st.uplinkFrames / expectFrames : 0.0,
           st.uplinkGapWorstMs, st.worstDeadRunMs,
           st.txQueueSamples ? st.txQueueSumMs / st.txQueueSamples : 0.0, st.txQueueMaxMs,
           mockI2sTxUnderruns, mockI2sRxOverruns, mockI2sLiveChannels);
    printf("  %-26s uplink RMS %.0f, echo at mic RMS %.0f, mic-referred suppression %.1f dB\n", "",
           upRms, echoRms, (upRms > 0 && echoRms > 0) ? 20.0 * log10(echoRms / upRms) : 0.0);
    printf("  %-26s downlink %.0f%% of line rate\n", "",
           expectFrames > 0 ? 100.0 * st.downlinkFrames / expectFrames : 0.0);
    printf("  %-26s uplink empty fetches %ld, starved intervals %ld, silent 200 ms windows while talking %ld\n", "",
           st.uplinkZeroReturns, st.uplinkStarvedIntervals, st.deadWindows);
    mockDumpMessages();
}

/* Prints the command line and the scenario list. */
static void printUsage(void) {
    printf("usage: sim <scenario> [speedup] [verbose]\n"
           "  1   180 s call, phone clock +60 ppm\n"
           "  2   180 s call, phone clock -60 ppm\n"
           "  3   40 short calls with music between them\n"
           "  4   CVSD / mSBC codec swaps mid-call\n"
           "  5   40 abrupt SCO losses\n"
           "  9   DAC byte accounting over one 30 s call\n"
           "  10  far-end-only 60 s call (gate suppression alone)\n"
           "  speedup: simulated seconds per real second, default 20\n"
           "  verbose: any third argument prints the firmware log to stderr\n");
}

/* True for a scenario number main() knows how to run. */
static int isKnownScenario(int scenario) {
    int isRunnable = (scenario >= 1 && scenario <= 5) || scenario == 9 || scenario == 10;
    return isRunnable;
}

int main(int argc, char **argv) {
    int scenario = (argc > 1) ? atoi(argv[1]) : 0;
    if (!isKnownScenario(scenario)) {
        printUsage();
        return 2;
    }
    if (argc > 2) {
        double requestedSpeedup = atof(argv[2]);
        if (requestedSpeedup <= 0.0) {
            printUsage();
            return 2;
        }
        simSpeedup = requestedSpeedup;
    }
    if (argc > 3) mockLogVerbose = 1;
    setvbuf(stdout, NULL, _IOLBF, 0);

    for (int k = 0; k < ECHO_TAPS; k++)
        echoImpulse[k] = (k == 0 ? 0.9f : 0.35f * expf(-(float)k / 25.0f) * frand());

    app_main();                                   /* real firmware init */
    simSleepUs(200000);

    pthread_t hw, ph, a2, tk;
    pthread_create(&hw, NULL, hardwareThread, NULL);
    pthread_create(&ph, NULL, phoneThread, NULL);
    pthread_create(&a2, NULL, a2dpThread, NULL);
    pthread_create(&tk, NULL, talkThread, NULL);

    /* bring up A2DP so the speaker path is in music mode, as after a real pairing */
    esp_a2d_cb_param_t ap; memset(&ap, 0, sizeof(ap));
    ap.conn_stat.state = ESP_A2D_CONNECTION_STATE_CONNECTED;
    if (mockA2dCb) mockA2dCb(ESP_A2D_CONNECTION_STATE_EVT, &ap);
    memset(&ap, 0, sizeof(ap)); ap.audio_cfg.mcc.type = ESP_A2D_MCT_SBC;
    if (mockA2dCb) mockA2dCb(ESP_A2D_AUDIO_CFG_EVT, &ap);
    esp_hf_client_cb_param_t hp; memset(&hp, 0, sizeof(hp));
    hp.conn_stat.state = ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED;
    hf(ESP_HF_CLIENT_CONNECTION_STATE_EVT, hp);
    simSleepUs(300000);

    double simSeconds = 0;
    long warnBefore = mockWarnCount;

    switch (scenario) {
    case 10: {  /* far-end only: isolates convergence from double-talk gating */
        btPpm = 0; nearEndEnabled = 0; resetStats();
        startCall(1);
        simSeconds = 60; simSleepUs((uint64_t)(simSeconds * 1e6));
        endCall();
        report("10 far-end only 60s", simSeconds);
        break;
    }
    case 9: {   /* focused DAC accounting on a single call */
        btPpm = 0; resetStats();
        mockTxAppBytes = mockTxPopBytes = mockTxPreloadBytes = mockTxPops = 0;
        startCall(1);
        simSeconds = 30; simSleepUs((uint64_t)(simSeconds * 1e6));
        printf("  DAC accounting over %.0f s of call at 16 kHz:\n", simSeconds);
        printf("    phone delivered %7ld downlink frames (expected %.0f), %.1f bytes accepted per frame\n",
               st.downlinkFrames, simSeconds / 0.0075,
               st.downlinkFrames ? (double)mockTxAppBytes / st.downlinkFrames : 0.0);
        printf("    app wrote      %8ld bytes  (%.1f%% of the %ld bytes the DAC needs)\n",
               mockTxAppBytes, 100.0 * mockTxAppBytes / (simSeconds * 16000 * 4), (long)(simSeconds * 16000 * 4));
        printf("    preloaded      %8ld bytes\n", mockTxPreloadBytes);
        printf("    write outcomes: ok=%ld partial=%ld nullchan=%ld deleted=%ld disabled=%ld\n",
               mockWrOk, mockWrTimeout, mockWrNull, mockWrDead, mockWrDisabled);
        printf("    DAC consumed   %8ld bytes in %ld descriptor fetches, %ld of them empty (%.0f%%)\n",
               mockTxPopBytes, mockTxPops, mockI2sTxUnderruns, 100.0 * mockI2sTxUnderruns / (mockTxPops ? mockTxPops : 1));
        endCall();
        break;
    }
    case 1: {   /* long single call, phone clock running fast */
        btPpm = 60; resetStats(); startCall(1);
        simSeconds = 180; simSleepUs((uint64_t)(simSeconds * 1e6));
        endCall();
        report("1 long call +60ppm", simSeconds);
        break;
    }
    case 2: {   /* long single call, phone clock running slow */
        btPpm = -60; resetStats(); startCall(1);
        simSeconds = 180; simSleepUs((uint64_t)(simSeconds * 1e6));
        endCall();
        report("2 long call -60ppm", simSeconds);
        break;
    }
    case 3: {   /* many short calls with music in between */
        btPpm = 20; resetStats();
        for (int i = 0; i < 40; i++) {
            a2dpStreaming = 1; simSleepUs(700000); a2dpStreaming = 0;
            startCall(i % 3 != 0);
            simSleepUs(3500000);
            endCall();
            simSleepUs(200000);
            simSeconds += 3.5;   /* call time only */
        }
        report("3 call/music churn x40", simSeconds);
        break;
    }
    case 4: {   /* codec renegotiation mid-call */
        btPpm = 0; resetStats(); startCall(1);
        for (int i = 0; i < 24; i++) {
            simSleepUs(4000000); simSeconds += 4;
            esp_hf_client_cb_param_t p; memset(&p, 0, sizeof(p));
            p.audio_stat.state = (i % 2) ? ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC
                                         : ESP_HF_CLIENT_AUDIO_STATE_CONNECTED;
            scoWideband = (i % 2);
            hf(ESP_HF_CLIENT_AUDIO_STATE_EVT, p);   /* re-CONNECT without a disconnect */
        }
        endCall();
        report("4 mid-call codec swaps", simSeconds);
        break;
    }
    case 5: {   /* abrupt SCO drops without the usual teardown */
        btPpm = 40; resetStats();
        for (int i = 0; i < 40; i++) {
            startCall(1);
            simSleepUs(2500000); simSeconds += 2.5;
            scoActive = 0;                                    /* radio just goes away */
            simSleepUs(400000); simSeconds += 0.4;
            esp_hf_client_cb_param_t p; memset(&p, 0, sizeof(p));
            p.audio_stat.state = ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED;
            hf(ESP_HF_CLIENT_AUDIO_STATE_EVT, p);
            simSleepUs(300000); simSeconds += 0.3;
        }
        report("5 abrupt SCO loss x40", simSeconds);
        break;
    }
    }

    printf("  %-26s warnings during run: %ld\n", "", mockWarnCount - warnBefore);
    mockShutdown = 1;
    simSleepUs(300000);
    return 0;
}
