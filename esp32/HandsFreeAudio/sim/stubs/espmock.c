#include "espmock.h"
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <sched.h>

double simSpeedup = 20.0;
int    mockLogVerbose = 0;
long   mockWarnCount = 0, mockErrCount = 0;
long   mockScanModeChanges = 0;

static struct timespec simStart;
static pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t realNowUs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(t.tv_sec - simStart.tv_sec) * 1000000ull + (t.tv_nsec - simStart.tv_nsec) / 1000;
}

__attribute__((constructor)) static void simInit(void) { clock_gettime(CLOCK_MONOTONIC, &simStart); }

int64_t esp_timer_get_time(void) { return (int64_t)((double)realNowUs() * simSpeedup); }

uint64_t simNowUs(void) { return (uint64_t)((double)realNowUs() * simSpeedup); }

void simSleepUs(uint64_t us) {
    /* nanosleep granularity is ~50 us, which at high speedups is a large fraction of a
       simulated frame. Spin for short waits so the simulated timing stays faithful. */
    /* Spinning here starves the other simulated tasks on a small core count, which
       showed up as the firmware appearing to write only 10% of its downlink audio.
       Sleep properly; keep the speedup low enough that nanosleep granularity is small
       relative to a 7.5 ms SCO frame. */
    double realUs = (double)us / simSpeedup;
    if (realUs < 1.0) { sched_yield(); return; }
    struct timespec t = { (time_t)(realUs / 1e6), (long)(fmod(realUs, 1e6) * 1000.0) };
    nanosleep(&t, NULL);
}

#define MSG_SLOTS 32
static char msgKey[MSG_SLOTS][72];
static long msgCount[MSG_SLOTS];
static char msgLevel[MSG_SLOTS];

static void tally(char level, const char *fmt) {
    for (int i = 0; i < MSG_SLOTS; i++) {
        if (msgKey[i][0] == 0) { snprintf(msgKey[i], sizeof(msgKey[i]), "%s", fmt); msgLevel[i] = level; msgCount[i] = 1; return; }
        if (strncmp(msgKey[i], fmt, sizeof(msgKey[i]) - 1) == 0) { msgCount[i]++; return; }
    }
}
void mockResetMessages(void) { memset(msgKey, 0, sizeof(msgKey)); memset(msgCount, 0, sizeof(msgCount)); }
void mockDumpMessages(void) {
    pthread_mutex_lock(&logMutex);
    for (int i = 0; i < MSG_SLOTS && msgKey[i][0]; i++)
        if (msgLevel[i] != 'I') printf("       %c x%-7ld %s\n", msgLevel[i], msgCount[i], msgKey[i]);
    pthread_mutex_unlock(&logMutex);
}

void mockLog(char level, const char *tag, const char *fmt, ...) {
    pthread_mutex_lock(&logMutex);
    if (level == 'W') mockWarnCount++;
    if (level == 'E') mockErrCount++;
    tally(level, fmt);
    if (mockLogVerbose) {
        fprintf(stderr, "  [%c %8.3fs %s] ", level, simNowUs() / 1e6, tag);
        va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
        fputc('\n', stderr);
    }
    pthread_mutex_unlock(&logMutex);
}

/* ===================== tasks / sync ===================== */
struct MockTask {
    pthread_t thread;
    TaskFunction_t fn;
    void *arg;
    pthread_mutex_t m;
    pthread_cond_t cv;
    volatile uint32_t notifyValue;
    char name[24];
    uint32_t stackBytes;
    int core;
};

#define MAX_TASKS 16
static struct MockTask *taskList[MAX_TASKS];
static int taskCount = 0;
static pthread_mutex_t taskListMutex = PTHREAD_MUTEX_INITIALIZER;
volatile int mockShutdown = 0;

/* The task running on this thread. Set by the thread itself, so it is valid from the
   task's first instruction; looking the thread up by pthread_t instead could miss
   while pthread_create() has not yet stored the id, losing the first notification. */
static __thread struct MockTask *threadTask = NULL;

