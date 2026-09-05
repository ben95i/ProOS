#include "opcutils.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include <arpa/inet.h>
#include <fcntl.h>
#include <grp.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pwd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace opcutils {

std::atomic<bool> isRunning{true};

// ---------------------------------------------------------------------------
// OPC protocol
// ---------------------------------------------------------------------------

void OpcStreamParser::append(const uint8_t* data, size_t length) {
    buffer_.insert(buffer_.end(), data, data + length);
}

bool OpcStreamParser::next(OpcMessage& message) {
    if ((buffer_.size() - parseOffset_) < OPC_HEADER_LEN) {
        return false;
    }

    const size_t payloadLength = (static_cast<size_t>(buffer_[parseOffset_ + 2]) << 8) |
                                 buffer_[parseOffset_ + 3];
    if ((buffer_.size() - parseOffset_ - OPC_HEADER_LEN) < payloadLength) {
        return false;  // message still in flight
    }

    message.channel = buffer_[parseOffset_];
    message.command = buffer_[parseOffset_ + 1];
    message.payload = payloadLength > 0 ? (buffer_.data() + parseOffset_ + OPC_HEADER_LEN) : nullptr;
    message.length = payloadLength;

    parseOffset_ += OPC_HEADER_LEN + payloadLength;
    return true;
}

void OpcStreamParser::compact() {
    if (parseOffset_ == 0) {
        return;
    }
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<long>(parseOffset_));
    parseOffset_ = 0;
}

void OpcStreamParser::reset() {
    buffer_.clear();
    parseOffset_ = 0;
}

void writePixelHeader(uint8_t header[OPC_HEADER_LEN], uint8_t channel, size_t payloadLength) {
    header[0] = channel;
    header[1] = OPC_COMMAND_SET_PIXELS;
    header[2] = static_cast<uint8_t>((payloadLength >> 8) & 0xFF);
    header[3] = static_cast<uint8_t>(payloadLength & 0xFF);
}

// ---------------------------------------------------------------------------
// Sockets
// ---------------------------------------------------------------------------

bool setSocketNonBlocking(int socketDescriptor) {
    const int currentFlags = fcntl(socketDescriptor, F_GETFL, 0);
    if (currentFlags < 0) {
        return false;
    }
    return fcntl(socketDescriptor, F_SETFL, currentFlags | O_NONBLOCK) == 0;
}

bool setSocketNoDelay(int socketDescriptor) {
    int enabled = 1;
    return setsockopt(socketDescriptor, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0;
}

bool sendAll(int socketDescriptor, const void* data, size_t length, int timeoutMs) {
    const uint8_t* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = length;

    while (remaining > 0) {
        const ssize_t written = send(socketDescriptor, cursor, remaining, MSG_NOSIGNAL);
        if (written > 0) {
            cursor += written;
            remaining -= static_cast<size_t>(written);
            continue;
        }
        if (written == 0) {
            return false;
        }
        if (errno == EINTR) {
            if (!isRunning.load(std::memory_order_relaxed)) return false;
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pollfd descriptor{socketDescriptor, POLLOUT, 0};
            const int ready = poll(&descriptor, 1, timeoutMs);
            if (ready > 0) continue;
            return false;  // timeout or error: a partial frame would desync fcserver
        }
        return false;
    }
    return true;
}

int createListenSocket(uint32_t hostOrderAddress, uint16_t port, int backlog) {
    const int listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket < 0) {
        std::cerr << "socket() failed: " << std::strerror(errno) << "\n";
        return -1;
    }

    int reuseEnabled = 1;
    setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuseEnabled, sizeof(reuseEnabled));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(hostOrderAddress);

    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        std::cerr << "bind(" << port << ") failed: " << std::strerror(errno) << "\n";
        close(listenSocket);
        return -1;
    }
    if (listen(listenSocket, backlog) < 0) {
        std::cerr << "listen(" << port << ") failed: " << std::strerror(errno) << "\n";
        close(listenSocket);
        return -1;
    }

    setSocketNonBlocking(listenSocket);
    return listenSocket;
}

