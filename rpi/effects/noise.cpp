#include "ledlayout.h"
#include "opcclient.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>

// ============================================================================
// CONSTANT DEFINITIONS
// ============================================================================

constexpr const char* OPC_SERVER_ADDRESS = "127.0.0.1";
constexpr int OPC_SERVER_PORT = 7890;
constexpr int TARGET_ANIMATION_FPS = 60;
constexpr int MILLISECONDS_PER_SECOND = 1000;

constexpr float GLOBAL_NOISE_SCALE = 0.005f;
constexpr float GLOBAL_MOVEMENT_SPEED = 0.002f;
constexpr float Z_AXIS_SPEED_SCALE = 0.1f;
constexpr float HSB_MAXIMUM_VALUE = 100.0f;
constexpr float RGB_MAXIMUM_VALUE = 255.0f;
constexpr int FRACTAL_NOISE_OCTAVES = 4;
constexpr int PERMUTATION_TABLE_SIZE = 512;
constexpr int PERMUTATION_MASK_VALUE = 255;

// ============================================================================
// DATA STRUCTURES
// ============================================================================

/**
 * @brief Represents the standard Red, Green, and Blue intensities for a pixel.
 */
struct PixelColorState { 
    uint8_t redIntensity;
    uint8_t greenIntensity;
    uint8_t blueIntensity; 
};

// ============================================================================
// PERLIN NOISE GENERATOR CLASS
// ============================================================================

/**
 * @brief Generates mathematical Perlin noise using a predefined permutation table.
 * Adheres to the Single Responsibility Principle by solely managing noise state.
 */
class PerlinNoiseGenerator {
private:
    int spatialPermutationTable[PERMUTATION_TABLE_SIZE];

    /**
     * @brief Calculates the ease curve to smooth interpolation.
     * Verbose implementation to avoid inline math expressions.
     */
    double calculateFadeCurve(double timeValueInput) const { 
        double timeSquared = timeValueInput * timeValueInput;
        double timeCubed = timeSquared * timeValueInput;
        
        double innerMultiplier = timeValueInput * 6.0;
        double innerPolynomial = innerMultiplier - 15.0;
        
        double middleMultiplier = timeValueInput * innerPolynomial;
        double middlePolynomial = middleMultiplier + 10.0;
        
        double finalFadeValue = timeCubed * middlePolynomial;
        return finalFadeValue;
    }
    
    /**
     * @brief Performs standard linear interpolation between two bounds.
     */
    double performLinearInterpolate(double blendFactorValue, double boundValueA, double boundValueB) const { 
        double valueDifference = boundValueB - boundValueA;
        double blendedDifference = blendFactorValue * valueDifference;
        double finalInterpolatedValue = boundValueA + blendedDifference;
        return finalInterpolatedValue;
    }
    
