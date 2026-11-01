#define _GNU_SOURCE   /* sched_setaffinity, sched_getcpu */
#include "espmock.h"
#include <pthread.h>
#include <time.h>
#include <stdarg.h>
#include <errno.h>
#include <math.h>
#include <sched.h>
#include <unistd.h>
#include <signal.h>

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

/* ===================== virtual-time scheduler ===================== */
/*
 * Every simulated thread (the firmware's tasks, sim.c's hardware, phone, A2DP and
 * talker threads, and main) is a pthread, but only the one holding the run token
 * executes; the rest wait on their own condition variable. A thread gives the token
 * up only when it blocks: a sleep, or a wait on an object (semaphore, notification,
 * ring buffer, I2S queue). The token then goes to the next ready thread, in the order
 * they became ready. When none is ready, virtual time jumps to the earliest deadline
 * and every thread due then becomes ready, in creation order.
 *
 * So virtual time never depends on how fast the host runs, and code between two
 * blocking calls takes no virtual time at all. A run is the same on any machine and
 * under any load, and repeats exactly.
 */
#define SIM_MAX_THREADS    32
#define SIM_NO_DEADLINE    UINT64_MAX
#define SIM_WATCHDOG_REAL_SECONDS 20   /* real seconds without a switch: a thread is spinning */
#define SIM_MUTEX_WAIT_LIMIT_US   10000000ull   /* the firmware holds its mutexes for ms, never 10 s */
#define SIM_VIRTUAL_LIMIT_US      900000000ull  /* no scenario runs 15 simulated minutes */

typedef enum { SIM_READY, SIM_RUNNING, SIM_SLEEPING, SIM_WAITING, SIM_DONE } SimThreadState;

typedef struct SimThread {
    int             index;          /* creation order, for deterministic wake order */
    char            name[24];
    SimThreadState  state;
    uint64_t        wakeAtUs;       /* SIM_SLEEPING / SIM_WAITING: deadline, or SIM_NO_DEADLINE */
    const void     *waitObject;     /* SIM_WAITING: simWakeObject(waitObject) readies it early */
    const char     *waitLabel;      /* SIM_WAITING: what it waits for, for the state dump */
    uint64_t        waitStartUs;    /* SIM_WAITING: when the wait began */
    uint64_t        waitSequence;   /* SIM_WAITING: order the waits began, for hand-overs */
    const void     *handOffObject;  /* a semaphore given straight to this thread (see xSemaphoreGive) */
    pthread_t       thread;
    pthread_cond_t  turn;           /* signalled when the thread is given the run token */
    void         *(*entry)(void *);
    void           *argument;
    struct SimThread *nextReady;
} SimThread;

static pthread_mutex_t schedulerLock = PTHREAD_MUTEX_INITIALIZER;
static SimThread *simThreads[SIM_MAX_THREADS];
static int        simThreadCount = 0;
static SimThread *runningThread = NULL;
static SimThread *readyHead = NULL, *readyTail = NULL;
static uint64_t   virtualNowUs = 0;
static volatile unsigned long switchCount = 0;
static __thread SimThread *selfThread = NULL;
static volatile sig_atomic_t dumpRequested = 0;
static uint64_t   nextWaitSequence = 0;
void (*simDumpHook)(void) = NULL;

/**
 * @brief Puts a thread at the back of the ready queue. Caller holds schedulerLock.
 */
static void makeReadyLocked(SimThread *thread) {
    thread->state = SIM_READY;
    thread->wakeAtUs = SIM_NO_DEADLINE;
    thread->waitObject = NULL;
    thread->waitLabel = NULL;
    thread->nextReady = NULL;
    if (readyTail != NULL) {
        readyTail->nextReady = thread;
    } else {
        readyHead = thread;
    }
    readyTail = thread;
}

/**
 * @brief Takes the thread at the front of the ready queue, or NULL. Caller holds
 *        schedulerLock.
 */
static SimThread *takeReadyLocked(void) {
    SimThread *thread = readyHead;
    if (thread != NULL) {
        readyHead = thread->nextReady;
        if (readyHead == NULL) {
            readyTail = NULL;
        }
        thread->nextReady = NULL;
    }
    return thread;
}

static void checkForStuckThreadsLocked(void);
static void dumpThreadsAndExit(const char *reason);

