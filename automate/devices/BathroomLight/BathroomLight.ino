// =====================================================================================
// BathroomLight.ino
// Raspberry Pi Pico W (arduino-pico core) bathroom light controller:
//   - one 12-bit PWM LED (pwmLED0) with anti-logarithmic brightness and non-blocking fades
//   - light modes: always off, motion activated (a DFRobot C4002 mmWave presence sensor
//     switches the light on and off) or always on
//   - HTTP command API on port 4000, settings stored in emulated EEPROM
//   - rolling in-memory log and radar status returned over HTTP
//
// Layout: every type and global is declared first, then every function is defined
// before the functions that call it, so the sketch needs no function prototypes.
// Keeping all types above the first function also keeps the Arduino IDE's own
// automatic prototypes valid.
// =====================================================================================

// shared libraries
#include <WiFi.h>
#include <neotimer.h>
#include <DFRobot_C4002.h>
#include <EEPROM.h>
#include <stdarg.h>

// local libraries
#include "creds.h"

// ---- GPIO pins ----
const uint8_t PWM_LED0_POWER_PIN = 19; // GP19 = physical pin 25
const uint8_t RADAR_TX_PIN = 0;        // GP0 (UART0 TX) = physical pin 1, wire to C4002 RX
const uint8_t RADAR_RX_PIN = 1;        // GP1 (UART0 RX) = physical pin 2, wire to C4002 TX
const uint8_t CYW43_GPIO_PIN_OFFSET = 64; // arduino-pico numbers the Wi-Fi chip's GPIOs from 64
const uint8_t REGULATOR_MODE_PIN = CYW43_GPIO_PIN_OFFSET + CYW43_WL_GPIO_SMPS_PIN; // RT6150 PS pin, HIGH = PWM mode

// ---- common resources ----
const char NULL_TERMINATOR = '\0';
const char NEW_LINE = '\n';
const char CARRIAGE_RETURN = '\r';
const uint32_t MS_PER_SECOND = 1000;
const uint32_t MS_PER_TENTH_SECOND = 100;
const uint32_t TENTHS_PER_SECOND = 10;
const uint32_t SECONDS_PER_MINUTE = 60;
const uint32_t SECONDS_PER_HOUR = 3600;
const uint32_t SECONDS_PER_DAY = 86400;
const uint32_t MINUTES_PER_HOUR = 60;
const uint32_t HOURS_PER_DAY = 24;
const uint16_t STARTUP_SETTLE_MS = 1000; // pause after starting Wi-Fi, before the web server starts

// ---- Wi-Fi and web server ----
const uint16_t HTTP_SERVER_PORT = 4000;
const uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000; // how often a dropped connection is retried
const uint32_t WIFI_STATUS_INTERVAL_MS = 1000;     // how often connection changes are checked for the log
const uint16_t WIFI_DISCONNECT_SETTLE_MS = 500;    // pause between disconnecting and reconnecting
const uint32_t WIFI_POWER_SAVE_BITS = 0xf;         // power-save mode bits cleared to disable power management
WiFiServer httpServer(HTTP_SERVER_PORT);
Neotimer wifiReconnectTimer = Neotimer(WIFI_RECONNECT_INTERVAL_MS);
Neotimer wifiStatusTimer = Neotimer(WIFI_STATUS_INTERVAL_MS);
bool wifiConnected = false; // last connection state written to the log

// ---- rolling log (the oldest entries are overwritten, each is stamped with the uptime) ----
const uint8_t LOG_CAPACITY = 60;     // entries kept
const uint8_t LOG_TEXT_LENGTH = 80;  // max characters per entry, including the terminator
const uint8_t TIMESTAMP_TEXT_SIZE = 24; // "+<days>d HH:MM:SS" plus terminator
struct LogEntry {
  uint32_t uptimeMs;             // millis() when logged
  char text[LOG_TEXT_LENGTH];
};
LogEntry logBuffer[LOG_CAPACITY];
uint8_t logNextIndex = 0;  // entry that the next message overwrites
uint8_t logEntryCount = 0; // entries stored so far, up to LOG_CAPACITY

// ---- command decoder: requests carry "?<command>,<unused>,<value>~" in the URL ----
const uint8_t COMMAND_MESSAGE_LENGTH = 32;
const uint8_t COMMAND_FIELD_MAX_DIGITS = 4; // 4 digits so ranges up to 1100 cm fit
const char COMMAND_FRAME_START = '?';
const char COMMAND_FRAME_END = '~';
const char COMMAND_FIELD_DELIMITER = ',';
const uint8_t COMMAND_FIRST_FIELD_START = 0;
const uint8_t COMMAND_VALUE_DELIMITER_OCCURRENCE = 2; // the value follows the second delimiter
const uint8_t COMMAND_DELIMITER_NOT_FOUND = 0;        // the second delimiter can never be at position 0
char commandMessageIn[COMMAND_MESSAGE_LENGTH + 1];
char commandReplyOut[COMMAND_MESSAGE_LENGTH + 1];
uint16_t commandFramePosition = 0;
bool commandFrameReading = false;

// command numbers (first field)
const uint16_t COMMAND_LIGHT = 2;               // value: light action below
const uint16_t COMMAND_SET_LEVEL = 3;           // value: level 0-255
const uint16_t COMMAND_SET_RANGE_MIN = 4;       // value: cm
const uint16_t COMMAND_SET_RANGE_MAX = 5;       // value: cm
const uint16_t COMMAND_SET_PRESENCE_HOLDS = 6;  // value: 0/1
const uint16_t COMMAND_SET_MOTION_TIMEOUT = 7;  // value: seconds
const uint16_t COMMAND_READ_SETTINGS = 8;       // value: 0 = all, or a setting command number
const uint16_t COMMAND_READ_LOG = 9;            // value: 0 = all entries, N = newest N
const uint16_t COMMAND_RADAR_STATUS = 10;       // value: 0 = live status, 1 = also read the sensor back
const uint16_t COMMAND_RESET_SETTINGS = 12;     // value: 1 to confirm
const uint16_t COMMAND_SET_LIGHT_MODE = 13;     // value: light mode below

// light actions (value of COMMAND_LIGHT)
const uint16_t LIGHT_ACTION_FADE_OFF = 0;
const uint16_t LIGHT_ACTION_FADE_ON = 1;
const uint16_t LIGHT_ACTION_READ_LEVEL = 2;
const uint16_t LIGHT_ACTION_READ_TARGET_STATE = 3;
const uint16_t LIGHT_ACTION_READ_ON_STATE = 4;
const int16_t LIGHT_STATE_OFF = 0; // reply of LIGHT_ACTION_READ_ON_STATE
const int16_t LIGHT_STATE_ON = 1;

// light modes (value of COMMAND_SET_LIGHT_MODE), stored with the settings
const uint16_t LIGHT_MODE_ALWAYS_OFF = 0; // the light stays off, commands can't turn it on
const uint16_t LIGHT_MODE_MOTION = 1;     // detections turn the light on and off
const uint16_t LIGHT_MODE_ALWAYS_ON = 2;  // the light stays on, commands can't turn it off
const char *const LIGHT_MODE_NAMES[] = { "always off", "motion activated", "always on" };
const uint8_t LIGHT_MODE_NAME_COUNT = sizeof(LIGHT_MODE_NAMES) / sizeof(LIGHT_MODE_NAMES[0]);
static_assert(LIGHT_MODE_NAME_COUNT == LIGHT_MODE_ALWAYS_ON + 1, "one name per light mode");

// other command values
const uint16_t READ_ALL_SETTINGS = 0;
const uint16_t RADAR_STATUS_WITH_READ_BACK = 1;
const uint16_t FLAG_MAX_VALUE = 1;
const uint16_t RESET_SETTINGS_CONFIRM = 1;

// ---- HTTP buffers ----
const uint8_t HTTP_REQUEST_BUFFER_SIZE = 255;
const uint16_t HTTP_CLIENT_TIMEOUT_MS = 1000; // client inactivity timeout
const uint16_t HTTP_BODY_BUFFER_SIZE = 6144;  // multi-line replies (log, status)
const uint8_t HTTP_LINE_ENDING_LENGTH = 1;    // the newline sent after every reply body
char httpRequestBuffer[HTTP_REQUEST_BUFFER_SIZE];
char httpBodyBuffer[HTTP_BODY_BUFFER_SIZE];
uint16_t httpBodyLength = 0;
bool httpReplyUsesBody = false; // true when the reply is in httpBodyBuffer instead of commandReplyOut

// ---- C4002 mmWave presence sensor ----
const uint32_t RADAR_BAUD_RATE = 115200;        // C4002 factory default
const uint8_t RADAR_REPORT_PERIOD = 3;          // sensor report interval in tenths of a second (300 ms)
const uint8_t RADAR_REPORTS_PAUSED_PERIOD = 255; // report period value that stops the report stream
const uint16_t RADAR_DISAPPEAR_DELAY_S = 5;     // sensor's own hold time, included within motionTimeout
const float RADAR_LIGHT_GATING_OFF = 0;         // light threshold 0 = detection never depends on the (covered) light sensor
const eThreshGroup_t RADAR_MOTION_SENSITIVITY = eHighThreshGroup;   // eLowThreshGroup / eMidThreshGroup / eHighThreshGroup
const eThreshGroup_t RADAR_PRESENCE_SENSITIVITY = eMidThreshGroup;  // static presence (breathing, sitting still)
const uint8_t RADAR_CONFIG_ATTEMPTS = 3;        // attempts per configuration command
const uint16_t RADAR_CONFIG_SETTLE_MS = 50;     // pause after an accepted command
const uint16_t RADAR_CONFIG_RETRY_MS = 100;     // pause before retrying a rejected command
const uint32_t RADAR_RETRY_INTERVAL_MS = 2000; // until the sensor answers (e.g. still starting up), try again this often
const uint16_t RADAR_UART_FIFO_SIZE = 256;      // receive buffer, survives short superloop stalls
const uint8_t RADAR_GATE_COUNT_MAX = 25;        // 25 gates at 20 cm resolution, 15 at 80 cm

// report frame: FA F5 AA A5 | length(2, LE, whole frame) | 00 | type | command | response | dataLength(2) | data... | checksum(2, LE)
const uint8_t RADAR_FRAME_MAX = 64;              // largest frame accepted
const uint8_t RADAR_READ_BYTES_PER_LOOP = 128;   // max bytes parsed per superloop pass
const uint8_t RADAR_FRAME_HEADER_LENGTH = 4;
const uint8_t RADAR_FRAME_HEADER[RADAR_FRAME_HEADER_LENGTH] = { FRAME_HEADER1, FRAME_HEADER2, FRAME_HEADER3, FRAME_HEADER4 };
const uint8_t RADAR_FRAME_LENGTH_INDEX = 4;      // 2 bytes, little endian
const uint8_t RADAR_FRAME_LENGTH_FIELD_END = 6;  // bytes received once the length field is complete
const uint8_t RADAR_FRAME_TYPE_INDEX = 7;
const uint8_t RADAR_FRAME_COMMAND_INDEX = 8;
const uint8_t RADAR_FRAME_DATA_LENGTH_INDEX = 10; // 2 bytes, little endian, counts the 4-byte data header too
const uint8_t RADAR_FRAME_DATA_INDEX = 12;        // first data byte, the target state in reports
const uint8_t RADAR_FRAME_CHECKSUM_LENGTH = 2;
const uint8_t RADAR_FRAME_MIN_LENGTH = 14;
const uint8_t RADAR_REPORT_MIN_LENGTH = 15;
const uint8_t RADAR_DATA_HEADER_LENGTH = 4;
const uint8_t RADAR_REPORT_DATA_MAX = 24;         // report data bytes kept for the status reply