int connectTo(const char* dottedIp, uint16_t port, int timeoutMs) {
    const int outboundSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (outboundSocket < 0) {
        return -1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, dottedIp, &address.sin_addr) != 1) {
        close(outboundSocket);
        return -1;
    }

    // Non-blocking connect so a dead fcserver cannot stall the caller.
    setSocketNonBlocking(outboundSocket);
    const int result =
        connect(outboundSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address));

    if (result != 0) {
        if (errno != EINPROGRESS) {
            close(outboundSocket);
            return -1;
        }
        pollfd descriptor{outboundSocket, POLLOUT, 0};
        if (poll(&descriptor, 1, timeoutMs) <= 0) {
            close(outboundSocket);
            return -1;
        }
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(outboundSocket, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) != 0 ||
            socketError != 0) {
            close(outboundSocket);
            return -1;
        }
    }

    setSocketNoDelay(outboundSocket);
    return outboundSocket;
}

// ---------------------------------------------------------------------------
// Process lifecycle
// ---------------------------------------------------------------------------

extern "C" void handleTerminationSignal(int) {
    isRunning.store(false, std::memory_order_relaxed);
}

void installSignalHandlers() {
    struct sigaction terminationAction {};
    terminationAction.sa_handler = handleTerminationSignal;
    sigemptyset(&terminationAction.sa_mask);
    terminationAction.sa_flags = 0;  // no SA_RESTART: blocking calls return EINTR
    sigaction(SIGINT, &terminationAction, nullptr);
    sigaction(SIGTERM, &terminationAction, nullptr);

    struct sigaction ignoreAction {};
    ignoreAction.sa_handler = SIG_IGN;
    sigemptyset(&ignoreAction.sa_mask);
    sigaction(SIGPIPE, &ignoreAction, nullptr);
}

bool dropProcessPrivileges(const char* userName) {
    struct passwd* processUser = getpwnam(userName);
    if (processUser == nullptr) {
        std::cerr << "Warning: user '" << userName
                  << "' not found, continuing with current privileges\n";
        return false;
    }

    if (setgroups(0, nullptr) != 0) {
        std::cerr << "Warning: setgroups() failed: " << std::strerror(errno) << "\n";
    }
    if (setgid(processUser->pw_gid) != 0) {
        std::cerr << "Warning: setgid() failed: " << std::strerror(errno) << "\n";
        return false;
    }
    if (setuid(processUser->pw_uid) != 0) {
        std::cerr << "Warning: setuid() failed: " << std::strerror(errno) << "\n";
        return false;
    }
    if (setuid(0) == 0) {
        std::cerr << "Fatal: privileges were not actually dropped\n";
        std::abort();
    }
    std::cerr << "Process running as user: " << userName << "\n";
    return true;
}

// ---------------------------------------------------------------------------
// Brightness and tone
// ---------------------------------------------------------------------------

float sliderToScale(int sliderValue, float responseGamma) {
    const float normalized = std::clamp(sliderValue, 0, 255) / 255.0f;
    if (responseGamma == 1.0f) {
        return normalized;
    }
    return std::pow(normalized, std::max(0.1f, responseGamma));
}

float sliderToPerceptualScale(int sliderValue) {
    const float lightness = (std::clamp(sliderValue, 0, 255) / 255.0f) * 100.0f;
    if (lightness <= 8.0f) {
        return lightness / 903.3f;
    }
    const float t = (lightness + 16.0f) / 116.0f;
    return t * t * t;
}

void buildScaleTable(float masterScale, const ToneCurve& tone, uint32_t table[256]) {
    const float headroom = masterScale * 255.0f;
    const float span = std::max(1e-6f, tone.headroomFull - tone.headroomFloor);
    const float blend = std::clamp((headroom - tone.headroomFloor) / span, 0.0f, 1.0f);
    const float effectiveGamma = tone.gammaFloor + ((tone.gammaFull - tone.gammaFloor) * blend);

    for (int iteratorIndex = 0; iteratorIndex < 256; ++iteratorIndex) {
        const float normalizedIndex = static_cast<float>(iteratorIndex) / 255.0f;
        const float outputValue = std::pow(normalizedIndex, effectiveGamma) * masterScale * 255.0f;
        const int32_t fixedPoint =
            static_cast<int32_t>(std::lround(outputValue * static_cast<float>(DITHER_ONE)));
        table[iteratorIndex] = static_cast<uint32_t>(std::clamp<int32_t>(fixedPoint, 0, OUTPUT_CEILING));
    }
}

