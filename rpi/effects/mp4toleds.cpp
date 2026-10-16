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
#include <system_error>
#include <sys/time.h>

// Native POSIX network headers
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

constexpr const char* OpcServerIpAddress = "127.0.0.1";
constexpr int OpcServerPortNumber = 7890;
constexpr int HttpServerPortNumber = 7860;
const std::string ApprovedVideoDirectory = "/opt/rpi/effects/videos/"; 

// Faster videos are thinned to this rate to prevent overwhelming the OPC server
constexpr double MaximumOutputFramesPerSecond = 60.0;
// Output frames the player will skip at once to catch up before it gives up and resyncs
constexpr int MaximumCatchUpFrames = 8;
// Each LED averages up to this many video samples across and down its cell
constexpr int MaximumTapsPerAxis = 8;
// Ceiling on the samples read per frame over all LEDs together
constexpr size_t MaximumTapsPerFrame = 32768;
// A control client that connects and then says nothing is dropped after this long
constexpr int HttpClientTimeoutSeconds = 2;

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
     * * @param layoutPixels The LED list, copied once from the layout by the caller.
     * @return int The total number of bytes required.
     */
    int calculateRequiredPayloadByteLength(const std::vector<LedPixel>& layoutPixels) const {
        int maximumHardwarePixelIndex = 0;
        for (const auto& individualPixel : layoutPixels) {
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
     * @brief Builds the table of frame byte offsets each LED averages its colour from.
     * * Every LED covers a rectangle of the video (its cell). Reading one video pixel 
     * per LED makes the LEDs shimmer, because fine detail and compression noise land 
     * on the LED at random. Instead each LED averages a small grid of sample points 
     * (taps) spread evenly over its cell, which approximates an area filter without 
     * touching the rest of the frame.
     * * @param layoutPixels The LED list.
     * @param targetAspectRatio The display's width / height.
     * @param frameWidth Width of the decoded frame in pixels.
     * @param frameHeight Height of the decoded frame in pixels.
     * @param frameRowByteStep Bytes from one frame row to the next.
     * @param tapByteOffsets Receives tapsPerLed offsets for each LED, in LED order.
     * @return int The number of taps per LED.
     */
    int buildTapByteOffsets(const std::vector<LedPixel>& layoutPixels, float targetAspectRatio,
                            int frameWidth, int frameHeight, size_t frameRowByteStep,
                            std::vector<size_t>& tapByteOffsets) const {
        float nativeVideoAspectRatio = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
        int croppedRenderWidth = frameWidth;
        int croppedRenderHeight = frameHeight;
        int renderOffsetX = 0;
        int renderOffsetY = 0;

        if (nativeVideoAspectRatio > targetAspectRatio) {
            croppedRenderWidth = static_cast<int>(frameHeight * targetAspectRatio);
            croppedRenderWidth = std::clamp(croppedRenderWidth, 1, frameWidth);
            renderOffsetX = (frameWidth - croppedRenderWidth) / 2;
        } else {
            croppedRenderHeight = static_cast<int>(frameWidth / targetAspectRatio);
            croppedRenderHeight = std::clamp(croppedRenderHeight, 1, frameHeight);
            renderOffsetY = (frameHeight - croppedRenderHeight) / 2;
        }

        // The size of one LED's cell in video pixels
        int logicalColumns = std::max(configuredDisplayLayout.getLogicalWidth(), 1);
        int logicalRows = std::max(configuredDisplayLayout.getLogicalHeight(), 1);
        float cellWidth = std::max(static_cast<float>(croppedRenderWidth) / logicalColumns, 1.0f);
        float cellHeight = std::max(static_cast<float>(croppedRenderHeight) / logicalRows, 1.0f);

        // More taps than the cell has pixels would only read the same pixel twice
        int tapsAcross = std::clamp(static_cast<int>(cellWidth), 1, MaximumTapsPerAxis);
        int tapsDown = std::clamp(static_cast<int>(cellHeight), 1, MaximumTapsPerAxis);

        // Keep the work per frame bounded on displays with many LEDs
        while (layoutPixels.size() * tapsAcross * tapsDown > MaximumTapsPerFrame) {
            if (tapsAcross == 1 && tapsDown == 1) {
                break;
            } else if (tapsAcross >= tapsDown) {
                tapsAcross -= 1;
            } else {
                tapsDown -= 1;
            }
        }

        tapByteOffsets.clear();
        tapByteOffsets.reserve(layoutPixels.size() * tapsAcross * tapsDown);

        for (const auto& activePixel : layoutPixels) {
            // normX runs from 0 to 1 across the LED centres, so the cell's left edge
            // runs from the crop's left edge to one cell short of its right edge.
            float cellLeft = renderOffsetX + activePixel.normX * (croppedRenderWidth - cellWidth);
            float cellTop = renderOffsetY + activePixel.normY * (croppedRenderHeight - cellHeight);

            for (int tapRow = 0; tapRow < tapsDown; ++tapRow) {
                int sampledPixelY = static_cast<int>(cellTop + (tapRow + 0.5f) * cellHeight / tapsDown);
                sampledPixelY = std::clamp(sampledPixelY, 0, frameHeight - 1);

                for (int tapColumn = 0; tapColumn < tapsAcross; ++tapColumn) {
                    int sampledPixelX = static_cast<int>(cellLeft + (tapColumn + 0.5f) * cellWidth / tapsAcross);
                    sampledPixelX = std::clamp(sampledPixelX, 0, frameWidth - 1);

                    size_t byteOffset = static_cast<size_t>(sampledPixelY) * frameRowByteStep
                                      + static_cast<size_t>(sampledPixelX) * 3;
                    tapByteOffsets.push_back(byteOffset);
                }
            }
        }

        return tapsAcross * tapsDown;
    }

    /**
     * @brief The core worker thread responsible for decoding video and transmitting pixels.
     * * This function runs endlessly until the application shuts down. It waits for 
     * video requests, opens the stream via hardware acceleration, dynamically 
     * determines the frame rate, and maps video pixels to physical LED coordinates.
     */
    void videoDecodingTask() {
        // getPixels() returns a copy of the whole LED list under a mutex, so it is
        // taken ONCE here and never called from the per-frame loops.
        const std::vector<LedPixel> layoutPixels = configuredDisplayLayout.getPixels();
        const float targetAspectRatio = configuredDisplayLayout.getTargetAspect();

        // Memory Optimization: Allocate buffers ONCE for the entire lifecycle of the thread
        const int requiredPayloadByteLength = calculateRequiredPayloadByteLength(layoutPixels);
        std::vector<uint8_t> framePayloadBuffer(requiredPayloadByteLength, 0);
        std::vector<size_t> tapByteOffsets;
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
                if (hasNewVideoRequest == false) {
                    isPlaybackActive = false;
                }
                continue;
            }

            // Dynamically calculate the frame rate based on the video file metadata
            double framesPerSecond = hardwareVideoCaptureStream.get(cv::CAP_PROP_FPS);
            
            // Fallback default if metadata is missing or corrupted (NaN fails both comparisons)
            if (!(framesPerSecond >= 1.0 && framesPerSecond <= 1000.0)) {
                framesPerSecond = 30.0;
            }
            
            // A video faster than the output cap keeps its real speed: only every
            // Nth frame is shown, and the frames between are decoded but not converted.
            int sourceFrameStride = static_cast<int>(std::ceil(framesPerSecond / MaximumOutputFramesPerSecond));
            if (sourceFrameStride < 1) {
                sourceFrameStride = 1;
            }
            
            long long frameDurationNanoseconds = static_cast<long long>(1000000000.0 * sourceFrameStride / framesPerSecond);
            const std::chrono::nanoseconds outputFrameDuration(frameDurationNanoseconds);
            
            int lastCapturedFrameWidth = -1;
            int lastCapturedFrameHeight = -1;
            size_t lastCapturedFrameRowByteStep = 0;
            int tapsPerLed = 1;
            bool hasRewoundWithoutFrame = false;
            bool wasOpcServerConnected = false;

            // The moment the next frame is due on the LEDs. It advances by exactly one
            // frame duration per frame, so decode time and wake-up latency do not
            // accumulate into the video running slow.
            auto nextFrameDeadline = std::chrono::steady_clock::now();

            // Inner Playback Loop
            while (isGlobalApplicationRunning == true) {
                int sourceFramesToSkip = sourceFrameStride - 1;

                // When decoding has fallen behind, drop frames rather than play in slow motion
                auto currentTimestamp = std::chrono::steady_clock::now();
                if (currentTimestamp > nextFrameDeadline + outputFrameDuration) {
                    long long outputFramesBehind = (currentTimestamp - nextFrameDeadline) / outputFrameDuration;
                    if (outputFramesBehind > MaximumCatchUpFrames) {
                        // Too far behind to catch up by skipping: carry on from here
                        nextFrameDeadline = currentTimestamp;
                    } else {
                        sourceFramesToSkip += static_cast<int>(outputFramesBehind) * sourceFrameStride;
                        nextFrameDeadline += outputFramesBehind * outputFrameDuration;
                    }
                }

                // grab() decodes a frame without the colour conversion and copy of read()
                bool isFrameAvailable = true;
                for (int skippedFrames = 0; skippedFrames < sourceFramesToSkip; ++skippedFrames) {
                    if (hardwareVideoCaptureStream.grab() == false) {
                        isFrameAvailable = false;
                        break;
                    }
                }
                if (isFrameAvailable == true) {
                    isFrameAvailable = hardwareVideoCaptureStream.read(currentDecodedFrame);
                }
                if (currentDecodedFrame.empty() == true) {
                    isFrameAvailable = false;
                }

                if (isFrameAvailable == false) {
                    // End of file: rewind for a seamless loop. A file that gives no frame
                    // straight after a rewind is unplayable, and retrying it would spin
                    // this thread at full CPU with no way to stop it.
                    bool isRewound = false;
                    if (hasRewoundWithoutFrame == false) {
                        isRewound = hardwareVideoCaptureStream.set(cv::CAP_PROP_POS_FRAMES, 0);
                    }
                    if (isRewound == false) {
                        std::cerr << "Error: No frames could be read from " << activeFilePath << std::endl;
                        break;
                    }
                    hasRewoundWithoutFrame = true;
                    nextFrameDeadline = std::chrono::steady_clock::now();
                    continue;
                }
                hasRewoundWithoutFrame = false;

                if (currentDecodedFrame.type() != CV_8UC3) {
                    std::cerr << "Error: Unsupported pixel format in " << activeFilePath << std::endl;
                    break;
                }

                // Recalculate mapping offsets ONLY if resolution changes to save CPU cycles
                size_t frameRowByteStep = currentDecodedFrame.step[0];
                if (currentDecodedFrame.cols != lastCapturedFrameWidth
                    || currentDecodedFrame.rows != lastCapturedFrameHeight
                    || frameRowByteStep != lastCapturedFrameRowByteStep) {
                    tapsPerLed = buildTapByteOffsets(layoutPixels, targetAspectRatio,
                                                     currentDecodedFrame.cols, currentDecodedFrame.rows,
                                                     frameRowByteStep, tapByteOffsets);
                    
                    lastCapturedFrameWidth = currentDecodedFrame.cols; 
                    lastCapturedFrameHeight = currentDecodedFrame.rows;
                    lastCapturedFrameRowByteStep = frameRowByteStep;
                }

                // Average each LED's taps, rounding to the nearest level
                const uint8_t* rawOpenCvBufferData = currentDecodedFrame.data;
                const size_t* nextTapByteOffset = tapByteOffsets.data();
                const unsigned int roundingHalf = tapsPerLed / 2;
                
                for (const auto& activePixel : layoutPixels) {
                    unsigned int blueSum = 0;
                    unsigned int greenSum = 0;
                    unsigned int redSum = 0;
                    
                    for (int tapIndex = 0; tapIndex < tapsPerLed; ++tapIndex) {
                        const uint8_t* sourceBgrPixel = rawOpenCvBufferData + *nextTapByteOffset;
                        nextTapByteOffset += 1;
                        blueSum  += sourceBgrPixel[0];
                        greenSum += sourceBgrPixel[1];
                        redSum   += sourceBgrPixel[2];
                    }
                    
                    int baseIndex = activePixel.opcIndex * 3;
                    framePayloadBuffer[baseIndex]     = static_cast<uint8_t>((redSum + roundingHalf) / tapsPerLed);   // Red
                    framePayloadBuffer[baseIndex + 1] = static_cast<uint8_t>((greenSum + roundingHalf) / tapsPerLed); // Green
                    framePayloadBuffer[baseIndex + 2] = static_cast<uint8_t>((blueSum + roundingHalf) / tapsPerLed);  // Blue
                }

                // Hold the frame until it is due, but allow immediate wake-up if playback state
                // changes. Checking here, straight before the send, also keeps a frame from
                // reaching the LEDs after a STOP that arrived while the file was opening.
                bool stateChangedInterrupt = false;
                {
                    std::unique_lock<std::mutex> lock(playbackStateMutex);
                    stateChangedInterrupt = playbackStateConditionVariable.wait_until(
                        lock, 
                        nextFrameDeadline, 
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
                }

                if (stateChangedInterrupt == true) {
                    break; // Exit inner loop instantly on STOP or new video request
                }

                // A false return is not fatal: the client drops a frame when the server is
                // backed up and reconnects by itself, so playback carries on either way.
                networkPixelTransmitter.sendPixelPayload(0, framePayloadBuffer.data(), framePayloadBuffer.size());
                
                bool isOpcServerConnected = networkPixelTransmitter.isConnected();
                if (wasOpcServerConnected == true && isOpcServerConnected == false) {
                    std::cerr << "Lost connection to OPC Server, reconnecting." << std::endl;
                }
                wasOpcServerConnected = isOpcServerConnected;

                nextFrameDeadline += outputFrameDuration;
            }

            // Proper memory cleanup for this specific video file
            hardwareVideoCaptureStream.release(); 
            currentDecodedFrame.release();        
            releaseOpcResource();

            // Playback that ended by itself (unplayable file) must not look active
            {
                std::lock_guard<std::mutex> lock(playbackStateMutex);
                if (hasNewVideoRequest == false) {
                    isPlaybackActive = false;
                }
            }
        }
    }

public:
    VideoPlaybackEngine(const LedLayout& layout) 
        : configuredDisplayLayout(layout), networkPixelTransmitter(OpcServerIpAddress, OpcServerPortNumber) {
        
        // The error_code overloads do not throw: the service user may not be allowed
        // to create the directory, and that must not abort the program at startup.
        std::error_code fileSystemError;
        bool directoryExists = std::filesystem::exists(ApprovedVideoDirectory, fileSystemError);
        if (directoryExists == false) {
            std::filesystem::create_directory(ApprovedVideoDirectory, fileSystemError);
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
    // Requests are served one at a time on the main thread, so a silent client
    // must not be able to hold the control API (and with it STOP) forever.
    timeval clientSocketTimeout;
    clientSocketTimeout.tv_sec = HttpClientTimeoutSeconds;
    clientSocketTimeout.tv_usec = 0;
    setsockopt(clientSocketDescriptor, SOL_SOCKET, SO_RCVTIMEO, &clientSocketTimeout, sizeof(clientSocketTimeout));
    setsockopt(clientSocketDescriptor, SOL_SOCKET, SO_SNDTIMEO, &clientSocketTimeout, sizeof(clientSocketTimeout));

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
                    // is_regular_file with an error_code never throws, and refuses a directory
                    std::error_code fileSystemError;
                    bool targetFileExists = std::filesystem::is_regular_file(ApprovedVideoDirectory + action, fileSystemError);
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
 * @return true If the service ran until shutdown.
 * @return false If the service could not be started.
 */
bool initializeNetworkService(VideoPlaybackEngine& playbackService) {
    int serverSocketFileDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocketFileDescriptor < 0) {
        return false;
    }

    int socketReuseFlag = 1;
    setsockopt(serverSocketFileDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketReuseFlag, sizeof(socketReuseFlag));

    sockaddr_in serverNetAddress;
    serverNetAddress.sin_family = AF_INET;
    // Loopback only: the one caller is ledparser.js on this same host
    serverNetAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverNetAddress.sin_port = htons(HttpServerPortNumber);

    int bindingStatus = bind(serverSocketFileDescriptor, (struct sockaddr*)&serverNetAddress, sizeof(serverNetAddress));
    if (bindingStatus < 0) {
        std::cerr << "Failed to bind network service to port " << HttpServerPortNumber << std::endl;
        close(serverSocketFileDescriptor);
        return false;
    }
    
    int listeningStatus = listen(serverSocketFileDescriptor, 10);
    if (listeningStatus < 0) {
        std::cerr << "Failed to transition network service to listening mode." << std::endl;
        close(serverSocketFileDescriptor);
        return false;
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
    return true;
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

    if (targetDisplayLayout.getPixels().empty() == true) {
        std::cerr << "Error: Layout \"" << argumentValues[1] << "\" has no LEDs." << std::endl;
        return 1;
    }

    VideoPlaybackEngine playbackEngine(targetDisplayLayout);
    bool serviceRanUntilShutdown = initializeNetworkService(playbackEngine);

    // A non-zero exit lets systemd see a failed start (port already in use)
    if (serviceRanUntilShutdown == false) {
        return 1;
    }
    return 0;
}