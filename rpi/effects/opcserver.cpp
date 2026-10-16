// opcserver.cpp Open Pixel Control receiver + WS2812 renderer
//
// Build:
//   g++ -std=c++17 -O3 -march=native -mtune=native
//       opcserver.cpp opcutils.cpp httpapiserver.cpp -o opcserver -lws2811 -pthread
//
// Usage:
//   sudo ./opcserver [WxHgrid] [powerLimit 1-255] [rotation 0|90|180|270] [refreshHz]
//
// The refresh rate is derived from the LED count unless argv[4] overrides it.
//
// Pipeline per channel:
//   8-bit input -> headroom-adaptive tone curve -> perceptual master scale
//   (CIE L*) -> spatiotemporal dither -> 8-bit WS2812 value
//
// The rpi_ws281x "hardware brightness" field is NOT a hardware register: the
// library multiplies every channel by (brightness + 1) / 256 in software while
// it builds the DMA bit pattern. Using it would re-quantise the dithered output,
// so the channel brightness is pinned to 255 and the power limit from argv[2] is
// folded into the software scale instead.

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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

#include <ws2811/ws2811.h>

#include "httpapiserver.h"
#include "opcutils.h"

// --- System Constants ---
// GPIO 18 is PWM0. rpi_ws281x accepts 12/18 (PWM0), 13/19 (PWM1), 21/31 (PCM)
// and 10 (SPI). Changing this without changing DMA_CHANNEL is usually fine.
constexpr int GPIO_OUTPUT_PIN = 18;
// Range 0-14. 10 is the conventional safe pick. Avoid channels the kernel is
// already using; channel 5 in particular has been linked to SD-card corruption
// on some models. If ws2811_init fails with an allocation error, try 9 or 11.
constexpr int DMA_CHANNEL = 10;
// One of WS2811_STRIP_{RGB,RBG,GRB,GBR,BRG,BGR} or SK6812_STRIP_{RGBW,...}.
// GRB is correct for standard WS2812B. An RGBW type also needs
// WS2812_BITS_PER_LED changed to 32.
constexpr int LED_STRIP_TYPE = WS2811_STRIP_GRB;
// Any existing unprivileged account. Privileges are dropped after ws2811_init
// has mapped the DMA hardware, so this account needs no special rights.
constexpr const char* SECURITY_USER = "nobody";
// Range 1024-65535 each, and the two must differ. 7890 is the OPC convention.
constexpr uint16_t OPC_LISTENER_PORT = 7890;
constexpr uint16_t HTTP_CONTROL_PORT = 7892;
// Range 0-1000 ms. Minimum spacing between accepted HTTP API calls; 0 disables
// the limit. Above ~200 a dragged brightness slider starts to feel laggy.
constexpr int HTTP_RATE_LIMIT_MS = 100;

// Range 1-1024. Rejects an implausible WxH argument before it reaches the
// mapping tables.
constexpr int MAX_MATRIX_DIMENSION = 512;
// Range 1-21845 (the OPC payload ceiling of 65535 bytes / 3). The practical
// limit is far lower: wire time is 30 us per LED, so 2000 LEDs alone caps the
// refresh near 16 Hz.
constexpr int MAX_TOTAL_LED_COUNT = 4096;
// Range 10-1000 ms. Sets how long shutdown can take, since the receiver thread
// only checks isRunning between polls. Lower costs idle wakeups.
constexpr int SOCKET_POLL_TIMEOUT_MS = 200;
// Range 1-64. Concurrent effect programs; beyond one they interleave frames
// into the same buffer, so 1-2 is the sane operating range.
constexpr int MAX_OPC_CLIENTS = 8;

// --- Refresh Rate Derivation ---
// A WS2812 needs 24 bits per LED at WS2811_TARGET_FREQ, plus a latch gap;
// rpi_ws281x uses a 300 us reset window (LED_RESET_WAIT_TIME). That sets a hard
// wire ceiling, which is what actually binds on large panels.
// Fixed by the protocol: 24 for RGB parts, 32 for RGBW (SK6812) parts.
constexpr double WS2812_BITS_PER_LED = 24.0;
// Range 0.00005-0.001 s. Must not be shorter than the latch time rpi_ws281x
// actually enforces, or the derived rate will overrun the wire.
constexpr double WS2812_RESET_SECONDS = 0.000300;
// Range 0.50-0.95. Slack left for the render maths, the DMA poll loop and
// scheduler jitter. Above ~0.95 the adaptive pacer will be backing off
// constantly; below ~0.7 you are giving away refresh rate for nothing.
constexpr double REFRESH_HEADROOM = 0.88;
// Range 24-250 Hz. On small panels the wire is not the limit the LED is. A
// WS2812B restarts its internal PWM counter (~400 Hz on classic dies) every
// time it latches, so the last PWM cycle before each latch is truncated. The
// resulting duty-cycle error grows with the update rate and its phase drifts,
// which shows up as slow whole-panel brightness beating. 60 is the safe value
// for classic dies; V5/2020 parts run ~2 kHz internally and tolerate more.
// argv[4] overrides this at runtime.
constexpr double DEFAULT_TARGET_HZ = 60.0;
// Range 10 Hz to DEFAULT_TARGET_HZ. Advisory only: it prints a warning and
// never raises the rate, because the wire ceiling cannot be exceeded.
constexpr double MIN_REFRESH_HZ = 50.0;

