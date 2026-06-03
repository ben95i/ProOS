#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "ledlayout.h"
#include "opcclient.h"

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <thread>
#include <chrono>
#include <complex>
#include <algorithm>
#include <cstdlib>
#include <alsa/asoundlib.h>

constexpr int TARGET_FRAMES_PER_SECOND = 60;
constexpr const char* DEFAULT_OPC_HOST = "127.0.0.1"; 
constexpr int DEFAULT_OPC_PORT = 7890;

constexpr unsigned int AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned int AUDIO_BUFFER_SIZE = 1024; 
constexpr unsigned int MAX_ALSA_LATENCY_US = 20000;
constexpr double PCM_NORMALIZATION_FACTOR = 32768.0;

constexpr double HAMMING_WINDOW_ALPHA = 0.54;
constexpr double HAMMING_WINDOW_BETA = 0.46;
constexpr double TWO_PI_CONSTANT = 2.0 * M_PI;

constexpr float DEFAULT_SCROLL_SPEED = 30.0f;

// --- DECAY RATES ---
// PEAK_DECAY_RATE: How fast the vertical bars fall down. (Lower = faster fall)
constexpr float PEAK_DECAY_RATE = 0.92f;

// AGC_DECAY_RATE: How fast the auto-gain recovers after a loud sound.
// 0.99f = very slow recovery, 0.90f = very fast recovery.
constexpr float AGC_DECAY_RATE = 0.92f; 

constexpr int MAX_COLOR_VALUE = 255;
constexpr float HSV_SECTOR_SIZE = 43.0f;
constexpr int HSV_REMAINDER_MULTIPLIER = 6;
constexpr int COLOR_CHANNELS = 3;

constexpr int MAX_GRID_DIMENSION = 1024;

struct PixelColor { uint8_t redIntensity, greenIntensity, blueIntensity; };

PixelColor convertHsvToRgb(int hueValue, int saturationValue, int brightnessValue) {
    if (saturationValue == 0) return { static_cast<uint8_t>(brightnessValue), static_cast<uint8_t>(brightnessValue), static_cast<uint8_t>(brightnessValue) };
    
    uint8_t colorRegion = static_cast<uint8_t>(hueValue / HSV_SECTOR_SIZE);
    uint8_t colorRemainder = (hueValue - (colorRegion * static_cast<int>(HSV_SECTOR_SIZE))) * HSV_REMAINDER_MULTIPLIER;
    
    uint8_t pColor = (brightnessValue * (MAX_COLOR_VALUE - saturationValue)) >> 8;
    uint8_t qColor = (brightnessValue * (MAX_COLOR_VALUE - ((saturationValue * colorRemainder) >> 8))) >> 8;
    uint8_t tColor = (brightnessValue * (MAX_COLOR_VALUE - ((saturationValue * (MAX_COLOR_VALUE - colorRemainder)) >> 8))) >> 8;
    
    switch (colorRegion) {
        case 0: return {static_cast<uint8_t>(brightnessValue), tColor, pColor}; 
        case 1: return {qColor, static_cast<uint8_t>(brightnessValue), pColor};
        case 2: return {pColor, static_cast<uint8_t>(brightnessValue), tColor}; 
        case 3: return {pColor, qColor, static_cast<uint8_t>(brightnessValue)};
        case 4: return {tColor, pColor, static_cast<uint8_t>(brightnessValue)}; 
        default: return {static_cast<uint8_t>(brightnessValue), pColor, qColor};
    }
}

class DisplayBuffer {
public:
    int gridWidth, gridHeight;
    std::vector<PixelColor> pixelDataBuffer;

    DisplayBuffer(int targetWidth, int targetHeight) 
        : gridWidth(targetWidth), gridHeight(targetHeight), pixelDataBuffer(targetWidth * targetHeight, {0, 0, 0}) {}

    void clearBufferData() { 
        std::fill(pixelDataBuffer.begin(), pixelDataBuffer.end(), PixelColor{0, 0, 0}); 
    }

    void applyPixelColor(int targetColumn, int targetRow, PixelColor colorToApply) {
        if (targetColumn >= 0 && targetColumn < gridWidth && targetRow >= 0 && targetRow < gridHeight) {
            pixelDataBuffer[targetRow * gridWidth + targetColumn] = colorToApply;
        }
    }

