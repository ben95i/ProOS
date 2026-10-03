// minimotion.cpp
// A small, low-overhead replacement for Motion on a Raspberry Pi Zero 2 W
// (Raspberry Pi OS Trixie, 32-bit).
//
// Watches one or more H.264 RTSP cameras. Each camera runs on its own thread
// with its own settings file. Motion is detected on a tiny grayscale copy of
// the picture, and each event is recorded to an MP4 named after its start
// time, in one sub-folder per day, e.g. 2025-03-05/2025-03-05_19.00.26.mp4.
// Finished movies can be copied to several destinations (e.g. SMB mounts);
// if a destination cannot be written, an optional fallback folder gets a copy.
//
// Configuration (like Motion):
//   /opt/rpi/minimotion.conf       global settings, defaults for every camera
//                                  and one "camera = <file>" line per camera
//   /opt/rpi/camera1.conf          one file per camera; its settings override
//                                  the defaults from minimotion.conf
//   Without camera lines, minimotion.conf alone describes a single camera.
//
// Components (each has one job):
//
//   loadAllConfiguration  global file + camera files -> one CameraConfig each
//   CameraRunner          one thread per camera: connect, record, reconnect
//   StreamSource          opens the RTSP stream and reads compressed packets
//   StreamClock           turns packet timestamps into clean stream seconds
//   H264Decoder           hardware (h264_v4l2m2m) decoding, software fallback
//   MotionAnalyzer        picks which decoded frames to analyse (detect_fps)
//   MotionDetector        the detection algorithm on a ~160 px luma image
//   PreCaptureBuffer      the last seconds of compressed packets
//   EventRecorder         event state machine: start, split, end, retry
//   MovieWriter           interface for writing one movie file, with
//     CopyMovieWriter       remuxes the camera's H.264 untouched (default)
//     ReencodeMovieWriter   decodes and re-encodes on the hardware encoder
//   Mp4Muxer              MP4 file output shared by both writers
//   MovieCopyService      copies finished movies to every destination,
//                         applies the fallback, runs on_movie_end
//   DestinationWorker     one thread per destination, so a hung SMB share
//                         only delays copies to that share
//
// Install build dependencies:
//   sudo apt install g++ pkg-config libavformat-dev libavcodec-dev libavutil-dev
//
// Build (one line):
//   g++ -std=c++17 -O2 -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -s -o minimotion minimotion.cpp $(pkg-config --cflags --libs libavformat libavcodec libavutil) -latomic -pthread
//
// Run:
//   minimotion [-c /opt/rpi/minimotion.conf] [-v]
//     -c <file>  global configuration file (default /opt/rpi/minimotion.conf)
//     -v         debug logging (prints the changed-area % of every analysed
//                frame, useful for tuning threshold_percent and noise_level)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
}

extern char **environ;

// ============================================================================
// Constants
// ============================================================================

// ---- Program --------------------------------------------------------------
static constexpr const char *DEFAULT_CONFIG_PATH = "/opt/rpi/minimotion.conf";
static constexpr const char *LOG_PREFIX = "minimotion";
static constexpr const char *DEFAULT_CAMERA_NAME = "camera";
static constexpr const char *COPY_COORDINATOR_LOG_CONTEXT = "copy";
static constexpr const char *DESTINATION_LOG_CONTEXT_PREFIX = "copy ";

// ---- Files ----------------------------------------------------------------
static constexpr const char *MOVIE_NAME_FORMAT = "%Y-%m-%d_%H.%M.%S";
static constexpr const char *DAY_FOLDER_FORMAT = "%Y-%m-%d";
static constexpr const char *MOVIE_FILE_EXTENSION = ".mp4";
static constexpr const char *MOVIE_CONTAINER_NAME = "mp4";
static constexpr const char *FRAGMENTED_MP4_FLAGS = "+frag_keyframe+empty_moov+default_base_moof";
static constexpr const char *PARTIAL_COPY_SUFFIX = ".part";
static constexpr int DISPLAY_MATRIX_ELEMENTS = 9;           // 3x3 matrix, as MP4 stores it
static constexpr int TIME_TEXT_BUFFER_SIZE = 32;
static constexpr int MAX_MOVIE_NAME_ATTEMPTS = 100;
static constexpr mode_t DIRECTORY_PERMISSIONS = 0755;
static constexpr mode_t MOVIE_FILE_PERMISSIONS = 0644;
static constexpr const char *SHELL_PATH = "/bin/sh";
static constexpr const char *FILE_PLACEHOLDER = "%f";
static constexpr size_t COPY_BUFFER_BYTES = 64u * 1024u;
static constexpr int CONFIG_LINE_BUFFER_SIZE = 2048;

// ---- Copying --------------------------------------------------------------
static constexpr int COPY_SHUTDOWN_GRACE_SECONDS = 20;
static constexpr int COORDINATOR_POLL_MILLISECONDS = 1000;
static constexpr int64_t WORKER_IDLE_MARKER = -1;
static constexpr int64_t NO_DEADLINE_MARKER = -1;
static constexpr int64_t MILLISECONDS_PER_SECOND = 1000;

// ---- Source ---------------------------------------------------------------
static constexpr const char *PROBE_SIZE_BYTES = "1000000";
static constexpr const char *ANALYZE_DURATION_MICROSECONDS = "2000000";
static constexpr int OPEN_TIMEOUT_MULTIPLIER = 2;
static constexpr double FALLBACK_FRAMES_PER_SECOND = 15.0;
static constexpr double MIN_PLAUSIBLE_FRAMES_PER_SECOND = 1.0;
static constexpr double MAX_PLAUSIBLE_FRAMES_PER_SECOND = 120.0;
static constexpr int MAX_RECONNECT_DELAY_SECONDS = 30;
static constexpr int RECONNECT_PAUSE_SECONDS = 1;

// ---- Codecs ---------------------------------------------------------------
static constexpr const char *HARDWARE_CODEC_NAME = "h264_v4l2m2m";
static constexpr const char *HARDWARE_DECODER_CAPTURE_BUFFERS = "8";
static constexpr const char *HARDWARE_DECODER_OUTPUT_BUFFERS = "4";
static constexpr const char *HARDWARE_ENCODER_OUTPUT_BUFFERS = "6";
static constexpr const char *HARDWARE_ENCODER_CAPTURE_BUFFERS = "4";
static constexpr int HARDWARE_STALL_PACKET_LIMIT = 90;
static constexpr int BACKPRESSURE_RETRY_LIMIT = 50;
static constexpr int BACKPRESSURE_SLEEP_MICROSECONDS = 2000;
static constexpr double DEFAULT_KEYFRAME_INTERVAL_SECONDS = 2.0;
static constexpr int FRAME_RATE_MAX_DENOMINATOR = 1001000;
static constexpr int PROBE_ENCODER_WIDTH = 640;
static constexpr int PROBE_ENCODER_HEIGHT = 480;
static constexpr int PROBE_ENCODER_FRAMES_PER_SECOND = 30;
static constexpr int64_t PROBE_ENCODER_BITRATE = 1000000;

// ---- H.264 bitstream ------------------------------------------------------
static constexpr int NAL_TYPE_MASK = 0x1f;
static constexpr int NAL_TYPE_NON_IDR_SLICE = 1;
static constexpr int NAL_TYPE_IDR_SLICE = 5;
static constexpr int NAL_TYPE_SEQUENCE_PARAMETER_SET = 7;
static constexpr int NAL_TYPE_PICTURE_PARAMETER_SET = 8;
static constexpr int SHORT_START_CODE_LENGTH = 3;
static constexpr uint8_t LONG_START_CODE[] = {0, 0, 0, 1};

// ---- Timing ---------------------------------------------------------------
static constexpr double MAX_TIMESTAMP_GAP_SECONDS = 5.0;
static constexpr int FALLBACK_TICKS_PER_SECOND = 90000;
static constexpr int64_t NO_OUTPUT_TIMESTAMP_YET = -1;
static constexpr int64_t MICROSECONDS_PER_SECOND = 1000000;
static constexpr int SLEEP_SLICE_MICROSECONDS = 100000;
static constexpr int SLEEP_SLICES_PER_SECOND = 10;
static constexpr double RECORDING_RETRY_DELAY_SECONDS = 10.0;

// ---- Pre-capture ----------------------------------------------------------
static constexpr size_t MAX_PRE_CAPTURE_BYTES = 16u * 1024u * 1024u;

// ---- Motion detection -----------------------------------------------------
static constexpr int BACKGROUND_FRACTION_BITS = 8;       // background is 8.8 fixed point
static constexpr int RECIPROCAL_FRACTION_BITS = 16;      // 1/sampleCount as 16.16 fixed point
static constexpr uint32_t RECIPROCAL_ONE = 1u << RECIPROCAL_FRACTION_BITS;
static constexpr uint32_t MAX_LUMA_VALUE = 255;
static constexpr int MIN_CHANGED_NEIGHBOURS = 2;
static constexpr int MIN_ANALYSIS_DIMENSION = 3;
static constexpr int ROW_SKIPPING_MIN_BLOCK_SIZE = 4;
static constexpr int ROW_STEP_SMALL_BLOCKS = 1;
static constexpr int ROW_STEP_LARGE_BLOCKS = 2;
static constexpr double FAST_BACKGROUND_SECONDS = 2.0;   // idle pixels and ghosts
static constexpr double SLOW_BACKGROUND_SECONDS = 10.0;  // pixels that are moving
static constexpr double SETTLE_SECONDS = 2.0;
static constexpr int MIN_SETTLE_FRAMES = 3;
static constexpr int SETTLE_BLEND_SHIFT = 1;
static constexpr int MIN_BLEND_SHIFT = 1;
static constexpr int MAX_BLEND_SHIFT = 10;
static constexpr double ANALYSIS_TIME_TOLERANCE_SECONDS = 0.001;
static constexpr double PERCENT_SCALE = 100.0;

// ---- Number parsing -------------------------------------------------------
static constexpr double THOUSAND = 1e3;
static constexpr double MILLION = 1e6;

// ============================================================================
// Process-wide state
// ============================================================================

