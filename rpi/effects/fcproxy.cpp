#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cmath>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <signal.h>
#include <chrono>
#include <poll.h>
#include "httpapiserver.h"

// --- System Constants ---
constexpr int OPC_LISTEN_PORT = 7890;
constexpr int OPC_TARGET_PORT = 7891;
constexpr int HTTP_LISTEN_PORT = 7892;
constexpr int HTTP_RATE_LIMIT_MS = 100;
constexpr const char* LOOPBACK_IP = "127.0.0.1";
constexpr int INITIAL_BRIGHTNESS = 175; 
constexpr float MIN_BRIGHTNESS_FLOOR = 0.1f; 
constexpr size_t OPC_HEADER_LEN = 4;
constexpr float GLOBAL_GAMMA = 2.2f;

std::atomic<int> globalCurrentBrightness{INITIAL_BRIGHTNESS};
std::atomic<uint32_t> globalBrightnessVersion{0}; 
std::atomic<int> globalActiveClients{0};

std::mutex cacheMutex;
std::vector<uint8_t> lastRawPayload;
uint8_t lastHeader[OPC_HEADER_LEN] = {0};

std::mutex tableMutex;
uint8_t gammaLookupTable[256];

void updateGammaTable(float brightnessNormalized) {
    std::lock_guard<std::mutex> lockGuard(tableMutex);
    for (int i = 0; i < 256; i++) {
        float normalizedIndex = static_cast<float>(i) / 255.0f;
        gammaLookupTable[i] = static_cast<uint8_t>(std::round(std::pow(normalizedIndex, GLOBAL_GAMMA) * brightnessNormalized * 255.0f));
    }
}