static void *taskTrampoline(void *p) {
    struct MockTask *t = (struct MockTask *)p;
    threadTask = t;
    t->fn(t->arg);
    return NULL;
}

static struct MockTask *currentTask(void) { return threadTask; }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, uint32_t prio, TaskHandle_t *out, int core) {
    (void)prio;
    struct MockTask *t = calloc(1, sizeof(*t));
    if (!t) return pdFALSE;
    t->fn = fn; t->arg = arg;
    t->stackBytes = stack; t->core = core;
    snprintf(t->name, sizeof(t->name), "%s", name);
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->cv, NULL);
    pthread_mutex_lock(&taskListMutex);
    if (taskCount >= MAX_TASKS) {
        pthread_mutex_unlock(&taskListMutex);
        fprintf(stderr, "mock: more than %d tasks\n", MAX_TASKS);
        free(t);
        return pdFALSE;
    }
    if (out) *out = t;
    /* Listed only once the thread exists, so uxTaskGetSystemState never reads an
       unset thread id. The new thread does not take taskListMutex. */
    pthread_create(&t->thread, NULL, taskTrampoline, t);
    taskList[taskCount++] = t;
    pthread_mutex_unlock(&taskListMutex);
    return pdPASS;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *statusArray, UBaseType_t arraySize,
                                 configRUN_TIME_COUNTER_TYPE *totalRunTime) {
    pthread_mutex_lock(&taskListMutex);
    UBaseType_t count = (UBaseType_t)taskCount;
    if (count > arraySize) { pthread_mutex_unlock(&taskListMutex); return 0; }
    for (UBaseType_t i = 0; i < count; i++) {
        struct MockTask *t = taskList[i];
        clockid_t threadClock;
        struct timespec cpuTime = { 0, 0 };
        if (pthread_getcpuclockid(t->thread, &threadClock) == 0) clock_gettime(threadClock, &cpuTime);
        statusArray[i].xHandle = t;
        statusArray[i].pcTaskName = t->name;
        statusArray[i].usStackHighWaterMark = t->stackBytes;
        statusArray[i].ulRunTimeCounter = (uint32_t)((uint64_t)cpuTime.tv_sec * 1000000ull + (uint64_t)cpuTime.tv_nsec / 1000);
    }
    pthread_mutex_unlock(&taskListMutex);
    if (totalRunTime) *totalRunTime = (uint32_t)realNowUs();
    return count;
}

UBaseType_t  uxTaskGetStackHighWaterMark(TaskHandle_t t) { return t ? t->stackBytes : 0; }
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core) { (void)core; return NULL; }
BaseType_t   xTaskGetCoreID(TaskHandle_t t) { return t ? t->core : tskNO_AFFINITY; }

void vTaskDelay(TickType_t ticks) { simSleepUs((uint64_t)ticks * 1000ull); }

uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait) {
    struct MockTask *t = currentTask();
    if (!t) { simSleepUs(1000); return 0; }
    pthread_mutex_lock(&t->m);
    while (t->notifyValue == 0 && !mockShutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&t->cv, &t->m, &ts);
        if (wait != portMAX_DELAY) break;
    }
    uint32_t v = t->notifyValue;
    t->notifyValue = clearOnExit ? 0 : (v ? v - 1 : 0);
    pthread_mutex_unlock(&t->m);
    return v;
}

void xTaskNotifyGive(TaskHandle_t t) {
    if (!t) return;
    pthread_mutex_lock(&t->m);
    t->notifyValue++;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->m);
}

struct MockSem { pthread_mutex_t m; pthread_cond_t cv; int count; int isMutex; };

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct MockSem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->cv, NULL);
    s->count = 1; s->isMutex = 1; return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    struct MockSem *s = calloc(1, sizeof(*s));
    pthread_mutex_init(&s->m, NULL); pthread_cond_init(&s->cv, NULL);
    s->count = 0; return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (!s) return pdFALSE;
    pthread_mutex_lock(&s->m);
    uint64_t deadline = simNowUs() + (wait == portMAX_DELAY ? 0 : (uint64_t)wait * 1000ull);
    while (s->count == 0) {
        if (mockShutdown) { pthread_mutex_unlock(&s->m); return pdFALSE; }
        if (wait != portMAX_DELAY && simNowUs() >= deadline) { pthread_mutex_unlock(&s->m); return pdFALSE; }
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 2000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&s->cv, &s->m, &ts);
    }
    s->count--;
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFALSE;
    pthread_mutex_lock(&s->m);
    if (!s->isMutex || s->count == 0) s->count++;
    pthread_cond_signal(&s->cv);
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