/**
 * @brief Moves virtual time to the earliest deadline and readies every thread due by
 *        then, in creation order. Caller holds schedulerLock.
 * * @return false when no thread has a deadline: every thread waits forever.
 */
static bool advanceTimeLocked(void) {
    uint64_t earliestUs = SIM_NO_DEADLINE;
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        bool isBlocked = (thread->state == SIM_SLEEPING || thread->state == SIM_WAITING);
        if (isBlocked && thread->wakeAtUs < earliestUs) {
            earliestUs = thread->wakeAtUs;
        }
    }
    if (earliestUs == SIM_NO_DEADLINE) {
        return false;
    }
    if (earliestUs > virtualNowUs) {
        virtualNowUs = earliestUs;
        checkForStuckThreadsLocked();
    }
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        bool isBlocked = (thread->state == SIM_SLEEPING || thread->state == SIM_WAITING);
        if (isBlocked && thread->wakeAtUs <= virtualNowUs) {
            makeReadyLocked(thread);
        }
    }
    return true;
}

/**
 * @brief Prints why the run stopped and every thread's state, then sim.c's list of the
 *        firmware's handles (so an object address can be named), and exits.
 *   sim: main has waited 10.0 s for a mutex, at 41.250 s virtual
 *     main                 waiting  mutex 0x55d0c2a0 since 31.250 s, no deadline
 *     sim hardware         sleeping until 41.250 s
 *     txMutex 0x55d0c2a0 | rxMutex 0x55d0c2c0 | ...
 * * Safe to call without schedulerLock only from the watchdog, when no thread is
 *   switching (the figures are then read as they are).
 */
static void dumpThreadsAndExit(const char *reason) {
    fprintf(stderr, "sim: %s, at %.3f s virtual\n", reason, virtualNowUs / 1e6);
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        char deadlineText[40];
        if (thread->wakeAtUs == SIM_NO_DEADLINE) {
            snprintf(deadlineText, sizeof(deadlineText), "no deadline");
        } else {
            snprintf(deadlineText, sizeof(deadlineText), "until %.3f s", thread->wakeAtUs / 1e6);
        }
        switch (thread->state) {
        case SIM_RUNNING:  fprintf(stderr, "  %-20s running\n", thread->name); break;
        case SIM_READY:    fprintf(stderr, "  %-20s ready\n", thread->name); break;
        case SIM_SLEEPING: fprintf(stderr, "  %-20s sleeping %s\n", thread->name, deadlineText); break;
        case SIM_WAITING:  fprintf(stderr, "  %-20s waiting  %s %p since %.3f s, %s\n", thread->name,
                                   thread->waitLabel ? thread->waitLabel : "?", thread->waitObject,
                                   thread->waitStartUs / 1e6, deadlineText); break;
        case SIM_DONE:     fprintf(stderr, "  %-20s finished\n", thread->name); break;
        }
    }
    if (simDumpHook != NULL) {
        simDumpHook();
    }
    fflush(stdout);
    fflush(stderr);
    _exit(3);
}

/**
 * @brief Stops a run that can no longer finish: a thread stuck on a mutex (time still
 *        moves, because the other threads keep sleeping and waking), or a run far
 *        longer than any scenario. Called after every time jump. Caller holds
 *        schedulerLock.
 */
static void checkForStuckThreadsLocked(void) {
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        bool isWaitingOnMutex = (thread->state == SIM_WAITING && thread->waitLabel != NULL &&
                                 strcmp(thread->waitLabel, "mutex") == 0);
        if (!isWaitingOnMutex) {
            continue;
        }
        uint64_t waitedUs = virtualNowUs - thread->waitStartUs;
        if (waitedUs > SIM_MUTEX_WAIT_LIMIT_US) {
            char reason[96];
            snprintf(reason, sizeof(reason), "%s has waited %.1f s for a mutex", thread->name, waitedUs / 1e6);
            dumpThreadsAndExit(reason);
        }
    }
    if (virtualNowUs > SIM_VIRTUAL_LIMIT_US) {
        dumpThreadsAndExit("the run passed 15 simulated minutes, longer than any scenario");
    }
}

/**
 * @brief Hands the run token to the next ready thread (advancing time if none is) and,
 *        unless the caller has finished, waits until the token comes back to it.
 * * The caller has already set its own state (ready, sleeping, waiting or done).
 *   Caller holds schedulerLock; it still does on return.
 */
