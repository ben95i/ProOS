#include "OPCClient.h"
#include <chrono>
#include <thread>
#include <vector>

constexpr const char* TARGET_SERVER_ADDRESS = "127.0.0.1";
constexpr int TARGET_SERVER_PORT = 7890;
constexpr int TOTAL_HARDWARE_LEDS = 512;
constexpr int COLOR_CHANNELS_COUNT = 3;
constexpr int CHASE_SLEEP_MILLISECONDS = 10;

int main() {
    OPCClient networkDisplayClient(TARGET_SERVER_ADDRESS, TARGET_SERVER_PORT);
    std::vector<uint8_t> outgoingNetworkPayloadBuffer(TOTAL_HARDWARE_LEDS * COLOR_CHANNELS_COUNT, 0);

    while (true) {
        for (int activeLedIndex = 0; activeLedIndex < TOTAL_HARDWARE_LEDS; ++activeLedIndex) {
            std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);
            
            int baseVectorIndex = activeLedIndex * COLOR_CHANNELS_COUNT;
            outgoingNetworkPayloadBuffer[baseVectorIndex] = 255;     // Red
            outgoingNetworkPayloadBuffer[baseVectorIndex + 1] = 255; // Green
            outgoingNetworkPayloadBuffer[baseVectorIndex + 2] = 255; // Blue
            
            networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(CHASE_SLEEP_MILLISECONDS));
        }
    }
    return 0;
}