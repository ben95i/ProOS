#include "LEDLayout.h"
#include "OPCClient.h"
#include "ColorUtils.h"
#include <chrono>
#include <thread>
#include <vector>
#include <random>

constexpr const char* TARGET_SERVER_ADDRESS = "127.0.0.1";
constexpr int TARGET_SERVER_PORT = 7890;
constexpr int TARGET_ANIMATION_FPS = 60;
constexpr int COLOR_CHANNELS_COUNT = 3;

int main(int argumentCount, char** argumentValues) {
    LEDLayout mappedHardwareLayout;
    mappedHardwareLayout.parseArgument((argumentCount > 1) ? argumentValues[1] : "8x8");

    OPCClient networkDisplayClient(TARGET_SERVER_ADDRESS, TARGET_SERVER_PORT);
    
    int maximumHardwareMappingIndex = 0;
    for (const auto& individualHardwarePixel : mappedHardwareLayout.pixels) {
        if (individualHardwarePixel.opc_index > maximumHardwareMappingIndex) {
            maximumHardwareMappingIndex = individualHardwarePixel.opc_index;
        }
    }
    
    std::vector<uint8_t> outgoingNetworkPayloadBuffer((maximumHardwareMappingIndex + 1) * COLOR_CHANNELS_COUNT, 0);
    int totalSystemPixelsCount = mappedHardwareLayout.pixels.size();
    
    std::mt19937 randomGeneratorStream(std::random_device{}());
    std::uniform_real_distribution<float> randomDistribution(0.0f, 1.0f);
    std::vector<float> randomizedPixelOffsets(totalSystemPixelsCount);
    for (auto& singleRandomValue : randomizedPixelOffsets) {
        singleRandomValue = randomDistribution(randomGeneratorStream);
    }

    auto applicationExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto frameRenderStartTime = std::chrono::steady_clock::now();
        float rawElapsedSecondsCount = std::chrono::duration<float>(frameRenderStartTime - applicationExecutionStartTime).count();
        float scaledElapsedSeconds = rawElapsedSecondsCount * 0.6f;

        std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);

        for (int pixelIteratorIndex = 0; pixelIteratorIndex < totalSystemPixelsCount; ++pixelIteratorIndex) {
            const auto& specificTargetPixel = mappedHardwareLayout.pixels[pixelIteratorIndex];
            
            float mutatedCoordinateX = specificTargetPixel.norm_x;
            float mutatedCoordinateY = specificTargetPixel.norm_y;
            float mutatedCoordinateZ = 0.0f; // Defaulting to 0.0 for 2D layouts

            mutatedCoordinateY += ColorUtils::waveCosine(mutatedCoordinateX + 0.2f * mutatedCoordinateZ, 0.0f, 1.0f, 0.0f, 0.6f);
            mutatedCoordinateZ += ColorUtils::waveCosine(mutatedCoordinateX, 0.0f, 1.0f, 0.0f, 0.3f);
            mutatedCoordinateX += ColorUtils::waveCosine(mutatedCoordinateY + mutatedCoordinateZ, 0.0f, 1.5f, 0.0f, 0.2f);

            if (pixelIteratorIndex % 7 == 0) {
                mutatedCoordinateX += std::fmod((pixelIteratorIndex * 123.0f), 5.0f) / totalSystemPixelsCount * 32.12f;
                mutatedCoordinateY += std::fmod((pixelIteratorIndex * 137.0f), 5.0f) / totalSystemPixelsCount * 22.23f;
                mutatedCoordinateZ += std::fmod((pixelIteratorIndex * 147.0f), 7.0f) / totalSystemPixelsCount * 44.34f;
            }

            float calculatedRedChannel = ColorUtils::waveCosine(mutatedCoordinateX, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            float calculatedGreenChannel = ColorUtils::waveCosine(mutatedCoordinateY, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            float calculatedBlueChannel = ColorUtils::waveCosine(mutatedCoordinateZ, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            ColorUtils::applyContrast(calculatedRedChannel, calculatedGreenChannel, calculatedBlueChannel, 0.5f, 1.5f);

            // Stretched vertical smears
            float stretchedVerticalSmear = std::pow(ColorUtils::waveCosine(static_cast<float>(pixelIteratorIndex) / totalSystemPixelsCount, scaledElapsedSeconds * 0.1f, 0.07f, 0.0f, 1.0f), 5.0f) * 0.3f;
            calculatedRedChannel += stretchedVerticalSmear;
            calculatedGreenChannel += stretchedVerticalSmear;
            calculatedBlueChannel += stretchedVerticalSmear;
            
            // Twinkle Logic
            float twinkleAnimationSpeed = 0.05f;
            float twinkleDensityFactor = 0.2f;
            float currentTwinklePhase = std::fmod((randomizedPixelOffsets[pixelIteratorIndex] * 7.0f + rawElapsedSecondsCount * twinkleAnimationSpeed), 1.0f);
            currentTwinklePhase = std::abs(currentTwinklePhase * 2.0f - 1.0f);
            currentTwinklePhase = ColorUtils::remapValue(currentTwinklePhase, 0.0f, 1.0f, -1.0f / twinkleDensityFactor, 1.1f);
            currentTwinklePhase = ColorUtils::clampValue(currentTwinklePhase, -0.5f, 1.1f);
            currentTwinklePhase = std::pow(currentTwinklePhase, 6.0f);
            
            float twinkleFadingMask = std::pow(ColorUtils::waveCosine(scaledElapsedSeconds - static_cast<float>(pixelIteratorIndex) / totalSystemPixelsCount, 0.0f, 7.0f, 0.0f, 1.0f), 20.0f);
            currentTwinklePhase *= twinkleFadingMask;
            currentTwinklePhase = ColorUtils::clampValue(currentTwinklePhase, -0.3f, 1.0f);
            
            calculatedRedChannel += currentTwinklePhase;
            calculatedGreenChannel += currentTwinklePhase;
            calculatedBlueChannel += currentTwinklePhase;

            int targetVectorBaseIndex = specificTargetPixel.opc_index * COLOR_CHANNELS_COUNT;
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex]     = ColorUtils::floatToByte(calculatedRedChannel);
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 1] = ColorUtils::floatToByte(calculatedGreenChannel);
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 2] = ColorUtils::floatToByte(calculatedBlueChannel);
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size());
        std::this_thread::sleep_until(frameRenderStartTime + std::chrono::milliseconds(1000 / TARGET_ANIMATION_FPS));
    }
    return 0;
}