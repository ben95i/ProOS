#include "LEDLayout.h"
#include "OPCClient.h"

#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <cmath>
#include <chrono>
#include <thread>
#include <signal.h>
#include <algorithm>

constexpr const char* OPC_SERVER_IP = "127.0.0.1";
constexpr int OPC_SERVER_PORT = 7890;
constexpr int TARGET_PLAYBACK_FPS = 25; 

volatile bool globalApplicationRunningFlag = true;
void systemSignalHandler(int signalCode) { globalApplicationRunningFlag = false; }

int main(int argumentCount, char** argumentValues) {
    if (argumentCount < 3) {
        std::cout << "Usage: ./mp4toleds <mode> <video_path>" << std::endl;
        return 1;
    }

    signal(SIGINT, systemSignalHandler);

    LEDLayout targetDisplayLayout;
    targetDisplayLayout.parseArgument(argumentValues[1]);

    cv::VideoCapture videoCaptureStream(argumentValues[2], cv::CAP_FFMPEG, {
        cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY
    });
    
    if (!videoCaptureStream.isOpened()) {
        std::cerr << "Failed to open video file with GStreamer hardware pipeline." << std::endl;
        return 1;
    }

    videoCaptureStream.set(cv::CAP_PROP_BUFFERSIZE, 1);

    OPCClient networkTransmitter(OPC_SERVER_IP, OPC_SERVER_PORT);

    int maximumHardwarePixelIndex = 0;
    for (const auto& individualPixel : targetDisplayLayout.pixels) {
        if (individualPixel.opc_index > maximumHardwarePixelIndex) {
            maximumHardwarePixelIndex = individualPixel.opc_index;
        }
    }
    
    int requiredPayloadByteLength = (maximumHardwarePixelIndex + 1) * 3;
    std::vector<uint8_t> framePayloadBuffer(requiredPayloadByteLength, 0);

    const std::chrono::nanoseconds targetFrameDuration(1000000000 / TARGET_PLAYBACK_FPS);
    std::vector<size_t> textureLookupOffsets;
    int lastCapturedFrameWidth = -1, lastCapturedFrameHeight = -1;
    cv::Mat currentDecodedFrame;

    std::cout << "Streaming at " << TARGET_PLAYBACK_FPS << " FPS." << std::endl;

    while (globalApplicationRunningFlag) {
        auto currentFrameStartTimestamp = std::chrono::steady_clock::now();

        videoCaptureStream >> currentDecodedFrame;
        if (currentDecodedFrame.empty()) {
            videoCaptureStream.set(cv::CAP_PROP_POS_FRAMES, 0); 
            continue;
        }

        if (currentDecodedFrame.cols != lastCapturedFrameWidth || currentDecodedFrame.rows != lastCapturedFrameHeight) {
            textureLookupOffsets.clear();
            float nativeVideoAspectRatio = static_cast<float>(currentDecodedFrame.cols) / static_cast<float>(currentDecodedFrame.rows);
            int croppedRenderWidth = currentDecodedFrame.cols;
            int croppedRenderHeight = currentDecodedFrame.rows;
            int renderOffsetX = 0;
            int renderOffsetY = 0;

            if (nativeVideoAspectRatio > targetDisplayLayout.targetAspect) {
                croppedRenderWidth = static_cast<int>(currentDecodedFrame.rows * targetDisplayLayout.targetAspect);
                renderOffsetX = (currentDecodedFrame.cols - croppedRenderWidth) / 2;
            } else {
                croppedRenderHeight = static_cast<int>(currentDecodedFrame.cols / targetDisplayLayout.targetAspect);
                renderOffsetY = (currentDecodedFrame.rows - croppedRenderHeight) / 2;
            }

            for (const auto& activePixel : targetDisplayLayout.pixels) {
                int sampledPixelX = std::clamp(renderOffsetX + static_cast<int>(activePixel.norm_x * (croppedRenderWidth - 1)), 0, currentDecodedFrame.cols - 1);
                int sampledPixelY = std::clamp(renderOffsetY + static_cast<int>(activePixel.norm_y * (croppedRenderHeight - 1)), 0, currentDecodedFrame.rows - 1);
                textureLookupOffsets.push_back((sampledPixelY * currentDecodedFrame.cols + sampledPixelX) * 3);
            }
            lastCapturedFrameWidth = currentDecodedFrame.cols; 
            lastCapturedFrameHeight = currentDecodedFrame.rows;
        }

        uint8_t* rawOpenCvBufferData = currentDecodedFrame.data;
        for (size_t arrayLoopIndex = 0; arrayLoopIndex < targetDisplayLayout.pixels.size(); ++arrayLoopIndex) {
            uint8_t* sourceBgrPixel = &rawOpenCvBufferData[textureLookupOffsets[arrayLoopIndex]];
            int baseVectorIndex = targetDisplayLayout.pixels[arrayLoopIndex].opc_index * 3;
            framePayloadBuffer[baseVectorIndex]     = sourceBgrPixel[2]; 
            framePayloadBuffer[baseVectorIndex + 1] = sourceBgrPixel[1]; 
            framePayloadBuffer[baseVectorIndex + 2] = sourceBgrPixel[0]; 
        }

        if (!networkTransmitter.sendPixelPayload(0, framePayloadBuffer.data(), framePayloadBuffer.size())) {
            break;
        }
        std::this_thread::sleep_until(currentFrameStartTimestamp + targetFrameDuration);
    }

    std::fill(framePayloadBuffer.begin(), framePayloadBuffer.end(), 0);
    networkTransmitter.sendPixelPayload(0, framePayloadBuffer.data(), framePayloadBuffer.size());
    
    return 0;
}