// --- Dither and Brightness ---
// Range 0-64 output LSBs. The level at and above which a pixel dithers purely
// in time, which is what the original server did everywhere: exact per-pixel
// average, no spatial grain. Below it the work shifts to spatial error
// diffusion, which holds a steady value rather than toggling 0<->1. Keep this
// low spatial diffusion trades smoothness for stability, so it should only
// cover levels where a temporal toggle would be near-100% modulation. 0
// disables the spatial path entirely and restores the original behaviour.
constexpr float TEMPORAL_CROSSOVER_LEVEL = 4.0f;

// Range 1.0-3.2. How the brightness slider maps to light output. 1.0 is a
// straight multiply, matching the original server: a 20% slider setting really
// is 20% of full output, so 51 of the 255 levels stay available and solid
// colours stay solid. Raising it evens out how the slider *feels* but costs
// real output at 3.0 (roughly the CIE lightness curve) a 20% setting is only
// 3% of full, leaving 7 levels, which is where dim colours break into dots.
constexpr float SLIDER_RESPONSE_GAMMA = 1.5f;

// Range 0.0-0.05 as a fraction of full luminance. Floors the master scale so
// the panel never goes completely dark at the bottom of the slider: the slider
// then spans MIN_LUMINANCE_FLOOR to 1.0 instead of 0.0 to 1.0. 0.0 keeps the
// current behaviour, where slider 0 is off. 0.003 matches the floor fcproxy
// inherited from its original code, and is roughly the dimmest setting that is
// still clearly lit in a dark room. Above ~0.02 the bottom of the slider stops
// feeling dim at all.
constexpr float MIN_LUMINANCE_FLOOR = 0.008f;

// --- Configuration (immutable once main() has parsed the command line) ---
int matrixWidth = 16;
int matrixHeight = 16;
int totalLedCount = 256;
int frameByteCount = 768;
int uiRotationDeg = 0;
int canvasWidth = 16;
int canvasHeight = 16;
float powerLimitScale = 1.0f;
double refreshOverrideHz = 0.0;

// --- Shared State ---
std::atomic<int> apiBrightnessLevel{255};
std::atomic<uint32_t> frameSequence{0};
ws2811_t ledStripDriver;

std::mutex frameMutex;
std::vector<uint8_t> sharedFrameBuffer;

std::vector<uint16_t> physicalLookupTable;
uint32_t scaledOutputTable[256];
opcutils::Ditherer panelDitherer;

// ---------------------------------------------------------------------------
// Brightness
// ---------------------------------------------------------------------------

// Perceptual slider position, floored so the output need not reach full black,
// then scaled by the argv[2] power limit.
float masterScaleForSlider(int sliderValue) {
    const float response = opcutils::sliderToScale(sliderValue, SLIDER_RESPONSE_GAMMA);
    const float floored = MIN_LUMINANCE_FLOOR + (response * (1.0f - MIN_LUMINANCE_FLOOR));
    return floored * powerLimitScale;
}

// ---------------------------------------------------------------------------
// Frame timing
// ---------------------------------------------------------------------------

