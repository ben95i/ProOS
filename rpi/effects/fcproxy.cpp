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

// --- Configuration Constants ---
constexpr int OPC_LISTEN_PORT = 7890;
constexpr int OPC_TARGET_PORT = 7891;
constexpr int HTTP_LISTEN_PORT = 7892;
constexpr int HTTP_RATE_LIMIT_MS = 100;
constexpr const char* LOOPBACK_IP = "127.0.0.1";

constexpr int INITIAL_BRIGHTNESS = 178; 
constexpr float MIN_BRIGHTNESS_FLOOR = 0.1f; 
constexpr size_t OPC_HEADER_LEN = 4;
constexpr float GLOBAL_GAMMA = 2.8f;

// --- Global State ---

std::atomic<int> globalCurrentBrightness{INITIAL_BRIGHTNESS};
std::atomic<uint32_t> globalBrightnessVersion{0}; 

std::atomic<int> globalActiveClients{0};

std::mutex cacheMutex;
std::vector<uint8_t> lastRawPayload;
uint8_t lastHeader[OPC_HEADER_LEN] = {0};

uint8_t gammaLookupTable[256];
std::mutex tableMutex;

// --- Logic Functions ---

void updateGammaTable(float brightnessNormalized) {
    std::lock_guard<std::mutex> lockGuard(tableMutex);
    
    for (int iteratorIndex = 0; iteratorIndex < 256; iteratorIndex++) {
        float normalizedIndex = static_cast<float>(iteratorIndex) / 255.0f;
        float powerApplied = std::pow(normalizedIndex, GLOBAL_GAMMA);
        float scaledValue = powerApplied * brightnessNormalized;
        
        float finalRoundedValue = std::round(scaledValue * 255.0f);
        gammaLookupTable[iteratorIndex] = static_cast<uint8_t>(finalRoundedValue);
    }
}

/**
 * @brief Sends the last cached frame to the target if no client is actively doing it.
 */
void triggerFallbackFrameSend() {
    int activeClientCount = globalActiveClients.load();
    
    bool isFallbackRequired = false;
    if (activeClientCount == 0) {
        isFallbackRequired = true;
    }

    if (isFallbackRequired == false) {
        return;
    }

    std::vector<uint8_t> cachedPayloadData;
    uint8_t cachedHeaderData[OPC_HEADER_LEN];
    uint8_t localGammaTable[256];

    {
        std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
        
        bool isCacheEmpty = lastRawPayload.empty();
        if (isCacheEmpty == false) {
            cachedPayloadData = lastRawPayload;
            std::copy(lastHeader, lastHeader + OPC_HEADER_LEN, cachedHeaderData);
        }
    }

    bool hasPayloadToProcess = false;
    if (cachedPayloadData.empty() == false) {
        hasPayloadToProcess = true;
    }

    if (hasPayloadToProcess == true) {
        {
            std::lock_guard<std::mutex> tableLockGuard(tableMutex);
            std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
        }
        
        for (size_t payloadIndex = 0; payloadIndex < cachedPayloadData.size(); ++payloadIndex) {
            uint8_t originalPixel = cachedPayloadData[payloadIndex];
            uint8_t correctedPixel = localGammaTable[originalPixel];
            cachedPayloadData[payloadIndex] = correctedPixel;
        }

        int temporarySocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
        
        bool isSocketValid = false;
        if (temporarySocketDescriptor >= 0) {
            isSocketValid = true;
        }

        if (isSocketValid == true) {
            struct sockaddr_in targetAddress{};
            targetAddress.sin_family = AF_INET;
            targetAddress.sin_port = htons(OPC_TARGET_PORT);
            inet_pton(AF_INET, LOOPBACK_IP, &targetAddress.sin_addr);

            int connectionResult = connect(temporarySocketDescriptor, (struct sockaddr*)&targetAddress, sizeof(targetAddress));
            
            bool isConnectionSuccessful = false;
            if (connectionResult == 0) {
                isConnectionSuccessful = true;
            }

            if (isConnectionSuccessful == true) {
                send(temporarySocketDescriptor, cachedHeaderData, OPC_HEADER_LEN, 0);
                send(temporarySocketDescriptor, cachedPayloadData.data(), cachedPayloadData.size(), 0);
            }
            close(temporarySocketDescriptor);
        }
    }
}

