#include "LEDLayout.h"
#include "OPCClient.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>

constexpr const char* OPC_SERVER_ADDRESS = "127.0.0.1";
constexpr int OPC_SERVER_PORT = 7890;
constexpr int TARGET_ANIMATION_FPS = 60;

// Simulation Constants
constexpr float GLOBAL_NOISE_SCALE = 0.005f;
constexpr float GLOBAL_MOVEMENT_SPEED = 0.002f;
constexpr float Z_AXIS_SPEED_SCALE = 0.1f;
constexpr float HSB_MAXIMUM_VALUE = 100.0f;
constexpr int FRACTAL_NOISE_OCTAVES = 4;

struct PixelColorState { 
    uint8_t redIntensity, greenIntensity, blueIntensity; 
};

class PerlinNoiseGenerator {
    int spatialPermutationTable[512];

    inline double calculateFadeCurve(double timeValueInput) { 
        return timeValueInput * timeValueInput * timeValueInput * (timeValueInput * (timeValueInput * 6 - 15) + 10); 
    }
    
    inline double performLinearInterpolate(double blendFactorValue, double boundValueA, double boundValueB) { 
        return boundValueA + blendFactorValue * (boundValueB - boundValueA); 
    }
    
    inline double calculateGradientVector(int hashIdentifier, double coordinateX, double coordinateY, double coordinateZ) {
        int maskedHashValue = hashIdentifier & 15;
        double componentVectorU = maskedHashValue < 8 ? coordinateX : coordinateY;
        double componentVectorV = maskedHashValue < 4 ? coordinateY : (maskedHashValue == 12 || maskedHashValue == 14 ? coordinateX : coordinateZ);
        return ((maskedHashValue & 1) == 0 ? componentVectorU : -componentVectorU) + ((maskedHashValue & 2) == 0 ? componentVectorV : -componentVectorV);
    }

public:
    PerlinNoiseGenerator() {
        int standardPermutationsList[] = { 151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180 };
        for (int populationIndex = 0; populationIndex < 256; populationIndex++) {
            spatialPermutationTable[256 + populationIndex] = spatialPermutationTable[populationIndex] = standardPermutationsList[populationIndex];
        }
    }

    double generateNoise(double targetCoordX, double targetCoordY, double targetCoordZ) {
        int integerUnitX = (int)floor(targetCoordX) & 255;
        int integerUnitY = (int)floor(targetCoordY) & 255;
        int integerUnitZ = (int)floor(targetCoordZ) & 255;
        targetCoordX -= floor(targetCoordX); targetCoordY -= floor(targetCoordY); targetCoordZ -= floor(targetCoordZ);
        double fadeCurveX = calculateFadeCurve(targetCoordX), fadeCurveY = calculateFadeCurve(targetCoordY), fadeCurveZ = calculateFadeCurve(targetCoordZ);
        
        int hashValA = spatialPermutationTable[integerUnitX] + integerUnitY;
        int hashValAA = spatialPermutationTable[hashValA] + integerUnitZ;
        int hashValAB = spatialPermutationTable[hashValA + 1] + integerUnitZ;
        int hashValB = spatialPermutationTable[integerUnitX + 1] + integerUnitY;
        int hashValBA = spatialPermutationTable[hashValB] + integerUnitZ;
        int hashValBB = spatialPermutationTable[hashValB + 1] + integerUnitZ;
        
        return performLinearInterpolate(fadeCurveZ, 
            performLinearInterpolate(fadeCurveY, performLinearInterpolate(fadeCurveX, calculateGradientVector(spatialPermutationTable[hashValAA], targetCoordX, targetCoordY, targetCoordZ), calculateGradientVector(spatialPermutationTable[hashValBA], targetCoordX - 1, targetCoordY, targetCoordZ)), 
                                     performLinearInterpolate(fadeCurveX, calculateGradientVector(spatialPermutationTable[hashValAB], targetCoordX, targetCoordY - 1, targetCoordZ), calculateGradientVector(spatialPermutationTable[hashValBB], targetCoordX - 1, targetCoordY - 1, targetCoordZ))), 
            performLinearInterpolate(fadeCurveY, performLinearInterpolate(fadeCurveX, calculateGradientVector(spatialPermutationTable[hashValAA + 1], targetCoordX, targetCoordY, targetCoordZ - 1), calculateGradientVector(spatialPermutationTable[hashValBA + 1], targetCoordX - 1, targetCoordY, targetCoordZ - 1)), 
                                     performLinearInterpolate(fadeCurveX, calculateGradientVector(spatialPermutationTable[hashValAB + 1], targetCoordX, targetCoordY - 1, targetCoordZ - 1), calculateGradientVector(spatialPermutationTable[hashValBB + 1], targetCoordX - 1, targetCoordY - 1, targetCoordZ - 1)))
        );
    }
};

