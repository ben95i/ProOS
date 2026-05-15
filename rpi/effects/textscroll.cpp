#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>
#include <cctype>

#include "ledlayout.h"
#include "opcclient.h"
#include "fontlibrary.h"

// -------------------------------------------------------------------------
// CONSTANTS
// -------------------------------------------------------------------------
const int MAX_MESSAGE_BYTES = 2048;
const int TARGET_FRAMES_PER_SECOND = 60;
const int BITS_PER_BYTE = 8;
const uint8_t OPC_DEFAULT_CHANNEL = 0;

// -------------------------------------------------------------------------
// DATA STRUCTURES
// -------------------------------------------------------------------------

/**
 * Represents a standard RGB color using 8-bit channels.
 */
struct ColorRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    ColorRgb() {
        r = 0;
        g = 0;
        b = 0;
    }
};

// -------------------------------------------------------------------------
// UTILITY CLASSES
// -------------------------------------------------------------------------

class ColorProcessor {
public:
    static ColorRgb fromHex(std::string hexString) {
        ColorRgb resultColor;
        
        bool isEmptyString = hexString.empty();
        if (isEmptyString == false) {
            bool hasHashPrefix = (hexString.at(0) == '#');
            if (hasHashPrefix == true) {
                hexString = hexString.substr(1);
            }
        }

        bool isCorrectLength = (hexString.length() == 6);
        if (isCorrectLength == true) {
            try {
                std::string redString = hexString.substr(0, 2);
                std::string greenString = hexString.substr(2, 2);
                std::string blueString = hexString.substr(4, 2);

                int redInt = std::stoi(redString, nullptr, 16);
                int greenInt = std::stoi(greenString, nullptr, 16);
                int blueInt = std::stoi(blueString, nullptr, 16);

                resultColor.r = static_cast<uint8_t>(redInt);
                resultColor.g = static_cast<uint8_t>(greenInt);
                resultColor.b = static_cast<uint8_t>(blueInt);
            } catch (...) {
                std::cerr << "Warning: Invalid hex format. Defaulting to white.\n";
                resultColor.r = 255; 
                resultColor.g = 255; 
                resultColor.b = 255;
            }
        }
        return resultColor;
    }
};

class FontManager {
private:
    int fontWidth;
    int fontHeight;
    int fontTypeIdentifier;

public:
    FontManager(std::string choiceString) {
        if (choiceString == "3x5") {
            fontTypeIdentifier = 1;
            fontWidth = 3;
            fontHeight = 5;
        } else if (choiceString == "8x8") {
            fontTypeIdentifier = 3;
            fontWidth = 8;
            fontHeight = 8;
        } else {
            // Defaulting fallback
            fontTypeIdentifier = 2;
            fontWidth = 5;
            fontHeight = 7;
        }
    }

    int getWidth() const { 
        return fontWidth; 
    }
    
    int getHeight() const { 
        return fontHeight; 
    }

    bool isPixelActive(char character, int columnPosition, int rowPosition) const {
        int asciiValue = static_cast<int>(static_cast<unsigned char>(character));
        int arrayIndex = asciiValue - 32;

        bool isBelowPrintableRange = (arrayIndex < 0);
        if (isBelowPrintableRange == true) {
            return false;
        }

        uint8_t bitmaskColumn = 0;

        if (fontTypeIdentifier == 1) { // 3x5 font processing
            bool isLowercaseCharacter = (asciiValue >= 97) && (asciiValue <= 122);
            if (isLowercaseCharacter == true) {
                int uppercaseAsciiValue = asciiValue - 32;
                arrayIndex = uppercaseAsciiValue - 32;
            }
            
            bool isOutsideFontBounds = (arrayIndex > 63);
            if (isOutsideFontBounds == true) {
                return false;
            }
            
            bool isOutsideColumnBounds = (columnPosition >= 3);
            if (isOutsideColumnBounds == true) {
                return false;
            }
            
            bitmaskColumn = FontLibrary::DATA_3X5[arrayIndex][columnPosition];
            
        } else if (fontTypeIdentifier == 3) { // 8x8 font processing
            bool isOutsideFontBounds = (arrayIndex > 94);
            if (isOutsideFontBounds == true) {
                return false;
            }
            
            bool isOutsideColumnBounds = (columnPosition >= 8);
            if (isOutsideColumnBounds == true) {
                return false;
            }
            
            bitmaskColumn = FontLibrary::DATA_8X8[arrayIndex][columnPosition];
            
        } else { // 5x7 font processing
            bool isOutsideFontBounds = (arrayIndex > 94);
            if (isOutsideFontBounds == true) {
                return false;
            }
            
            bool isOutsideColumnBounds = (columnPosition >= 5);
            if (isOutsideColumnBounds == true) {
                return false;
            }
            
            bitmaskColumn = FontLibrary::DATA_5X7[arrayIndex][columnPosition];
        }

        int bitShiftAmount = rowPosition;
        int isolatedPixelBit = (bitmaskColumn >> bitShiftAmount) & 0x01;
        
        bool isPixelIlluminated = false;
        if (isolatedPixelBit == 1) {
            isPixelIlluminated = true;
        }
        
        return isPixelIlluminated;
    }
};

