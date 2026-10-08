// rtspcam.cpp
// One-process replacement for imx462.sh (rpicam-vid | ffmpeg) + rtsprelay.
// Captures the IMX462 with libcamera, encodes H.264 on the Pi's hardware
// encoder and serves it as RTSP to the whole local network, all inside a
// single GStreamer pipeline:
//
//   libcamerasrc (sensor mode, ISP scales to WxH)
//     -> v4l2h264enc (hardware) -> [orientation tag] -> rtph264pay -> RTSP
//
// No pipes, no mux/demux, no parser, no queues, no extra processes.
// The camera only runs while at least one client is connected.
//
// Usage: rtspcam [options]
//   -w <width>     output width            (default 1280)
//   -h <height>    output height           (default 720)
//   -f <fps>       frame rate              (default 15)
//   -b <bitrate>   H.264 bitrate in bit/s  (default 2000000)
//   -i <frames>    keyframe interval       (default 15)
//   -s <WxH:bits>  sensor mode             (default 1920x1080:10)
//   -p <port>      RTSP port               (default 8554)
//   -m <mount>     RTSP path               (default /cam)
//   -r <degrees>   display rotation: 0, 90 (clockwise, portrait) or
//                  270 (anticlockwise, portrait)          (default 0)
//   -c <port>      day/night control HTTP port, on 127.0.0.1 only
//                                                         (default 8556)
//
// Rotation: the Pi's sensor, ISP and encoder cannot turn an image by 90
// degrees, and doing it on the CPU would mean rewriting every frame. Instead
// each frame carries a tiny H.264 "display orientation" message (SEI, about
// 8 bytes) telling the player to show it rotated. The pixels are untouched,
// so this costs no CPU or memory. FFmpeg-based players (mpv, ffplay) honour
// it; players that ignore it show the image unrotated.
//
// Night mode: with the IR-cut filter out, the IR LEDs reach all three colour
// channels and the colour matrix turns that into a magenta cast. In night
// mode the ISP is told to output greyscale (plus a small contrast and
// sharpening change); in day mode those controls are left at libcamera's
// defaults, so no correction is applied. ldrcmd.sh switches it with curl:
//   GET http://127.0.0.1:8556/mode               -> "night" or "day"
//   GET http://127.0.0.1:8556/mode?state=night   -> switch to night
//   GET http://127.0.0.1:8556/mode?state=day     -> switch to day
// The mode is saved to /run/rtspcam.mode and read back at startup, so it
// survives a restart of rtspcam. The change is made by the ISP on the next
// frame: no CPU work per frame, no pipeline or process restart, and
// greyscale frames leave the encoder's chroma idle.
//
// libcamerasrc can silently drop a control change: it moves pending controls
// into a new capture request before checking that a buffer is free, and
// throws the request (and the controls) away when none is. To make every
// switch land first time, the controls are set from libcamerasrc's own
// streaming thread, inside a probe on its output, and that one frame is
// dropped: the dropped buffer goes straight back to libcamerasrc's pool, so
// the very next request it builds is sure to get a buffer and carries the
// new controls. Cost: one skipped frame (66 ms at 15 fps) per switch.
//
// Result: rtsp://<pi-address>:<port><mount>  (also rtsp://127.0.0.1:...)
// Needs libcamerasrc with the sensor-config property
// (gstreamer1.0-libcamera 0.7.1+rpt20260429 or newer).
//
// Conventions: camelCase for variables and functions, UPPER_CASE for
// constants, a comment above every function, struct and class, and
// calculations split into named steps rather than written inline.
//
// Build (one line):
//   g++ -O2 -s -o rtspcam rtspcam.cpp $(pkg-config --cflags --libs
//       gstreamer-rtsp-server-1.0 gstreamer-rtsp-1.0 gstreamer-1.0)

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib-unix.h>

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <mutex>
#include <unistd.h>
#include <vector>

// ---- Fixed settings --------------------------------------------------------

// Address the RTSP server binds to. "0.0.0.0" serves every IPv4 interface,
// so any device on the local network can view the stream. Set to
// "127.0.0.1" to restrict it to this Pi only.
static constexpr const char *LISTEN_ADDRESS = "0.0.0.0";
static constexpr const char *ALL_INTERFACES_ADDRESS = "0.0.0.0";

// Placeholder host shown when the Pi's addresses can't be listed.
static constexpr const char *UNKNOWN_HOST_TEXT = "<pi-address>";

// How often sessions of clients that vanished without a TEARDOWN are reaped,
// so a crashed viewer doesn't keep the camera and encoder running.
static constexpr guint SESSION_CLEANUP_SECONDS = 10;

// Limits of the Pi's H.264 hardware encoder at level 4.
static constexpr long MIN_FRAME_DIMENSION = 16;
static constexpr long MAX_FRAME_DIMENSION = 1920;
static constexpr long MIN_BITRATE = 10000;
static constexpr long MAX_BITRATE = 25000000;

// Accepted ranges of the other numeric options.
static constexpr long MIN_FRAMES_PER_SECOND = 1;
static constexpr long MAX_FRAMES_PER_SECOND = 120;
static constexpr long MIN_KEYFRAME_INTERVAL = 1;
static constexpr long MAX_KEYFRAME_INTERVAL = 1000;
static constexpr long MIN_PORT = 1;
static constexpr long MAX_PORT = 65535;
static constexpr long MIN_ROTATION_DEGREES = 0;
static constexpr long MAX_ROTATION_DEGREES = 359;

// Display rotations the orientation message supports.
static constexpr long NO_ROTATION_DEGREES = 0;
static constexpr long QUARTER_TURN_CLOCKWISE_DEGREES = 90;
static constexpr long QUARTER_TURN_ANTICLOCKWISE_DEGREES = 270;

// Accepted sensor mode values (-s WxH:bits).
static constexpr long MIN_SENSOR_DIMENSION = 1;
static constexpr long MAX_SENSOR_DIMENSION = 8192;
static constexpr long MIN_SENSOR_BIT_DEPTH = 8;
static constexpr long MAX_SENSOR_BIT_DEPTH = 16;
static constexpr int SENSOR_MODE_FIELD_COUNT = 3;   // width, height and bit depth

// getopt() option string and its end-of-options return value.
static constexpr const char *OPTION_LETTERS = "w:h:f:b:i:s:p:m:r:c:";
static constexpr int END_OF_OPTIONS = -1;

// Characters that would break an RTSP URL if they appeared in the mount path.
static constexpr const char *MOUNT_PATH_FORBIDDEN_CHARACTERS = " \t\r\n?#";

// Base for parsing decimal numbers.
static constexpr int DECIMAL_BASE = 10;

// Signals that trigger a clean shutdown.
static constexpr int SHUTDOWN_SIGNALS[] = {SIGTERM, SIGINT, SIGHUP};

// H.264 constants used to build the display orientation message.
static constexpr guint8 ANNEX_B_START_CODE[] = {0x00, 0x00, 0x00, 0x01};
static constexpr guint8 SEI_NAL_HEADER = 0x06;                 // nal_unit_type 6 (SEI)
static constexpr guint8 DISPLAY_ORIENTATION_PAYLOAD_TYPE = 47; // H.264 Annex D.1.27
static constexpr guint8 RBSP_STOP_BITS = 0x80;                 // rbsp_trailing_bits
static constexpr long FULL_TURN_ROTATION_UNITS = 65536;        // rotation units per 360 degrees
static constexpr long DEGREES_PER_TURN = 360;
static constexpr int FLAG_FIELD_BITS = 1;                      // a one-bit syntax element
static constexpr int ROTATION_FIELD_BITS = 16;                 // anticlockwise_rotation
static constexpr guint32 FLAG_CLEAR = 0;
static constexpr guint32 REPETITION_THIS_FRAME_ONLY = 0;       // display_orientation_repetition_period
static constexpr guint8 EMULATION_PREVENTION_BYTE = 0x03;
static constexpr guint8 LARGEST_START_CODE_LIKE_BYTE = 0x03;   // 00 00 0x (x <= 3) needs protecting
static constexpr int ZERO_BYTES_BEFORE_PROTECTION = 2;
static constexpr int BITS_PER_BYTE = 8;
static constexpr guint8 HIGHEST_BIT_IN_BYTE = 0x80;

// Name gst-rtsp-server requires for the payloader of the first stream.
static constexpr const char *PAYLOADER_NAME = "pay0";

// Prefix on every log line, so messages are easy to find in the journal.
static constexpr const char *LOG_PREFIX = "rtspcam: ";

// Where rtspcam saves the current light mode ("night" or "day") so it
// survives a restart; a missing file or any other text means day. /run is
// tmpfs, so this needs no writes to the read-only root filesystem.
static constexpr const char *LIGHT_MODE_FILE = "/run/rtspcam.mode";
static constexpr const char *NIGHT_MODE_TEXT = "night";
static constexpr const char *DAY_MODE_TEXT = "day";
static constexpr const char *NIGHT_MODE_DESCRIPTION = "night (greyscale)";
static constexpr const char *DAY_MODE_DESCRIPTION = "day (no correction)";