// report data layouts from the DFRobot library: 18 bytes when packed, 20 when 32-bit aligned
const uint8_t RADAR_REPORT_PACKED_LENGTH = 18;
const uint8_t RADAR_REPORT_ALIGNED_LENGTH = 20;
struct RadarReportLayout {
  uint8_t presenceDistanceIndex; // 2 bytes, cm
  uint8_t presenceEnergyIndex;
  uint8_t motionDistanceIndex;   // 2 bytes, cm
  uint8_t motionEnergyIndex;
};
const RadarReportLayout RADAR_REPORT_PACKED_LAYOUT = { 9, 11, 12, 16 };
const RadarReportLayout RADAR_REPORT_ALIGNED_LAYOUT = { 10, 12, 14, 18 };

// configuration commands sent to the sensor
enum RadarSetting {
  RADAR_SETTING_REPORTS_PAUSED,
  RADAR_SETTING_REPORTS_RESUMED,
  RADAR_SETTING_LIGHT_GATING_OFF,
  RADAR_SETTING_MOTION_SENSITIVITY,
  RADAR_SETTING_PRESENCE_SENSITIVITY,
  RADAR_SETTING_MOTION_GATES,
  RADAR_SETTING_PRESENCE_GATES,
  RADAR_SETTING_DETECT_RANGE,
  RADAR_SETTING_DISAPPEAR_DELAY,
  RADAR_SETTING_COUNT
};
// the name each configuration command is logged under, in RadarSetting order
const char *const RADAR_SETTING_NAMES[] = {
  "report pause", "report period", "light gating off", "motion sensitivity", "presence sensitivity",
  "motion gates", "presence gates", "detect range", "disappear delay"
};
static_assert(sizeof(RADAR_SETTING_NAMES) / sizeof(RADAR_SETTING_NAMES[0]) == RADAR_SETTING_COUNT, "one name per RadarSetting");
// sent in this order at boot, reports are resumed last so they can't interfere with the replies
const RadarSetting RADAR_BOOT_SEQUENCE[] = {
  RADAR_SETTING_LIGHT_GATING_OFF,
  RADAR_SETTING_MOTION_SENSITIVITY,
  RADAR_SETTING_PRESENCE_SENSITIVITY,
  RADAR_SETTING_MOTION_GATES,
  RADAR_SETTING_PRESENCE_GATES,
  RADAR_SETTING_DETECT_RANGE,
  RADAR_SETTING_DISAPPEAR_DELAY,
  RADAR_SETTING_REPORTS_RESUMED
};
const uint8_t RADAR_BOOT_SEQUENCE_LENGTH = sizeof(RADAR_BOOT_SEQUENCE) / sizeof(RADAR_BOOT_SEQUENCE[0]);

// readable names for the status reply, indexed by the library's enum values
const char *const TARGET_NAMES[] = { "none", "static presence", "motion" };
const uint8_t TARGET_NAME_COUNT = sizeof(TARGET_NAMES) / sizeof(TARGET_NAMES[0]);
static_assert(eNoTarget == 0 && ePresence == 1 && eMotion == 2, "TARGET_NAMES follows eTargetState_t");
const char *const SENSITIVITY_NAMES[] = { "low", "medium", "high", "custom (calibrated)" };
const uint8_t SENSITIVITY_NAME_COUNT = sizeof(SENSITIVITY_NAMES) / sizeof(SENSITIVITY_NAMES[0]);
static_assert(eLowThreshGroup == 0 && eMidThreshGroup == 1 && eHighThreshGroup == 2 && eCustomThreshGroup == 3,
              "SENSITIVITY_NAMES follows eThreshGroup_t");

// the library keeps getLedStatus() protected, this exposes it for the status reply
class C4002Sensor : public DFRobot_C4002 {
public:
  using DFRobot_C4002::DFRobot_C4002;
  using DFRobot_C4002::getLedStatus;
};
C4002Sensor radarSensor(&Serial1, RADAR_BAUD_RATE);

// radar state
uint8_t radarFrameBuffer[RADAR_FRAME_MAX];
uint8_t radarFrameReceived = 0;          // bytes of the current frame received so far
uint16_t radarFrameExpectedLength = 0;   // whole-frame length from the frame's length field
uint8_t radarTargetState = eNoTarget;    // last reported state: 0=none, 1=static presence, 2=motion
bool roomOccupied = false;               // true while something was detected within motionTimeout
uint32_t lastDetectionMs = 0;
bool radarConfigured = false;            // true once the sensor has answered and been configured
uint32_t radarLastAttemptMs = 0;         // when the sensor was last tried while it wasn't answering
uint32_t radarFramesValid = 0;
uint32_t radarFramesBadChecksum = 0;
uint32_t radarLastReportMs = 0;
bool radarHasReport = false;
uint8_t radarLastReportData[RADAR_REPORT_DATA_MAX]; // data bytes of the latest detection report
uint8_t radarLastReportDataLength = 0;

// ---- persistent settings (each firmware build starts from the defaults, see loadSettings) ----
const uint16_t RADAR_RANGE_MIN_DEFAULT_CM = 0;    // nearest detection distance
const uint16_t RADAR_RANGE_MAX_DEFAULT_CM = 200;  // farthest detection distance
const uint8_t PRESENCE_HOLDS_LIGHT_DEFAULT = 1;   // 1 = static presence (e.g. sitting still) also keeps the light on
const uint16_t MOTION_TIMEOUT_DEFAULT_S = 30;     // light fades off after no detection for this long
const uint16_t LIGHT_MODE_DEFAULT = LIGHT_MODE_MOTION;
const uint16_t RADAR_RANGE_LIMIT_CM = 1000;       // sensor maximum distance
const uint16_t MOTION_TIMEOUT_MAX_S = 9999;       // longest timeout accepted
static_assert(RADAR_DISAPPEAR_DELAY_S < MOTION_TIMEOUT_DEFAULT_S, "RADAR_DISAPPEAR_DELAY_S must be shorter than the motion timeout");
const uint32_t SETTINGS_SAVE_INTERVAL_MS = 300000; // minimum time between EEPROM writes, 5 minutes
const uint16_t SETTINGS_EEPROM_SIZE = 256;         // emulated EEPROM size in bytes
const uint16_t SETTINGS_EEPROM_ADDRESS = 0;
const uint8_t EEPROM_ERASED_BYTE = 0xFF;           // value every byte is wiped to
const uint16_t SETTINGS_MAGIC = 0xB1A7;            // marks a stored settings block
// compile date and time: every newly compiled upload has a different stamp, so its firmware ID
// differs from the one stored with settings saved by any earlier build
const char FIRMWARE_BUILD_STAMP[] = __DATE__ " " __TIME__;
const uint32_t FNV_OFFSET_BASIS = 2166136261u;     // 32-bit FNV-1a hash constants
const uint32_t FNV_PRIME = 16777619u;
struct Settings {
  uint32_t firmwareId;        // ID of the firmware build that saved this block
  uint16_t magic;
  uint8_t presenceHoldsLight;
  uint8_t lightMode;          // LIGHT_MODE_ALWAYS_OFF / LIGHT_MODE_MOTION / LIGHT_MODE_ALWAYS_ON
  uint16_t radarRangeMin;     // cm
  uint16_t radarRangeMax;     // cm
  uint16_t motionTimeout;     // seconds
  uint16_t checksum;
};
// the checksum, the stored-copy comparison and the EEPROM image all treat Settings as raw
// bytes, so it must not contain padding bytes (their contents are undefined)
static_assert(sizeof(Settings) == 16, "Settings must not contain padding bytes");
Settings settings;       // live settings
Settings savedSettings;  // copy of what is stored in EEPROM
bool settingsSavePending = false; // true while a change is waiting to be written
uint32_t settingsLastSaveMs = 0;  // time of the last EEPROM write (boot counts as one)

// ---- LED light ----
const uint8_t PWM_MAX_LEVEL = 255;              // API brightness range: 0 = off, 255 = 100%
const uint16_t PWM_DUTY_MAX = 4095;              // 12-bit duty cycle: 0 = off, 4095 = always on
// The PWM counter runs from the system clock through a whole-number divider, so every period is
// exactly the same length (a fractional divider is dithered, which shimmers at low brightness).
// At 125MHz: 1 = 30.5kHz, 2 = 15.3kHz, 4 = 7.6kHz. 2 doubles the shortest pulse (level 1 = 32ns)
// so the TC4420 and MOSFET switch the lowest levels more evenly, while staying near-silent.
const uint32_t PWM_CLOCK_DIVIDER = 2;
static_assert(PWM_CLOCK_DIVIDER >= 1 && PWM_CLOCK_DIVIDER <= 255, "the PWM clock divider must be 1-255");
const uint32_t PWM_CLOCKS_PER_PERIOD = PWM_DUTY_MAX * PWM_CLOCK_DIVIDER; // system clocks in one PWM period
const uint32_t PWM_FREQUENCY_HZ = F_CPU / PWM_CLOCKS_PER_PERIOD;
const uint8_t PWM_LED0_FADE_STEP_MS = 8;         // per level for normal fades, ~2s full range
const uint8_t PWM_LED0_MOTION_FADE_STEP_MS = 20; // per level for motion fade-in, ~5s full range
const uint16_t ACTIVITY_LED_BLINK_MS = 100;      // on-board LED lights this long for each command
bool activityLedOn = false;                      // true while the on-board LED shows a command
uint32_t activityLedOnMs = 0;                    // when the on-board LED was last lit
static_assert(PWM_LED0_FADE_STEP_MS > 0, "fade step time must not be zero");
static_assert(PWM_LED0_MOTION_FADE_STEP_MS > 0, "fade step time must not be zero");
int16_t pwmLED0Level = 0;                        // 0 = off, 255 = 100%
int16_t pwmLED0OnLevel = PWM_MAX_LEVEL;          // level motion and always on fade in to, set by direct control
int16_t pwmLED0TargetLevel = 0;                  // level the LED is at or heading to, a fade runs while they differ
uint8_t pwmLED0FadeStepMs = PWM_LED0_FADE_STEP_MS; // step time of the active fade
uint32_t pwmLED0LastStepMs = 0;                  // time of the last fade step
// CIE 1931 lightness curve: converts a 0-255 brightness level to a 12-bit duty cycle so equal
// level steps look like equal brightness steps. Every level has its own duty cycle, so level 1
// is just visible and fades have no flat spots.
const uint16_t ANTI_LOGARITHMIC_TABLE[256] = {
      0,    2,    4,    5,    7,    9,   11,   12,   14,   16,   18,   20,   21,   23,   25,   27,
     28,   30,   32,   34,   36,   37,   39,   41,   43,   45,   47,   49,   52,   54,   56,   59,
     61,   64,   66,   69,   72,   75,   77,   80,   83,   87,   90,   93,   96,  100,  103,  107,
    111,  115,  118,  122,  126,  131,  135,  139,  144,  148,  153,  157,  162,  167,  172,  177,
    182,  187,  193,  198,  204,  209,  215,  221,  227,  233,  239,  246,  252,  259,  265,  272,
    279,  286,  293,  300,  308,  315,  323,  330,  338,  346,  354,  362,  371,  379,  388,  396,
    405,  414,  423,  432,  442,  451,  461,  470,  480,  490,  501,  511,  521,  532,  543,  553,
    564,  576,  587,  598,  610,  622,  634,  646,  658,  670,  683,  695,  708,  721,  734,  748,
    761,  775,  788,  802,  816,  831,  845,  860,  874,  889,  904,  920,  935,  951,  966,  982,
    999, 1015, 1031, 1048, 1065, 1082, 1099, 1116, 1134, 1152, 1170, 1188, 1206, 1224, 1243, 1262,
   1281, 1300, 1320, 1339, 1359, 1379, 1399, 1420, 1440, 1461, 1482, 1503, 1525, 1546, 1568, 1590,
   1612, 1635, 1657, 1680, 1703, 1726, 1750, 1774, 1797, 1822, 1846, 1870, 1895, 1920, 1945, 1971,
   1996, 2022, 2048, 2074, 2101, 2128, 2155, 2182, 2209, 2237, 2265, 2293, 2321, 2350, 2378, 2407,
   2437, 2466, 2496, 2526, 2556, 2587, 2617, 2648, 2679, 2711, 2743, 2774, 2807, 2839, 2872, 2905,
   2938, 2971, 3005, 3039, 3073, 3107, 3142, 3177, 3212, 3248, 3283, 3319, 3356, 3392, 3429, 3466,
   3503, 3541, 3578, 3617, 3655, 3694, 3732, 3772, 3811, 3851, 3891, 3931, 3972, 4012, 4054, 4095,
};

