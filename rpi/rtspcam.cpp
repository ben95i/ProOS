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
//
// Rotation: the Pi's sensor, ISP and encoder cannot turn an image by 90
// degrees, and doing it on the CPU would mean rewriting every frame. Instead
// each frame carries a tiny H.264 "display orientation" message (SEI, about
// 8 bytes) telling the player to show it rotated. The pixels are untouched,
// so this costs no CPU or memory. FFmpeg-based players (mpv, ffplay) honour
// it; players that ignore it show the image unrotated.
//
// Result: rtsp://<pi-address>:<port><mount>  (also rtsp://127.0.0.1:...)
// Needs libcamerasrc with the sensor-config property
// (gstreamer1.0-libcamera 0.7.1+rpt20260429 or newer).
//
// Build (one line):
//   g++ -O2 -s -o rtspcam rtspcam.cpp $(pkg-config --cflags --libs
//       gstreamer-rtsp-server-1.0 gstreamer-rtsp-1.0 gstreamer-1.0)

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib-unix.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#include <vector>

// ---- Fixed settings --------------------------------------------------------

// Address the RTSP server binds to. "0.0.0.0" serves every IPv4 interface,
// so any device on the local network can view the stream. Set to
// "127.0.0.1" to restrict it to this Pi only.
static constexpr const char *kListenAddress = "0.0.0.0";

// How often sessions of clients that vanished without a TEARDOWN are reaped,
// so a crashed viewer doesn't keep the camera and encoder running.
static constexpr guint kSessionCleanupSeconds = 10;

// Limits of the Pi's H.264 hardware encoder at level 4.
static constexpr long kMaxFrameDimension = 1920;
static constexpr long kMaxBitrate = 25000000;

// Signals that trigger a clean shutdown.
static constexpr int kShutdownSignals[] = {SIGTERM, SIGINT, SIGHUP};

// H.264 constants used to build the display orientation message.
static constexpr guint8 kAnnexBStartCode[] = {0x00, 0x00, 0x00, 0x01};
static constexpr guint8 kSeiNalHeader = 0x06;                 // nal_unit_type 6 (SEI)
static constexpr guint8 kDisplayOrientationPayloadType = 47;  // H.264 Annex D.1.27
static constexpr guint8 kRbspStopBits = 0x80;                 // rbsp_trailing_bits
static constexpr guint32 kFullTurnRotationUnits = 65536;      // rotation units per 360 degrees

// Prefix on every log line, so messages are easy to find in the journal.
static constexpr const char *kLogPrefix = "rtspcam: ";

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
static constexpr NumericOption kNumericOptions[] = {
    {'w', &Config::width,            16,    kMaxFrameDimension},
    {'h', &Config::height,           16,    kMaxFrameDimension},
    {'f', &Config::framesPerSecond,  1,     120},
    {'b', &Config::bitrate,          10000, kMaxBitrate},
    {'i', &Config::keyframeInterval, 1,     1000},
    {'p', &Config::port,             1,     65535},
    {'r', &Config::rotationDegrees,  0,     359},
};

// A GStreamer element the pipeline needs, plus a property it must support
// (nullptr when any version of the element will do).
struct RequiredElement {
    const char *name;
    const char *requiredProperty;
};

// Every element used in the pipeline, checked once at startup.
static constexpr RequiredElement kRequiredElements[] = {
    {"libcamerasrc", "sensor-config"},
    {"v4l2h264enc",  nullptr},
    {"rtph264pay",   nullptr},
};

// Main loop, global so the signal handler can stop it.
static GMainLoop *mainLoop = nullptr;

// ---- Logging ---------------------------------------------------------------