/* ===================== stream buffer ===================== */
struct MockStreamBuffer {
    uint8_t *buf; size_t cap, head, tail, used;
    pthread_mutex_t m;
};
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger) {
    (void)trigger;
    struct MockStreamBuffer *b = calloc(1, sizeof(*b));
    b->buf = calloc(1, size); b->cap = size;
    pthread_mutex_init(&b->m, NULL);
    return b;
}
size_t xStreamBufferSend(StreamBufferHandle_t b, const void *d, size_t n, TickType_t w) {
    (void)w;
    pthread_mutex_lock(&b->m);
    size_t space = b->cap - b->used, k = n < space ? n : space;
    for (size_t i = 0; i < k; i++) { b->buf[b->head] = ((const uint8_t *)d)[i]; b->head = (b->head + 1) % b->cap; }
    b->used += k;
    pthread_mutex_unlock(&b->m);
    return k;
}
size_t xStreamBufferReceive(StreamBufferHandle_t b, void *d, size_t n, TickType_t w) {
    (void)w;
    pthread_mutex_lock(&b->m);
    size_t k = n < b->used ? n : b->used;
    for (size_t i = 0; i < k; i++) { ((uint8_t *)d)[i] = b->buf[b->tail]; b->tail = (b->tail + 1) % b->cap; }
    b->used -= k;
    pthread_mutex_unlock(&b->m);
    return k;
}
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t b) {
    pthread_mutex_lock(&b->m); size_t u = b->used; pthread_mutex_unlock(&b->m); return u;
}
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t b) {
    pthread_mutex_lock(&b->m); size_t s = b->cap - b->used; pthread_mutex_unlock(&b->m); return s;
}
BaseType_t xStreamBufferReset(StreamBufferHandle_t b) {
    pthread_mutex_lock(&b->m); b->head = b->tail = b->used = 0; pthread_mutex_unlock(&b->m); return pdTRUE;
}

/* ===================== ring buffer (bytebuf) ===================== */
struct MockRingBuffer {
    uint8_t *buf; size_t cap, head, tail, used;
    uint8_t *scratch; size_t scratchLen;
    pthread_mutex_t m;
};
RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type) {
    (void)type;
    struct MockRingBuffer *r = calloc(1, sizeof(*r));
    r->buf = calloc(1, size); r->cap = size; r->scratch = calloc(1, 8192);
    pthread_mutex_init(&r->m, NULL);
    return r;
}
BaseType_t xRingbufferSend(RingbufHandle_t r, const void *d, size_t n, TickType_t w) {
    (void)w;
    pthread_mutex_lock(&r->m);
    if (r->cap - r->used < n) { pthread_mutex_unlock(&r->m); return pdFALSE; }
    for (size_t i = 0; i < n; i++) { r->buf[r->head] = ((const uint8_t *)d)[i]; r->head = (r->head + 1) % r->cap; }
    r->used += n;
    pthread_mutex_unlock(&r->m);
    return pdTRUE;
}
void *xRingbufferReceive(RingbufHandle_t r, size_t *outSize, TickType_t w) {
    uint64_t deadline = simNowUs() + (uint64_t)w * 1000ull;
    for (;;) {
        pthread_mutex_lock(&r->m);
        if (r->used > 0) {
            size_t k = r->used < 4096 ? r->used : 4096;
            for (size_t i = 0; i < k; i++) { r->scratch[i] = r->buf[r->tail]; r->tail = (r->tail + 1) % r->cap; }
            r->used -= k; r->scratchLen = k;
            pthread_mutex_unlock(&r->m);
            *outSize = k;
            return r->scratch;
        }
        pthread_mutex_unlock(&r->m);
        if (mockShutdown || simNowUs() >= deadline) { *outSize = 0; return NULL; }
        simSleepUs(500);
    }
}
void vRingbufferReturnItem(RingbufHandle_t r, void *item) { (void)r; (void)item; }
void vRingbufferGetInfo(RingbufHandle_t r, void *a, void *b, void *c, void *d, size_t *filled) {
    (void)a; (void)b; (void)c; (void)d;
    pthread_mutex_lock(&r->m); if (filled) *filled = r->used; pthread_mutex_unlock(&r->m);
}