// Day/night control HTTP server. Bound to loopback only: it is meant for
// ldrcmd.sh on this Pi, not for the network.
static constexpr const char *CONTROL_LISTEN_ADDRESS = "127.0.0.1";
static constexpr const char *CONTROL_PATH = "/mode";              // the only URL path served
static constexpr const char *CONTROL_STATE_PARAMETER = "state";   // ?state=night|day
static constexpr gint CONTROL_WORKER_THREADS = 1;                 // one request at a time; they are tiny and rare
static constexpr guint CONTROL_TIMEOUT_SECONDS = 2;               // per read/write on a connection
static constexpr gsize CONTROL_MAX_REQUEST_BYTES = 1024;          // request line + headers

// Name of the libcamerasrc element in the pipeline, used to find it when
// changing its controls, and the pad names the probes attach to.
static constexpr const char *CAMERA_ELEMENT_NAME = "camera";
static constexpr const char *SOURCE_PAD_NAME = "src";
static constexpr const char *SINK_PAD_NAME = "sink";

// Log messages for the camera starting and stopping.
static constexpr const char *CAMERA_STREAMING_MESSAGE = "camera streaming";
static constexpr const char *CAMERA_RELEASED_MESSAGE = "camera released";

// Text used when a client's address or an error's details are unavailable.
static constexpr const char *UNKNOWN_TEXT = "?";
static constexpr const char *UNKNOWN_ERROR_TEXT = "unknown error";

// ---- Configuration ---------------------------------------------------------

// Stream and server settings, filled in from the command-line options.
struct Config {
    long width = 1280;
    long height = 720;
    long framesPerSecond = 15;
    long bitrate = 2000000; // high quality
    long keyframeInterval = 15;
    long sensorWidth = 1920;
    long sensorHeight = 1080;
    long sensorBitDepth = 10;
    long port = 8554;
    const char *mountPath = "/cam";
    long rotationDegrees = 0;   // clockwise display rotation: 0, 90 or 270
    long controlPort = 8556;    // day/night control HTTP port (loopback only)
};

// Describes one numeric command-line option: its letter, the Config field
// it sets and the range of values it accepts.
struct NumericOption {
    char letter;
    long Config::*field;
    long minimum;
    long maximum;
};

// All numeric options, so they share one parsing and validation path.
static constexpr NumericOption NUMERIC_OPTIONS[] = {
    {'w', &Config::width,            MIN_FRAME_DIMENSION,   MAX_FRAME_DIMENSION},
    {'h', &Config::height,           MIN_FRAME_DIMENSION,   MAX_FRAME_DIMENSION},
    {'f', &Config::framesPerSecond,  MIN_FRAMES_PER_SECOND, MAX_FRAMES_PER_SECOND},
    {'b', &Config::bitrate,          MIN_BITRATE,           MAX_BITRATE},
    {'i', &Config::keyframeInterval, MIN_KEYFRAME_INTERVAL, MAX_KEYFRAME_INTERVAL},
    {'p', &Config::port,             MIN_PORT,              MAX_PORT},
    {'r', &Config::rotationDegrees,  MIN_ROTATION_DEGREES,  MAX_ROTATION_DEGREES},
    {'c', &Config::controlPort,      MIN_PORT,              MAX_PORT},
};

// Option letters handled outside NUMERIC_OPTIONS.
static constexpr int SENSOR_MODE_OPTION = 's';
static constexpr int MOUNT_PATH_OPTION = 'm';

// A GStreamer element the pipeline needs, plus a property it must support
// (nullptr when any version of the element will do).
struct RequiredElement {
    const char *name;
    const char *requiredProperty;
};

// Every element used in the pipeline, checked once at startup.
static constexpr const char *CAMERA_ELEMENT_TYPE = "libcamerasrc";
static constexpr RequiredElement REQUIRED_ELEMENTS[] = {
    {CAMERA_ELEMENT_TYPE, "sensor-config"},
    {"v4l2h264enc",       nullptr},
    {"rtph264pay",        nullptr},
};

// One ISP control, set through the libcamerasrc property of the same name,
// with its value in each light mode. Values are strings so one table can
// hold any property type (gst_util_set_object_arg converts them).
struct LightModeControl {
    const char *property;
    const char *dayValue;     // libcamera's neutral default: no correction
    const char *nightValue;
};

// Controls changed for night mode. All are applied inside the ISP on the
// next frame, so they cost no CPU and no memory.
static constexpr LightModeControl LIGHT_MODE_CONTROLS[] = {
    {"saturation", "1.0", "0.0"},     // greyscale: removes the IR magenta cast
    {"contrast",   "1.0", "1.25"},    // IR light flattens tones; stretch around mid-grey
    {"brightness", "0.0", "-0.05"},   // keeps bright walls from clipping after the stretch
    {"sharpness",  "1.0", "0.5"},     // less sharpening of high-gain sensor noise
};

// Main loop, global so the signal handler can stop it. Only touched from
// the main thread.
static GMainLoop *mainLoop = nullptr;

// ---- Logging ---------------------------------------------------------------

// Formats a message and prints it as one prefixed line, to stderr for
// errors and stdout otherwise. Shared by logInfo() and logError().
// g_print/g_printerr are thread-safe, so any thread may log.
static void logLine(bool isError, const char *format, va_list arguments)
{
    gchar *messageText = g_strdup_vprintf(format, arguments);
    if (isError) {
        g_printerr("%s%s\n", LOG_PREFIX, messageText);
    } else {
        g_print("%s%s\n", LOG_PREFIX, messageText);
    }
    g_free(messageText);
}

