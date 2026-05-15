#include "opcclient.h"
#include "ledlayout.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <thread>
#include <chrono>

// Configuration
constexpr const char* TARGET_OPC_HOST = "127.0.0.1";
constexpr int TARGET_OPC_PORT = 7890;

struct RGBColor {
    uint8_t r, g, b;
};

// Converts hex string (e.g., "FF0000") to RGB components
RGBColor parseHexColorCode(const std::string& hexString) {
    if (hexString.length() != 6) {
        throw std::invalid_argument("Hex color must be 6 characters (e.g., 0000FF)");
    }
    uint32_t parsedValue = std::stoul(hexString, nullptr, 16);
    return {
        static_cast<uint8_t>((parsedValue >> 16) & 0xFF),
        static_cast<uint8_t>((parsedValue >> 8) & 0xFF),
        static_cast<uint8_t>(parsedValue & 0xFF)
    };
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <LAYOUT> <HEX_COLOR>\n"
                  << "Example: " << argv[0] << " 93ring FF0000" << std::endl;
        return 1;
    }

    // 1. Calculate pixel count from the layout argument
    LEDLayout layout;
    try {
        layout.parseArgument(argv[1]);
    } catch (...) {
        std::cerr << "Error: Failed to parse layout: " << argv[1] << std::endl;
        return 1;
    }

    if (layout.pixels.empty()) {
        std::cerr << "Error: Layout resulted in 0 LEDs." << std::endl;
        return 1;
    }

    // 2. Parse the target color
    RGBColor targetColor;
    try {
        targetColor = parseHexColorCode(argv[2]);
    } catch (...) {
        std::cerr << "Error: Invalid hex color format." << std::endl;
        return 1;
    }

    // 3. Find the MAXIMUM opc_index to size our buffer correctly
    int maxOpcIndex = 0;
    for (const auto& pixel : layout.pixels) {
        if (pixel.opc_index > maxOpcIndex) {
            maxOpcIndex = pixel.opc_index;
        }
    }

    // The buffer size must cover up to the max index (index 0 to maxOpcIndex)
    const int totalPayloadPixels = maxOpcIndex + 1;

    // 4. Prepare the data payload (initialize all to 0 / black for the "gaps")
    std::vector<uint8_t> pixelPayloadData(totalPayloadPixels * 3, 0);

    // Fill the buffer by mapping the color specifically to the opc_index
    for (const auto& pixel : layout.pixels) {
        int bufferIdx = pixel.opc_index * 3;
        pixelPayloadData[bufferIdx]     = targetColor.r;
        pixelPayloadData[bufferIdx + 1] = targetColor.g;
        pixelPayloadData[bufferIdx + 2] = targetColor.b;
    }

    // 5. Connect once and send frames
    OPCClient displayClient(TARGET_OPC_HOST, TARGET_OPC_PORT);
    
    std::cout << "Sending " << argv[2] << " mapped to " << layout.pixels.size() 
              << " LEDs (Max index: " << maxOpcIndex << ")..." << std::endl;

    for (int frame = 0; frame < 10; ++frame) {
        if (!displayClient.sendPixelPayload(0, pixelPayloadData.data(), pixelPayloadData.size())) {
            std::cerr << "Error: Failed to transmit frame " << frame << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    std::cout << "Done." << std::endl;
    return 0;
}