double computeRefreshHz(int ledCount) {
    const double transmitSeconds =
        (static_cast<double>(ledCount) * WS2812_BITS_PER_LED) / WS2811_TARGET_FREQ;
    const double wireCeilingHz = 1.0 / (transmitSeconds + WS2812_RESET_SECONDS);

    if (refreshOverrideHz > 0.0) {
        if (refreshOverrideHz > wireCeilingHz) {
            std::cerr << "Requested " << refreshOverrideHz << " Hz exceeds the " << wireCeilingHz
                      << " Hz wire limit for " << ledCount << " LEDs, clamping\n";
            return wireCeilingHz * REFRESH_HEADROOM;
        }
        return refreshOverrideHz;
    }

    // The wire ceiling is a hard physical limit, so it always wins: on a very
    // large panel the achievable rate simply lands below MIN_REFRESH_HZ.
    const double refreshHz = std::min(wireCeilingHz * REFRESH_HEADROOM, DEFAULT_TARGET_HZ);
    if (refreshHz < MIN_REFRESH_HZ) {
        std::cerr << "Note: " << ledCount << " LEDs cap the refresh at " << refreshHz
                  << " Hz; dithering quality will suffer below ~60 Hz\n";
    }
    return refreshHz;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

void generatePhysicalLookupTable() {
    physicalLookupTable.resize(totalLedCount);
    std::vector<uint8_t> panelMask(totalLedCount);

    canvasWidth = (uiRotationDeg == 90 || uiRotationDeg == 270) ? matrixHeight : matrixWidth;
    canvasHeight = (uiRotationDeg == 90 || uiRotationDeg == 270) ? matrixWidth : matrixHeight;

    for (int iteratorIndex = 0; iteratorIndex < totalLedCount; ++iteratorIndex) {
        const int sourceX = iteratorIndex % canvasWidth;
        const int sourceY = iteratorIndex / canvasWidth;
        int rotatedX = 0;
        int rotatedY = 0;

        switch (uiRotationDeg) {
            case 90:
                rotatedX = (matrixWidth - 1) - sourceY;
                rotatedY = sourceX;
                break;
            case 180:
                rotatedX = (matrixWidth - 1) - sourceX;
                rotatedY = (matrixHeight - 1) - sourceY;
                break;
            case 270:
                rotatedX = sourceY;
                rotatedY = (matrixHeight - 1) - sourceX;
                break;
            default:
                rotatedX = sourceX;
                rotatedY = sourceY;
                break;
        }

        int physicalX = rotatedX;

        // ORIGINAL LOGIC RETAINED: reverses the EVEN rows (0, 2, 4...)
        if ((rotatedY % 2) == 0) {
            physicalX = (matrixWidth - 1) - rotatedX;
        }

        const int finalIndex = (rotatedY * matrixWidth) + physicalX;
        physicalLookupTable[iteratorIndex] =
            static_cast<uint16_t>(std::clamp(finalIndex, 0, totalLedCount - 1));

        // Keyed to the panel's geometric position, not the wire order, so the
        // noise pattern stays spatially coherent on the panel under rotation.
        panelMask[iteratorIndex] = opcutils::computeOrderedThreshold(rotatedX, rotatedY);
    }

    panelDitherer.configure(canvasWidth, canvasHeight, TEMPORAL_CROSSOVER_LEVEL);
    panelDitherer.setMask(panelMask);
}

// ---------------------------------------------------------------------------
// OPC receiver
// ---------------------------------------------------------------------------

struct OpcClient {
    int socketDescriptor = -1;
    opcutils::OpcStreamParser parser;
};

void publishFrame(const uint8_t* payload, size_t payloadLength) {
    const size_t copyLength = std::min(payloadLength, static_cast<size_t>(frameByteCount));
    {
        std::lock_guard<std::mutex> lockGuard(frameMutex);
        std::memcpy(sharedFrameBuffer.data(), payload, copyLength);
    }
    frameSequence.fetch_add(1, std::memory_order_release);
}

void runOpcReceiver() {
    const int listenSocket =
        opcutils::createListenSocket(INADDR_LOOPBACK, OPC_LISTENER_PORT, MAX_OPC_CLIENTS);
    if (listenSocket < 0) {
        opcutils::isRunning.store(false);
        return;
    }

    std::vector<OpcClient> connectedClients;
    std::vector<pollfd> pollDescriptors;
    std::vector<uint8_t> readScratch(8192);

    while (opcutils::isRunning.load(std::memory_order_relaxed)) {
        pollDescriptors.clear();
        pollDescriptors.push_back(pollfd{listenSocket, POLLIN, 0});
        for (const OpcClient& client : connectedClients) {
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
                    close(clientSocket);
                    continue;
                }
                opcutils::setSocketNoDelay(clientSocket);
                opcutils::setSocketNonBlocking(clientSocket);

                OpcClient newClient;
                newClient.socketDescriptor = clientSocket;
                connectedClients.push_back(std::move(newClient));
            }
        }

        // Only the clients that were in this poll() have a descriptor entry. One
        // accepted just above has none yet, and reading past the end of the list
        // picked up a stale entry that could close the new connection at once.
        const size_t polledClientCount = pollDescriptors.size() - 1;

        for (size_t clientIndex = 0; clientIndex < polledClientCount;) {
            const short events = pollDescriptors[clientIndex + 1].revents;
            bool shouldClose = (events & (POLLERR | POLLHUP | POLLNVAL)) != 0;

            if (!shouldClose && (events & POLLIN) != 0) {
                OpcClient& client = connectedClients[clientIndex];
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
                    // Command 0 is "set 8-bit pixels"; anything else (255 =
                    // system exclusive) is consumed and discarded rather than
                    // being blitted as pixel data.
                    if (message.command == opcutils::OPC_COMMAND_SET_PIXELS &&
                        (message.channel == 0 || message.channel == 1) && message.length > 0) {
                        publishFrame(message.payload, message.length);
                    }
                }
                client.parser.compact();

                if (client.parser.overflowed()) {
                    shouldClose = true;
                }
            }

            if (shouldClose) {
                close(connectedClients[clientIndex].socketDescriptor);
                connectedClients.erase(connectedClients.begin() + static_cast<long>(clientIndex));
                break;  // poll indices are stale; rebuild next iteration
            }
            ++clientIndex;
        }
    }

    for (const OpcClient& client : connectedClients) {
        close(client.socketDescriptor);
    }
    close(listenSocket);
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