PerlinNoiseGenerator sharedNoiseEngine;

float calculateFractalNoiseLevels(float positionCoordinateX, float positionCoordinateY, float positionCoordinateZ) {
    float cumulativeCalculationResult = 0.0f;
    float currentAmplitudeStrength = 1.0f;
    for (int octaveLevel = 0; octaveLevel < FRACTAL_NOISE_OCTAVES; octaveLevel++) {
        cumulativeCalculationResult += (sharedNoiseEngine.generateNoise(positionCoordinateX, positionCoordinateY, positionCoordinateZ) + 1.0f) * 0.5f * currentAmplitudeStrength;
        currentAmplitudeStrength *= 0.5f; positionCoordinateX *= 2.0f; positionCoordinateY *= 2.0f; positionCoordinateZ *= 2.0f;
    }
    return cumulativeCalculationResult;
}

PixelColorState calculateHsbToRgb(float inputHueAngle, float inputSaturationPercentage, float inputBrightnessPercentage) {
    inputHueAngle = fmod(inputHueAngle, HSB_MAXIMUM_VALUE) / HSB_MAXIMUM_VALUE;
    inputSaturationPercentage = std::clamp(inputSaturationPercentage / HSB_MAXIMUM_VALUE, 0.0f, 1.0f);
    inputBrightnessPercentage = std::clamp(inputBrightnessPercentage / HSB_MAXIMUM_VALUE, 0.0f, 1.0f);

    if (inputSaturationPercentage == 0) {
        uint8_t grayscaleCalculatedValue = (uint8_t)(inputBrightnessPercentage * 255);
        return { grayscaleCalculatedValue, grayscaleCalculatedValue, grayscaleCalculatedValue };
    }

    float colorSectorPosition = inputHueAngle * 6.0f;
    int sectorBaseIndex = (int)floor(colorSectorPosition);
    float factorialFractionalPart = colorSectorPosition - sectorBaseIndex;
    
    float intermediateValueP = inputBrightnessPercentage * (1.0f - inputSaturationPercentage);
    float intermediateValueQ = inputBrightnessPercentage * (1.0f - inputSaturationPercentage * factorialFractionalPart);
    float intermediateValueT = inputBrightnessPercentage * (1.0f - inputSaturationPercentage * (1.0f - factorialFractionalPart));

    float finalRedFloat, finalGreenFloat, finalBlueFloat;
    switch (sectorBaseIndex % 6) {
        case 0: finalRedFloat = inputBrightnessPercentage; finalGreenFloat = intermediateValueT; finalBlueFloat = intermediateValueP; break;
        case 1: finalRedFloat = intermediateValueQ; finalGreenFloat = inputBrightnessPercentage; finalBlueFloat = intermediateValueP; break;
        case 2: finalRedFloat = intermediateValueP; finalGreenFloat = inputBrightnessPercentage; finalBlueFloat = intermediateValueT; break;
        case 3: finalRedFloat = intermediateValueP; finalGreenFloat = intermediateValueQ; finalBlueFloat = inputBrightnessPercentage; break;
        case 4: finalRedFloat = intermediateValueT; finalGreenFloat = intermediateValueP; finalBlueFloat = inputBrightnessPercentage; break;
        default: finalRedFloat = inputBrightnessPercentage; finalGreenFloat = intermediateValueP; finalBlueFloat = intermediateValueQ; break;
    }
    return { (uint8_t)(finalRedFloat * 255), (uint8_t)(finalGreenFloat * 255), (uint8_t)(finalBlueFloat * 255) };
}

