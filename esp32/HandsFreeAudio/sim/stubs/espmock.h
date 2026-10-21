/*
 * Host-side stand-ins for the ESP-IDF, FreeRTOS and Bluedroid APIs that main.c uses,
 * plus the hooks the simulator (../sim.c) needs to drive and inspect them. Every
 * header main.c includes (freertos/task.h, driver/i2s_std.h, esp_bt.h ...) is a
 * one-line forwarder to this file. A new IDF call in main.c needs its declaration
 * here and its body in espmock.c.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* ===================== time base ===================== */
/* All simulated time is real time multiplied by simSpeedup. */
extern double simSpeedup;
uint64_t simNowUs(void);
int64_t  esp_timer_get_time(void);          /* virtual microseconds since start */
void     simSleepUs(uint64_t us); /* sleep for `us` virtual microseconds */

/* ===================== errors / log ===================== */
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_NVS_NO_FREE_PAGES 0x1110
#define ESP_ERR_NVS_NEW_VERSION_FOUND 0x1111
#define ESP_ERROR_CHECK(x) do { esp_err_t rc__ = (x); if (rc__ != ESP_OK) { fprintf(stderr,"ESP_ERROR_CHECK failed %d\n", rc__); abort(); } } while (0)

void mockLog(char level, const char *tag, const char *fmt, ...);
#define ESP_LOGI(tag, ...) mockLog('I', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) mockLog('W', tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) mockLog('E', tag, __VA_ARGS__)
extern long mockWarnCount, mockErrCount;
void mockDumpMessages(void);
void mockResetMessages(void);
extern int  mockLogVerbose;

/* Set by the simulator to make every blocking mock return, so the run can end. */
extern volatile int mockShutdown;

/* ===================== FreeRTOS ===================== */
typedef int32_t  BaseType_t;
typedef uint32_t TickType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define configMAX_PRIORITIES 25

typedef struct MockTask         *TaskHandle_t;
typedef struct MockSem          *SemaphoreHandle_t;
typedef struct MockStreamBuffer *StreamBufferHandle_t;
typedef struct MockRingBuffer   *RingbufHandle_t;

typedef void (*TaskFunction_t)(void *);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, uint32_t prio, TaskHandle_t *out, int core);
void       vTaskDelay(TickType_t ticks);
uint32_t   ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t wait);
void       xTaskNotifyGive(TaskHandle_t task);

/* Task statistics for main.c's diagnostics. Run time is the host thread's CPU time
   in microseconds and the total is real elapsed microseconds, so a task's share is
   of one host core. There are no idle tasks, so idle reads 0%. */
typedef uint32_t UBaseType_t;
#define configRUN_TIME_COUNTER_TYPE uint32_t
#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)
typedef struct {
    TaskHandle_t                xHandle;
    const char                 *pcTaskName;
    configRUN_TIME_COUNTER_TYPE ulRunTimeCounter;
    uint32_t                    usStackHighWaterMark;   /* the task's whole stack */
} TaskStatus_t;
UBaseType_t  uxTaskGetSystemState(TaskStatus_t *statusArray, UBaseType_t arraySize,
                                  configRUN_TIME_COUNTER_TYPE *totalRunTime);
UBaseType_t  uxTaskGetStackHighWaterMark(TaskHandle_t task);   /* the task's whole stack */
TaskHandle_t xTaskGetIdleTaskHandleForCore(BaseType_t core);   /* always NULL */
BaseType_t   xTaskGetCoreID(TaskHandle_t task);                /* the core it was created on */

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t        xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t        xSemaphoreGive(SemaphoreHandle_t s);

StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger);
size_t     xStreamBufferSend(StreamBufferHandle_t b, const void *d, size_t n, TickType_t w);
size_t     xStreamBufferReceive(StreamBufferHandle_t b, void *d, size_t n, TickType_t w);
size_t     xStreamBufferBytesAvailable(StreamBufferHandle_t b);
size_t     xStreamBufferSpacesAvailable(StreamBufferHandle_t b);
BaseType_t xStreamBufferReset(StreamBufferHandle_t b);

