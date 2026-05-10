#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include "LEDLayout.h"
#include "OPCClient.h"

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
constexpr float PEAK_DECAY_RATE = 0.92f;
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
    LEDLayout mappedLayout;
    std::string configurationArgument = (argumentCount > 1) ? argumentValues[1] : "8x8";
    mappedLayout.parseArgument(configurationArgument);

    int configuredGridWidth = std::clamp(mappedLayout.logicalWidth > 0 ? mappedLayout.logicalWidth : 16, 1, MAX_GRID_DIMENSION);
    int configuredGridHeight = std::clamp(mappedLayout.logicalHeight > 0 ? mappedLayout.logicalHeight : 16, 1, MAX_GRID_DIMENSION);

    float audioGainMultiplier = 50.0f;
    float minimumFrequencyThreshold = 0.0f;
    float maximumFrequencyThreshold = 20000.0f;

    if (argumentCount > 2) audioGainMultiplier = std::atof(argumentValues[2]);
    if (argumentCount > 3) minimumFrequencyThreshold = std::atof(argumentValues[3]);
    if (argumentCount > 4) maximumFrequencyThreshold = std::atof(argumentValues[4]);

    std::cout << "--- Visualizer Configuration ---\n"
              << "Virtual Grid: " << configuredGridWidth << "x" << configuredGridHeight << "\n"
              << "Gain:      " << audioGainMultiplier << "\n"
              << "EQ Band:   " << minimumFrequencyThreshold << " Hz - " << maximumFrequencyThreshold << " Hz\n"
              << "--------------------------------\n";

    int minimumBinIndex = static_cast<int>(minimumFrequencyThreshold * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
    int maximumBinIndex = static_cast<int>(maximumFrequencyThreshold * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
    
    minimumBinIndex = std::clamp(minimumBinIndex, 0, static_cast<int>(AUDIO_BUFFER_SIZE / 2) - 2);
    maximumBinIndex = std::clamp(maximumBinIndex, minimumBinIndex + 1, static_cast<int>(AUDIO_BUFFER_SIZE / 2) - 1);

    float frequencyBinsPerDisplayColumn = static_cast<float>(maximumBinIndex - minimumBinIndex) / configuredGridWidth;

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
    std::vector<float> audioColumnPeakValues(configuredGridWidth, 0.0f);
    int activeGlobalHueState = 0;

    auto scheduledNextFrameTime = std::chrono::steady_clock::now();
    auto initialApplicationStartTime = std::chrono::steady_clock::now();
    
    // Find highest map index to properly size OPC payload
    int maximumOpcPixelIndex = 0;
    for (const auto& physicalPixel : mappedLayout.pixels) {
        if (physicalPixel.opc_index > maximumOpcPixelIndex) {
            maximumOpcPixelIndex = physicalPixel.opc_index;
        }
    }
    std::vector<uint8_t> outgoingNetworkPayload((maximumOpcPixelIndex + 1) * COLOR_CHANNELS, 0);

    while (true) {
        activeDisplayBuffer.clearBufferData();

        // Audio processing is bypassed smoothly if no hardware is detected
        if (activeAudioDeviceHandle) {
            int capturedAudioFrames = snd_pcm_readi(activeAudioDeviceHandle, capturedAudioRawBuffer.data(), AUDIO_BUFFER_SIZE);
            if (capturedAudioFrames < 0) snd_pcm_recover(activeAudioDeviceHandle, capturedAudioFrames, 0);
            
            for (int audioSampleIndex = 0; audioSampleIndex < AUDIO_BUFFER_SIZE; ++audioSampleIndex) {
                double computedWindowMultiplier = HAMMING_WINDOW_ALPHA - HAMMING_WINDOW_BETA * cos(TWO_PI_CONSTANT * audioSampleIndex / (AUDIO_BUFFER_SIZE - 1));
                fastFourierComplexBuffer[audioSampleIndex] = { (capturedAudioRawBuffer[audioSampleIndex] / PCM_NORMALIZATION_FACTOR) * computedWindowMultiplier, 0.0 };
            }
            performFastFourierTransform(fastFourierComplexBuffer);
        } else {
            // Nullify FFT buffer if no audio handle to prevent drawing garbage bars
            std::fill(fastFourierComplexBuffer.begin(), fastFourierComplexBuffer.end(), std::complex<double>(0.0, 0.0));
        }

        auto currentRenderingTimestamp = std::chrono::steady_clock::now();
        float elapsedExecutionSeconds = std::chrono::duration<float>(currentRenderingTimestamp - initialApplicationStartTime).count();
        
        // This ensures the background still scrolls entirely dependent on system clock duration, bypassing audio availability 
        activeDisplayBuffer.drawScrollingImage(scrollingBackgroundGraphic, loadedImageWidthPixels, loadedImageHeightPixels, elapsedExecutionSeconds * DEFAULT_SCROLL_SPEED);

        PixelColor activeBarDrawColor = convertHsvToRgb(activeGlobalHueState, MAX_COLOR_VALUE, MAX_COLOR_VALUE);
        
        for (int displayColumnIndex = 0; displayColumnIndex < configuredGridWidth; ++displayColumnIndex) {
            int signalBlockStartBin = minimumBinIndex + static_cast<int>(displayColumnIndex * frequencyBinsPerDisplayColumn);
            int signalBlockEndBin = minimumBinIndex + static_cast<int>((displayColumnIndex + 1) * frequencyBinsPerDisplayColumn);
            
            if (signalBlockEndBin <= signalBlockStartBin) signalBlockEndBin = signalBlockStartBin + 1; 

            float aggregatedMagnitudeTotal = 0;
            int totalFrequencyBinsInColumn = signalBlockEndBin - signalBlockStartBin;
            
            for (int frequencyBinIndex = signalBlockStartBin; frequencyBinIndex < signalBlockEndBin; ++frequencyBinIndex) {
                aggregatedMagnitudeTotal += std::abs(fastFourierComplexBuffer[frequencyBinIndex]);
            }
            
            float finalCalculatedAmplitude = (aggregatedMagnitudeTotal / totalFrequencyBinsInColumn) * audioGainMultiplier; 
            if (finalCalculatedAmplitude > audioColumnPeakValues[displayColumnIndex]) {
                audioColumnPeakValues[displayColumnIndex] = finalCalculatedAmplitude;
            } else {
                audioColumnPeakValues[displayColumnIndex] *= PEAK_DECAY_RATE; 
            }

            activeDisplayBuffer.drawVerticalFrequencyBar(displayColumnIndex, static_cast<int>(audioColumnPeakValues[displayColumnIndex] * configuredGridHeight), activeBarDrawColor);
        }

        // Map internal 2D buffer array to physical OPC vector
        std::fill(outgoingNetworkPayload.begin(), outgoingNetworkPayload.end(), 0);
        for (const auto& physicalPixelTarget : mappedLayout.pixels) {
            int mappedPixelXCoord = std::clamp(static_cast<int>(physicalPixelTarget.norm_x * activeDisplayBuffer.gridWidth), 0, activeDisplayBuffer.gridWidth - 1);
            int mappedPixelYCoord = std::clamp(static_cast<int>(physicalPixelTarget.norm_y * activeDisplayBuffer.gridHeight), 0, activeDisplayBuffer.gridHeight - 1);
            
            PixelColor selectedPixelColor = activeDisplayBuffer.pixelDataBuffer[mappedPixelYCoord * activeDisplayBuffer.gridWidth + mappedPixelXCoord];
            
            int baseVectorIndex = physicalPixelTarget.opc_index * COLOR_CHANNELS;
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