static void switchAwayLocked(SimThread *self) {
    SimThread *next = takeReadyLocked();
    while (next == NULL) {
        bool hasDeadline = advanceTimeLocked();
        if (!hasDeadline) {
            dumpThreadsAndExit("deadlock: every thread waits with no deadline");
        }
        next = takeReadyLocked();
    }
    switchCount++;
    next->state = SIM_RUNNING;
    runningThread = next;
    if (next == self) {
        return;
    }
    pthread_cond_signal(&next->turn);
    if (self->state == SIM_DONE) {
        return;
    }
    while (runningThread != self) {
        pthread_cond_wait(&self->turn, &schedulerLock);
    }
}

/**
 * @brief The calling thread's scheduler entry; aborts for a thread the scheduler never
 *        created (it could not take part in virtual time).
 */
static SimThread *currentSimThread(void) {
    SimThread *self = selfThread;
    if (self == NULL) {
        fprintf(stderr, "sim: a thread not created by simThreadCreate() called a blocking mock\n");
        abort();
    }
    return self;
}

/**
 * @brief Runs a scheduled thread: waits for its first turn, runs its entry, then gives
 *        the token away for good.
 */
static void *simThreadTrampoline(void *parameter) {
    SimThread *self = (SimThread *)parameter;
    selfThread = self;
    pthread_mutex_lock(&schedulerLock);
    while (runningThread != self) {
        pthread_cond_wait(&self->turn, &schedulerLock);
    }
    pthread_mutex_unlock(&schedulerLock);

    self->entry(self->argument);

    pthread_mutex_lock(&schedulerLock);
    self->state = SIM_DONE;
    switchAwayLocked(self);
    pthread_mutex_unlock(&schedulerLock);
    return NULL;
}

/**
 * @brief Registers a thread record in creation order. Caller holds schedulerLock.
 */
static SimThread *registerThreadLocked(const char *name) {
    if (simThreadCount >= SIM_MAX_THREADS) {
        fprintf(stderr, "sim: more than %d threads\n", SIM_MAX_THREADS);
        abort();
    }
    SimThread *thread = calloc(1, sizeof(*thread));
    thread->index = simThreadCount;
    snprintf(thread->name, sizeof(thread->name), "%s", name);
    thread->wakeAtUs = SIM_NO_DEADLINE;
    pthread_cond_init(&thread->turn, NULL);
    simThreads[simThreadCount] = thread;
    simThreadCount++;
    return thread;
}

pthread_t simThreadCreate(void *(*entry)(void *), void *argument, const char *name) {
    pthread_mutex_lock(&schedulerLock);
    SimThread *thread = registerThreadLocked(name);
    thread->entry = entry;
    thread->argument = argument;
    makeReadyLocked(thread);
    /* Created under the lock, so its id is stored before it can be read. The new
       thread only waits for its turn, so it never needs the lock to start. */
    pthread_create(&thread->thread, NULL, simThreadTrampoline, thread);
    pthread_t threadId = thread->thread;
    pthread_mutex_unlock(&schedulerLock);
    return threadId;
}

/**
 * @brief Ctrl+C: asks the watchdog for the state dump (printing from a signal handler
 *        is not safe).
 */
static void requestDumpOnSignal(int signalNumber) {
    (void)signalNumber;
    dumpRequested = 1;
}

/**
 * @brief Dumps every thread's state and exits if no switch happened for
 *        SIM_WATCHDOG_REAL_SECONDS (a loop that never blocks stops virtual time, so
 *        nothing else could notice), or when Ctrl+C asks for it.
 */
static void *watchdogThread(void *parameter) {
    (void)parameter;
    unsigned long lastSwitchCount = switchCount;
    int quietSeconds = 0;
    while (true) {
        sleep(1);
        if (dumpRequested) {
            dumpThreadsAndExit("stopped with Ctrl+C");
        }
        unsigned long switchCountNow = switchCount;
        if (switchCountNow != lastSwitchCount) {
            lastSwitchCount = switchCountNow;
            quietSeconds = 0;
            continue;
        }
        quietSeconds++;
        if (quietSeconds >= SIM_WATCHDOG_REAL_SECONDS) {
            SimThread *stuck = runningThread;
            char reason[128];
            snprintf(reason, sizeof(reason), "%s has run %d real seconds without blocking (a loop with no blocking call)",
                     stuck ? stuck->name : "?", quietSeconds);
            dumpThreadsAndExit(reason);
        }
    }
    return NULL;
}