// ==== rolling log ====

// Adds one printf-style entry to the rolling log, overwriting the oldest entry when full.
// Each entry records the uptime it was logged at.
void logMessage(const char *format, ...) {
  LogEntry &entry = logBuffer[logNextIndex];
  entry.uptimeMs = millis();
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(entry.text, sizeof(entry.text), format, arguments);
  va_end(arguments);
  uint16_t followingIndex = logNextIndex + 1;
  logNextIndex = followingIndex % LOG_CAPACITY;
  if (logEntryCount < LOG_CAPACITY) {
    logEntryCount = logEntryCount + 1;
  }
}

// Formats an uptime as "+<days>d HH:MM:SS".
void formatUptime(char *output, size_t outputSize, uint32_t uptimeMs) {
  uint32_t totalSeconds = uptimeMs / MS_PER_SECOND;
  uint32_t totalMinutes = totalSeconds / SECONDS_PER_MINUTE;
  uint32_t totalHours = totalSeconds / SECONDS_PER_HOUR;
  uint32_t days = totalSeconds / SECONDS_PER_DAY;
  uint32_t hours = totalHours % HOURS_PER_DAY;
  uint32_t minutes = totalMinutes % MINUTES_PER_HOUR;
  uint32_t seconds = totalSeconds % SECONDS_PER_MINUTE;
  snprintf(output, outputSize, "+%lud %02lu:%02lu:%02lu",
           (unsigned long)days, (unsigned long)hours, (unsigned long)minutes, (unsigned long)seconds);
}

// ==== HTTP reply text ====

// Starts a multi-line reply in httpBodyBuffer, replacing the single-value reply.
void startBodyReply() {
  httpBodyLength = 0;
  httpBodyBuffer[0] = NULL_TERMINATOR;
  httpReplyUsesBody = true;
}

// Appends printf-style text, given as a va_list, to the multi-line reply; text that does
// not fit is cut off. The length is summed in 32 bits so it can never wrap around.
void appendBodyFormatted(const char *format, va_list arguments) {
  uint16_t lastUsableIndex = HTTP_BODY_BUFFER_SIZE - 1;
  if (httpBodyLength >= lastUsableIndex) {
    return;
  }
  char *writePosition = httpBodyBuffer + httpBodyLength;
  size_t spaceRemaining = HTTP_BODY_BUFFER_SIZE - httpBodyLength;
  int charactersWritten = vsnprintf(writePosition, spaceRemaining, format, arguments);
  if (charactersWritten > 0) {
    uint32_t extendedLength = (uint32_t)httpBodyLength + (uint32_t)charactersWritten;
    if (extendedLength > lastUsableIndex) {
      extendedLength = lastUsableIndex;
    }
    httpBodyLength = (uint16_t)extendedLength;
  }
}