// Set by the SIGINT/SIGTERM handler; read by every camera thread and by
// FFmpeg's interrupt callback, which can run on an FFmpeg worker thread. It
// must be a lock-free atomic to be safe inside a signal handler and across threads.
static std::atomic<bool> stopRequested{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the stop flag must be lock-free to be signal-safe");

enum class LogLevel : int { ERROR = 0, WARNING = 1, INFO = 2, DEBUG = 3 };

// Highest level that is printed. Written once in main() before any thread
// exists and only read afterwards, so it needs no synchronisation.
static int activeLogLevel = static_cast<int>(LogLevel::INFO);

// Name shown in this thread's log lines (a camera name, or "copy <folder>").
// thread_local: every thread has its own copy, so no locking is needed.
static thread_local std::string logContext;

// ============================================================================
// Logging and error text
// ============================================================================

// Sets the name shown in log lines written by the calling thread.
static void setLogContext(const std::string &contextName) {
    logContext = contextName;
}

// Prints one line to stderr ("minimotion[camera] <level>: <message>") when
// `level` is enabled. The stream is locked for the whole line so lines from
// different threads never interleave. systemd's journal adds the timestamp.
__attribute__((format(printf, 2, 3)))
static void logMessage(LogLevel level, const char *format, ...) {
    const int levelValue = static_cast<int>(level);
    if (levelValue > activeLogLevel) {
        return;
    }
    static const char *const LEVEL_NAMES[] = {"error", "warn", "info", "debug"};
    const char *levelName = LEVEL_NAMES[levelValue];

    va_list arguments;
    va_start(arguments, format);
    flockfile(stderr);
    const bool hasContext = !logContext.empty();
    if (hasContext) {
        fprintf(stderr, "%s[%s] %s: ", LOG_PREFIX, logContext.c_str(), levelName);
    } else {
        fprintf(stderr, "%s %s: ", LOG_PREFIX, levelName);
    }
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(arguments);
}

// Returns readable text for an FFmpeg error code. av_strerror() is
// thread-safe, unlike strerror().
static std::string describeError(int errorCode) {
    char messageBuffer[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(errorCode, messageBuffer, sizeof messageBuffer);
    return std::string(messageBuffer);
}

// Returns readable text for a POSIX errno value (thread-safe).
static std::string describeErrno(int errnoValue) {
    const int ffmpegErrorCode = AVERROR(errnoValue);
    return describeError(ffmpegErrorCode);
}

// Milliseconds on the monotonic clock (unaffected by changes to the wall clock).
static int64_t monotonicMilliseconds() {
    const auto sinceStart = std::chrono::steady_clock::now().time_since_epoch();
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(sinceStart);
    return static_cast<int64_t>(milliseconds.count());
}

// ============================================================================
// Configuration
// ============================================================================

enum class RecordMode { COPY, ENCODE };

// Where the video comes from.
struct SourceSettings {
    std::string url = "rtsp://127.0.0.1:8554/cam";
    std::string transport = "tcp";
    int ioTimeoutSeconds = 10;
};

// How motion is detected.
struct DetectionSettings {
    double thresholdPercent = 1.0;
    int noiseLevel = 16;
    double lightswitchPercent = 40.0;
    int minimumMotionFrames = 2;
    int analysisLongSide = 160;
    double analysisFramesPerSecond = 5.0;
};

// When events start, split and end, where movies go, what runs afterwards.
struct EventSettings {
    std::string temporaryDirectory = "/mnt/ramdisk/minimotion";   // movies are recorded here, then copied
    double preCaptureSeconds = 2.0;
    double eventGapSeconds = 5.0;
    double movieMaxSeconds = 120.0;
    std::string onMovieEndCommand;
};

// How each movie file is written.
struct MovieSettings {
    RecordMode recordMode = RecordMode::COPY;
    long long bitrate = 2000000;
    int keyframeIntervalFrames = 0;
    bool fragmented = true;
    int rotationDegrees = 0;         // clockwise display rotation: 0, 90, 180 or 270
};

// How H.264 is decoded.
struct DecoderSettings {
    bool useHardware = true;
    int softwareThreads = 2;
    bool skipDeblocking = false;
};

// Where finished movies are copied, and what happens when a copy fails.
struct CopySettings {
    std::vector<std::string> destinations;
    std::string fallbackDirectory;   // empty = fallback disabled
    int timeoutSeconds = 120;        // a copy taking longer counts as failed
};

// Everything one camera needs.
struct CameraConfig {
    std::string name = DEFAULT_CAMERA_NAME;
    std::string configPath;
    SourceSettings source;
    DetectionSettings detection;
    EventSettings events;
    MovieSettings movie;
    DecoderSettings decoder;
    CopySettings copy;
};

// Settings that exist once per process (only allowed in minimotion.conf).
struct GlobalSettings {
    std::vector<std::string> cameraConfigPaths;
    int logLevel = static_cast<int>(LogLevel::INFO);
};

// What a configuration file is applied to. `global` is null while a camera
// file is read, because camera files may not contain global-only settings.
struct ConfigurationTarget {
    GlobalSettings *global = nullptr;
    CameraConfig *camera = nullptr;
};

// Whether a setting may appear in camera files or only in minimotion.conf.
enum class OptionScope { GLOBAL_ONLY, ANY_FILE };

// Returns `text` without leading and trailing spaces, tabs and line breaks.
static std::string trimWhitespace(const std::string &text) {
    const char *whitespaceCharacters = " \t\r\n";
    const size_t firstContent = text.find_first_not_of(whitespaceCharacters);
    if (firstContent == std::string::npos) {
        return std::string();
    }
    const size_t lastContent = text.find_last_not_of(whitespaceCharacters);
    const size_t contentLength = lastContent - firstContent + 1;
    return text.substr(firstContent, contentLength);
}

// Removes trailing slashes from a directory path ("/mnt/nas/" -> "/mnt/nas"),
// keeping "/" itself, so the same folder is always written the same way.
static std::string normalizeDirectoryPath(const std::string &path) {
    std::string normalizedPath = path;
    while (normalizedPath.size() > 1 && normalizedPath.back() == '/') {
        normalizedPath.pop_back();
    }
    return normalizedPath;
}

// Parses a number with an optional k or M suffix ("2M" = 2000000), checks it
// lies within [minimum, maximum] and stores it in `target` (rounded for
// integer targets). Returns false and leaves `target` alone if invalid.
template <class Value>
static bool parseNumber(const std::string &text, Value &target, double minimum, double maximum) {
    const char *textStart = text.c_str();
    char *parseEnd = nullptr;
    const double parsedValue = strtod(textStart, &parseEnd);
    const bool nothingParsed = (parseEnd == textStart);
    if (nothingParsed) {
        return false;
    }

    const std::string suffix = trimWhitespace(parseEnd);
    double multiplier = 1.0;
    if (suffix == "k" || suffix == "K") {
        multiplier = THOUSAND;
    } else if (suffix == "M") {
        multiplier = MILLION;
    } else if (!suffix.empty()) {
        return false;
    }

    const double scaledValue = parsedValue * multiplier;
    const bool isNotANumber = std::isnan(scaledValue);
    const bool belowMinimum = (scaledValue < minimum);
    const bool aboveMaximum = (scaledValue > maximum);
    if (isNotANumber || belowMinimum || aboveMaximum) {
        return false;
    }

    if constexpr (std::is_integral<Value>::value) {
        const long long roundedValue = llround(scaledValue);
        target = static_cast<Value>(roundedValue);
    } else {
        target = static_cast<Value>(scaledValue);
    }
    return true;
}

// Parses on/off, yes/no, true/false or 1/0 into `target`.
static bool parseBoolean(const std::string &text, bool &target) {
    const bool isTrueWord = (text == "1" || text == "on" || text == "yes" || text == "true");
    const bool isFalseWord = (text == "0" || text == "off" || text == "no" || text == "false");
    if (isTrueWord) {
        target = true;
        return true;
    }
    if (isFalseWord) {
        target = false;
        return true;
    }
    return false;
}

// Validates a value and stores it in the target; false if the value is invalid.
using OptionParser = std::function<bool(ConfigurationTarget &, const std::string &)>;
// Empties a list setting (used before a file's first line for that list).
using OptionClearer = std::function<void(ConfigurationTarget &)>;

// One configuration-file key, where it may appear, and how its value is applied.
// List options may repeat; the first occurrence in a file replaces the list
// inherited from minimotion.conf instead of adding to it.
struct OptionDefinition {
    std::string key;
    OptionScope scope = OptionScope::ANY_FILE;
    OptionParser parse;
    OptionClearer clearList;   // empty function for single-value options
};

// Builds the definition of a numeric camera option stored in `group.field`.
template <class Group, class Value>
static OptionDefinition makeNumberOption(const char *key, Group CameraConfig::*group, Value Group::*field,
                                         double minimum, double maximum) {
    OptionParser parser = [group, field, minimum, maximum](ConfigurationTarget &target, const std::string &text) {
        Group &settingsGroup = target.camera->*group;
        Value &value = settingsGroup.*field;
        return parseNumber(text, value, minimum, maximum);
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of an on/off camera option stored in `group.field`.
template <class Group>
static OptionDefinition makeBooleanOption(const char *key, Group CameraConfig::*group, bool Group::*field) {
    OptionParser parser = [group, field](ConfigurationTarget &target, const std::string &text) {
        Group &settingsGroup = target.camera->*group;
        bool &value = settingsGroup.*field;
        return parseBoolean(text, value);
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of a text camera option stored in `group.field`. If
// `allowedValues` is not empty the value must be one of them.
template <class Group>
static OptionDefinition makeTextOption(const char *key, Group CameraConfig::*group, std::string Group::*field,
                                       std::vector<std::string> allowedValues) {
    OptionParser parser = [group, field, allowedValues](ConfigurationTarget &target, const std::string &text) {
        if (!allowedValues.empty()) {
            const auto match = std::find(allowedValues.begin(), allowedValues.end(), text);
            const bool isAllowed = (match != allowedValues.end());
            if (!isAllowed) {
                return false;
            }
        }
        Group &settingsGroup = target.camera->*group;
        std::string &value = settingsGroup.*field;
        value = text;
        return true;
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of a directory camera option (trailing slashes removed).
template <class Group>
static OptionDefinition makeDirectoryOption(const char *key, Group CameraConfig::*group, std::string Group::*field) {
    OptionParser parser = [group, field](ConfigurationTarget &target, const std::string &text) {
        Group &settingsGroup = target.camera->*group;
        std::string &value = settingsGroup.*field;
        value = normalizeDirectoryPath(text);
        return true;
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of a repeatable directory option collected into a list.
template <class Group>
static OptionDefinition makeDirectoryListOption(const char *key, Group CameraConfig::*group,
                                                std::vector<std::string> Group::*field) {
    OptionParser parser = [group, field](ConfigurationTarget &target, const std::string &text) {
        if (text.empty()) {
            return false;
        }
        Group &settingsGroup = target.camera->*group;
        std::vector<std::string> &values = settingsGroup.*field;
        const std::string normalizedPath = normalizeDirectoryPath(text);
        const auto existing = std::find(values.begin(), values.end(), normalizedPath);
        const bool alreadyListed = (existing != values.end());
        if (!alreadyListed) {
            values.push_back(normalizedPath);
        }
        return true;
    };
    OptionClearer clearer = [group, field](ConfigurationTarget &target) {
        Group &settingsGroup = target.camera->*group;
        std::vector<std::string> &values = settingsGroup.*field;
        values.clear();
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, clearer};
}

// Builds the definition of the record_mode option (copy | encode).
static OptionDefinition makeRecordModeOption(const char *key) {
    OptionParser parser = [](ConfigurationTarget &target, const std::string &text) {
        if (text == "copy") {
            target.camera->movie.recordMode = RecordMode::COPY;
            return true;
        }
        if (text == "encode") {
            target.camera->movie.recordMode = RecordMode::ENCODE;
            return true;
        }
        return false;
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of the rotation option: 0, 90, 180 or 270 degrees clockwise.
static OptionDefinition makeRotationOption(const char *key) {
    OptionParser parser = [](ConfigurationTarget &target, const std::string &text) {
        int parsedDegrees = 0;
        const bool isNumber = parseNumber(text, parsedDegrees, 0, 270);
        if (!isNumber) {
            return false;
        }
        const int remainder = parsedDegrees % 90;
        const bool isQuarterTurn = (remainder == 0);
        if (!isQuarterTurn) {
            return false;
        }
        target.camera->movie.rotationDegrees = parsedDegrees;
        return true;
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of camera_name (used in log lines).
static OptionDefinition makeCameraNameOption(const char *key) {
    OptionParser parser = [](ConfigurationTarget &target, const std::string &text) {
        if (text.empty()) {
            return false;
        }
        target.camera->name = text;
        return true;
    };
    return OptionDefinition{key, OptionScope::ANY_FILE, parser, OptionClearer()};
}

// Builds the definition of the global-only `camera = <file>` option.
static OptionDefinition makeCameraFileOption(const char *key) {
    OptionParser parser = [](ConfigurationTarget &target, const std::string &text) {
        if (text.empty()) {
            return false;
        }
        target.global->cameraConfigPaths.push_back(text);
        return true;
    };
    return OptionDefinition{key, OptionScope::GLOBAL_ONLY, parser, OptionClearer()};
}

// Builds the definition of the global-only log_level option.
static OptionDefinition makeLogLevelOption(const char *key) {
    OptionParser parser = [](ConfigurationTarget &target, const std::string &text) {
        return parseNumber(text, target.global->logLevel, 0, 3);
    };
    return OptionDefinition{key, OptionScope::GLOBAL_ONLY, parser, OptionClearer()};
}

// Every configuration-file key, its storage location and its valid range.
static const std::vector<OptionDefinition> &optionDefinitions() {
    static const std::vector<OptionDefinition> OPTION_DEFINITIONS = {
        makeCameraFileOption("camera"),
        makeLogLevelOption("log_level"),
        makeCameraNameOption("camera_name"),

        makeTextOption("rtsp_url", &CameraConfig::source, &SourceSettings::url, {}),
        makeTextOption("rtsp_transport", &CameraConfig::source, &SourceSettings::transport, {"tcp", "udp"}),
        makeNumberOption("io_timeout", &CameraConfig::source, &SourceSettings::ioTimeoutSeconds, 2, 300),

        makeNumberOption("threshold_percent", &CameraConfig::detection, &DetectionSettings::thresholdPercent, 0.01, 100),
        makeNumberOption("noise_level", &CameraConfig::detection, &DetectionSettings::noiseLevel, 1, 255),
        makeNumberOption("lightswitch_percent", &CameraConfig::detection, &DetectionSettings::lightswitchPercent, 0, 100),
        makeNumberOption("minimum_motion_frames", &CameraConfig::detection, &DetectionSettings::minimumMotionFrames, 1, 1000),
        makeNumberOption("detect_size", &CameraConfig::detection, &DetectionSettings::analysisLongSide, 32, 1920),
        makeNumberOption("detect_fps", &CameraConfig::detection, &DetectionSettings::analysisFramesPerSecond, 0, 120),

        makeDirectoryOption("temp_dir", &CameraConfig::events, &EventSettings::temporaryDirectory),
        makeNumberOption("pre_capture_time", &CameraConfig::events, &EventSettings::preCaptureSeconds, 0, 60),
        makeNumberOption("event_gap", &CameraConfig::events, &EventSettings::eventGapSeconds, 0, 3600),
        makeNumberOption("movie_max_time", &CameraConfig::events, &EventSettings::movieMaxSeconds, 0, 86400),
        makeTextOption("on_movie_end", &CameraConfig::events, &EventSettings::onMovieEndCommand, {}),

        makeRecordModeOption("record_mode"),
        makeNumberOption("movie_bps", &CameraConfig::movie, &MovieSettings::bitrate, 100000, 25000000),
        makeNumberOption("movie_gop", &CameraConfig::movie, &MovieSettings::keyframeIntervalFrames, 0, 1000),
        makeBooleanOption("mp4_fragmented", &CameraConfig::movie, &MovieSettings::fragmented),
        makeRotationOption("rotation"),

        makeBooleanOption("hw_decode", &CameraConfig::decoder, &DecoderSettings::useHardware),
        makeNumberOption("decoder_threads", &CameraConfig::decoder, &DecoderSettings::softwareThreads, 1, 4),
        makeBooleanOption("fast_decode", &CameraConfig::decoder, &DecoderSettings::skipDeblocking),

        makeDirectoryListOption("copy_to", &CameraConfig::copy, &CopySettings::destinations),
        makeDirectoryOption("fallback_dir", &CameraConfig::copy, &CopySettings::fallbackDirectory),
        makeNumberOption("copy_timeout", &CameraConfig::copy, &CopySettings::timeoutSeconds, 5, 3600),
    };
    return OPTION_DEFINITIONS;
}

// Finds the definition for `key`, or nullptr if there is none.
static const OptionDefinition *findOptionDefinition(const std::string &key) {
    for (const OptionDefinition &definition : optionDefinitions()) {
        if (definition.key == key) {
            return &definition;
        }
    }
    return nullptr;
}

// Removes one pair of surrounding double quotes, if present.
static std::string removeSurroundingQuotes(const std::string &text) {
    const bool longEnough = (text.size() >= 2);
    if (!longEnough) {
        return text;
    }
    const bool quoted = (text.front() == '"' && text.back() == '"');
    if (!quoted) {
        return text;
    }
    const size_t innerLength = text.size() - 2;
    return text.substr(1, innerLength);
}

// Splits a settings line into key and value. Accepts "key = value",
// "key=value" and Motion's "key value". The key ends at the first space, tab
// or '=', so '=' characters inside the value (e.g. in a URL) are kept.
static bool splitSettingLine(const std::string &line, std::string &key, std::string &value) {
    const size_t keyEnd = line.find_first_of(" \t=");
    const bool hasSeparator = (keyEnd != std::string::npos);
    if (!hasSeparator) {
        return false;
    }
    key = line.substr(0, keyEnd);
    std::string remainder = trimWhitespace(line.substr(keyEnd));
    const bool startsWithEquals = (!remainder.empty() && remainder.front() == '=');
    if (startsWithEquals) {
        remainder = trimWhitespace(remainder.substr(1));
    }
    value = removeSurroundingQuotes(remainder);
    return !key.empty();
}

// Applies one setting to `target`, enforcing global-only scope and replacing
// inherited lists on their first appearance in the file (tracked in `listsSeen`).
static bool applySetting(ConfigurationTarget &target, const std::string &key, const std::string &value,
                         std::set<std::string> &listsSeen) {
    const OptionDefinition *definition = findOptionDefinition(key);
    if (definition == nullptr) {
        logMessage(LogLevel::ERROR, "unknown setting '%s'", key.c_str());
        return false;
    }
    const bool globalOnly = (definition->scope == OptionScope::GLOBAL_ONLY);
    const bool readingCameraFile = (target.global == nullptr);
    if (globalOnly && readingCameraFile) {
        logMessage(LogLevel::ERROR, "'%s' is only allowed in the global minimotion.conf", key.c_str());
        return false;
    }
    const bool isList = static_cast<bool>(definition->clearList);
    if (isList) {
        const bool firstInThisFile = (listsSeen.count(key) == 0);
        if (firstInThisFile) {
            definition->clearList(target);
            listsSeen.insert(key);
        }
    }
    return definition->parse(target, value);
}

// Reads one settings file into `target`. Lines starting with # or ; are
// comments. A missing file is an error only when `mustExist` is true.
static bool loadConfigurationFile(const std::string &path, ConfigurationTarget &target, bool mustExist) {
    FILE *configFile = fopen(path.c_str(), "r");
    if (configFile == nullptr) {
        const int openErrno = errno;
        const bool fileMissing = (openErrno == ENOENT);
        if (mustExist || !fileMissing) {
            logMessage(LogLevel::ERROR, "cannot open %s: %s", path.c_str(), describeErrno(openErrno).c_str());
            return false;
        }
        logMessage(LogLevel::WARNING, "%s not found, using built-in defaults", path.c_str());
        return true;
    }

    char lineBuffer[CONFIG_LINE_BUFFER_SIZE];
    int lineNumber = 0;
    bool allLinesValid = true;
    std::set<std::string> listsSeen;
    while (fgets(lineBuffer, sizeof lineBuffer, configFile) != nullptr) {
        lineNumber += 1;
        const std::string line = trimWhitespace(lineBuffer);
        const bool isBlank = line.empty();
        if (isBlank) {
            continue;
        }
        const bool isComment = (line[0] == '#' || line[0] == ';');
        if (isComment) {
            continue;
        }
        std::string key;
        std::string value;
        const bool lineSplit = splitSettingLine(line, key, value);
        bool lineValid = false;
        if (lineSplit) {
            lineValid = applySetting(target, key, value, listsSeen);
        }
        if (!lineValid) {
            logMessage(LogLevel::ERROR, "%s:%d: invalid line: %s", path.c_str(), lineNumber, line.c_str());
            allLinesValid = false;
        }
    }
    fclose(configFile);
    return allLinesValid;
}

// Returns `path` unchanged if absolute, otherwise relative to the folder of
// `referenceFile` (camera files listed in minimotion.conf work like Motion's).
static std::string resolveRelativeTo(const std::string &referenceFile, const std::string &path) {
    const bool isAbsolute = (!path.empty() && path.front() == '/');
    if (isAbsolute) {
        return path;
    }
    const size_t lastSlash = referenceFile.rfind('/');
    const bool referenceHasFolder = (lastSlash != std::string::npos);
    if (!referenceHasFolder) {
        return path;
    }
    const std::string referenceFolder = referenceFile.substr(0, lastSlash);
    return referenceFolder + "/" + path;
}

// File name without folder and extension ("/opt/rpi/camera1.conf" -> "camera1").
static std::string fileStem(const std::string &path) {
    const size_t lastSlash = path.rfind('/');
    size_t nameStart = 0;
    if (lastSlash != std::string::npos) {
        nameStart = lastSlash + 1;
    }
    const std::string fileName = path.substr(nameStart);
    const size_t lastDot = fileName.rfind('.');
    const bool hasExtension = (lastDot != std::string::npos && lastDot > 0);
    if (!hasExtension) {
        return fileName;
    }
    return fileName.substr(0, lastDot);
}

// Reads minimotion.conf and every camera file it lists. Each camera starts
// from the settings in minimotion.conf and its own file overrides them; its
// name defaults to the file name (camera1.conf -> "camera1"). Without camera
// lines, minimotion.conf itself is the one camera.
static bool loadAllConfiguration(const std::string &globalPath, bool mustExist, GlobalSettings &global,
                                 std::vector<CameraConfig> &cameras) {
    CameraConfig defaults;
    defaults.configPath = globalPath;
    ConfigurationTarget globalTarget;
    globalTarget.global = &global;
    globalTarget.camera = &defaults;
    const bool globalLoaded = loadConfigurationFile(globalPath, globalTarget, mustExist);
    if (!globalLoaded) {
        return false;
    }

    const bool singleCameraMode = global.cameraConfigPaths.empty();
    if (singleCameraMode) {
        cameras.push_back(defaults);
        return true;
    }

    bool allCamerasLoaded = true;
    for (const std::string &listedPath : global.cameraConfigPaths) {
        const std::string cameraPath = resolveRelativeTo(globalPath, listedPath);
        CameraConfig camera = defaults;
        camera.name = fileStem(cameraPath);
        camera.configPath = cameraPath;
        ConfigurationTarget cameraTarget;
        cameraTarget.global = nullptr;
        cameraTarget.camera = &camera;
        const bool cameraLoaded = loadConfigurationFile(cameraPath, cameraTarget, true);
        if (cameraLoaded) {
            cameras.push_back(camera);
        } else {
            allCamerasLoaded = false;
        }
    }
    return allCamerasLoaded;
}

// Checks the cameras against each other: names must be unique; shared or
// overlapping folders only produce warnings.
static bool validateCameras(const std::vector<CameraConfig> &cameras) {
    bool configurationValid = true;
    std::map<std::string, std::string> cameraByName;
    std::map<std::string, std::string> cameraByTemporaryDirectory;
    for (const CameraConfig &camera : cameras) {
        const auto sameName = cameraByName.find(camera.name);
        const bool nameTaken = (sameName != cameraByName.end());
        if (nameTaken) {
            logMessage(LogLevel::ERROR, "%s and %s both use camera_name '%s'", sameName->second.c_str(),
                       camera.configPath.c_str(), camera.name.c_str());
            configurationValid = false;
        } else {
            cameraByName[camera.name] = camera.configPath;
        }

        const std::string &temporaryDirectory = camera.events.temporaryDirectory;
        const auto sameTarget = cameraByTemporaryDirectory.find(temporaryDirectory);
        const bool targetShared = (sameTarget != cameraByTemporaryDirectory.end());
        if (targetShared) {
            logMessage(LogLevel::WARNING, "cameras %s and %s share temp_dir %s; their movies will be mixed",
                       sameTarget->second.c_str(), camera.name.c_str(), temporaryDirectory.c_str());
        } else {
            cameraByTemporaryDirectory[temporaryDirectory] = camera.name;
        }

        const bool hasDestinations = !camera.copy.destinations.empty();
        if (!hasDestinations) {
            logMessage(LogLevel::WARNING, "camera %s: no copy_to set; movies stay in temp_dir %s",
                       camera.name.c_str(), temporaryDirectory.c_str());
        }
        const bool fallbackIsTemporary = (camera.copy.fallbackDirectory == temporaryDirectory);
        if (fallbackIsTemporary) {
            logMessage(LogLevel::WARNING, "camera %s: fallback_dir is the same folder as temp_dir",
                       camera.name.c_str());
        }
        for (const std::string &destination : camera.copy.destinations) {
            const bool destinationIsTemporary = (destination == temporaryDirectory);
            if (destinationIsTemporary) {
                logMessage(LogLevel::WARNING, "camera %s: copy_to %s is the same folder as temp_dir",
                           camera.name.c_str(), destination.c_str());
            }
        }
    }
    return configurationValid;
}

// ============================================================================
// Filesystem and process helpers
// ============================================================================

// Sleeps for `seconds`, waking early if a stop was requested.
static void sleepInterruptibly(int seconds) {
    const int totalSlices = seconds * SLEEP_SLICES_PER_SECOND;
    for (int slice = 0; slice < totalSlices; ++slice) {
        if (stopRequested.load()) {
            return;
        }
        usleep(SLEEP_SLICE_MICROSECONDS);
    }
}

// Creates `directoryPath` and any missing parents (like mkdir -p) and checks
// that it is writable. On failure returns false with the cause in `failureErrno`.
// Safe if another thread or process creates the same folder at the same time.
static bool createDirectoryTree(const std::string &directoryPath, int &failureErrno) {
    failureErrno = 0;
    std::string partialPath;
    const size_t pathLength = directoryPath.size();
    for (size_t index = 0; index <= pathLength; ++index) {
        const bool atEnd = (index == pathLength);
        bool atSeparator = false;
        if (!atEnd) {
            atSeparator = (directoryPath[index] == '/');
        }
        const bool componentComplete = (atEnd || atSeparator);
        if (componentComplete && !partialPath.empty()) {
            const int mkdirResult = mkdir(partialPath.c_str(), DIRECTORY_PERMISSIONS);
            const int mkdirErrno = errno;
            const bool alreadyExists = (mkdirErrno == EEXIST);
            if (mkdirResult != 0 && !alreadyExists) {
                failureErrno = mkdirErrno;
                return false;
            }
        }
        if (!atEnd) {
            partialPath.push_back(directoryPath[index]);
        }
    }
    const int accessResult = access(directoryPath.c_str(), W_OK);
    if (accessResult != 0) {
        failureErrno = errno;
        return false;
    }
    return true;
}

// Formats `localTime` with a strftime pattern such as "%Y-%m-%d".
static std::string formatLocalTime(const struct tm &localTime, const char *pattern) {
    char timeText[TIME_TEXT_BUFFER_SIZE] = {0};
    strftime(timeText, sizeof timeText, pattern, &localTime);
    return std::string(timeText);
}

// Returns "<basePath>.mp4" for attempt 0 and "<basePath>_<attempt>.mp4" after
// that, the naming used whenever a movie name is already taken.
static std::string numberedMoviePath(const std::string &basePath, int attempt) {
    std::string candidatePath = basePath;
    if (attempt > 0) {
        candidatePath += "_" + std::to_string(attempt);
    }
    candidatePath += MOVIE_FILE_EXTENSION;
    return candidatePath;
}

// Where a new movie lives: the full local path, and the part below
// temp_dir ("YYYY-MM-DD/YYYY-MM-DD_HH.MM.SS.mp4") that copies reuse.
struct MovieLocation {
    std::string fullPath;
    std::string relativePath;
};

// Creates a new, empty movie file named after the current local time,
// <dir>/YYYY-MM-DD/YYYY-MM-DD_HH.MM.SS.mp4, adding _1, _2... if that name
// exists. The day folder is created on the first movie of each day. The file
// is created with O_EXCL, so the name is claimed atomically: no other thread,
// process or second event in the same second can end up with the same file.
static bool reserveMovieFile(const std::string &directoryPath, MovieLocation &location) {
    const time_t currentTime = time(nullptr);
    struct tm localTime;
    localtime_r(&currentTime, &localTime);

    // Folder and file name come from the same moment, so an event starting at
    // 23:59:59.9 can never land in the next day's folder.
    const std::string dayName = formatLocalTime(localTime, DAY_FOLDER_FORMAT);
    const std::string dayFolderPath = directoryPath + "/" + dayName;
    int folderErrno = 0;
    const bool dayFolderReady = createDirectoryTree(dayFolderPath, folderErrno);
    if (!dayFolderReady) {
        logMessage(LogLevel::ERROR, "cannot create folder %s: %s", dayFolderPath.c_str(),
                   describeErrno(folderErrno).c_str());
        return false;
    }
    const std::string timestampText = formatLocalTime(localTime, MOVIE_NAME_FORMAT);
    const std::string basePath = dayFolderPath + "/" + timestampText;

    for (int attempt = 0; attempt < MAX_MOVIE_NAME_ATTEMPTS; ++attempt) {
        const std::string candidatePath = numberedMoviePath(basePath, attempt);
        const int openFlags = O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC;
        const int fileDescriptor = open(candidatePath.c_str(), openFlags, MOVIE_FILE_PERMISSIONS);
        if (fileDescriptor >= 0) {
            close(fileDescriptor);
            const size_t relativeStart = directoryPath.size() + 1;
            location.fullPath = candidatePath;
            location.relativePath = candidatePath.substr(relativeStart);
            return true;
        }
        const int openErrno = errno;
        const bool nameTaken = (openErrno == EEXIST);
        if (!nameTaken) {
            logMessage(LogLevel::ERROR, "cannot create %s: %s", candidatePath.c_str(),
                       describeErrno(openErrno).c_str());
            return false;
        }
    }
    logMessage(LogLevel::ERROR, "too many movies named %s*", basePath.c_str());
    return false;
}

// Wraps `text` in single quotes for /bin/sh, escaping embedded quotes.
static std::string quoteForShell(const std::string &text) {
    std::string quotedText = "'";
    for (const char character : text) {
        if (character == '\'') {
            quotedText += "'\\''";
        } else {
            quotedText.push_back(character);
        }
    }
    quotedText += "'";
    return quotedText;
}

// Replaces every %f in `commandTemplate` with `replacement`.
static std::string expandFilePlaceholder(const std::string &commandTemplate, const std::string &replacement) {
    const std::string placeholder = FILE_PLACEHOLDER;
    std::string expandedCommand;
    size_t searchFrom = 0;
    while (true) {
        const size_t placeholderPosition = commandTemplate.find(placeholder, searchFrom);
        const bool placeholderFound = (placeholderPosition != std::string::npos);
        if (!placeholderFound) {
            expandedCommand += commandTemplate.substr(searchFrom);
            return expandedCommand;
        }
        const size_t literalLength = placeholderPosition - searchFrom;
        expandedCommand += commandTemplate.substr(searchFrom, literalLength);
        expandedCommand += replacement;
        searchFrom = placeholderPosition + placeholder.size();
    }
}

// Starts the on_movie_end command in the background with %f replaced by the
// movie path, without waiting for it. The child gets default SIGCHLD and
// SIGPIPE handling back, because this process ignores both.
static void runMovieEndCommand(const std::string &commandTemplate, const std::string &moviePath) {
    if (commandTemplate.empty()) {
        return;
    }
    const std::string quotedPath = quoteForShell(moviePath);
    const std::string command = expandFilePlaceholder(commandTemplate, quotedPath);

    posix_spawnattr_t spawnAttributes;
    posix_spawnattr_init(&spawnAttributes);
    sigset_t signalsToReset;
    sigemptyset(&signalsToReset);
    sigaddset(&signalsToReset, SIGCHLD);
    sigaddset(&signalsToReset, SIGPIPE);
    posix_spawnattr_setsigdefault(&spawnAttributes, &signalsToReset);
    posix_spawnattr_setflags(&spawnAttributes, POSIX_SPAWN_SETSIGDEF);

    char shellName[] = "sh";
    char commandFlag[] = "-c";
    std::vector<char> commandText(command.begin(), command.end());
    commandText.push_back('\0');
    char *shellArguments[] = {shellName, commandFlag, commandText.data(), nullptr};

    pid_t childProcessId = 0;
    const int spawnResult = posix_spawn(&childProcessId, SHELL_PATH, nullptr, &spawnAttributes,
                                        shellArguments, environ);
    posix_spawnattr_destroy(&spawnAttributes);
    if (spawnResult != 0) {
        logMessage(LogLevel::WARNING, "on_movie_end failed to start: %s", describeErrno(spawnResult).c_str());
    }
}

// ============================================================================
// Copying movie files
// ============================================================================

// read() that retries when a signal interrupts it.
static ssize_t readRetryingInterrupts(int fileDescriptor, char *buffer, size_t capacity) {
    while (true) {
        const ssize_t bytesRead = read(fileDescriptor, buffer, capacity);
        const bool interrupted = (bytesRead < 0 && errno == EINTR);
        if (!interrupted) {
            return bytesRead;
        }
    }
}

// Writes all `byteCount` bytes, continuing after short writes and signals.
static bool writeAll(int fileDescriptor, const char *data, size_t byteCount) {
    size_t bytesDone = 0;
    while (bytesDone < byteCount) {
        const size_t bytesLeft = byteCount - bytesDone;
        const ssize_t bytesWritten = write(fileDescriptor, data + bytesDone, bytesLeft);
        if (bytesWritten < 0) {
            const bool interrupted = (errno == EINTR);
            if (interrupted) {
                continue;
            }
            return false;
        }
        bytesDone += static_cast<size_t>(bytesWritten);
    }
    return true;
}

// Checks that a copy destination can be used right now: the folder must
// already exist, be a folder and be writable. It is never created here. Point
// copy_to at a folder that lives on the share (e.g. /mnt/nas1/cameras): when
// the share is not mounted that folder is missing, so the copy is refused
// instead of being written into the empty local mount point on the SD card.
static bool checkDestinationAvailable(const std::string &destinationRoot, std::string &failureReason) {
    struct stat destinationStatus;
    const int statResult = stat(destinationRoot.c_str(), &destinationStatus);
    if (statResult != 0) {
        const int statErrno = errno;
        failureReason = "not available (" + describeErrno(statErrno) + "; not mounted?)";
        return false;
    }
    const bool isDirectory = S_ISDIR(destinationStatus.st_mode);
    if (!isDirectory) {
        failureReason = "not a folder";
        return false;
    }
    const int accessResult = access(destinationRoot.c_str(), W_OK);
    if (accessResult != 0) {
        const int accessErrno = errno;
        failureReason = "not writable (" + describeErrno(accessErrno) + ")";
        return false;
    }
    return true;
}

// Copies the bytes of `sourcePath` into a new `destinationPath` and flushes
// them to the storage (fsync), so a network share reports write errors here
// rather than later. A failed copy is deleted.
static bool copyFileContents(const std::string &sourcePath, const std::string &destinationPath,
                             std::string &failureReason) {
    const int sourceDescriptor = open(sourcePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (sourceDescriptor < 0) {
        const int openErrno = errno;
        failureReason = "cannot read " + sourcePath + " (" + describeErrno(openErrno) + ")";
        return false;
    }
    const int destinationFlags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC;
    const int destinationDescriptor = open(destinationPath.c_str(), destinationFlags, MOVIE_FILE_PERMISSIONS);
    if (destinationDescriptor < 0) {
        const int openErrno = errno;
        close(sourceDescriptor);
        failureReason = "cannot create file (" + describeErrno(openErrno) + ")";
        return false;
    }

    std::vector<char> copyBuffer(COPY_BUFFER_BYTES);
    bool copySucceeded = true;
    while (copySucceeded) {
        const ssize_t bytesRead = readRetryingInterrupts(sourceDescriptor, copyBuffer.data(), copyBuffer.size());
        if (bytesRead == 0) {
            break;
        }
        if (bytesRead < 0) {
            const int readErrno = errno;
            failureReason = "read failed (" + describeErrno(readErrno) + ")";
            copySucceeded = false;
            break;
        }
        const bool chunkWritten = writeAll(destinationDescriptor, copyBuffer.data(), static_cast<size_t>(bytesRead));
        if (!chunkWritten) {
            const int writeErrno = errno;
            failureReason = "write failed (" + describeErrno(writeErrno) + ")";
            copySucceeded = false;
        }
    }
    if (copySucceeded) {
        const int syncResult = fsync(destinationDescriptor);
        if (syncResult != 0) {
            const int syncErrno = errno;
            failureReason = "flush failed (" + describeErrno(syncErrno) + ")";
            copySucceeded = false;
        }
    }
    const int closeResult = close(destinationDescriptor);
    if (closeResult != 0 && copySucceeded) {
        const int closeErrno = errno;
        failureReason = "close failed (" + describeErrno(closeErrno) + ")";
        copySucceeded = false;
    }
    close(sourceDescriptor);
    if (!copySucceeded) {
        unlink(destinationPath.c_str());
    }
    return copySucceeded;
}

// Copies a movie to <destinationRoot>/<relativePath>, creating the day folder.
// The data goes to a ".part" file first and is renamed into place once
// complete, so nobody ever sees a half-written movie. If the name is taken,
// _1, _2... is added. Callers must not copy to the same destination from two
// threads at once (each destination has its own worker thread).
static bool copyMovieToDirectory(const std::string &sourcePath, const std::string &destinationRoot,
                                 const std::string &relativePath, std::string &copiedPath,
                                 std::string &failureReason) {
    const bool available = checkDestinationAvailable(destinationRoot, failureReason);
    if (!available) {
        return false;
    }

    const size_t folderEnd = relativePath.rfind('/');
    std::string dayFolderName;
    std::string fileName = relativePath;
    if (folderEnd != std::string::npos) {
        dayFolderName = relativePath.substr(0, folderEnd);
        fileName = relativePath.substr(folderEnd + 1);
    }
    std::string targetFolder = destinationRoot;
    if (!dayFolderName.empty()) {
        targetFolder += "/" + dayFolderName;
    }
    int folderErrno = 0;
    const bool folderReady = createDirectoryTree(targetFolder, folderErrno);
    if (!folderReady) {
        failureReason = "cannot create " + targetFolder + " (" + describeErrno(folderErrno) + ")";
        return false;
    }

    const std::string extension = MOVIE_FILE_EXTENSION;
    std::string fileStemName = fileName;
    const bool hasMovieExtension = (fileName.size() > extension.size() &&
                                    fileName.compare(fileName.size() - extension.size(), extension.size(), extension) == 0);
    if (hasMovieExtension) {
        const size_t stemLength = fileName.size() - extension.size();
        fileStemName = fileName.substr(0, stemLength);
    }
    const std::string basePath = targetFolder + "/" + fileStemName;
    std::string finalPath;
    for (int attempt = 0; attempt < MAX_MOVIE_NAME_ATTEMPTS; ++attempt) {
        const std::string candidatePath = numberedMoviePath(basePath, attempt);
        const int accessResult = access(candidatePath.c_str(), F_OK);
        const bool nameFree = (accessResult != 0 && errno == ENOENT);
        if (nameFree) {
            finalPath = candidatePath;
            break;
        }
    }
    if (finalPath.empty()) {
        failureReason = "no free file name in " + targetFolder;
        return false;
    }

    const std::string partialPath = finalPath + PARTIAL_COPY_SUFFIX;
    const bool contentsCopied = copyFileContents(sourcePath, partialPath, failureReason);
    if (!contentsCopied) {
        return false;
    }
    const int renameResult = rename(partialPath.c_str(), finalPath.c_str());
    if (renameResult != 0) {
        const int renameErrno = errno;
        unlink(partialPath.c_str());
        failureReason = "rename failed (" + describeErrno(renameErrno) + ")";
        return false;
    }
    copiedPath = finalPath;
    return true;
}

// ============================================================================
// FFmpeg helpers
// ============================================================================

// True when the packet starts a group of pictures (can be decoded on its own).
static bool packetIsKeyframe(const AVPacket *packet) {
    const int keyframeBits = packet->flags & AV_PKT_FLAG_KEY;
    return keyframeBits != 0;
}

// The packet's decode timestamp, or its presentation timestamp if it has none.
static int64_t packetDecodeTimestamp(const AVPacket *packet) {
    const bool hasDecodeTimestamp = (packet->dts != AV_NOPTS_VALUE);
    if (hasDecodeTimestamp) {
        return packet->dts;
    }
    return packet->pts;
}

// True for pixel formats that live in hardware memory (e.g. DRM PRIME).
static bool isHardwarePixelFormat(int pixelFormat) {
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pixelFormat));
    if (descriptor == nullptr) {
        return false;
    }
    const uint64_t hardwareBits = descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL;
    return hardwareBits != 0;
}

// True when plane 0 holds 8-bit luma, one byte per pixel (yuv420p, nv12, ...).
static bool hasEightBitLumaPlane(int pixelFormat) {
    const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(pixelFormat));
    if (descriptor == nullptr) {
        return false;
    }
    const uint64_t unsuitableFlags = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_HWACCEL |
                                     AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM;
    const uint64_t unsuitableBits = descriptor->flags & unsuitableFlags;
    const bool hasUnsuitableFlag = (unsuitableBits != 0);
    const AVComponentDescriptor &lumaComponent = descriptor->comp[0];
    const bool lumaInFirstPlane = (lumaComponent.plane == 0);
    const bool lumaIsEightBit = (lumaComponent.depth == 8);
    const bool lumaIsOneBytePerPixel = (lumaComponent.step == 1);
    return !hasUnsuitableFlag && lumaInFirstPlane && lumaIsEightBit && lumaIsOneBytePerPixel;
}

// get_format callback: picks the first pixel format the decoder offers that
// lives in normal memory, so frames can be read without a hardware mapping.
static AVPixelFormat pickSoftwarePixelFormat(AVCodecContext *, const AVPixelFormat *offeredFormats) {
    for (const AVPixelFormat *candidate = offeredFormats; *candidate != AV_PIX_FMT_NONE; ++candidate) {
        const bool isHardware = isHardwarePixelFormat(*candidate);
        if (!isHardware) {
            return *candidate;
        }
    }
    return offeredFormats[0];
}

// FFmpeg's send/receive API returns EAGAIN from a send when the codec's output
// has to be collected first. This calls `sendOnce`; on EAGAIN it runs
// `collectOutput` and tries again, sleeping briefly between attempts so a
// hardware codec has time to finish. Returns the final send result, the first
// error from `collectOutput`, or ETIMEDOUT if the codec never accepts input.
static int sendWithBackpressure(const std::function<int()> &sendOnce, const std::function<int()> &collectOutput) {
    for (int attempt = 0; attempt <= BACKPRESSURE_RETRY_LIMIT; ++attempt) {
        const int sendResult = sendOnce();
        const bool codecFull = (sendResult == AVERROR(EAGAIN));
        if (!codecFull) {
            return sendResult;
        }
        const int collectResult = collectOutput();
        if (collectResult < 0) {
            return collectResult;
        }
        if (attempt > 0) {
            av_usleep(BACKPRESSURE_SLEEP_MICROSECONDS);
        }
    }
    return AVERROR(ETIMEDOUT);
}

// ============================================================================
// H.264 bitstream scanning
// ============================================================================

// What an Annex-B H.264 buffer contains.
struct AnnexBScanResult {
    bool hasSequenceParameterSet = false;
    bool hasPictureParameterSet = false;
    bool hasIdrSlice = false;
    bool hasNonIdrSlice = false;
};

// Returns the offset of the next 00 00 01 start code at or after `searchFrom`,
// or `size` if there is none.
static int findStartCode(const uint8_t *data, int size, int searchFrom) {
    const int lastPossibleStart = size - SHORT_START_CODE_LENGTH;
    for (int offset = searchFrom; offset <= lastPossibleStart; ++offset) {
        const bool firstZero = (data[offset] == 0);
        const bool secondZero = (data[offset + 1] == 0);
        const bool thenOne = (data[offset + 2] == 1);
        if (firstZero && secondZero && thenOne) {
            return offset;
        }
    }
    return size;
}

// Lists the NAL unit types in an Annex-B buffer. If `parameterSets` is given,
// every SPS and PPS found is appended to it with a 00 00 00 01 start code, in
// the form MP4 extradata accepts.
static AnnexBScanResult scanAnnexB(const uint8_t *data, int size, std::vector<uint8_t> *parameterSets) {
    AnnexBScanResult result;
    int startCodeOffset = findStartCode(data, size, 0);
    while (startCodeOffset < size) {
        const int unitBegin = startCodeOffset + SHORT_START_CODE_LENGTH;
        const int nextStartCodeOffset = findStartCode(data, size, unitBegin);
        int unitEnd = nextStartCodeOffset;
        // Trailing zero bytes belong to the next 4-byte start code, not this unit.
        while (unitEnd > unitBegin && data[unitEnd - 1] == 0) {
            unitEnd -= 1;
        }
        const bool unitHasContent = (unitBegin < unitEnd);
        if (unitHasContent) {
            const int nalType = data[unitBegin] & NAL_TYPE_MASK;
            const bool isSequenceParameterSet = (nalType == NAL_TYPE_SEQUENCE_PARAMETER_SET);
            const bool isPictureParameterSet = (nalType == NAL_TYPE_PICTURE_PARAMETER_SET);
            if (isSequenceParameterSet) {
                result.hasSequenceParameterSet = true;
            }
            if (isPictureParameterSet) {
                result.hasPictureParameterSet = true;
            }
            if (nalType == NAL_TYPE_IDR_SLICE) {
                result.hasIdrSlice = true;
            }
            if (nalType == NAL_TYPE_NON_IDR_SLICE) {
                result.hasNonIdrSlice = true;
            }
            const bool isParameterSet = (isSequenceParameterSet || isPictureParameterSet);
            if (parameterSets != nullptr && isParameterSet) {
                const uint8_t *unitStart = data + unitBegin;
                const uint8_t *unitStop = data + unitEnd;
                parameterSets->insert(parameterSets->end(), std::begin(LONG_START_CODE), std::end(LONG_START_CODE));
                parameterSets->insert(parameterSets->end(), unitStart, unitStop);
            }
        }
        startCodeOffset = nextStartCodeOffset;
    }
    return result;
}

// ============================================================================
// Timestamps
// ============================================================================

// Turns a source's raw timestamps into a clean, strictly increasing timeline
// starting at 0. A gap longer than MAX_TIMESTAMP_GAP_SECONDS, a backward jump
// or a missing timestamp (reconnects, RTCP resyncs) becomes one nominal frame
// step instead of a hole or a jump in the output.
class TimestampNormalizer {
public:
    // Starts a new timeline in `timeBase` ticks for a stream at `framesPerSecond`.
    void reset(AVRational timeBase, double framesPerSecond) {
        double secondsPerTick = av_q2d(timeBase);
        const bool timeBaseUsable = (secondsPerTick > 0.0);
        if (!timeBaseUsable) {
            secondsPerTick = 1.0 / FALLBACK_TICKS_PER_SECOND;
        }
        double usableFramesPerSecond = framesPerSecond;
        const bool frameRateUsable = (usableFramesPerSecond > 0.0);
        if (!frameRateUsable) {
            usableFramesPerSecond = FALLBACK_FRAMES_PER_SECOND;
        }

        const double secondsPerFrame = 1.0 / usableFramesPerSecond;
        const double ticksPerFrame = secondsPerFrame / secondsPerTick;
        const int64_t roundedTicksPerFrame = llround(ticksPerFrame);
        nominalFrameTicks = std::max<int64_t>(1, roundedTicksPerFrame);

        const double maximumGapTicks = MAX_TIMESTAMP_GAP_SECONDS / secondsPerTick;
        maximumStepTicks = llround(maximumGapTicks);

        lastInputTimestamp = AV_NOPTS_VALUE;
        lastOutputTimestamp = NO_OUTPUT_TIMESTAMP_YET;
    }

    // Maps one input timestamp (AV_NOPTS_VALUE allowed) onto the clean timeline.
    int64_t normalize(int64_t inputTimestamp) {
        int64_t stepTicks = nominalFrameTicks;
        const bool inputValid = (inputTimestamp != AV_NOPTS_VALUE);
        const bool previousValid = (lastInputTimestamp != AV_NOPTS_VALUE);
        if (inputValid && previousValid) {
            const int64_t inputStep = inputTimestamp - lastInputTimestamp;
            const bool movesForward = (inputStep > 0);
            const bool withinMaximumGap = (inputStep <= maximumStepTicks);
            if (movesForward && withinMaximumGap) {
                stepTicks = inputStep;
            }
        }
        if (inputValid) {
            lastInputTimestamp = inputTimestamp;
        }

        const bool isFirstOutput = (lastOutputTimestamp == NO_OUTPUT_TIMESTAMP_YET);
        if (isFirstOutput) {
            lastOutputTimestamp = 0;
        } else {
            lastOutputTimestamp = lastOutputTimestamp + stepTicks;
        }
        return lastOutputTimestamp;
    }

private:
    int64_t nominalFrameTicks = 1;
    int64_t maximumStepTicks = 1;
    int64_t lastInputTimestamp = AV_NOPTS_VALUE;
    int64_t lastOutputTimestamp = NO_OUTPUT_TIMESTAMP_YET;
};

// Seconds of stream time since the session started. Pre-capture, event gap and
// movie length are measured in stream time, so recordings get exactly the
// configured lengths regardless of network jitter.
class StreamClock {
public:
    // Starts at 0 for a stream with this time base and frame rate.
    void reset(AVRational timeBase, double framesPerSecond) {
        normalizer.reset(timeBase, framesPerSecond);
        double usableSecondsPerTick = av_q2d(timeBase);
        const bool timeBaseUsable = (usableSecondsPerTick > 0.0);
        if (!timeBaseUsable) {
            usableSecondsPerTick = 1.0 / FALLBACK_TICKS_PER_SECOND;
        }
        secondsPerTick = usableSecondsPerTick;
        currentSeconds = 0.0;
    }

    // Moves the clock to the packet with `timestamp` and returns the new time.
    double advance(int64_t timestamp) {
        const int64_t normalizedTicks = normalizer.normalize(timestamp);
        const double tickCount = static_cast<double>(normalizedTicks);
        currentSeconds = tickCount * secondsPerTick;
        return currentSeconds;
    }

    // The time of the most recent packet.
    double now() const {
        return currentSeconds;
    }

private:
    TimestampNormalizer normalizer;
    double secondsPerTick = 1.0 / FALLBACK_TICKS_PER_SECOND;
    double currentSeconds = 0.0;
};

// ============================================================================
// Data flow interfaces
// ============================================================================

// Receives compressed packets. The packet stays owned by the caller.
class PacketSink {
public:
    virtual ~PacketSink() = default;
    virtual void consumePacket(const AVPacket *packet) = 0;
};

// Receives decoded frames. The frame is only valid during the call and may be
// modified by the sink (timestamps, picture type).
class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void consumeFrame(AVFrame *frame) = 0;
};

// ============================================================================
// Source stream description
// ============================================================================

// An owned copy of what recording needs to know about the source video stream,
// so nothing keeps pointers into the demuxer's internal structures.
class StreamDescription {
public:
    StreamDescription() = default;
    ~StreamDescription() {
        avcodec_parameters_free(&codecParameters);
    }
    StreamDescription(const StreamDescription &) = delete;
    StreamDescription &operator=(const StreamDescription &) = delete;

    // Copies the stream's codec parameters and timing. False if out of memory.
    bool assign(const AVStream *stream, double streamFramesPerSecond) {
        avcodec_parameters_free(&codecParameters);
        codecParameters = avcodec_parameters_alloc();
        if (codecParameters == nullptr) {
            return false;
        }
        const int copyResult = avcodec_parameters_copy(codecParameters, stream->codecpar);
        if (copyResult < 0) {
            avcodec_parameters_free(&codecParameters);
            return false;
        }
        streamTimeBase = stream->time_base;
        framesPerSecondValue = streamFramesPerSecond;
        return true;
    }

    const AVCodecParameters *parameters() const {
        return codecParameters;
    }
    AVRational timeBase() const {
        return streamTimeBase;
    }
    double framesPerSecond() const {
        return framesPerSecondValue;
    }

private:
    AVCodecParameters *codecParameters = nullptr;
    AVRational streamTimeBase{1, FALLBACK_TICKS_PER_SECOND};
    double framesPerSecondValue = FALLBACK_FRAMES_PER_SECOND;
};

// ============================================================================
// Pre-capture buffer
// ============================================================================

// Holds compressed packets (references, not copies) starting at the most
// recent keyframe that is at least `keepSeconds` old, so a recording can begin
// with a decodable keyframe and the moments before motion was noticed.
class PreCaptureBuffer {
public:
    PreCaptureBuffer() = default;
    ~PreCaptureBuffer() {
        clear();
    }
    PreCaptureBuffer(const PreCaptureBuffer &) = delete;
    PreCaptureBuffer &operator=(const PreCaptureBuffer &) = delete;

    // Adds a packet seen at `streamSeconds` and drops packets no longer needed.
    void push(const AVPacket *packet, double streamSeconds, double keepSeconds) {
        const bool isKeyframe = packetIsKeyframe(packet);
        const bool bufferEmpty = entries.empty();
        if (bufferEmpty && !isKeyframe) {
            return;  // the buffer must always start with a keyframe
        }
        AVPacket *packetReference = av_packet_clone(packet);
        if (packetReference == nullptr) {
            return;
        }
        Entry newEntry;
        newEntry.packet = packetReference;
        newEntry.streamSeconds = streamSeconds;
        newEntry.isKeyframe = isKeyframe;
        entries.push_back(newEntry);
        bufferedBytes += static_cast<size_t>(packetReference->size);

        dropExpiredGroups(streamSeconds, keepSeconds);
        const bool stillTooLarge = (bufferedBytes > MAX_PRE_CAPTURE_BYTES);
        if (stillTooLarge) {
            clear();  // a single group of pictures exceeds the cap: start over
        }
    }

    // Hands every buffered packet, oldest first, to `sink` and empties the buffer.
    void drainInto(PacketSink &sink) {
        for (Entry &entry : entries) {
            sink.consumePacket(entry.packet);
            av_packet_free(&entry.packet);
        }
        entries.clear();
        bufferedBytes = 0;
    }

    // Releases every buffered packet.
    void clear() {
        const size_t entryCount = entries.size();
        dropOldest(entryCount);
    }

private:
    struct Entry {
        AVPacket *packet = nullptr;
        double streamSeconds = 0.0;
        bool isKeyframe = false;
    };

    // Index of the first keyframe after the oldest entry, or entries.size().
    size_t findSecondKeyframe() const {
        const size_t entryCount = entries.size();
        for (size_t index = 1; index < entryCount; ++index) {
            if (entries[index].isKeyframe) {
                return index;
            }
        }
        return entryCount;
    }

    // Drops the oldest group of pictures while the next group alone still
    // reaches back `keepSeconds`, or while the buffer is over its byte cap.
    void dropExpiredGroups(double newestSeconds, double keepSeconds) {
        const double cutoffSeconds = newestSeconds - keepSeconds;
        while (true) {
            const size_t secondKeyframeIndex = findSecondKeyframe();
            const bool onlyOneGroup = (secondKeyframeIndex >= entries.size());
            if (onlyOneGroup) {
                return;
            }
            const double secondGroupStart = entries[secondKeyframeIndex].streamSeconds;
            const bool secondGroupTooRecent = (secondGroupStart > cutoffSeconds);
            const bool withinByteCap = (bufferedBytes <= MAX_PRE_CAPTURE_BYTES);
            if (secondGroupTooRecent && withinByteCap) {
                return;
            }
            dropOldest(secondKeyframeIndex);
        }
    }

    // Releases the `count` oldest entries (fewer if the buffer is shorter).
    void dropOldest(size_t count) {
        const size_t availableCount = entries.size();
        const size_t dropCount = std::min(count, availableCount);
        for (size_t dropped = 0; dropped < dropCount; ++dropped) {
            Entry &oldestEntry = entries.front();
            const size_t packetBytes = static_cast<size_t>(oldestEntry.packet->size);
            bufferedBytes -= packetBytes;
            av_packet_free(&oldestEntry.packet);
            entries.pop_front();
        }
    }

    std::deque<Entry> entries;
    size_t bufferedBytes = 0;
};

// ============================================================================
// H.264 decoder
// ============================================================================

// Decodes H.264, preferring the Pi's hardware decoder (h264_v4l2m2m) and
// switching to software if the hardware one is missing, fails or stalls.
// Decoded frames go to a FrameSink and are released as soon as it returns.
class H264Decoder {
public:
    explicit H264Decoder(const char *decoderRole) : role(decoderRole) {}
    ~H264Decoder() {
        close();
    }
    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    // Prepares to decode a stream with these parameters. False if no decoder works.
    bool open(const AVCodecParameters *parameters, const DecoderSettings &decoderSettings) {
        close();
        settings = decoderSettings;
        streamParameters = avcodec_parameters_alloc();
        decodedFrame = av_frame_alloc();
        transferFrame = av_frame_alloc();
        const bool allocated = (streamParameters != nullptr && decodedFrame != nullptr && transferFrame != nullptr);
        if (!allocated) {
            close();
            return false;
        }
        const int copyResult = avcodec_parameters_copy(streamParameters, parameters);
        if (copyResult < 0) {
            close();
            return false;
        }

        if (settings.useHardware) {
            const bool openedWithFewerBuffers = openCodec(true, true);
            if (openedWithFewerBuffers) {
                return true;
            }
            const bool openedWithDefaultBuffers = openCodec(true, false);
            if (openedWithDefaultBuffers) {
                return true;
            }
        }
        const bool openedSoftware = openCodec(false, false);
        if (openedSoftware) {
            return true;
        }
        close();
        return false;
    }

    // Releases the decoder and its frames.
    void close() {
        avcodec_free_context(&codecContext);
        av_frame_free(&decodedFrame);
        av_frame_free(&transferFrame);
        avcodec_parameters_free(&streamParameters);
    }

    // Decodes one packet (nullptr flushes) and passes every finished frame to
    // `sink`. Packets before the first keyframe are skipped. Returns false only
    // if decoding failed and no replacement decoder could be opened.
    bool decode(const AVPacket *packet, FrameSink &sink) {
        if (codecContext == nullptr) {
            return false;
        }
        const bool isFlush = (packet == nullptr);
        if (!isFlush) {
            const bool isKeyframe = packetIsKeyframe(packet);
            if (waitingForKeyframe) {
                if (!isKeyframe) {
                    return true;
                }
                waitingForKeyframe = false;
            }
            packetsSinceLastFrame += 1;
        }

        int result = sendPacket(packet, sink);
        const bool corruptPacket = (result == AVERROR_INVALIDDATA);
        const bool alreadyFlushed = (result == AVERROR_EOF);
        if (corruptPacket || alreadyFlushed) {
            result = 0;  // skip the bad packet, keep decoding
        }
        if (result >= 0) {
            result = receiveFrames(sink);
        }
        if (result >= 0 && usingHardware) {
            const bool hardwareStalled = (packetsSinceLastFrame > HARDWARE_STALL_PACKET_LIMIT);
            if (hardwareStalled) {
                result = AVERROR(ETIMEDOUT);
            }
        }
        if (result >= 0) {
            return true;
        }
        return replaceFailedDecoder(result);
    }

private:
    // Opens the hardware or software decoder. With `limitBuffers` the hardware
    // decoder uses fewer V4L2 buffers (less CMA memory); that is enough for
    // Pi-encoded streams, which use one reference frame.
    bool openCodec(bool useHardware, bool limitBuffers) {
        const AVCodec *codec = nullptr;
        if (useHardware) {
            codec = avcodec_find_decoder_by_name(HARDWARE_CODEC_NAME);
        } else {
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        }
        if (codec == nullptr) {
            const char *kind = "software";
            if (useHardware) {
                kind = "hardware";
            }
            logMessage(LogLevel::INFO, "%s: no %s H.264 decoder", role, kind);
            return false;
        }

        codecContext = avcodec_alloc_context3(codec);
        if (codecContext == nullptr) {
            return false;
        }
        const int parameterResult = avcodec_parameters_to_context(codecContext, streamParameters);
        if (parameterResult < 0) {
            avcodec_free_context(&codecContext);
            return false;
        }
        codecContext->get_format = pickSoftwarePixelFormat;

        AVDictionary *codecOptions = nullptr;
        if (useHardware) {
            if (limitBuffers) {
                av_dict_set(&codecOptions, "num_capture_buffers", HARDWARE_DECODER_CAPTURE_BUFFERS, 0);
                av_dict_set(&codecOptions, "num_output_buffers", HARDWARE_DECODER_OUTPUT_BUFFERS, 0);
            }
        } else {
            codecContext->thread_count = settings.softwareThreads;
            codecContext->thread_type = FF_THREAD_FRAME;
            if (settings.skipDeblocking) {
                codecContext->skip_loop_filter = AVDISCARD_ALL;
                codecContext->flags2 = codecContext->flags2 | AV_CODEC_FLAG2_FAST;
            }
        }
        const int openResult = avcodec_open2(codecContext, codec, &codecOptions);
        av_dict_free(&codecOptions);
        if (openResult < 0) {
            logMessage(LogLevel::INFO, "%s: cannot open %s: %s", role, codec->name, describeError(openResult).c_str());
            avcodec_free_context(&codecContext);
            return false;
        }

        usingHardware = useHardware;
        waitingForKeyframe = true;
        packetsSinceLastFrame = 0;
        const char *description = "software";
        if (useHardware) {
            description = "hardware (h264_v4l2m2m)";
        }
        logMessage(LogLevel::INFO, "%s: using %s decoder", role, description);
        return true;
    }

    // After an error, replaces the current decoder with the software decoder.
    bool replaceFailedDecoder(int errorCode) {
        logMessage(LogLevel::WARNING, "%s: %s decoder failed: %s", role, codecContext->codec->name,
                   describeError(errorCode).c_str());
        avcodec_free_context(&codecContext);
        return openCodec(false, false);
    }

    // Sends a packet, collecting frames first if the decoder is full.
    int sendPacket(const AVPacket *packet, FrameSink &sink) {
        std::function<int()> sendOnce = [this, packet]() {
            return avcodec_send_packet(codecContext, packet);
        };
        std::function<int()> collectOutput = [this, &sink]() {
            return receiveFrames(sink);
        };
        return sendWithBackpressure(sendOnce, collectOutput);
    }

    // Passes every frame the decoder has ready to `sink`.
    int receiveFrames(FrameSink &sink) {
        while (true) {
            const int receiveResult = avcodec_receive_frame(codecContext, decodedFrame);
            const bool noFrameReady = (receiveResult == AVERROR(EAGAIN));
            const bool decoderDrained = (receiveResult == AVERROR_EOF);
            if (noFrameReady || decoderDrained) {
                return 0;
            }
            if (receiveResult < 0) {
                return receiveResult;
            }
            packetsSinceLastFrame = 0;
            const int deliverResult = deliverFrame(sink);
            if (deliverResult < 0) {
                return deliverResult;
            }
        }
    }

    // Hands `decodedFrame` to the sink, copying it into normal memory first if
    // the decoder produced a hardware surface. Both frames are released after.
    int deliverFrame(FrameSink &sink) {
        AVFrame *frameToDeliver = decodedFrame;
        const bool isHardwareSurface = isHardwarePixelFormat(decodedFrame->format);
        if (isHardwareSurface) {
            av_frame_unref(transferFrame);
            const int transferResult = av_hwframe_transfer_data(transferFrame, decodedFrame, 0);
            if (transferResult < 0) {
                av_frame_unref(transferFrame);
                av_frame_unref(decodedFrame);
                return transferResult;
            }
            av_frame_copy_props(transferFrame, decodedFrame);
            frameToDeliver = transferFrame;
        }
        sink.consumeFrame(frameToDeliver);
        av_frame_unref(decodedFrame);
        av_frame_unref(transferFrame);
        return 0;
    }

    const char *role;
    DecoderSettings settings;
    AVCodecParameters *streamParameters = nullptr;
    AVCodecContext *codecContext = nullptr;
    AVFrame *decodedFrame = nullptr;
    AVFrame *transferFrame = nullptr;
    bool usingHardware = false;
    bool waitingForKeyframe = true;
    int packetsSinceLastFrame = 0;
};

// ============================================================================
// Motion detection
// ============================================================================

// A read-only view of a luma (brightness) plane.
struct LumaImage {
    const uint8_t *pixels = nullptr;
    int bytesPerRow = 0;
    int width = 0;
    int height = 0;
};

// The verdict for one analysed frame.
struct MotionResult {
    bool motionDetected = false;
    bool lightChange = false;
    double changedPercent = 0.0;
};

// The detection algorithm. Per analysed frame, on a ~160x90 image (14 400 px):
//   1. Box-average the luma plane down by an integer factor. Averaging 8x8
//      blocks cuts sensor and IR night noise ~5x before anything else.
//   2. Compare with a per-pixel running-average background (8.8 fixed point).
//      |difference| > noise_level marks the pixel as changed.
//   3. If most of the picture changed at once (lights, IR day/night switch,
//      exposure jump) it is a light change: re-learn, no event.
//   4. Despeckle: a changed pixel only counts when at least 2 of its 4
//      neighbours changed too, which removes isolated noise pixels.
//   5. Motion when the counted area >= threshold_percent for
//      minimum_motion_frames analysed frames in a row.
//   6. Update the background: slowly where something is moving (so it does not
//      burn in), quickly everywhere else, including pixels that differ from the
//      background but are still frame to frame - the "ghost" left where an
//      object stood, or something that stopped.
// Integer-only loops over small arrays; GCC vectorises them with NEON.
class MotionDetector {
public:
    MotionDetector(const DetectionSettings &detectionSettings, double analysedFramesPerSecond)
        : settings(detectionSettings) {
        fastBlendShift = blendShiftForTimeConstant(FAST_BACKGROUND_SECONDS, analysedFramesPerSecond);
        slowBlendShift = blendShiftForTimeConstant(SLOW_BACKGROUND_SECONDS, analysedFramesPerSecond);
        const double settleFramesExact = SETTLE_SECONDS * analysedFramesPerSecond;
        const int settleFramesRounded = static_cast<int>(lround(settleFramesExact));
        settleFrameCount = std::max(MIN_SETTLE_FRAMES, settleFramesRounded);
    }

    // Analyses one frame and returns whether it shows motion.
    MotionResult analyse(const LumaImage &image) {
        MotionResult result;
        const bool sizeChanged = (image.width != sourceWidth || image.height != sourceHeight);
        if (sizeChanged) {
            configureForSize(image.width, image.height);
        }
        downscale(image);

        const bool isFirstFrame = (analysedFrameCount == 0);
        analysedFrameCount += 1;
        if (isFirstFrame) {
            restartBackground();
            return result;
        }

        const bool settling = (framesUntilSettled > 0);
        if (settling) {
            framesUntilSettled -= 1;
            settleBackground();
            rememberCurrentImage();
            consecutiveMotionFrames = 0;
            return result;
        }

        const int changedPixelCount = markChangedPixels();
        const bool lightChanged = isLightChange(changedPixelCount);
        if (lightChanged) {
            restartBackground();
            result.lightChange = true;
            return result;
        }

        const int confirmedPixelCount = countConfirmedPixels();
        updateBackground();
        rememberCurrentImage();

        const int analysisPixelCount = analysisWidth * analysisHeight;
        const double confirmedFraction = static_cast<double>(confirmedPixelCount) / analysisPixelCount;
        const double confirmedPercent = confirmedFraction * PERCENT_SCALE;
        result.changedPercent = confirmedPercent;

        const bool overThreshold = (confirmedPercent >= settings.thresholdPercent);
        if (overThreshold) {
            consecutiveMotionFrames += 1;
        } else {
            consecutiveMotionFrames = 0;
        }
        result.motionDetected = (consecutiveMotionFrames >= settings.minimumMotionFrames);
        return result;
    }

private:
    // Blend shift whose exponential average has roughly the given time constant:
    // each update moves the background 1/2^shift of the way to the new value.
    static int blendShiftForTimeConstant(double timeConstantSeconds, double framesPerSecond) {
        const double framesInTimeConstant = timeConstantSeconds * framesPerSecond;
        const double atLeastOneFrame = std::max(1.0, framesInTimeConstant);
        const double exactShift = std::log2(atLeastOneFrame);
        const long roundedShift = lround(exactShift);
        const int shift = static_cast<int>(roundedShift);
        return std::clamp(shift, MIN_BLEND_SHIFT, MAX_BLEND_SHIFT);
    }

    // Chooses the downscale factor for this frame size and sizes the buffers.
    void configureForSize(int width, int height) {
        sourceWidth = width;
        sourceHeight = height;

        const int longSide = std::max(width, height);
        const int blockSizeForTarget = longSide / settings.analysisLongSide;
        blockSize = std::max(1, blockSizeForTarget);
        analysisWidth = width / blockSize;
        analysisHeight = height / blockSize;
        const bool analysisTooSmall = (analysisWidth < MIN_ANALYSIS_DIMENSION || analysisHeight < MIN_ANALYSIS_DIMENSION);
        if (analysisTooSmall) {
            blockSize = 1;
            analysisWidth = width;
            analysisHeight = height;
        }

        // Large blocks: sampling every other row is plenty and halves the work.
        rowStep = ROW_STEP_SMALL_BLOCKS;
        if (blockSize >= ROW_SKIPPING_MIN_BLOCK_SIZE) {
            rowStep = ROW_STEP_LARGE_BLOCKS;
        }
        const int roundedUpRows = blockSize + rowStep - 1;
        const int rowsSampledPerBlock = roundedUpRows / rowStep;
        const uint32_t samplesPerBlock = static_cast<uint32_t>(blockSize * rowsSampledPerBlock);
        // 1/samplesPerBlock in 16.16 fixed point, rounded to nearest.
        const uint32_t halfSampleCount = samplesPerBlock / 2;
        const uint32_t roundedNumerator = RECIPROCAL_ONE + halfSampleCount;
        reciprocalSampleCount = roundedNumerator / samplesPerBlock;

        const size_t analysisPixelCount = static_cast<size_t>(analysisWidth) * static_cast<size_t>(analysisHeight);
        currentImage.assign(analysisPixelCount, 0);
        previousImage.assign(analysisPixelCount, 0);
        changeMask.assign(analysisPixelCount, 0);
        background.assign(analysisPixelCount, 0);
        rowAccumulator.assign(static_cast<size_t>(analysisWidth), 0);
        analysedFrameCount = 0;
        consecutiveMotionFrames = 0;
        framesUntilSettled = 0;
        logMessage(LogLevel::INFO, "analysing %dx%d frames at %dx%d", width, height, analysisWidth, analysisHeight);
    }

    // Box-averages the luma plane into `currentImage`.
    void downscale(const LumaImage &image) {
        const size_t bytesPerRow = static_cast<size_t>(image.bytesPerRow);
        for (int outputRow = 0; outputRow < analysisHeight; ++outputRow) {
            std::fill(rowAccumulator.begin(), rowAccumulator.end(), 0u);
            const size_t firstSourceRow = static_cast<size_t>(outputRow) * static_cast<size_t>(blockSize);
            for (int rowInBlock = 0; rowInBlock < blockSize; rowInBlock += rowStep) {
                const size_t sourceRow = firstSourceRow + static_cast<size_t>(rowInBlock);
                const size_t sourceRowOffset = sourceRow * bytesPerRow;
                const uint8_t *sourcePixel = image.pixels + sourceRowOffset;
                for (int outputColumn = 0; outputColumn < analysisWidth; ++outputColumn) {
                    uint32_t blockRowSum = 0;
                    for (int columnInBlock = 0; columnInBlock < blockSize; ++columnInBlock) {
                        blockRowSum += sourcePixel[columnInBlock];
                    }
                    rowAccumulator[outputColumn] += blockRowSum;
                    sourcePixel += blockSize;
                }
            }

            const size_t outputRowOffset = static_cast<size_t>(outputRow) * static_cast<size_t>(analysisWidth);
            uint8_t *outputPixels = currentImage.data() + outputRowOffset;
            for (int outputColumn = 0; outputColumn < analysisWidth; ++outputColumn) {
                const uint32_t blockSum = rowAccumulator[outputColumn];
                const uint32_t scaledSum = blockSum * reciprocalSampleCount;
                const uint32_t averageValue = scaledSum >> RECIPROCAL_FRACTION_BITS;
                const uint32_t clampedValue = std::min(averageValue, MAX_LUMA_VALUE);
                outputPixels[outputColumn] = static_cast<uint8_t>(clampedValue);
            }
        }
    }

    // Replaces the background with the current image and waits for the
    // picture to settle (used at start-up and after a light change).
    void restartBackground() {
        const size_t pixelCount = currentImage.size();
        for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
            const uint16_t currentValue = currentImage[pixelIndex];
            const uint16_t fixedPointValue = static_cast<uint16_t>(currentValue << BACKGROUND_FRACTION_BITS);
            background[pixelIndex] = fixedPointValue;
        }
        rememberCurrentImage();
        framesUntilSettled = settleFrameCount;
        consecutiveMotionFrames = 0;
    }

    // While settling, the background follows the picture very quickly.
    void settleBackground() {
        const size_t pixelCount = currentImage.size();
        for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
            blendPixel(pixelIndex, SETTLE_BLEND_SHIFT);
        }
    }

    // Marks pixels that differ from the background by more than noise_level.
    // Returns how many were marked.
    int markChangedPixels() {
        const int noiseThresholdFixedPoint = settings.noiseLevel << BACKGROUND_FRACTION_BITS;
        const size_t pixelCount = currentImage.size();
        int changedPixelCount = 0;
        for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
            const int currentFixedPoint = static_cast<int>(currentImage[pixelIndex]) << BACKGROUND_FRACTION_BITS;
            const int backgroundFixedPoint = static_cast<int>(background[pixelIndex]);
            const int difference = currentFixedPoint - backgroundFixedPoint;
            const int absoluteDifference = std::abs(difference);
            const bool pixelChanged = (absoluteDifference > noiseThresholdFixedPoint);
            const uint8_t maskValue = static_cast<uint8_t>(pixelChanged);
            changeMask[pixelIndex] = maskValue;
            changedPixelCount += maskValue;
        }
        return changedPixelCount;
    }

    // True when so much of the picture changed that it must be the lighting.
    bool isLightChange(int changedPixelCount) const {
        const bool lightSwitchDetectionEnabled = (settings.lightswitchPercent > 0.0);
        if (!lightSwitchDetectionEnabled) {
            return false;
        }
        const int analysisPixelCount = analysisWidth * analysisHeight;
        const double changedFraction = static_cast<double>(changedPixelCount) / analysisPixelCount;
        const double changedPercent = changedFraction * PERCENT_SCALE;
        return changedPercent >= settings.lightswitchPercent;
    }

    // Counts changed pixels with at least MIN_CHANGED_NEIGHBOURS changed
    // neighbours (left, right, above, below). Border pixels are skipped.
    int countConfirmedPixels() const {
        int confirmedPixelCount = 0;
        const int lastRow = analysisHeight - 1;
        const int lastColumn = analysisWidth - 1;
        for (int row = 1; row < lastRow; ++row) {
            const size_t rowOffset = static_cast<size_t>(row) * static_cast<size_t>(analysisWidth);
            const uint8_t *maskRow = changeMask.data() + rowOffset;
            const uint8_t *maskRowAbove = maskRow - analysisWidth;
            const uint8_t *maskRowBelow = maskRow + analysisWidth;
            for (int column = 1; column < lastColumn; ++column) {
                const int centre = maskRow[column];
                const int left = maskRow[column - 1];
                const int right = maskRow[column + 1];
                const int above = maskRowAbove[column];
                const int below = maskRowBelow[column];
                const int changedNeighbours = left + right + above + below;
                const int neighbourhoodConfirms = (changedNeighbours >= MIN_CHANGED_NEIGHBOURS);
                const int pixelConfirmed = centre & neighbourhoodConfirms;
                confirmedPixelCount += pixelConfirmed;
            }
        }
        return confirmedPixelCount;
    }

    // Moves every background pixel towards the current image: slowly where an
    // object is moving, quickly everywhere else (see step 6 above).
    void updateBackground() {
        const int noiseLevel = settings.noiseLevel;
        const size_t pixelCount = currentImage.size();
        for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
            const int currentValue = currentImage[pixelIndex];
            const int previousValue = previousImage[pixelIndex];
            const int frameToFrameChange = std::abs(currentValue - previousValue);
            const bool differsFromBackground = (changeMask[pixelIndex] != 0);
            const bool stillChanging = (frameToFrameChange > noiseLevel);
            const bool isMovingObject = (differsFromBackground && stillChanging);
            int blendShift = fastBlendShift;
            if (isMovingObject) {
                blendShift = slowBlendShift;
            }
            blendPixel(pixelIndex, blendShift);
        }
    }

    // Moves one background pixel 1/2^blendShift of the way to the current value.
    void blendPixel(size_t pixelIndex, int blendShift) {
        const int backgroundValue = background[pixelIndex];
        const int targetValue = static_cast<int>(currentImage[pixelIndex]) << BACKGROUND_FRACTION_BITS;
        const int distanceToTarget = targetValue - backgroundValue;
        const int blendDivisor = 1 << blendShift;
        const int adjustment = distanceToTarget / blendDivisor;
        const int newBackgroundValue = backgroundValue + adjustment;
        background[pixelIndex] = static_cast<uint16_t>(newBackgroundValue);
    }

    // The current image becomes the previous one for the next comparison.
    void rememberCurrentImage() {
        previousImage.swap(currentImage);
    }

    DetectionSettings settings;
    int fastBlendShift = MIN_BLEND_SHIFT;
    int slowBlendShift = MIN_BLEND_SHIFT;
    int settleFrameCount = MIN_SETTLE_FRAMES;
    int sourceWidth = 0;
    int sourceHeight = 0;
    int blockSize = 1;
    int analysisWidth = 0;
    int analysisHeight = 0;
    int rowStep = ROW_STEP_SMALL_BLOCKS;
    uint32_t reciprocalSampleCount = 0;
    std::vector<uint8_t> currentImage;
    std::vector<uint8_t> previousImage;
    std::vector<uint8_t> changeMask;
    std::vector<uint16_t> background;
    std::vector<uint32_t> rowAccumulator;
    long analysedFrameCount = 0;
    int framesUntilSettled = 0;
    int consecutiveMotionFrames = 0;
};

// Feeds decoded frames to the MotionDetector at most detect_fps times per
// second of stream time and remembers whether any of them showed motion.
class MotionAnalyzer : public FrameSink {
public:
    MotionAnalyzer(const DetectionSettings &detectionSettings, double streamFramesPerSecond)
        : detector(detectionSettings, analysisRate(detectionSettings, streamFramesPerSecond)) {
        const bool throttled = (detectionSettings.analysisFramesPerSecond > 0.0);
        if (throttled) {
            analysisIntervalSeconds = 1.0 / detectionSettings.analysisFramesPerSecond;
        }
    }

    // Call before decoding each packet: sets the stream time and clears the flag.
    void beginPacket(double streamSeconds) {
        currentStreamSeconds = streamSeconds;
        motionSeenThisPacket = false;
    }

    // True if any frame decoded from the current packet showed motion.
    bool motionSeen() const {
        return motionSeenThisPacket;
    }

    void consumeFrame(AVFrame *frame) override {
        const bool due = isDueForAnalysis();
        if (!due) {
            return;
        }
        const bool usableFormat = hasEightBitLumaPlane(frame->format);
        const bool usableLayout = (frame->linesize[0] > 0);
        if (!usableFormat || !usableLayout) {
            if (!formatWarningShown) {
                const char *formatName = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
                logMessage(LogLevel::ERROR, "decoder output %s has no usable luma plane", formatName);
                formatWarningShown = true;
            }
            return;
        }

        LumaImage lumaImage;
        lumaImage.pixels = frame->data[0];
        lumaImage.bytesPerRow = frame->linesize[0];
        lumaImage.width = frame->width;
        lumaImage.height = frame->height;
        const MotionResult result = detector.analyse(lumaImage);

        if (result.lightChange) {
            logMessage(LogLevel::INFO, "light change, re-learning background");
        }
        const char *motionMarker = "";
        if (result.motionDetected) {
            motionMarker = "  MOTION";
            motionSeenThisPacket = true;
        }
        logMessage(LogLevel::DEBUG, "changed %.2f%%%s", result.changedPercent, motionMarker);
    }

private:
    // Frames per second actually analysed: detect_fps, capped at the stream rate.
    static double analysisRate(const DetectionSettings &detectionSettings, double streamFramesPerSecond) {
        const bool throttled = (detectionSettings.analysisFramesPerSecond > 0.0);
        if (!throttled) {
            return streamFramesPerSecond;
        }
        return std::min(detectionSettings.analysisFramesPerSecond, streamFramesPerSecond);
    }

    // True when enough stream time has passed since the last analysed frame.
    bool isDueForAnalysis() {
        const bool analyseEveryFrame = (analysisIntervalSeconds <= 0.0);
        if (analyseEveryFrame) {
            return true;
        }
        const double toleranceAdjustedTime = currentStreamSeconds + ANALYSIS_TIME_TOLERANCE_SECONDS;
        const bool tooEarly = (toleranceAdjustedTime < nextAnalysisSeconds);
        if (tooEarly) {
            return false;
        }
        nextAnalysisSeconds = nextAnalysisSeconds + analysisIntervalSeconds;
        const bool fellBehind = (nextAnalysisSeconds < currentStreamSeconds);
        if (fellBehind) {
            nextAnalysisSeconds = currentStreamSeconds + analysisIntervalSeconds;
        }
        return true;
    }

    MotionDetector detector;
    double analysisIntervalSeconds = 0.0;
    double currentStreamSeconds = 0.0;
    double nextAnalysisSeconds = 0.0;
    bool motionSeenThisPacket = false;
    bool formatWarningShown = false;
};

// ============================================================================
// MP4 output
// ============================================================================

// Writes one H.264 MP4 file. The file always starts on a keyframe: earlier
// packets are dropped, and the header is written when that keyframe arrives,
// taking SPS/PPS from the bitstream if the codec parameters lack them.
// Timestamps are rebased to start at 0 and forced to increase.
class Mp4Muxer {
public:
    Mp4Muxer() = default;
    ~Mp4Muxer() {
        close();
    }
    Mp4Muxer(const Mp4Muxer &) = delete;
    Mp4Muxer &operator=(const Mp4Muxer &) = delete;

    // Opens `path` for writing with the container options in `movieSettings`
    // (fragmentation, display rotation). On failure the file is removed and
    // false returned.
    bool open(const std::string &path, const MovieSettings &movieSettings) {
        close();
        filePath = path;
        fragmentedOutput = movieSettings.fragmented;
        rotationDegrees = movieSettings.rotationDegrees;
        headerWritten = false;
        writeFailed = false;
        timestampOffset = AV_NOPTS_VALUE;
        lastWrittenDecodeTimestamp = AV_NOPTS_VALUE;
        collectedParameterSets.clear();
        haveSequenceParameterSet = false;
        havePictureParameterSet = false;

        const int allocateResult = avformat_alloc_output_context2(&formatContext, nullptr, MOVIE_CONTAINER_NAME, path.c_str());
        if (allocateResult < 0 || formatContext == nullptr) {
            logMessage(LogLevel::ERROR, "cannot set up MP4 output: %s", describeError(allocateResult).c_str());
            formatContext = nullptr;
            removeFile();
            return false;
        }
        videoStream = avformat_new_stream(formatContext, nullptr);
        if (videoStream == nullptr) {
            releaseResources();
            removeFile();
            return false;
        }
        const int openResult = avio_open(&formatContext->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (openResult < 0) {
            logMessage(LogLevel::ERROR, "cannot open %s: %s", path.c_str(), describeError(openResult).c_str());
            releaseResources();
            removeFile();
            return false;
        }
        return true;
    }

    // The output stream; its codec parameters and time base must be filled in
    // before the first packet is written.
    AVStream *stream() const {
        return videoStream;
    }

    // Writes `packet` (timestamps in `packetTimeBase`) and unreferences it.
    void writePacket(AVPacket *packet, AVRational packetTimeBase) {
        const bool cannotWrite = (formatContext == nullptr || writeFailed);
        if (cannotWrite) {
            av_packet_unref(packet);
            return;
        }
        if (!headerWritten) {
            const bool canStartHere = isUsableFirstPacket(packet);
            if (!canStartHere) {
                av_packet_unref(packet);
                return;
            }
            const bool headerOk = writeHeader();
            if (!headerOk) {
                av_packet_unref(packet);
                return;
            }
        }
        packet->stream_index = videoStream->index;
        packet->pos = -1;
        av_packet_rescale_ts(packet, packetTimeBase, videoStream->time_base);
        sanitizeTimestamps(packet);
        const int writeResult = av_write_frame(formatContext, packet);
        av_packet_unref(packet);
        if (writeResult < 0) {
            logMessage(LogLevel::ERROR, "writing %s: %s", filePath.c_str(), describeError(writeResult).c_str());
            writeFailed = true;
        }
    }

    // Finishes the file. Returns true if it holds video; an empty file is deleted.
    bool close() {
        if (formatContext == nullptr) {
            return false;
        }
        const bool fileHasVideo = headerWritten;
        if (headerWritten) {
            const int trailerResult = av_write_trailer(formatContext);
            if (trailerResult < 0) {
                logMessage(LogLevel::ERROR, "finishing %s: %s", filePath.c_str(), describeError(trailerResult).c_str());
            }
        }
        releaseResources();
        if (!fileHasVideo) {
            removeFile();
        }
        return fileHasVideo;
    }

    bool hasFailed() const {
        return writeFailed;
    }

private:
    // True when `packet` can open the file: a keyframe with slice data, and
    // SPS/PPS known either from the codec parameters or from the stream so far.
    bool isUsableFirstPacket(const AVPacket *packet) {
        std::vector<uint8_t> *parameterSetTarget = nullptr;
        const bool stillCollecting = !(haveSequenceParameterSet && havePictureParameterSet);
        if (stillCollecting) {
            parameterSetTarget = &collectedParameterSets;
        }
        const AnnexBScanResult scan = scanAnnexB(packet->data, packet->size, parameterSetTarget);
        if (scan.hasSequenceParameterSet) {
            haveSequenceParameterSet = true;
        }
        if (scan.hasPictureParameterSet) {
            havePictureParameterSet = true;
        }

        const bool hasSliceData = (scan.hasIdrSlice || scan.hasNonIdrSlice);
        const bool hasParameterSets = (scan.hasSequenceParameterSet || scan.hasPictureParameterSet);
        const bool parameterSetsOnly = (hasParameterSets && !hasSliceData);
        if (parameterSetsOnly) {
            return false;
        }
        const bool isKeyframe = (scan.hasIdrSlice || packetIsKeyframe(packet));
        if (!isKeyframe) {
            return false;
        }

        AVCodecParameters *codecParameters = videoStream->codecpar;
        const bool codecHasExtradata = (codecParameters->extradata_size > 0);
        if (codecHasExtradata) {
            return true;
        }
        const bool collectedBoth = (haveSequenceParameterSet && havePictureParameterSet);
        if (!collectedBoth) {
            logMessage(LogLevel::DEBUG, "waiting for SPS/PPS before starting %s", filePath.c_str());
            return false;
        }
        return installCollectedExtradata();
    }

    // Stores the collected SPS/PPS as the stream's extradata.
    bool installCollectedExtradata() {
        AVCodecParameters *codecParameters = videoStream->codecpar;
        av_freep(&codecParameters->extradata);  // never leak a zero-length allocation
        codecParameters->extradata_size = 0;
        const size_t extradataSize = collectedParameterSets.size();
        const size_t allocationSize = extradataSize + AV_INPUT_BUFFER_PADDING_SIZE;
        uint8_t *extradata = static_cast<uint8_t *>(av_mallocz(allocationSize));
        if (extradata == nullptr) {
            writeFailed = true;
            return false;
        }
        memcpy(extradata, collectedParameterSets.data(), extradataSize);
        codecParameters->extradata = extradata;
        codecParameters->extradata_size = static_cast<int>(extradataSize);
        return true;
    }

    // Writes the MP4 header (fragmented and rotated if configured).
    bool writeHeader() {
        applyDisplayRotation();
        AVDictionary *muxerOptions = nullptr;
        if (fragmentedOutput) {
            av_dict_set(&muxerOptions, "movflags", FRAGMENTED_MP4_FLAGS, 0);
        }
        const int headerResult = avformat_write_header(formatContext, &muxerOptions);
        av_dict_free(&muxerOptions);
        if (headerResult < 0) {
            logMessage(LogLevel::ERROR, "writing %s: %s", filePath.c_str(), describeError(headerResult).c_str());
            writeFailed = true;
            return false;
        }
        headerWritten = true;
        return true;
    }

    // Marks the video as rotated: stores a display matrix on the stream, which
    // the MP4 muxer writes into the track header. Players rotate the picture
    // when showing it; the H.264 data is untouched, so this costs nothing and
    // motion detection is unaffected. A rotation of 0 leaves the stream as is.
    void applyDisplayRotation() {
        const bool rotationRequested = (rotationDegrees != 0);
        if (!rotationRequested) {
            return;
        }
        AVCodecParameters *codecParameters = videoStream->codecpar;
        // Replace any matrix inherited from the source rather than adding a second one.
        av_packet_side_data_remove(codecParameters->coded_side_data, &codecParameters->nb_coded_side_data,
                                   AV_PKT_DATA_DISPLAYMATRIX);
        const size_t matrixBytes = sizeof(int32_t) * DISPLAY_MATRIX_ELEMENTS;
        AVPacketSideData *matrixSideData =
            av_packet_side_data_new(&codecParameters->coded_side_data, &codecParameters->nb_coded_side_data,
                                    AV_PKT_DATA_DISPLAYMATRIX, matrixBytes, 0);
        if (matrixSideData == nullptr) {
            logMessage(LogLevel::WARNING, "cannot set rotation on %s (out of memory)", filePath.c_str());
            return;
        }
        int32_t *displayMatrix = reinterpret_cast<int32_t *>(matrixSideData->data);
        // Both the setting and av_display_rotation_set() use clockwise degrees
        // (verified: rotation = 90 turns the top-left corner to the top-right).
        const double clockwiseDegrees = static_cast<double>(rotationDegrees);
        av_display_rotation_set(displayMatrix, clockwiseDegrees);
    }

    // Fills missing timestamps, rebases the first packet to 0 and keeps decode
    // timestamps strictly increasing with presentation >= decode, as MP4 requires.
    void sanitizeTimestamps(AVPacket *packet) {
        const bool missingDecodeTimestamp = (packet->dts == AV_NOPTS_VALUE);
        if (missingDecodeTimestamp) {
            packet->dts = packet->pts;
        }
        const bool stillMissing = (packet->dts == AV_NOPTS_VALUE);
        if (stillMissing) {
            const bool nothingWrittenYet = (lastWrittenDecodeTimestamp == AV_NOPTS_VALUE);
            if (nothingWrittenYet) {
                packet->dts = 0;
            } else {
                packet->dts = lastWrittenDecodeTimestamp + 1;
            }
        }

        const bool isFirstPacket = (timestampOffset == AV_NOPTS_VALUE);
        if (isFirstPacket) {
            timestampOffset = packet->dts;
        }
        packet->dts = packet->dts - timestampOffset;
        const bool hasPresentationTimestamp = (packet->pts != AV_NOPTS_VALUE);
        if (hasPresentationTimestamp) {
            packet->pts = packet->pts - timestampOffset;
        }

        const bool havePrevious = (lastWrittenDecodeTimestamp != AV_NOPTS_VALUE);
        if (havePrevious) {
            const bool notIncreasing = (packet->dts <= lastWrittenDecodeTimestamp);
            if (notIncreasing) {
                packet->dts = lastWrittenDecodeTimestamp + 1;
            }
        }
        const bool presentationMissing = (packet->pts == AV_NOPTS_VALUE);
        const bool presentationTooEarly = (!presentationMissing && packet->pts < packet->dts);
        if (presentationMissing || presentationTooEarly) {
            packet->pts = packet->dts;
        }
        lastWrittenDecodeTimestamp = packet->dts;
    }

    // Closes the file handle and frees the muxer (no trailer).
    void releaseResources() {
        if (formatContext != nullptr) {
            if (formatContext->pb != nullptr) {
                avio_closep(&formatContext->pb);
            }
            avformat_free_context(formatContext);
            formatContext = nullptr;
        }
        videoStream = nullptr;
    }

    // Deletes the file at filePath (ignores a file that is already gone).
    void removeFile() {
        if (!filePath.empty()) {
            unlink(filePath.c_str());
        }
    }

    std::string filePath;
    bool fragmentedOutput = true;
    int rotationDegrees = 0;
    AVFormatContext *formatContext = nullptr;
    AVStream *videoStream = nullptr;
    bool headerWritten = false;
    bool writeFailed = false;
    int64_t timestampOffset = AV_NOPTS_VALUE;
    int64_t lastWrittenDecodeTimestamp = AV_NOPTS_VALUE;
    std::vector<uint8_t> collectedParameterSets;
    bool haveSequenceParameterSet = false;
    bool havePictureParameterSet = false;
};

// ============================================================================
// Movie writers
// ============================================================================

// Writes one movie file from the source's compressed packets.
class MovieWriter : public PacketSink {
public:
    // Opens a movie at `path` (an existing, empty file). On failure no file
    // is left behind.
    virtual bool start(const std::string &path, const StreamDescription &source) = 0;
    // Finishes the movie. True if the file holds video (else it was deleted).
    virtual bool finish() = 0;
    // True after an unrecoverable write or codec error.
    virtual bool hasFailed() const = 0;
};

// Stores the camera's H.264 packets unchanged: no decoding, no encoding.
class CopyMovieWriter : public MovieWriter {
public:
    explicit CopyMovieWriter(const MovieSettings &movieSettings) : settings(movieSettings) {}
    ~CopyMovieWriter() override {
        CopyMovieWriter::finish();
    }

    bool start(const std::string &path, const StreamDescription &source) override {
        finish();
        referenceFailed = false;
        sourceTimeBase = source.timeBase();
        normalizer.reset(sourceTimeBase, source.framesPerSecond());
        scratchPacket = av_packet_alloc();
        if (scratchPacket == nullptr) {
            unlink(path.c_str());
            return false;
        }
        const bool opened = muxer.open(path, settings);
        if (!opened) {
            av_packet_free(&scratchPacket);
            return false;
        }
        AVStream *outputStream = muxer.stream();
        const int copyResult = avcodec_parameters_copy(outputStream->codecpar, source.parameters());
        if (copyResult < 0) {
            muxer.close();
            av_packet_free(&scratchPacket);
            return false;
        }
        outputStream->codecpar->codec_tag = 0;
        outputStream->time_base = sourceTimeBase;
        return true;
    }

    void consumePacket(const AVPacket *packet) override {
        if (scratchPacket == nullptr) {
            return;
        }
        const int referenceResult = av_packet_ref(scratchPacket, packet);
        if (referenceResult < 0) {
            referenceFailed = true;
            return;
        }
        const int64_t sourceDecodeTimestamp = packetDecodeTimestamp(packet);
        const int64_t normalizedDecodeTimestamp = normalizer.normalize(sourceDecodeTimestamp);
        int64_t normalizedPresentationTimestamp = normalizedDecodeTimestamp;
        const bool bothTimestampsValid = (packet->pts != AV_NOPTS_VALUE && packet->dts != AV_NOPTS_VALUE);
        if (bothTimestampsValid) {
            const int64_t presentationDelay = packet->pts - packet->dts;
            const bool hasDelay = (presentationDelay > 0);
            if (hasDelay) {
                normalizedPresentationTimestamp = normalizedDecodeTimestamp + presentationDelay;
            }
        }
        scratchPacket->dts = normalizedDecodeTimestamp;
        scratchPacket->pts = normalizedPresentationTimestamp;
        const bool negativeDuration = (scratchPacket->duration < 0);
        if (negativeDuration) {
            scratchPacket->duration = 0;
        }
        muxer.writePacket(scratchPacket, sourceTimeBase);
    }

    bool finish() override {
        const bool fileHasVideo = muxer.close();
        av_packet_free(&scratchPacket);
        return fileHasVideo;
    }

    bool hasFailed() const override {
        return referenceFailed || muxer.hasFailed();
    }

private:
    MovieSettings settings;
    Mp4Muxer muxer;
    TimestampNormalizer normalizer;
    AVRational sourceTimeBase{1, FALLBACK_TICKS_PER_SECOND};
    AVPacket *scratchPacket = nullptr;
    bool referenceFailed = false;
};

// Decodes the camera's H.264 and re-encodes it on the Pi's hardware encoder
// at movie_bps, so file size and quality are set here rather than by the camera.
class ReencodeMovieWriter : public MovieWriter, public FrameSink {
public:
    ReencodeMovieWriter(const MovieSettings &movieSettings, const DecoderSettings &decoderSettings)
        : settings(movieSettings), recordingDecoderSettings(decoderSettings), decoder("recording") {
        // Recordings are always decoded at full quality.
        recordingDecoderSettings.skipDeblocking = false;
    }
    ~ReencodeMovieWriter() override {
        ReencodeMovieWriter::finish();
    }

    bool start(const std::string &path, const StreamDescription &source) override {
        finish();
        codecFailed = false;
        isFirstFrame = true;
        sourceTimeBase = source.timeBase();
        sourceFramesPerSecond = source.framesPerSecond();
        normalizer.reset(sourceTimeBase, sourceFramesPerSecond);
        encodedPacket = av_packet_alloc();
        if (encodedPacket == nullptr) {
            unlink(path.c_str());
            return false;
        }
        const bool opened = muxer.open(path, settings);
        if (!opened) {
            av_packet_free(&encodedPacket);
            return false;
        }
        const bool decoderOpened = decoder.open(source.parameters(), recordingDecoderSettings);
        if (!decoderOpened) {
            muxer.close();
            av_packet_free(&encodedPacket);
            return false;
        }
        return true;
    }

    void consumePacket(const AVPacket *packet) override {
        if (codecFailed || encodedPacket == nullptr) {
            return;
        }
        const bool decoded = decoder.decode(packet, *this);
        if (!decoded) {
            codecFailed = true;
        }
    }

    void consumeFrame(AVFrame *frame) override {
        if (codecFailed) {
            return;
        }
        if (encoderContext == nullptr) {
            const bool encoderOpened = openEncoder(frame);
            if (!encoderOpened) {
                codecFailed = true;
                return;
            }
        }
        const bool sizeChanged = (frame->width != encoderContext->width || frame->height != encoderContext->height);
        if (sizeChanged) {
            return;  // the encoder cannot change size mid-file; skip the frame
        }

        int64_t sourceTimestamp = frame->best_effort_timestamp;
        if (sourceTimestamp == AV_NOPTS_VALUE) {
            sourceTimestamp = frame->pts;
        }
        frame->pts = normalizer.normalize(sourceTimestamp);
        if (isFirstFrame) {
            frame->pict_type = AV_PICTURE_TYPE_I;  // every file starts with a keyframe
            isFirstFrame = false;
        } else {
            frame->pict_type = AV_PICTURE_TYPE_NONE;
        }
        if (frame->format == AV_PIX_FMT_YUVJ420P) {
            frame->format = AV_PIX_FMT_YUV420P;  // same memory layout, full-range flag kept in color_range
        }

        std::function<int()> sendOnce = [this, frame]() {
            return avcodec_send_frame(encoderContext, frame);
        };
        std::function<int()> collectOutput = [this]() {
            return collectEncodedPackets();
        };
        const int sendResult = sendWithBackpressure(sendOnce, collectOutput);
        if (sendResult < 0) {
            logMessage(LogLevel::ERROR, "encoder: %s", describeError(sendResult).c_str());
            codecFailed = true;
            return;
        }
        const int collectResult = collectEncodedPackets();
        if (collectResult < 0) {
            codecFailed = true;
        }
    }

    bool finish() override {
        if (encodedPacket != nullptr && !codecFailed) {
            decoder.decode(nullptr, *this);  // frames still inside the decoder
            if (encoderContext != nullptr && !codecFailed) {
                const int flushResult = avcodec_send_frame(encoderContext, nullptr);
                if (flushResult >= 0) {
                    collectEncodedPackets();
                }
            }
        }
        const bool fileHasVideo = muxer.close();
        avcodec_free_context(&encoderContext);
        decoder.close();
        av_packet_free(&encodedPacket);
        return fileHasVideo;
    }

    bool hasFailed() const override {
        return codecFailed || muxer.hasFailed();
    }

private:
    // Opens the hardware encoder for frames like `frame` and describes its
    // output on the MP4 stream.
    bool openEncoder(const AVFrame *frame) {
        const AVCodec *encoder = avcodec_find_encoder_by_name(HARDWARE_CODEC_NAME);
        if (encoder == nullptr) {
            logMessage(LogLevel::ERROR, "%s encoder not available", HARDWARE_CODEC_NAME);
            return false;
        }
        encoderContext = avcodec_alloc_context3(encoder);
        if (encoderContext == nullptr) {
            return false;
        }

        AVPixelFormat inputPixelFormat = static_cast<AVPixelFormat>(frame->format);
        if (inputPixelFormat == AV_PIX_FMT_YUVJ420P) {
            inputPixelFormat = AV_PIX_FMT_YUV420P;
        }
        int keyframeInterval = settings.keyframeIntervalFrames;
        const bool useDefaultInterval = (keyframeInterval <= 0);
        if (useDefaultInterval) {
            const double framesInDefaultInterval = sourceFramesPerSecond * DEFAULT_KEYFRAME_INTERVAL_SECONDS;
            const int roundedFrames = static_cast<int>(lround(framesInDefaultInterval));
            keyframeInterval = std::max(1, roundedFrames);
        }

        encoderContext->width = frame->width;
        encoderContext->height = frame->height;
        encoderContext->pix_fmt = inputPixelFormat;
        encoderContext->sample_aspect_ratio = frame->sample_aspect_ratio;
        encoderContext->color_range = frame->color_range;
        encoderContext->color_primaries = frame->color_primaries;
        encoderContext->color_trc = frame->color_trc;
        encoderContext->colorspace = frame->colorspace;
        encoderContext->time_base = sourceTimeBase;
        encoderContext->framerate = av_d2q(sourceFramesPerSecond, FRAME_RATE_MAX_DENOMINATOR);
        encoderContext->bit_rate = settings.bitrate;
        encoderContext->gop_size = keyframeInterval;
        encoderContext->max_b_frames = 0;

        AVDictionary *encoderOptions = nullptr;
        av_dict_set(&encoderOptions, "num_output_buffers", HARDWARE_ENCODER_OUTPUT_BUFFERS, 0);
        av_dict_set(&encoderOptions, "num_capture_buffers", HARDWARE_ENCODER_CAPTURE_BUFFERS, 0);
        const int openResult = avcodec_open2(encoderContext, encoder, &encoderOptions);
        av_dict_free(&encoderOptions);
        if (openResult < 0) {
            logMessage(LogLevel::ERROR, "cannot open hardware encoder for %dx%d %s: %s", frame->width, frame->height,
                       av_get_pix_fmt_name(inputPixelFormat), describeError(openResult).c_str());
            avcodec_free_context(&encoderContext);
            return false;
        }

        AVStream *outputStream = muxer.stream();
        const int parameterResult = avcodec_parameters_from_context(outputStream->codecpar, encoderContext);
        if (parameterResult < 0) {
            avcodec_free_context(&encoderContext);
            return false;
        }
        outputStream->codecpar->codec_tag = 0;
        outputStream->time_base = encoderContext->time_base;
        logMessage(LogLevel::INFO, "encoding %dx%d at %lld bit/s on the hardware encoder", frame->width,
                   frame->height, settings.bitrate);
        return true;
    }

    // Moves every packet the encoder has ready into the MP4 file.
    int collectEncodedPackets() {
        while (true) {
            const int receiveResult = avcodec_receive_packet(encoderContext, encodedPacket);
            const bool noPacketReady = (receiveResult == AVERROR(EAGAIN));
            const bool encoderDrained = (receiveResult == AVERROR_EOF);
            if (noPacketReady || encoderDrained) {
                return 0;
            }
            if (receiveResult < 0) {
                logMessage(LogLevel::ERROR, "encoder: %s", describeError(receiveResult).c_str());
                return receiveResult;
            }
            muxer.writePacket(encodedPacket, encoderContext->time_base);
        }
    }

    MovieSettings settings;
    DecoderSettings recordingDecoderSettings;
    H264Decoder decoder;
    AVCodecContext *encoderContext = nullptr;
    Mp4Muxer muxer;
    TimestampNormalizer normalizer;
    AVRational sourceTimeBase{1, FALLBACK_TICKS_PER_SECOND};
    double sourceFramesPerSecond = FALLBACK_FRAMES_PER_SECOND;
    AVPacket *encodedPacket = nullptr;
    bool codecFailed = false;
    bool isFirstFrame = true;
};

// Creates the movie writer for the configured record mode.
static std::unique_ptr<MovieWriter> createMovieWriter(const MovieSettings &movieSettings,
                                                      const DecoderSettings &decoderSettings) {
    if (movieSettings.recordMode == RecordMode::ENCODE) {
        return std::make_unique<ReencodeMovieWriter>(movieSettings, decoderSettings);
    }
    return std::make_unique<CopyMovieWriter>(movieSettings);
}

// True if the hardware H.264 encoder exists and can be opened.
static bool hardwareEncoderAvailable() {
    const AVCodec *encoder = avcodec_find_encoder_by_name(HARDWARE_CODEC_NAME);
    if (encoder == nullptr) {
        return false;
    }
    AVCodecContext *probeContext = avcodec_alloc_context3(encoder);
    if (probeContext == nullptr) {
        return false;
    }
    probeContext->width = PROBE_ENCODER_WIDTH;
    probeContext->height = PROBE_ENCODER_HEIGHT;
    probeContext->pix_fmt = AV_PIX_FMT_YUV420P;
    probeContext->time_base = AVRational{1, PROBE_ENCODER_FRAMES_PER_SECOND};
    probeContext->framerate = AVRational{PROBE_ENCODER_FRAMES_PER_SECOND, 1};
    probeContext->bit_rate = PROBE_ENCODER_BITRATE;
    const int openResult = avcodec_open2(probeContext, encoder, nullptr);
    avcodec_free_context(&probeContext);
    return openResult >= 0;
}

// ============================================================================
// Saving and copying finished movies
// ============================================================================

// A finished movie and what should happen to it next. Never modified after it
// is handed over, so any thread may read it without locking.
struct SavedMovie {
    std::string cameraName;
    std::string localPath;
    std::string relativePath;
    CopySettings copy;
    std::string onMovieEndCommand;
};

// Receives every movie once it is closed. Implementations must return quickly
// (the camera keeps recording meanwhile) and be safe to call from several
// camera threads at once.
class MovieSaveHandler {
public:
    virtual ~MovieSaveHandler() = default;
    virtual void movieSaved(const SavedMovie &movie) = 0;
};

// The result of copying one movie to one destination.
struct CopyOutcome {
    std::string destination;
    bool finished = false;
    bool succeeded = false;
    std::string copiedPath;
    std::string failureReason;
};

// One movie on its way to all its destinations. Shared (via shared_ptr) by the
// destination workers, which fill in outcomes, and the coordinator, which waits
// for them. `movie` and `deadlineMilliseconds` are fixed before the job is
// shared; everything after `mutex` is guarded by it.
struct CopyJob {
    SavedMovie movie;
    int64_t deadlineMilliseconds = 0;
    std::mutex mutex;
    std::condition_variable outcomeChanged;
    std::vector<CopyOutcome> outcomes;
    size_t pendingCount = 0;
    bool finalized = false;
};

// Stores the result of one destination in `job` and wakes the coordinator.
// A result arriving after the coordinator gave up on the job is only logged.
static void recordCopyOutcome(CopyJob &job, size_t outcomeIndex, bool succeeded, const std::string &copiedPath,
                              const std::string &failureReason) {
    bool arrivedLate = false;
    {
        std::lock_guard<std::mutex> jobLock(job.mutex);
        CopyOutcome &outcome = job.outcomes[outcomeIndex];
        if (outcome.finished) {
            return;
        }
        outcome.finished = true;
        outcome.succeeded = succeeded;
        outcome.copiedPath = copiedPath;
        outcome.failureReason = failureReason;
        job.pendingCount -= 1;
        arrivedLate = job.finalized;
    }
    job.outcomeChanged.notify_all();
    if (arrivedLate && succeeded) {
        logMessage(LogLevel::INFO, "late copy finished: %s", copiedPath.c_str());
    }
}

// Copies movies to one destination folder, one at a time, on its own thread.
// A slow or hung SMB server therefore only delays copies to that server, and
// copies to the same folder never race each other for a file name.
class DestinationWorker {
public:
    explicit DestinationWorker(const std::string &destinationPath) : destination(destinationPath) {}
    DestinationWorker(const DestinationWorker &) = delete;
    DestinationWorker &operator=(const DestinationWorker &) = delete;

    // Starts the worker thread.
    void start() {
        workerThread = std::thread(&DestinationWorker::run, this);
    }

    // Queues outcome `outcomeIndex` of `job`. Returns false (nothing queued) if
    // the worker has already been stuck on one copy for longer than
    // `timeoutSeconds`, so movies do not pile up behind a dead share.
    bool enqueue(const std::shared_ptr<CopyJob> &job, size_t outcomeIndex, int timeoutSeconds) {
        const int64_t busySince = busySinceMilliseconds.load();
        const bool busy = (busySince != WORKER_IDLE_MARKER);
        if (busy) {
            const int64_t busyMilliseconds = monotonicMilliseconds() - busySince;
            const int64_t timeoutMilliseconds = static_cast<int64_t>(timeoutSeconds) * MILLISECONDS_PER_SECOND;
            const bool stuck = (busyMilliseconds > timeoutMilliseconds);
            if (stuck) {
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> queueLock(queueMutex);
            if (stopping) {
                return false;
            }
            QueuedCopy queuedCopy;
            queuedCopy.job = job;
            queuedCopy.outcomeIndex = outcomeIndex;
            pendingCopies.push_back(queuedCopy);
        }
        queueChanged.notify_one();
        return true;
    }

    // Asks the thread to finish the queued copies and then exit.
    void requestStop() {
        {
            std::lock_guard<std::mutex> queueLock(queueMutex);
            stopping = true;
        }
        queueChanged.notify_one();
    }

    // Waits until `deadlineMilliseconds` (monotonic) for the thread to exit and
    // joins it. Returns false if it is still busy (e.g. blocked on a hung share).
    bool joinBefore(int64_t deadlineMilliseconds) {
        std::unique_lock<std::mutex> finishedLock(finishedMutex);
        while (!threadFinished) {
            const int64_t remainingMilliseconds = deadlineMilliseconds - monotonicMilliseconds();
            if (remainingMilliseconds <= 0) {
                return false;
            }
            finishedSignal.wait_for(finishedLock, std::chrono::milliseconds(remainingMilliseconds));
        }
        finishedLock.unlock();
        if (workerThread.joinable()) {
            workerThread.join();
        }
        return true;
    }

    // True while the thread object still refers to a running thread.
    bool isJoinable() const {
        return workerThread.joinable();
    }

    // Lets a thread that will not finish keep running on its own.
    void detach() {
        if (workerThread.joinable()) {
            workerThread.detach();
        }
    }

    const std::string &path() const {
        return destination;
    }

private:
    struct QueuedCopy {
        std::shared_ptr<CopyJob> job;
        size_t outcomeIndex = 0;
    };

    // Thread body: reports whether the destination is usable, then copies
    // queued movies until asked to stop and the queue is empty.
    void run() {
        setLogContext(DESTINATION_LOG_CONTEXT_PREFIX + destination);
        reportAvailability();
        while (true) {
            QueuedCopy nextCopy;
            {
                std::unique_lock<std::mutex> queueLock(queueMutex);
                queueChanged.wait(queueLock, [this]() {
                    return !pendingCopies.empty() || stopping;
                });
                if (pendingCopies.empty()) {
                    break;
                }
                nextCopy = pendingCopies.front();
                pendingCopies.pop_front();
            }
            copyOne(nextCopy);
        }
        {
            std::lock_guard<std::mutex> finishedLock(finishedMutex);
            threadFinished = true;
        }
        finishedSignal.notify_all();
    }

    // Logs once at start-up whether the destination can currently be written.
    void reportAvailability() {
        std::string failureReason;
        const bool available = checkDestinationAvailable(destination, failureReason);
        if (available) {
            logMessage(LogLevel::INFO, "destination is writable");
        } else {
            logMessage(LogLevel::WARNING, "destination is %s", failureReason.c_str());
        }
    }

    // Copies one movie, unless the coordinator already gave up on it.
    void copyOne(const QueuedCopy &queuedCopy) {
        CopyJob &job = *queuedCopy.job;
        {
            std::lock_guard<std::mutex> jobLock(job.mutex);
            if (job.finalized) {
                return;
            }
        }
        busySinceMilliseconds.store(monotonicMilliseconds());
        std::string copiedPath;
        std::string failureReason;
        const bool copied = copyMovieToDirectory(job.movie.localPath, destination, job.movie.relativePath,
                                                 copiedPath, failureReason);
        busySinceMilliseconds.store(WORKER_IDLE_MARKER);
        if (copied) {
            logMessage(LogLevel::DEBUG, "copied %s", copiedPath.c_str());
        }
        recordCopyOutcome(job, queuedCopy.outcomeIndex, copied, copiedPath, failureReason);
    }

    const std::string destination;
    std::thread workerThread;
    std::mutex queueMutex;
    std::condition_variable queueChanged;
    std::deque<QueuedCopy> pendingCopies;
    bool stopping = false;
    // When the current copy started, or WORKER_IDLE_MARKER. Atomic: written by
    // the worker, read by camera threads in enqueue(); 64-bit, so a plain
    // variable could tear on 32-bit ARM.
    std::atomic<int64_t> busySinceMilliseconds{WORKER_IDLE_MARKER};
    std::mutex finishedMutex;
    std::condition_variable finishedSignal;
    bool threadFinished = false;
};

// Handles every finished movie: copies it to all copy_to destinations in
// parallel, waits up to copy_timeout for them, writes it to fallback_dir if
// any destination could not be written, optionally removes the local file,
// and finally runs on_movie_end. One coordinator thread does the waiting and
// finishing; one DestinationWorker per distinct folder does the copying.
class MovieCopyService : public MovieSaveHandler {
public:
    // Creates one worker per distinct destination used by any camera. The
    // worker map is complete before any thread starts and never changes, so
    // camera threads may read it without locking.
    explicit MovieCopyService(const std::vector<CameraConfig> &cameras) {
        for (const CameraConfig &camera : cameras) {
            for (const std::string &destination : camera.copy.destinations) {
                const bool known = (workersByPath.count(destination) != 0);
                if (!known) {
                    workersByPath[destination] =
                        std::make_unique<DestinationWorker>(destination);
                }
            }
        }
    }

    ~MovieCopyService() override {
        shutdown(0);
    }
    MovieCopyService(const MovieCopyService &) = delete;
    MovieCopyService &operator=(const MovieCopyService &) = delete;

    // Starts the coordinator and every destination worker.
    void start() {
        for (auto &entry : workersByPath) {
            entry.second->start();
        }
        coordinatorThread = std::thread(&MovieCopyService::coordinate, this);
    }

    void movieSaved(const SavedMovie &movie) override {
        std::shared_ptr<CopyJob> job = std::make_shared<CopyJob>();
        job->movie = movie;
        const int64_t timeoutMilliseconds = static_cast<int64_t>(movie.copy.timeoutSeconds) * MILLISECONDS_PER_SECOND;
        job->deadlineMilliseconds = monotonicMilliseconds() + timeoutMilliseconds;
        const size_t destinationCount = movie.copy.destinations.size();
        job->outcomes.resize(destinationCount);
        for (size_t index = 0; index < destinationCount; ++index) {
            job->outcomes[index].destination = movie.copy.destinations[index];
        }
        job->pendingCount = destinationCount;

        for (size_t index = 0; index < destinationCount; ++index) {
            const std::string &destination = movie.copy.destinations[index];
            const auto workerEntry = workersByPath.find(destination);
            bool queued = false;
            if (workerEntry != workersByPath.end()) {
                queued = workerEntry->second->enqueue(job, index, movie.copy.timeoutSeconds);
            }
            if (!queued) {
                recordCopyOutcome(*job, index, false, "", "not responding (an earlier copy is still stuck)");
            }
        }
        {
            std::lock_guard<std::mutex> coordinatorLock(coordinatorMutex);
            finishedMovies.push_back(job);
        }
        coordinatorWakeup.notify_one();
    }

    // Finishes outstanding work within `graceSeconds`: movies still being
    // copied get the fallback treatment when the grace period ends. Returns
    // false if a destination worker is still blocked (e.g. on a hung share);
    // that thread is then left running so the process can exit.
    bool shutdown(int graceSeconds) {
        if (!coordinatorThread.joinable()) {
            return true;
        }
        const int64_t graceMilliseconds = static_cast<int64_t>(graceSeconds) * MILLISECONDS_PER_SECOND;
        const int64_t shutdownDeadline = monotonicMilliseconds() + graceMilliseconds;
        shutdownDeadlineMilliseconds.store(shutdownDeadline);
        {
            std::lock_guard<std::mutex> coordinatorLock(coordinatorMutex);
            coordinatorStopping = true;
        }
        coordinatorWakeup.notify_one();
        coordinatorThread.join();

        bool allWorkersFinished = true;
        for (auto &entry : workersByPath) {
            entry.second->requestStop();
        }
        for (auto &entry : workersByPath) {
            DestinationWorker &worker = *entry.second;
            const bool joined = worker.joinBefore(shutdownDeadline);
            if (!joined) {
                logMessage(LogLevel::WARNING, "copy to %s still blocked at shutdown", worker.path().c_str());
                // The thread may still use the worker object: detach it and
                // deliberately keep the object alive instead of destroying it.
                worker.detach();
                entry.second.release();
                allWorkersFinished = false;
            }
        }
        return allWorkersFinished;
    }

private:
    // Coordinator thread: finishes movies in the order they were saved.
    void coordinate() {
        setLogContext(COPY_COORDINATOR_LOG_CONTEXT);
        while (true) {
            std::shared_ptr<CopyJob> job;
            {
                std::unique_lock<std::mutex> coordinatorLock(coordinatorMutex);
                coordinatorWakeup.wait(coordinatorLock, [this]() {
                    return !finishedMovies.empty() || coordinatorStopping;
                });
                if (finishedMovies.empty()) {
                    return;
                }
                job = finishedMovies.front();
                finishedMovies.pop_front();
            }
            const std::vector<CopyOutcome> outcomes = waitForOutcomes(*job);
            finishMovie(job->movie, outcomes);
        }
    }

    // Waits until every destination has reported, the job's copy_timeout has
    // passed, or the shutdown grace period ends. Marks the job finalized and
    // returns a snapshot of its outcomes, unfinished ones counted as failed.
    std::vector<CopyOutcome> waitForOutcomes(CopyJob &job) {
        std::unique_lock<std::mutex> jobLock(job.mutex);
        while (job.pendingCount > 0) {
            int64_t effectiveDeadline = job.deadlineMilliseconds;
            const int64_t shutdownDeadline = shutdownDeadlineMilliseconds.load();
            const bool shuttingDown = (shutdownDeadline != NO_DEADLINE_MARKER);
            if (shuttingDown) {
                effectiveDeadline = std::min(effectiveDeadline, shutdownDeadline);
            }
            const int64_t remainingMilliseconds = effectiveDeadline - monotonicMilliseconds();
            if (remainingMilliseconds <= 0) {
                break;
            }
            // Wake at least once a second so a shutdown that starts meanwhile is noticed.
            const int64_t waitMilliseconds = std::min<int64_t>(remainingMilliseconds, COORDINATOR_POLL_MILLISECONDS);
            job.outcomeChanged.wait_for(jobLock, std::chrono::milliseconds(waitMilliseconds));
        }
        job.finalized = true;
        std::vector<CopyOutcome> outcomes = job.outcomes;
        jobLock.unlock();

        for (CopyOutcome &outcome : outcomes) {
            if (!outcome.finished) {
                outcome.failureReason = "timed out after " + std::to_string(job.movie.copy.timeoutSeconds) +
                                        " s (destination not responding)";
            }
        }
        return outcomes;
    }

    // Applies the fallback, removal and on_movie_end steps for one movie.
    void finishMovie(const SavedMovie &movie, const std::vector<CopyOutcome> &outcomes) {
        setLogContext(movie.cameraName);
        std::vector<std::string> failureDescriptions;
        std::string firstStoredCopy;
        for (const CopyOutcome &outcome : outcomes) {
            if (outcome.succeeded) {
                if (firstStoredCopy.empty()) {
                    firstStoredCopy = outcome.copiedPath;
                }
            } else {
                failureDescriptions.push_back(outcome.destination + " " + outcome.failureReason);
            }
        }
        const size_t destinationCount = outcomes.size();
        const size_t failureCount = failureDescriptions.size();
        const bool hasDestinations = (destinationCount > 0);
        const bool allCopied = (failureCount == 0);

        bool fallbackStored = false;
        if (hasDestinations && allCopied) {
            logMessage(LogLevel::INFO, "copied %s to %zu destination(s)", movie.relativePath.c_str(), destinationCount);
        }
        if (!allCopied) {
            std::string failureList;
            for (const std::string &description : failureDescriptions) {
                if (!failureList.empty()) {
                    failureList += "; ";
                }
                failureList += description;
            }
            logMessage(LogLevel::WARNING, "%zu of %zu destination(s) not writable for %s: %s", failureCount,
                       destinationCount, movie.relativePath.c_str(), failureList.c_str());
            fallbackStored = storeInFallback(movie, firstStoredCopy);
        }

        // temp_dir only holds a movie until it is safely stored elsewhere:
        // every destination succeeded, or the fallback copy did. Otherwise it
        // stays, so a movie is never deleted without a copy existing.
        std::string pathForCommand = movie.localPath;
        const bool storedElsewhere = (hasDestinations && (allCopied || fallbackStored));
        if (storedElsewhere) {
            const int unlinkResult = unlink(movie.localPath.c_str());
            if (unlinkResult == 0) {
                pathForCommand = firstStoredCopy;
            } else {
                const int unlinkErrno = errno;
                logMessage(LogLevel::WARNING, "cannot remove %s: %s", movie.localPath.c_str(),
                           describeErrno(unlinkErrno).c_str());
            }
        }
        runMovieEndCommand(movie.onMovieEndCommand, pathForCommand);
        setLogContext(COPY_COORDINATOR_LOG_CONTEXT);
    }

    // Writes the movie to fallback_dir (if configured). Like a copy_to folder,
    // fallback_dir must already exist and is never created, so a USB drive that
    // is not mounted is reported instead of its empty mount point being filled.
    // Updates `firstStoredCopy` when nothing else holds a copy yet.
    bool storeInFallback(const SavedMovie &movie, std::string &firstStoredCopy) {
        const std::string &fallbackDirectory = movie.copy.fallbackDirectory;
        const bool fallbackConfigured = !fallbackDirectory.empty();
        if (!fallbackConfigured) {
            logMessage(LogLevel::WARNING, "no fallback_dir set; movie only kept at %s", movie.localPath.c_str());
            return false;
        }
        std::string fallbackPath;
        std::string failureReason;
        const bool copied = copyMovieToDirectory(movie.localPath, fallbackDirectory, movie.relativePath,
                                                 fallbackPath, failureReason);
        if (!copied) {
            logMessage(LogLevel::ERROR, "fallback_dir %s %s; movie kept at %s", fallbackDirectory.c_str(),
                       failureReason.c_str(), movie.localPath.c_str());
            return false;
        }
        logMessage(LogLevel::WARNING, "saved fallback copy %s", fallbackPath.c_str());
        if (firstStoredCopy.empty()) {
            firstStoredCopy = fallbackPath;
        }
        return true;
    }

    std::map<std::string, std::unique_ptr<DestinationWorker>> workersByPath;
    std::thread coordinatorThread;
    std::mutex coordinatorMutex;
    std::condition_variable coordinatorWakeup;
    std::deque<std::shared_ptr<CopyJob>> finishedMovies;
    bool coordinatorStopping = false;
    // Monotonic end of the shutdown grace period, or NO_DEADLINE_MARKER while
    // running. Atomic: set by main(), read by the coordinator thread.
    std::atomic<int64_t> shutdownDeadlineMilliseconds{NO_DEADLINE_MARKER};
};

// ============================================================================
// Event recording
// ============================================================================

// The event state machine of one camera. Keeps the pre-capture buffer filled,
// starts a movie when motion is detected, continues it while motion goes on,
// splits it at a keyframe after movie_max_time, ends it after event_gap
// without motion, and hands each finished movie to the MovieSaveHandler.
class EventRecorder {
public:
    // `source` and `saveHandler` must outlive this object.
    EventRecorder(const CameraConfig &cameraConfig, std::unique_ptr<MovieWriter> movieWriter,
                  const StreamDescription &source, MovieSaveHandler &movieSaveHandler)
        : settings(cameraConfig.events), copySettings(cameraConfig.copy), cameraName(cameraConfig.name),
          writer(std::move(movieWriter)), sourceDescription(source), saveHandler(movieSaveHandler) {}

    ~EventRecorder() {
        endEvent(lastPacketSeconds);
    }
    EventRecorder(const EventRecorder &) = delete;
    EventRecorder &operator=(const EventRecorder &) = delete;

    // Call for every video packet, before it is analysed.
    void handlePacket(const AVPacket *packet, double streamSeconds) {
        lastPacketSeconds = streamSeconds;
        const bool isKeyframe = packetIsKeyframe(packet);
        const bool readyToSplit = (recording && splitPending && isKeyframe);
        if (readyToSplit) {
            endEvent(streamSeconds);
            preCapture.clear();  // already in the previous file
            beginEvent(streamSeconds);
        }

        preCapture.push(packet, streamSeconds, settings.preCaptureSeconds);
        if (recording) {
            writer->consumePacket(packet);
            const bool writerFailed = writer->hasFailed();
            if (writerFailed) {
                logMessage(LogLevel::ERROR, "recording failed, closing it");
                endEvent(streamSeconds);
                retryAfterSeconds = streamSeconds + RECORDING_RETRY_DELAY_SECONDS;
            }
        }
    }

    // Call after each packet has been analysed.
    void handleDetection(bool motionDetected, double streamSeconds) {
        if (motionDetected) {
            if (!recording) {
                logMessage(LogLevel::INFO, "motion detected");
            }
            lastMotionSeconds = streamSeconds;
            const bool retryAllowed = (streamSeconds >= retryAfterSeconds);
            if (!recording && retryAllowed) {
                const bool started = beginEvent(streamSeconds);
                if (!started) {
                    retryAfterSeconds = streamSeconds + RECORDING_RETRY_DELAY_SECONDS;
                }
            }
        }
        if (!recording) {
            return;
        }

        const double secondsWithoutMotion = streamSeconds - lastMotionSeconds;
        const bool eventOver = (secondsWithoutMotion > settings.eventGapSeconds);
        if (eventOver) {
            endEvent(streamSeconds);
            return;
        }
        const bool lengthLimited = (settings.movieMaxSeconds > 0.0);
        const double movieSeconds = streamSeconds - movieStartSeconds;
        const bool movieFull = (movieSeconds >= settings.movieMaxSeconds);
        if (lengthLimited && movieFull && !splitPending) {
            splitPending = true;  // switch files at the next keyframe
        }
    }

    // Closes the current movie, if any, and hands it to the save handler.
    void endEvent(double streamSeconds) {
        if (!recording) {
            return;
        }
        recording = false;
        splitPending = false;
        const bool fileHasVideo = writer->finish();
        if (!fileHasVideo) {
            logMessage(LogLevel::WARNING, "discarded empty recording %s", currentMovie.fullPath.c_str());
            return;
        }
        const double recordedSeconds = streamSeconds - movieStartSeconds;
        logMessage(LogLevel::INFO, "saved %s (%.0f s + pre-capture)", currentMovie.fullPath.c_str(), recordedSeconds);
        SavedMovie savedMovie;
        savedMovie.cameraName = cameraName;
        savedMovie.localPath = currentMovie.fullPath;
        savedMovie.relativePath = currentMovie.relativePath;
        savedMovie.copy = copySettings;
        savedMovie.onMovieEndCommand = settings.onMovieEndCommand;
        saveHandler.movieSaved(savedMovie);
    }

private:
    // Opens a new movie and writes the pre-capture packets into it.
    bool beginEvent(double streamSeconds) {
        MovieLocation newMovie;
        const bool reserved = reserveMovieFile(settings.temporaryDirectory, newMovie);
        if (!reserved) {
            return false;
        }
        const bool started = writer->start(newMovie.fullPath, sourceDescription);
        if (!started) {
            return false;
        }
        currentMovie = newMovie;
        logMessage(LogLevel::INFO, "recording %s", currentMovie.fullPath.c_str());
        preCapture.drainInto(*writer);
        recording = true;
        splitPending = false;
        movieStartSeconds = streamSeconds;
        return true;
    }

    EventSettings settings;
    CopySettings copySettings;
    std::string cameraName;
    std::unique_ptr<MovieWriter> writer;
    const StreamDescription &sourceDescription;
    MovieSaveHandler &saveHandler;
    PreCaptureBuffer preCapture;
    MovieLocation currentMovie;
    bool recording = false;
    bool splitPending = false;
    double lastMotionSeconds = 0.0;
    double movieStartSeconds = 0.0;
    double retryAfterSeconds = 0.0;
    double lastPacketSeconds = 0.0;
};

// ============================================================================
// RTSP source
// ============================================================================

// Opens the RTSP stream and reads its H.264 packets. Every blocking network
// call is abandoned when its deadline passes or a stop is requested.
class StreamSource {
public:
    explicit StreamSource(const SourceSettings &sourceSettings) : settings(sourceSettings) {}
    ~StreamSource() {
        close();
    }
    StreamSource(const StreamSource &) = delete;
    StreamSource &operator=(const StreamSource &) = delete;

    // Connects and finds the H.264 video stream. False if anything fails.
    bool open() {
        close();
        formatContext = avformat_alloc_context();
        if (formatContext == nullptr) {
            return false;
        }
        formatContext->interrupt_callback.callback = &StreamSource::interruptCallback;
        formatContext->interrupt_callback.opaque = this;

        AVDictionary *inputOptions = nullptr;
        const bool isRtspUrl = (settings.url.rfind("rtsp", 0) == 0);
        if (isRtspUrl) {
            const long long timeoutMicroseconds = settings.ioTimeoutSeconds * MICROSECONDS_PER_SECOND;
            const std::string timeoutText = std::to_string(timeoutMicroseconds);
            av_dict_set(&inputOptions, "rtsp_transport", settings.transport.c_str(), 0);
            av_dict_set(&inputOptions, "timeout", timeoutText.c_str(), 0);
            av_dict_set(&inputOptions, "allowed_media_types", "video", 0);  // don't set up audio
        }
        av_dict_set(&inputOptions, "probesize", PROBE_SIZE_BYTES, 0);
        av_dict_set(&inputOptions, "analyzeduration", ANALYZE_DURATION_MICROSECONDS, 0);

        const int openTimeoutSeconds = settings.ioTimeoutSeconds * OPEN_TIMEOUT_MULTIPLIER;
        armTimeout(openTimeoutSeconds);
        const int openResult = avformat_open_input(&formatContext, settings.url.c_str(), nullptr, &inputOptions);
        av_dict_free(&inputOptions);
        if (openResult < 0) {
            // avformat_open_input has already freed formatContext and set it to nullptr.
            logMessage(LogLevel::WARNING, "cannot open %s: %s", settings.url.c_str(), describeError(openResult).c_str());
            return false;
        }
        const int infoResult = avformat_find_stream_info(formatContext, nullptr);
        if (infoResult < 0) {
            logMessage(LogLevel::WARNING, "no stream info from %s: %s", settings.url.c_str(),
                       describeError(infoResult).c_str());
            close();
            return false;
        }

        const bool foundVideo = selectVideoStream();
        if (!foundVideo) {
            logMessage(LogLevel::ERROR, "%s has no H.264 video stream", settings.url.c_str());
            close();
            return false;
        }
        measureFrameRate();
        const AVStream *stream = videoStream();
        logMessage(LogLevel::INFO, "connected to %s: %dx%d, %.2f fps", settings.url.c_str(),
                   stream->codecpar->width, stream->codecpar->height, framesPerSecondValue);
        return true;
    }

    // Disconnects (safe to call when not connected).
    void close() {
        if (formatContext != nullptr) {
            avformat_close_input(&formatContext);
        }
        videoIndex = -1;
    }

    // Gives the next blocking call `seconds` to complete.
    void armTimeout(int seconds) {
        const int64_t currentMicroseconds = av_gettime_relative();
        const int64_t allowedMicroseconds = seconds * MICROSECONDS_PER_SECOND;
        const int64_t newDeadline = currentMicroseconds + allowedMicroseconds;
        deadlineMicroseconds.store(newDeadline);
    }

    // Gives the next read the configured io_timeout.
    void armReadTimeout() {
        armTimeout(settings.ioTimeoutSeconds);
    }

    // Reads the next packet of any stream (check stream_index).
    int readPacket(AVPacket *packet) {
        return av_read_frame(formatContext, packet);
    }

    const AVStream *videoStream() const {
        return formatContext->streams[videoIndex];
    }
    int videoStreamIndex() const {
        return videoIndex;
    }
    double framesPerSecond() const {
        return framesPerSecondValue;
    }

private:
    // Finds the best video stream, requires H.264, and discards all others.
    bool selectVideoStream() {
        const int bestIndex = av_find_best_stream(formatContext, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (bestIndex < 0) {
            return false;
        }
        const AVCodecID codecId = formatContext->streams[bestIndex]->codecpar->codec_id;
        if (codecId != AV_CODEC_ID_H264) {
            return false;
        }
        videoIndex = bestIndex;
        const unsigned streamCount = formatContext->nb_streams;
        for (unsigned streamIndex = 0; streamIndex < streamCount; ++streamIndex) {
            const bool isSelected = (static_cast<int>(streamIndex) == videoIndex);
            if (!isSelected) {
                formatContext->streams[streamIndex]->discard = AVDISCARD_ALL;
            }
        }
        return true;
    }

    // Takes the stream's frame rate, or FALLBACK_FRAMES_PER_SECOND if implausible.
    void measureFrameRate() {
        AVStream *stream = formatContext->streams[videoIndex];
        const AVRational guessedRate = av_guess_frame_rate(formatContext, stream, nullptr);
        double measuredRate = 0.0;
        const bool rateValid = (guessedRate.num > 0 && guessedRate.den > 0);
        if (rateValid) {
            measuredRate = av_q2d(guessedRate);
        }
        const bool tooLow = (measuredRate < MIN_PLAUSIBLE_FRAMES_PER_SECOND);
        const bool tooHigh = (measuredRate > MAX_PLAUSIBLE_FRAMES_PER_SECOND);
        if (tooLow || tooHigh) {
            measuredRate = FALLBACK_FRAMES_PER_SECOND;
        }
        framesPerSecondValue = measuredRate;
    }

    // Called by FFmpeg during blocking I/O, possibly from one of its worker
    // threads (e.g. the UDP receive thread). Non-zero aborts the operation.
    static int interruptCallback(void *opaque) {
        const StreamSource *source = static_cast<const StreamSource *>(opaque);
        const bool stopping = stopRequested.load();
        const int64_t currentMicroseconds = av_gettime_relative();
        const int64_t deadline = source->deadlineMicroseconds.load();
        const bool deadlinePassed = (currentMicroseconds > deadline);
        const bool shouldAbort = (stopping || deadlinePassed);
        return static_cast<int>(shouldAbort);
    }

    SourceSettings settings;
    AVFormatContext *formatContext = nullptr;
    int videoIndex = -1;
    double framesPerSecondValue = FALLBACK_FRAMES_PER_SECOND;
    // Atomic because the interrupt callback may read it from an FFmpeg thread
    // while the main thread re-arms it; a plain 64-bit value can tear on 32-bit ARM.
    std::atomic<int64_t> deadlineMicroseconds{0};
};

// ============================================================================
// Session, cameras and main
// ============================================================================

// Runs one connected session of one camera: reads packets, records events,
// detects motion. Returns when the stream fails or a stop is requested.
static void runSession(StreamSource &source, const CameraConfig &config, MovieSaveHandler &saveHandler) {
    StreamDescription description;
    const bool described = description.assign(source.videoStream(), source.framesPerSecond());
    if (!described) {
        return;
    }
    H264Decoder detectionDecoder("detection");
    const bool decoderOpened = detectionDecoder.open(description.parameters(), config.decoder);
    if (!decoderOpened) {
        logMessage(LogLevel::ERROR, "no usable H.264 decoder");
        return;
    }
    MotionAnalyzer analyzer(config.detection, description.framesPerSecond());
    std::unique_ptr<MovieWriter> movieWriter = createMovieWriter(config.movie, config.decoder);
    EventRecorder recorder(config, std::move(movieWriter), description, saveHandler);
    StreamClock clock;
    clock.reset(description.timeBase(), description.framesPerSecond());

    AVPacket *packet = av_packet_alloc();
    if (packet == nullptr) {
        return;
    }
    const int videoStreamIndex = source.videoStreamIndex();
    while (!stopRequested.load()) {
        source.armReadTimeout();
        const int readResult = source.readPacket(packet);
        if (readResult < 0) {
            const bool stopping = stopRequested.load();
            if (!stopping) {
                const bool endOfStream = (readResult == AVERROR_EOF);
                if (endOfStream) {
                    logMessage(LogLevel::WARNING, "stream ended");
                } else {
                    logMessage(LogLevel::WARNING, "stream error: %s", describeError(readResult).c_str());
                }
            }
            break;
        }
        const bool isVideoPacket = (packet->stream_index == videoStreamIndex);
        if (!isVideoPacket) {
            av_packet_unref(packet);
            continue;
        }

        const int64_t sourceTimestamp = packetDecodeTimestamp(packet);
        const double streamSeconds = clock.advance(sourceTimestamp);
        recorder.handlePacket(packet, streamSeconds);
        analyzer.beginPacket(streamSeconds);
        const bool decodeSucceeded = detectionDecoder.decode(packet, analyzer);
        av_packet_unref(packet);
        if (!decodeSucceeded) {
            logMessage(LogLevel::ERROR, "detection decoder failed");
            break;
        }
        const bool motionDetected = analyzer.motionSeen();
        recorder.handleDetection(motionDetected, streamSeconds);
    }
    recorder.endEvent(clock.now());
    av_packet_free(&packet);
}

// Runs one camera on its own thread: connects, records and reconnects with
// back-off until a stop is requested. Each runner only touches its own
// objects, apart from the thread-safe MovieSaveHandler.
class CameraRunner {
public:
    CameraRunner(const CameraConfig &cameraConfig, MovieSaveHandler &movieSaveHandler)
        : config(cameraConfig), saveHandler(movieSaveHandler) {}
    CameraRunner(const CameraRunner &) = delete;
    CameraRunner &operator=(const CameraRunner &) = delete;

    // Starts the camera thread.
    void start() {
        cameraThread = std::thread(&CameraRunner::run, this);
    }

    // Waits for the camera thread to finish (after a stop was requested).
    void join() {
        if (cameraThread.joinable()) {
            cameraThread.join();
        }
    }

private:
    // Thread body: the connect / record / reconnect loop.
    void run() {
        setLogContext(config.name);
        const char *recordingDescription = "copied";
        if (config.movie.recordMode == RecordMode::ENCODE) {
            recordingDescription = "re-encoded";
        }
        logMessage(LogLevel::INFO, "%s: threshold %.2f%%, noise %d, %s recording to %s, %zu copy destination(s)",
                   config.configPath.c_str(), config.detection.thresholdPercent, config.detection.noiseLevel,
                   recordingDescription, config.events.temporaryDirectory.c_str(), config.copy.destinations.size());

        int reconnectDelaySeconds = 1;
        while (!stopRequested.load()) {
            StreamSource source(config.source);
            const bool connected = source.open();
            if (!connected) {
                sleepInterruptibly(reconnectDelaySeconds);
                const int doubledDelay = reconnectDelaySeconds * 2;
                reconnectDelaySeconds = std::min(doubledDelay, MAX_RECONNECT_DELAY_SECONDS);
                continue;
            }
            reconnectDelaySeconds = 1;
            runSession(source, config, saveHandler);
            source.close();
            const bool stopping = stopRequested.load();
            if (!stopping) {
                sleepInterruptibly(RECONNECT_PAUSE_SECONDS);
            }
        }
        logMessage(LogLevel::INFO, "camera stopped");
    }

    CameraConfig config;
    MovieSaveHandler &saveHandler;
    std::thread cameraThread;
};

// SIGINT/SIGTERM handler: only sets the lock-free stop flag (signal-safe).
static void handleStopSignal(int) {
    stopRequested.store(true);
}

// Installs the stop handler and ignores SIGPIPE and SIGCHLD (on_movie_end
// children are then reaped automatically). SA_RESTART lets file and network
// calls in other threads continue instead of failing with EINTR; every
// blocking loop still notices the stop flag within a fraction of a second.
static void installSignalHandlers() {
    struct sigaction stopAction;
    memset(&stopAction, 0, sizeof stopAction);
    stopAction.sa_handler = handleStopSignal;
    stopAction.sa_flags = SA_RESTART;
    sigemptyset(&stopAction.sa_mask);
    sigaction(SIGINT, &stopAction, nullptr);
    sigaction(SIGTERM, &stopAction, nullptr);

    struct sigaction ignoreAction;
    memset(&ignoreAction, 0, sizeof ignoreAction);
    ignoreAction.sa_handler = SIG_IGN;
    sigemptyset(&ignoreAction.sa_mask);
    sigaction(SIGPIPE, &ignoreAction, nullptr);
    sigaction(SIGCHLD, &ignoreAction, nullptr);
}

// Prints the command-line usage.
static void printUsage(const char *programName) {
    fprintf(stderr, "usage: %s [-c config] [-v]\n", programName);
}

// Creates every camera's temp_dir up front (it is usually inside a RAM disk
// that starts empty at boot), so a problem is reported at start-up rather than
// at the first event. fallback_dir and copy_to folders are deliberately not
// created, because they must be on a mounted drive or share; fallback_dir is
// only checked here, and a missing one is a warning, not an error.
static bool prepareCameraFolders(const std::vector<CameraConfig> &cameras) {
    bool allReady = true;
    for (const CameraConfig &camera : cameras) {
        const std::string &temporaryDirectory = camera.events.temporaryDirectory;
        int folderErrno = 0;
        const bool temporaryReady = createDirectoryTree(temporaryDirectory, folderErrno);
        if (!temporaryReady) {
            logMessage(LogLevel::ERROR, "camera %s: temp_dir %s is not writable: %s", camera.name.c_str(),
                       temporaryDirectory.c_str(), describeErrno(folderErrno).c_str());
            allReady = false;
        }

        const std::string &fallbackDirectory = camera.copy.fallbackDirectory;
        const bool hasFallback = !fallbackDirectory.empty();
        if (hasFallback) {
            std::string failureReason;
            const bool fallbackAvailable = checkDestinationAvailable(fallbackDirectory, failureReason);
            if (!fallbackAvailable) {
                logMessage(LogLevel::WARNING, "camera %s: fallback_dir %s is %s", camera.name.c_str(),
                           fallbackDirectory.c_str(), failureReason.c_str());
            }
        }
    }
    return allReady;
}

// Switches cameras that asked for record_mode=encode to copy mode if the
// hardware encoder cannot be opened. The encoder is probed at most once.
static void applyEncoderAvailability(std::vector<CameraConfig> &cameras) {
    bool encoderProbed = false;
    bool encoderAvailable = false;
    for (CameraConfig &camera : cameras) {
        const bool wantsEncode = (camera.movie.recordMode == RecordMode::ENCODE);
        if (!wantsEncode) {
            continue;
        }
        if (!encoderProbed) {
            encoderAvailable = hardwareEncoderAvailable();
            encoderProbed = true;
        }
        if (!encoderAvailable) {
            logMessage(LogLevel::WARNING, "camera %s: hardware H.264 encoder unavailable, using record_mode=copy",
                       camera.name.c_str());
            camera.movie.recordMode = RecordMode::COPY;
        }
    }
}

int main(int argc, char **argv) {
    std::string configPath = DEFAULT_CONFIG_PATH;
    bool configPathGiven = false;
    bool verboseLogging = false;
    int option = 0;
    while ((option = getopt(argc, argv, "c:vh")) != -1) {
        if (option == 'c') {
            configPath = optarg;
            configPathGiven = true;
        } else if (option == 'v') {
            verboseLogging = true;
        } else if (option == 'h') {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    GlobalSettings global;
    std::vector<CameraConfig> cameras;
    const bool configLoaded = loadAllConfiguration(configPath, configPathGiven, global, cameras);
    if (!configLoaded) {
        return 1;
    }
    if (verboseLogging) {
        activeLogLevel = static_cast<int>(LogLevel::DEBUG);
    } else {
        activeLogLevel = global.logLevel;
    }
    const bool debugLogging = (activeLogLevel >= static_cast<int>(LogLevel::DEBUG));
    if (debugLogging) {
        av_log_set_level(AV_LOG_WARNING);
    } else {
        av_log_set_level(AV_LOG_ERROR);
    }
    tzset();

    const bool camerasValid = validateCameras(cameras);
    if (!camerasValid) {
        return 1;
    }
    const bool foldersReady = prepareCameraFolders(cameras);
    if (!foldersReady) {
        return 1;
    }
    installSignalHandlers();
    applyEncoderAvailability(cameras);
    avformat_network_init();

    // Everything the threads share is set up before the first thread starts.
    MovieCopyService copyService(cameras);
    copyService.start();
    std::vector<std::unique_ptr<CameraRunner>> runners;
    for (const CameraConfig &camera : cameras) {
        std::unique_ptr<CameraRunner> runner = std::make_unique<CameraRunner>(camera, copyService);
        runner->start();
        runners.push_back(std::move(runner));
    }
    logMessage(LogLevel::INFO, "running %zu camera(s)", runners.size());

    // Camera threads exit by themselves once the stop flag is set; each one
    // closes its current movie and hands it to the copy service first.
    for (std::unique_ptr<CameraRunner> &runner : runners) {
        runner->join();
    }
    const bool copiesFinished = copyService.shutdown(COPY_SHUTDOWN_GRACE_SECONDS);
    avformat_network_deinit();
    if (!copiesFinished) {
        logMessage(LogLevel::WARNING, "exiting while a copy is still blocked; movies it did not finish are in the fallback or local folder");
        fflush(stderr);
        // A blocked worker thread cannot be joined; leave without running
        // static destructors underneath it.
        std::_Exit(0);
    }
    logMessage(LogLevel::INFO, "stopped");
    return 0;
}
