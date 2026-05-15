#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <algorithm>
#include <cmath> 
#include <ws2811/ws2811.h> 
#include <netinet/tcp.h>
#include "httpapiserver.h"

// --- System Constants ---
constexpr int GPIO_OUTPUT_PIN = 18;
constexpr int DMA_CHANNEL = 10;
constexpr int LED_STRIP_TYPE = WS2811_STRIP_GRB;
constexpr float GAMMA_EXPONENT = 2.7f;
constexpr const char* SECURITY_USER = "nobody";
constexpr int OPC_LISTENER_PORT = 7890;
constexpr int HTTP_CONTROL_PORT = 7892;
constexpr int HTTP_RATE_LIMIT_MS = 100;
constexpr const char* LOOPBACK_IP = "127.0.0.1";
constexpr float REFRESH_RATE_HZ = 60.0f;
constexpr auto TARGET_FRAME_TIME = std::chrono::microseconds(static_cast<long long>(1000000 / REFRESH_RATE_HZ));
constexpr uint16_t MAP_INDEX_INVALID = 0xFFFF;

// --- State Variables ---
int matrixWidth = 16;
int matrixHeight = 16;
int totalLedCount = 256;
int startupHardwareLimit = 255;
int uiRotationDeg = 0;

std::atomic<bool> isRunning{true};
ws2811_t ledStripDriver;
std::mutex sharedDataMutex;

// --- High Performance Buffers ---
std::atomic<int> apiBrightnessLevel{255};
float gammaLookupTable[256];
std::vector<uint16_t> physicalLookupTable; // Pre-calculated map

std::vector<uint8_t> incomingFrameBuffer;
std::vector<float> quantizationErrorRed;
std::vector<float> quantizationErrorGreen;
std::vector<float> quantizationErrorBlue;

void handleTerminationSignal(int) { 
    bool falseState = false;
    isRunning.store(falseState); 
}

void dropProcessPrivileges() {
    struct passwd* processUser = getpwnam(SECURITY_USER);
    
    bool isUserFound = false;
    if (processUser != nullptr) {
        isUserFound = true;
    }

    if (isUserFound == true) {
        int gidResult = setgid(processUser->pw_gid);
        int uidResult = setuid(processUser->pw_uid);
        
        bool isPrivilegeDropFailed = false;
        if (gidResult != 0 || uidResult != 0) {
            isPrivilegeDropFailed = true;
        }

        if (isPrivilegeDropFailed == true) {
            std::cerr << "Process running as user: " << SECURITY_USER << "\n";
        }
    }
}

void generatePhysicalLookupTable() {
    physicalLookupTable.resize(totalLedCount);
    for (int iteratorIndex = 0; iteratorIndex < totalLedCount; ++iteratorIndex) {
        uint16_t canvasWidth = matrixWidth;
        
        bool isRotatedSideways = false;
        if (uiRotationDeg == 90 || uiRotationDeg == 270) {
            isRotatedSideways = true;
        }

        if (isRotatedSideways == true) {
            canvasWidth = matrixHeight;
        }
        
        uint16_t sourceX = iteratorIndex % canvasWidth;
        uint16_t sourceY = iteratorIndex / canvasWidth;
        uint16_t rotatedX = 0;
        uint16_t rotatedY = 0;

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

        bool isEvenRow = false;
        if (rotatedY % 2 == 0) {
            isEvenRow = true;
        }

        uint16_t physicalX = rotatedX;
        if (isEvenRow == true) {
            physicalX = (matrixWidth - 1) - rotatedX;
        }
        
        uint16_t finalIndex = (rotatedY * matrixWidth) + physicalX;
        physicalLookupTable[iteratorIndex] = finalIndex;
    }
}

void precalculateGammaTable() {
    for (int iteratorIndex = 0; iteratorIndex < 256; ++iteratorIndex) {
        float normalizedIndex = static_cast<float>(iteratorIndex) / 255.0f;
        float powerApplied = std::pow(normalizedIndex, GAMMA_EXPONENT);
        float scaledResult = powerApplied * 255.0f;
        
        gammaLookupTable[iteratorIndex] = scaledResult;
    }
}

void runOpcReceiver() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    int socketOptions = 1;
    setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &socketOptions, sizeof(socketOptions));
    
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(OPC_LISTENER_PORT);
    inet_pton(AF_INET, LOOPBACK_IP, &serverAddress.sin_addr);
    
    int bindResult = bind(serverSocket, (sockaddr*)&serverAddress, sizeof(serverAddress));
    if (bindResult < 0) {
        return;
    }
    
    listen(serverSocket, 1);

    std::vector<uint8_t> payloadBuffer;
    int payloadCapacity = totalLedCount * 3;
    payloadBuffer.reserve(payloadCapacity);
    
    uint8_t headerBuffer[4];

    while (isRunning.load() == true) {
        int clientSocket = accept(serverSocket, nullptr, nullptr);
        
        bool isConnectionActive = true;
        while (isConnectionActive == true) {
            ssize_t headerBytesRead = recv(clientSocket, headerBuffer, 4, MSG_WAITALL);
            if (headerBytesRead <= 0) {
                isConnectionActive = false;
                continue;
            }

            uint16_t payloadLength = (headerBuffer[2] << 8) | headerBuffer[3];
            
            bool isPayloadResizeRequired = false;
            if (payloadLength > payloadBuffer.size()) {
                isPayloadResizeRequired = true;
            }

            if (isPayloadResizeRequired == true) {
                payloadBuffer.resize(payloadLength);
            }
            
            recv(clientSocket, payloadBuffer.data(), payloadLength, MSG_WAITALL);

            std::lock_guard<std::mutex> lockGuard(sharedDataMutex);
            size_t copyLength = std::min(static_cast<size_t>(payloadLength), incomingFrameBuffer.size());
            std::memcpy(incomingFrameBuffer.data(), payloadBuffer.data(), copyLength);
        }
        close(clientSocket);
    }
    close(serverSocket);
}