// Appends printf-style text to the multi-line reply.
void appendBody(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

// Appends a status line: the label padded to 21 characters, then the value. Indented lines
// put their two spaces at the start of the label.
void appendBodyField(const char *label, const char *format, ...) {
  appendBody("%-21s", label);
  va_list arguments;
  va_start(arguments, format);
  appendBodyFormatted(format, arguments);
  va_end(arguments);
}

// Sets a single number as the reply.
void replyWithNumber(int16_t value) {
  snprintf(commandReplyOut, sizeof(commandReplyOut), "%d", value);
}

// Reads a little-endian 16-bit value from a byte buffer.
uint16_t readLittleEndianUint16(const uint8_t *buffer, uint16_t index) {
  uint16_t lowByte = buffer[index];
  uint16_t highByte = buffer[index + 1];
  uint16_t shiftedHighByte = highByte << 8;
  uint16_t value = lowByte | shiftedHighByte;
  return value;
}

// ==== persistent settings ====

// Returns the 16-bit sum of a run of bytes, the checksum used by the settings and the sensor.
uint16_t sumBytes(const uint8_t *bytes, size_t length) {
  uint16_t byteSum = 0;
  for (size_t byteIndex = 0; byteIndex < length; byteIndex++) {
    byteSum = byteSum + bytes[byteIndex];
  }
  return byteSum;
}

// Returns the checksum of every settings byte before the checksum field.
uint16_t calculateSettingsChecksum(const Settings &candidate) {
  const uint8_t *settingsBytes = (const uint8_t *)&candidate;
  size_t checksummedLength = offsetof(Settings, checksum);
  return sumBytes(settingsBytes, checksummedLength);
}

// Returns true when every setting value is within its allowed range.
bool settingValuesInRange(const Settings &candidate) {
  bool presenceFlagValid = (candidate.presenceHoldsLight <= FLAG_MAX_VALUE);
  bool rangeMaxWithinLimit = (candidate.radarRangeMax <= RADAR_RANGE_LIMIT_CM);
  bool rangeMinBelowMax = (candidate.radarRangeMin < candidate.radarRangeMax);
  bool timeoutLongerThanSensorHold = (candidate.motionTimeout > RADAR_DISAPPEAR_DELAY_S);
  bool timeoutWithinLimit = (candidate.motionTimeout <= MOTION_TIMEOUT_MAX_S);
  bool lightModeKnown = (candidate.lightMode <= LIGHT_MODE_ALWAYS_ON);
  bool valuesInRange = presenceFlagValid && rangeMaxWithinLimit && rangeMinBelowMax &&
                       timeoutLongerThanSensorHold && timeoutWithinLimit && lightModeKnown;
  return valuesInRange;
}

// Returns the ID of this firmware build: a 32-bit FNV-1a hash of its compile date and time.
uint32_t calculateFirmwareId() {
  uint32_t hash = FNV_OFFSET_BASIS;
  size_t stampLength = strlen(FIRMWARE_BUILD_STAMP);
  for (size_t characterIndex = 0; characterIndex < stampLength; characterIndex++) {
    uint8_t character = (uint8_t)FIRMWARE_BUILD_STAMP[characterIndex];
    uint32_t mixedHash = hash ^ character;
    hash = mixedHash * FNV_PRIME;
  }
  return hash;
}

// Returns true when a settings block was saved by this firmware build, is intact and every
// value is within its allowed range.
bool settingsAreValid(const Settings &candidate) {
  uint16_t expectedChecksum = calculateSettingsChecksum(candidate);
  uint32_t thisFirmwareId = calculateFirmwareId();
  bool hasMagic = (candidate.magic == SETTINGS_MAGIC);
  bool savedByThisFirmware = (candidate.firmwareId == thisFirmwareId);
  bool checksumMatches = (candidate.checksum == expectedChecksum);
  bool blockIsIntact = hasMagic && savedByThisFirmware && checksumMatches;
  bool valuesInRange = settingValuesInRange(candidate);
  bool allValid = blockIsIntact && valuesInRange;
  return allValid;
}

// Fills a settings block with the default values, including its firmware ID, marker and checksum.
void applyDefaultSettings(Settings &target) {
  target.firmwareId = calculateFirmwareId();
  target.magic = SETTINGS_MAGIC;
  target.presenceHoldsLight = PRESENCE_HOLDS_LIGHT_DEFAULT;
  target.radarRangeMin = RADAR_RANGE_MIN_DEFAULT_CM;
  target.radarRangeMax = RADAR_RANGE_MAX_DEFAULT_CM;
  target.motionTimeout = MOTION_TIMEOUT_DEFAULT_S;
  target.lightMode = (uint8_t)LIGHT_MODE_DEFAULT;
  target.checksum = calculateSettingsChecksum(target);
}

// Erases every byte of the emulated EEPROM and stores the live settings in it, so nothing saved
// by another firmware build survives. Returns true when the flash write succeeded.
bool wipeSettingsStorage() {
  for (uint16_t address = 0; address < SETTINGS_EEPROM_SIZE; address++) {
    EEPROM.write(address, EEPROM_ERASED_BYTE);
  }
  EEPROM.put(SETTINGS_EEPROM_ADDRESS, settings);
  bool committed = EEPROM.commit();
  return committed;
}

// Restores the settings this firmware build saved in EEPROM. Settings are never carried over from
// another build: on the first boot after an upload (or when the block is missing or damaged) the
// EEPROM is wiped and the defaults are stored. Boot counts as a write, so the first save can
// happen 5 minutes after power-up.
void loadSettings() {
  EEPROM.begin(SETTINGS_EEPROM_SIZE);
  EEPROM.get(SETTINGS_EEPROM_ADDRESS, settings);
  bool storedSettingsValid = settingsAreValid(settings);
  if (storedSettingsValid) {
    logMessage("settings restored: range %u-%ucm, presence %u, timeout %us, mode %u",
               settings.radarRangeMin, settings.radarRangeMax,
               settings.presenceHoldsLight, settings.motionTimeout, settings.lightMode);
  } else {
    applyDefaultSettings(settings);
    bool wiped = wipeSettingsStorage();
    if (wiped) {
      logMessage("no settings from this firmware: EEPROM wiped, using defaults");
    } else {
      logMessage("EEPROM wipe failed, using defaults");
    }
  }
  savedSettings = settings;
  settingsLastSaveMs = millis();
}

// Marks the settings as changed, so they are written on the next allowed save.
void markSettingsChanged() {
  settings.checksum = calculateSettingsChecksum(settings);
  settingsSavePending = true;
}

// Writes pending settings to EEPROM, at most once per SETTINGS_SAVE_INTERVAL_MS to limit flash
// wear. Several quick changes cost one write, and nothing is written when the settings were
// changed back to what is already stored.
void saveSettingsWhenDue() {
  if (!settingsSavePending) {
    return;
  }
  uint32_t msSinceLastSave = millis() - settingsLastSaveMs;
  bool intervalElapsed = (msSinceLastSave >= SETTINGS_SAVE_INTERVAL_MS);
  if (!intervalElapsed) {
    return;
  }
  settingsSavePending = false;
  int comparison = memcmp(&settings, &savedSettings, sizeof(Settings));
  bool matchesStoredCopy = (comparison == 0);
  if (matchesStoredCopy) {
    return;
  }
  EEPROM.put(SETTINGS_EEPROM_ADDRESS, settings);
  bool committed = EEPROM.commit();
  if (committed) {
    savedSettings = settings;
    logMessage("settings saved to EEPROM");
  } else {
    settingsSavePending = true; // retry after the next interval
    logMessage("settings save to EEPROM failed, retrying in 5 min");
  }
  settingsLastSaveMs = millis();
}

// Returns how long the Pico keeps the room occupied after the sensor's own hold time ends.
// The sensor's disappear delay plus this hold add up to the motion timeout.
uint32_t calculateOccupancyHoldMs() {
  uint32_t motionTimeoutSeconds = settings.motionTimeout;
  uint32_t sensorHoldSeconds = RADAR_DISAPPEAR_DELAY_S;
  uint32_t picoHoldSeconds = motionTimeoutSeconds - sensorHoldSeconds;
  uint32_t picoHoldMs = picoHoldSeconds * MS_PER_SECOND;
  return picoHoldMs;
}

// ==== LED light ====

// Holds the Pico W's RT6150 3.3V regulator in fixed-frequency PWM mode. Its default
// power-saving mode skips switching cycles at light load, which leaves a low-frequency
// ripple on the 3.3V rail; PWM mode keeps the rail steady for a few mA more idle current.
void forceRegulatorPwmMode() {
  pinMode(REGULATOR_MODE_PIN, OUTPUT);
  digitalWrite(REGULATOR_MODE_PIN, HIGH);
}

// Limits a requested brightness level to 0-255.
int16_t clampPwmLevel(int16_t requestedLevel) {
  int16_t clampedLevel = requestedLevel;
  if (clampedLevel >= PWM_MAX_LEVEL) {
    clampedLevel = PWM_MAX_LEVEL;
  }
  if (clampedLevel <= 0) {
    clampedLevel = 0;
  }
  return clampedLevel;
}

// Sets pwmLED0's brightness: the level is clamped, converted to a 12-bit duty cycle through
// the anti-logarithmic table, and fully on or off levels use a plain digital output.
void setPwmLED0(int16_t requestedLevel) {
  int16_t level = clampPwmLevel(requestedLevel);
  uint16_t dutyCycle = ANTI_LOGARITHMIC_TABLE[level];
  bool fullyOn = (dutyCycle == PWM_DUTY_MAX);
  bool fullyOff = (dutyCycle == 0);
  if (fullyOn) {
    digitalWrite(PWM_LED0_POWER_PIN, HIGH);
  } else if (fullyOff) {
    digitalWrite(PWM_LED0_POWER_PIN, LOW);
  } else {
    analogWrite(PWM_LED0_POWER_PIN, dutyCycle);
  }
  pwmLED0Level = level;
}

// Starts a non-blocking fade from the current level to the target level. stepMs sets the
// speed, in milliseconds per level. A new fade replaces any fade already in progress.
void startFadePwmLED0(int16_t requestedTarget, uint8_t stepMs) {
  pwmLED0TargetLevel = clampPwmLevel(requestedTarget);
  pwmLED0FadeStepMs = stepMs;
  pwmLED0LastStepMs = millis();
}

// Advances an active fade (the level differs from the target); called from the superloop. Steps
// missed while the loop was busy (e.g. serving a slow client) are applied together, so the fade
// keeps its overall duration.
void updatePwmLED0Fade() {
  bool fadeInProgress = (pwmLED0Level != pwmLED0TargetLevel);
  if (!fadeInProgress) {
    return;
  }
  uint32_t msSinceLastStep = millis() - pwmLED0LastStepMs;
  bool stepIsDue = (msSinceLastStep >= pwmLED0FadeStepMs);
  if (!stepIsDue) {
    return;
  }
  uint32_t stepsDue = msSinceLastStep / pwmLED0FadeStepMs;
  uint32_t msAccountedFor = stepsDue * pwmLED0FadeStepMs;
  pwmLED0LastStepMs = pwmLED0LastStepMs + msAccountedFor;
  int16_t levelDifference = pwmLED0TargetLevel - pwmLED0Level;
  int16_t stepsRemaining = abs(levelDifference);
  if (stepsDue > (uint32_t)stepsRemaining) {
    stepsDue = stepsRemaining;
  }
  int16_t stepsToApply = (int16_t)stepsDue;
  bool fadingUp = (pwmLED0TargetLevel > pwmLED0Level);
  int16_t nextLevel = 0;
  if (fadingUp) {
    nextLevel = pwmLED0Level + stepsToApply;
  } else {
    nextLevel = pwmLED0Level - stepsToApply;
  }
  setPwmLED0(nextLevel);
}

// Lights the on-board LED to show a command ran; updateActivityLed() turns it off again, so
// nothing waits. A command arriving while it is lit restarts the blink.
void blinkActivityLed() {
  if (!activityLedOn) {
    digitalWrite(LED_BUILTIN, HIGH);
  }
  activityLedOn = true;
  activityLedOnMs = millis();
}

// Turns the on-board LED off once it has been lit for ACTIVITY_LED_BLINK_MS; called from the superloop.
void updateActivityLed() {
  if (!activityLedOn) {
    return;
  }
  uint32_t msLit = millis() - activityLedOnMs;
  bool blinkFinished = (msLit >= ACTIVITY_LED_BLINK_MS);
  if (!blinkFinished) {
    return;
  }
  digitalWrite(LED_BUILTIN, LOW);
  activityLedOn = false;
}

// ==== light modes ====

// Returns the readable name of a light mode.
const char *lightModeName(uint16_t lightMode) {
  bool knownMode = (lightMode < LIGHT_MODE_NAME_COUNT);
  if (!knownMode) {
    return "unknown";
  }
  return LIGHT_MODE_NAMES[lightMode];
}

// Returns true when the light mode wants the light on: always on does, always off doesn't, and
// motion activated does while the room is occupied.
bool lightModeWantsLightOn() {
  bool alwaysOn = (settings.lightMode == LIGHT_MODE_ALWAYS_ON);
  bool motionActivated = (settings.lightMode == LIGHT_MODE_MOTION);
  bool motionWantsLightOn = motionActivated && roomOccupied;
  bool wantsLightOn = alwaysOn || motionWantsLightOn;
  return wantsLightOn;
}

// Brings the light to what the light mode calls for, at boot and after a mode change: it fades
// in to the on level or fades off. A light that is already where the mode wants it is left alone.
void applyLightMode() {
  bool wantsLightOn = lightModeWantsLightOn();
  bool lightOnOrFadingOn = (pwmLED0TargetLevel > 0);
  bool needsTurningOn = wantsLightOn && !lightOnOrFadingOn;
  bool needsTurningOff = !wantsLightOn && lightOnOrFadingOn;
  const char *modeName = lightModeName(settings.lightMode);
  if (needsTurningOn) {
    startFadePwmLED0(pwmLED0OnLevel, PWM_LED0_FADE_STEP_MS);
    logMessage("%s: light fading in to %d", modeName, pwmLED0OnLevel);
  } else if (needsTurningOff) {
    startFadePwmLED0(0, PWM_LED0_FADE_STEP_MS);
    logMessage("%s: light fading off", modeName);
  }
}

// Returns true when a command may turn the light on: never in always off mode, and in motion
// activated mode only while the room is occupied or the light is already on, so an empty room
// stays dark until motion is detected.
bool commandMayTurnLightOn() {
  bool alwaysOff = (settings.lightMode == LIGHT_MODE_ALWAYS_OFF);
  bool motionActivated = (settings.lightMode == LIGHT_MODE_MOTION);
  bool lightOnOrFadingOn = (pwmLED0TargetLevel > 0);
  bool roomEmptyAndDark = !roomOccupied && !lightOnOrFadingOn;
  bool motionKeepsLightOff = motionActivated && roomEmptyAndDark;
  bool mayTurnOn = !alwaysOff && !motionKeepsLightOff;
  return mayTurnOn;
}

// Returns true when a command may turn the light off: in every mode except always on.
bool commandMayTurnLightOff() {
  bool alwaysOn = (settings.lightMode == LIGHT_MODE_ALWAYS_ON);
  bool mayTurnOff = !alwaysOn;
  return mayTurnOff;
}

// Returns why a command was not allowed to turn the light on, for the log.
const char *lightStaysOffReason() {
  bool alwaysOff = (settings.lightMode == LIGHT_MODE_ALWAYS_OFF);
  if (alwaysOff) {
    return "always off mode";
  }
  return "motion activated, room empty";
}

// ==== Wi-Fi ====

// Sets Wi-Fi client mode with the hostname from creds.h and connects to the configured network.
void connectWiFi() {
  logMessage("connecting to Wi-Fi %s", ssid);
  WiFi.mode(WIFI_STA);
  // the hostname must be set before WiFi.begin so the DHCP request carries it
  WiFi.setHostname(hostname);
  // disable power management by clearing the power-save mode bits
  uint32_t powerManagementMode = CYW43_DEFAULT_PM & ~WIFI_POWER_SAVE_BITS;
  cyw43_wifi_pm(&cyw43_state, powerManagementMode);
  WiFi.begin(ssid, ssid_pwd);
}

// Logs when the Wi-Fi connection is made or lost, checked once a second.
void logWiFiStateChanges() {
  bool checkIsDue = wifiStatusTimer.repeat();
  if (!checkIsDue) {
    return;
  }
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  bool stateChanged = (connectedNow != wifiConnected);
  if (!stateChanged) {
    return;
  }
  wifiConnected = connectedNow;
  if (connectedNow) {
    IPAddress localAddress = WiFi.localIP();
    String addressText = localAddress.toString(); // kept alive while its text is logged
    long signalStrength = (long)WiFi.RSSI();
    logMessage("Wi-Fi connected, IP %s, RSSI %ld dBm", addressText.c_str(), signalStrength);
  } else {
    logMessage("Wi-Fi connection lost");
  }
}

// Reconnects a dropped Wi-Fi connection, checked every WIFI_RECONNECT_INTERVAL_MS.
void reconnectWiFiWhenDropped() {
  bool checkIsDue = wifiReconnectTimer.repeat();
  if (!checkIsDue) {
    return;
  }
  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  if (connectedNow) {
    return;
  }
  WiFi.disconnect();
  delay(WIFI_DISCONNECT_SETTLE_MS);
  connectWiFi();
}

// ==== C4002 configuration ====

// Discards any partly received report frame. Needed after every library exchange, because
// the library reads (and flushes) the same serial port the report parser uses.
void resetRadarFrameParser() {
  radarFrameReceived = 0;
}

// Enables every distance gate of one type; the detect range then limits the distance.
bool enableAllRadarGates(eDistanceGateType_t gateType) {
  uint8_t gateStates[RADAR_GATE_COUNT_MAX];
  memset(gateStates, C4002_ENABLE, sizeof(gateStates));
  bool accepted = radarSensor.configureGate(gateType, gateStates);
  return accepted;
}

// Sends one configuration command to the sensor once; returns true when it was accepted.
bool sendRadarSetting(RadarSetting setting) {
  switch (setting) {
    case RADAR_SETTING_REPORTS_PAUSED:
      return radarSensor.setReportPeriod(RADAR_REPORTS_PAUSED_PERIOD);
    case RADAR_SETTING_REPORTS_RESUMED:
      return radarSensor.setReportPeriod(RADAR_REPORT_PERIOD);
    case RADAR_SETTING_LIGHT_GATING_OFF:
      // the light sensor is covered: threshold 0 turns off light-level gating, so detection
      // never depends on it, whatever the sensor was left set to
      return radarSensor.setLightThresh(RADAR_LIGHT_GATING_OFF);
    case RADAR_SETTING_MOTION_SENSITIVITY:
      return radarSensor.setSensitivity(eMotionDistGate, RADAR_MOTION_SENSITIVITY);
    case RADAR_SETTING_PRESENCE_SENSITIVITY:
      return radarSensor.setSensitivity(ePresenceDistGate, RADAR_PRESENCE_SENSITIVITY);
    case RADAR_SETTING_MOTION_GATES:
      return enableAllRadarGates(eMotionDistGate);
    case RADAR_SETTING_PRESENCE_GATES:
      return enableAllRadarGates(ePresenceDistGate);
    case RADAR_SETTING_DETECT_RANGE:
      // limits detection to the room, ignoring movement beyond radarRangeMax
      return radarSensor.setDetectRange(settings.radarRangeMin, settings.radarRangeMax);
    case RADAR_SETTING_DISAPPEAR_DELAY:
      // holds detection through brief pauses; short holds make the sensor flip to unoccupied
      // and lock itself out of detecting for a further second each time
      return radarSensor.setTargetDisappearDelay(RADAR_DISAPPEAR_DELAY_S);
    default:
      return false;
  }
}

// Sends a configuration command, retrying when it is rejected; logs it by name if every
// attempt fails. Returns true when the sensor accepted it. Resets the report parser after
// the exchange.
bool configureRadarSetting(RadarSetting setting) {
  bool accepted = false;
  for (uint8_t attempt = 0; attempt < RADAR_CONFIG_ATTEMPTS; attempt++) {
    accepted = sendRadarSetting(setting);
    if (accepted) {
      delay(RADAR_CONFIG_SETTLE_MS);
      break;
    }
    delay(RADAR_CONFIG_RETRY_MS);
  }
  resetRadarFrameParser();
  if (!accepted) {
    const char *settingName = RADAR_SETTING_NAMES[setting];
    logMessage("C4002 failed to set %s", settingName);
  }
  return accepted;
}

// Sends the stored detect range to the running sensor. Reports are paused around the change:
// the library takes the first frame it reads as the reply, so a report arriving at the same
// moment would otherwise be mistaken for it, and a rejected change would look accepted.
void updateRadarRange() {
  if (!radarConfigured) {
    // the sensor isn't answering yet: it gets the stored range when it is configured
    return;
  }
  bool paused = configureRadarSetting(RADAR_SETTING_REPORTS_PAUSED);
  if (!paused) {
    return;
  }
  configureRadarSetting(RADAR_SETTING_DETECT_RANGE);
  configureRadarSetting(RADAR_SETTING_REPORTS_RESUMED);
}

// Sends every boot setting in RADAR_BOOT_SEQUENCE, carrying on past failures so as much as
// possible is configured. Returns true when every setting was accepted.
bool applyRadarBootConfiguration() {
  bool allAccepted = true;
  for (uint8_t stepIndex = 0; stepIndex < RADAR_BOOT_SEQUENCE_LENGTH; stepIndex++) {
    RadarSetting setting = RADAR_BOOT_SEQUENCE[stepIndex];
    bool accepted = configureRadarSetting(setting);
    if (!accepted) {
      allAccepted = false;
    }
  }
  return allAccepted;
}

// Tries once to reach the sensor and, when it answers, sends the boot configuration. Returns
// true when the sensor answered. An unanswered attempt blocks for about 0.1 s.
bool tryStartRadar() {
  radarLastAttemptMs = millis();
  // begin() opens Serial1 and pauses reporting while the sensor is configured
  bool sensorAnswered = radarSensor.begin();
  if (!sensorAnswered) {
    return false;
  }
  bool allAccepted = applyRadarBootConfiguration();
  radarConfigured = true;
  if (allAccepted) {
    logMessage("C4002 ready");
  } else {
    logMessage("C4002 found, configuration incomplete");
  }
  return true;
}

// Sets up UART0 for the sensor and tries it once (setup only). A sensor that is still starting
// up is retried from the superloop, so boot isn't held up and the light stays controllable
// over HTTP in the meantime.
void initRadar() {
  // a larger receive buffer lets reports survive short superloop stalls
  // (Wi-Fi reconnect, slow HTTP client)
  Serial1.setTX(RADAR_TX_PIN);
  Serial1.setRX(RADAR_RX_PIN);
  Serial1.setFIFOSize(RADAR_UART_FIFO_SIZE);
  bool sensorAnswered = tryStartRadar();
  if (!sensorAnswered) {
    logMessage("C4002 not responding yet, retrying every %lus",
               (unsigned long)(RADAR_RETRY_INTERVAL_MS / MS_PER_SECOND));
  }
}

// Retries a sensor that hasn't answered yet, every RADAR_RETRY_INTERVAL_MS, until it does.
void retryRadarWhenDue() {
  if (radarConfigured) {
    return;
  }
  uint32_t msSinceAttempt = millis() - radarLastAttemptMs;
  bool retryIsDue = (msSinceAttempt >= RADAR_RETRY_INTERVAL_MS);
  if (retryIsDue) {
    tryStartRadar();
  }
}

// ==== C4002 reports and occupancy ====

// Updates occupancy from a reported target state, and in motion activated mode fades the light
// in when someone arrives. Motion always counts; static presence only keeps an already occupied
// room occupied, and only when presenceHoldsLight is on. Occupancy is tracked in every mode.
void handleRadarTarget(uint8_t targetState) {
  radarTargetState = targetState;
  bool motionDetected = (targetState == eMotion);
  bool presenceDetected = (targetState == ePresence);
  bool presenceMayHoldLight = (settings.presenceHoldsLight != 0);
  bool presenceKeepsRoomOccupied = presenceMayHoldLight && roomOccupied && presenceDetected;
  bool detected = motionDetected || presenceKeepsRoomOccupied;
  if (!detected) {
    return;
  }
  lastDetectionMs = millis();
  if (roomOccupied) {
    return;
  }
  roomOccupied = true;
  bool motionActivated = (settings.lightMode == LIGHT_MODE_MOTION);
  if (!motionActivated) {
    const char *modeName = lightModeName(settings.lightMode);
    logMessage("motion detected, light mode %s", modeName);
    return;
  }
  // fade in only when the light is off or on its way off;
  // a light that is already on keeps its current level
  bool lightOffOrFadingOff = (pwmLED0TargetLevel == 0);
  if (lightOffOrFadingOff) {
    startFadePwmLED0(pwmLED0OnLevel, PWM_LED0_MOTION_FADE_STEP_MS);
    logMessage("motion detected, light fading in to %d", pwmLED0OnLevel);
  } else {
    logMessage("motion detected, light already on");
  }
}

// Keeps the data bytes of a detection report for the status reply.
void storeRadarReportData(uint16_t frameLength) {
  // the frame's data length counts the 4-byte data header, the data itself ends before the checksum
  uint16_t declaredDataLength = readLittleEndianUint16(radarFrameBuffer, RADAR_FRAME_DATA_LENGTH_INDEX);
  uint16_t reportDataLength = 0;
  if (declaredDataLength > RADAR_DATA_HEADER_LENGTH) {
    reportDataLength = declaredDataLength - RADAR_DATA_HEADER_LENGTH;
  }
  uint16_t bytesBeforeChecksum = frameLength - RADAR_FRAME_CHECKSUM_LENGTH;
  uint16_t dataBytesInFrame = bytesBeforeChecksum - RADAR_FRAME_DATA_INDEX;
  if (reportDataLength > dataBytesInFrame) {
    reportDataLength = dataBytesInFrame;
  }
  if (reportDataLength > RADAR_REPORT_DATA_MAX) {
    reportDataLength = RADAR_REPORT_DATA_MAX;
  }
  radarLastReportDataLength = (uint8_t)reportDataLength;
  memcpy(radarLastReportData, &radarFrameBuffer[RADAR_FRAME_DATA_INDEX], radarLastReportDataLength);
  radarLastReportMs = millis();
  radarHasReport = true;
}

// Validates a complete frame and acts on detection reports; other frames are ignored.
void processRadarFrame() {
  uint16_t frameLength = radarFrameExpectedLength;
  // the checksum covers every byte before it
  uint16_t checksumIndex = frameLength - RADAR_FRAME_CHECKSUM_LENGTH;
  uint16_t calculatedChecksum = sumBytes(radarFrameBuffer, checksumIndex);
  uint16_t receivedChecksum = readLittleEndianUint16(radarFrameBuffer, checksumIndex);
  bool checksumMatches = (calculatedChecksum == receivedChecksum);
  if (!checksumMatches) {
    radarFramesBadChecksum = radarFramesBadChecksum + 1;
    return;
  }
  radarFramesValid = radarFramesValid + 1;
  // only detection-result notifications carry the target state
  uint8_t frameType = radarFrameBuffer[RADAR_FRAME_TYPE_INDEX];
  uint8_t frameCommand = radarFrameBuffer[RADAR_FRAME_COMMAND_INDEX];
  bool isNotification = (frameType == FRAME_TYPE_NOTIFICATION);
  bool isDetectionResult = (frameCommand == NOTE_RESULT_CMD);
  bool isDetectionReport = isNotification && isDetectionResult;
  if (!isDetectionReport) {
    return;
  }
  bool longEnough = (frameLength >= RADAR_REPORT_MIN_LENGTH);
  if (!longEnough) {
    return;
  }
  storeRadarReportData(frameLength);
  uint8_t targetState = radarFrameBuffer[RADAR_FRAME_DATA_INDEX];
  handleRadarTarget(targetState);
}

// Feeds one received byte into the frame parser: syncs on the 4-byte header, reads the frame
// length, and processes the frame once it is complete. Corrupt lengths restart the search.
void processRadarByte(uint8_t receivedByte) {
  bool readingHeader = (radarFrameReceived < RADAR_FRAME_HEADER_LENGTH);
  if (readingHeader) {
    uint8_t expectedByte = RADAR_FRAME_HEADER[radarFrameReceived];
    bool headerContinues = (receivedByte == expectedByte);
    bool headerRestarts = (receivedByte == RADAR_FRAME_HEADER[0]);
    if (headerContinues) {
      radarFrameBuffer[radarFrameReceived] = receivedByte;
      radarFrameReceived = radarFrameReceived + 1;
    } else if (headerRestarts) {
      radarFrameBuffer[0] = receivedByte;
      radarFrameReceived = 1;
    } else {
      resetRadarFrameParser();
    }
    return;
  }
  // bounds guard: a frame never outgrows the buffer, its length is checked below
  bool bufferFull = (radarFrameReceived >= RADAR_FRAME_MAX);
  if (bufferFull) {
    resetRadarFrameParser();
    return;
  }
  radarFrameBuffer[radarFrameReceived] = receivedByte;
  radarFrameReceived = radarFrameReceived + 1;
  bool lengthFieldComplete = (radarFrameReceived == RADAR_FRAME_LENGTH_FIELD_END);
  if (lengthFieldComplete) {
    radarFrameExpectedLength = readLittleEndianUint16(radarFrameBuffer, RADAR_FRAME_LENGTH_INDEX);
    bool lengthTooShort = (radarFrameExpectedLength < RADAR_FRAME_MIN_LENGTH);
    bool lengthTooLong = (radarFrameExpectedLength > RADAR_FRAME_MAX);
    if (lengthTooShort || lengthTooLong) {
      resetRadarFrameParser(); // corrupt length, resync
    }
    return;
  }
  bool pastLengthField = (radarFrameReceived > RADAR_FRAME_LENGTH_FIELD_END);
  bool allBytesReceived = (radarFrameReceived >= radarFrameExpectedLength);
  bool frameComplete = pastLengthField && allBytesReceived;
  if (frameComplete) {
    processRadarFrame();
    resetRadarFrameParser();
  }
}

// Non-blocking reader for the sensor's report frames, limited to RADAR_READ_BYTES_PER_LOOP
// bytes per superloop pass so the rest of the loop keeps running.
void readRadar() {
  for (uint8_t bytesRead = 0; bytesRead < RADAR_READ_BYTES_PER_LOOP; bytesRead++) {
    int bytesWaiting = Serial1.available();
    if (bytesWaiting <= 0) {
      break;
    }
    int receivedValue = Serial1.read();
    uint8_t receivedByte = (uint8_t)receivedValue;
    processRadarByte(receivedByte);
  }
}

// Marks the room vacant once nothing has been detected for the motion timeout (the sensor's
// disappear delay plus the Pico's occupancy hold), and in motion activated mode fades the light off.
void updateOccupancy() {
  if (!roomOccupied) {
    return;
  }
  uint32_t msSinceDetection = millis() - lastDetectionMs;
  uint32_t occupancyHoldMs = calculateOccupancyHoldMs();
  bool stillHeld = (msSinceDetection < occupancyHoldMs);
  if (stillHeld) {
    return;
  }
  roomOccupied = false;
  bool motionActivated = (settings.lightMode == LIGHT_MODE_MOTION);
  bool lightOnOrFadingOn = (pwmLED0TargetLevel > 0);
  bool fadeLightOff = motionActivated && lightOnOrFadingOn;
  if (fadeLightOff) {
    startFadePwmLED0(0, PWM_LED0_FADE_STEP_MS);
    logMessage("no detection for %us, light fading off", settings.motionTimeout);
  } else {
    logMessage("no detection for %us, room vacant", settings.motionTimeout);
  }
}

// ==== status and log replies ====

// Returns names[index], or the fallback when the index is outside the table.
const char *lookupName(const char *const names[], uint8_t nameCount, uint8_t index, const char *fallback) {
  bool inTable = (index < nameCount);
  if (!inTable) {
    return fallback;
  }
  return names[index];
}

// Returns a readable name for a sensitivity (threshold group).
const char *sensitivityName(uint8_t thresholdGroup) {
  return lookupName(SENSITIVITY_NAMES, SENSITIVITY_NAME_COUNT, thresholdGroup, "read failed");
}

// Returns a readable name for a sensor indicator LED mode.
const char *ledModeName(eLedMode_t ledMode) {
  if (ledMode == eLedOn) {
    return "on";
  }
  if (ledMode == eLedOff) {
    return "off";
  }
  return "?";
}

// Appends the target distances and energies from the latest report, decoded with the given layout.
void appendRadarReadings(const RadarReportLayout &layout) {
  uint16_t presenceDistanceCm = readLittleEndianUint16(radarLastReportData, layout.presenceDistanceIndex);
  uint8_t presenceEnergy = radarLastReportData[layout.presenceEnergyIndex];
  uint16_t motionDistanceCm = readLittleEndianUint16(radarLastReportData, layout.motionDistanceIndex);
  uint8_t motionEnergy = radarLastReportData[layout.motionEnergyIndex];
  appendBodyField("presence target:", "%u cm, energy %u\n", presenceDistanceCm, presenceEnergy);
  appendBodyField("motion target:", "%u cm, energy %u\n", motionDistanceCm, motionEnergy);
}

// Appends the sensor's state, report counters, current target and latest readings.
void appendRadarSensorStatus(uint32_t nowMs) {
  const char *sensorDescription = "not answering yet, retrying";
  if (radarConfigured) {
    sensorDescription = "configured";
  }
  appendBodyField("sensor:", "%s\n", sensorDescription);
  unsigned long framesValid = radarFramesValid;
  unsigned long framesBadChecksum = radarFramesBadChecksum;
  if (radarHasReport) {
    uint32_t reportAgeMs = nowMs - radarLastReportMs;
    uint32_t ageWholeSeconds = reportAgeMs / MS_PER_SECOND;
    uint32_t ageRemainderMs = reportAgeMs % MS_PER_SECOND;
    uint32_t ageTenths = ageRemainderMs / MS_PER_TENTH_SECOND;
    appendBodyField("reports:", "%lu ok, %lu bad checksum, last %lu.%lus ago\n",
                    framesValid, framesBadChecksum, (unsigned long)ageWholeSeconds, (unsigned long)ageTenths);
  } else {
    appendBodyField("reports:", "%lu ok, %lu bad checksum, no detection report yet\n",
                    framesValid, framesBadChecksum);
  }
  const char *currentTargetName = lookupName(TARGET_NAMES, TARGET_NAME_COUNT, radarTargetState, "unknown");
  appendBodyField("target:", "%s (%u)\n", currentTargetName, radarTargetState);
  // the report layout is identified by its length
  bool packedLayout = (radarLastReportDataLength == RADAR_REPORT_PACKED_LENGTH);
  bool alignedLayout = (radarLastReportDataLength == RADAR_REPORT_ALIGNED_LENGTH);
  if (packedLayout) {
    appendRadarReadings(RADAR_REPORT_PACKED_LAYOUT);
  } else if (alignedLayout) {
    appendRadarReadings(RADAR_REPORT_ALIGNED_LAYOUT);
  } else if (radarHasReport) {
    appendBodyField("readings:", "unknown report layout (%u data bytes)\n", radarLastReportDataLength);
  }
}

// Appends room occupancy, with a countdown to vacant, and the light's levels.
void appendOccupancyStatus(uint32_t nowMs) {
  if (roomOccupied) {
    uint32_t msSinceDetection = nowMs - lastDetectionMs;
    uint32_t occupancyHoldMs = calculateOccupancyHoldMs();
    uint32_t secondsSinceDetection = msSinceDetection / MS_PER_SECOND;
    uint32_t secondsUntilVacant = 0;
    bool holdRemaining = (msSinceDetection < occupancyHoldMs);
    if (holdRemaining) {
      uint32_t msUntilVacant = occupancyHoldMs - msSinceDetection;
      secondsUntilVacant = msUntilVacant / MS_PER_SECOND;
    }
    appendBodyField("room:", "occupied, last detection %lus ago, vacant in %lus (+%us sensor hold)\n",
                    (unsigned long)secondsSinceDetection, (unsigned long)secondsUntilVacant, RADAR_DISAPPEAR_DELAY_S);
  } else {
    appendBodyField("room:", "vacant\n");
  }
  appendBodyField("pwmLED0:", "level %d, target %d, on level %d\n",
                  pwmLED0Level, pwmLED0TargetLevel, pwmLED0OnLevel);
}

// Appends the stored settings and whether an EEPROM write is pending.
void appendSettingsStatus(uint32_t nowMs) {
  appendBody("settings:\n");
  appendBodyField("  detect range:", "%u-%u cm\n", settings.radarRangeMin, settings.radarRangeMax);
  appendBodyField("  presenceHoldsLight:", "%u\n", settings.presenceHoldsLight);
  appendBodyField("  motionTimeout:", "%u s\n", settings.motionTimeout);
  const char *modeName = lightModeName(settings.lightMode);
  appendBodyField("  lightMode:", "%u (%s)\n", settings.lightMode, modeName);
  if (settingsSavePending) {
    uint32_t msSinceLastSave = nowMs - settingsLastSaveMs;
    uint32_t secondsUntilWrite = 0;
    bool intervalRemaining = (msSinceLastSave < SETTINGS_SAVE_INTERVAL_MS);
    if (intervalRemaining) {
      uint32_t msUntilWrite = SETTINGS_SAVE_INTERVAL_MS - msSinceLastSave;
      secondsUntilWrite = msUntilWrite / MS_PER_SECOND;
    }
    appendBodyField("  EEPROM:", "pending, writes in %lus\n", (unsigned long)secondsUntilWrite);
  } else {
    appendBodyField("  EEPROM:", "saved\n");
  }
}

// Appends the fixed configuration sent to the sensor at boot.
void appendFixedRadarConfiguration() {
  uint8_t reportPeriodWholeSeconds = RADAR_REPORT_PERIOD / TENTHS_PER_SECOND;
  uint8_t reportPeriodTenths = RADAR_REPORT_PERIOD % TENTHS_PER_SECOND;
  const char *motionSensitivityName = sensitivityName(RADAR_MOTION_SENSITIVITY);
  const char *presenceSensitivityName = sensitivityName(RADAR_PRESENCE_SENSITIVITY);
  appendBody("fixed configuration:\n");
  appendBodyField("  report period:", "%u.%u s\n", reportPeriodWholeSeconds, reportPeriodTenths);
  appendBodyField("  disappear delay:", "%u s\n", RADAR_DISAPPEAR_DELAY_S);
  appendBodyField("  motion sens.:", "%s\n", motionSensitivityName);
  appendBodyField("  presence sens.:", "%s", presenceSensitivityName);
}

// Appends what the sensor itself holds, read back with its reports paused so the replies
// aren't mixed with reports. Each setting is read on its own, which leaves the light
// sensor's threshold unread.
void appendSensorReadBack() {
  appendBody("\nread from C4002:\n");
  bool paused = configureRadarSetting(RADAR_SETTING_REPORTS_PAUSED);
  if (!paused) {
    appendBody("  sensor did not respond");
    return;
  }
  sDetectRange_t detectRange = radarSensor.getDetectRange();
  uint16_t disappearDelaySeconds = radarSensor.getTargetDisappearDelay();
  eThreshGroup_t motionSensitivity = radarSensor.getSensitivity(eMotionDistGate);
  eThreshGroup_t presenceSensitivity = radarSensor.getSensitivity(ePresenceDistGate);
  eResolutionMode_t resolutionMode = radarSensor.getResolutionMode();
  eOutpinMode_t outPinMode = radarSensor.getOutPinMode();
  sLedStatus_t ledStatus = radarSensor.getLedStatus();
  configureRadarSetting(RADAR_SETTING_REPORTS_RESUMED);
  const char *resolutionName = "80 cm gates";
  if (resolutionMode == eResolution20Cm) {
    resolutionName = "20 cm gates";
  }
  const char *motionSensitivityName = sensitivityName(motionSensitivity);
  const char *presenceSensitivityName = sensitivityName(presenceSensitivity);
  const char *runLedName = ledModeName(ledStatus.runLed);
  const char *outLedName = ledModeName(ledStatus.outLed);
  unsigned int outPinModeNumber = (unsigned int)outPinMode;
  appendBodyField("  detect range:", "%u-%u cm\n", detectRange.closest, detectRange.farthest);
  appendBodyField("  disappear delay:", "%u s\n", disappearDelaySeconds);
  appendBodyField("  motion sens.:", "%s\n", motionSensitivityName);
  appendBodyField("  presence sens.:", "%s\n", presenceSensitivityName);
  appendBodyField("  resolution:", "%s\n", resolutionName);
  appendBodyField("  OUT pin mode:", "%u\n", outPinModeNumber);
  appendBodyField("  LEDs:", "run %s, out %s", runLedName, outLedName);
}

// Replies with the radar's live status and settings; with readSensorConfiguration the
// sensor's stored configuration is read back too.
void writeRadarStatus(bool readSensorConfiguration) {
  startBodyReply();
  uint32_t nowMs = millis();
  appendRadarSensorStatus(nowMs);
  appendOccupancyStatus(nowMs);
  appendSettingsStatus(nowMs);
  appendFixedRadarConfiguration();
  if (readSensorConfiguration) {
    appendSensorReadBack();
  }
}

// Replies with the newest log entries, oldest first, one per line (0 = every stored entry).
void writeLog(uint16_t requestedEntries) {
  startBodyReply();
  uint8_t entriesToShow = logEntryCount;
  bool limitRequested = (requestedEntries != 0);
  bool limitWithinStored = (requestedEntries <= logEntryCount);
  if (limitRequested && limitWithinStored) {
    entriesToShow = (uint8_t)requestedEntries;
  }
  if (entriesToShow == 0) {
    appendBody("(log empty)");
    return;
  }
  // the oldest entry shown sits entriesToShow places behind the next write position
  uint16_t oldestShownOffset = LOG_CAPACITY - entriesToShow;
  for (uint8_t position = 0; position < entriesToShow; position++) {
    uint16_t unwrappedIndex = logNextIndex + oldestShownOffset + position;
    uint16_t bufferIndex = unwrappedIndex % LOG_CAPACITY;
    const LogEntry &entry = logBuffer[bufferIndex];
    char timestamp[TIMESTAMP_TEXT_SIZE];
    formatUptime(timestamp, sizeof(timestamp), entry.uptimeMs);
    uint16_t entryNumber = position + 1;
    bool isLastEntry = (entryNumber == entriesToShow);
    const char *lineEnding = "\n";
    if (isLastEntry) {
      lineEnding = "";
    }
    appendBody("%s  %s%s", timestamp, entry.text, lineEnding);
  }
}

// ==== commands ====

// Fade off command: fades the light off, except in always on mode which keeps it on. Replies
// with the level the light ends at.
void fadePwmLED0OffByCommand() {
  bool mayTurnOff = commandMayTurnLightOff();
  if (mayTurnOff) {
    startFadePwmLED0(0, PWM_LED0_FADE_STEP_MS);
    logMessage("http: light fade off");
  } else {
    logMessage("http: light stays on (always on mode)");
  }
  replyWithNumber(pwmLED0TargetLevel);
}

// Fade on command: fades the light on to full brightness when the light mode allows it (see
// commandMayTurnLightOn). Replies with the level the light ends at.
void fadePwmLED0OnByCommand() {
  bool mayTurnOn = commandMayTurnLightOn();
  if (mayTurnOn) {
    startFadePwmLED0(PWM_MAX_LEVEL, PWM_LED0_FADE_STEP_MS);
    logMessage("http: light fade on");
  } else {
    const char *reason = lightStaysOffReason();
    logMessage("http: light stays off (%s)", reason);
  }
  replyWithNumber(pwmLED0TargetLevel);
}

// Replies with the light's brightness (command 2, action 2): the level the light is on at or
// fading to while it is on, and the on level it will come on at next while it is off.
void replyWithBrightness() {
  bool lightOnOrFadingOn = (pwmLED0TargetLevel > 0);
  int16_t brightness = pwmLED0OnLevel;
  if (lightOnOrFadingOn) {
    brightness = pwmLED0TargetLevel;
  }
  replyWithNumber(brightness);
}

// Replies whether the light is on (command 2, action 4): 1 while it is on or fading on, 0 while
// it is off or fading off.
void replyWithOnState() {
  bool lightOnOrFadingOn = (pwmLED0TargetLevel > 0);
  int16_t onState = LIGHT_STATE_OFF;
  if (lightOnOrFadingOn) {
    onState = LIGHT_STATE_ON;
  }
  replyWithNumber(onState);
}

// Light controls (command 2): fade off, fade on, read the brightness, read the sensor's target
// state, read whether the light is on.
void runLightCommand(uint16_t action) {
  switch (action) {
    case LIGHT_ACTION_FADE_OFF:
      fadePwmLED0OffByCommand();
      break;
    case LIGHT_ACTION_FADE_ON:
      fadePwmLED0OnByCommand();
      break;
    case LIGHT_ACTION_READ_LEVEL:
      replyWithBrightness();
      break;
    case LIGHT_ACTION_READ_TARGET_STATE:
      replyWithNumber(radarTargetState);
      break;
    case LIGHT_ACTION_READ_ON_STATE:
      replyWithOnState();
      break;
    default:
      replyWithNumber(0);
      break;
  }
}

// Direct level control (command 3). A non-zero level becomes the on level, the level motion and
// always on fade in to. The light jumps to the level (cancelling any fade) when the light mode
// allows it; otherwise the light is left as it is:
//   - always off mode, or motion activated with the room empty and the light off: the level is
//     only kept for the next time the light comes on, and the reply is that kept level
//   - always on mode: a level of 0 is ignored, and the reply is the level the light stays at
void setPwmLED0Directly(uint16_t requestedLevel) {
  int16_t signedRequestedLevel = (int16_t)requestedLevel;
  int16_t level = clampPwmLevel(signedRequestedLevel);
  bool turnsLightOn = (level > 0);
  if (turnsLightOn) {
    pwmLED0OnLevel = level;
  }
  bool mayTurnOn = commandMayTurnLightOn();
  bool mayTurnOff = commandMayTurnLightOff();
  bool keptForNextTime = turnsLightOn && !mayTurnOn;
  bool keptOn = !turnsLightOn && !mayTurnOff;
  if (keptForNextTime) {
    const char *reason = lightStaysOffReason();
    logMessage("http: on level set to %d, light stays off (%s)", pwmLED0OnLevel, reason);
    replyWithNumber(pwmLED0OnLevel);
    return;
  }
  if (keptOn) {
    logMessage("http: light stays on (always on mode)");
    replyWithNumber(pwmLED0TargetLevel);
    return;
  }
  setPwmLED0(level);
  pwmLED0TargetLevel = pwmLED0Level;
  logMessage("http: light set to %d", pwmLED0Level);
  replyWithNumber(pwmLED0Level);
}

// Finishes an accepted settings change: schedules the EEPROM save, logs the change and replies
// with the new value.
void completeSettingChange(const char *settingName, uint16_t newValue, const char *unit) {
  markSettingsChanged();
  logMessage("http: %s set to %u%s", settingName, newValue, unit);
  int16_t replyValue = (int16_t)newValue;
  replyWithNumber(replyValue);
}

// Sets the nearest detection distance (command 4); it must stay below the farthest.
// Invalid values leave the reply empty, which returns 400 ERR.
void setRadarRangeMin(uint16_t requestedCm) {
  bool belowRangeMax = (requestedCm < settings.radarRangeMax);
  if (!belowRangeMax) {
    return;
  }
  settings.radarRangeMin = requestedCm;
  updateRadarRange();
  completeSettingChange("radarRangeMin", settings.radarRangeMin, "cm");
}

// Sets the farthest detection distance (command 5); it must stay above the nearest and
// within the sensor's limit.
void setRadarRangeMax(uint16_t requestedCm) {
  bool aboveRangeMin = (requestedCm > settings.radarRangeMin);
  bool withinSensorLimit = (requestedCm <= RADAR_RANGE_LIMIT_CM);
  bool valid = aboveRangeMin && withinSensorLimit;
  if (!valid) {
    return;
  }
  settings.radarRangeMax = requestedCm;
  updateRadarRange();
  completeSettingChange("radarRangeMax", settings.radarRangeMax, "cm");
}

// Sets whether static presence keeps the light on (command 6, 0 or 1).
void setPresenceHoldsLight(uint16_t requestedFlag) {
  bool valid = (requestedFlag <= FLAG_MAX_VALUE);
  if (!valid) {
    return;
  }
  settings.presenceHoldsLight = (uint8_t)requestedFlag;
  completeSettingChange("presenceHoldsLight", settings.presenceHoldsLight, "");
}

// Sets the motion timeout in seconds (command 7); it must be longer than the sensor's own hold.
void setMotionTimeout(uint16_t requestedSeconds) {
  bool longerThanSensorHold = (requestedSeconds > RADAR_DISAPPEAR_DELAY_S);
  bool withinLimit = (requestedSeconds <= MOTION_TIMEOUT_MAX_S);
  bool valid = longerThanSensorHold && withinLimit;
  if (!valid) {
    return;
  }
  settings.motionTimeout = requestedSeconds;
  completeSettingChange("motionTimeout", settings.motionTimeout, "s");
}

// Sets the light mode (command 13): 0 = always off, 1 = motion activated, 2 = always on. A new
// mode fades the light straight to what that mode calls for; other values are rejected (400 ERR).
void setLightMode(uint16_t requestedMode) {
  bool valid = (requestedMode <= LIGHT_MODE_ALWAYS_ON);
  if (!valid) {
    return;
  }
  bool modeChanged = (requestedMode != settings.lightMode);
  settings.lightMode = (uint8_t)requestedMode;
  if (modeChanged) {
    applyLightMode();
  }
  completeSettingChange("lightMode", settings.lightMode, "");
}

// Reads settings (command 8): 0 replies "min~max~presence~timeout~mode", a setting's command
// number (4-7 or 13) replies with that setting. Unknown selectors leave the reply empty (400 ERR).
void replyWithSettings(uint16_t selector) {
  switch (selector) {
    case READ_ALL_SETTINGS:
      snprintf(commandReplyOut, sizeof(commandReplyOut), "%d~%d~%d~%d~%d",
               settings.radarRangeMin, settings.radarRangeMax,
               settings.presenceHoldsLight, settings.motionTimeout, settings.lightMode);
      break;
    case COMMAND_SET_RANGE_MIN:
      replyWithNumber(settings.radarRangeMin);
      break;
    case COMMAND_SET_RANGE_MAX:
      replyWithNumber(settings.radarRangeMax);
      break;
    case COMMAND_SET_PRESENCE_HOLDS:
      replyWithNumber(settings.presenceHoldsLight);
      break;
    case COMMAND_SET_MOTION_TIMEOUT:
      replyWithNumber(settings.motionTimeout);
      break;
    case COMMAND_SET_LIGHT_MODE:
      replyWithNumber(settings.lightMode);
      break;
    default:
      break;
  }
}

// Resets every setting to its default (command 12, value 1 to confirm; anything else is
// rejected with 400 ERR). The sensor gets the default detect range and the light follows the
// default light mode straight away, the reset is saved to EEPROM like any other change, and the
// reply lists the restored settings.
void resetSettingsToDefaults(uint16_t confirmation) {
  bool confirmed = (confirmation == RESET_SETTINGS_CONFIRM);
  if (!confirmed) {
    return;
  }
  uint16_t previousLightMode = settings.lightMode;
  applyDefaultSettings(settings);
  updateRadarRange();
  bool lightModeChanged = (settings.lightMode != previousLightMode);
  if (lightModeChanged) {
    applyLightMode();
  }
  markSettingsChanged();
  logMessage("http: settings reset to defaults");
  replyWithSettings(READ_ALL_SETTINGS);
}

// Radar status (command 10): 0 = live status, 1 = also read the sensor back; others are invalid.
void runRadarStatusCommand(uint16_t mode) {
  bool validMode = (mode <= RADAR_STATUS_WITH_READ_BACK);
  if (!validMode) {
    return;
  }
  bool readSensorConfiguration = (mode == RADAR_STATUS_WITH_READ_BACK);
  writeRadarStatus(readSensorConfiguration);
}

// Runs a decoded command and blinks the on-board LED. The command numbers are kept from earlier
// versions so existing clients keep working.
void runCommand(uint16_t command, uint16_t value) {
  blinkActivityLed();
  switch (command) {
    case COMMAND_LIGHT:
      runLightCommand(value);
      break;
    case COMMAND_SET_LEVEL:
      setPwmLED0Directly(value);
      break;
    case COMMAND_SET_RANGE_MIN:
      setRadarRangeMin(value);
      break;
    case COMMAND_SET_RANGE_MAX:
      setRadarRangeMax(value);
      break;
    case COMMAND_SET_PRESENCE_HOLDS:
      setPresenceHoldsLight(value);
      break;
    case COMMAND_SET_MOTION_TIMEOUT:
      setMotionTimeout(value);
      break;
    case COMMAND_READ_SETTINGS:
      replyWithSettings(value);
      break;
    case COMMAND_READ_LOG:
      writeLog(value);
      break;
    case COMMAND_RADAR_STATUS:
      runRadarStatusCommand(value);
      break;
    case COMMAND_RESET_SETTINGS:
      resetSettingsToDefaults(value);
      break;
    case COMMAND_SET_LIGHT_MODE:
      setLightMode(value);
      break;
    default:
      replyWithNumber(0);
      break;
  }
}

// Returns the position of the given delimiter occurrence (1 = first) in the command message,
// or COMMAND_DELIMITER_NOT_FOUND when the message has fewer delimiters.
uint8_t findCommandDelimiter(uint8_t occurrence) {
  uint8_t delimitersSeen = 0;
  for (uint8_t position = 0; position < COMMAND_MESSAGE_LENGTH; position++) {
    char character = commandMessageIn[position];
    if (character == NULL_TERMINATOR) {
      break;
    }
    if (character == COMMAND_FIELD_DELIMITER) {
      delimitersSeen = delimitersSeen + 1;
      if (delimitersSeen == occurrence) {
        return position;
      }
    }
  }
  return COMMAND_DELIMITER_NOT_FOUND;
}

// Parses the number in the field starting at startPosition. The field ends at a delimiter,
// the end of the message or after COMMAND_FIELD_MAX_DIGITS characters.
uint16_t parseCommandField(uint8_t startPosition) {
  char fieldText[COMMAND_FIELD_MAX_DIGITS + 1];
  uint8_t fieldLength = 0;
  uint8_t readPosition = startPosition;
  while (fieldLength < COMMAND_FIELD_MAX_DIGITS) {
    // bounds guard: the message is always terminated before its buffer ends
    bool insideMessage = (readPosition < COMMAND_MESSAGE_LENGTH);
    if (!insideMessage) {
      break;
    }
    char character = commandMessageIn[readPosition];
    bool reachedDelimiter = (character == COMMAND_FIELD_DELIMITER);
    bool reachedEnd = (character == NULL_TERMINATOR);
    if (reachedDelimiter || reachedEnd) {
      break;
    }
    fieldText[fieldLength] = character;
    fieldLength = fieldLength + 1;
    readPosition = readPosition + 1;
  }
  fieldText[fieldLength] = NULL_TERMINATOR;
  int parsedNumber = atoi(fieldText);
  uint16_t fieldValue = (uint16_t)parsedNumber;
  return fieldValue;
}

// Decodes "<command>,<unused>,<value>" and runs it; messages with fewer than two delimiters
// are logged and ignored.
void decodeCommandMessage() {
  uint8_t valueDelimiterPosition = findCommandDelimiter(COMMAND_VALUE_DELIMITER_OCCURRENCE);
  bool enoughDelimiters = (valueDelimiterPosition != COMMAND_DELIMITER_NOT_FOUND);
  if (!enoughDelimiters) {
    logMessage("invalid command frame: %s", commandMessageIn);
    return;
  }
  uint16_t command = parseCommandField(COMMAND_FIRST_FIELD_START);
  uint8_t valueStartPosition = valueDelimiterPosition + 1;
  uint16_t value = parseCommandField(valueStartPosition);
  runCommand(command, value);
}

// Collects the characters between '?' and '~' and decodes the message at the closing '~'.
// Characters beyond the message buffer are dropped.
void processCommandCharacter(char receivedCharacter) {
  if (!commandFrameReading) {
    bool frameStarts = (receivedCharacter == COMMAND_FRAME_START);
    if (frameStarts) {
      commandFrameReading = true;
      commandFramePosition = 0;
    }
    return;
  }
  bool frameEnds = (receivedCharacter == COMMAND_FRAME_END);
  if (frameEnds) {
    commandMessageIn[commandFramePosition] = NULL_TERMINATOR;
    commandFrameReading = false;
    decodeCommandMessage();
    return;
  }
  bool isFrameStart = (receivedCharacter == COMMAND_FRAME_START);
  uint16_t lastStorablePosition = COMMAND_MESSAGE_LENGTH - 1;
  bool hasSpace = (commandFramePosition < lastStorablePosition);
  if (!isFrameStart && hasSpace) {
    commandMessageIn[commandFramePosition] = receivedCharacter;
    commandFramePosition = commandFramePosition + 1;
  }
}

// ==== HTTP server ====

// Writes a complete HTTP response with a plain-text body followed by a newline.
void sendHttpResponse(WiFiClient &client, const char *status, const char *body) {
  size_t bodyLength = strlen(body);
  size_t contentLength = bodyLength + HTTP_LINE_ENDING_LENGTH;
  client.print("HTTP/1.1 ");
  client.println(status);
  client.println("Content-Type: text/plain");
  client.print("Content-Length: ");
  client.println(contentLength);
  client.println("Connection: close");
  client.println();
  client.print(body);
  client.print(NEW_LINE);
  client.flush(); // wait for the response to leave before the connection closes
}

// Decodes the command in the request line and replies. Every request starts with a clean
// decoder and an empty reply, so a half-received frame or an old result never leaks into it.
// Only the request line is parsed: headers such as Referer may also contain '?'.
void handleHttpRequest(WiFiClient &client, uint8_t requestLength) {
  commandFrameReading = false;
  commandReplyOut[0] = NULL_TERMINATOR;
  httpReplyUsesBody = false;
  for (uint8_t position = 0; position < requestLength; position++) {
    char character = httpRequestBuffer[position];
    bool endOfRequestLine = (character == CARRIAGE_RETURN) || (character == NEW_LINE) || (character == NULL_TERMINATOR);
    if (endOfRequestLine) {
      break;
    }
    processCommandCharacter(character);
  }
  bool noValidCommand = (commandReplyOut[0] == NULL_TERMINATOR);
  if (httpReplyUsesBody) {
    sendHttpResponse(client, "200 OK", httpBodyBuffer);
  } else if (noValidCommand) {
    sendHttpResponse(client, "400 Bad Request", "ERR");
  } else {
    sendHttpResponse(client, "200 OK", commandReplyOut);
  }
}

// Reads the request headers into httpRequestBuffer until the blank line that ends them;
// characters beyond the buffer are dropped. Returns true when a complete request arrived,
// false when the client disconnected or stalled for HTTP_CLIENT_TIMEOUT_MS.
bool readHttpRequest(WiFiClient &client, uint8_t &requestLength) {
  requestLength = 0;
  bool lineIsBlank = true;
  uint32_t lastActivityMs = millis();
  while (client.connected()) {
    uint32_t msSinceActivity = millis() - lastActivityMs;
    bool timedOut = (msSinceActivity > HTTP_CLIENT_TIMEOUT_MS);
    if (timedOut) {
      return false;
    }
    if (!client.available()) {
      continue;
    }
    int receivedValue = client.read();
    char receivedCharacter = (char)receivedValue;
    lastActivityMs = millis();
    uint8_t lastStorablePosition = HTTP_REQUEST_BUFFER_SIZE - 1;
    bool bufferHasSpace = (requestLength < lastStorablePosition);
    if (bufferHasSpace) {
      httpRequestBuffer[requestLength] = receivedCharacter;
      requestLength = requestLength + 1;
    }
    // the headers end with a blank line (\r\n followed by another \r\n)
    bool isNewLine = (receivedCharacter == NEW_LINE);
    if (isNewLine && lineIsBlank) {
      return true;
    }
    if (isNewLine) {
      lineIsBlank = true;
    } else if (receivedCharacter != CARRIAGE_RETURN) {
      lineIsBlank = false;
    }
  }
  return false;
}

// Serves one waiting HTTP client, if any: reads its request, replies, and closes it.
void serviceHttpClient() {
  WiFiClient client = httpServer.accept();
  if (!client) {
    return;
  }
  client.setNoDelay(true); // disable Nagle's algorithm
  uint8_t requestLength = 0;
  bool requestComplete = readHttpRequest(client, requestLength);
  if (requestComplete) {
    handleHttpRequest(client, requestLength);
  }
  client.stop();
}

// ==== main entry points ====

// Steadies the 3.3V regulator, sets up the light output, restores settings, configures the
// sensor and starts networking.
void setup() {
  forceRegulatorPwmMode();
  // initialise 12-bit PWM
  pinMode(PWM_LED0_POWER_PIN, OUTPUT);
  analogWriteFreq(PWM_FREQUENCY_HZ);
  analogWriteRange(PWM_DUTY_MAX);
  digitalWrite(PWM_LED0_POWER_PIN, LOW);
  // the on-board LED stays lit while starting up
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  logMessage("BathroomLight started");
  loadSettings();
  applyLightMode(); // always on mode fades the light in, the others start dark
  initRadar();
  connectWiFi();
  delay(STARTUP_SETTLE_MS);
  httpServer.begin();
  digitalWrite(LED_BUILTIN, LOW);
}

// Superloop: every task is non-blocking apart from short sensor exchanges.
void loop() {
  logWiFiStateChanges();
  reconnectWiFiWhenDropped();
  retryRadarWhenDue();
  readRadar();
  updateOccupancy();
  updatePwmLED0Fade();
  updateActivityLed();
  serviceHttpClient();
  saveSettingsWhenDue();
}