// ---------------------------------------------------------------------------
// Dithering
// ---------------------------------------------------------------------------

namespace {
constexpr double R2_ALPHA_X = 0.7548776662466927;
constexpr double R2_ALPHA_Y = 0.5698402909980532;
}  // namespace

uint8_t computeOrderedThreshold(int panelX, int panelY) {
    const double raw = (panelX * R2_ALPHA_X) + (panelY * R2_ALPHA_Y);
    const double fractional = raw - std::floor(raw);
    return static_cast<uint8_t>(std::min(255, static_cast<int>(fractional * 256.0)));
}

uint8_t computeModeSelector(int slotIndex) {
    uint32_t state = static_cast<uint32_t>(slotIndex) * 2654435761u;
    state ^= state >> 15;
    state *= 0x27D4EB2Du;
    state ^= state >> 15;
    return static_cast<uint8_t>(state & 0xFFu);
}

void Ditherer::rebuildTables(float crossoverLevel) {
    const float crossover = std::max(0.5f, crossoverLevel);
    for (int outputLevel = 0; outputLevel < 256; ++outputLevel) {
        float fraction = std::clamp(static_cast<float>(outputLevel) / crossover, 0.0f, 1.0f);
        fraction = fraction * fraction * (3.0f - (2.0f * fraction));  // smoothstep
        temporalGainTable_[outputLevel] =
            static_cast<int32_t>(std::lround(fraction * static_cast<float>(DITHER_ONE)));
    }
}

void Ditherer::configure(int width, int height, float crossoverLevel) {
    width_ = std::max(1, width);
    height_ = std::max(1, height);
    const size_t pixels = static_cast<size_t>(width_) * static_cast<size_t>(height_);
    const size_t slots = pixels * 3;

    maskTable_.resize(pixels);
    selectTable_.resize(slots);
    carryTable_.assign(slots, 0);
    spatialError_.assign(slots, 0);

    for (int rowIndex = 0; rowIndex < height_; ++rowIndex) {
        for (int columnIndex = 0; columnIndex < width_; ++columnIndex) {
            const int pixelIndex = (rowIndex * width_) + columnIndex;
            maskTable_[pixelIndex] = computeOrderedThreshold(columnIndex, rowIndex);
            for (int channelIndex = 0; channelIndex < 3; ++channelIndex) {
                const int slot = (pixelIndex * 3) + channelIndex;
                selectTable_[slot] = computeModeSelector(slot);
            }
        }
    }

    rebuildTables(crossoverLevel);
}

void Ditherer::setMask(const std::vector<uint8_t>& perPixelMask) {
    if (perPixelMask.size() == maskTable_.size()) {
        maskTable_ = perPixelMask;
    }
}

void Ditherer::resetState() {
    std::fill(carryTable_.begin(), carryTable_.end(), 0);
    std::fill(spatialError_.begin(), spatialError_.end(), 0);
}