    /**
     * @brief Calculates the gradient vector based on the hash identifier and coordinates.
     */
    double calculateGradientVector(int hashIdentifier, double coordinateX, 
                                   double coordinateY, double coordinateZ) const {
        
        int maskedHashValue = hashIdentifier & 15;
        double componentVectorU;
        
        if (maskedHashValue < 8) {
            componentVectorU = coordinateX;
        } else {
            componentVectorU = coordinateY;
        }

        double componentVectorV;
        if (maskedHashValue < 4) {
            componentVectorV = coordinateY;
        } else if (maskedHashValue == 12 || maskedHashValue == 14) {
            componentVectorV = coordinateX;
        } else {
            componentVectorV = coordinateZ;
        }

        double calculatedResultU;
        if ((maskedHashValue & 1) == 0) {
            calculatedResultU = componentVectorU;
        } else {
            calculatedResultU = -componentVectorU;
        }

        double calculatedResultV;
        if ((maskedHashValue & 2) == 0) {
            calculatedResultV = componentVectorV;
        } else {
            calculatedResultV = -componentVectorV;
        }

        double finalGradientSum = calculatedResultU + calculatedResultV;
        return finalGradientSum;
    }

public:
    /**
     * @brief Constructs the generator and explicitly fills the spatial hash tables.
     */
    PerlinNoiseGenerator() {
        int standardPermutationsList[256] = { 
            151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
            190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,88,237,149,56,87,174,
            20,125,136,171,168,68,175,74,165,71,134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,
            230,220,105,92,41,55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,18,
            169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,250,124,123,5,202,38,
            147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,223,183,170,213,119,248,152,2,
            44,154,163,70,221,153,101,155,167,43,172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,
            104,218,246,97,228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,107,49,
            192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,138,236,205,93,222,114,67,
            29,24,72,243,141,128,195,78,66,215,61,156,180 
        };
        
        for (int populationIndex = 0; populationIndex < 256; populationIndex++) {
            int currentPermutationValue = standardPermutationsList[populationIndex];
            spatialPermutationTable[populationIndex] = currentPermutationValue;
            
            int offsetIndex = 256 + populationIndex;
            spatialPermutationTable[offsetIndex] = currentPermutationValue;
        }
    }

    /**
     * @brief Generates a single noise sample at the given 3D coordinates.
     */
    double generateNoise(double targetCoordX, double targetCoordY, double targetCoordZ) const {
        double floorValueX = floor(targetCoordX);
        double floorValueY = floor(targetCoordY);
        double floorValueZ = floor(targetCoordZ);

        int integerUnitX = static_cast<int>(floorValueX) & PERMUTATION_MASK_VALUE;
        int integerUnitY = static_cast<int>(floorValueY) & PERMUTATION_MASK_VALUE;
        int integerUnitZ = static_cast<int>(floorValueZ) & PERMUTATION_MASK_VALUE;
        
        double fractionalCoordX = targetCoordX - floorValueX; 
        double fractionalCoordY = targetCoordY - floorValueY; 
        double fractionalCoordZ = targetCoordZ - floorValueZ;
        
        double fadeCurveX = calculateFadeCurve(fractionalCoordX);
        double fadeCurveY = calculateFadeCurve(fractionalCoordY);
        double fadeCurveZ = calculateFadeCurve(fractionalCoordZ);
        
        int hashValA = spatialPermutationTable[integerUnitX] + integerUnitY;
        int hashValAA = spatialPermutationTable[hashValA] + integerUnitZ;
        int hashValAB = spatialPermutationTable[hashValA + 1] + integerUnitZ;
        
        int nextIntegerUnitX = integerUnitX + 1;
        int hashValB = spatialPermutationTable[nextIntegerUnitX] + integerUnitY;
        int hashValBA = spatialPermutationTable[hashValB] + integerUnitZ;
        int hashValBB = spatialPermutationTable[hashValB + 1] + integerUnitZ;
        
        double coordinateXMinusOne = fractionalCoordX - 1.0;
        double coordinateYMinusOne = fractionalCoordY - 1.0;
        double coordinateZMinusOne = fractionalCoordZ - 1.0;

        int permAA = spatialPermutationTable[hashValAA];
        int permBA = spatialPermutationTable[hashValBA];
        int permAB = spatialPermutationTable[hashValAB];
        int permBB = spatialPermutationTable[hashValBB];

        double gradientAA = calculateGradientVector(permAA, fractionalCoordX, fractionalCoordY, fractionalCoordZ);
        double gradientBA = calculateGradientVector(permBA, coordinateXMinusOne, fractionalCoordY, fractionalCoordZ);
        double gradientAB = calculateGradientVector(permAB, fractionalCoordX, coordinateYMinusOne, fractionalCoordZ);
        double gradientBB = calculateGradientVector(permBB, coordinateXMinusOne, coordinateYMinusOne, fractionalCoordZ);

        double interpolateXBottom = performLinearInterpolate(fadeCurveX, gradientAA, gradientBA);
        double interpolateXTop = performLinearInterpolate(fadeCurveX, gradientAB, gradientBB);
        double interpolateYBottom = performLinearInterpolate(fadeCurveY, interpolateXBottom, interpolateXTop);

        int permAAZ = spatialPermutationTable[hashValAA + 1];
        int permBAZ = spatialPermutationTable[hashValBA + 1];
        int permABZ = spatialPermutationTable[hashValAB + 1];
        int permBBZ = spatialPermutationTable[hashValBB + 1];

        double gradientAAZ = calculateGradientVector(permAAZ, fractionalCoordX, fractionalCoordY, coordinateZMinusOne);
        double gradientBAZ = calculateGradientVector(permBAZ, coordinateXMinusOne, fractionalCoordY, coordinateZMinusOne);
        double gradientABZ = calculateGradientVector(permABZ, fractionalCoordX, coordinateYMinusOne, coordinateZMinusOne);
        double gradientBBZ = calculateGradientVector(permBBZ, coordinateXMinusOne, coordinateYMinusOne, coordinateZMinusOne);

        double interpolateXBottomZ = performLinearInterpolate(fadeCurveX, gradientAAZ, gradientBAZ);
        double interpolateXTopZ = performLinearInterpolate(fadeCurveX, gradientABZ, gradientBBZ);
        double interpolateYTop = performLinearInterpolate(fadeCurveY, interpolateXBottomZ, interpolateXTopZ);

        double finalNoiseResult = performLinearInterpolate(fadeCurveZ, interpolateYBottom, interpolateYTop);
        return finalNoiseResult;
    }
};