    void drawScrollingImage(const std::vector<PixelColor>& sourceImageData, int imageWidthPixels, int imageHeightPixels, float scrollingOffsetAmount) {
        if (sourceImageData.empty() || imageWidthPixels <= 0 || imageHeightPixels <= 0) return;
        
        for (int currentRow = 0; currentRow < gridHeight; ++currentRow) {
            for (int currentColumn = 0; currentColumn < gridWidth; ++currentColumn) {
                int sourcePixelX = static_cast<int>(currentColumn * (static_cast<float>(imageWidthPixels) / gridWidth)) % imageWidthPixels;
                int sourcePixelY = static_cast<int>(currentRow * (static_cast<float>(imageHeightPixels) / gridHeight) + scrollingOffsetAmount) % imageHeightPixels;
                if (sourcePixelY < 0) sourcePixelY += imageHeightPixels; 
                applyPixelColor(currentColumn, currentRow, sourceImageData[sourcePixelY * imageWidthPixels + sourcePixelX]);
            }
        }
    }

    void drawVerticalFrequencyBar(int targetColumn, int barHeightPixels, PixelColor barDrawColor) {
        int boundedHeightValue = std::min(barHeightPixels, gridHeight);
        int startingRowIndex = gridHeight - 1;
        int endingRowIndex = gridHeight - boundedHeightValue;
        
        for (int rowPointer = startingRowIndex; rowPointer >= endingRowIndex && rowPointer >= 0; --rowPointer) {
            applyPixelColor(targetColumn, rowPointer, barDrawColor);
        }
    }
};

void performFastFourierTransform(std::vector<std::complex<double>>& complexDataBuffer) {
    int totalBufferSize = complexDataBuffer.size();
    
    for (int currentIndex = 1, reversedIndex = 0; currentIndex < totalBufferSize; currentIndex++) {
        int halfSizeBitMask = totalBufferSize >> 1;
        for (; reversedIndex & halfSizeBitMask; halfSizeBitMask >>= 1) reversedIndex ^= halfSizeBitMask;
        reversedIndex ^= halfSizeBitMask;
        if (currentIndex < reversedIndex) std::swap(complexDataBuffer[currentIndex], complexDataBuffer[reversedIndex]);
    }
    
    for (int sublistLengthValue = 2; sublistLengthValue <= totalBufferSize; sublistLengthValue <<= 1) {
        double rotationAngleStep = TWO_PI_CONSTANT / sublistLengthValue;
        std::complex<double> phaseMultiplierConstant(cos(rotationAngleStep), sin(rotationAngleStep));
        
        for (int blockStartIndex = 0; blockStartIndex < totalBufferSize; blockStartIndex += sublistLengthValue) {
            std::complex<double> currentPhaseState(1.0);
            int halfSublistLength = sublistLengthValue / 2;
            
            for (int elementIndexValue = 0; elementIndexValue < halfSublistLength; elementIndexValue++) {
                std::complex<double> evenComponentValue = complexDataBuffer[blockStartIndex + elementIndexValue];
                std::complex<double> oddComponentValue = complexDataBuffer[blockStartIndex + elementIndexValue + halfSublistLength] * currentPhaseState;
                
                complexDataBuffer[blockStartIndex + elementIndexValue] = evenComponentValue + oddComponentValue;
                complexDataBuffer[blockStartIndex + elementIndexValue + halfSublistLength] = evenComponentValue - oddComponentValue;
                currentPhaseState *= phaseMultiplierConstant;
            }
        }
    }
}