typedef enum { RINGBUF_TYPE_NOSPLIT = 0, RINGBUF_TYPE_ALLOWSPLIT, RINGBUF_TYPE_BYTEBUF } RingbufferType_t;
RingbufHandle_t xRingbufferCreate(size_t size, RingbufferType_t type);
BaseType_t      xRingbufferSend(RingbufHandle_t r, const void *d, size_t n, TickType_t w);
void           *xRingbufferReceive(RingbufHandle_t r, size_t *outSize, TickType_t w);
void            vRingbufferReturnItem(RingbufHandle_t r, void *item);
void            vRingbufferGetInfo(RingbufHandle_t r, void *a, void *b, void *c, void *d, size_t *filled);

/* ===================== I2S ===================== */
typedef struct MockI2sChan *i2s_chan_handle_t;
typedef enum { I2S_NUM_0 = 0, I2S_NUM_1 = 1 } i2s_port_t;
typedef enum { I2S_ROLE_MASTER = 0, I2S_ROLE_SLAVE } i2s_role_t;
typedef enum { I2S_CLK_SRC_DEFAULT = 0, I2S_CLK_SRC_APLL } i2s_clock_src_t;
typedef enum { I2S_DATA_BIT_WIDTH_8BIT = 8, I2S_DATA_BIT_WIDTH_16BIT = 16,
               I2S_DATA_BIT_WIDTH_24BIT = 24, I2S_DATA_BIT_WIDTH_32BIT = 32 } i2s_data_bit_width_t;
typedef enum { I2S_SLOT_BIT_WIDTH_AUTO = 0, I2S_SLOT_BIT_WIDTH_16BIT = 16,
               I2S_SLOT_BIT_WIDTH_32BIT = 32 } i2s_slot_bit_width_t;
typedef enum { I2S_SLOT_MODE_MONO = 0, I2S_SLOT_MODE_STEREO } i2s_slot_mode_t;
typedef enum { I2S_STD_SLOT_LEFT = 0, I2S_STD_SLOT_RIGHT, I2S_STD_SLOT_BOTH } i2s_std_slot_mask_t;
#define I2S_GPIO_UNUSED (-1)

typedef struct {
    i2s_port_t id;
    i2s_role_t role;
    uint32_t   dma_desc_num;
    uint32_t   dma_frame_num;
    bool       auto_clear;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(port, r) ((i2s_chan_config_t){ .id = (port), .role = (r), \
        .dma_desc_num = 6, .dma_frame_num = 240, .auto_clear = false })

typedef struct {
    uint32_t        sample_rate_hz;
    i2s_clock_src_t clk_src;
    uint32_t        mclk_multiple;
} i2s_std_clk_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) ((i2s_std_clk_config_t){ .sample_rate_hz = (rate), \
        .clk_src = I2S_CLK_SRC_DEFAULT, .mclk_multiple = 256 })

typedef struct {
    i2s_data_bit_width_t data_bit_width;
    i2s_slot_bit_width_t slot_bit_width;
    i2s_slot_mode_t      slot_mode;
    i2s_std_slot_mask_t  slot_mask;
    i2s_data_bit_width_t ws_width;
    bool                 ws_pol;
    bool                 bit_shift;
    bool                 msb_right;
} i2s_std_slot_config_t;

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) ((i2s_std_slot_config_t){ \
        .data_bit_width = (bits), .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = (mode), \
        .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = (bits), .ws_pol = false, \
        .bit_shift = true, .msb_right = false })