// ============================================================================
// COLOR UTILITY CLASS
// ============================================================================

/**
 * @brief Handles conversion of colors between different mathematical models.
 */
class ColorConverter {
public:
    /**
     * @brief Converts Hue, Saturation, and Brightness directly into RGB intensities.
     */
    static PixelColorState calculateHsbToRgb(float inputHueAngle, float inputSaturationPercentage, 
                                             float inputBrightnessPercentage) {
        
        float hueModulo = fmod(inputHueAngle, HSB_MAXIMUM_VALUE);
        float normalizedHue = hueModulo / HSB_MAXIMUM_VALUE;
        
        float normalizedSaturationRaw = inputSaturationPercentage / HSB_MAXIMUM_VALUE;
        float normalizedSaturation = std::clamp(normalizedSaturationRaw, 0.0f, 1.0f);
        
        float normalizedBrightnessRaw = inputBrightnessPercentage / HSB_MAXIMUM_VALUE;
        float normalizedBrightness = std::clamp(normalizedBrightnessRaw, 0.0f, 1.0f);

        if (normalizedSaturation == 0.0f) {
            float grayscaleFloatValue = normalizedBrightness * RGB_MAXIMUM_VALUE;
            uint8_t grayscaleCalculatedValue = static_cast<uint8_t>(grayscaleFloatValue);
            return { grayscaleCalculatedValue, grayscaleCalculatedValue, grayscaleCalculatedValue };
        }

        float colorSectorPosition = normalizedHue * 6.0f;
        float sectorFloorValue = floor(colorSectorPosition);
        int sectorBaseIndex = static_cast<int>(sectorFloorValue);
        
        float factorialFractionalPart = colorSectorPosition - sectorFloorValue;
        
        float inverseSaturation = 1.0f - normalizedSaturation;
        float intermediateValueP = normalizedBrightness * inverseSaturation;
        
        float partialSaturationQ = 1.0f - (normalizedSaturation * factorialFractionalPart);
        float intermediateValueQ = normalizedBrightness * partialSaturationQ;
        
        float inverseFractionalPart = 1.0f - factorialFractionalPart;
        float partialSaturationT = 1.0f - (normalizedSaturation * inverseFractionalPart);
        float intermediateValueT = normalizedBrightness * partialSaturationT;

        float finalRedFloat;
        float finalGreenFloat;
        float finalBlueFloat;
        
        int switchCondition = sectorBaseIndex % 6;

        switch (switchCondition) {
            case 0: 
                finalRedFloat = normalizedBrightness; 
                finalGreenFloat = intermediateValueT; 
                finalBlueFloat = intermediateValueP; 
                break;
            case 1: 
                finalRedFloat = intermediateValueQ; 
                finalGreenFloat = normalizedBrightness; 
                finalBlueFloat = intermediateValueP; 
                break;
            case 2: 
                finalRedFloat = intermediateValueP; 
                finalGreenFloat = normalizedBrightness; 
                finalBlueFloat = intermediateValueT; 
                break;
            case 3: 
                finalRedFloat = intermediateValueP; 
                finalGreenFloat = intermediateValueQ; 
                finalBlueFloat = normalizedBrightness; 
                break;
            case 4: 
                finalRedFloat = intermediateValueT; 
                finalGreenFloat = intermediateValueP; 
                finalBlueFloat = normalizedBrightness; 
                break;
            default: 
                finalRedFloat = normalizedBrightness; 
                finalGreenFloat = intermediateValueP; 
                finalBlueFloat = intermediateValueQ; 
                break;
        }
        
        uint8_t finalRedInt = static_cast<uint8_t>(finalRedFloat * RGB_MAXIMUM_VALUE);
        uint8_t finalGreenInt = static_cast<uint8_t>(finalGreenFloat * RGB_MAXIMUM_VALUE);
        uint8_t finalBlueInt = static_cast<uint8_t>(finalBlueFloat * RGB_MAXIMUM_VALUE);
        
        return { finalRedInt, finalGreenInt, finalBlueInt };
    }
};

