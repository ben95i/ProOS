#include "opcclient.h"
#include "ledlayout.h"
#include <chrono>
#include <thread>
#include <vector>
#include <iostream>
#include <algorithm>

constexpr const char* TARGET_SERVER_ADDRESS = "127.0.0.1";
constexpr int TARGET_SERVER_PORT = 7890;
constexpr int COLOR_CHANNELS_COUNT = 3;
constexpr int CHASE_SLEEP_MILLISECONDS = 35;

int main(int argc, char* argv[]) {
    // 1. Validate and Parse Layout Argument
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <LAYOUT>\n"
                  << "Example: " << argv[0] << " 16x16grid" << std::endl;
        return 1;
    }

    LEDLayout layout;
    try {
        layout.parseArgument(argv[1]);
    } catch (...) {
        std::cerr << "Error: Failed to parse layout argument: " << argv[1] << std::endl;
        return 1;
    }

    const int totalPixels = layout.pixels.size();
    if (totalPixels == 0) {
        std::cerr << "Error: Layout resulted in 0 LEDs." << std::endl;
        return 1;
    }

    // 2. Initialize Client and Buffer
    // We size the buffer exactly to the layout to avoid server-side lag
    OPCClient networkDisplayClient(TARGET_SERVER_ADDRESS, TARGET_SERVER_PORT);
    std::vector<uint8_t> outgoingNetworkPayloadBuffer(totalPixels * COLOR_CHANNELS_COUNT, 0);

    std::cout << "Starting chase on " << totalPixels << " LEDs..." << std::endl;

    // 3. Chase Logic
    while (true) {
        for (int activeLedIndex = 0; activeLedIndex < totalPixels; ++activeLedIndex) {
            // Clear previous frame
            std::fill(outgoingNetworkPayloadBuffer.begin(), outgoingNetworkPayloadBuffer.end(), 0);
            
            // Set the current pixel to White (R+G+B)
            int baseVectorIndex = activeLedIndex * COLOR_CHANNELS_COUNT;
            outgoingNetworkPayloadBuffer[baseVectorIndex]     = 255; // Red
            outgoingNetworkPayloadBuffer[baseVectorIndex + 1] = 255; // Green
            outgoingNetworkPayloadBuffer[baseVectorIndex + 2] = 255; // Blue
            
            // Send the exact payload size expected by the server
            if (!networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size())) {
                std::cerr << "Connection lost. Attempting to reconnect..." << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(CHASE_SLEEP_MILLISECONDS));
        }
    }

    return 0;
}