// --- Core Execution ---

int main(int argumentCount, char* argumentValues[]) {
    if (argumentCount > 1) {
        int matchedArguments = sscanf(argumentValues[1], "%dx%dgrid", &matrixWidth, &matrixHeight);
        if (matchedArguments == 2) {
            totalLedCount = matrixWidth * matrixHeight;
        }
    }
    
    if (argumentCount > 2) {
        int parsedHardwareLimit = std::stoi(argumentValues[2]);
        startupHardwareLimit = std::clamp(parsedHardwareLimit, 1, 255);
    }
    
    if (argumentCount > 3) {
        uiRotationDeg = std::stoi(argumentValues[3]);
    }

    int incomingFrameBufferSize = totalLedCount * 3;
    incomingFrameBuffer.assign(incomingFrameBufferSize, 0);
    quantizationErrorRed.assign(totalLedCount, 0.0f);
    quantizationErrorGreen.assign(totalLedCount, 0.0f);
    quantizationErrorBlue.assign(totalLedCount, 0.0f);
    
    precalculateGammaTable();
    generatePhysicalLookupTable(); 

    signal(SIGINT, handleTerminationSignal);
    signal(SIGTERM, handleTerminationSignal);
    signal(SIGPIPE, SIG_IGN);

    memset(&ledStripDriver, 0, sizeof(ws2811_t));
    ledStripDriver.freq = WS2811_TARGET_FREQ;
    ledStripDriver.dmanum = DMA_CHANNEL;
    ledStripDriver.channel[0].gpionum = GPIO_OUTPUT_PIN;
    ledStripDriver.channel[0].count = totalLedCount;
    ledStripDriver.channel[0].strip_type = LED_STRIP_TYPE;
    ledStripDriver.channel[0].brightness = startupHardwareLimit; 

    ws2811_return_t initializeResult = ws2811_init(&ledStripDriver);
    if (initializeResult != WS2811_SUCCESS) {
        return 1;
    }

    dropProcessPrivileges();
    
    // Set up and start the common HTTP Server
    HttpApiServer httpServer(HTTP_CONTROL_PORT, HTTP_RATE_LIMIT_MS);
    httpServer.setCallbacks(
        []() -> int {
            int currentBrightness = apiBrightnessLevel.load();
            return currentBrightness;
        },
        [](int newBrightnessLevel) -> void {
            int clampedBrightness = std::clamp(newBrightnessLevel, 0, 255);
            apiBrightnessLevel.store(clampedBrightness);
        }
    );
    httpServer.startServer();

    std::thread backgroundReceiverThread(runOpcReceiver);
    backgroundReceiverThread.detach();

    std::chrono::time_point<std::chrono::steady_clock> frameDeadline = std::chrono::steady_clock::now();

    while (isRunning.load() == true) {
        frameDeadline += TARGET_FRAME_TIME;
        
        int currentBrightnessLevel = apiBrightnessLevel.load();
        float softwareScaleFactor = static_cast<float>(currentBrightnessLevel) / 255.0f;

        {
            std::lock_guard<std::mutex> lockGuard(sharedDataMutex);

            for (int iteratorIndex = 0; iteratorIndex < totalLedCount; ++iteratorIndex) {
                uint16_t physicalIdentifier = physicalLookupTable[iteratorIndex];
                int pixelOffset = iteratorIndex * 3;
                
                int indexRed = incomingFrameBuffer[pixelOffset];
                int indexGreen = incomingFrameBuffer[pixelOffset + 1];
                int indexBlue = incomingFrameBuffer[pixelOffset + 2];

                float targetRed = (gammaLookupTable[indexRed] * softwareScaleFactor) + quantizationErrorRed[physicalIdentifier];
                float targetGreen = (gammaLookupTable[indexGreen] * softwareScaleFactor) + quantizationErrorGreen[physicalIdentifier];
                float targetBlue = (gammaLookupTable[indexBlue] * softwareScaleFactor) + quantizationErrorBlue[physicalIdentifier];

                uint8_t outputRed = static_cast<uint8_t>(std::clamp(targetRed, 0.0f, 255.0f));
                uint8_t outputGreen = static_cast<uint8_t>(std::clamp(targetGreen, 0.0f, 255.0f));
                uint8_t outputBlue = static_cast<uint8_t>(std::clamp(targetBlue, 0.0f, 255.0f));

                float finalErrorRed = targetRed - static_cast<float>(outputRed);
                float finalErrorGreen = targetGreen - static_cast<float>(outputGreen);
                float finalErrorBlue = targetBlue - static_cast<float>(outputBlue);

                quantizationErrorRed[physicalIdentifier] = finalErrorRed;
                quantizationErrorGreen[physicalIdentifier] = finalErrorGreen;
                quantizationErrorBlue[physicalIdentifier] = finalErrorBlue;

                uint32_t finalColorPixel = (static_cast<uint32_t>(outputRed) << 16) | (static_cast<uint32_t>(outputGreen) << 8) | static_cast<uint32_t>(outputBlue);
                ledStripDriver.channel[0].leds[physicalIdentifier] = finalColorPixel;
            }
            ws2811_render(&ledStripDriver);
        }
        std::this_thread::sleep_until(frameDeadline);
    }

    httpServer.stopServer();
    ws2811_fini(&ledStripDriver);
    return 0;
}