// -------------------------------------------------------------------------
// MAIN APPLICATION
// -------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // 1. Argument Validation
    if (argc < 6) {
        std::cout << "Usage: " << argv[0] << " <type> <message> <hex> <font_id> <speed>\n";
        std::cout << "Fonts Choices: 3x5, 5x7, 8x8\n";
        return 1;
    }

    const std::string physicalLayoutType = argv[1];
    std::string textMessage = argv[2];
    const std::string hexColorString = argv[3];
    const std::string fontChoiceString = argv[4];
    const float scrollVelocityPixelsPerSecond = std::stof(argv[5]);

    bool isMessageTooLong = (textMessage.length() > MAX_MESSAGE_BYTES);
    if (isMessageTooLong == true) {
        textMessage = textMessage.substr(0, MAX_MESSAGE_BYTES);
    }

    // 2. Resource Initialization
    LEDLayout hardwareLayout;
    hardwareLayout.parseArgument(physicalLayoutType);

    OPCClient networkClient("127.0.0.1", 7890);
    FontManager textFont(fontChoiceString);
    ColorRgb messageTextColor = ColorProcessor::fromHex(hexColorString);

    // 3. Coordinate Scaling Strategy
    float layoutLogicalHeight = static_cast<float>(hardwareLayout.logicalHeight);
    float fontLogicalHeight = static_cast<float>(textFont.getHeight());
    float canvasVerticalScaleFactor = layoutLogicalHeight / fontLogicalHeight;

    int characterLetterSpacing = 1;
    int singleCharacterStrideWidth = textFont.getWidth() + characterLetterSpacing;
    
    int messageLengthInt = static_cast<int>(textMessage.length());
    float totalTextCanvasWidth = static_cast<float>(messageLengthInt * singleCharacterStrideWidth);

    // 4. Time Tracking Initialization
    float horizontalScrollPosition = 0.0f;
    auto previousFrameTime = std::chrono::high_resolution_clock::now();
    int sleepMillisecondsTarget = 1000 / TARGET_FRAMES_PER_SECOND;
    std::chrono::milliseconds frameTargetDuration(sleepMillisecondsTarget);

    // Lambda to sample the virtual canvas state for anti-aliasing interpolation
    auto sampleVirtualTextCanvas = [&](float canvasXCoordinate, float canvasYCoordinate) -> float {
        float sampleIntensity = 0.0f;
        
        bool isOutsideVerticalBounds = (canvasYCoordinate < 0.0f) || (canvasYCoordinate >= fontLogicalHeight);
        if (isOutsideVerticalBounds == true) {
            return sampleIntensity;
        }

        float loopingWrappedX = std::fmod(canvasXCoordinate, totalTextCanvasWidth);
        bool isNegativeXWrapped = (loopingWrappedX < 0.0f);
        if (isNegativeXWrapped == true) {
            loopingWrappedX = loopingWrappedX + totalTextCanvasWidth;
        }

        int floorPixelX = static_cast<int>(std::floor(loopingWrappedX));
        int floorPixelY = static_cast<int>(std::floor(canvasYCoordinate));

        int characterStringIndex = floorPixelX / singleCharacterStrideWidth;
        int characterInternalColumn = floorPixelX % singleCharacterStrideWidth;

        bool isBeyondStringEnd = (characterStringIndex >= messageLengthInt);
        if (isBeyondStringEnd == true) {
            return sampleIntensity;
        }

        bool isInsideLetterSpacingGap = (characterInternalColumn >= textFont.getWidth());
        if (isInsideLetterSpacingGap == true) {
            return sampleIntensity;
        }

        char targetLetter = textMessage.at(static_cast<size_t>(characterStringIndex));
        bool isActiveInFont = textFont.isPixelActive(targetLetter, characterInternalColumn, floorPixelY);
        
        if (isActiveInFont == true) {
            sampleIntensity = 1.0f;
        }

        return sampleIntensity;
    };

    // 5. Main Render Loop
    while (true) {
        
        // Calculate true delta time for perfectly smooth sub-pixel scroll increments
        auto currentFrameTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<float> elapsedTimeDuration = currentFrameTime - previousFrameTime;
        previousFrameTime = currentFrameTime;
        
        float deltaTimeSecondsFloat = elapsedTimeDuration.count();
        float positionDelta = scrollVelocityPixelsPerSecond * deltaTimeSecondsFloat;
        horizontalScrollPosition = horizontalScrollPosition + positionDelta;

        size_t layoutTotalPixels = hardwareLayout.pixels.size();
        std::vector<uint8_t> frameNetworkBuffer(layoutTotalPixels * 3, 0);

        for (size_t pixelIndex = 0; pixelIndex < layoutTotalPixels; pixelIndex = pixelIndex + 1) {
            LEDPixel& activePixel = hardwareLayout.pixels[pixelIndex];

            float layoutWorldX = activePixel.norm_x * static_cast<float>(hardwareLayout.logicalWidth);
            float layoutWorldY = activePixel.norm_y * static_cast<float>(hardwareLayout.logicalHeight);

            float virtualCanvasX = layoutWorldX + horizontalScrollPosition;
            float virtualCanvasY = layoutWorldY / canvasVerticalScaleFactor;

            // Bilinear Interpolation for smooth, anti-aliased sub-pixel rendering
            float leftSampleX = std::floor(virtualCanvasX);
            float rightSampleX = leftSampleX + 1.0f;
            float topSampleY = std::floor(virtualCanvasY);
            float bottomSampleY = topSampleY + 1.0f;

            float topLeftSampleIntensity = sampleVirtualTextCanvas(leftSampleX, topSampleY);
            float topRightSampleIntensity = sampleVirtualTextCanvas(rightSampleX, topSampleY);
            float bottomLeftSampleIntensity = sampleVirtualTextCanvas(leftSampleX, bottomSampleY);
            float bottomRightSampleIntensity = sampleVirtualTextCanvas(rightSampleX, bottomSampleY);

            float horizontalFractionalDifference = virtualCanvasX - leftSampleX;
            float verticalFractionalDifference = virtualCanvasY - topSampleY;

            float inverseHorizontalFraction = 1.0f - horizontalFractionalDifference;
            float inverseVerticalFraction = 1.0f - verticalFractionalDifference;

            float topRowBlendedIntensity = (topLeftSampleIntensity * inverseHorizontalFraction) + (topRightSampleIntensity * horizontalFractionalDifference);
            float bottomRowBlendedIntensity = (bottomLeftSampleIntensity * inverseHorizontalFraction) + (bottomRightSampleIntensity * horizontalFractionalDifference);

            float finalAntiAliasedIntensity = (topRowBlendedIntensity * inverseVerticalFraction) + (bottomRowBlendedIntensity * verticalFractionalDifference);

            bool isPixelVisible = (finalAntiAliasedIntensity > 0.0f);
            if (isPixelVisible == true) {
                size_t bufferMemoryIndex = static_cast<size_t>(activePixel.opc_index) * 3;
                
                bool isSafeMemoryWrite = ((bufferMemoryIndex + 2) < frameNetworkBuffer.size());
                if (isSafeMemoryWrite == true) {
                    
                    float finalRedFloat = static_cast<float>(messageTextColor.r) * finalAntiAliasedIntensity;
                    float finalGreenFloat = static_cast<float>(messageTextColor.g) * finalAntiAliasedIntensity;
                    float finalBlueFloat = static_cast<float>(messageTextColor.b) * finalAntiAliasedIntensity;

                    frameNetworkBuffer[bufferMemoryIndex] = static_cast<uint8_t>(std::round(finalRedFloat));
                    frameNetworkBuffer[bufferMemoryIndex + 1] = static_cast<uint8_t>(std::round(finalGreenFloat));
                    frameNetworkBuffer[bufferMemoryIndex + 2] = static_cast<uint8_t>(std::round(finalBlueFloat));
                }
            }
        }

        // Dispatch anti-aliased frame to Open Pixel Control server
        const uint8_t* payloadPointer = frameNetworkBuffer.data();
        size_t payloadByteSize = frameNetworkBuffer.size();
        networkClient.sendPixelPayload(OPC_DEFAULT_CHANNEL, payloadPointer, payloadByteSize);

        // Seamless message looping logic
        float wrappingBoundaryPoint = totalTextCanvasWidth + static_cast<float>(hardwareLayout.logicalWidth);
        bool hasPassedBoundary = (horizontalScrollPosition > wrappingBoundaryPoint);
        if (hasPassedBoundary == true) {
            horizontalScrollPosition = 0.0f - static_cast<float>(hardwareLayout.logicalWidth);
        }

        std::this_thread::sleep_for(frameTargetDuration);
    }

    return 0;
}