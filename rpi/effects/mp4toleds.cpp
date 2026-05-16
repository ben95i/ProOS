#include "ledlayout.h"
#include "opcclient.h"

#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <cmath>
#include <chrono>
#include <thread>
#include <signal.h>
#include <algorithm>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <filesystem>

// Native POSIX network headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

constexpr const char* OpcServerIpAddress = "127.0.0.1";
constexpr int OpcServerPortNumber = 7890;
constexpr int HttpServerPortNumber = 7860;
const std::string ApprovedVideoDirectory = "/opt/rpi/effects/videos/"; 

// Global execution state changed to std::atomic to eliminate race conditions across threads
std::atomic<bool> isGlobalApplicationRunning{true};

/**
 * @brief System signal handler to cleanly shut down the application.
 * * This function is triggered by OS signals (like SIGINT/Ctrl+C). It sets the 
 * global atomic flag to false, which instructs all running loops and threads 
 * to terminate safely.
 * * @param signalCode The integer code of the received OS signal.
 */
void systemSignalHandler(int signalCode) { 
    isGlobalApplicationRunning = false; 
}

class VideoPlaybackEngine {
private:
    const LedLayout& configuredDisplayLayout;
    OPCClient networkPixelTransmitter;
    
    std::thread videoDecodingThread;
    std::mutex playbackStateMutex;
    std::condition_variable playbackStateConditionVariable;
    
    std::string currentVideoFilePath;
    
    // Protected strictly by playbackStateMutex to prevent race conditions
    bool isPlaybackActive{false};
    bool hasNewVideoRequest{false};

    /**
     * @brief Calculates the exact byte length required for the OPC payload buffer.
     * * This function iterates through the layout configuration to find the highest 
     * assigned LED index. Since each LED requires 3 bytes (Red, Green, Blue), 
     * it calculates the total required memory footprint.
     * * @return int The total number of bytes required.
     */
    int calculateRequiredPayloadByteLength() const {
        int maximumHardwarePixelIndex = 0;
        for (const auto& individualPixel : configuredDisplayLayout.getPixels()) {
            if (individualPixel.opcIndex > maximumHardwarePixelIndex) {
                maximumHardwarePixelIndex = individualPixel.opcIndex;
            }
        }
        
        int totalPayloadBytes = (maximumHardwarePixelIndex + 1) * 3;
        return totalPayloadBytes;
    }

    /**
     * @brief Closes the network connection to the OPC server.
     * * Disconnects the socket explicitly. By disconnecting instead of sending 
     * blank data, the physical LEDs will retain their last known state, 
     * and the server channel becomes available for other software.
     */
    void releaseOpcResource() {
        networkPixelTransmitter.disconnectFromServer(); 
        std::cout << "OPC Connection closed." << std::endl;
    }

