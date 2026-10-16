#include "opcclient.h"
#include "ledlayout.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cctype>
#include <cstdint>

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
    // std::stoul alone accepts a sign, a "0x" prefix and trailing junk, so
    // "12zzzz" was taken as a colour. Every character has to be a hex digit.
    for (char hexCharacter : hexString) {
        if (std::isxdigit(static_cast<unsigned char>(hexCharacter)) == 0) {
            throw std::invalid_argument("Hex color must only hold 0-9 and A-F");
        }
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
    LedLayout layout;
    try {
        layout.parseArgument(argv[1]);
    } catch (...) {
        std::cerr << "Error: Failed to parse layout: " << argv[1] << std::endl;
        return 1;
    }

    if (layout.getPixels().empty()) {
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
    for (const auto& pixel : layout.getPixels()) {
        if (pixel.opcIndex > maxOpcIndex) {
            maxOpcIndex = pixel.opcIndex;
        }
    }

    // The buffer size must cover up to the max index (index 0 to maxOpcIndex)
    const int totalPayloadPixels = maxOpcIndex + 1;

    // 4. Prepare the data payload (initialize all to 0 / black for the "gaps")
    std::vector<uint8_t> pixelPayloadData(totalPayloadPixels * 3, 0);

    // Fill the buffer by mapping the color specifically to the opc_index
    for (const auto& pixel : layout.getPixels()) {
        int bufferIdx = pixel.opcIndex * 3;
        pixelPayloadData[bufferIdx]     = targetColor.r;
        pixelPayloadData[bufferIdx + 1] = targetColor.g;
        pixelPayloadData[bufferIdx + 2] = targetColor.b;
    }

    // 5. Connect once and send frames
    OPCClient displayClient(TARGET_OPC_HOST, TARGET_OPC_PORT);
    
    std::cout << "Sending " << argv[2] << " mapped to " << layout.getPixels().size() 
              << " LEDs (Max index: " << maxOpcIndex << ")..." << std::endl;

    // The client drops a frame while the server is busy and waits half a second
    // before it retries a failed connection, so one failed send is not the end:
    // keep trying until the colour has gone out ten times or the time is up.
    const int requiredFrameCount = 10;
    const int maximumSendAttempts = 100;
    int transmittedFrameCount = 0;

    for (int attempt = 0; attempt < maximumSendAttempts && transmittedFrameCount < requiredFrameCount; ++attempt) {
        if (displayClient.sendPixelPayload(0, pixelPayloadData.data(), pixelPayloadData.size())) {
            transmittedFrameCount += 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (transmittedFrameCount == 0) {
        std::cerr << "Error: Could not reach the OPC server, color not sent." << std::endl;
        return 1;
    }

    std::cout << "Done." << std::endl;
    return 0;
}