typedef struct { int mclk, bclk, ws, dout, din; } i2s_std_gpio_config_t;
typedef struct {
    i2s_std_clk_config_t  clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

/* Event callbacks. The simulated I2S clock (sim.c's hardware thread) calls on_sent
   for every descriptor it plays (zeros on an underrun, as auto_clear does),
   on_send_q_ovf on a speaker underrun and on_recv_q_ovf when the mic queue drops a
   descriptor. */
#define IRAM_ATTR
typedef struct { void *dma_buf; size_t size; } i2s_event_data_t;
typedef bool (*i2s_isr_callback_t)(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx);
typedef struct {
    i2s_isr_callback_t on_recv;
    i2s_isr_callback_t on_recv_q_ovf;
    i2s_isr_callback_t on_sent;
    i2s_isr_callback_t on_send_q_ovf;
} i2s_event_callbacks_t;
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t h, const i2s_event_callbacks_t *callbacks, void *user_data);

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_del_channel(i2s_chan_handle_t h);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h, const i2s_std_config_t *cfg);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t h, const i2s_std_clk_config_t *cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t h);
esp_err_t i2s_channel_disable(i2s_chan_handle_t h);
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *src, size_t n, size_t *written, TickType_t wait);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t h, const void *src, size_t n, size_t *loaded);
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *dst, size_t n, size_t *read, TickType_t wait);

/* introspection for the test suite */
extern int      mockI2sLiveChannels;      /* alloc/free balance */
extern long     mockI2sTxUnderruns, mockI2sRxOverruns;
extern long mockWrNull, mockWrDead, mockWrDisabled, mockWrTimeout, mockWrOk;
extern long     mockTxAppBytes, mockTxPopBytes, mockTxPreloadBytes, mockTxPops;
extern i2s_chan_handle_t mockTxChan, mockRxChan;
size_t   mockI2sTxQueuedBytes(i2s_chan_handle_t h);
int      mockI2sDescBytes(i2s_chan_handle_t h);
/* hardware side, called by the simulated I2S clock: take one descriptor from the
   DAC queue (negative byte count on an underrun), or add one to the mic queue */
int      mockI2sTxPop(i2s_chan_handle_t h, uint8_t *out);
int      mockI2sRxPush(i2s_chan_handle_t h, const uint8_t *in);
uint32_t mockI2sRate(i2s_chan_handle_t h);
bool     mockI2sEnabled(i2s_chan_handle_t h);

/* ===================== heap ===================== */
/* Fixed figures: the host heap says nothing about the ESP32's. */
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_INTERNAL (1 << 11)
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
size_t heap_caps_get_minimum_free_size(uint32_t caps);

/* ===================== GPIO ===================== */
typedef enum { GPIO_NUM_4 = 4, GPIO_NUM_18 = 18, GPIO_NUM_19 = 19, GPIO_NUM_21 = 21,
               GPIO_NUM_22 = 22, GPIO_NUM_23 = 23, GPIO_NUM_25 = 25, GPIO_NUM_26 = 26,
               GPIO_NUM_32 = 32, GPIO_NUM_34 = 34 } gpio_num_t;
#define GPIO_INTR_DISABLE 0
#define GPIO_MODE_INPUT 1
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
typedef struct { int intr_type, mode; uint64_t pin_bit_mask; int pull_down_en, pull_up_en; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *c);
int       gpio_get_level(gpio_num_t pin);

/* ===================== NVS ===================== */
typedef int nvs_handle_t;
#define NVS_READWRITE 1
#define NVS_READONLY  0
esp_err_t nvs_flash_init(void);
esp_err_t nvs_flash_erase(void);
esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h);
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n);
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *v, size_t *n);
esp_err_t nvs_commit(nvs_handle_t h);
void      nvs_close(nvs_handle_t h);

/* ===================== Bluetooth ===================== */
typedef uint8_t esp_bd_addr_t[6];
typedef struct { int dummy; } esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() ((esp_bt_controller_config_t){ .dummy = 0 })
#define ESP_BT_MODE_BLE        1
#define ESP_BT_MODE_CLASSIC_BT 2
typedef enum { ESP_BT_NON_CONNECTABLE = 0, ESP_BT_CONNECTABLE } esp_bt_connection_mode_t;
typedef enum { ESP_BT_NON_DISCOVERABLE = 0, ESP_BT_LIMITED_DISCOVERABLE, ESP_BT_GENERAL_DISCOVERABLE } esp_bt_discovery_mode_t;
esp_err_t esp_bt_controller_mem_release(int mode);
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *c);
esp_err_t esp_bt_controller_enable(int mode);
esp_err_t esp_bluedroid_init(void);
esp_err_t esp_bluedroid_enable(void);
esp_err_t esp_bt_gap_set_device_name(const char *n);
esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t c, esp_bt_discovery_mode_t d);
uint8_t  *esp_bt_dev_get_address(void);
extern long mockScanModeChanges;