    /**
     * @brief The core worker thread responsible for decoding video and transmitting pixels.
     * * This function runs endlessly until the application shuts down. It waits for 
     * video requests, opens the stream via hardware acceleration, dynamically 
     * determines the frame rate, and maps video pixels to physical LED coordinates.
     */
    void videoDecodingTask() {
        // Memory Optimization: Allocate buffers ONCE for the entire lifecycle of the thread
        const int requiredPayloadByteLength = calculateRequiredPayloadByteLength();
        std::vector<uint8_t> framePayloadBuffer(requiredPayloadByteLength, 0);
        std::vector<size_t> textureLookupOffsets;
        cv::Mat currentDecodedFrame;
        
        while (isGlobalApplicationRunning == true) {
            std::string activeFilePath;
            
            // Wait state: Pause thread entirely until a playback request comes in
            {
                std::unique_lock<std::mutex> lock(playbackStateMutex);
                playbackStateConditionVariable.wait(lock, [this] { 
                    if (hasNewVideoRequest == true) {
                        return true;
                    } else if (isGlobalApplicationRunning == false) {
                        return true;
                    } else {
                        return false;
                    }
                });
                
                if (isGlobalApplicationRunning == false) {
                    break;
                }
                
                activeFilePath = currentVideoFilePath;
                hasNewVideoRequest = false;
                isPlaybackActive = true;
            }

            cv::VideoCapture hardwareVideoCaptureStream(activeFilePath, cv::CAP_FFMPEG, {
                cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
            });

            bool isVideoStreamOpened = hardwareVideoCaptureStream.isOpened();
            if (isVideoStreamOpened == false) {
                std::cerr << "Error: Unable to open " << activeFilePath << std::endl;
                std::lock_guard<std::mutex> lock(playbackStateMutex);
                isPlaybackActive = false;
                continue;
            }

            hardwareVideoCaptureStream.set(cv::CAP_PROP_BUFFERSIZE, 1);
            
            // Dynamically calculate the frame rate based on the video file metadata
            double framesPerSecond = hardwareVideoCaptureStream.get(cv::CAP_PROP_FPS);
            
            if (framesPerSecond <= 0.0) {
                framesPerSecond = 30.0; // Fallback default if metadata is missing or corrupted
            } else if (framesPerSecond > 60.0) {
                framesPerSecond = 60.0; // Cap the frame rate to prevent overwhelming the OPC server
            }
            
            long long frameDurationNanoseconds = static_cast<long long>(1000000000.0 / framesPerSecond);
            const std::chrono::nanoseconds targetFrameDuration(frameDurationNanoseconds);
            
            int lastCapturedFrameWidth = -1;
            int lastCapturedFrameHeight = -1;

            // Inner Playback Loop
            while (isGlobalApplicationRunning == true) {
                auto currentFrameStartTimestamp = std::chrono::steady_clock::now();

                hardwareVideoCaptureStream >> currentDecodedFrame;
                
                bool isFrameEmpty = currentDecodedFrame.empty();
                if (isFrameEmpty == true) {
                    hardwareVideoCaptureStream.set(cv::CAP_PROP_POS_FRAMES, 0); // Seamless loop
                    continue;
                }

                // Recalculate mapping offsets ONLY if resolution changes to save CPU cycles
                if (currentDecodedFrame.cols != lastCapturedFrameWidth || currentDecodedFrame.rows != lastCapturedFrameHeight) {
                    textureLookupOffsets.clear();
                    float nativeVideoAspectRatio = static_cast<float>(currentDecodedFrame.cols) / static_cast<float>(currentDecodedFrame.rows);
                    int croppedRenderWidth = currentDecodedFrame.cols;
                    int croppedRenderHeight = currentDecodedFrame.rows;
                    int renderOffsetX = 0;
                    int renderOffsetY = 0;

                    if (nativeVideoAspectRatio > configuredDisplayLayout.getTargetAspect()) {
                        croppedRenderWidth = static_cast<int>(currentDecodedFrame.rows * configuredDisplayLayout.getTargetAspect());
                        renderOffsetX = (currentDecodedFrame.cols - croppedRenderWidth) / 2;
                    } else {
                        croppedRenderHeight = static_cast<int>(currentDecodedFrame.cols / configuredDisplayLayout.getTargetAspect());
                        renderOffsetY = (currentDecodedFrame.rows - croppedRenderHeight) / 2;
                    }

                    for (const auto& activePixel : configuredDisplayLayout.getPixels()) {
                        int sampledPixelX = renderOffsetX + static_cast<int>(activePixel.normX * (croppedRenderWidth - 1));
                        int sampledPixelY = renderOffsetY + static_cast<int>(activePixel.normY * (croppedRenderHeight - 1));
                        
                        sampledPixelX = std::clamp(sampledPixelX, 0, currentDecodedFrame.cols - 1);
                        sampledPixelY = std::clamp(sampledPixelY, 0, currentDecodedFrame.rows - 1);
                        
                        size_t byteOffset = (sampledPixelY * currentDecodedFrame.cols + sampledPixelX) * 3;
                        textureLookupOffsets.push_back(byteOffset);
                    }
                    
                    lastCapturedFrameWidth = currentDecodedFrame.cols; 
                    lastCapturedFrameHeight = currentDecodedFrame.rows;
                }

                uint8_t* rawOpenCvBufferData = currentDecodedFrame.data;
                for (size_t index = 0; index < configuredDisplayLayout.getPixels().size(); ++index) {
                    uint8_t* sourceBgrPixel = &rawOpenCvBufferData[textureLookupOffsets[index]];
                    int baseIndex = configuredDisplayLayout.getPixels()[index].opcIndex * 3;
                    
                    framePayloadBuffer[baseIndex]     = sourceBgrPixel[2]; // Red
                    framePayloadBuffer[baseIndex + 1] = sourceBgrPixel[1]; // Green
                    framePayloadBuffer[baseIndex + 2] = sourceBgrPixel[0]; // Blue
                }

                bool isTransmissionSuccessful = networkPixelTransmitter.sendPixelPayload(0, framePayloadBuffer.data(), framePayloadBuffer.size());
                if (isTransmissionSuccessful == false) {
                    std::cerr << "Lost connection to OPC Server." << std::endl;
                    break; 
                }
                
                // Wait for the exact frame duration, but allow immediate wake-up if playback state changes
                std::unique_lock<std::mutex> lock(playbackStateMutex);
                bool stateChangedInterrupt = playbackStateConditionVariable.wait_until(
                    lock, 
                    currentFrameStartTimestamp + targetFrameDuration, 
                    [this] { 
                        if (isPlaybackActive == false) {
                            return true;
                        } else if (hasNewVideoRequest == true) {
                            return true;
                        } else if (isGlobalApplicationRunning == false) {
                            return true;
                        } else {
                            return false;
                        }
                    }
                );

                if (stateChangedInterrupt == true) {
                    break; // Exit inner loop instantly on STOP or new video request
                }
            }

            // Proper memory cleanup for this specific video file
            hardwareVideoCaptureStream.release(); 
            currentDecodedFrame.release();        
            releaseOpcResource();
        }
    }

public:
    VideoPlaybackEngine(const LedLayout& layout) 
        : configuredDisplayLayout(layout), networkPixelTransmitter(OpcServerIpAddress, OpcServerPortNumber) {
        
        bool directoryExists = std::filesystem::exists(ApprovedVideoDirectory);
        if (directoryExists == false) {
            std::filesystem::create_directory(ApprovedVideoDirectory);
        }
        
        videoDecodingThread = std::thread(&VideoPlaybackEngine::videoDecodingTask, this);
    }