// Formats a message and prints it as one prefixed line, to stderr for
// errors and stdout otherwise. Shared by logInfo() and logError().
static void logLine(bool isError, const char *format, va_list arguments)
{
    gchar *messageText = g_strdup_vprintf(format, arguments);
    if (isError)
        g_printerr("%s%s\n", kLogPrefix, messageText);
    else
        g_print("%s%s\n", kLogPrefix, messageText);
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

// ---- Argument parsing ------------------------------------------------------

// Prints the command-line synopsis.
static void printUsage(const char *programName)
{
    std::fprintf(stderr,
        "Usage: %s [-w width] [-h height] [-f fps] [-b bitrate] [-i intra]\n"
        "          [-s WxH:bits] [-p port] [-m mount] [-r 0|90|270]\n", programName);
}

// Returns true when value lies within [minimum, maximum].
static bool isInRange(long value, long minimum, long maximum)
{
    return value >= minimum && value <= maximum;
}

// Parses a whole string as a decimal number within [minimum, maximum].
// Rejects empty input, trailing characters and overflow.
static bool parseRangedLong(const char *text, long minimum, long maximum, long &result)
{
    if (!text || !*text)
        return false;
    errno = 0;
    char *parseEnd = nullptr;
    long parsedValue = std::strtol(text, &parseEnd, 10);
    if (errno != 0 || *parseEnd != '\0' || !isInRange(parsedValue, minimum, maximum))
        return false;
    result = parsedValue;
    return true;
}

// Parses a sensor mode written as WxH:bits (e.g. 1920x1080:10).
static bool parseSensorMode(const char *text, Config &config)
{
    long sensorWidth, sensorHeight, sensorBitDepth;
    int charactersConsumed = 0;
    if (std::sscanf(text, "%ldx%ld:%ld%n", &sensorWidth, &sensorHeight,
                    &sensorBitDepth, &charactersConsumed) != 3 ||
        text[charactersConsumed] != '\0')
        return false;
    if (!isInRange(sensorWidth, 1, 8192) || !isInRange(sensorHeight, 1, 8192) ||
        !isInRange(sensorBitDepth, 8, 16))
        return false;
    config.sensorWidth = sensorWidth;
    config.sensorHeight = sensorHeight;
    config.sensorBitDepth = sensorBitDepth;
    return true;
}

// Returns the numeric option for a letter, or nullptr if it isn't one.
static const NumericOption *findNumericOption(int optionLetter)
{
    for (const NumericOption &numericOption : kNumericOptions)
        if (numericOption.letter == optionLetter)
            return &numericOption;
    return nullptr;
}

// Checks that the mount path looks like "/name" with no characters that
// would break an RTSP URL.
static bool isValidMountPath(const char *mountPath)
{
    return mountPath[0] == '/' && mountPath[1] != '\0' &&
           !std::strpbrk(mountPath, " \t\r\n?#");
}

// Reads all command-line options into config, logging the first problem
// found. Returns false if the options are unusable.
static bool parseArguments(int argc, char *argv[], Config &config)
{
    int optionLetter;
    while ((optionLetter = getopt(argc, argv, "w:h:f:b:i:s:p:m:r:")) != -1) {
        bool isValueValid;
        if (const NumericOption *numericOption = findNumericOption(optionLetter)) {
            isValueValid = parseRangedLong(optarg, numericOption->minimum,
                                           numericOption->maximum,
                                           config.*(numericOption->field));
        } else if (optionLetter == 's') {
            isValueValid = parseSensorMode(optarg, config);
        } else if (optionLetter == 'm') {
            config.mountPath = optarg;
            isValueValid = isValidMountPath(optarg);
        } else {
            return false;   // getopt has already reported the problem
        }
        if (!isValueValid) {
            logError("invalid value for -%c: '%s'", optionLetter, optarg);
            return false;
        }
    }
    if (optind < argc) {
        logError("unexpected argument '%s'", argv[optind]);
        return false;
    }
    if ((config.width & 1) || (config.height & 1)) {
        logError("width and height must be even");
        return false;
    }
    if (config.rotationDegrees != 0 && config.rotationDegrees != 90 &&
        config.rotationDegrees != 270) {
        logError("rotation must be 0, 90 or 270");
        return false;
    }
    return true;
}

// ---- Startup checks --------------------------------------------------------

// Returns true when the element type created by factory has a property
// with the given name. Loads the element's plugin to find out.
static bool elementHasProperty(GstElementFactory *factory, const char *propertyName)
{
    GstPluginFeature *loadedFeature = gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory));
    if (!loadedFeature)
        return false;
    GType elementType = gst_element_factory_get_element_type(GST_ELEMENT_FACTORY(loadedFeature));
    gpointer elementClass = g_type_class_ref(elementType);
    bool hasProperty = g_object_class_find_property(G_OBJECT_CLASS(elementClass), propertyName);
    g_type_class_unref(elementClass);
    gst_object_unref(loadedFeature);
    return hasProperty;
}