int main(int argumentCount, char** argumentValues) {
    LEDLayout hardwareDisplayLayout;
    hardwareDisplayLayout.parseArgument((argumentCount > 1) ? argumentValues[1] : "8x8");

    OPCClient networkPixelsClient(OPC_SERVER_ADDRESS, OPC_SERVER_PORT);
    
    int maximalHardwareAddressIndex = 0;
    for (const auto& systemPixel : hardwareDisplayLayout.pixels) {
        if (systemPixel.opc_index > maximalHardwareAddressIndex) {
            maximalHardwareAddressIndex = systemPixel.opc_index;
        }
    }
    
    std::vector<uint8_t> outgoingNetworkPayloadBuffer((maximalHardwareAddressIndex + 1) * 3, 0);
    auto baselineStartupTime = std::chrono::steady_clock::now();

    float animatedOffsetX = 0, animatedOffsetY = 0, animatedOffsetZ = 0;

    while (true) {
        auto renderedFrameStartTime = std::chrono::steady_clock::now();
        float elapsedMillisecondsCount = std::chrono::duration<float, std::milli>(renderedFrameStartTime - baselineStartupTime).count();

        float rotatingGeometricAngle = sinf(elapsedMillisecondsCount * 0.001f);
        float advancingTimeZAxis = elapsedMillisecondsCount * 0.00008f;
        float shiftingCurrentHue = elapsedMillisecondsCount * 0.01f;
        
        float shiftingSaturationLevel = 150.0f * std::clamp(powf(1.15f * (float)(sharedNoiseEngine.generateNoise(elapsedMillisecondsCount * 0.000122, 0, 0) + 1.0) * 0.5f, 2.5f), 0.0f, 1.0f);
        float fluidPulseSpacing = (float)(sharedNoiseEngine.generateNoise(elapsedMillisecondsCount * 0.000124, 0, 0) + 1.0) * 0.5f * 0.1f;

        animatedOffsetX += cosf(rotatingGeometricAngle) * GLOBAL_MOVEMENT_SPEED;
        animatedOffsetY += sinf(rotatingGeometricAngle) * GLOBAL_MOVEMENT_SPEED;
        animatedOffsetZ += (float)((sharedNoiseEngine.generateNoise(elapsedMillisecondsCount * 0.000014, 0, 0) + 1.0) * 0.5 - 0.5) * Z_AXIS_SPEED_SCALE;

        float dynamicFocusCenterX = (float)(sharedNoiseEngine.generateNoise(elapsedMillisecondsCount * 0.000125, 0, 0) + 1.0) * 0.5f * 1.25f * (float)hardwareDisplayLayout.logicalWidth;
        float dynamicFocusCenterY = (float)(sharedNoiseEngine.generateNoise(elapsedMillisecondsCount * -0.000125, 0, 0) + 1.0) * 0.5f * 1.25f * (float)hardwareDisplayLayout.logicalHeight;

        for (size_t layoutIteratorIndex = 0; layoutIteratorIndex < hardwareDisplayLayout.pixels.size(); ++layoutIteratorIndex) {
            const auto& specificMappedPixel = hardwareDisplayLayout.pixels[layoutIteratorIndex];
            
            float logicalCoordinateX = specificMappedPixel.norm_x * (float)hardwareDisplayLayout.logicalWidth;
            float logicalCoordinateY = specificMappedPixel.norm_y * (float)hardwareDisplayLayout.logicalHeight;

            float calculatedDistanceMetric = sqrtf(powf(logicalCoordinateX - dynamicFocusCenterX, 2) + powf(logicalCoordinateY - dynamicFocusCenterY, 2));
            float radialPulseIntensity = (sinf(animatedOffsetZ + calculatedDistanceMetric * fluidPulseSpacing) - 0.3f) * 0.3f;

            float primaryNoiseSample = calculateFractalNoiseLevels(animatedOffsetX + specificMappedPixel.norm_x * 0.5f + radialPulseIntensity, animatedOffsetY + specificMappedPixel.norm_y * 0.5f, advancingTimeZAxis) - 0.75f;
            float secondaryNoiseSample = calculateFractalNoiseLevels(animatedOffsetX + specificMappedPixel.norm_x * 0.5f, animatedOffsetY + specificMappedPixel.norm_y * 0.5f, advancingTimeZAxis + 10.0f) - 0.75f;

            PixelColorState resolvedColor = calculateHsbToRgb(
                fmodf(shiftingCurrentHue + 40.0f * secondaryNoiseSample, HSB_MAXIMUM_VALUE),
                shiftingSaturationLevel,
                100.0f * std::clamp(powf(3.0f * primaryNoiseSample, 1.5f), 0.0f, 0.9f)
            );
            
            int activeVectorBaseIndex = specificMappedPixel.opc_index * 3;
            outgoingNetworkPayloadBuffer[activeVectorBaseIndex]     = resolvedColor.redIntensity;
            outgoingNetworkPayloadBuffer[activeVectorBaseIndex + 1] = resolvedColor.greenIntensity;
            outgoingNetworkPayloadBuffer[activeVectorBaseIndex + 2] = resolvedColor.blueIntensity;
        }

        networkPixelsClient.sendPixelPayload(0, outgoingNetworkPayloadBuffer.data(), outgoingNetworkPayloadBuffer.size());
        std::this_thread::sleep_until(renderedFrameStartTime + std::chrono::milliseconds(1000 / TARGET_ANIMATION_FPS));
    }
    return 0;
}