/* ===================== I2S with a faithful DMA descriptor model ===================== */
/*
 * i2s_channel_write() only releases a descriptor to the hardware once it is completely
 * full, and blocks when every descriptor is in flight. That chunking is precisely what
 * governs downlink latency, so it is modelled exactly rather than as a byte FIFO.
 */
#define MAX_DESC 16
struct MockI2sChan {
    bool isTx, enabled, inited, alive;
    int  port;
    uint32_t rate, descNum, frameNum;
    int  bytesPerFrame, descBytes;
    uint8_t *desc[MAX_DESC];
    int  descLen[MAX_DESC];
    int  readyHead, readyCount;        /* ring of full descriptors */
    uint8_t *staging; int stagingFill; /* descriptor currently being filled by write() */
    i2s_event_callbacks_t callbacks;   /* registered event callbacks, all NULL by default */
    void *callbackContext;
    pthread_mutex_t m;
};

int  mockI2sLiveChannels = 0;
long mockI2sTxUnderruns = 0, mockI2sRxOverruns = 0;
long mockTxAppBytes = 0, mockTxPopBytes = 0, mockTxPreloadBytes = 0, mockTxPops = 0;
i2s_chan_handle_t mockTxChan = NULL, mockRxChan = NULL;

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx) {
    struct MockI2sChan *c = calloc(1, sizeof(*c));
    c->isTx = (tx != NULL); c->port = cfg->id; c->alive = true;
    c->descNum = cfg->dma_desc_num > MAX_DESC ? MAX_DESC : cfg->dma_desc_num;
    c->frameNum = cfg->dma_frame_num;
    pthread_mutex_init(&c->m, NULL);
    mockI2sLiveChannels++;
    if (tx) { *tx = c; mockTxChan = c; }
    if (rx) { *rx = c; mockRxChan = c; }
    return ESP_OK;
}

esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t c, const i2s_event_callbacks_t *callbacks, void *user_data) {
    if (!c || !callbacks) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (c->enabled) { pthread_mutex_unlock(&c->m); return ESP_ERR_INVALID_STATE; }   /* IDF: before enable only */
    c->callbacks = *callbacks;
    c->callbackContext = user_data;
    pthread_mutex_unlock(&c->m);
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t c, const i2s_std_config_t *cfg) {
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    c->rate = cfg->clk_cfg.sample_rate_hz;
    int slots = (cfg->slot_cfg.slot_mode == I2S_SLOT_MODE_STEREO) ? 2 : 1;
    int bytesPerSample = (cfg->slot_cfg.data_bit_width > 16) ? 4 : 2;
    c->bytesPerFrame = slots * bytesPerSample;
    c->descBytes = c->frameNum * c->bytesPerFrame;
    for (uint32_t i = 0; i < c->descNum; i++) { free(c->desc[i]); c->desc[i] = calloc(1, c->descBytes); }
    free(c->staging); c->staging = calloc(1, c->descBytes);
    c->stagingFill = 0; c->readyHead = c->readyCount = 0;
    c->inited = true;
    pthread_mutex_unlock(&c->m);
    return ESP_OK;
}

esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t c, const i2s_std_clk_config_t *cfg) {
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (c->enabled) { pthread_mutex_unlock(&c->m); return ESP_ERR_INVALID_STATE; }   /* IDF requires the channel be disabled */
    c->rate = cfg->sample_rate_hz;
    pthread_mutex_unlock(&c->m);
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t c) {
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (c->enabled) { pthread_mutex_unlock(&c->m); mockLog('E', "i2s", "enable on already-enabled channel"); return ESP_ERR_INVALID_STATE; }
    c->stagingFill = 0;
    c->enabled = true;
    pthread_mutex_unlock(&c->m);
    return ESP_OK;
}

esp_err_t i2s_channel_disable(i2s_chan_handle_t c) {
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (!c->enabled) { pthread_mutex_unlock(&c->m); mockLog('E', "i2s", "disable on a channel that has not been enabled"); return ESP_ERR_INVALID_STATE; }
    c->enabled = false;
    pthread_mutex_unlock(&c->m);
    return ESP_OK;
}

esp_err_t i2s_del_channel(i2s_chan_handle_t c) {
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (c->enabled) { pthread_mutex_unlock(&c->m); mockLog('E', "i2s", "del on an enabled channel"); return ESP_ERR_INVALID_STATE; }
    c->alive = false;
    for (uint32_t i = 0; i < c->descNum; i++) { free(c->desc[i]); c->desc[i] = NULL; }
    free(c->staging); c->staging = NULL;
    pthread_mutex_unlock(&c->m);
    mockI2sLiveChannels--;
    if (mockTxChan == c) mockTxChan = NULL;
    if (mockRxChan == c) mockRxChan = NULL;
    /* deliberately not freed: catches use-after-free from another task */
    return ESP_OK;
}

esp_err_t i2s_channel_preload_data(i2s_chan_handle_t c, const void *src, size_t n, size_t *loaded) {
    if (loaded) *loaded = 0;
    if (!c) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&c->m);
    if (!c->alive) { pthread_mutex_unlock(&c->m); return ESP_ERR_INVALID_ARG; }
    if (c->enabled) { pthread_mutex_unlock(&c->m); return ESP_ERR_INVALID_STATE; }   /* only valid in the READY state */
    size_t done = 0;
    while (done < n && c->readyCount < (int)c->descNum) {
        size_t take = (n - done) < (size_t)c->descBytes ? (n - done) : (size_t)c->descBytes;
        int slot = (c->readyHead + c->readyCount) % c->descNum;
        memset(c->desc[slot], 0, c->descBytes);
        memcpy(c->desc[slot], (const uint8_t *)src + done, take);
        c->descLen[slot] = c->descBytes;
        c->readyCount++; done += take;
    }
    pthread_mutex_unlock(&c->m);
    if (loaded) *loaded = done;
    mockTxPreloadBytes += (long)done;
    return ESP_OK;
}

long mockWrNull = 0, mockWrDead = 0, mockWrDisabled = 0, mockWrTimeout = 0, mockWrOk = 0;
esp_err_t i2s_channel_write(i2s_chan_handle_t c, const void *src, size_t n, size_t *written, TickType_t wait) {
    if (written) *written = 0;
    if (!c)          { mockWrNull++;     return ESP_ERR_INVALID_ARG; }
    if (!c->alive)   { mockWrDead++;     return ESP_ERR_INVALID_ARG; }
    if (!c->enabled) { mockWrDisabled++; return ESP_ERR_INVALID_ARG; }
    const uint8_t *p = (const uint8_t *)src;
    size_t done = 0;
    uint64_t deadline = simNowUs() + (uint64_t)wait * 1000ull;

    while (done < n) {
        pthread_mutex_lock(&c->m);
        if (!c->alive || !c->enabled) { pthread_mutex_unlock(&c->m); break; }
        int room = c->descBytes - c->stagingFill;
        int take = (int)(n - done) < room ? (int)(n - done) : room;
        memcpy(c->staging + c->stagingFill, p + done, take);
        c->stagingFill += take; done += take;

        if (c->stagingFill == c->descBytes) {
            if (c->readyCount < (int)c->descNum) {
                int slot = (c->readyHead + c->readyCount) % c->descNum;
                memcpy(c->desc[slot], c->staging, c->descBytes);
                c->descLen[slot] = c->descBytes;
                c->readyCount++;
                c->stagingFill = 0;
            } else {
                /* every descriptor in flight: block until the hardware frees one */
                c->stagingFill -= take; done -= take;
                pthread_mutex_unlock(&c->m);
                if (simNowUs() >= deadline || mockShutdown) break;
                simSleepUs(200);
                continue;
            }
        }
        pthread_mutex_unlock(&c->m);
    }
    if (written) *written = done;
    if (done < n) mockWrTimeout++; else mockWrOk++;
    __sync_fetch_and_add(&mockTxAppBytes, (long)done);
    return ESP_OK;
}