/* AVRCP */
typedef enum { ESP_AVRC_PT_CMD_PLAY = 0x44, ESP_AVRC_PT_CMD_PAUSE = 0x46,
               ESP_AVRC_PT_CMD_FORWARD = 0x4B, ESP_AVRC_PT_CMD_BACKWARD = 0x4C } esp_avrc_pt_cmd_t;
typedef enum { ESP_AVRC_PT_CMD_STATE_PRESSED = 0, ESP_AVRC_PT_CMD_STATE_RELEASED } esp_avrc_pt_cmd_state_t;
esp_err_t esp_avrc_ct_init(void);
esp_err_t esp_avrc_ct_send_passthrough_cmd(uint8_t tl, uint8_t key, uint8_t state);
typedef enum { ESP_AVRC_CT_CONNECTION_STATE_EVT = 0, ESP_AVRC_CT_PASSTHROUGH_RSP_EVT,
               ESP_AVRC_CT_CHANGE_NOTIFY_EVT, ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT } esp_avrc_ct_cb_event_t;
typedef enum { ESP_AVRC_RN_PLAY_STATUS_CHANGE = 0x01 } esp_avrc_rn_event_ids_t;
typedef enum { ESP_AVRC_PLAYBACK_STOPPED = 0, ESP_AVRC_PLAYBACK_PLAYING, ESP_AVRC_PLAYBACK_PAUSED,
               ESP_AVRC_PLAYBACK_FWD_SEEK, ESP_AVRC_PLAYBACK_REV_SEEK } esp_avrc_playback_stat_t;
typedef enum { ESP_AVRC_BIT_MASK_OP_TEST = 0, ESP_AVRC_BIT_MASK_OP_SET, ESP_AVRC_BIT_MASK_OP_CLEAR } esp_avrc_bit_mask_op_t;
typedef struct { uint16_t bits; } esp_avrc_rn_evt_cap_mask_t;
typedef union { esp_avrc_playback_stat_t playback; uint8_t volume; } esp_avrc_rn_param_t;
typedef union {
    struct { bool connected; esp_bd_addr_t remote_bda; } conn_stat;
    struct { uint8_t event_id; esp_avrc_rn_param_t event_parameter; } change_ntf;
    struct { uint8_t cap_count; esp_avrc_rn_evt_cap_mask_t evt_set; } get_rn_caps_rsp;
} esp_avrc_ct_cb_param_t;
typedef void (*esp_avrc_ct_cb_t)(esp_avrc_ct_cb_event_t e, esp_avrc_ct_cb_param_t *p);
esp_err_t esp_avrc_ct_register_callback(esp_avrc_ct_cb_t cb);
esp_err_t esp_avrc_ct_send_get_rn_capabilities_cmd(uint8_t tl);
esp_err_t esp_avrc_ct_send_register_notification_cmd(uint8_t tl, uint8_t event_id, uint32_t event_parameter);
bool esp_avrc_rn_evt_bit_mask_operation(esp_avrc_bit_mask_op_t op, esp_avrc_rn_evt_cap_mask_t *events,
                                        esp_avrc_rn_event_ids_t event_id);
extern esp_avrc_ct_cb_t mockAvrcCtCb;

/* A2DP */
typedef enum { ESP_A2D_CONNECTION_STATE_EVT = 0, ESP_A2D_AUDIO_STATE_EVT, ESP_A2D_AUDIO_CFG_EVT } esp_a2d_cb_event_t;
typedef enum { ESP_A2D_CONNECTION_STATE_DISCONNECTED = 0, ESP_A2D_CONNECTION_STATE_CONNECTING,
               ESP_A2D_CONNECTION_STATE_CONNECTED } esp_a2d_connection_state_t;