// Verifies every element in kRequiredElements is installed and new enough,
// so a missing package fails at startup instead of on the first client.
static bool checkRequiredElements()
{
    bool allPresent = true;
    for (const RequiredElement &requiredElement : kRequiredElements) {
        GstElementFactory *factory = gst_element_factory_find(requiredElement.name);
        if (!factory) {
            logError("GStreamer element '%s' not installed", requiredElement.name);
            allPresent = false;
            continue;
        }
        if (requiredElement.requiredProperty &&
            !elementHasProperty(factory, requiredElement.requiredProperty)) {
            logError("'%s' lacks the '%s' property (package too old)",
                     requiredElement.name, requiredElement.requiredProperty);
            allPresent = false;
        }
        gst_object_unref(factory);
    }
    return allPresent;
}

// ---- Display orientation (SEI) ----------------------------------------------

// Collects bits most-significant first into bytes, the order H.264 syntax
// elements are written in.
class BitWriter {
public:
    // Appends the lowest bitCount bits of value, highest bit first.
    void writeBits(guint32 value, int bitCount)
    {
        for (int bitIndex = bitCount - 1; bitIndex >= 0; --bitIndex)
            writeBit((value >> bitIndex) & 1u);
    }

    // Appends value as an unsigned Exp-Golomb code, ue(v) in the H.264 spec.
    void writeUnsignedExpGolomb(guint32 value)
    {
        guint32 codeValue = value + 1;
        int significantBitCount = 0;
        for (guint32 remainingValue = codeValue; remainingValue != 0; remainingValue >>= 1)
            ++significantBitCount;
        int leadingZeroCount = significantBitCount - 1;
        writeBits(0, leadingZeroCount);
        writeBits(codeValue, significantBitCount);
    }

    // Ends an SEI payload: a one bit, then zero bits up to a byte boundary.
    void alignPayload()
    {
        writeBit(1);
        while (writtenBitCount % 8 != 0)
            writeBit(0);
    }

    // Returns the bytes written so far.
    const std::vector<guint8> &bytes() const { return byteData; }

private:
    // Appends a single bit, starting a new byte when the last one is full.
    void writeBit(guint32 bitValue)
    {
        int bitPosition = static_cast<int>(writtenBitCount % 8);
        if (bitPosition == 0)
            byteData.push_back(0);
        if (bitValue)
            byteData.back() |= static_cast<guint8>(0x80u >> bitPosition);
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
        if (consecutiveZeroCount >= 2 && rbspByte <= 0x03) {
            nalUnit.push_back(0x03);
            consecutiveZeroCount = 0;
        }
        nalUnit.push_back(rbspByte);
        consecutiveZeroCount = (rbspByte == 0x00) ? consecutiveZeroCount + 1 : 0;
    }
}

// Builds a complete Annex B SEI NAL unit holding a display orientation
// message that tells players to show the picture turned clockwise by
// clockwiseDegrees. The message covers only the frame it is attached to,
// so it is added to every frame.
static std::vector<guint8> buildOrientationSeiNal(long clockwiseDegrees)
{
    // H.264 expresses the rotation anticlockwise, in 1/65536ths of a turn.
    long anticlockwiseDegrees = (360 - clockwiseDegrees) % 360;
    guint32 rotationUnits =
        static_cast<guint32>(anticlockwiseDegrees * kFullTurnRotationUnits / 360);

    BitWriter payloadWriter;
    payloadWriter.writeBits(0, 1);              // display_orientation_cancel_flag
    payloadWriter.writeBits(0, 1);              // hor_flip
    payloadWriter.writeBits(0, 1);              // ver_flip
    payloadWriter.writeBits(rotationUnits, 16); // anticlockwise_rotation
    payloadWriter.writeUnsignedExpGolomb(0);    // repetition period: this frame only
    payloadWriter.writeBits(0, 1);              // display_orientation_extension_flag
    payloadWriter.alignPayload();
    const std::vector<guint8> &payloadBytes = payloadWriter.bytes();

    std::vector<guint8> rbspBytes;
    rbspBytes.push_back(kDisplayOrientationPayloadType);
    rbspBytes.push_back(static_cast<guint8>(payloadBytes.size()));
    rbspBytes.insert(rbspBytes.end(), payloadBytes.begin(), payloadBytes.end());
    rbspBytes.push_back(kRbspStopBits);

    std::vector<guint8> nalUnit(std::begin(kAnnexBStartCode), std::end(kAnnexBStartCode));
    nalUnit.push_back(kSeiNalHeader);
    appendWithEmulationPrevention(nalUnit, rbspBytes);
    return nalUnit;
}

