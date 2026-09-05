// fcproxy.cpp — OPC brightness proxy in front of the Fadecandy server
//
// Effect programs connect here on 7890; frames are rescaled to the current
// brightness and forwarded to fcserver on 7891. Brightness is read and written
// over the HTTP API on 7892.
//
// Build:
//   g++ -std=c++17 -O3 -march=native -mtune=native
//       fcproxy.cpp opcutils.cpp httpapiserver.cpp -o fcproxy -pthread
//
// Usage:
//   ./fcproxy [rowLength] [fcserverGamma] [outputHz]
//
//   rowLength     pixels per row for spatial dithering; 0 (default) treats each
//                 OPC channel as one strip. For the 8x60 strips, 60 makes the
//                 dither diffuse in two dimensions across the strips.
//   fcserverGamma the gamma in your fcserver config (default 2.5). This must
//                 match or brightness scaling will be off — see below.
//   outputHz      forwarding cadence (default 60).
//
// Why brightness is a plain linear multiply here and not a gamma curve:
// fcserver already applies its own gamma + whitepoint LUT and expands to 16-bit
// before the Fadecandy firmware dithers it. Applying a second gamma in the proxy
// stacks the exponents — 2.2 here on top of fcserver's 2.5 gives an effective
// 5.5 — which is what crushes the bottom of the range. Working in fcserver's
// code space instead, a luminance scale S is exactly a linear code multiply of
// S^(1/gamma), because the power law commutes. At 1% luminance that leaves ~40
// distinct codes rather than ~2, and the dither recovers the fraction below the
// last code, so the effective depth is closer to 12 bits.
//
// The theoretical best would be to leave the pixel data untouched and set
// fcserver's whitepoint over its WebSocket API, since that scales inside the
// 16-bit LUT. This proxy stays a pure OPC pass-through instead, which keeps it
// independent of the fcserver build and config.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "httpapiserver.h"
#include "opcutils.h"

// --- System Constants ---
constexpr uint16_t OPC_LISTEN_PORT = 7890;
constexpr uint16_t OPC_TARGET_PORT = 7891;
constexpr uint16_t HTTP_LISTEN_PORT = 7892;
constexpr int HTTP_RATE_LIMIT_MS = 100;
constexpr const char* LOOPBACK_IP = "127.0.0.1";
constexpr int INITIAL_BRIGHTNESS = 175;

// The original code never let the output reach zero: at slider 0 it still
// emitted 10% in its own post-gamma code space, which fcserver's 2.5 gamma
// turned into roughly 0.3% luminance. Kept at that luminance so the panel
// behaves the same at the bottom of the slider.
constexpr float MIN_LUMINANCE_FLOOR = 0.003f;

constexpr double DEFAULT_OUTPUT_HZ = 60.0;
constexpr float DEFAULT_FCSERVER_GAMMA = 2.5f;

// Fadecandy interpolates between frames and dithers to 16 bits in firmware, so
// a temporally dithered code never reads as flicker. That makes it safe to keep
// almost every pixel on the temporal path, where the per-pixel time average is
// exact and there is no spatial grain; only the darkest few codes fall back to
// spatial diffusion, which is what preserves structure when codes run out.
constexpr float PROXY_CROSSOVER_LEVEL = 3.0f;

constexpr int SOCKET_POLL_TIMEOUT_MS = 100;
constexpr int MAX_OPC_CLIENTS = 8;
constexpr int RECONNECT_BACKOFF_MS = 500;
constexpr int OPC_CHANNEL_COUNT = 256;

// Even with nothing changing, each channel is re-sent at this interval. It is
// not needed to hold the image — fcserver keeps the last frame — but fcserver
// times its frame interpolation from the gap between arriving frames. After a
// long silence that estimate goes stale, and the next frame (a brightness
// change on a still image, say) can ramp in slowly instead of applying at once.
// Four frames a second of 1.7 kB keeps the estimate fresh for free.
constexpr int KEEPALIVE_INTERVAL_MS = 250;

// --- Configuration ---
int configuredRowLength = 0;
float fcserverGamma = DEFAULT_FCSERVER_GAMMA;
double outputHz = DEFAULT_OUTPUT_HZ;

// --- Shared State ---
std::atomic<int> apiBrightnessLevel{INITIAL_BRIGHTNESS};

// ---------------------------------------------------------------------------
// Outbound link to fcserver
// ---------------------------------------------------------------------------

