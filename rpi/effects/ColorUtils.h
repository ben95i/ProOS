#ifndef COLOR_UTILS_H
#define COLOR_UTILS_H

#include <cmath>
#include <algorithm>
#include <cstdint>

namespace ColorUtils {
    inline float clampValue(float inputValue, float minimumBound, float maximumBound) {
        return std::max(minimumBound, std::min(maximumBound, inputValue));
    }

    inline float remapValue(float inputValue, float originalMin, float originalMax, float targetMin, float targetMax) {
        return targetMin + (inputValue - originalMin) * (targetMax - targetMin) / (originalMax - originalMin);
    }

    inline float waveCosine(float inputCoordinate, float phaseOffset, float wavePeriod, float minimumOutput, float maximumOutput) {
        float normalizedWave = (std::cos((inputCoordinate / wavePeriod - phaseOffset) * M_PI * 2.0f) / 2.0f) + 0.5f;
        return normalizedWave * (maximumOutput - minimumOutput) + minimumOutput;
    }

    inline void applyContrast(float& redChannel, float& greenChannel, float& blueChannel, float contrastCenter, float contrastMultiplier) {
        redChannel = (redChannel - contrastCenter) * contrastMultiplier + contrastCenter;
        greenChannel = (greenChannel - contrastCenter) * contrastMultiplier + contrastCenter;
        blueChannel = (blueChannel - contrastCenter) * contrastMultiplier + contrastCenter;
    }

    inline float modularDistance(float positionA, float positionB, float modulusWrap) {
        float absoluteDifference = std::fmod(std::abs(positionA - positionB), modulusWrap);
        return std::min(absoluteDifference, modulusWrap - absoluteDifference);
    }

    // Safely scales 0.0-1.0 floats to 0-255 bytes (fixing the Python wrap-around bug at 1.0 * 256)
    inline uint8_t floatToByte(float colorFloat) {
        return static_cast<uint8_t>(clampValue(colorFloat * 255.0f, 0.0f, 255.0f));
    }
}

#endif