// Logs an informational message (printf-style).
static void logInfo(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void logInfo(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    logLine(false, format, arguments);
    va_end(arguments);
}

// Logs an error message (printf-style).
static void logError(const char *format, ...) G_GNUC_PRINTF(1, 2);
static void logError(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    logLine(true, format, arguments);
    va_end(arguments);
}

// Returns the message of a GError, or a fallback when there is no error
// object, so a missing error can never be dereferenced.
static const char *errorMessageOrFallback(const GError *error)
{
    if (error == nullptr) {
        return UNKNOWN_ERROR_TEXT;
    }
    return error->message;
}

// ---- Argument parsing ------------------------------------------------------

// Prints the command-line synopsis.
static void printUsage(const char *programName)
{
    std::fprintf(stderr,
        "Usage: %s [-w width] [-h height] [-f fps] [-b bitrate] [-i intra]\n"
        "          [-s WxH:bits] [-p port] [-m mount] [-r 0|90|270] [-c control-port]\n",
        programName);
}

// Returns true when value lies within [minimum, maximum].
static bool isInRange(long value, long minimum, long maximum)
{
    bool isAtLeastMinimum = value >= minimum;
    bool isAtMostMaximum = value <= maximum;
    return isAtLeastMinimum && isAtMostMaximum;
}

// Returns true when value is a multiple of two.
static bool isEven(long value)
{
    long remainder = value % 2;
    return remainder == 0;
}

// Parses a whole string as a decimal number within [minimum, maximum].
// Rejects empty input, trailing characters and overflow.
static bool parseRangedLong(const char *text, long minimum, long maximum, long &result)
{
    if (text == nullptr) {
        return false;
    }
    bool isEmpty = text[0] == '\0';
    if (isEmpty) {
        return false;
    }
    errno = 0;
    char *parseEnd = nullptr;
    long parsedValue = std::strtol(text, &parseEnd, DECIMAL_BASE);
    bool hasOverflowed = errno != 0;
    bool hasTrailingCharacters = *parseEnd != '\0';
    bool isWithinRange = isInRange(parsedValue, minimum, maximum);
    if (hasOverflowed || hasTrailingCharacters || !isWithinRange) {
        return false;
    }
    result = parsedValue;
    return true;
}

// Parses a sensor mode written as WxH:bits (e.g. 1920x1080:10).
static bool parseSensorMode(const char *text, Config &config)
{
    long sensorWidth = 0;
    long sensorHeight = 0;
    long sensorBitDepth = 0;
    int charactersConsumed = 0;
    int fieldsParsed = std::sscanf(text, "%ldx%ld:%ld%n", &sensorWidth, &sensorHeight,
                                   &sensorBitDepth, &charactersConsumed);
    if (fieldsParsed != SENSOR_MODE_FIELD_COUNT) {
        return false;
    }
    bool hasTrailingCharacters = text[charactersConsumed] != '\0';
    if (hasTrailingCharacters) {
        return false;
    }
    bool isWidthValid = isInRange(sensorWidth, MIN_SENSOR_DIMENSION, MAX_SENSOR_DIMENSION);
    bool isHeightValid = isInRange(sensorHeight, MIN_SENSOR_DIMENSION, MAX_SENSOR_DIMENSION);
    bool isBitDepthValid = isInRange(sensorBitDepth, MIN_SENSOR_BIT_DEPTH, MAX_SENSOR_BIT_DEPTH);
    if (!isWidthValid || !isHeightValid || !isBitDepthValid) {
        return false;
    }
    config.sensorWidth = sensorWidth;
    config.sensorHeight = sensorHeight;
    config.sensorBitDepth = sensorBitDepth;
    return true;
}

// Returns the numeric option for a letter, or nullptr if it isn't one.
static const NumericOption *findNumericOption(int optionLetter)
{
    for (const NumericOption &numericOption : NUMERIC_OPTIONS) {
        if (numericOption.letter == optionLetter) {
            return &numericOption;
        }
    }
    return nullptr;
}

// Checks that the mount path looks like "/name" with no characters that
// would break an RTSP URL.
static bool isValidMountPath(const char *mountPath)
{
    bool startsWithSlash = mountPath[0] == '/';
    if (!startsWithSlash) {
        return false;
    }
    bool hasNameAfterSlash = mountPath[1] != '\0';
    const char *forbiddenCharacter = std::strpbrk(mountPath, MOUNT_PATH_FORBIDDEN_CHARACTERS);
    bool hasForbiddenCharacter = forbiddenCharacter != nullptr;
    return hasNameAfterSlash && !hasForbiddenCharacter;
}

// Returns true for the display rotations the orientation message supports.
static bool isSupportedRotation(long rotationDegrees)
{
    bool isNoRotation = rotationDegrees == NO_ROTATION_DEGREES;
    bool isClockwiseQuarterTurn = rotationDegrees == QUARTER_TURN_CLOCKWISE_DEGREES;
    bool isAnticlockwiseQuarterTurn = rotationDegrees == QUARTER_TURN_ANTICLOCKWISE_DEGREES;
    return isNoRotation || isClockwiseQuarterTurn || isAnticlockwiseQuarterTurn;
}

// Parses one option's value into config. Returns false for an unknown
// option (getopt has already reported it), with isValueValid untouched.
static bool parseOptionValue(int optionLetter, const char *optionValue, Config &config,
                             bool &isValueValid)
{
    const NumericOption *numericOption = findNumericOption(optionLetter);
    if (numericOption != nullptr) {
        long &targetField = config.*(numericOption->field);
        isValueValid = parseRangedLong(optionValue, numericOption->minimum,
                                       numericOption->maximum, targetField);
        return true;
    }
    if (optionLetter == SENSOR_MODE_OPTION) {
        isValueValid = parseSensorMode(optionValue, config);
        return true;
    }
    if (optionLetter == MOUNT_PATH_OPTION) {
        config.mountPath = optionValue;
        isValueValid = isValidMountPath(optionValue);
        return true;
    }
    return false;
}

// Checks rules that involve the options together, after all are parsed,
// logging the first one broken.
static bool validateCombinedOptions(const Config &config)
{
    bool isWidthEven = isEven(config.width);
    bool isHeightEven = isEven(config.height);
    if (!isWidthEven || !isHeightEven) {
        logError("width and height must be even");
        return false;
    }
    if (!isSupportedRotation(config.rotationDegrees)) {
        logError("rotation must be 0, 90 or 270");
        return false;
    }
    if (config.controlPort == config.port) {
        logError("control port must differ from the RTSP port");
        return false;
    }
    return true;
}

// Reads all command-line options into config, logging the first problem
// found. Returns false if the options are unusable.
static bool parseArguments(int argc, char *argv[], Config &config)
{
    while (true) {
        int optionLetter = getopt(argc, argv, OPTION_LETTERS);
        if (optionLetter == END_OF_OPTIONS) {
            break;
        }
        bool isValueValid = false;
        bool isKnownOption = parseOptionValue(optionLetter, optarg, config, isValueValid);
        if (!isKnownOption) {
            return false;   // getopt has already reported the problem
        }
        if (!isValueValid) {
            logError("invalid value for -%c: '%s'", optionLetter, optarg);
            return false;
        }
    }
    bool hasLeftoverArguments = optind < argc;
    if (hasLeftoverArguments) {
        logError("unexpected argument '%s'", argv[optind]);
        return false;
    }
    return validateCombinedOptions(config);
}

// ---- Startup checks --------------------------------------------------------

// Returns true when the element type created by factory has a property
// with the given name. Loads the element's plugin to find out.
static bool elementHasProperty(GstElementFactory *factory, const char *propertyName)
{
    GstPluginFeature *loadedFeature = gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory));
    if (loadedFeature == nullptr) {
        return false;
    }
    GstElementFactory *loadedFactory = GST_ELEMENT_FACTORY(loadedFeature);
    GType elementType = gst_element_factory_get_element_type(loadedFactory);
    gpointer elementClass = g_type_class_ref(elementType);
    GParamSpec *propertySpec = g_object_class_find_property(G_OBJECT_CLASS(elementClass), propertyName);
    bool hasProperty = propertySpec != nullptr;
    g_type_class_unref(elementClass);
    gst_object_unref(loadedFeature);
    return hasProperty;
}

// Verifies every element in REQUIRED_ELEMENTS is installed and new enough,
// so a missing package fails at startup instead of on the first client.
static bool checkRequiredElements()
{
    bool allPresent = true;
    for (const RequiredElement &requiredElement : REQUIRED_ELEMENTS) {
        GstElementFactory *factory = gst_element_factory_find(requiredElement.name);
        if (factory == nullptr) {
            logError("GStreamer element '%s' not installed", requiredElement.name);
            allPresent = false;
            continue;
        }
        bool needsProperty = requiredElement.requiredProperty != nullptr;
        if (needsProperty) {
            bool hasProperty = elementHasProperty(factory, requiredElement.requiredProperty);
            if (!hasProperty) {
                logError("'%s' lacks the '%s' property (package too old)",
                         requiredElement.name, requiredElement.requiredProperty);
                allPresent = false;
            }
        }
        gst_object_unref(factory);
    }
    return allPresent;
}

// Verifies libcamerasrc exposes every control in LIGHT_MODE_CONTROLS as a
// property, so a typo or an old package fails at startup rather than when
// night falls. Call after checkRequiredElements() has found libcamerasrc.
static bool checkLightModeControls()
{
    GstElementFactory *cameraFactory = gst_element_factory_find(CAMERA_ELEMENT_TYPE);
    if (cameraFactory == nullptr) {
        return false;
    }
    bool allPresent = true;
    for (const LightModeControl &lightModeControl : LIGHT_MODE_CONTROLS) {
        bool hasProperty = elementHasProperty(cameraFactory, lightModeControl.property);
        if (!hasProperty) {
            logError("'libcamerasrc' lacks the '%s' control (needed for night mode)",
                     lightModeControl.property);
            allPresent = false;
        }
    }
    gst_object_unref(cameraFactory);
    return allPresent;
}

// ---- Pipeline element helpers -----------------------------------------------

// Finds the element called elementName inside bin and returns a new
// reference to it, or nullptr if there is none. The caller unrefs it.
static GstElement *findElementInBin(GstElement *bin, const char *elementName)
{
    GstElement *foundElement = gst_bin_get_by_name(GST_BIN(bin), elementName);
    return foundElement;
}

// Adds a buffer probe to the pad called padName of element. probeData is
// handed to the probe and released with releaseProbeData when the probe is
// removed (with its pad); if the pad doesn't exist it is released at once,
// so a reference passed in is never leaked. Returns true when the probe
// was added.
static bool addBufferProbe(GstElement *element, const char *padName, GstPadProbeCallback probeCallback,
                           gpointer probeData, GDestroyNotify releaseProbeData)
{
    GstPad *targetPad = gst_element_get_static_pad(element, padName);
    if (targetPad == nullptr) {
        bool hasDataToRelease = probeData != nullptr && releaseProbeData != nullptr;
        if (hasDataToRelease) {
            releaseProbeData(probeData);
        }
        return false;
    }
    gst_pad_add_probe(targetPad, GST_PAD_PROBE_TYPE_BUFFER, probeCallback, probeData, releaseProbeData);
    gst_object_unref(targetPad);
    return true;
}

// ---- Display orientation (SEI) ----------------------------------------------

// Collects bits most-significant first into bytes, the order H.264 syntax
// elements are written in.
class BitWriter {
public:
    // Appends the lowest bitCount bits of value, highest bit first.
    void writeBits(guint32 value, int bitCount)
    {
        int highestBitIndex = bitCount - 1;
        for (int bitIndex = highestBitIndex; bitIndex >= 0; --bitIndex) {
            guint32 shiftedValue = value >> bitIndex;
            guint32 bitValue = shiftedValue & 1u;
            writeBit(bitValue);
        }
    }

    // Appends value as an unsigned Exp-Golomb code, ue(v) in the H.264 spec:
    // (value + 1) written in binary, preceded by one fewer zero bits than
    // it has significant bits.
    void writeUnsignedExpGolomb(guint32 value)
    {
        guint32 codeValue = value + 1;
        int significantBitCount = countSignificantBits(codeValue);
        int leadingZeroCount = significantBitCount - 1;
        writeBits(0, leadingZeroCount);
        writeBits(codeValue, significantBitCount);
    }

    // Ends an SEI payload: a one bit, then zero bits up to a byte boundary.
    void alignPayload()
    {
        writeBit(1);
        while (!isByteAligned()) {
            writeBit(0);
        }
    }

    // Returns the bytes written so far.
    const std::vector<guint8> &bytes() const
    {
        return byteData;
    }

private:
    // Returns how many bits are needed to write value (0 for 0).
    static int countSignificantBits(guint32 value)
    {
        int significantBitCount = 0;
        guint32 remainingValue = value;
        while (remainingValue != 0) {
            ++significantBitCount;
            remainingValue = remainingValue >> 1;
        }
        return significantBitCount;
    }