bool Ditherer::render(const uint8_t* source, const uint32_t scaleTable[256], uint8_t* destination) {
    const uint8_t* const maskTable = maskTable_.data();
    const uint8_t* const selectTable = selectTable_.data();
    int32_t* const carryTable = carryTable_.data();
    int32_t* const spatialError = spatialError_.data();
    const int32_t* const gainTable = temporalGainTable_;
    const bool oneDimensional = (height_ == 1);

    std::memset(spatialError, 0, spatialError_.size() * sizeof(int32_t));
    int32_t changeAccumulator = 0;

    for (int rowIndex = 0; rowIndex < height_; ++rowIndex) {
        const bool rowReversed = (rowIndex & 1) != 0;
        const int scanDirection = rowReversed ? -1 : 1;

        for (int stepIndex = 0; stepIndex < width_; ++stepIndex) {
            const int columnIndex = rowReversed ? (width_ - 1 - stepIndex) : stepIndex;
            const int pixelIndex = (rowIndex * width_) + columnIndex;
            const int byteOffset = pixelIndex * 3;
            const int32_t orderedThreshold = maskTable[pixelIndex];

            for (int channelIndex = 0; channelIndex < 3; ++channelIndex) {
                const int slot = byteOffset + channelIndex;

                const int32_t target = static_cast<int32_t>(scaleTable[source[slot]]) +
                                       carryTable[slot] + spatialError[slot];

                // Mode index is taken pre-quantiser so a pixel cannot oscillate
                // between the two dithering strategies frame to frame.
                int32_t outputLevel = target >> DITHER_FRACTION_BITS;
                if (outputLevel < 0) outputLevel = 0;
                else if (outputLevel > 255) outputLevel = 255;
                const bool runsTemporally = selectTable[slot] < gainTable[outputLevel];

                // Temporal pixels take the shared ordered threshold, which
                // decorrelates their phase without splitting the channels.
                // Spatial pixels round to nearest and let the diffusion carry
                // the residual, which is what preserves image structure.
                const int32_t quantiserOffset =
                    runsTemporally ? orderedThreshold : (DITHER_ONE / 2);
                int32_t quantized = (target + quantiserOffset) >> DITHER_FRACTION_BITS;
                if (quantized < 0) quantized = 0;
                else if (quantized > 255) quantized = 255;

                // The residual goes to exactly one destination — forward in time
                // or outward in space — never both, or it would show up twice.
                const int32_t residual = target - (quantized << DITHER_FRACTION_BITS);
                const int32_t newCarry = runsTemporally ? residual : 0;
                changeAccumulator |= newCarry ^ carryTable[slot];
                carryTable[slot] = newCarry;

                if (!runsTemporally && residual != 0) {
                    const int nextColumn = columnIndex + scanDirection;
                    const bool nextInRange = (nextColumn >= 0) && (nextColumn < width_);

                    if (oneDimensional) {
                        // A strip or ring has no row below, so the whole residual
                        // goes to the next pixel; splitting it would discard
                        // 9/16 of the error and darken the output.
                        if (nextInRange) {
                            spatialError[(nextColumn * 3) + channelIndex] += residual;
                        }
                    } else {
                        const int backColumn = columnIndex - scanDirection;
                        const int nextRow = rowIndex + 1;
                        const bool hasNextRow = nextRow < height_;

                        if (nextInRange) {
                            spatialError[(((rowIndex * width_) + nextColumn) * 3) + channelIndex] +=
                                (residual * 7) / 16;
                        }
                        if (hasNextRow) {
                            const int belowBase = (nextRow * width_) * 3;
                            if (backColumn >= 0 && backColumn < width_) {
                                spatialError[belowBase + (backColumn * 3) + channelIndex] +=
                                    (residual * 3) / 16;
                            }
                            spatialError[belowBase + (columnIndex * 3) + channelIndex] +=
                                (residual * 5) / 16;
                            if (nextInRange) {
                                spatialError[belowBase + (nextColumn * 3) + channelIndex] +=
                                    residual / 16;
                            }
                        }
                    }
                }

                destination[slot] = static_cast<uint8_t>(quantized);
            }
        }
    }

    return changeAccumulator == 0;
}

// ---------------------------------------------------------------------------
// Frame pacing
// ---------------------------------------------------------------------------

void FramePacer::start(double targetHz) {
    const double safeHz = std::max(1.0, targetHz);
    interval_ = std::chrono::nanoseconds(static_cast<long long>(1e9 / safeHz));
    slowestInterval_ = interval_ * 3;  // never back off past a third of the target
    deadline_ = std::chrono::steady_clock::now();
    framesInWindow_ = 0;
    lateFramesInWindow_ = 0;
}

void FramePacer::noteFrameRendered() {
    ++framesInWindow_;
    if (std::chrono::steady_clock::now() > (deadline_ + interval_)) {
        ++lateFramesInWindow_;
    }
    if (framesInWindow_ >= 512) {
        if (lateFramesInWindow_ > (framesInWindow_ / 10) && interval_ < slowestInterval_) {
            interval_ += (interval_ / 20);
            if (interval_ > slowestInterval_) {
                interval_ = slowestInterval_;
            }
        }
        framesInWindow_ = 0;
        lateFramesInWindow_ = 0;
    }
}

void FramePacer::waitForNextFrame() {
    deadline_ += interval_;
    const auto now = std::chrono::steady_clock::now();
    if (deadline_ < now) {
        deadline_ = now;  // resync instead of spinning to catch up
    }
    std::this_thread::sleep_until(deadline_);
}

double FramePacer::currentHz() const {
    if (interval_.count() <= 0) {
        return 0.0;
    }
    return 1e9 / static_cast<double>(interval_.count());
}

}  // namespace opcutils