/* main() is the first scheduled thread, holding the run token from the start. The
   process is pinned to one CPU first (every thread inherits it): only one simulated
   thread runs at a time anyway, and a hand-over on the same core is far quicker than
   waking a thread on another one. */
__attribute__((constructor)) static void simInit(void) {
    clock_gettime(CLOCK_MONOTONIC, &simStart);
    int currentCpu = sched_getcpu();
    if (currentCpu >= 0) {
        cpu_set_t oneCpu;
        CPU_ZERO(&oneCpu);
        CPU_SET(currentCpu, &oneCpu);
        sched_setaffinity(0, sizeof(oneCpu), &oneCpu);
    }
    pthread_mutex_lock(&schedulerLock);
    SimThread *mainThread = registerThreadLocked("main");
    mainThread->thread = pthread_self();
    mainThread->state = SIM_RUNNING;
    runningThread = mainThread;
    selfThread = mainThread;
    pthread_mutex_unlock(&schedulerLock);

    signal(SIGINT, requestDumpOnSignal);
    pthread_t watchdog;
    pthread_create(&watchdog, NULL, watchdogThread, NULL);
    pthread_detach(watchdog);
}

uint64_t simNowUs(void) { return __atomic_load_n(&virtualNowUs, __ATOMIC_RELAXED); }

int64_t esp_timer_get_time(void) { return (int64_t)simNowUs(); }

void simSleepUs(uint64_t us) {
    pthread_mutex_lock(&schedulerLock);
    SimThread *self = currentSimThread();
    if (us == 0) {
        makeReadyLocked(self);          /* a yield: run again after the threads already ready */
    } else {
        self->state = SIM_SLEEPING;
        self->wakeAtUs = virtualNowUs + us;
        self->waitObject = NULL;
    }
    switchAwayLocked(self);
    pthread_mutex_unlock(&schedulerLock);
}

void simWaitObject(const void *object, uint64_t deadlineUs, const char *label) {
    pthread_mutex_lock(&schedulerLock);
    SimThread *self = currentSimThread();
    bool isPastDeadline = (deadlineUs <= virtualNowUs);
    if (!isPastDeadline) {
        self->state = SIM_WAITING;
        self->wakeAtUs = deadlineUs;
        self->waitObject = object;
        self->waitLabel = label;
        self->waitStartUs = virtualNowUs;
        self->waitSequence = nextWaitSequence;
        nextWaitSequence++;
        switchAwayLocked(self);
    }
    pthread_mutex_unlock(&schedulerLock);
}

bool simCpuCostEnabled = true;

/**
 * @brief Charges `us` virtual microseconds of work to the calling thread (see
 *        espmock.h). Does nothing for 0 or with the cost model off, so it never adds
 *        a switch that a run without it would not have.
 *   simSpendCpuUs(4300);   // one mic frame's echo cancelling at 16 kHz
 */
void simSpendCpuUs(uint64_t us) {
    bool isCharged = (simCpuCostEnabled && us > 0);
    if (!isCharged) {
        return;
    }
    simSleepUs(us);
}

void simWakeObject(const void *object) {
    pthread_mutex_lock(&schedulerLock);
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        bool isWaitingOnObject = (thread->state == SIM_WAITING && thread->waitObject == object);
        if (isWaitingOnObject) {
            makeReadyLocked(thread);
        }
    }
    pthread_mutex_unlock(&schedulerLock);
}

/**
 * @brief Readies only the thread that has waited longest on object and returns it, or
 *        NULL if none waits.
 */
static SimThread *wakeLongestWaiter(const void *object) {
    pthread_mutex_lock(&schedulerLock);
    SimThread *longestWaiter = NULL;
    for (int index = 0; index < simThreadCount; index++) {
        SimThread *thread = simThreads[index];
        bool isWaitingOnObject = (thread->state == SIM_WAITING && thread->waitObject == object);
        if (!isWaitingOnObject) {
            continue;
        }
        bool hasWaitedLonger = (longestWaiter == NULL || thread->waitSequence < longestWaiter->waitSequence);
        if (hasWaitedLonger) {
            longestWaiter = thread;
        }
    }
    if (longestWaiter != NULL) {
        makeReadyLocked(longestWaiter);
    }
    pthread_mutex_unlock(&schedulerLock);
    return longestWaiter;
}