int main(int argumentCount, char** argumentValues) {
    LedLayout mappedLayout;
    std::string configurationArgument = (argumentCount > 1) ? argumentValues[1] : "8x8";
    mappedLayout.parseArgument(configurationArgument);

    int configuredGridWidth = std::clamp(mappedLayout.getLogicalWidth() > 0 ? mappedLayout.getLogicalWidth() : 16, 1, MAX_GRID_DIMENSION);
    int configuredGridHeight = std::clamp(mappedLayout.getLogicalHeight() > 0 ? mappedLayout.getLogicalHeight() : 16, 1, MAX_GRID_DIMENSION);

    float verticalZoom = 1.0f; // Replaces hard max bar height
    int octaveBands = configuredGridWidth; 
    float minimumFrequencyThreshold = 20.0f;
    float maximumFrequencyThreshold = 20000.0f;
    float minAgcThreshold = 0.001f; // Default low floor

    if (argumentCount > 2) verticalZoom = std::atof(argumentValues[2]);
    if (argumentCount > 3) octaveBands = std::atoi(argumentValues[3]);
    if (argumentCount > 4) minimumFrequencyThreshold = std::atof(argumentValues[4]);
    if (argumentCount > 5) maximumFrequencyThreshold = std::atof(argumentValues[5]);
    if (argumentCount > 6) minAgcThreshold = std::atof(argumentValues[6]);

    // Safety checks for inputs and boundaries
    verticalZoom = std::max(0.01f, verticalZoom);
    minimumFrequencyThreshold = std::max(1.0f, minimumFrequencyThreshold);
    maximumFrequencyThreshold = std::clamp(maximumFrequencyThreshold, minimumFrequencyThreshold + 1.0f, static_cast<float>(AUDIO_SAMPLE_RATE / 2));
    octaveBands = std::clamp(octaveBands, 1, configuredGridWidth);
    minAgcThreshold = std::max(0.0001f, minAgcThreshold);

    std::cout << "--- Visualizer Configuration ---\n"
              << "Usage: ./spectro [Layout] [Vertical Zoom] [Octave Bands] [Min Freq] [Max Freq] [Min AGC Threshold]\n\n"
              << "Arguments Info:\n"
              << "  [1] Layout String   : Defines LED mapping     (Default: 8x8)\n"
              << "  [2] Vertical Zoom   : Audio scale multiplier  (Default: 1.0. Try 0.5 to shrink, 2.0 to stretch up)\n"
              << "  [3] Octave Bands    : Columns of audio data   (Min: 1, Max: " << configuredGridWidth << " based on layout)\n"
              << "  [4] Min Freq (Hz)   : EQ lower cutoff         (Min: 1.0, Max: < Max Freq)\n"
              << "  [5] Max Freq (Hz)   : EQ upper cutoff         (Min: > Min Freq, Max: " << (AUDIO_SAMPLE_RATE / 2) << " Nyquist limit)\n"
              << "  [6] AGC Noise Floor : Minimum gain threshold  (Try: 0.01 to 0.05 for noise suppression)\n\n"
              << "Current Settings:\n"
              << "  Virtual Grid:   " << configuredGridWidth << "x" << configuredGridHeight << "\n"
              << "  Vertical Zoom:  " << verticalZoom << "x\n"
              << "  Octave Bands:   " << octaveBands << " bars\n"
              << "  EQ Band:        " << minimumFrequencyThreshold << " Hz - " << maximumFrequencyThreshold << " Hz\n"
              << "  AGC Floor:      " << minAgcThreshold << "\n"
              << "--------------------------------\n";

    // Precalculate logarithmic boundaries
    float logMinFreq = std::log10(minimumFrequencyThreshold);
    float logMaxFreq = std::log10(maximumFrequencyThreshold);
    float logFreqStep = (logMaxFreq - logMinFreq) / octaveBands;

    DisplayBuffer activeDisplayBuffer(configuredGridWidth, configuredGridHeight);
    OPCClient networkDisplayClient(DEFAULT_OPC_HOST, DEFAULT_OPC_PORT);
    
    int loadedImageWidthPixels, loadedImageHeightPixels, loadedImageChannelsCount;
    unsigned char* rawLoadedImageData = stbi_load("spectro_bg.jpg", &loadedImageWidthPixels, &loadedImageHeightPixels, &loadedImageChannelsCount, COLOR_CHANNELS);
    std::vector<PixelColor> scrollingBackgroundGraphic;
    
    if (rawLoadedImageData) {
        for (int pixelDataOffset = 0; pixelDataOffset < loadedImageWidthPixels * loadedImageHeightPixels * COLOR_CHANNELS; pixelDataOffset += COLOR_CHANNELS) {
            scrollingBackgroundGraphic.push_back({rawLoadedImageData[pixelDataOffset], rawLoadedImageData[pixelDataOffset+1], rawLoadedImageData[pixelDataOffset+2]});
        }
        stbi_image_free(rawLoadedImageData);
    }

    snd_pcm_t* activeAudioDeviceHandle;
    if (snd_pcm_open(&activeAudioDeviceHandle, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "Audio device unavailable. Visualizer will run silently, background animation will continue.\n";
        activeAudioDeviceHandle = nullptr;
    } else {
        snd_pcm_set_params(activeAudioDeviceHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_SAMPLE_RATE, 1, MAX_ALSA_LATENCY_US);
    }

    std::vector<int16_t> capturedAudioRawBuffer(AUDIO_BUFFER_SIZE);
    std::vector<std::complex<double>> fastFourierComplexBuffer(AUDIO_BUFFER_SIZE);
    
    std::vector<float> audioColumnPeakValues(octaveBands, 0.0f);
    
    // Initialize autoGainMaxPeak to the new user-defined floor
    float autoGainMaxPeak = minAgcThreshold;
    int activeGlobalHueState = 0;

    auto scheduledNextFrameTime = std::chrono::steady_clock::now();
    auto initialApplicationStartTime = std::chrono::steady_clock::now();
    
    int maximumOpcPixelIndex = 0;
    for (const auto& physicalPixel : mappedLayout.getPixels()) {
        if (physicalPixel.opcIndex > maximumOpcPixelIndex) {
            maximumOpcPixelIndex = physicalPixel.opcIndex;
        }
    }
    std::vector<uint8_t> outgoingNetworkPayload((maximumOpcPixelIndex + 1) * COLOR_CHANNELS, 0);

    while (true) {
        activeDisplayBuffer.clearBufferData();

        if (activeAudioDeviceHandle) {
            int capturedAudioFrames = snd_pcm_readi(activeAudioDeviceHandle, capturedAudioRawBuffer.data(), AUDIO_BUFFER_SIZE);
            if (capturedAudioFrames < 0) snd_pcm_recover(activeAudioDeviceHandle, capturedAudioFrames, 0);
            
            for (int audioSampleIndex = 0; audioSampleIndex < AUDIO_BUFFER_SIZE; ++audioSampleIndex) {
                double computedWindowMultiplier = HAMMING_WINDOW_ALPHA - HAMMING_WINDOW_BETA * cos(TWO_PI_CONSTANT * audioSampleIndex / (AUDIO_BUFFER_SIZE - 1));
                fastFourierComplexBuffer[audioSampleIndex] = { (capturedAudioRawBuffer[audioSampleIndex] / PCM_NORMALIZATION_FACTOR) * computedWindowMultiplier, 0.0 };
            }
            performFastFourierTransform(fastFourierComplexBuffer);
        } else {
            std::fill(fastFourierComplexBuffer.begin(), fastFourierComplexBuffer.end(), std::complex<double>(0.0, 0.0));
        }

        auto currentRenderingTimestamp = std::chrono::steady_clock::now();
        float elapsedExecutionSeconds = std::chrono::duration<float>(currentRenderingTimestamp - initialApplicationStartTime).count();
        
        activeDisplayBuffer.drawScrollingImage(scrollingBackgroundGraphic, loadedImageWidthPixels, loadedImageHeightPixels, elapsedExecutionSeconds * DEFAULT_SCROLL_SPEED);

        PixelColor activeBarDrawColor = convertHsvToRgb(activeGlobalHueState, MAX_COLOR_VALUE, MAX_COLOR_VALUE);
        
        float currentFrameMax = 0.0001f;
        std::vector<float> rawBandAmplitudes(octaveBands, 0.0f);

        // Map frequencies logarithmically to discrete octave bands
        for (int bandIndex = 0; bandIndex < octaveBands; ++bandIndex) {
            float startFreq = std::pow(10.0f, logMinFreq + bandIndex * logFreqStep);
            float endFreq = std::pow(10.0f, logMinFreq + (bandIndex + 1) * logFreqStep);
            
            int signalBlockStartBin = static_cast<int>(startFreq * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
            int signalBlockEndBin = static_cast<int>(endFreq * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
            
            signalBlockStartBin = std::clamp(signalBlockStartBin, 0, static_cast<int>(AUDIO_BUFFER_SIZE / 2) - 1);
            signalBlockEndBin = std::clamp(signalBlockEndBin, signalBlockStartBin + 1, static_cast<int>(AUDIO_BUFFER_SIZE / 2));
            
            float aggregatedMagnitudeTotal = 0;
            int totalFrequencyBinsInBand = signalBlockEndBin - signalBlockStartBin;
            
            for (int frequencyBinIndex = signalBlockStartBin; frequencyBinIndex < signalBlockEndBin; ++frequencyBinIndex) {
                aggregatedMagnitudeTotal += std::abs(fastFourierComplexBuffer[frequencyBinIndex]);
            }
            
            float rawAmplitude = aggregatedMagnitudeTotal / totalFrequencyBinsInBand;
            rawBandAmplitudes[bandIndex] = rawAmplitude;
            if (rawAmplitude > currentFrameMax) {
                currentFrameMax = rawAmplitude;
            }
        }

        // --- Auto-Gain Control (AGC) Update ---
        if (currentFrameMax > autoGainMaxPeak) {
            autoGainMaxPeak = currentFrameMax; 
        } else {
            autoGainMaxPeak = autoGainMaxPeak * AGC_DECAY_RATE; 
            
            // Restrict AGC from amplifying silence by using the user-defined floor
            if (autoGainMaxPeak < minAgcThreshold) {
                autoGainMaxPeak = minAgcThreshold;
            }
        }

        // Draw frequency bands appropriately scaled and smoothed
        float columnsPerBand = static_cast<float>(configuredGridWidth) / octaveBands;

        for (int bandIndex = 0; bandIndex < octaveBands; ++bandIndex) {
            float normalizedAmplitude = rawBandAmplitudes[bandIndex] / autoGainMaxPeak;
            
            // 1. Calculate the raw target height based on the grid limit and user's vertical zoom
            float finalCalculatedHeight = (normalizedAmplitude * configuredGridHeight) * verticalZoom;

            // 2. Cap the target to the top of the grid BEFORE applying the decay so bars don't "hang" mathematically off-screen
            finalCalculatedHeight = std::min(finalCalculatedHeight, static_cast<float>(configuredGridHeight));

            // 3. Track falling peaks
            if (finalCalculatedHeight > audioColumnPeakValues[bandIndex]) {
                audioColumnPeakValues[bandIndex] = finalCalculatedHeight;
            } else {
                audioColumnPeakValues[bandIndex] *= PEAK_DECAY_RATE; 
            }

            // 4. Safely clamp output to screen constraints for rendering
            int targetHeight = static_cast<int>(audioColumnPeakValues[bandIndex]);
            targetHeight = std::clamp(targetHeight, 0, configuredGridHeight);

            int startCol = static_cast<int>(bandIndex * columnsPerBand);
            int endCol = static_cast<int>((bandIndex + 1) * columnsPerBand);
            
            for (int col = startCol; col < endCol; ++col) {
                if (col < configuredGridWidth) {
                    activeDisplayBuffer.drawVerticalFrequencyBar(col, targetHeight, activeBarDrawColor);
                }
            }
        }

        // Map internal 2D buffer array to physical OPC vector
        std::fill(outgoingNetworkPayload.begin(), outgoingNetworkPayload.end(), 0);
        for (const auto& physicalPixelTarget : mappedLayout.getPixels()) {
            int mappedPixelXCoord = std::clamp(static_cast<int>(physicalPixelTarget.normX * activeDisplayBuffer.gridWidth), 0, activeDisplayBuffer.gridWidth - 1);
            int mappedPixelYCoord = std::clamp(static_cast<int>(physicalPixelTarget.normY * activeDisplayBuffer.gridHeight), 0, activeDisplayBuffer.gridHeight - 1);
            
            PixelColor selectedPixelColor = activeDisplayBuffer.pixelDataBuffer[mappedPixelYCoord * activeDisplayBuffer.gridWidth + mappedPixelXCoord];
            
            int baseVectorIndex = physicalPixelTarget.opcIndex * COLOR_CHANNELS;
            outgoingNetworkPayload[baseVectorIndex]     = selectedPixelColor.redIntensity;
            outgoingNetworkPayload[baseVectorIndex + 1] = selectedPixelColor.greenIntensity;
            outgoingNetworkPayload[baseVectorIndex + 2] = selectedPixelColor.blueIntensity;
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayload.data(), outgoingNetworkPayload.size());
        
        activeGlobalHueState = (activeGlobalHueState + 1) % (MAX_COLOR_VALUE + 1);
        
        scheduledNextFrameTime += std::chrono::nanoseconds(1000000000 / TARGET_FRAMES_PER_SECOND);
        std::this_thread::sleep_until(scheduledNextFrameTime);
    }
    
    if (activeAudioDeviceHandle) snd_pcm_close(activeAudioDeviceHandle);
    return 0;
}