    // Returns true when the next bit would start a new byte.
    bool isByteAligned() const
    {
        size_t bitPositionInByte = writtenBitCount % BITS_PER_BYTE;
        return bitPositionInByte == 0;
    }

    // Appends a single bit, starting a new byte when the last one is full.
    void writeBit(guint32 bitValue)
    {
        if (isByteAligned()) {
            byteData.push_back(0);
        }
        if (bitValue != 0) {
            size_t bitPositionInByte = writtenBitCount % BITS_PER_BYTE;
            guint8 bitMask = static_cast<guint8>(HIGHEST_BIT_IN_BYTE >> bitPositionInByte);
            byteData.back() = static_cast<guint8>(byteData.back() | bitMask);
        }
        ++writtenBitCount;
    }

    std::vector<guint8> byteData;
    size_t writtenBitCount = 0;
};

// Appends rbspBytes to nalUnit, inserting an emulation prevention byte
// (0x03) wherever two zero bytes would be followed by a byte <= 0x03, so
// the payload can never be mistaken for a start code.
static void appendWithEmulationPrevention(std::vector<guint8> &nalUnit,
                                          const std::vector<guint8> &rbspBytes)
{
    int consecutiveZeroCount = 0;
    for (guint8 rbspByte : rbspBytes) {
        bool followsEnoughZeros = consecutiveZeroCount >= ZERO_BYTES_BEFORE_PROTECTION;
        bool looksLikeStartCode = rbspByte <= LARGEST_START_CODE_LIKE_BYTE;
        if (followsEnoughZeros && looksLikeStartCode) {
            nalUnit.push_back(EMULATION_PREVENTION_BYTE);
            consecutiveZeroCount = 0;
        }
        nalUnit.push_back(rbspByte);
        if (rbspByte == 0x00) {
            consecutiveZeroCount = consecutiveZeroCount + 1;
        } else {
            consecutiveZeroCount = 0;
        }
    }
}

// Converts a clockwise display rotation in degrees into the anticlockwise
// rotation, in 1/65536ths of a turn, that the H.264 message carries.
static guint32 clockwiseDegreesToRotationUnits(long clockwiseDegrees)
{
    long degreesLeftInTurn = DEGREES_PER_TURN - clockwiseDegrees;
    long anticlockwiseDegrees = degreesLeftInTurn % DEGREES_PER_TURN;
    long scaledDegrees = anticlockwiseDegrees * FULL_TURN_ROTATION_UNITS;
    long rotationUnits = scaledDegrees / DEGREES_PER_TURN;
    return static_cast<guint32>(rotationUnits);
}

// Builds a complete Annex B SEI NAL unit holding a display orientation
// message that tells players to show the picture turned clockwise by
// clockwiseDegrees. The message covers only the frame it is attached to,
// so it is added to every frame.
static std::vector<guint8> buildOrientationSeiNal(long clockwiseDegrees)
{
    guint32 rotationUnits = clockwiseDegreesToRotationUnits(clockwiseDegrees);

    BitWriter payloadWriter;
    payloadWriter.writeBits(FLAG_CLEAR, FLAG_FIELD_BITS);                   // display_orientation_cancel_flag
    payloadWriter.writeBits(FLAG_CLEAR, FLAG_FIELD_BITS);                   // hor_flip
    payloadWriter.writeBits(FLAG_CLEAR, FLAG_FIELD_BITS);                   // ver_flip
    payloadWriter.writeBits(rotationUnits, ROTATION_FIELD_BITS);            // anticlockwise_rotation
    payloadWriter.writeUnsignedExpGolomb(REPETITION_THIS_FRAME_ONLY);       // repetition period
    payloadWriter.writeBits(FLAG_CLEAR, FLAG_FIELD_BITS);                   // display_orientation_extension_flag
    payloadWriter.alignPayload();
    const std::vector<guint8> &payloadBytes = payloadWriter.bytes();
    guint8 payloadSize = static_cast<guint8>(payloadBytes.size());

    std::vector<guint8> rbspBytes;
    rbspBytes.push_back(DISPLAY_ORIENTATION_PAYLOAD_TYPE);
    rbspBytes.push_back(payloadSize);
    rbspBytes.insert(rbspBytes.end(), payloadBytes.begin(), payloadBytes.end());
    rbspBytes.push_back(RBSP_STOP_BITS);

    std::vector<guint8> nalUnit(std::begin(ANNEX_B_START_CODE), std::end(ANNEX_B_START_CODE));
    nalUnit.push_back(SEI_NAL_HEADER);
    appendWithEmulationPrevention(nalUnit, rbspBytes);
    return nalUnit;
}

// Wraps the orientation SEI for clockwiseDegrees in a read-only,
// reference-counted GstMemory. Every frame shares this one memory, and every
// holder (the media factory's signal, each pipeline's probe, each frame in
// flight) keeps its own reference, so the bytes stay valid for as long as
// anything can still use them, including pipelines shutting down after
// main() has finished.
static GstMemory *createOrientationSeiMemory(long clockwiseDegrees)
{
    std::vector<guint8> seiNalUnit = buildOrientationSeiNal(clockwiseDegrees);
    gsize seiSize = seiNalUnit.size();
    gpointer seiBytes = g_memdup2(seiNalUnit.data(), seiSize);
    GstMemory *seiMemory = gst_memory_new_wrapped(GST_MEMORY_FLAG_READONLY, seiBytes, seiSize,
                                                  0, seiSize, seiBytes, g_free);
    return seiMemory;
}

// Releases one reference to the orientation SEI memory (GDestroyNotify).
static void releaseSeiMemory(gpointer seiMemory)
{
    gst_memory_unref(GST_MEMORY_CAST(seiMemory));
}

// Releases the media factory's SEI reference when its signal handler is
// disconnected (GClosureNotify).
static void releaseSeiMemoryClosure(gpointer seiMemory, GClosure *)
{
    releaseSeiMemory(seiMemory);
}

// Payloader input probe: returns a new buffer holding the orientation SEI
// followed by the encoded frame. Nothing is copied: the SEI memory gains a
// reference and the frame's memories are shared. The encoder's own buffer
// is left unmodified so it can go straight back to the encoder's pool.
static GstPadProbeReturn onEncodedFrame(GstPad *, GstPadProbeInfo *probeInfo, gpointer seiMemoryData)
{
    GstMemory *seiMemory = GST_MEMORY_CAST(seiMemoryData);
    GstBuffer *encodedFrame = GST_PAD_PROBE_INFO_BUFFER(probeInfo);

    GstBuffer *taggedFrame = gst_buffer_new();
    GstMemory *seiReference = gst_memory_ref(seiMemory);
    gst_buffer_append_memory(taggedFrame, seiReference);   // takes the reference
    GstBufferCopyFlags copyFlags = GstBufferCopyFlags(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_MEMORY);
    gsize copyFromStart = 0;
    gsize copyWholeBuffer = static_cast<gsize>(-1);
    gst_buffer_copy_into(taggedFrame, encodedFrame, copyFlags, copyFromStart, copyWholeBuffer);

    gst_buffer_unref(encodedFrame);
    GST_PAD_PROBE_INFO_DATA(probeInfo) = taggedFrame;
    return GST_PAD_PROBE_OK;
}

// Installs the orientation probe on the payloader's input in mediaBin. The
// probe holds its own reference to seiMemory for as long as it exists.
static void attachOrientationTagger(GstElement *mediaBin, GstMemory *seiMemory)
{
    GstElement *payloader = findElementInBin(mediaBin, PAYLOADER_NAME);
    if (payloader == nullptr) {
        logError("payloader not found; stream will not be rotated");
        return;
    }
    GstMemory *probeReference = gst_memory_ref(seiMemory);
    bool isProbeAdded = addBufferProbe(payloader, SINK_PAD_NAME, onEncodedFrame,
                                       probeReference, releaseSeiMemory);
    if (!isProbeAdded) {
        logError("payloader not found; stream will not be rotated");
    }
    gst_object_unref(payloader);
}

// ---- Night mode -------------------------------------------------------------

// Returns the word for a light mode, as used in the mode file and HTTP replies.
static const char *lightModeText(bool nightMode)
{
    if (nightMode) {
        return NIGHT_MODE_TEXT;
    }
    return DAY_MODE_TEXT;
}

// Returns a description of a light mode, for log lines.
static const char *lightModeDescription(bool nightMode)
{
    if (nightMode) {
        return NIGHT_MODE_DESCRIPTION;
    }
    return DAY_MODE_DESCRIPTION;
}

// Reads LIGHT_MODE_FILE and returns true when it says night. A missing or
// unreadable file means day.
static bool readNightModeFromFile()
{
    gchar *fileText = nullptr;
    gboolean isFileRead = g_file_get_contents(LIGHT_MODE_FILE, &fileText, nullptr, nullptr);
    if (!isFileRead) {
        return false;
    }
    const gchar *trimmedText = g_strstrip(fileText);
    bool fileSaysNight = std::strcmp(trimmedText, NIGHT_MODE_TEXT) == 0;
    g_free(fileText);
    return fileSaysNight;
}