esp_err_t i2s_channel_read(i2s_chan_handle_t c, void *dst, size_t n, size_t *readOut, TickType_t wait) {
    if (readOut) *readOut = 0;
    if (!c || !c->alive || !c->enabled) return ESP_ERR_INVALID_ARG;
    uint64_t deadline = simNowUs() + (uint64_t)wait * 1000ull;
    for (;;) {
        pthread_mutex_lock(&c->m);
        if (c->readyCount > 0) {
            int slot = c->readyHead;
            size_t k = (size_t)c->descLen[slot] < n ? (size_t)c->descLen[slot] : n;
            memcpy(dst, c->desc[slot], k);
            c->readyHead = (c->readyHead + 1) % c->descNum;
            c->readyCount--;
            pthread_mutex_unlock(&c->m);
            if (readOut) *readOut = k;
            return ESP_OK;
        }
        pthread_mutex_unlock(&c->m);
        if (mockShutdown) return ESP_ERR_TIMEOUT;
        if (simNowUs() >= deadline) return ESP_ERR_TIMEOUT;
        simSleepUs(200);
    }
}

/* hardware side, called by the simulated I2S clock */
int mockI2sTxPop(i2s_chan_handle_t c, uint8_t *out) {
    if (!c || !c->alive) return 0;
    pthread_mutex_lock(&c->m);
    if (!c->enabled) { pthread_mutex_unlock(&c->m); return 0; }
    mockTxPops++;
    i2s_isr_callback_t onSent = c->callbacks.on_sent;
    void *context = c->callbackContext;
    if (c->readyCount == 0) {
        mockI2sTxUnderruns++;
        int emptyBytes = c->descBytes;
        pthread_mutex_unlock(&c->m);
        if (onSent) {   /* an underrun plays an auto-cleared, silent descriptor */
            static uint8_t silentDescriptor[16384];
            i2s_event_data_t event = { silentDescriptor, (size_t)emptyBytes };
            onSent(c, &event, context);
        }
        return -emptyBytes;
    }
    mockTxPopBytes += c->descBytes;
    int slot = c->readyHead;
    memcpy(out, c->desc[slot], c->descBytes);
    c->readyHead = (c->readyHead + 1) % c->descNum;
    c->readyCount--;
    int nb = c->descBytes;
    pthread_mutex_unlock(&c->m);
    if (onSent) {
        i2s_event_data_t event = { out, (size_t)nb };
        onSent(c, &event, context);
    }
    return nb;
}

int mockI2sRxPush(i2s_chan_handle_t c, const uint8_t *in) {
    if (!c || !c->alive) return 0;
    pthread_mutex_lock(&c->m);
    if (!c->enabled) { pthread_mutex_unlock(&c->m); return 0; }
    bool isOverflow = (c->readyCount == (int)c->descNum);
    if (isOverflow) {          /* overflow: oldest is dropped */
        c->readyHead = (c->readyHead + 1) % c->descNum; c->readyCount--; mockI2sRxOverruns++;
    }
    int slot = (c->readyHead + c->readyCount) % c->descNum;
    memcpy(c->desc[slot], in, c->descBytes);
    c->descLen[slot] = c->descBytes;
    c->readyCount++;
    int nb = c->descBytes;
    i2s_isr_callback_t onOverflow = c->callbacks.on_recv_q_ovf;
    void *context = c->callbackContext;
    pthread_mutex_unlock(&c->m);
    if (isOverflow && onOverflow) {
        i2s_event_data_t event = { NULL, 0 };
        onOverflow(c, &event, context);
    }
    return nb;
}