// One persistent connection shared by every inbound client. The original opened
// a separate fcserver connection per client plus another one per slider move,
// which meant duplicate frames racing each other into fcserver.
class OutboundLink {
public:
    bool sendMessage(const uint8_t* header, const uint8_t* payload, size_t payloadLength) {
        std::lock_guard<std::mutex> lockGuard(mutex_);
        if (!ensureConnectedLocked()) {
            return false;
        }
        if (!opcutils::sendAll(socketDescriptor_, header, opcutils::OPC_HEADER_LEN)) {
            dropLocked();
            return false;
        }
        if (payloadLength > 0 && !opcutils::sendAll(socketDescriptor_, payload, payloadLength)) {
            dropLocked();
            return false;
        }
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lockGuard(mutex_);
        dropLocked();
    }

    bool isConnected() {
        std::lock_guard<std::mutex> lockGuard(mutex_);
        return socketDescriptor_ >= 0;
    }

private:
    bool ensureConnectedLocked() {
        if (socketDescriptor_ >= 0) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now < nextRetryTime_) {
            return false;
        }
        socketDescriptor_ = opcutils::connectTo(LOOPBACK_IP, OPC_TARGET_PORT, 1000);
        if (socketDescriptor_ < 0) {
            nextRetryTime_ = now + std::chrono::milliseconds(RECONNECT_BACKOFF_MS);
            return false;
        }
        if (!announcedOnce_) {
            std::cerr << "Connected to fcserver on port " << OPC_TARGET_PORT << "\n";
            announcedOnce_ = true;
        }
        return true;
    }

    void dropLocked() {
        if (socketDescriptor_ >= 0) {
            ::close(socketDescriptor_);
            socketDescriptor_ = -1;
            nextRetryTime_ =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(RECONNECT_BACKOFF_MS);
        }
    }

    std::mutex mutex_;
    int socketDescriptor_ = -1;
    bool announcedOnce_ = false;
    std::chrono::steady_clock::time_point nextRetryTime_{};
};

OutboundLink outboundLink;

// ---------------------------------------------------------------------------
// Per-OPC-channel state
// ---------------------------------------------------------------------------

// The ring and the strips arrive as separate OPC channels, so frame caches and
// dither state are per channel. The original kept a single global cache, which
// meant a brightness change replayed only whichever device sent last.
struct ChannelState {
    // Guarded by channelMutex.
    std::vector<uint8_t> latestFrame;
    uint32_t sequence = 0;
    bool everReceived = false;

    // Emitter thread only.
    std::vector<uint8_t> renderSource;
    std::vector<uint8_t> renderOutput;
    opcutils::Ditherer ditherer;
    uint32_t renderedSequence = 0;
    int configuredPixels = 0;
    bool emitPending = false;
    bool ditherSteady = true;
    std::chrono::steady_clock::time_point lastEmitTime{};
};

std::mutex channelMutex;
std::array<ChannelState, OPC_CHANNEL_COUNT> opcChannels;
std::vector<int> activeChannels;  // guarded by channelMutex

void recordIncomingFrame(uint8_t channelId, const uint8_t* payload, size_t payloadLength) {
    ChannelState& state = opcChannels[channelId];
    std::lock_guard<std::mutex> lockGuard(channelMutex);

    if (state.latestFrame.size() != payloadLength) {
        state.latestFrame.assign(payloadLength, 0);
    }
    std::memcpy(state.latestFrame.data(), payload, payloadLength);
    ++state.sequence;

    if (!state.everReceived) {
        state.everReceived = true;
        activeChannels.push_back(channelId);
    }
}

// ---------------------------------------------------------------------------
// Inbound OPC receiver
// ---------------------------------------------------------------------------

struct ProxyClient {
    int socketDescriptor = -1;
    opcutils::OpcStreamParser parser;
};