bool parseIntegerArgument(const char* text, int& outputValue) {
    try {
        size_t consumed = 0;
        const int parsed = std::stoi(std::string(text), &consumed);
        if (consumed == 0) return false;
        outputValue = parsed;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void parseCommandLine(int argumentCount, char* argumentValues[]) {
    if (argumentCount > 1) {
        int parsedWidth = 0;
        int parsedHeight = 0;
        if (std::sscanf(argumentValues[1], "%dx%d", &parsedWidth, &parsedHeight) == 2 &&
            parsedWidth > 0 && parsedHeight > 0 && parsedWidth <= MAX_MATRIX_DIMENSION &&
            parsedHeight <= MAX_MATRIX_DIMENSION &&
            (parsedWidth * parsedHeight) <= MAX_TOTAL_LED_COUNT) {
            matrixWidth = parsedWidth;
            matrixHeight = parsedHeight;
        } else {
            std::cerr << "Invalid geometry '" << argumentValues[1] << "', using " << matrixWidth
                      << "x" << matrixHeight << "\n";
        }
    }
    totalLedCount = matrixWidth * matrixHeight;
    frameByteCount = totalLedCount * 3;

    if (argumentCount > 2) {
        int parsedLimit = 255;
        if (parseIntegerArgument(argumentValues[2], parsedLimit)) {
            powerLimitScale = static_cast<float>(std::clamp(parsedLimit, 1, 255)) / 255.0f;
        } else {
            std::cerr << "Invalid power limit '" << argumentValues[2] << "', using 255\n";
        }
    }

    if (argumentCount > 3) {
        int parsedRotation = 0;
        if (parseIntegerArgument(argumentValues[3], parsedRotation)) {
            parsedRotation = ((parsedRotation % 360) + 360) % 360;
            if (parsedRotation == 0 || parsedRotation == 90 || parsedRotation == 180 ||
                parsedRotation == 270) {
                uiRotationDeg = parsedRotation;
            } else {
                std::cerr << "Rotation must be 0/90/180/270, using 0\n";
            }
        }
    }

    if (argumentCount > 4) {
        int parsedRefresh = 0;
        if (parseIntegerArgument(argumentValues[4], parsedRefresh) && parsedRefresh > 0) {
            refreshOverrideHz = static_cast<double>(parsedRefresh);
        } else {
            std::cerr << "Invalid refresh rate '" << argumentValues[4] << "', auto-selecting\n";
        }
    }
}

// ---------------------------------------------------------------------------
// Core execution
// ---------------------------------------------------------------------------

int main(int argumentCount, char* argumentValues[]) {
    parseCommandLine(argumentCount, argumentValues);

    sharedFrameBuffer.assign(static_cast<size_t>(frameByteCount), 0);
    generatePhysicalLookupTable();

    // Plain fixed 2.8 gamma by default, as the original server used. Setting
    // panelTone.gammaFloor below gammaFull enables shadow compression at very
    // low headroom; it lifts dark detail but desaturates, so it stays off.
    //   gammaFull      1.0-3.2   display gamma
    //   gammaFloor     0.7-3.2   gamma once headroom runs out (== gammaFull: off)
    //   headroomFull   8-64 LSB  headroom at which gammaFull is used in full
    //   headroomFloor  1-8 LSB   headroom at which gammaFloor is reached
    opcutils::ToneCurve panelTone;
    int lastBrightnessLevel = apiBrightnessLevel.load();
    opcutils::buildScaleTable(masterScaleForSlider(lastBrightnessLevel), panelTone,
                              scaledOutputTable);

    opcutils::FramePacer pacer;
    pacer.start(computeRefreshHz(totalLedCount));
    std::cerr << "Panel " << matrixWidth << "x" << matrixHeight << " (" << totalLedCount
              << " LEDs), refresh " << pacer.currentHz() << " Hz\n";

    opcutils::installSignalHandlers();

    std::memset(&ledStripDriver, 0, sizeof(ws2811_t));
    ledStripDriver.freq = WS2811_TARGET_FREQ;
    ledStripDriver.dmanum = DMA_CHANNEL;
    ledStripDriver.channel[0].gpionum = GPIO_OUTPUT_PIN;
    ledStripDriver.channel[0].count = totalLedCount;
    ledStripDriver.channel[0].strip_type = LED_STRIP_TYPE;
    ledStripDriver.channel[0].brightness = 255;

    const ws2811_return_t initResult = ws2811_init(&ledStripDriver);
    if (initResult != WS2811_SUCCESS) {
        std::cerr << "ws2811_init failed: " << ws2811_get_return_t_str(initResult) << "\n";
        return 1;
    }

    std::memset(ledStripDriver.channel[0].leds, 0,
                static_cast<size_t>(totalLedCount) * sizeof(ws2811_led_t));
    ws2811_render(&ledStripDriver);

    opcutils::dropProcessPrivileges(SECURITY_USER);

    HttpApiServer httpServer(HTTP_CONTROL_PORT, HTTP_RATE_LIMIT_MS);
    httpServer.setCallbacks(
        []() -> int { return apiBrightnessLevel.load(std::memory_order_relaxed); },
        [](int newBrightnessLevel) -> void {
            apiBrightnessLevel.store(std::clamp(newBrightnessLevel, 0, 255),
                                     std::memory_order_relaxed);
        });
    httpServer.startServer();

    std::thread receiverThread(runOpcReceiver);

    std::vector<uint8_t> localRenderBuffer(static_cast<size_t>(frameByteCount), 0);
    std::vector<uint8_t> ditheredBuffer(static_cast<size_t>(frameByteCount), 0);
    uint32_t lastFrameSequence = frameSequence.load(std::memory_order_acquire);
    bool renderPending = true;

    while (opcutils::isRunning.load(std::memory_order_relaxed)) {
        const int currentBrightnessLevel = apiBrightnessLevel.load(std::memory_order_relaxed);
        if (currentBrightnessLevel != lastBrightnessLevel) {
            opcutils::buildScaleTable(masterScaleForSlider(currentBrightnessLevel), panelTone,
                                      scaledOutputTable);
            lastBrightnessLevel = currentBrightnessLevel;
            renderPending = true;
        }

        const uint32_t currentSequence = frameSequence.load(std::memory_order_acquire);
        if (currentSequence != lastFrameSequence) {
            {
                std::lock_guard<std::mutex> lockGuard(frameMutex);
                std::memcpy(localRenderBuffer.data(), sharedFrameBuffer.data(),
                            static_cast<size_t>(frameByteCount));
            }
            lastFrameSequence = currentSequence;
            renderPending = true;
        }

        if (renderPending) {
            const bool ditherIsSteady = panelDitherer.render(localRenderBuffer.data(),
                                                             scaledOutputTable,
                                                             ditheredBuffer.data());

            uint32_t* const ledOutput = ledStripDriver.channel[0].leds;
            const uint16_t* const mapTable = physicalLookupTable.data();
            const uint8_t* const dithered = ditheredBuffer.data();
            for (int pixelIndex = 0; pixelIndex < totalLedCount; ++pixelIndex) {
                const int byteOffset = pixelIndex * 3;
                ledOutput[mapTable[pixelIndex]] =
                    (static_cast<uint32_t>(dithered[byteOffset]) << 16) |
                    (static_cast<uint32_t>(dithered[byteOffset + 1]) << 8) |
                    static_cast<uint32_t>(dithered[byteOffset + 2]);
            }

            ws2811_render(&ledStripDriver);
            // WS2812s latch and hold, so once the dither settles there is
            // nothing left to send until new data or a new brightness arrives.
            renderPending = !ditherIsSteady;
            pacer.noteFrameRendered();
        }

        pacer.waitForNextFrame();
    }

    opcutils::isRunning.store(false);
    if (receiverThread.joinable()) {
        receiverThread.join();
    }
    httpServer.stopServer();

    std::memset(ledStripDriver.channel[0].leds, 0,
                static_cast<size_t>(totalLedCount) * sizeof(ws2811_led_t));
    ws2811_render(&ledStripDriver);
    ws2811_fini(&ledStripDriver);
    return 0;
}