// Payloader input probe: returns a new buffer holding the orientation SEI
// followed by the encoded frame. The frame's data is shared, not copied,
// and the encoder's own buffer is left unmodified so it can go straight
// back to the encoder's buffer pool.
static GstPadProbeReturn onEncodedFrame(GstPad *, GstPadProbeInfo *probeInfo, gpointer seiNalData)
{
    const auto *orientationSeiNal = static_cast<const std::vector<guint8> *>(seiNalData);
    GstBuffer *encodedFrame = GST_PAD_PROBE_INFO_BUFFER(probeInfo);

    GstBuffer *taggedFrame = gst_buffer_new();
    GstMemory *seiMemory = gst_memory_new_wrapped(
        GST_MEMORY_FLAG_READONLY,
        const_cast<guint8 *>(orientationSeiNal->data()), orientationSeiNal->size(),
        0, orientationSeiNal->size(), nullptr, nullptr);
    gst_buffer_append_memory(taggedFrame, seiMemory);
    gst_buffer_copy_into(taggedFrame, encodedFrame,
                         GstBufferCopyFlags(GST_BUFFER_COPY_METADATA | GST_BUFFER_COPY_MEMORY),
                         0, static_cast<gsize>(-1));

    gst_buffer_unref(encodedFrame);
    GST_PAD_PROBE_INFO_DATA(probeInfo) = taggedFrame;
    return GST_PAD_PROBE_OK;
}

// Installs the orientation probe on the payloader's input in mediaBin.
static void attachOrientationTagger(GstElement *mediaBin, const std::vector<guint8> *orientationSeiNal)
{
    GstElement *payloader = gst_bin_get_by_name(GST_BIN(mediaBin), "pay0");
    if (!payloader) {
        logError("payloader not found; stream will not be rotated");
        return;
    }
    GstPad *payloaderInput = gst_element_get_static_pad(payloader, "sink");
    gst_pad_add_probe(payloaderInput, GST_PAD_PROBE_TYPE_BUFFER, onEncodedFrame,
                      const_cast<std::vector<guint8> *>(orientationSeiNal), nullptr);
    gst_object_unref(payloaderInput);
    gst_object_unref(payloader);
}

// ---- Pipeline --------------------------------------------------------------

// Builds the gst-launch style description of the camera pipeline.
//  - Frames are copied into the encoder's own buffers; handing it the
//    camera's buffers directly (dmabuf import) fails on this hardware.
//  - The encoder repeats SPS/PPS before every keyframe itself
//    (repeat_sequence_header), so no h264parse is needed and the
//    payloader doesn't inject them again (config-interval=0).
//  - No queues, so no extra threads or buffered frames.
// The caller frees the returned string with g_free().
static gchar *buildPipelineDescription(const Config &config)
{
    return g_strdup_printf(
        "( libcamerasrc sensor-config=\"sensor/config,width=%ld,height=%ld,depth=%ld\""
        " ! video/x-raw,format=I420,width=%ld,height=%ld,framerate=%ld/1"
        " ! v4l2h264enc extra-controls=\"controls"
        ",video_bitrate=%ld,h264_i_frame_period=%ld,repeat_sequence_header=1\""
        " ! video/x-h264,level=(string)4,profile=(string)high"
        " ! rtph264pay name=pay0 pt=96 config-interval=0 )",
        config.sensorWidth, config.sensorHeight, config.sensorBitDepth,
        config.width, config.height, config.framesPerSecond,
        config.bitrate, config.keyframeInterval);
}

// ---- Runtime callbacks -----------------------------------------------------