// Saves the light mode to LIGHT_MODE_FILE (written to a temporary file and
// renamed, so a reader never sees half a file). A failure is only logged:
// the mode still applies, it just won't survive a restart.
static void saveLightModeFile(bool nightMode)
{
    gchar *fileText = g_strdup_printf("%s\n", lightModeText(nightMode));
    GError *saveError = nullptr;
    gboolean isFileSaved = g_file_set_contents(LIGHT_MODE_FILE, fileText, -1, &saveError);
    if (!isFileSaved) {
        logError("cannot save light mode to %s: %s", LIGHT_MODE_FILE, errorMessageOrFallback(saveError));
        g_clear_error(&saveError);
    }
    g_free(fileText);
}

// Returns the value a light mode control takes in the given mode.
static const char *controlValueForMode(const LightModeControl &lightModeControl, bool nightMode)
{
    if (nightMode) {
        return lightModeControl.nightValue;
    }
    return lightModeControl.dayValue;
}

// Sets every control in LIGHT_MODE_CONTROLS on camera to its night or day
// value. libcamerasrc queues the values onto its next capture request; while
// streaming, call this only from the camera probe (see onCameraFrame).
static void applyLightModeControls(GstElement *camera, bool nightMode)
{
    for (const LightModeControl &lightModeControl : LIGHT_MODE_CONTROLS) {
        const char *controlValue = controlValueForMode(lightModeControl, nightMode);
        gst_util_set_object_arg(G_OBJECT(camera), lightModeControl.property, controlValue);
    }
}

// Owns the light mode and decides when the camera's controls change. It is
// used from four threads, so every member function is thread-safe:
//  - the control server's worker thread requests modes,
//  - media-configure (RTSP client thread) prepares each new camera,
//  - the camera probe (camera streaming thread) applies pending changes,
//  - the main thread reads the mode for logging.
// modeLock guards nightMode and makes "check pending, apply, clear pending"
// one step; applyPending is atomic only so the per-frame probe can skip all
// work with a single load when nothing is pending.
class LightModeController {
public:
    // Restores the mode saved before a restart (day if there is none).
    void restoreSavedMode()
    {
        bool savedNightMode = readNightModeFromFile();
        std::lock_guard<std::mutex> modeGuard(modeLock);
        nightMode = savedNightMode;
    }

    // Returns true when night mode is in force.
    bool isNightMode() const
    {
        std::lock_guard<std::mutex> modeGuard(modeLock);
        return nightMode;
    }

    // Switches to the requested mode: saves it, logs it and marks it pending,
    // so the camera probe applies it on the next frame. With no client
    // connected nothing is streaming; prepareNewCamera() applies the mode
    // when a pipeline is next built. Requesting the mode already in force
    // does nothing.
    void requestMode(bool nightModeRequested)
    {
        std::lock_guard<std::mutex> modeGuard(modeLock);
        bool isModeChange = nightModeRequested != nightMode;
        if (!isModeChange) {
            return;
        }
        nightMode = nightModeRequested;
        logInfo("light mode: %s", lightModeDescription(nightMode));
        saveLightModeFile(nightMode);
        applyPending.store(true);
    }

    // Puts a newly built camera into the current mode before it starts. At
    // night the controls are set now (a camera's first request always gets
    // a buffer, so they can't be lost); by day nothing is set, so the tuning
    // file's own processing is used unchanged. Any pending change is
    // cleared, since the new camera already starts in the right mode.
    void prepareNewCamera(GstElement *camera)
    {
        std::lock_guard<std::mutex> modeGuard(modeLock);
        if (nightMode) {
            applyLightModeControls(camera, true);
        }
        applyPending.store(false);
    }

    // Returns true when a mode change is waiting for the camera. A cheap
    // pre-check for the per-frame probe; applyPendingChange() decides.
    bool hasPendingChange() const
    {
        return applyPending.load();
    }

    // Called by the camera probe on the camera's streaming thread: if a
    // change is still pending, applies the current mode's controls to
    // camera, clears the flag and returns true (the caller then drops the
    // frame). The check, the apply and the clear happen under one lock, so
    // a request arriving at the same moment is never lost or applied twice.
    bool applyPendingChange(GstElement *camera)
    {
        std::lock_guard<std::mutex> modeGuard(modeLock);
        bool isStillPending = applyPending.load();
        if (!isStillPending) {
            return false;
        }
        applyPending.store(false);
        applyLightModeControls(camera, nightMode);
        logInfo("light mode: %s applied", lightModeText(nightMode));
        return true;
    }

private:
    mutable std::mutex modeLock;
    bool nightMode = false;
    std::atomic<bool> applyPending{false};
};

// The one light mode controller, shared by every thread.
static LightModeController lightModeController;

// Camera output probe, run by libcamerasrc's streaming thread for every
// frame. When a mode change is pending it sets the new controls and drops
// this frame. Both matter:
//  - Setting the controls from this thread means libcamerasrc can't be
//    building a request at the same moment (it does that on this thread).
//  - Dropping the frame returns its buffer to libcamerasrc's pool at once,
//    so the next request it builds is sure to get a buffer. libcamerasrc
//    moves pending controls into a request before looking for a buffer and
//    throws both away when there is none; with a free buffer guaranteed,
//    the controls always reach the camera on the first try.
// With nothing pending the probe costs one atomic load per frame.
static GstPadProbeReturn onCameraFrame(GstPad *cameraPad, GstPadProbeInfo *, gpointer)
{
    bool hasPendingChange = lightModeController.hasPendingChange();
    if (!hasPendingChange) {
        return GST_PAD_PROBE_OK;
    }
    GstElement *camera = gst_pad_get_parent_element(cameraPad);
    if (camera == nullptr) {
        return GST_PAD_PROBE_OK;
    }
    bool wasChangeApplied = lightModeController.applyPendingChange(camera);
    gst_object_unref(camera);
    if (wasChangeApplied) {
        return GST_PAD_PROBE_DROP;
    }
    return GST_PAD_PROBE_OK;
}

// Adds the mode-switching probe to a newly configured pipeline's camera and
// puts the camera in the current mode before it starts.
static void trackCamera(GstElement *mediaBin)
{
    GstElement *camera = findElementInBin(mediaBin, CAMERA_ELEMENT_NAME);
    if (camera == nullptr) {
        logError("camera element not found; night mode will not be applied");
        return;
    }
    addBufferProbe(camera, SOURCE_PAD_NAME, onCameraFrame, nullptr, nullptr);
    lightModeController.prepareNewCamera(camera);
    gst_object_unref(camera);
}

// ---- Control HTTP server ----------------------------------------------------

// An HTTP status code with its reason phrase.
struct HttpStatus {
    int code;
    const char *reason;
};

static constexpr HttpStatus HTTP_OK                 = {200, "OK"};
static constexpr HttpStatus HTTP_BAD_REQUEST        = {400, "Bad Request"};
static constexpr HttpStatus HTTP_NOT_FOUND          = {404, "Not Found"};
static constexpr HttpStatus HTTP_METHOD_NOT_ALLOWED = {405, "Method Not Allowed"};

// Help text sent with every error reply.
static constexpr const char *CONTROL_USAGE_TEXT = "use GET /mode or GET /mode?state=night|day\n";

// Pieces of an HTTP request line ("GET /mode HTTP/1.1") and head.
static constexpr const char *REQUEST_PART_SEPARATOR = " ";
static constexpr int REQUEST_LINE_PART_COUNT = 3;     // method, target, version
static constexpr int REQUEST_METHOD_INDEX = 0;
static constexpr int REQUEST_TARGET_INDEX = 1;
static constexpr int REQUEST_VERSION_INDEX = 2;
static constexpr const char *SUPPORTED_METHOD = "GET";
static constexpr const char *HTTP_VERSION_PREFIX = "HTTP/";
static constexpr const char *QUERY_PARAMETER_SEPARATORS = "&";
static constexpr const char *HEAD_END_CRLF = "\r\n\r\n";
static constexpr const char *HEAD_END_LF = "\n\n";
static constexpr const char *LINE_END_CHARACTERS = "\r\n";

// Returns true once requestText holds a complete request head, i.e. the
// blank line that ends the headers (CRLF or bare LF line endings).
static bool hasCompleteRequestHead(const char *requestText)
{
    const char *crlfHeadEnd = std::strstr(requestText, HEAD_END_CRLF);
    const char *lfHeadEnd = std::strstr(requestText, HEAD_END_LF);
    bool hasCrlfHeadEnd = crlfHeadEnd != nullptr;
    bool hasLfHeadEnd = lfHeadEnd != nullptr;
    return hasCrlfHeadEnd || hasLfHeadEnd;
}