    ~VideoPlaybackEngine() {
        {
            std::lock_guard<std::mutex> lock(playbackStateMutex);
            isPlaybackActive = false;
        }
        
        playbackStateConditionVariable.notify_all();
        
        bool threadIsJoinable = videoDecodingThread.joinable();
        if (threadIsJoinable == true) {
            videoDecodingThread.join();
        }
    }

    /**
     * @brief Queues a specific video file for playback.
     * * @param targetFilename The string name of the file to attempt to play.
     */
    void requestVideoPlayback(const std::string& targetFilename) {
        std::lock_guard<std::mutex> lock(playbackStateMutex);
        currentVideoFilePath = ApprovedVideoDirectory + targetFilename;
        hasNewVideoRequest = true;
        isPlaybackActive = true; 
        playbackStateConditionVariable.notify_one();
    }

    /**
     * @brief Signals the engine to halt the current video stream instantly.
     */
    void requestStopPlayback() {
        std::lock_guard<std::mutex> lock(playbackStateMutex);
        isPlaybackActive = false;
        hasNewVideoRequest = false; 
        playbackStateConditionVariable.notify_one(); 
    }
};

/**
 * @brief Evaluates a filename string against OWASP directory traversal vulnerabilities.
 * * @param filenameToValidate The requested filename from the HTTP query.
 * @return true If the string is safe and properly formatted.
 * @return false If the string contains illegal characters or paths.
 */
bool isValidSecureFilename(const std::string& filenameToValidate) {
    if (filenameToValidate.empty() == true) {
        return false;
    } else if (filenameToValidate.find("..") != std::string::npos) {
        return false;
    } else if (filenameToValidate.find('/') != std::string::npos) {
        return false;
    } else if (filenameToValidate.find('\\') != std::string::npos) {
        return false;
    } else {
        return true;
    }
}

/**
 * @brief Parses and routes incoming HTTP requests from the client socket.
 * * @param clientSocketDescriptor The file descriptor belonging to the connected client.
 * @param playbackService A reference to the engine that manages the decoding thread.
 */