// Bus sync handler: logs pipeline errors. Runs in the thread that posted
// the message and passes every message on to the RTSP server's own bus
// watch, so it adds no thread or queue.
static GstBusSyncReply onBusSyncMessage(GstBus *, GstMessage *message, gpointer)
{
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError *pipelineError = nullptr;
        gchar *debugDetails = nullptr;
        gst_message_parse_error(message, &pipelineError, &debugDetails);
        logError("error from %s: %s", GST_MESSAGE_SRC_NAME(message),
                 pipelineError ? pipelineError->message : "unknown");
        if (debugDetails)
            logError("  %s", debugDetails);
        g_clear_error(&pipelineError);
        g_free(debugDetails);
    }
    return GST_BUS_PASS;
}

// Logs a media state change; the message to print is the signal's user data.
static void onMediaStateChanged(GstRTSPMedia *, gpointer stateMessage)
{
    logInfo("%s", static_cast<const char *>(stateMessage));
}

// Called once per new media pipeline: attaches the error logger to the
// pipeline's bus, adds the orientation tagger when rotating (seiNalData is
// nullptr otherwise) and logs when the camera starts and stops.
static void onMediaConfigure(GstRTSPMediaFactory *, GstRTSPMedia *media, gpointer seiNalData)
{
    GstElement *mediaBin = gst_rtsp_media_get_element(media);
    if (mediaBin) {
        if (seiNalData)
            attachOrientationTagger(mediaBin, static_cast<const std::vector<guint8> *>(seiNalData));
        GstObject *pipeline = gst_object_get_parent(GST_OBJECT(mediaBin));
        if (pipeline && GST_IS_PIPELINE(pipeline)) {
            GstBus *pipelineBus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
            gst_bus_set_sync_handler(pipelineBus, onBusSyncMessage, nullptr, nullptr);
            gst_object_unref(pipelineBus);
        }
        if (pipeline)
            gst_object_unref(pipeline);
        gst_object_unref(mediaBin);
    }
    g_signal_connect(media, "prepared", G_CALLBACK(onMediaStateChanged),
                     const_cast<char *>("camera streaming"));
    g_signal_connect(media, "unprepared", G_CALLBACK(onMediaStateChanged),
                     const_cast<char *>("camera released"));
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

// Logs a new client and arranges for its disconnect to be logged too.
// The IP address copy is freed automatically with the signal handler.
static void onClientConnected(GstRTSPServer *, GstRTSPClient *client, gpointer)
{
    GstRTSPConnection *connection = gst_rtsp_client_get_connection(client);
    const char *connectionAddress = connection ? gst_rtsp_connection_get_ip(connection) : nullptr;
    gchar *clientAddress = g_strdup(connectionAddress ? connectionAddress : "?");
    logInfo("client %s connected", clientAddress);
    g_signal_connect_data(client, "closed", G_CALLBACK(onClientClosed), clientAddress,
                          freeClientAddress, GConnectFlags(0));
}

// Periodic timer: removes expired sessions so their pipeline can stop.
static gboolean onSessionCleanup(gpointer rtspServer)
{
    GstRTSPSessionPool *sessionPool =
        gst_rtsp_server_get_session_pool(static_cast<GstRTSPServer *>(rtspServer));
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

// Creates the RTSP server with one shared media factory mounted at the
// configured path. shared=TRUE lets every viewer reuse the same capture and
// encode; the pipeline stops (camera released) when the last client leaves.
// orientationSeiNal is the rotation message to tag frames with, or nullptr.
static GstRTSPServer *createRtspServer(const Config &config,
                                       const std::vector<guint8> *orientationSeiNal)
{
    GstRTSPServer *rtspServer = gst_rtsp_server_new();
    gchar *portText = g_strdup_printf("%ld", config.port);
    gst_rtsp_server_set_address(rtspServer, kListenAddress);
    gst_rtsp_server_set_service(rtspServer, portText);
    g_free(portText);
    g_signal_connect(rtspServer, "client-connected", G_CALLBACK(onClientConnected), nullptr);

    gchar *pipelineDescription = buildPipelineDescription(config);
    GstRTSPMediaFactory *mediaFactory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(mediaFactory, pipelineDescription);
    gst_rtsp_media_factory_set_shared(mediaFactory, TRUE);
    gst_rtsp_media_factory_set_protocols(mediaFactory, GST_RTSP_LOWER_TRANS_TCP);
    g_signal_connect(mediaFactory, "media-configure", G_CALLBACK(onMediaConfigure),
                     const_cast<std::vector<guint8> *>(orientationSeiNal));
    g_free(pipelineDescription);

    GstRTSPMountPoints *mountPoints = gst_rtsp_server_get_mount_points(rtspServer);
    gst_rtsp_mount_points_add_factory(mountPoints, config.mountPath, mediaFactory);  // takes ownership
    g_object_unref(mountPoints);
    return rtspServer;
}

// Logs one viewer URL for the given host.
static void logStreamUrl(const char *host, const Config &config)
{
    logInfo("  rtsp://%s:%ld%s", host, config.port, config.mountPath);
}

// Logs a usable URL for every IPv4 address the server is reachable on.
static void logStreamUrls(const Config &config)
{
    if (std::strcmp(kListenAddress, "0.0.0.0") != 0) {
        logStreamUrl(kListenAddress, config);
        return;
    }
    struct ifaddrs *interfaceList = nullptr;
    if (getifaddrs(&interfaceList) != 0) {
        logStreamUrl("<pi-address>", config);
        return;
    }
    for (struct ifaddrs *networkInterface = interfaceList; networkInterface;
         networkInterface = networkInterface->ifa_next) {
        if (!networkInterface->ifa_addr || networkInterface->ifa_addr->sa_family != AF_INET ||
            !(networkInterface->ifa_flags & IFF_UP))
            continue;
        char addressText[INET_ADDRSTRLEN];
        const auto *ipv4Address = reinterpret_cast<const struct sockaddr_in *>(networkInterface->ifa_addr);
        if (inet_ntop(AF_INET, &ipv4Address->sin_addr, addressText, sizeof addressText))
            logStreamUrl(addressText, config);
    }
    freeifaddrs(interfaceList);
}

// Removes a main-loop source if it still exists (it may already have
// removed itself, as the shutdown signal handlers do).
static void removeSourceIfPresent(guint sourceId)
{
    if (GSource *source = g_main_context_find_source_by_id(nullptr, sourceId))
        g_source_destroy(source);
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
    if (!parseArguments(argc, argv, config)) {
        printUsage(argv[0]);
        return 1;
    }
    if (!checkRequiredElements())
        return 1;

    // Built once and shared by every pipeline; lives until main returns.
    std::vector<guint8> orientationSeiNal;
    if (config.rotationDegrees != 0)
        orientationSeiNal = buildOrientationSeiNal(config.rotationDegrees);
    GstRTSPServer *rtspServer =
        createRtspServer(config, orientationSeiNal.empty() ? nullptr : &orientationSeiNal);

    // create_source binds the socket now and reports why if it can't.
    GError *bindError = nullptr;
    GSource *serverSource = gst_rtsp_server_create_source(rtspServer, nullptr, &bindError);
    if (!serverSource) {
        logError("cannot listen on %s:%ld: %s", kListenAddress, config.port,
                 bindError ? bindError->message : "unknown error");
        g_clear_error(&bindError);
        g_object_unref(rtspServer);
        return 1;
    }
    g_source_attach(serverSource, nullptr);

    mainLoop = g_main_loop_new(nullptr, FALSE);
    guint sessionCleanupSourceId =
        g_timeout_add_seconds(kSessionCleanupSeconds, onSessionCleanup, rtspServer);
    guint signalSourceIds[G_N_ELEMENTS(kShutdownSignals)];
    for (gsize signalIndex = 0; signalIndex < G_N_ELEMENTS(kShutdownSignals); ++signalIndex)
        signalSourceIds[signalIndex] =
            g_unix_signal_add(kShutdownSignals[signalIndex], onShutdownSignal, nullptr);

    logInfo("%ldx%ld@%ld %ld bit/s, keyframe every %ld, sensor %ldx%ld:%ld, rotation %ld",
            config.width, config.height, config.framesPerSecond, config.bitrate,
            config.keyframeInterval, config.sensorWidth, config.sensorHeight,
            config.sensorBitDepth, config.rotationDegrees);
    logStreamUrls(config);

    g_main_loop_run(mainLoop);

    logInfo("shutting down");
    removeSourceIfPresent(sessionCleanupSourceId);
    for (guint signalSourceId : signalSourceIds)
        removeSourceIfPresent(signalSourceId);
    g_source_destroy(serverSource);
    g_source_unref(serverSource);
    g_object_unref(rtspServer);
    g_main_loop_unref(mainLoop);
    return 0;
}