// Reads the request line and headers (up to the blank line that ends them)
// into requestBuffer, NUL-terminated. Reading the whole head matters: closing
// a socket with unread data makes the kernel reset the connection, which can
// make curl lose the reply. Returns false on timeout, error, early close or
// a request larger than the buffer.
static bool readRequestHead(GInputStream *inputStream, char *requestBuffer, gsize bufferSize)
{
    gsize usableBytes = bufferSize - 1;   // one byte is kept for the terminating NUL
    gsize receivedBytes = 0;
    while (receivedBytes < usableBytes) {
        char *writePosition = requestBuffer + receivedBytes;
        gsize spaceLeft = usableBytes - receivedBytes;
        gssize readResult = g_input_stream_read(inputStream, writePosition, spaceLeft, nullptr, nullptr);
        bool isEndOrError = readResult <= 0;
        if (isEndOrError) {
            return false;
        }
        gsize bytesJustRead = static_cast<gsize>(readResult);
        receivedBytes = receivedBytes + bytesJustRead;
        requestBuffer[receivedBytes] = '\0';
        if (hasCompleteRequestHead(requestBuffer)) {
            return true;
        }
    }
    return false;
}

// Interprets a requested state value: sets isNightRequested and returns true
// for "night" or "day", returns false for anything else (including none).
static bool parseRequestedState(const char *requestedState, bool &isNightRequested)
{
    if (requestedState == nullptr) {
        return false;
    }
    bool isNightRequest = std::strcmp(requestedState, NIGHT_MODE_TEXT) == 0;
    bool isDayRequest = std::strcmp(requestedState, DAY_MODE_TEXT) == 0;
    if (isNightRequest) {
        isNightRequested = true;
        return true;
    }
    if (isDayRequest) {
        isNightRequested = false;
        return true;
    }
    return false;
}

// Handles the query of a GET /mode request: no query reports the mode,
// state=night or state=day switches to it. On success replyBody is set to
// the mode now in force.
static HttpStatus handleModeQuery(const char *queryText, const char *&replyBody)
{
    bool hasQuery = queryText != nullptr;
    if (hasQuery) {
        GHashTable *queryParameters = g_uri_parse_params(queryText, -1, QUERY_PARAMETER_SEPARATORS,
                                                         G_URI_PARAMS_NONE, nullptr);
        if (queryParameters == nullptr) {
            return HTTP_BAD_REQUEST;
        }
        gpointer stateLookup = g_hash_table_lookup(queryParameters, CONTROL_STATE_PARAMETER);
        const char *requestedState = static_cast<const char *>(stateLookup);
        bool isNightRequested = false;
        bool isValidState = parseRequestedState(requestedState, isNightRequested);
        if (isValidState) {
            lightModeController.requestMode(isNightRequested);
        }
        g_hash_table_unref(queryParameters);
        if (!isValidState) {
            return HTTP_BAD_REQUEST;
        }
    }
    bool nightModeNow = lightModeController.isNightMode();
    replyBody = lightModeText(nightModeNow);
    return HTTP_OK;
}

// Handles the target of a GET request ("/mode?state=night"): only
// CONTROL_PATH exists. replyBody is set on success.
static HttpStatus handleRequestTarget(const char *requestTarget, const char *&replyBody)
{
    gchar *requestPath = nullptr;
    gchar *queryText = nullptr;
    HttpStatus replyStatus = HTTP_BAD_REQUEST;
    gboolean isTargetParsed = g_uri_split(requestTarget, G_URI_FLAGS_ENCODED_QUERY, nullptr, nullptr,
                                          nullptr, nullptr, &requestPath, &queryText, nullptr, nullptr);
    if (isTargetParsed) {
        bool isControlPath = std::strcmp(requestPath, CONTROL_PATH) == 0;
        if (isControlPath) {
            replyStatus = handleModeQuery(queryText, replyBody);
        } else {
            replyStatus = HTTP_NOT_FOUND;
        }
    }
    g_free(requestPath);
    g_free(queryText);
    return replyStatus;
}

// Works out the reply to one request line ("GET /mode?state=night HTTP/1.1").
// replyBody is set to the mode for a successful request and left alone
// (the usage text) otherwise.
static HttpStatus handleRequestLine(const char *requestLine, const char *&replyBody)
{
    gchar **requestParts = g_strsplit(requestLine, REQUEST_PART_SEPARATOR, REQUEST_LINE_PART_COUNT);
    guint partCount = g_strv_length(requestParts);
    HttpStatus replyStatus = HTTP_BAD_REQUEST;
    bool hasAllParts = partCount == static_cast<guint>(REQUEST_LINE_PART_COUNT);
    if (hasAllParts) {
        const char *requestMethod = requestParts[REQUEST_METHOD_INDEX];
        const char *requestTarget = requestParts[REQUEST_TARGET_INDEX];
        const char *requestVersion = requestParts[REQUEST_VERSION_INDEX];
        bool isHttpVersion = g_str_has_prefix(requestVersion, HTTP_VERSION_PREFIX);
        bool isSupportedMethod = std::strcmp(requestMethod, SUPPORTED_METHOD) == 0;
        if (isHttpVersion && !isSupportedMethod) {
            replyStatus = HTTP_METHOD_NOT_ALLOWED;
        } else if (isHttpVersion) {
            replyStatus = handleRequestTarget(requestTarget, replyBody);
        }
    }
    g_strfreev(requestParts);
    return replyStatus;
}

// Builds the reply body: the mode followed by a newline for a successful
// request, the usage text otherwise. The caller frees it with g_free().
static gchar *buildReplyBody(HttpStatus replyStatus, const char *replyBody)
{
    bool isSuccess = replyStatus.code == HTTP_OK.code;
    if (isSuccess) {
        return g_strdup_printf("%s\n", replyBody);
    }
    return g_strdup(replyBody);
}

// Writes a complete plain-text HTTP reply. The connection is closed after
// it (Connection: close), so no keep-alive handling is needed.
static void sendHttpReply(GOutputStream *outputStream, HttpStatus replyStatus, const char *replyBody)
{
    size_t bodyLength = std::strlen(replyBody);
    gchar *replyText = g_strdup_printf(
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        replyStatus.code, replyStatus.reason, bodyLength, replyBody);
    size_t replyLength = std::strlen(replyText);
    g_output_stream_write_all(outputStream, replyText, replyLength, nullptr, nullptr, nullptr);
    g_free(replyText);
}

// Handles one control connection on the worker thread: reads one request,
// answers it and closes the connection. Blocking reads (with a timeout)
// are fine here, because this thread never runs the RTSP main loop.
static void handleControlConnection(GSocketConnection *connection)
{
    GSocket *connectionSocket = g_socket_connection_get_socket(connection);
    g_socket_set_timeout(connectionSocket, CONTROL_TIMEOUT_SECONDS);
    GIOStream *connectionStream = G_IO_STREAM(connection);
    GInputStream *inputStream = g_io_stream_get_input_stream(connectionStream);
    GOutputStream *outputStream = g_io_stream_get_output_stream(connectionStream);

    char requestBuffer[CONTROL_MAX_REQUEST_BYTES];
    bool isRequestRead = readRequestHead(inputStream, requestBuffer, sizeof requestBuffer);
    if (isRequestRead) {
        size_t requestLineLength = std::strcspn(requestBuffer, LINE_END_CHARACTERS);
        requestBuffer[requestLineLength] = '\0';   // keep the request line only
        const char *replyBody = CONTROL_USAGE_TEXT;
        HttpStatus replyStatus = handleRequestLine(requestBuffer, replyBody);
        gchar *replyBodyText = buildReplyBody(replyStatus, replyBody);
        sendHttpReply(outputStream, replyStatus, replyBodyText);
        g_free(replyBodyText);
    }
    g_io_stream_close(connectionStream, nullptr, nullptr);
}

// Worker thread entry (GFunc): handles one queued connection and releases
// the reference the accept handler took for it.
static void onControlJob(gpointer connectionData, gpointer)
{
    GSocketConnection *connection = G_SOCKET_CONNECTION(connectionData);
    handleControlConnection(connection);
    g_object_unref(connection);
}

// Accept handler, run on the main loop for each new connection: queues the
// connection for the worker thread and returns at once, so a slow client
// can never stall the RTSP server.
static gboolean onControlConnectionAccepted(GSocketService *, GSocketConnection *connection,
                                            GObject *, gpointer workerPoolData)
{
    GThreadPool *workerPool = static_cast<GThreadPool *>(workerPoolData);
    GSocketConnection *queuedConnection = G_SOCKET_CONNECTION(g_object_ref(connection));
    g_thread_pool_push(workerPool, queuedConnection, nullptr);
    return TRUE;
}

// The control HTTP server: a listening socket served by the main loop, and
// a worker pool (CONTROL_WORKER_THREADS threads, created on demand) that
// handles the connections one at a time, in arrival order.
// GThreadedSocketService is deliberately not used: when its thread limit is
// reached it stops and restarts its listener from the worker thread, which
// races with the main loop accepting connections and can crash under
// concurrent requests.
struct ControlServer {
    GSocketService *socketService = nullptr;
    GThreadPool *workerPool = nullptr;
};

// Releases a connection still waiting in the worker queue when the pool is
// freed at shutdown (GDestroyNotify).
static void releaseQueuedConnection(gpointer connectionData)
{
    g_object_unref(connectionData);
}