void runInboundReceiver() {
    // INADDR_ANY preserved: effect programs may run on other hosts.
    const int listenSocket =
        opcutils::createListenSocket(INADDR_ANY, OPC_LISTEN_PORT, MAX_OPC_CLIENTS);
    if (listenSocket < 0) {
        opcutils::isRunning.store(false);
        return;
    }
    std::cout << "OPC Proxy Active on Port " << OPC_LISTEN_PORT << std::endl;

    std::vector<ProxyClient> connectedClients;
    std::vector<pollfd> pollDescriptors;
    std::vector<uint8_t> readScratch(16384);

    while (opcutils::isRunning.load(std::memory_order_relaxed)) {
        pollDescriptors.clear();
        pollDescriptors.push_back(pollfd{listenSocket, POLLIN, 0});
        for (const ProxyClient& client : connectedClients) {
            pollDescriptors.push_back(pollfd{client.socketDescriptor, POLLIN, 0});
        }

        const int readyCount =
            poll(pollDescriptors.data(), pollDescriptors.size(), SOCKET_POLL_TIMEOUT_MS);
        if (readyCount < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (readyCount == 0) continue;

        if ((pollDescriptors[0].revents & POLLIN) != 0) {
            for (;;) {
                const int clientSocket = accept(listenSocket, nullptr, nullptr);
                if (clientSocket < 0) break;
                if (static_cast<int>(connectedClients.size()) >= MAX_OPC_CLIENTS) {
                    ::close(clientSocket);
                    continue;
                }
                opcutils::setSocketNoDelay(clientSocket);
                opcutils::setSocketNonBlocking(clientSocket);

                ProxyClient newClient;
                newClient.socketDescriptor = clientSocket;
                connectedClients.push_back(std::move(newClient));
            }
        }

        for (size_t clientIndex = 0; clientIndex < connectedClients.size();) {
            const short events = pollDescriptors[clientIndex + 1].revents;
            bool shouldClose = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;

            if (!shouldClose && (events & POLLIN) != 0) {
                ProxyClient& client = connectedClients[clientIndex];
                for (;;) {
                    const ssize_t bytesRead = recv(client.socketDescriptor, readScratch.data(),
                                                   readScratch.size(), 0);
                    if (bytesRead > 0) {
                        client.parser.append(readScratch.data(), static_cast<size_t>(bytesRead));
                        if (bytesRead < static_cast<ssize_t>(readScratch.size())) break;
                        continue;
                    }
                    if (bytesRead == 0) {
                        shouldClose = true;
                        break;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    shouldClose = true;
                    break;
                }

                opcutils::OpcMessage message;
                while (client.parser.next(message)) {
                    if (message.command == opcutils::OPC_COMMAND_SET_PIXELS) {
                        if (message.length > 0) {
                            recordIncomingFrame(message.channel, message.payload, message.length);
                        }
                    } else {
                        // Everything that is not pixel data is forwarded byte for
                        // byte and immediately: system-exclusive messages carry
                        // Fadecandy config and must not be rescaled or delayed.
                        uint8_t passthroughHeader[opcutils::OPC_HEADER_LEN] = {
                            message.channel, message.command,
                            static_cast<uint8_t>((message.length >> 8) & 0xFF),
                            static_cast<uint8_t>(message.length & 0xFF)};
                        outboundLink.sendMessage(passthroughHeader, message.payload,
                                                 message.length);
                    }
                }
                client.parser.compact();

                if (client.parser.overflowed()) {
                    shouldClose = true;
                }
            }

            if (shouldClose) {
                ::close(connectedClients[clientIndex].socketDescriptor);
                connectedClients.erase(connectedClients.begin() + static_cast<long>(clientIndex));
                break;  // poll indices are stale; rebuild next iteration
            }
            ++clientIndex;
        }
    }

    for (const ProxyClient& client : connectedClients) {
        ::close(client.socketDescriptor);
    }
    ::close(listenSocket);
}

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

// Luminance scale for the slider, floored so the output never goes fully dark.
float sliderToLuminanceScale(int sliderValue) {
    const float perceptual = opcutils::sliderToPerceptualScale(sliderValue);
    return MIN_LUMINANCE_FLOOR + (perceptual * (1.0f - MIN_LUMINANCE_FLOOR));
}

// A luminance scale S becomes a linear multiply of S^(1/gamma) in the code space
// fcserver expects, because its LUT raises the code to that same gamma.
float sliderToCodeScale(int sliderValue) {
    const float luminanceScale = sliderToLuminanceScale(sliderValue);
    return std::pow(luminanceScale, 1.0f / std::max(1.0f, fcserverGamma));
}

// ---------------------------------------------------------------------------
// Emitter
// ---------------------------------------------------------------------------

void configureChannelIfNeeded(ChannelState& state, size_t payloadLength) {
    const int pixelCount = static_cast<int>(payloadLength / 3);
    if (pixelCount == state.configuredPixels && pixelCount > 0) {
        return;
    }

    int rowWidth = pixelCount;
    int rowCount = 1;
    if (configuredRowLength > 1 && pixelCount >= configuredRowLength &&
        (pixelCount % configuredRowLength) == 0) {
        rowWidth = configuredRowLength;
        rowCount = pixelCount / configuredRowLength;
    }

    state.ditherer.configure(rowWidth, rowCount, PROXY_CROSSOVER_LEVEL);
    state.renderOutput.assign(static_cast<size_t>(pixelCount) * 3, 0);
    state.configuredPixels = pixelCount;
    state.ditherSteady = false;

    std::cerr << "Channel configured: " << pixelCount << " pixels, dither grid " << rowWidth << "x"
              << rowCount << "\n";
}

int main(int argumentCount, char* argumentValues[]) {
    if (argumentCount > 1) {
        configuredRowLength = std::max(0, std::atoi(argumentValues[1]));
    }
    if (argumentCount > 2) {
        const float parsedGamma = static_cast<float>(std::atof(argumentValues[2]));
        if (parsedGamma >= 1.0f && parsedGamma <= 4.0f) {
            fcserverGamma = parsedGamma;
        } else {
            std::cerr << "Invalid fcserver gamma, using " << fcserverGamma << "\n";
        }
    }
    if (argumentCount > 3) {
        const double parsedHz = std::atof(argumentValues[3]);
        if (parsedHz >= 10.0 && parsedHz <= 200.0) {
            outputHz = parsedHz;
        } else {
            std::cerr << "Invalid output rate, using " << outputHz << " Hz\n";
        }
    }

    opcutils::installSignalHandlers();

    // fcserver applies the gamma, so the proxy's own curve is linear. It only
    // bends below a couple of codes of headroom, where a straight line would
    // leave the picture as isolated dots.
    opcutils::ToneCurve proxyTone;
    proxyTone.gammaFull = 1.0f;
    proxyTone.gammaFloor = 1.0f;  // equal to gammaFull: no shadow compression
    proxyTone.headroomFull = 24.0f;
    proxyTone.headroomFloor = 2.0f;

    uint32_t scaleTable[256];
    int lastBrightnessLevel = apiBrightnessLevel.load();
    opcutils::buildScaleTable(sliderToCodeScale(lastBrightnessLevel), proxyTone, scaleTable);

    HttpApiServer httpServer(HTTP_LISTEN_PORT, HTTP_RATE_LIMIT_MS);
    httpServer.setCallbacks(
        []() -> int { return apiBrightnessLevel.load(std::memory_order_relaxed); },
        [](int newBrightnessLevel) -> void {
            // The setter now only stores the value. Rebuilding tables and
            // re-sending frames happens on the emitter thread, so dragging the
            // slider cannot stall the HTTP server on a socket connect.
            apiBrightnessLevel.store(std::clamp(newBrightnessLevel, 0, 255),
                                     std::memory_order_relaxed);
        });
    httpServer.startServer();

    std::thread receiverThread(runInboundReceiver);

    opcutils::FramePacer pacer;
    pacer.start(outputHz);
    std::cerr << "Forwarding at " << pacer.currentHz() << " Hz, fcserver gamma " << fcserverGamma
              << "\n";

    std::vector<int> emitOrder;
    uint8_t outboundHeader[opcutils::OPC_HEADER_LEN];

    while (opcutils::isRunning.load(std::memory_order_relaxed)) {
        const int currentBrightnessLevel = apiBrightnessLevel.load(std::memory_order_relaxed);
        const bool brightnessChanged = (currentBrightnessLevel != lastBrightnessLevel);
        if (brightnessChanged) {
            opcutils::buildScaleTable(sliderToCodeScale(currentBrightnessLevel), proxyTone,
                                      scaleTable);
            lastBrightnessLevel = currentBrightnessLevel;
        }

        // One lock per cycle, holding it only long enough to snapshot frames
        // that changed. Rendering and sending happen outside it.
        {
            std::lock_guard<std::mutex> lockGuard(channelMutex);
            emitOrder = activeChannels;
            for (const int channelId : emitOrder) {
                ChannelState& state = opcChannels[channelId];
                if (state.sequence != state.renderedSequence) {
                    state.renderSource = state.latestFrame;
                    state.renderedSequence = state.sequence;
                    state.emitPending = true;
                }
            }
        }

        bool didWork = false;
        for (const int channelId : emitOrder) {
            ChannelState& state = opcChannels[channelId];
            if (state.renderSource.size() < 3) {
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            const bool keepaliveDue =
                (now - state.lastEmitTime) >= std::chrono::milliseconds(KEEPALIVE_INTERVAL_MS);
            if (!state.emitPending && state.ditherSteady && !brightnessChanged && !keepaliveDue) {
                continue;  // nothing changed, dither settled, keepalive not due
            }

            configureChannelIfNeeded(state, state.renderSource.size());
            const size_t payloadLength = static_cast<size_t>(state.configuredPixels) * 3;
            if (payloadLength == 0) {
                continue;
            }

            state.ditherSteady =
                state.ditherer.render(state.renderSource.data(), scaleTable,
                                      state.renderOutput.data());
            state.emitPending = false;

            opcutils::writePixelHeader(outboundHeader, static_cast<uint8_t>(channelId),
                                       payloadLength);
            outboundLink.sendMessage(outboundHeader, state.renderOutput.data(), payloadLength);
            state.lastEmitTime = now;
            didWork = true;
        }

        if (didWork) {
            pacer.noteFrameRendered();
        }
        pacer.waitForNextFrame();
    }

    opcutils::isRunning.store(false);
    if (receiverThread.joinable()) {
        receiverThread.join();
    }
    httpServer.stopServer();
    outboundLink.close();
    return 0;
}