int      mockI2sDescBytes(i2s_chan_handle_t c) { return c && c->alive ? c->descBytes : 0; }
size_t   mockI2sTxQueuedBytes(i2s_chan_handle_t c) { return c && c->alive ? (size_t)c->readyCount * c->descBytes : 0; }
uint32_t mockI2sRate(i2s_chan_handle_t c) { return c && c->alive ? c->rate : 0; }
bool     mockI2sEnabled(i2s_chan_handle_t c) { return c && c->alive && c->enabled; }

/* ===================== trivial stubs ===================== */
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 100 * 1024; }
size_t heap_caps_get_largest_free_block(uint32_t caps) { (void)caps; return 60 * 1024; }
size_t heap_caps_get_minimum_free_size(uint32_t caps) { (void)caps; return 90 * 1024; }

esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { (void)pin; return 0; }

static uint8_t nvsBlob[6]; static int nvsHas = 0;
esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) { (void)ns; (void)mode; *h = 1; return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { (void)h; (void)k; memcpy(nvsBlob, v, n < 6 ? n : 6); nvsHas = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n) { (void)h; (void)k; if (!nvsHas) return ESP_FAIL; memcpy(v, nvsBlob, 6); *n = 6; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }

esp_err_t esp_bt_controller_mem_release(int m) { (void)m; return ESP_OK; }
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *c) { (void)c; return ESP_OK; }
esp_err_t esp_bt_controller_enable(int m) { (void)m; return ESP_OK; }
esp_err_t esp_bluedroid_init(void) { return ESP_OK; }
esp_err_t esp_bluedroid_enable(void) { return ESP_OK; }
esp_err_t esp_bt_gap_set_device_name(const char *n) { (void)n; return ESP_OK; }
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c, esp_bt_discovery_mode_t d) { (void)c; (void)d; mockScanModeChanges++; return ESP_OK; }
static uint8_t ownAddr[6] = { 1, 2, 3, 4, 5, 6 };
uint8_t *esp_bt_dev_get_address(void) { return ownAddr; }

esp_err_t esp_avrc_ct_init(void) { return ESP_OK; }
esp_err_t esp_avrc_ct_send_passthrough_cmd(uint8_t tl, uint8_t k, uint8_t s) { (void)tl; (void)k; (void)s; return ESP_OK; }

esp_a2d_cb_t mockA2dCb = NULL; esp_a2d_sink_data_cb_t mockA2dDataCb = NULL;
esp_err_t esp_a2d_register_callback(esp_a2d_cb_t cb) { mockA2dCb = cb; return ESP_OK; }
esp_err_t esp_a2d_sink_register_data_callback(esp_a2d_sink_data_cb_t cb) { mockA2dDataCb = cb; return ESP_OK; }
esp_err_t esp_a2d_sink_init(void) { return ESP_OK; }
esp_err_t esp_a2d_sink_connect(esp_bd_addr_t a) { (void)a; return ESP_OK; }

esp_hf_client_cb_t mockHfCb = NULL;
esp_hf_client_incoming_data_cb_t mockHfRecvCb = NULL;
esp_hf_client_outgoing_data_cb_t mockHfSendCb = NULL;
volatile long mockOutgoingReadyPending = 0;
esp_err_t esp_hf_client_register_callback(esp_hf_client_cb_t cb) { mockHfCb = cb; return ESP_OK; }
esp_err_t esp_hf_client_register_data_callback(esp_hf_client_incoming_data_cb_t r, esp_hf_client_outgoing_data_cb_t s) { mockHfRecvCb = r; mockHfSendCb = s; return ESP_OK; }
esp_err_t esp_hf_client_init(void) { return ESP_OK; }
esp_err_t esp_hf_client_reject_call(void) { return ESP_OK; }
esp_err_t esp_hf_client_outgoing_data_ready(void) { __sync_fetch_and_add(&mockOutgoingReadyPending, 1); return ESP_OK; }