void triggerFallbackFrameSend() {
    if (globalActiveClients.load() > 0) return;

    std::vector<uint8_t> cachedPayloadData;
    uint8_t cachedHeaderData[OPC_HEADER_LEN];
    uint8_t localGammaTable[256];

    {
        std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
        if (lastRawPayload.empty()) return;
        cachedPayloadData = lastRawPayload;
        std::copy(lastHeader, lastHeader + OPC_HEADER_LEN, cachedHeaderData);
    }

    {
        std::lock_guard<std::mutex> tableLockGuard(tableMutex);
        std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
    }
    
    for (uint8_t& pixel : cachedPayloadData) {
        pixel = localGammaTable[pixel];
    }

    int tempSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (tempSocket >= 0) {
        struct sockaddr_in targetAddress{};
        targetAddress.sin_family = AF_INET;
        targetAddress.sin_port = htons(OPC_TARGET_PORT);
        inet_pton(AF_INET, LOOPBACK_IP, &targetAddress.sin_addr);

        // Keep socket lifetime brief
        struct timeval timeout{1, 0}; 
        setsockopt(tempSocket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

        if (connect(tempSocket, (struct sockaddr*)&targetAddress, sizeof(targetAddress)) == 0) {
            send(tempSocket, cachedHeaderData, OPC_HEADER_LEN, MSG_NOSIGNAL);
            send(tempSocket, cachedPayloadData.data(), cachedPayloadData.size(), MSG_NOSIGNAL);
        }
        close(tempSocket);
    }
}

void handleOpcClient(int clientSocketDescriptor, int serverSocketDescriptor) {
    struct ClientTracker {
        ClientTracker() { globalActiveClients++; }
        ~ClientTracker() { globalActiveClients--; }
    } tracker;

    // Apply strict network timeouts to prevent deadlocks
    struct timeval timeout{2, 0};
    setsockopt(clientSocketDescriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(serverSocketDescriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    uint8_t headerBuffer[OPC_HEADER_LEN];
    std::vector<uint8_t> payloadBuffer; 
    uint8_t localGammaTable[256];
    uint32_t localBrightnessVersion = globalBrightnessVersion.load();

    struct pollfd pollFd{clientSocketDescriptor, POLLIN, 0};

    while (true) {
        uint32_t currentGlobalVersion = globalBrightnessVersion.load();
        if (localBrightnessVersion != currentGlobalVersion) {
            localBrightnessVersion = currentGlobalVersion;
            
            std::vector<uint8_t> cachedPayloadData;
            uint8_t cachedHeaderData[OPC_HEADER_LEN];

            {
                std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
                if (!lastRawPayload.empty()) {
                    cachedPayloadData = lastRawPayload;
                    std::copy(lastHeader, lastHeader + OPC_HEADER_LEN, cachedHeaderData);
                }
            }

            if (!cachedPayloadData.empty()) {
                {
                    std::lock_guard<std::mutex> tableLockGuard(tableMutex);
                    std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
                }
                
                for (uint8_t& pixel : cachedPayloadData) {
                    pixel = localGammaTable[pixel];
                }

                if (send(serverSocketDescriptor, cachedHeaderData, OPC_HEADER_LEN, MSG_NOSIGNAL) <= 0 ||
                    send(serverSocketDescriptor, cachedPayloadData.data(), cachedPayloadData.size(), MSG_NOSIGNAL) <= 0) {
                    break;
                }
            }
        }

        int pollResult = poll(&pollFd, 1, 50); 
        if (pollResult < 0) break;
        if (pollResult == 0) continue;

        if (recv(clientSocketDescriptor, headerBuffer, OPC_HEADER_LEN, MSG_WAITALL) <= 0) break;

        uint16_t payloadLength = (headerBuffer[2] << 8) | headerBuffer[3];
        if (payloadBuffer.size() < payloadLength) payloadBuffer.resize(payloadLength);

        if (payloadLength > 0 && recv(clientSocketDescriptor, payloadBuffer.data(), payloadLength, MSG_WAITALL) <= 0) break;
        
        if (headerBuffer[1] == 0) { 
            {
                std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
                std::copy(headerBuffer, headerBuffer + OPC_HEADER_LEN, lastHeader);
                lastRawPayload.assign(payloadBuffer.begin(), payloadBuffer.begin() + payloadLength);
            }

            {
                std::lock_guard<std::mutex> tableLockGuard(tableMutex);
                std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
            }

            for (size_t i = 0; i < payloadLength; ++i) {
                payloadBuffer[i] = localGammaTable[payloadBuffer[i]];
            }
        }

        if (send(serverSocketDescriptor, headerBuffer, OPC_HEADER_LEN, MSG_NOSIGNAL) <= 0) break;
        if (payloadLength > 0 && send(serverSocketDescriptor, payloadBuffer.data(), payloadLength, MSG_NOSIGNAL) <= 0) break;
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    float initialRatio = static_cast<float>(INITIAL_BRIGHTNESS) / 255.0f;
    updateGammaTable(MIN_BRIGHTNESS_FLOOR + (initialRatio * (1.0f - MIN_BRIGHTNESS_FLOOR)));

    HttpApiServer httpServer(HTTP_LISTEN_PORT, HTTP_RATE_LIMIT_MS);
    httpServer.setCallbacks(
        []() { return globalCurrentBrightness.load(); },
        [](int newBrightness) {
            int clamped = std::clamp(newBrightness, 0, 255);
            float ratio = clamped / 255.0f;
            updateGammaTable(MIN_BRIGHTNESS_FLOOR + (ratio * (1.0f - MIN_BRIGHTNESS_FLOOR)));
            globalCurrentBrightness.store(clamped);
            globalBrightnessVersion++;
            triggerFallbackFrameSend();
        }
    );
    httpServer.startServer();

    int proxySocket = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(proxySocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in proxyAddress{};
    proxyAddress.sin_family = AF_INET;
    proxyAddress.sin_addr.s_addr = INADDR_ANY;
    proxyAddress.sin_port = htons(OPC_LISTEN_PORT);

    if (bind(proxySocket, (struct sockaddr*)&proxyAddress, sizeof(proxyAddress)) < 0) return 1;

    listen(proxySocket, 10);
    std::cout << "OPC Proxy Active on Port " << OPC_LISTEN_PORT << std::endl;

    while (true) {
        int clientSocket = accept(proxySocket, nullptr, nullptr);
        if (clientSocket < 0) continue;

        int targetSocket = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in targetAddress{};
        targetAddress.sin_family = AF_INET;
        targetAddress.sin_port = htons(OPC_TARGET_PORT);
        inet_pton(AF_INET, LOOPBACK_IP, &targetAddress.sin_addr);

        if (connect(targetSocket, (struct sockaddr*)&targetAddress, sizeof(targetAddress)) == 0) {
            std::thread([clientSocket, targetSocket]() {
                handleOpcClient(clientSocket, targetSocket);
                close(clientSocket);
                close(targetSocket);
            }).detach();
        } else {
            close(clientSocket);
            close(targetSocket);
        }
    }

    httpServer.stopServer();
    return 0;
}