/**
 * @brief The virtual deadline for a FreeRTOS wait of `ticks` (1 tick = 1 ms here);
 *        portMAX_DELAY waits forever.
 */
static uint64_t deadlineAfterTicks(TickType_t ticks) {
    if (ticks == portMAX_DELAY) {
        return SIM_NO_DEADLINE;
    }
    uint64_t waitUs = (uint64_t)ticks * 1000ull;
    uint64_t deadlineUs = simNowUs() + waitUs;
    return deadlineUs;
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
    uint32_t notifyValue;
    char name[24];
    uint32_t stackBytes;
    int core;
};

#define MAX_TASKS 16
static struct MockTask *taskList[MAX_TASKS];
static int taskCount = 0;
volatile int mockShutdown = 0;

/* The task running on this thread. Set by the thread itself, so it is valid from the
   task's first instruction. */
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
    (void)prio;   /* the scheduler runs ready threads in order, not by priority */
    if (taskCount >= MAX_TASKS) {
        fprintf(stderr, "mock: more than %d tasks\n", MAX_TASKS);
        return pdFALSE;
    }
    struct MockTask *t = calloc(1, sizeof(*t));
    if (!t) return pdFALSE;
    t->fn = fn; t->arg = arg;
    t->stackBytes = stack; t->core = core;
    snprintf(t->name, sizeof(t->name), "%s", name);
    if (out) *out = t;
    taskList[taskCount++] = t;
    /* Ready, not running: it first runs when this thread next blocks. */
    t->thread = simThreadCreate(taskTrampoline, t, name);
    return pdPASS;
}

UBaseType_t uxTaskGetSystemState(TaskStatus_t *statusArray, UBaseType_t arraySize,
                                 configRUN_TIME_COUNTER_TYPE *totalRunTime) {
    UBaseType_t count = (UBaseType_t)taskCount;
    if (count > arraySize) return 0;
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
    uint64_t deadline = deadlineAfterTicks(wait);
    while (t->notifyValue == 0) {
        if (simNowUs() >= deadline) break;
        simWaitObject(t, deadline, "notification");
    }
    uint32_t v = t->notifyValue;
    t->notifyValue = clearOnExit ? 0 : (v ? v - 1 : 0);
    return v;
}

void xTaskNotifyGive(TaskHandle_t t) {
    if (!t) return;
    t->notifyValue++;
    simWakeObject(t);
}

/* Only one simulated thread runs at a time and it switches only inside a blocking
   mock, so the counts below need no host lock.

   A give with a thread waiting hands the semaphore straight to the longest waiter
   instead of raising the count. Without that, a task that gives a mutex and takes it
   again before its next block (microphoneTask does, once per frame) would keep it
   forever: the woken waiter only runs once the giver blocks, and by then the count is
   back to 0. On the ESP32 the waiter gets it at once, from the other core or by
   preempting. */
struct MockSem { int count; int isMutex; };

SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    struct MockSem *s = calloc(1, sizeof(*s));
    s->count = 1; s->isMutex = 1; return s;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) {
    struct MockSem *s = calloc(1, sizeof(*s));
    s->count = 0; return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait) {
    if (!s) return pdFALSE;
    SimThread *self = currentSimThread();
    uint64_t deadline = deadlineAfterTicks(wait);
    for (;;) {
        bool wasHandedOver = (self->handOffObject == s);   /* checked first, so it is never left set */
        if (wasHandedOver) {
            self->handOffObject = NULL;
            return pdTRUE;
        }
        if (s->count > 0) {
            s->count--;
            return pdTRUE;
        }
        if (simNowUs() >= deadline) return pdFALSE;
        simWaitObject(s, deadline, s->isMutex ? "mutex" : "semaphore");
    }
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    if (!s) return pdFALSE;
    bool isAvailableToGive = (!s->isMutex || s->count == 0);
    if (!isAvailableToGive) return pdTRUE;
    SimThread *waiter = wakeLongestWaiter(s);
    if (waiter != NULL) {
        waiter->handOffObject = s;   /* its xSemaphoreTake() returns pdTRUE; the count stays 0 */
    } else {
        s->count++;
    }
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
    simWakeObject(r);
    return pdTRUE;
}
void *xRingbufferReceive(RingbufHandle_t r, size_t *outSize, TickType_t w) {
    uint64_t deadline = deadlineAfterTicks(w);
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
        if (simNowUs() >= deadline) { *outSize = 0; return NULL; }
        simWaitObject(r, deadline, "ring buffer");
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
    simWakeObject(c);
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
    simWakeObject(c);
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
    uint64_t deadline = deadlineAfterTicks(wait);

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
                if (simNowUs() >= deadline) break;
                simWaitObject(c, deadline, "i2s write");   /* until the hardware frees one */
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

#define MOCK_FAILED_READ_US 100
esp_err_t i2s_channel_read(i2s_chan_handle_t c, void *dst, size_t n, size_t *readOut, TickType_t wait) {
    if (readOut) *readOut = 0;
    if (!c || !c->alive || !c->enabled) {
        /* Returns at once, as the driver does. Costs a little virtual time, so a task
           retrying it in a loop still lets the rest of the simulation run. */
        simSleepUs(MOCK_FAILED_READ_US);
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t deadline = deadlineAfterTicks(wait);
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
        if (simNowUs() >= deadline) return ESP_ERR_TIMEOUT;
        simWaitObject(c, deadline, "i2s read");   /* until the hardware adds a descriptor */
    }
}

/* hardware side, called by the simulated I2S clock */
int mockI2sTxPop(i2s_chan_handle_t c, uint8_t *out) {
    if (!c || !c->alive) return 0;
    pthread_mutex_lock(&c->m);
    if (!c->enabled) { pthread_mutex_unlock(&c->m); return 0; }
    mockTxPops++;
    i2s_isr_callback_t onSent = c->callbacks.on_sent;
    i2s_isr_callback_t onSendOverflow = c->callbacks.on_send_q_ovf;
    void *context = c->callbackContext;
    if (c->readyCount == 0) {
        mockI2sTxUnderruns++;
        int emptyBytes = c->descBytes;
        pthread_mutex_unlock(&c->m);
        if (onSendOverflow) {   /* the driver reports an underrun as a send queue overflow */
            i2s_event_data_t event = { NULL, (size_t)emptyBytes };
            onSendOverflow(c, &event, context);
        }
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
    simWakeObject(c);   /* a descriptor is free for a blocked write */
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
    i2s_isr_callback_t onReceive = c->callbacks.on_recv;
    i2s_isr_callback_t onOverflow = c->callbacks.on_recv_q_ovf;
    void *context = c->callbackContext;
    void *filledDescriptor = c->desc[slot];
    pthread_mutex_unlock(&c->m);
    simWakeObject(c);   /* a descriptor is ready for a blocked read */
    if (onReceive) {    /* the driver reports every filled descriptor */
        i2s_event_data_t event = { filledDescriptor, (size_t)nb };
        onReceive(c, &event, context);
    }
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
esp_avrc_ct_cb_t mockAvrcCtCb = NULL;
esp_err_t esp_avrc_ct_register_callback(esp_avrc_ct_cb_t cb) { mockAvrcCtCb = cb; return ESP_OK; }
esp_err_t esp_avrc_ct_send_get_rn_capabilities_cmd(uint8_t tl) { (void)tl; return ESP_OK; }
esp_err_t esp_avrc_ct_send_register_notification_cmd(uint8_t tl, uint8_t e, uint32_t p) { (void)tl; (void)e; (void)p; return ESP_OK; }
bool esp_avrc_rn_evt_bit_mask_operation(esp_avrc_bit_mask_op_t op, esp_avrc_rn_evt_cap_mask_t *events,
                                        esp_avrc_rn_event_ids_t id) {
    uint16_t bit = (uint16_t)(1u << id);
    if (op == ESP_AVRC_BIT_MASK_OP_SET) { events->bits |= bit; return true; }
    if (op == ESP_AVRC_BIT_MASK_OP_CLEAR) { events->bits &= (uint16_t)~bit; return true; }
    return (events->bits & bit) != 0;
}

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
esp_err_t esp_hf_client_start_voice_recognition(void) { return ESP_OK; }
esp_err_t esp_hf_client_stop_voice_recognition(void) { return ESP_OK; }
esp_err_t esp_hf_client_outgoing_data_ready(void) {
    __sync_fetch_and_add(&mockOutgoingReadyPending, 1);
    simWakeObject((const void *)&mockOutgoingReadyPending);   /* the phone thread, as Bluedroid's task */
    return ESP_OK;
}
