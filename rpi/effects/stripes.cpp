#include "ledlayout.h"
#include "opcclient.h"
#include "colorutils.h"
#include <chrono>
#include <thread>
#include <vector>

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
    int totalSystemPixels = mappedHardwareLayout.getPixels().size();
    auto applicationExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto frameRenderStartTime = std::chrono::steady_clock::now();
        float elapsedSecondsCount = std::chrono::duration<float>(frameRenderStartTime - applicationExecutionStartTime).count();

        std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);

        for (int pixelIteratorIndex = 0; pixelIteratorIndex < totalSystemPixels; ++pixelIteratorIndex) {
            const auto& specificTargetPixel = mappedHardwareLayout.getPixels()[pixelIteratorIndex];
            
            float calculatedRedChannel = ColorUtils::waveCosine(specificTargetPixel.normX, elapsedSecondsCount / 4.0f, 1.0f, 0.0f, 0.7f);
            float calculatedGreenChannel = ColorUtils::waveCosine(specificTargetPixel.normY, elapsedSecondsCount / 4.0f, 1.0f, 0.0f, 0.7f);
            float calculatedBlueChannel = ColorUtils::waveCosine(0.0f, elapsedSecondsCount / 4.0f, 1.0f, 0.0f, 0.7f);
            
            ColorUtils::applyContrast(calculatedRedChannel, calculatedGreenChannel, calculatedBlueChannel, 0.5f, 2.0f);

            // Make a moving white dot showing pixel index sequence
            float activeSparkIndexTracker = std::fmod((elapsedSecondsCount * 80.0f), totalSystemPixels);
            float structuralSparkRadius = 8.0f;
            float derivedSparkValue = std::max(0.0f, (structuralSparkRadius - ColorUtils::modularDistance(static_cast<float>(pixelIteratorIndex), activeSparkIndexTracker, static_cast<float>(totalSystemPixels))) / structuralSparkRadius);
            derivedSparkValue = std::min(1.0f, derivedSparkValue * 2.0f);
            
            calculatedRedChannel += derivedSparkValue;
            calculatedGreenChannel += derivedSparkValue;
            calculatedBlueChannel += derivedSparkValue;

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