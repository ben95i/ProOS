// opcutils.h — code shared between opcserver.cpp and fcproxy.cpp
//
// Covers the OPC wire protocol, socket plumbing, signal handling, the
// perceptual brightness curve and the spatiotemporal dithering pipeline.

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opcutils {

// ---------------------------------------------------------------------------
// Open Pixel Control protocol
// ---------------------------------------------------------------------------

constexpr size_t OPC_HEADER_LEN = 4;
constexpr uint8_t OPC_COMMAND_SET_PIXELS = 0;
constexpr size_t OPC_MAX_PAYLOAD = 65535;

struct OpcMessage {
    uint8_t channel = 0;
    uint8_t command = 0;
    const uint8_t* payload = nullptr;  // valid until the next append()/compact()
    size_t length = 0;
};

// Incremental framing over a TCP byte stream. A short read never desynchronises
// the stream: bytes accumulate until a whole message is present.
class OpcStreamParser {
public:
    void append(const uint8_t* data, size_t length);
    bool next(OpcMessage& message);
    void compact();
    size_t buffered() const { return buffer_.size() - parseOffset_; }
    bool overflowed() const { return buffered() > (OPC_HEADER_LEN + OPC_MAX_PAYLOAD + 65536); }
    void reset();

private:
    std::vector<uint8_t> buffer_;
    size_t parseOffset_ = 0;
};

// Fills a 4-byte OPC header for a set-pixels message.
void writePixelHeader(uint8_t header[OPC_HEADER_LEN], uint8_t channel, size_t payloadLength);

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

bool setSocketNonBlocking(int socketDescriptor);
bool setSocketNoDelay(int socketDescriptor);

// Writes the whole buffer, retrying on partial writes, EINTR and EAGAIN.
// Returns false on a real error, a closed peer or timeout expiry.
bool sendAll(int socketDescriptor, const void* data, size_t length, int timeoutMs = 2000);

// hostOrderAddress: INADDR_LOOPBACK or INADDR_ANY. Returns -1 on failure.
int createListenSocket(uint32_t hostOrderAddress, uint16_t port, int backlog);

// Blocking connect with a bounded timeout. Returns -1 on failure.
int connectTo(const char* dottedIp, uint16_t port, int timeoutMs);

// ---------------------------------------------------------------------------
// Process lifecycle
// ---------------------------------------------------------------------------

extern std::atomic<bool> isRunning;

// SIGINT/SIGTERM clear isRunning; SIGPIPE ignored. Handlers are installed
// without SA_RESTART so blocking syscalls return EINTR and loops can exit.
void installSignalHandlers();

bool dropProcessPrivileges(const char* userName);

// ---------------------------------------------------------------------------
// Brightness and tone
// ---------------------------------------------------------------------------

// Q8.8 fixed point is used throughout the dither pipeline.
constexpr int DITHER_FRACTION_BITS = 8;
constexpr int32_t DITHER_ONE = 1 << DITHER_FRACTION_BITS;
constexpr int32_t OUTPUT_CEILING = 255 * DITHER_ONE;

// Slider response. responseGamma 1.0 is a straight multiply on linear light,
// which is what the original server did and what keeps the most output levels
// available at any given slider position. Raising it makes equal slider steps
// look more like equal perceived steps, at the cost of real brightness: around
// 3.0 approximates the CIE lightness curve, which is perceptually even but
// leaves a 20% slider setting nearly 7x dimmer than a linear one.
float sliderToScale(int sliderValue, float responseGamma);

// CIE 1931 lightness curve. Equivalent to sliderToScale with responseGamma near
// 3.0; kept for callers that have the headroom to spend on it.
float sliderToPerceptualScale(int sliderValue);

// Optional shadow compression. With gammaFloor equal to gammaFull (the default)
// the curve is a plain fixed gamma and nothing adapts. Setting gammaFloor below
// gammaFull fades the exponent down as the available headroom shrinks, which
// keeps dark content above the quantiser floor when there are only a handful of
// output levels left — at the cost of contrast and some desaturation, since
// lifting a dark channel more than a bright one pulls colours toward grey.
struct ToneCurve {
    float gammaFull = 2.8f;    // range 1.0-3.2, display gamma
    float gammaFloor = 2.8f;   // range 0.7-3.2, gamma once headroom runs out
    float headroomFull = 24.0f;   // range 8-64 LSB, where gammaFull applies in full
    float headroomFloor = 2.0f;   // range 1-8 LSB, where gammaFloor is reached
};

// Builds input byte -> Q8.8 output value. Call only when the scale changes.
void buildScaleTable(float masterScale, const ToneCurve& tone, uint32_t table[256]);

// ---------------------------------------------------------------------------
// Spatiotemporal dithering
// ---------------------------------------------------------------------------

// R2 low-discrepancy threshold, shared across R/G/B on purpose: per-channel
// offsets guarantee the channels never round up together, which turns every
// sub-LSB neutral pixel into a pure red, green or blue dot.
uint8_t computeOrderedThreshold(int panelX, int panelY);

// Selector deciding which pixels run temporally. Must be independent of the
// ordered threshold or the temporal subset is biased and the average drifts.
uint8_t computeModeSelector(int slotIndex);

// Each channel of each pixel is either a first-order temporal modulator (exact
// time average, no grain) or a spatial error-diffusion pixel (static, preserves
// structure at very low levels). The mix is chosen per pixel by output level.
// Set height to 1 for a strip or ring; diffusion then runs one-dimensionally.
class Ditherer {
public:
    void configure(int width, int height, float crossoverLevel = 10.0f);

    // Optional: supply a threshold per pixel keyed to physical geometry rather
    // than to the incoming image's coordinates.
    void setMask(const std::vector<uint8_t>& perPixelMask);

    void resetState();

    int width() const { return width_; }
    int height() const { return height_; }
    int pixelCount() const { return width_ * height_; }

    // source and destination are interleaved RGB, pixelCount() * 3 bytes.
    // Returns true when the dither has reached a fixed point, i.e. the next
    // frame would produce byte-identical output.
    bool render(const uint8_t* source, const uint32_t scaleTable[256], uint8_t* destination);

private:
    void rebuildTables(float crossoverLevel);

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> maskTable_;
    std::vector<uint8_t> selectTable_;
    std::vector<int32_t> carryTable_;
    std::vector<int32_t> spatialError_;
    int32_t temporalGainTable_[256] = {0};
};

// ---------------------------------------------------------------------------
// Frame pacing
// ---------------------------------------------------------------------------

// Fixed-cadence loop timer that resyncs instead of spinning when a deadline is
// missed, backs the rate off if the work consistently overruns, and works its
// way back to the target rate once the overrun has passed.
class FramePacer {
public:
    void start(double targetHz);
    void waitForNextFrame();
    void noteFrameRendered();
    double currentHz() const;

private:
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::nanoseconds interval_{0};
    std::chrono::nanoseconds targetInterval_{0};
    std::chrono::nanoseconds slowestInterval_{0};
    int framesInWindow_ = 0;
    int lateFramesInWindow_ = 0;
};

}  // namespace opcutils