// Stops the control server and releases it: no new connections are
// accepted, the request being handled (if any) finishes, queued ones are
// dropped, then the socket is closed. Safe to call on a partly created server.
static void destroyControlServer(ControlServer *controlServer)
{
    if (controlServer->socketService != nullptr) {
        g_socket_service_stop(controlServer->socketService);
        g_socket_listener_close(G_SOCKET_LISTENER(controlServer->socketService));
    }
    if (controlServer->workerPool != nullptr) {
        gboolean dropQueuedJobs = TRUE;
        gboolean waitForRunningJob = TRUE;
        g_thread_pool_free(controlServer->workerPool, dropQueuedJobs, waitForRunningJob);
        controlServer->workerPool = nullptr;
    }
    if (controlServer->socketService != nullptr) {
        g_object_unref(controlServer->socketService);
        controlServer->socketService = nullptr;
    }
    delete controlServer;
}

// Creates and starts the control HTTP server on CONTROL_LISTEN_ADDRESS at
// the configured port. Returns nullptr after logging why if it can't bind.
static ControlServer *createControlServer(const Config &config)
{
    ControlServer *controlServer = new ControlServer();
    controlServer->socketService = g_socket_service_new();
    GInetAddress *listenAddress = g_inet_address_new_from_string(CONTROL_LISTEN_ADDRESS);
    guint16 listenPort = static_cast<guint16>(config.controlPort);
    GSocketAddress *socketAddress = g_inet_socket_address_new(listenAddress, listenPort);
    GError *bindError = nullptr;
    gboolean isListening = g_socket_listener_add_address(G_SOCKET_LISTENER(controlServer->socketService),
                                                         socketAddress, G_SOCKET_TYPE_STREAM,
                                                         G_SOCKET_PROTOCOL_TCP, nullptr, nullptr, &bindError);
    g_object_unref(socketAddress);
    g_object_unref(listenAddress);
    if (!isListening) {
        logError("cannot listen on %s:%ld (control): %s", CONTROL_LISTEN_ADDRESS,
                 config.controlPort, errorMessageOrFallback(bindError));
        g_clear_error(&bindError);
        destroyControlServer(controlServer);
        return nullptr;
    }
    gboolean useExclusiveThreads = FALSE;   // threads are created on demand and exit when idle
    controlServer->workerPool = g_thread_pool_new_full(onControlJob, nullptr, releaseQueuedConnection,
                                                       CONTROL_WORKER_THREADS, useExclusiveThreads, nullptr);
    g_signal_connect(controlServer->socketService, "incoming", G_CALLBACK(onControlConnectionAccepted),
                     controlServer->workerPool);
    g_socket_service_start(controlServer->socketService);
    return controlServer;
}

// ---- Pipeline --------------------------------------------------------------

// Builds the gst-launch style description of the camera pipeline.
//  - Frames are copied into the encoder's own buffers; handing it the
//    camera's buffers directly (dmabuf import) fails on this hardware.
//  - The encoder repeats SPS/PPS before every keyframe itself
//    (repeat_sequence_header), so no h264parse is needed and the
//    payloader doesn't inject them again (config-interval=0).
//  - No queues, so no extra threads or buffered frames.
//  - libcamerasrc is named so night mode can find it and set its controls.
// The caller frees the returned string with g_free().
static gchar *buildPipelineDescription(const Config &config)
{
    gchar *pipelineDescription = g_strdup_printf(
        "( libcamerasrc name=%s"
        " sensor-config=\"sensor/config,width=%ld,height=%ld,depth=%ld\""
        " ! video/x-raw,format=I420,width=%ld,height=%ld,framerate=%ld/1"
        " ! v4l2h264enc extra-controls=\"controls"
        ",video_bitrate=%ld,h264_i_frame_period=%ld,repeat_sequence_header=1\""
        " ! video/x-h264,level=(string)4,profile=(string)high"
        " ! rtph264pay name=%s pt=96 config-interval=0 )",
        CAMERA_ELEMENT_NAME, config.sensorWidth, config.sensorHeight, config.sensorBitDepth,
        config.width, config.height, config.framesPerSecond,
        config.bitrate, config.keyframeInterval, PAYLOADER_NAME);
    return pipelineDescription;
}

// ---- Runtime callbacks -----------------------------------------------------

// Bus sync handler: logs pipeline errors. Runs in the thread that posted
// the message and passes every message on to the RTSP server's own bus
// watch, so it adds no thread or queue.
static GstBusSyncReply onBusSyncMessage(GstBus *, GstMessage *message, gpointer)
{
    bool isErrorMessage = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
    if (!isErrorMessage) {
        return GST_BUS_PASS;
    }
    GError *pipelineError = nullptr;
    gchar *debugDetails = nullptr;
    gst_message_parse_error(message, &pipelineError, &debugDetails);
    const char *sourceName = GST_MESSAGE_SRC_NAME(message);
    if (sourceName == nullptr) {
        sourceName = UNKNOWN_TEXT;
    }
    const char *errorText = "unknown";
    if (pipelineError != nullptr) {
        errorText = pipelineError->message;
    }
    logError("error from %s: %s", sourceName, errorText);
    if (debugDetails != nullptr) {
        logError("  %s", debugDetails);
    }
    g_clear_error(&pipelineError);
    g_free(debugDetails);
    return GST_BUS_PASS;
}

// Logs a media state change; the message to print is the signal's user data.
static void onMediaStateChanged(GstRTSPMedia *, gpointer stateMessage)
{
    logInfo("%s", static_cast<const char *>(stateMessage));
}

// Attaches the error logger to the bus of the pipeline that holds mediaBin.
static void attachBusErrorLogger(GstElement *mediaBin)
{
    GstObject *pipeline = gst_object_get_parent(GST_OBJECT(mediaBin));
    if (pipeline == nullptr) {
        return;
    }
    if (GST_IS_PIPELINE(pipeline)) {
        GstBus *pipelineBus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
        gst_bus_set_sync_handler(pipelineBus, onBusSyncMessage, nullptr, nullptr);
        gst_object_unref(pipelineBus);
    }
    gst_object_unref(pipeline);
}

// Called once per new media pipeline: hands the camera to night mode, adds
// the orientation tagger when rotating (seiMemoryData is nullptr otherwise),
// attaches the error logger and logs when the camera starts and stops.
static void onMediaConfigure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer seiMemoryData)
{
    GstElement *mediaBin = gst_rtsp_media_get_element(media);
    if (mediaBin != nullptr) {
        trackCamera(mediaBin);
        bool isRotating = seiMemoryData != nullptr;
        if (isRotating) {
            attachOrientationTagger(mediaBin, GST_MEMORY_CAST(seiMemoryData));
        }
        attachBusErrorLogger(mediaBin);
        gst_object_unref(mediaBin);
    }
    g_signal_connect(media, "prepared", G_CALLBACK(onMediaStateChanged),
                     const_cast<char *>(CAMERA_STREAMING_MESSAGE));
    g_signal_connect(media, "unprepared", G_CALLBACK(onMediaStateChanged),
                     const_cast<char *>(CAMERA_RELEASED_MESSAGE));
}

// Logs a client disconnecting; the user data is its IP address.
static void onClientClosed(GstRTSPClient *, gpointer clientAddress)
{
    logInfo("client %s disconnected", static_cast<const char *>(clientAddress));
}

// Frees the client IP address copy when its "closed" handler is removed.
static void freeClientAddress(gpointer clientAddress, GClosure *)
{
    g_free(clientAddress);
}

// Returns a copy of a client's IP address, or of "?" if it is unknown.
// The caller frees it with g_free().
static gchar *copyClientAddress(GstRTSPClient *client)
{
    GstRTSPConnection *connection = gst_rtsp_client_get_connection(client);
    if (connection == nullptr) {
        return g_strdup(UNKNOWN_TEXT);
    }
    const char *connectionAddress = gst_rtsp_connection_get_ip(connection);
    if (connectionAddress == nullptr) {
        return g_strdup(UNKNOWN_TEXT);
    }
    return g_strdup(connectionAddress);
}

// Logs a new client and arranges for its disconnect to be logged too.
// The IP address copy is freed automatically with the signal handler.
static void onClientConnected(GstRTSPServer *, GstRTSPClient *client, gpointer)
{
    gchar *clientAddress = copyClientAddress(client);
    logInfo("client %s connected", clientAddress);
    g_signal_connect_data(client, "closed", G_CALLBACK(onClientClosed), clientAddress,
                          freeClientAddress, GConnectFlags(0));
}

// Periodic timer: removes expired sessions so their pipeline can stop.
static gboolean onSessionCleanup(gpointer rtspServer)
{
    GstRTSPServer *server = static_cast<GstRTSPServer *>(rtspServer);
    GstRTSPSessionPool *sessionPool = gst_rtsp_server_get_session_pool(server);
    gst_rtsp_session_pool_cleanup(sessionPool);
    g_object_unref(sessionPool);
    return G_SOURCE_CONTINUE;
}

// Shutdown signal: stops the main loop. Removing the handler means a
// second identical signal terminates immediately.
static gboolean onShutdownSignal(gpointer)
{
    g_main_loop_quit(mainLoop);
    return G_SOURCE_REMOVE;
}

// ---- Server setup and teardown ---------------------------------------------

// Everything main() creates for serving, released together by
// releaseServerResources() whichever step failed.
struct ServerResources {
    GstRTSPServer *rtspServer = nullptr;
    GSource *serverSource = nullptr;
    ControlServer *controlServer = nullptr;
};