// ============================================================================
// ANIMATION ENGINE CLASS
// ============================================================================

/**
 * @brief Manages the state, calculations, and rendering loop for the LED animation.
 */
class AnimationEngine {
private:
    PerlinNoiseGenerator sharedNoiseEngine;
    float animatedOffsetX;
    float animatedOffsetY;
    float animatedOffsetZ;

    /**
     * @brief Generates multiple octaves of Perlin noise for richer organic textures.
     */
    float calculateFractalNoiseLevels(float positionCoordinateX, float positionCoordinateY, 
                                      float positionCoordinateZ) {
        
        float cumulativeCalculationResult = 0.0f;
        float currentAmplitudeStrength = 1.0f;
        
        float trackingCoordinateX = positionCoordinateX;
        float trackingCoordinateY = positionCoordinateY;
        float trackingCoordinateZ = positionCoordinateZ;

        for (int octaveLevel = 0; octaveLevel < FRACTAL_NOISE_OCTAVES; octaveLevel++) {
            double rawNoiseSample = sharedNoiseEngine.generateNoise(
                trackingCoordinateX, trackingCoordinateY, trackingCoordinateZ);
            
            float shiftedNoiseSample = static_cast<float>(rawNoiseSample) + 1.0f;
            float halvedNoiseSample = shiftedNoiseSample * 0.5f;
            float amplitudeAppliedNoise = halvedNoiseSample * currentAmplitudeStrength;
            
            cumulativeCalculationResult = cumulativeCalculationResult + amplitudeAppliedNoise;
            
            currentAmplitudeStrength = currentAmplitudeStrength * 0.5f; 
            trackingCoordinateX = trackingCoordinateX * 2.0f; 
            trackingCoordinateY = trackingCoordinateY * 2.0f; 
            trackingCoordinateZ = trackingCoordinateZ * 2.0f;
        }
        
        return cumulativeCalculationResult;
    }

public:
    /**
     * @brief Constructor initializes animation state values.
     */
    AnimationEngine() {
        animatedOffsetX = 0.0f;
        animatedOffsetY = 0.0f;
        animatedOffsetZ = 0.0f;
    }