void processIncomingHttpRequest(int clientSocketDescriptor, VideoPlaybackEngine& playbackService) {
    char requestBuffer[2048] = {0};
    ssize_t bytesRead = read(clientSocketDescriptor, requestBuffer, sizeof(requestBuffer) - 1);
    
    if (bytesRead <= 0) {
        close(clientSocketDescriptor);
        return;
    }

    std::string rawRequest(requestBuffer);
    std::string responseContent;
    int httpStatusCode = 200;

    size_t getPosition = rawRequest.find("GET ");
    size_t protocolPosition = rawRequest.find(" HTTP/");

    if (getPosition != std::string::npos) {
        if (protocolPosition != std::string::npos) {
            std::string requestPath = rawRequest.substr(getPosition + 4, protocolPosition - (getPosition + 4));
            std::string apiQueryKey = "/api?video=";
            
            if (requestPath.find(apiQueryKey) == 0) {
                std::string action = requestPath.substr(apiQueryKey.length());

                if (action == "stop") {
                    playbackService.requestStopPlayback();
                    responseContent = "{\"status\":\"success\", \"message\":\"Playback stopped, socket released.\"}";
                } else if (isValidSecureFilename(action) == false) {
                    httpStatusCode = 400; 
                    responseContent = "{\"status\":\"error\", \"message\":\"Invalid filename requested.\"}";
                } else {
                    bool targetFileExists = std::filesystem::exists(ApprovedVideoDirectory + action);
                    if (targetFileExists == false) {
                        httpStatusCode = 404; 
                        responseContent = "{\"status\":\"error\", \"message\":\"Target video file not found.\"}";
                    } else {
                        playbackService.requestVideoPlayback(action);
                        responseContent = "{\"status\":\"success\", \"message\":\"Playing: " + action + "\"}";
                    }
                }
            } else {
                httpStatusCode = 400;
                responseContent = "{\"status\":\"error\", \"message\":\"Invalid API endpoint.\"}";
            }
        } else {
            httpStatusCode = 400;
            responseContent = "{\"status\":\"error\", \"message\":\"Malformed HTTP request.\"}";
        }
    } else {
        httpStatusCode = 400;
        responseContent = "{\"status\":\"error\", \"message\":\"Malformed HTTP request.\"}";
    }

    std::string httpStatusText;
    if (httpStatusCode == 200) {
        httpStatusText = "OK";
    } else if (httpStatusCode == 400) {
        httpStatusText = "Bad Request";
    } else if (httpStatusCode == 404) {
        httpStatusText = "Not Found";
    } else {
        httpStatusText = "Unknown Error";
    }

    std::string httpHeaderResponse = "HTTP/1.1 ";
    httpHeaderResponse += std::to_string(httpStatusCode);
    httpHeaderResponse += " ";
    httpHeaderResponse += httpStatusText;
    httpHeaderResponse += "\r\n";
    httpHeaderResponse += "Content-Type: application/json\r\n";
    httpHeaderResponse += "Content-Length: ";
    httpHeaderResponse += std::to_string(responseContent.length());
    httpHeaderResponse += "\r\n";
    httpHeaderResponse += "Connection: close\r\n\r\n";

    send(clientSocketDescriptor, httpHeaderResponse.c_str(), httpHeaderResponse.length(), MSG_NOSIGNAL);
    send(clientSocketDescriptor, responseContent.c_str(), responseContent.length(), MSG_NOSIGNAL);
    close(clientSocketDescriptor);
}

/**
 * @brief Instantiates the TCP socket server and handles event polling loops.
 * * Uses standard POSIX sockets combined with a select() block to yield CPU time 
 * safely while remaining responsive to incoming requests and operating system 
 * shutdown signals.
 * * @param playbackService A reference to the engine that manages the decoding thread.
 */
void initializeNetworkService(VideoPlaybackEngine& playbackService) {
    int serverSocketFileDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocketFileDescriptor < 0) {
        return;
    }

    int socketReuseFlag = 1;
    setsockopt(serverSocketFileDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketReuseFlag, sizeof(socketReuseFlag));

    sockaddr_in serverNetAddress;
    serverNetAddress.sin_family = AF_INET;
    serverNetAddress.sin_addr.s_addr = INADDR_ANY;
    serverNetAddress.sin_port = htons(HttpServerPortNumber);

    int bindingStatus = bind(serverSocketFileDescriptor, (struct sockaddr*)&serverNetAddress, sizeof(serverNetAddress));
    if (bindingStatus < 0) {
        std::cerr << "Failed to bind network service to port " << HttpServerPortNumber << std::endl;
        close(serverSocketFileDescriptor);
        return;
    }
    
    int listeningStatus = listen(serverSocketFileDescriptor, 10);
    if (listeningStatus < 0) {
        std::cerr << "Failed to transition network service to listening mode." << std::endl;
        close(serverSocketFileDescriptor);
        return;
    }

    std::cout << "Service active at http://localhost:" << HttpServerPortNumber << std::endl;

    while (isGlobalApplicationRunning == true) {
        fd_set readFdSet;
        FD_ZERO(&readFdSet);
        FD_SET(serverSocketFileDescriptor, &readFdSet);
        
        // Timeout configuration re-instantiated here because select() modifies it
        timeval selectionTimeout;
        selectionTimeout.tv_sec = 1;
        selectionTimeout.tv_usec = 0;

        int networkEventStatus = select(serverSocketFileDescriptor + 1, &readFdSet, NULL, NULL, &selectionTimeout);
        
        if (networkEventStatus > 0) {
            int clientConnectionSocket = accept(serverSocketFileDescriptor, NULL, NULL);
            if (clientConnectionSocket >= 0) {
                processIncomingHttpRequest(clientConnectionSocket, playbackService);
            }
        }
    }
    close(serverSocketFileDescriptor);
}

int main(int argumentCount, char** argumentValues) {
    if (argumentCount < 2) {
        std::cout << "Usage: ./mp4toleds <mode>" << std::endl;
        return 1;
    }

    signal(SIGINT, systemSignalHandler);
    signal(SIGTERM, systemSignalHandler);

    LedLayout targetDisplayLayout;
    targetDisplayLayout.parseArgument(argumentValues[1]);

    VideoPlaybackEngine playbackEngine(targetDisplayLayout);
    initializeNetworkService(playbackEngine);

    return 0;
}