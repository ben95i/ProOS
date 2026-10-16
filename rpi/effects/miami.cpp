#include "ledlayout.h"
#include "opcclient.h"
#include "colorutils.h"
#include <chrono>
#include <thread>
#include <vector>
#include <random>
#include <cmath>

constexpr const char* TARGET_SERVER_ADDRESS = "127.0.0.1";
constexpr int TARGET_SERVER_PORT = 7890;
constexpr int TARGET_ANIMATION_FPS = 60;
constexpr int COLOR_CHANNELS_COUNT = 3;

int main(int argumentCount, char** argumentValues) {
    LedLayout mappedHardwareLayout;
    mappedHardwareLayout.parseArgument((argumentCount > 1) ? argumentValues[1] : "8x8");

    OPCClient networkDisplayClient(TARGET_SERVER_ADDRESS, TARGET_SERVER_PORT);
    
    int maximumHardwareMappingIndex = 0;
    for (const auto& individualHardwarePixel : mappedHardwareLayout.getPixels()) {
        if (individualHardwarePixel.opcIndex > maximumHardwareMappingIndex) {
            maximumHardwareMappingIndex = individualHardwarePixel.opcIndex;
        }
    }
    
    std::vector<uint8_t> outgoingNetworkPayloadBuffer((maximumHardwareMappingIndex + 1) * COLOR_CHANNELS_COUNT, 0);

    // getPixels() returns a copy of the whole list. Taking a reference to one element
    // of that temporary inside the frame loop left it pointing at freed memory, so
    // the list is copied once here and read from this copy.
    const std::vector<LedPixel> layoutPixels = mappedHardwareLayout.getPixels();
    int totalSystemPixelsCount = layoutPixels.size();
    
    std::mt19937 randomGeneratorStream(std::random_device{}());
    std::uniform_real_distribution<float> randomDistribution(0.0f, 1.0f);
    std::vector<float> randomizedPixelOffsets(totalSystemPixelsCount);
    for (auto& singleRandomValue : randomizedPixelOffsets) {
        singleRandomValue = randomDistribution(randomGeneratorStream);
    }

    auto applicationExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto frameRenderStartTime = std::chrono::steady_clock::now();
        // Time stays a double and every phase is wrapped to one period before it is
        // narrowed: a float running total loses its fine steps after about a day,
        // which made the animation visibly jerky.
        double rawElapsedSecondsCount = std::chrono::duration<double>(frameRenderStartTime - applicationExecutionStartTime).count();
        double scaledElapsedSeconds = rawElapsedSecondsCount * 0.6;
        float colorWavePhase = static_cast<float>(std::fmod(scaledElapsedSeconds / 4.0, 1.0));
        float regionWavePhase = static_cast<float>(std::fmod(scaledElapsedSeconds / 10.0, 1.0));
        float twinkleSweepPosition = static_cast<float>(std::fmod(scaledElapsedSeconds, 7.0));
        float twinkleTimePhase = static_cast<float>(std::fmod(rawElapsedSecondsCount * 0.07, 1.0));

        std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);

        for (int pixelIteratorIndex = 0; pixelIteratorIndex < totalSystemPixelsCount; ++pixelIteratorIndex) {
            const LedPixel& specificTargetPixel = layoutPixels[pixelIteratorIndex];
            
            float mutatedCoordinateX = specificTargetPixel.normX;
            float mutatedCoordinateY = specificTargetPixel.normY;
            float mutatedCoordinateZ = 0.0f; // Defaulting to 0.0 for 2D layouts

            mutatedCoordinateY += ColorUtils::waveCosine(mutatedCoordinateX + 0.2f * mutatedCoordinateZ, 0.0f, 1.0f, 0.0f, 0.6f);
            mutatedCoordinateZ += ColorUtils::waveCosine(mutatedCoordinateX, 0.0f, 1.0f, 0.0f, 0.3f);
            mutatedCoordinateX += ColorUtils::waveCosine(mutatedCoordinateY + mutatedCoordinateZ, 0.0f, 1.5f, 0.0f, 0.2f);

            // Rotate
            float temporarySwappedValue = mutatedCoordinateX;
            mutatedCoordinateX = mutatedCoordinateY;
            mutatedCoordinateY = mutatedCoordinateZ;
            mutatedCoordinateZ = temporarySwappedValue;

            float calculatedRedChannel = ColorUtils::waveCosine(mutatedCoordinateX, colorWavePhase, 2.5f, 0.0f, 1.0f);
            float calculatedGreenChannel = ColorUtils::waveCosine(mutatedCoordinateY, colorWavePhase, 2.5f, 0.0f, 1.0f);
            float calculatedBlueChannel = ColorUtils::waveCosine(mutatedCoordinateZ, colorWavePhase, 2.5f, 0.0f, 1.0f);
            ColorUtils::applyContrast(calculatedRedChannel, calculatedGreenChannel, calculatedBlueChannel, 0.5f, 1.4f);

            float initialClampingFactor = (calculatedRedChannel + calculatedGreenChannel + calculatedBlueChannel) / 2.0f;
            initialClampingFactor = ColorUtils::remapValue(initialClampingFactor, 0.4f, 0.5f, 0.0f, 1.0f);
            initialClampingFactor = ColorUtils::clampValue(initialClampingFactor, 0.0f, 1.0f);
            initialClampingFactor *= 0.9f;
            
            calculatedRedChannel *= initialClampingFactor;
            calculatedGreenChannel *= initialClampingFactor;
            calculatedBlueChannel *= initialClampingFactor;

            // Secondary black out regions
            float regionClampRed = ColorUtils::waveCosine(mutatedCoordinateX, regionWavePhase + 12.345f, 4.0f, 0.0f, 1.0f);
            float regionClampGreen = ColorUtils::waveCosine(mutatedCoordinateY, regionWavePhase + 24.536f, 4.0f, 0.0f, 1.0f);
            float regionClampBlue = ColorUtils::waveCosine(mutatedCoordinateZ, regionWavePhase + 34.675f, 4.0f, 0.0f, 1.0f);
            
            float secondaryClampingFactor = (regionClampRed + regionClampGreen + regionClampBlue) / 2.0f;
            secondaryClampingFactor = ColorUtils::remapValue(secondaryClampingFactor, 0.2f, 0.3f, 0.0f, 1.0f);
            secondaryClampingFactor = ColorUtils::clampValue(secondaryClampingFactor, 0.0f, 1.0f);
            
            calculatedRedChannel *= secondaryClampingFactor;
            calculatedGreenChannel *= secondaryClampingFactor;
            calculatedBlueChannel *= secondaryClampingFactor;

            calculatedGreenChannel = calculatedGreenChannel * 0.6f + ((calculatedRedChannel + calculatedBlueChannel) / 2.0f) * 0.4f;

            // Fade behind twinkle
            float twinkleBackgroundFade = std::pow(ColorUtils::waveCosine(twinkleSweepPosition - static_cast<float>(pixelIteratorIndex) / totalSystemPixelsCount, 0.0f, 7.0f, 0.0f, 1.0f), 20.0f);
            twinkleBackgroundFade = 1.0f - twinkleBackgroundFade * 0.2f;
            calculatedRedChannel *= twinkleBackgroundFade;
            calculatedGreenChannel *= twinkleBackgroundFade;
            calculatedBlueChannel *= twinkleBackgroundFade;

            // Twinkle Logic
            float twinkleDensityFactor = 0.1f;
            float currentTwinklePhase = std::fmod((randomizedPixelOffsets[pixelIteratorIndex] * 7.0f + twinkleTimePhase), 1.0f);
            currentTwinklePhase = std::abs(currentTwinklePhase * 2.0f - 1.0f);
            currentTwinklePhase = ColorUtils::remapValue(currentTwinklePhase, 0.0f, 1.0f, -1.0f / twinkleDensityFactor, 1.1f);
            currentTwinklePhase = ColorUtils::clampValue(currentTwinklePhase, -0.5f, 1.1f);
            currentTwinklePhase = std::pow(currentTwinklePhase, 5.0f);
            
            float twinkleMaskModifier = std::pow(ColorUtils::waveCosine(twinkleSweepPosition - static_cast<float>(pixelIteratorIndex) / totalSystemPixelsCount, 0.0f, 7.0f, 0.0f, 1.0f), 20.0f);
            currentTwinklePhase *= twinkleMaskModifier;
            currentTwinklePhase = ColorUtils::clampValue(currentTwinklePhase, -0.3f, 1.0f);
            
            calculatedRedChannel += currentTwinklePhase;
            calculatedGreenChannel += currentTwinklePhase;
            calculatedBlueChannel += currentTwinklePhase;

            int targetVectorBaseIndex = specificTargetPixel.opcIndex * COLOR_CHANNELS_COUNT;
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex]     = ColorUtils::floatToByte(calculatedRedChannel);
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 1] = ColorUtils::floatToByte(calculatedGreenChannel);
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 2] = ColorUtils::floatToByte(calculatedBlueChannel);
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size());
        std::this_thread::sleep_until(frameRenderStartTime + std::chrono::milliseconds(1000 / TARGET_ANIMATION_FPS));
    }
    return 0;
}