    /**
     * @brief Generates the frame payload based on the mathematical state.
     */
    void generateFramePayload(const LedLayout& hardwareDisplayLayout, float elapsedMillisecondsCount, 
                              std::vector<uint8_t>& outgoingNetworkPayloadBuffer) {
        
        float timeMultiplierGeometric = 0.001f;
        float geometricTimeValue = elapsedMillisecondsCount * timeMultiplierGeometric;
        float rotatingGeometricAngle = sinf(geometricTimeValue);
        
        float timeMultiplierZAxis = 0.00008f;
        float advancingTimeZAxis = elapsedMillisecondsCount * timeMultiplierZAxis;
        
        float timeMultiplierHue = 0.05f;
        float shiftingCurrentHue = elapsedMillisecondsCount * timeMultiplierHue; 
        
        float saturationTimeMultiplier = 0.000122f;
        float saturationTimeInput = elapsedMillisecondsCount * saturationTimeMultiplier;
        double saturationNoiseRaw = sharedNoiseEngine.generateNoise(saturationTimeInput, 0.0, 0.0);
        float saturationNoiseShifted = static_cast<float>(saturationNoiseRaw) + 1.0f;
        float saturationNoiseHalved = saturationNoiseShifted * 0.5f;
        float saturationNoiseScaled = 1.15f * saturationNoiseHalved;
        float saturationPowered = powf(saturationNoiseScaled, 2.5f);
        float clampedSaturation = std::clamp(saturationPowered, 0.0f, 1.0f);
        float shiftingSaturationLevel = 150.0f * clampedSaturation;
        
        float pulseTimeMultiplier = 0.000124f;
        float pulseTimeInput = elapsedMillisecondsCount * pulseTimeMultiplier;
        double pulseNoiseRaw = sharedNoiseEngine.generateNoise(pulseTimeInput, 0.0, 0.0);
        float pulseNoiseShifted = static_cast<float>(pulseNoiseRaw) + 1.0f;
        float pulseNoiseHalved = pulseNoiseShifted * 0.5f;
        float fluidPulseSpacing = pulseNoiseHalved * 0.1f;

        float offsetCosine = cosf(rotatingGeometricAngle);
        float scaledOffsetCosine = offsetCosine * GLOBAL_MOVEMENT_SPEED;
        animatedOffsetX = animatedOffsetX + scaledOffsetCosine;
        
        float offsetSine = sinf(rotatingGeometricAngle);
        float scaledOffsetSine = offsetSine * GLOBAL_MOVEMENT_SPEED;
        animatedOffsetY = animatedOffsetY + scaledOffsetSine;
        
        float zAxisTimeMultiplier = 0.000014f;
        float zAxisTimeInput = elapsedMillisecondsCount * zAxisTimeMultiplier;
        double zAxisNoiseRaw = sharedNoiseEngine.generateNoise(zAxisTimeInput, 0.0, 0.0);
        float zAxisNoiseShifted = static_cast<float>(zAxisNoiseRaw) + 1.0f;
        float zAxisNoiseHalved = zAxisNoiseShifted * 0.5f;
        float zAxisNoiseCentered = zAxisNoiseHalved - 0.5f;
        float scaledZAxisNoise = zAxisNoiseCentered * Z_AXIS_SPEED_SCALE;
        animatedOffsetZ = animatedOffsetZ + scaledZAxisNoise;

        float focusTimeMultiplierX = 0.000125f;
        float focusTimeInputX = elapsedMillisecondsCount * focusTimeMultiplierX;
        double focusNoiseRawX = sharedNoiseEngine.generateNoise(focusTimeInputX, 0.0, 0.0);
        float focusNoiseShiftedX = static_cast<float>(focusNoiseRawX) + 1.0f;
        float focusNoiseHalvedX = focusNoiseShiftedX * 0.5f;
        float focusMultiplierX = focusNoiseHalvedX * 1.25f;
        float dynamicFocusCenterX = focusMultiplierX * static_cast<float>(hardwareDisplayLayout.getLogicalWidth());

        float focusTimeMultiplierY = -0.000125f;
        float focusTimeInputY = elapsedMillisecondsCount * focusTimeMultiplierY;
        double focusNoiseRawY = sharedNoiseEngine.generateNoise(focusTimeInputY, 0.0, 0.0);
        float focusNoiseShiftedY = static_cast<float>(focusNoiseRawY) + 1.0f;
        float focusNoiseHalvedY = focusNoiseShiftedY * 0.5f;
        float focusMultiplierY = focusNoiseHalvedY * 1.25f;
        float dynamicFocusCenterY = focusMultiplierY * static_cast<float>(hardwareDisplayLayout.getLogicalHeight());

        for (size_t layoutIteratorIndex = 0; layoutIteratorIndex < hardwareDisplayLayout.getPixels().size(); 
             ++layoutIteratorIndex) {
            
            const auto& specificMappedPixel = hardwareDisplayLayout.getPixels()[layoutIteratorIndex];
            
            float logicalCoordinateX = specificMappedPixel.normX * static_cast<float>(hardwareDisplayLayout.getLogicalWidth());
            float logicalCoordinateY = specificMappedPixel.normY * static_cast<float>(hardwareDisplayLayout.getLogicalHeight());

            float deltaCoordinateX = logicalCoordinateX - dynamicFocusCenterX;
            float deltaCoordinateY = logicalCoordinateY - dynamicFocusCenterY;
            float squaredDeltaX = powf(deltaCoordinateX, 2.0f);
            float squaredDeltaY = powf(deltaCoordinateY, 2.0f);
            float sumOfSquares = squaredDeltaX + squaredDeltaY;
            float calculatedDistanceMetric = sqrtf(sumOfSquares);
            
            float distanceScaledSpacing = calculatedDistanceMetric * fluidPulseSpacing;
            float combinedSineInput = animatedOffsetZ + distanceScaledSpacing;
            float evaluatedSineValue = sinf(combinedSineInput);
            float shiftedSineValue = evaluatedSineValue - 0.3f;
            float radialPulseIntensity = shiftedSineValue * 0.3f;

            float halvedNormX = specificMappedPixel.normX * 0.5f;
            float halvedNormY = specificMappedPixel.normY * 0.5f;
            
            float primaryNoiseCoordX = animatedOffsetX + halvedNormX + radialPulseIntensity;
            float primaryNoiseCoordY = animatedOffsetY + halvedNormY;
            float primaryNoiseCoordZ = advancingTimeZAxis;
            
            float primaryNoiseRaw = calculateFractalNoiseLevels(
                primaryNoiseCoordX, primaryNoiseCoordY, primaryNoiseCoordZ);
            
            // Allow the noise curve to dip below zero naturally by shifting down by 0.45
            // This introduces moving "voids" of black across the canvas.
            float primaryNoiseSample = primaryNoiseRaw - 0.45f; 
            
            float secondaryNoiseCoordX = animatedOffsetX + halvedNormX;
            float secondaryNoiseCoordY = animatedOffsetY + halvedNormY;
            float secondaryNoiseCoordZ = advancingTimeZAxis + 10.0f;
            
            float secondaryNoiseRaw = calculateFractalNoiseLevels(
                secondaryNoiseCoordX, secondaryNoiseCoordY, secondaryNoiseCoordZ);
            float secondaryNoiseSample = secondaryNoiseRaw - 0.75f;

            float spatialColorVariance = 150.0f * secondaryNoiseSample;
            float combinedHueValue = shiftingCurrentHue + spatialColorVariance + HSB_MAXIMUM_VALUE;
            float finalModuloHue = fmodf(combinedHueValue, HSB_MAXIMUM_VALUE);
            
            // Protect the math engine: Never pass a negative number into powf()
            float primaryNoiseSafe = std::max(0.0f, primaryNoiseSample);
            
            // Boost the remaining visible signal so it still pops against the new dark background
            float boostedNoiseSignal = primaryNoiseSafe * 1.8f; 
            float primaryNoisePowered = powf(boostedNoiseSignal, 1.5f);
            
            float primaryNoiseClamped = std::clamp(primaryNoisePowered, 0.0f, 1.0f);
            
            // Removed the artificial +25.0f base brightness floor. Now allowed to hit 0.
            float finalBrightnessValue = 100.0f * primaryNoiseClamped; 

            PixelColorState resolvedColor = ColorConverter::calculateHsbToRgb(
                finalModuloHue, shiftingSaturationLevel, finalBrightnessValue);
            
            int activeVectorBaseIndex = specificMappedPixel.opcIndex * 3;
            outgoingNetworkPayloadBuffer[activeVectorBaseIndex] = resolvedColor.redIntensity;
            
            int activeVectorGreenIndex = activeVectorBaseIndex + 1;
            outgoingNetworkPayloadBuffer[activeVectorGreenIndex] = resolvedColor.greenIntensity;
            
            int activeVectorBlueIndex = activeVectorBaseIndex + 2;
            outgoingNetworkPayloadBuffer[activeVectorBlueIndex] = resolvedColor.blueIntensity;
        }
    }
};

