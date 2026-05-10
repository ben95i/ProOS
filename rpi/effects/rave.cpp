#include "OPCClient.h"
#include "ColorUtils.h"
#include <chrono>
#include <thread>
#include <vector>

constexpr const char* TARGET_SERVER_ADDRESS = "127.0.0.1";
constexpr int TARGET_SERVER_PORT = 7890;
constexpr int TOTAL_LAYOUT_PIXELS = 128;
constexpr int TARGET_ANIMATION_FPS = 30;
constexpr int COLOR_CHANNELS_COUNT = 3;

constexpr float FREQUENCY_RED_CHANNEL = 24.0f;
constexpr float FREQUENCY_GREEN_CHANNEL = 24.0f;
constexpr float FREQUENCY_BLUE_CHANNEL = 24.0f;

constexpr float SPEED_RED_CHANNEL = 7.0f * 3.0f;
constexpr float SPEED_GREEN_CHANNEL = -13.0f * 3.0f;
constexpr float SPEED_BLUE_CHANNEL = 19.0f * 3.0f;

int main() {
    OPCClient networkDisplayClient(TARGET_SERVER_ADDRESS, TARGET_SERVER_PORT);
    std::vector<uint8_t> outgoingNetworkPayloadBuffer(TOTAL_LAYOUT_PIXELS * COLOR_CHANNELS_COUNT, 0);
    auto applicationExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto frameRenderStartTime = std::chrono::steady_clock::now();
        float elapsedSecondsCount = std::chrono::duration<float>(frameRenderStartTime - applicationExecutionStartTime).count();

        for (int currentPixelIndex = 0; currentPixelIndex < TOTAL_LAYOUT_PIXELS; ++currentPixelIndex) {
            float fractionalPosition = static_cast<float>(currentPixelIndex) / TOTAL_LAYOUT_PIXELS;
            
            // Diagonal black stripes computation
            float jitteredFractionalPosition = std::fmod((fractionalPosition * 77.0f), 37.0f);
            float baseBlackStripeMask = ColorUtils::waveCosine(jitteredFractionalPosition, elapsedSecondsCount * 0.05f, 1.0f, -1.5f, 1.5f);
            float movingStripeOffset = ColorUtils::waveCosine(elapsedSecondsCount, 0.9f, 60.0f, -0.5f, 3.0f);
            float finalBlackStripeMask = ColorUtils::clampValue(baseBlackStripeMask + movingStripeOffset, 0.0f, 1.0f);
            
            // 3 out-of-sync sine waves mapped 0 to 255
            float rawRedIntensity = finalBlackStripeMask * ColorUtils::remapValue(std::cos((elapsedSecondsCount / SPEED_RED_CHANNEL + fractionalPosition * FREQUENCY_RED_CHANNEL) * M_PI * 2.0f), -1.0f, 1.0f, 0.0f, 255.0f);
            float rawGreenIntensity = finalBlackStripeMask * ColorUtils::remapValue(std::cos((elapsedSecondsCount / SPEED_GREEN_CHANNEL + fractionalPosition * FREQUENCY_GREEN_CHANNEL) * M_PI * 2.0f), -1.0f, 1.0f, 0.0f, 255.0f);
            float rawBlueIntensity = finalBlackStripeMask * ColorUtils::remapValue(std::cos((elapsedSecondsCount / SPEED_BLUE_CHANNEL + fractionalPosition * FREQUENCY_BLUE_CHANNEL) * M_PI * 2.0f), -1.0f, 1.0f, 0.0f, 255.0f);

            int targetVectorBaseIndex = currentPixelIndex * COLOR_CHANNELS_COUNT;
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex]     = static_cast<uint8_t>(ColorUtils::clampValue(rawRedIntensity, 0.0f, 255.0f));
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 1] = static_cast<uint8_t>(ColorUtils::clampValue(rawGreenIntensity, 0.0f, 255.0f));
            outgoingNetworkPayloadBuffer[targetVectorBaseIndex + 2] = static_cast<uint8_t>(ColorUtils::clampValue(rawBlueIntensity, 0.0f, 255.0f));
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size());
        std::this_thread::sleep_until(frameRenderStartTime + std::chrono::milliseconds(1000 / TARGET_ANIMATION_FPS));
    }
    return 0;
}