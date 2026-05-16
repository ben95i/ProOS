#include "ledlayout.h"
#include <iostream>
#include <cmath>
#include <cstdio>

void LedLayout::parseArgument(const std::string& layoutArgument) {
    std::string ringStandard = "93ring";
    std::string ringInverted = "93ringinv";
    std::string identifierStrips = "strips";
    std::string identifierGrid = "grid";

    bool isStandardRing = (layoutArgument == ringStandard);
    bool isInvertedRing = (layoutArgument == ringInverted);
    bool containsStrips = (layoutArgument.find(identifierStrips) != std::string::npos);
    bool containsGrid = (layoutArgument.find(identifierGrid) != std::string::npos);

    if (isStandardRing) {
        bool invertCoordinates = false;
        setup93Ring(invertCoordinates);
    } 
    else if (isInvertedRing) {
        bool invertCoordinates = true;
        setup93Ring(invertCoordinates);
    } 
    else if (containsStrips) {
        int defaultStripsCount = 8;
        int defaultLedsPerStrip = 60;
        int parsedStripsCount = defaultStripsCount;
        int parsedLedsPerStrip = defaultLedsPerStrip;
        
        sscanf(layoutArgument.c_str(), "%dx%dstrips", &parsedStripsCount, &parsedLedsPerStrip);
        setupStrips(parsedStripsCount, parsedLedsPerStrip);
    } 
    else {
        int defaultWidth = 8;
        int defaultHeight = 8;
        int parsedWidth = defaultWidth;
        int parsedHeight = defaultHeight;

        if (containsGrid) {
            sscanf(layoutArgument.c_str(), "%dx%dgrid", &parsedWidth, &parsedHeight);
        } else {
            sscanf(layoutArgument.c_str(), "%dx%d", &parsedWidth, &parsedHeight);
        }
        setupGrid(parsedWidth, parsedHeight);
    }
}

void LedLayout::setupGrid(int totalColumns, int totalRows) {
    std::lock_guard<std::mutex> lock(layoutMutex);
    
    pixels.clear();
    logicalWidth = totalColumns;
    logicalHeight = totalRows;
    
    float floatColumns = static_cast<float>(totalColumns);
    float floatRows = static_cast<float>(totalRows);
    float calculatedAspectRatio = floatColumns / floatRows;
    targetAspect = calculatedAspectRatio;

    for (int currentRowIndex = 0; currentRowIndex < totalRows; currentRowIndex++) {
        for (int currentColumnIndex = 0; currentColumnIndex < totalColumns; currentColumnIndex++) {
            LedPixel currentPixel;
            
            int rowOffset = currentRowIndex * totalColumns;
            int finalOpcIndex = rowOffset + currentColumnIndex;
            currentPixel.opcIndex = finalOpcIndex;
            
            if (totalColumns > 1) {
                float currentColumnFloat = static_cast<float>(currentColumnIndex);
                int maxColumnIndex = totalColumns - 1;
                float maxColumnFloat = static_cast<float>(maxColumnIndex);
                float normalizedX = currentColumnFloat / maxColumnFloat;
                currentPixel.normX = normalizedX;
            } else {
                currentPixel.normX = CENTER_COORDINATE;
            }
            
            if (totalRows > 1) {
                float currentRowFloat = static_cast<float>(currentRowIndex);
                int maxRowIndex = totalRows - 1;
                float maxRowFloat = static_cast<float>(maxRowIndex);
                float normalizedY = currentRowFloat / maxRowFloat;
                currentPixel.normY = normalizedY;
            } else {
                currentPixel.normY = CENTER_COORDINATE;
            }
            
            pixels.push_back(currentPixel);
        }
    }
    std::cout << "Initialized " << totalColumns << " columns and " << totalRows << " rows of LEDs\n";
}