// ============================================================================
// MAIN APPLICATION ENTRY
// ============================================================================

int main(int argumentCount, char** argumentValues) {
    LedLayout hardwareDisplayLayout;
    
    std::string layoutArgument = "8x8";
    if (argumentCount > 1) {
        layoutArgument = argumentValues[1];
    }
    hardwareDisplayLayout.parseArgument(layoutArgument);

    OPCClient networkPixelsClient(OPC_SERVER_ADDRESS, OPC_SERVER_PORT);
    
    int maximalHardwareAddressIndex = 0;
    for (const auto& systemPixel : hardwareDisplayLayout.getPixels()) {
        if (systemPixel.opcIndex > maximalHardwareAddressIndex) {
            maximalHardwareAddressIndex = systemPixel.opcIndex;
        }
    }
    
    int requiredArrayCapacity = maximalHardwareAddressIndex + 1;
    int payloadSizeInBytes = requiredArrayCapacity * 3;
    
    // std::vector handles dynamic memory securely, avoiding manual memory management or leaks
    std::vector<uint8_t> outgoingNetworkPayloadBuffer(payloadSizeInBytes, 0);
    
    AnimationEngine visualAnimationEngine;
    auto baselineStartupTime = std::chrono::steady_clock::now();
    int targetFrameDelayMilliseconds = MILLISECONDS_PER_SECOND / TARGET_ANIMATION_FPS;
    auto frameDelayDuration = std::chrono::milliseconds(targetFrameDelayMilliseconds);

    // Infinite render loop
    while (true) {
        auto renderedFrameStartTime = std::chrono::steady_clock::now();
        auto elapsedTimeDifference = renderedFrameStartTime - baselineStartupTime;
        float elapsedMillisecondsCount = std::chrono::duration<float, std::milli>(elapsedTimeDifference).count();

        visualAnimationEngine.generateFramePayload(
            hardwareDisplayLayout, 
            elapsedMillisecondsCount, 
            outgoingNetworkPayloadBuffer
        );

        networkPixelsClient.sendPixelPayload(
            0, 
            outgoingNetworkPayloadBuffer.data(), 
            outgoingNetworkPayloadBuffer.size()
        );
        
        auto nextFrameTimeTarget = renderedFrameStartTime + frameDelayDuration;
        std::this_thread::sleep_until(nextFrameTimeTarget);
    }
    
    return EXIT_SUCCESS;
}