// --- OPC PROXY ---
void handleOpcClient(int clientSocketDescriptor, int serverSocketDescriptor) {
    class ClientTracker {
    public:
        ClientTracker() { 
            globalActiveClients.fetch_add(1); 
        }
        ~ClientTracker() { 
            globalActiveClients.fetch_sub(1); 
        }
    };
    
    ClientTracker activeClientTracker;

    uint8_t headerBuffer[OPC_HEADER_LEN];
    std::vector<uint8_t> payloadBuffer; 
    uint8_t localGammaTable[256];
    
    uint32_t localBrightnessVersion = globalBrightnessVersion.load();

    struct pollfd pollFileDescriptor;
    pollFileDescriptor.fd = clientSocketDescriptor;
    pollFileDescriptor.events = POLLIN;

    bool isClientActive = true;
    while (isClientActive == true) {
        
        uint32_t currentGlobalVersion = globalBrightnessVersion.load();
        bool isVersionMismatch = false;
        
        if (localBrightnessVersion != currentGlobalVersion) {
            isVersionMismatch = true;
        }

        if (isVersionMismatch == true) {
            localBrightnessVersion = currentGlobalVersion;
            
            std::vector<uint8_t> cachedPayloadData;
            uint8_t cachedHeaderData[OPC_HEADER_LEN];

            {
                std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
                
                bool isCacheEmpty = lastRawPayload.empty();
                if (isCacheEmpty == false) {
                    cachedPayloadData = lastRawPayload;
                    std::copy(lastHeader, lastHeader + OPC_HEADER_LEN, cachedHeaderData);
                }
            }

            bool hasCachedPayload = false;
            if (cachedPayloadData.empty() == false) {
                hasCachedPayload = true;
            }

            if (hasCachedPayload == true) {
                {
                    std::lock_guard<std::mutex> tableLockGuard(tableMutex);
                    std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
                }
                
                for (size_t payloadIndex = 0; payloadIndex < cachedPayloadData.size(); ++payloadIndex) {
                    uint8_t originalPixel = cachedPayloadData[payloadIndex];
                    uint8_t correctedPixel = localGammaTable[originalPixel];
                    cachedPayloadData[payloadIndex] = correctedPixel;
                }

                ssize_t headerBytesSent = send(serverSocketDescriptor, cachedHeaderData, OPC_HEADER_LEN, 0);
                if (headerBytesSent <= 0) {
                    break;
                }
                
                ssize_t payloadBytesSent = send(serverSocketDescriptor, cachedPayloadData.data(), cachedPayloadData.size(), 0);
                if (payloadBytesSent <= 0) {
                    break;
                }
            }
        }

        int pollResult = poll(&pollFileDescriptor, 1, 50); 
        
        bool isPollError = false;
        if (pollResult < 0) {
            isPollError = true;
        }
        if (isPollError == true) {
            break;
        }
        
        bool isPollTimeout = false;
        if (pollResult == 0) {
            isPollTimeout = true;
        }
        if (isPollTimeout == true) {
            continue;
        }

        ssize_t headerBytesReceived = recv(clientSocketDescriptor, headerBuffer, OPC_HEADER_LEN, MSG_WAITALL);
        
        bool isHeaderReceived = false;
        if (headerBytesReceived > 0) {
            isHeaderReceived = true;
        }

        if (isHeaderReceived == false) {
            break;
        }

        uint16_t payloadLength = (static_cast<uint16_t>(headerBuffer[2]) << 8) | headerBuffer[3];
        
        bool isPayloadResizeRequired = false;
        if (payloadBuffer.size() < payloadLength) {
            isPayloadResizeRequired = true;
        }

        if (isPayloadResizeRequired == true) {
            payloadBuffer.resize(payloadLength);
        }

        bool hasPayloadContent = false;
        if (payloadLength > 0) {
            hasPayloadContent = true;
        }

        if (hasPayloadContent == true) {
            ssize_t payloadBytesReceived = recv(clientSocketDescriptor, payloadBuffer.data(), payloadLength, MSG_WAITALL);
            
            bool isPayloadReceived = false;
            if (payloadBytesReceived > 0) {
                isPayloadReceived = true;
            }

            if (isPayloadReceived == false) {
                break;
            }
        }
        
        bool isStandardFrame = false;
        if (headerBuffer[1] == 0) { 
            isStandardFrame = true;
        }

        if (isStandardFrame == true) { 
            {
                std::lock_guard<std::mutex> cacheLockGuard(cacheMutex);
                std::copy(headerBuffer, headerBuffer + OPC_HEADER_LEN, lastHeader);
                lastRawPayload.assign(payloadBuffer.begin(), payloadBuffer.begin() + payloadLength);
            }

            {
                std::lock_guard<std::mutex> tableLockGuard(tableMutex);
                std::copy(std::begin(gammaLookupTable), std::end(gammaLookupTable), std::begin(localGammaTable));
            }

            for (size_t payloadIndex = 0; payloadIndex < payloadLength; ++payloadIndex) {
                uint8_t rawPixelData = payloadBuffer[payloadIndex];
                uint8_t correctedPixelData = localGammaTable[rawPixelData];
                payloadBuffer[payloadIndex] = correctedPixelData;
            }
        }

        ssize_t sentHeaderBytes = send(serverSocketDescriptor, headerBuffer, OPC_HEADER_LEN, 0);
        
        bool isHeaderSentSuccessfully = false;
        if (sentHeaderBytes > 0) {
            isHeaderSentSuccessfully = true;
        }

        if (isHeaderSentSuccessfully == false) {
            break;
        }
        
        if (hasPayloadContent == true) {
            ssize_t sentPayloadBytes = send(serverSocketDescriptor, payloadBuffer.data(), payloadLength, 0);
            
            bool isPayloadSentSuccessfully = false;
            if (sentPayloadBytes > 0) {
                isPayloadSentSuccessfully = true;
            }

            if (isPayloadSentSuccessfully == false) {
                break;
            }
        }
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    float initialRatio = static_cast<float>(INITIAL_BRIGHTNESS) / 255.0f;
    float startNormalizedValue = MIN_BRIGHTNESS_FLOOR + (initialRatio * (1.0f - MIN_BRIGHTNESS_FLOOR));
    updateGammaTable(startNormalizedValue);

    // Set up and start the common HTTP Server
    HttpApiServer httpServer(HTTP_LISTEN_PORT, HTTP_RATE_LIMIT_MS);
    httpServer.setCallbacks(
        []() -> int {
            int currentBrightness = globalCurrentBrightness.load();
            return currentBrightness;
        },
        [](int newBrightnessLevel) -> void {
            int clampedBrightness = std::clamp(newBrightnessLevel, 0, 255);
            float rawValueFloat = static_cast<float>(clampedBrightness);
            float inputRatioFloat = rawValueFloat / 255.0f;
            float normalizedBrightnessValue = MIN_BRIGHTNESS_FLOOR + (inputRatioFloat * (1.0f - MIN_BRIGHTNESS_FLOOR));
            
            updateGammaTable(normalizedBrightnessValue);
            globalCurrentBrightness.store(clampedBrightness);
            globalBrightnessVersion.fetch_add(1);

            triggerFallbackFrameSend();
        }
    );
    
    httpServer.startServer();

    int proxySocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    int socketOptions = 1;
    setsockopt(proxySocketDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketOptions, sizeof(socketOptions));

    struct sockaddr_in proxyAddress{};
    proxyAddress.sin_family = AF_INET;
    proxyAddress.sin_addr.s_addr = INADDR_ANY;
    proxyAddress.sin_port = htons(OPC_LISTEN_PORT);

    int bindResult = bind(proxySocketDescriptor, (struct sockaddr*)&proxyAddress, sizeof(proxyAddress));
    
    bool isBindSuccessful = false;
    if (bindResult >= 0) {
        isBindSuccessful = true;
    }

    if (isBindSuccessful == false) {
        std::cerr << "Fatal: Could not bind OPC port." << std::endl;
        return 1;
    }

    listen(proxySocketDescriptor, 10);
    std::cout << "OPC Proxy Active on Port " << OPC_LISTEN_PORT << std::endl;

    bool isProxyRunning = true;
    while (isProxyRunning == true) {
        int clientSocketDescriptor = accept(proxySocketDescriptor, nullptr, nullptr);
        
        bool isClientValid = false;
        if (clientSocketDescriptor >= 0) {
            isClientValid = true;
        }

        if (isClientValid == false) {
            continue;
        }

        int targetServerSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in targetAddress{};
        targetAddress.sin_family = AF_INET;
        targetAddress.sin_port = htons(OPC_TARGET_PORT);
        inet_pton(AF_INET, LOOPBACK_IP, &targetAddress.sin_addr);

        int connectResult = connect(targetServerSocketDescriptor, (struct sockaddr*)&targetAddress, sizeof(targetAddress));
        
        bool isConnectSuccessful = false;
        if (connectResult == 0) {
            isConnectSuccessful = true;
        }

        if (isConnectSuccessful == false) {
            close(clientSocketDescriptor);
            close(targetServerSocketDescriptor);
            continue;
        }

        std::thread clientThread([clientSocketDescriptor, targetServerSocketDescriptor]() {
            handleOpcClient(clientSocketDescriptor, targetServerSocketDescriptor);
            close(clientSocketDescriptor);
            close(targetServerSocketDescriptor);
        });
        clientThread.detach();
    }

    httpServer.stopServer();
    return 0;
}


