#include "ledlayout.h"
#include "opcclient.h"
#include "colorutils.h"
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
    
    // Generate constant random values per pixel
    std::mt19937 randomGeneratorStream(std::random_device{}());
    std::uniform_real_distribution<float> randomDistribution(0.0f, 1.0f);
    std::vector<float> randomizedPixelOffsets(mappedHardwareLayout.pixels.size());
    for (auto& singleRandomValue : randomizedPixelOffsets) {
        singleRandomValue = randomDistribution(randomGeneratorStream);
    }

    auto applicationExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto frameRenderStartTime = std::chrono::steady_clock::now();
        float rawElapsedSecondsCount = std::chrono::duration<float>(frameRenderStartTime - applicationExecutionStartTime).count();
        float scaledElapsedSeconds = rawElapsedSecondsCount * 0.6f;

        std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);

        for (size_t pixelIteratorIndex = 0; pixelIteratorIndex < mappedHardwareLayout.pixels.size(); ++pixelIteratorIndex) {
            const auto& specificTargetPixel = mappedHardwareLayout.pixels[pixelIteratorIndex];
            
            float mutatedCoordinateX = specificTargetPixel.norm_x;
            float mutatedCoordinateY = specificTargetPixel.norm_y;
            float mutatedCoordinateZ = 0.0f; // Defaulting to 0.0 for 2D layouts

            mutatedCoordinateY += ColorUtils::waveCosine(mutatedCoordinateX + 0.2f * mutatedCoordinateZ, 0.0f, 1.0f, 0.0f, 0.6f);
            mutatedCoordinateZ += ColorUtils::waveCosine(mutatedCoordinateX, 0.0f, 1.0f, 0.0f, 0.3f);
            mutatedCoordinateX += ColorUtils::waveCosine(mutatedCoordinateY + mutatedCoordinateZ, 0.0f, 1.5f, 0.0f, 0.2f);

            // Rotate coordinates
            float temporarySwappedValue = mutatedCoordinateX;
            mutatedCoordinateX = mutatedCoordinateY;
            mutatedCoordinateY = mutatedCoordinateZ;
            mutatedCoordinateZ = temporarySwappedValue;

            float calculatedRedChannel = ColorUtils::waveCosine(mutatedCoordinateX, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            float calculatedGreenChannel = ColorUtils::waveCosine(mutatedCoordinateY, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            float calculatedBlueChannel = ColorUtils::waveCosine(mutatedCoordinateZ, scaledElapsedSeconds / 4.0f, 2.0f, 0.0f, 1.0f);
            
            ColorUtils::applyContrast(calculatedRedChannel, calculatedGreenChannel, calculatedBlueChannel, 0.5f, 1.5f);

            // Black out regions
            float regionClampRed = ColorUtils::waveCosine(mutatedCoordinateX, scaledElapsedSeconds / 10.0f + 12.345f, 3.0f, 0.0f, 1.0f);
            float regionClampGreen = ColorUtils::waveCosine(mutatedCoordinateY, scaledElapsedSeconds / 10.0f + 24.536f, 3.0f, 0.0f, 1.0f);
            float regionClampBlue = ColorUtils::waveCosine(mutatedCoordinateZ, scaledElapsedSeconds / 10.0f + 34.675f, 3.0f, 0.0f, 1.0f);
            
            float clampingFactorValue = (regionClampRed + regionClampGreen + regionClampBlue) / 2.0f;
            clampingFactorValue = ColorUtils::remapValue(clampingFactorValue, 0.8f, 0.9f, 0.0f, 1.0f);
            clampingFactorValue = ColorUtils::clampValue(clampingFactorValue, 0.0f, 1.0f);
            
            calculatedRedChannel *= clampingFactorValue;
            calculatedGreenChannel *= clampingFactorValue;
            calculatedBlueChannel *= clampingFactorValue;

            // Color scheme shift: fade towards blue-and-orange
            calculatedGreenChannel = calculatedGreenChannel * 0.6f + ((calculatedRedChannel + calculatedBlueChannel) / 2.0f) * 0.4f;

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