void LedLayout::setupStrips(int totalStrips, int ledsPerStrip) {
    std::lock_guard<std::mutex> lock(layoutMutex);
    
    pixels.clear();
    logicalWidth = ledsPerStrip;
    logicalHeight = totalStrips;
    
    float floatLedsPerStrip = static_cast<float>(ledsPerStrip);
    float floatTotalStrips = static_cast<float>(totalStrips);
    float calculatedAspectRatio = floatLedsPerStrip / floatTotalStrips;
    targetAspect = calculatedAspectRatio;

    for (int currentStripIndex = 0; currentStripIndex < totalStrips; currentStripIndex++) {
        for (int currentLedIndex = 0; currentLedIndex < ledsPerStrip; currentLedIndex++) {
            LedPixel currentPixel;
            
            int stripOffset = currentStripIndex * ledsPerStrip;
            int finalOpcIndex = stripOffset + currentLedIndex;
            currentPixel.opcIndex = finalOpcIndex;
            
            if (ledsPerStrip > 1) {
                float currentLedFloat = static_cast<float>(currentLedIndex);
                int maxLedIndex = ledsPerStrip - 1;
                float maxLedFloat = static_cast<float>(maxLedIndex);
                float normalizedX = currentLedFloat / maxLedFloat;
                currentPixel.normX = normalizedX;
            } else {
                currentPixel.normX = CENTER_COORDINATE;
            }
            
            if (totalStrips > 1) {
                float currentStripFloat = static_cast<float>(currentStripIndex);
                int maxStripIndex = totalStrips - 1;
                float maxStripFloat = static_cast<float>(maxStripIndex);
                float normalizedY = currentStripFloat / maxStripFloat;
                currentPixel.normY = normalizedY;
            } else {
                currentPixel.normY = CENTER_COORDINATE;
            }
            
            pixels.push_back(currentPixel);
        }
    }
    std::cout << "Initialized " << totalStrips << " strips of " << ledsPerStrip << " LEDs\n";
}

void LedLayout::setup93Ring(bool shouldInvertX) {
    std::lock_guard<std::mutex> lock(layoutMutex);
    
    pixels.clear();
    
    int defaultVirtualWidth = 16;
    int defaultVirtualHeight = 16;
    logicalWidth = defaultVirtualWidth;
    logicalHeight = defaultVirtualHeight;
    targetAspect = DEFAULT_ASPECT_RATIO;
    
    struct RingDefinition { 
        int startingIndex; 
        int ledCount; 
    };
    
    std::vector<RingDefinition> concentricRings = {
        {0, 1}, 
        {8, 8}, 
        {20, 12}, 
        {36, 16}, 
        {60, 24}, 
        {92, 32}
    };
    
    int totalRings = static_cast<int>(concentricRings.size());

    for (int currentRingIndex = 0; currentRingIndex < totalRings; currentRingIndex++) {
        
        float calculatedRadius = 0.0f;
        if (totalRings > 1) {
            float currentRingFloat = static_cast<float>(currentRingIndex);
            int maxRingIndex = totalRings - 1;
            float maxRingFloat = static_cast<float>(maxRingIndex);
            float ringRatio = currentRingFloat / maxRingFloat;
            calculatedRadius = ringRatio * MAX_RING_RADIUS;
        }

        RingDefinition currentRingDefinition = concentricRings[currentRingIndex];
        int currentRingLedCount = currentRingDefinition.ledCount;
        int currentRingStartIndex = currentRingDefinition.startingIndex;
        float floatRingLedCount = static_cast<float>(currentRingLedCount);

        for (int currentLedIndex = 0; currentLedIndex < currentRingLedCount; currentLedIndex++) {
            
            float floatLedIndex = static_cast<float>(currentLedIndex);
            float angleRatio = floatLedIndex / floatRingLedCount;
            float currentAngleRadians = angleRatio * FULL_CIRCLE_RADIANS;
            
            LedPixel currentPixel;
            
            int finalOpcIndex = currentRingStartIndex + currentLedIndex;
            currentPixel.opcIndex = finalOpcIndex;
            
            // --- X Coordinate Calculation ---
            float cosineValue = std::cos(currentAngleRadians);
            float xOffset = calculatedRadius * cosineValue;
            float baseNormalizedX = CENTER_COORDINATE + xOffset;
            
            float finalNormalizedX = baseNormalizedX;
            if (shouldInvertX) {
                finalNormalizedX = INVERT_MAX_VALUE - baseNormalizedX;
            }
            currentPixel.normX = finalNormalizedX;
            
            // --- Y Coordinate Calculation ---
            float sineValue = std::sin(currentAngleRadians);
            float yOffset = calculatedRadius * sineValue;
            float finalNormalizedY = CENTER_COORDINATE + yOffset;
            
            currentPixel.normY = finalNormalizedY;
            
            pixels.push_back(currentPixel);
        }
    }
    
    if (shouldInvertX) {
        std::cout << "Initialized an inverted ring of 93 LEDs for projection\n";
    } else {
        std::cout << "Initialized a standard ring of 93 LEDs\n";
    }
}

// --- THREAD-SAFE GETTERS IMPLEMENTATION ---

int LedLayout::getLogicalWidth() const {
    std::lock_guard<std::mutex> lock(layoutMutex);
    return logicalWidth;
}

int LedLayout::getLogicalHeight() const {
    std::lock_guard<std::mutex> lock(layoutMutex);
    return logicalHeight;
}

float LedLayout::getTargetAspect() const {
    std::lock_guard<std::mutex> lock(layoutMutex);
    return targetAspect;
}

std::vector<LedPixel> LedLayout::getPixels() const {
    std::lock_guard<std::mutex> lock(layoutMutex);
    return pixels;
}