typedef enum { ESP_A2D_AUDIO_STATE_SUSPEND = 0, ESP_A2D_AUDIO_STATE_STARTED } esp_a2d_audio_state_t;
#define ESP_A2D_MCT_SBC 0
typedef union {
    struct { esp_a2d_connection_state_t state; esp_bd_addr_t remote_bda; } conn_stat;
    struct { esp_a2d_audio_state_t state; esp_bd_addr_t remote_bda; } audio_stat;
    struct { struct { int type; uint8_t cie[10]; } mcc; } audio_cfg;
} esp_a2d_cb_param_t;
typedef void (*esp_a2d_cb_t)(esp_a2d_cb_event_t e, esp_a2d_cb_param_t *p);
typedef void (*esp_a2d_sink_data_cb_t)(const uint8_t *d, uint32_t n);
esp_err_t esp_a2d_register_callback(esp_a2d_cb_t cb);
esp_err_t esp_a2d_sink_register_data_callback(esp_a2d_sink_data_cb_t cb);
esp_err_t esp_a2d_sink_init(void);
esp_err_t esp_a2d_sink_connect(esp_bd_addr_t addr);
extern esp_a2d_cb_t           mockA2dCb;
extern esp_a2d_sink_data_cb_t mockA2dDataCb;

/* HFP client */
typedef enum {
    ESP_HF_CLIENT_CONNECTION_STATE_EVT = 0,
    ESP_HF_CLIENT_AUDIO_STATE_EVT,
    ESP_HF_CLIENT_CIND_CALL_EVT,
    ESP_HF_CLIENT_CIND_CALL_SETUP_EVT,
    ESP_HF_CLIENT_BVRA_EVT
} esp_hf_client_cb_event_t;
typedef enum { ESP_HF_VR_STATE_DISABLED = 0, ESP_HF_VR_STATE_ENABLED } esp_hf_vr_state_t;
typedef enum { ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED = 0, ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING,
               ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED, ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED,
               ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING } esp_hf_client_connection_state_t;
typedef enum { ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED = 0, ESP_HF_CLIENT_AUDIO_STATE_CONNECTING,
               ESP_HF_CLIENT_AUDIO_STATE_CONNECTED, ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC } esp_hf_client_audio_state_t;
typedef enum { ESP_HF_CALL_STATUS_NO_CALLS = 0, ESP_HF_CALL_STATUS_CALL_IN_PROGRESS } esp_hf_call_status_t;
typedef enum { ESP_HF_CALL_SETUP_STATUS_IDLE = 0, ESP_HF_CALL_SETUP_STATUS_INCOMING,
               ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING, ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING } esp_hf_call_setup_status_t;
typedef union {
    struct { esp_hf_client_connection_state_t state; esp_bd_addr_t remote_bda; } conn_stat;
    struct { esp_hf_client_audio_state_t state; } audio_stat;
    struct { esp_hf_vr_state_t value; } bvra;
    struct { esp_hf_call_status_t status; } call;
    struct { esp_hf_call_setup_status_t status; } call_setup;
} esp_hf_client_cb_param_t;
typedef void (*esp_hf_client_cb_t)(esp_hf_client_cb_event_t e, esp_hf_client_cb_param_t *p);
typedef void (*esp_hf_client_incoming_data_cb_t)(const uint8_t *buf, uint32_t len);
typedef uint32_t (*esp_hf_client_outgoing_data_cb_t)(uint8_t *buf, uint32_t len);
esp_err_t esp_hf_client_register_callback(esp_hf_client_cb_t cb);
esp_err_t esp_hf_client_register_data_callback(esp_hf_client_incoming_data_cb_t r, esp_hf_client_outgoing_data_cb_t s);
esp_err_t esp_hf_client_init(void);
esp_err_t esp_hf_client_reject_call(void);
esp_err_t esp_hf_client_start_voice_recognition(void);
esp_err_t esp_hf_client_stop_voice_recognition(void);
esp_err_t esp_hf_client_outgoing_data_ready(void);
extern esp_hf_client_cb_t               mockHfCb;
extern esp_hf_client_incoming_data_cb_t mockHfRecvCb;
extern esp_hf_client_outgoing_data_cb_t mockHfSendCb;
extern volatile long mockOutgoingReadyPending;   /* posted to the BT task queue */
