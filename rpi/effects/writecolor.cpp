#include "OPCClient.h"
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>

// Configuration
constexpr int TOTAL_PIXELS_COUNT = 1024;
constexpr int PAYLOAD_BYTE_SIZE = TOTAL_PIXELS_COUNT * 3;
constexpr const char* TARGET_OPC_HOST = "127.0.0.1";
constexpr int TARGET_OPC_PORT = 7890;

struct RGBColor {
    uint8_t redIntensity, greenIntensity, blueIntensity;
};

// Converts hex string (0000FF) to RGB components
RGBColor parseHexColorCode(const std::string& hexString) {
    if (hexString.length() != 6) {
        throw std::invalid_argument("Hex color must be exactly 6 characters (e.g., 0000FF)");
    }
    uint32_t parsedValue = std::stoul(hexString, nullptr, 16);
    return {
        static_cast<uint8_t>((parsedValue >> 16) & 0xFF),
        static_cast<uint8_t>((parsedValue >> 8) & 0xFF),
        static_cast<uint8_t>(parsedValue & 0xFF)
    };
}

int main(int argumentCount, char* argumentValues[]) {
    if (argumentCount != 2) {
        std::cerr << "Usage: " << argumentValues[0] << " <HEX_COLOR>\nExample: " << argumentValues[0] << " 0000FF" << std::endl;
        return 1;
    }

    RGBColor targetColor;
    try {
        targetColor = parseHexColorCode(argumentValues[1]);
    } catch (...) {
        std::cerr << "Error: Invalid hex color format." << std::endl;
        return 1;
    }

    std::vector<uint8_t> pixelPayloadData;
    pixelPayloadData.reserve(PAYLOAD_BYTE_SIZE);

    for (int pixelIndex = 0; pixelIndex < TOTAL_PIXELS_COUNT; ++pixelIndex) {
        pixelPayloadData.push_back(targetColor.redIntensity);
        pixelPayloadData.push_back(targetColor.greenIntensity);
        pixelPayloadData.push_back(targetColor.blueIntensity);
    }

    OPCClient displayClient(TARGET_OPC_HOST, TARGET_OPC_PORT);
    if (!displayClient.sendPixelPayload(0, pixelPayloadData.data(), pixelPayloadData.size())) {
        std::cerr << "Warning: Failed to transmit initial packet to OPC server." << std::endl;
        return 1;
    }

    return 0;
}