// Creates the RTSP server with one shared media factory mounted at the
// configured path. shared=TRUE lets every viewer reuse the same capture and
// encode; the pipeline stops (camera released) when the last client leaves.
// seiMemory is the rotation message to tag frames with, or nullptr; the
// factory keeps its own reference to it.
static GstRTSPServer *createRtspServer(const Config &config, GstMemory *seiMemory)
{
    GstRTSPServer *rtspServer = gst_rtsp_server_new();
    gchar *portText = g_strdup_printf("%ld", config.port);
    gst_rtsp_server_set_address(rtspServer, LISTEN_ADDRESS);
    gst_rtsp_server_set_service(rtspServer, portText);
    g_free(portText);
    g_signal_connect(rtspServer, "client-connected", G_CALLBACK(onClientConnected), nullptr);

    gchar *pipelineDescription = buildPipelineDescription(config);
    GstRTSPMediaFactory *mediaFactory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(mediaFactory, pipelineDescription);
    gst_rtsp_media_factory_set_shared(mediaFactory, TRUE);
    gst_rtsp_media_factory_set_protocols(mediaFactory, GST_RTSP_LOWER_TRANS_TCP);
    g_free(pipelineDescription);

    if (seiMemory != nullptr) {
        GstMemory *factoryReference = gst_memory_ref(seiMemory);
        g_signal_connect_data(mediaFactory, "media-configure", G_CALLBACK(onMediaConfigure),
                              factoryReference, releaseSeiMemoryClosure, GConnectFlags(0));
    } else {
        g_signal_connect(mediaFactory, "media-configure", G_CALLBACK(onMediaConfigure), nullptr);
    }

    GstRTSPMountPoints *mountPoints = gst_rtsp_server_get_mount_points(rtspServer);
    gst_rtsp_mount_points_add_factory(mountPoints, config.mountPath, mediaFactory);  // takes ownership
    g_object_unref(mountPoints);
    return rtspServer;
}

// Binds the RTSP server's socket and attaches it to the main loop. Returns
// the server source, or nullptr after logging why it couldn't bind.
static GSource *startRtspServer(GstRTSPServer *rtspServer, const Config &config)
{
    // create_source binds the socket now and reports why if it can't.
    GError *bindError = nullptr;
    GSource *serverSource = gst_rtsp_server_create_source(rtspServer, nullptr, &bindError);
    if (serverSource == nullptr) {
        logError("cannot listen on %s:%ld: %s", LISTEN_ADDRESS, config.port,
                 errorMessageOrFallback(bindError));
        g_clear_error(&bindError);
        return nullptr;
    }
    g_source_attach(serverSource, nullptr);
    return serverSource;
}

// Releases whatever main() created for serving, in reverse order of
// creation. Members that were never created (nullptr) are skipped.
static void releaseServerResources(ServerResources &serverResources)
{
    if (serverResources.controlServer != nullptr) {
        destroyControlServer(serverResources.controlServer);
        serverResources.controlServer = nullptr;
    }
    if (serverResources.serverSource != nullptr) {
        g_source_destroy(serverResources.serverSource);
        g_source_unref(serverResources.serverSource);
        serverResources.serverSource = nullptr;
    }
    if (serverResources.rtspServer != nullptr) {
        g_object_unref(serverResources.rtspServer);
        serverResources.rtspServer = nullptr;
    }
}

// Logs one viewer URL for the given host.
static void logStreamUrl(const char *host, const Config &config)
{
    logInfo("  rtsp://%s:%ld%s", host, config.port, config.mountPath);
}

// Returns true for an interface that is up and has an IPv4 address.
static bool isUsableIpv4Interface(const struct ifaddrs *networkInterface)
{
    const struct sockaddr *interfaceAddress = networkInterface->ifa_addr;
    if (interfaceAddress == nullptr) {
        return false;
    }
    bool isIpv4 = interfaceAddress->sa_family == AF_INET;
    unsigned int upFlag = networkInterface->ifa_flags & IFF_UP;
    bool isUp = upFlag != 0;
    return isIpv4 && isUp;
}

// Logs a usable URL for every IPv4 address the server is reachable on.
static void logStreamUrls(const Config &config)
{
    bool listensOnAllInterfaces = std::strcmp(LISTEN_ADDRESS, ALL_INTERFACES_ADDRESS) == 0;
    if (!listensOnAllInterfaces) {
        logStreamUrl(LISTEN_ADDRESS, config);
        return;
    }
    struct ifaddrs *interfaceList = nullptr;
    int listResult = getifaddrs(&interfaceList);
    if (listResult != 0) {
        logStreamUrl(UNKNOWN_HOST_TEXT, config);
        return;
    }
    for (struct ifaddrs *networkInterface = interfaceList; networkInterface != nullptr;
         networkInterface = networkInterface->ifa_next) {
        if (!isUsableIpv4Interface(networkInterface)) {
            continue;
        }
        char addressText[INET_ADDRSTRLEN];
        const auto *ipv4Address = reinterpret_cast<const struct sockaddr_in *>(networkInterface->ifa_addr);
        const char *convertedAddress = inet_ntop(AF_INET, &ipv4Address->sin_addr, addressText, sizeof addressText);
        if (convertedAddress != nullptr) {
            logStreamUrl(addressText, config);
        }
    }
    freeifaddrs(interfaceList);
}

// Removes a main-loop source if it still exists (it may already have
// removed itself, as the shutdown signal handlers do). 0 means no source.
static void removeSourceIfPresent(guint sourceId)
{
    if (sourceId == 0) {
        return;
    }
    GSource *source = g_main_context_find_source_by_id(nullptr, sourceId);
    if (source != nullptr) {
        g_source_destroy(source);
    }
}

// Logs the settings in use, the light mode and every URL to view the stream.
static void logStartupSummary(const Config &config)
{
    logInfo("%ldx%ld@%ld %ld bit/s, keyframe every %ld, sensor %ldx%ld:%ld, rotation %ld",
            config.width, config.height, config.framesPerSecond, config.bitrate,
            config.keyframeInterval, config.sensorWidth, config.sensorHeight,
            config.sensorBitDepth, config.rotationDegrees);
    bool nightModeNow = lightModeController.isNightMode();
    logInfo("light mode: %s, control http://%s:%ld%s", lightModeDescription(nightModeNow),
            CONTROL_LISTEN_ADDRESS, config.controlPort, CONTROL_PATH);
    logStreamUrls(config);
}

// ---- Main ------------------------------------------------------------------

// Parses options, checks the GStreamer install, starts the RTSP server and
// runs until a shutdown signal arrives.
int main(int argc, char *argv[])
{
    // Line-buffer stdout so messages reach journald immediately under systemd.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    gst_init(&argc, &argv);

    Config config;
    bool areArgumentsValid = parseArguments(argc, argv, config);
    if (!areArgumentsValid) {
        printUsage(argv[0]);
        return 1;
    }
    bool areElementsPresent = checkRequiredElements();
    if (!areElementsPresent) {
        return 1;
    }
    bool areControlsPresent = checkLightModeControls();
    if (!areControlsPresent) {
        return 1;
    }

    lightModeController.restoreSavedMode();

    // Built once; the media factory and every pipeline keep their own
    // references, so main's reference is released straight away.
    GstMemory *seiMemory = nullptr;
    bool isRotating = config.rotationDegrees != NO_ROTATION_DEGREES;
    if (isRotating) {
        seiMemory = createOrientationSeiMemory(config.rotationDegrees);
    }
    ServerResources serverResources;
    serverResources.rtspServer = createRtspServer(config, seiMemory);
    if (seiMemory != nullptr) {
        gst_memory_unref(seiMemory);
        seiMemory = nullptr;
    }

    serverResources.serverSource = startRtspServer(serverResources.rtspServer, config);
    if (serverResources.serverSource == nullptr) {
        releaseServerResources(serverResources);
        return 1;
    }
    serverResources.controlServer = createControlServer(config);
    if (serverResources.controlServer == nullptr) {
        releaseServerResources(serverResources);
        return 1;
    }

    mainLoop = g_main_loop_new(nullptr, FALSE);
    guint sessionCleanupSourceId =
        g_timeout_add_seconds(SESSION_CLEANUP_SECONDS, onSessionCleanup, serverResources.rtspServer);
    constexpr gsize shutdownSignalCount = G_N_ELEMENTS(SHUTDOWN_SIGNALS);
    guint signalSourceIds[shutdownSignalCount];
    for (gsize signalIndex = 0; signalIndex < shutdownSignalCount; ++signalIndex) {
        int shutdownSignal = SHUTDOWN_SIGNALS[signalIndex];
        signalSourceIds[signalIndex] = g_unix_signal_add(shutdownSignal, onShutdownSignal, nullptr);
    }

    logStartupSummary(config);

    g_main_loop_run(mainLoop);

    logInfo("shutting down");
    removeSourceIfPresent(sessionCleanupSourceId);
    for (guint signalSourceId : signalSourceIds) {
        removeSourceIfPresent(signalSourceId);
    }
    releaseServerResources(serverResources);
    g_main_loop_unref(mainLoop);
    mainLoop = nullptr;
    return 0;
}
