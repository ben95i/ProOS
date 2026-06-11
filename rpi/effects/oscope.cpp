#include "ledlayout.h"
#include "opcclient.h"

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cstdlib>
#include <alsa/asoundlib.h>

constexpr int TARGET_FRAMES_PER_SECOND = 60;
constexpr const char* DEFAULT_OPC_HOST = "127.0.0.1"; 
constexpr int DEFAULT_OPC_PORT = 7890;

constexpr unsigned int AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned int AUDIO_BUFFER_SIZE = 1024; 
constexpr unsigned int MAX_ALSA_LATENCY_US = 20000;

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

    void drawLine(int x0, int y0, int x1, int y1, PixelColor color) {
        int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1; 
        int err = dx + dy, e2;

        while (true) {
            applyPixelColor(x0, y0, color);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
};

int main(int argumentCount, char** argumentValues) {
    LedLayout mappedLayout;
    std::string configurationArgument = (argumentCount > 1) ? argumentValues[1] : "8x8";
    mappedLayout.parseArgument(configurationArgument);

    // Increase the fallback dimension for non-logical mapped sets (like rings) to provide 
    // enough internal resolution for the aspect-aware projection logic to sample from.
    int fallbackDimension = 64;
    int configuredGridWidth = std::clamp(mappedLayout.getLogicalWidth() > 0 ? mappedLayout.getLogicalWidth() : fallbackDimension, 1, MAX_GRID_DIMENSION);
    int configuredGridHeight = std::clamp(mappedLayout.getLogicalHeight() > 0 ? mappedLayout.getLogicalHeight() : fallbackDimension, 1, MAX_GRID_DIMENSION);

    float amplitudeDivision = 1.0f;
    if (argumentCount > 2) {
        amplitudeDivision = static_cast<float>(std::atof(argumentValues[2]));
        amplitudeDivision = std::clamp(amplitudeDivision, 0.1f, 100.0f); 
    }

    int frequencyDivision = 1;
    if (argumentCount > 3) {
        frequencyDivision = std::atoi(argumentValues[3]);
        frequencyDivision = std::clamp(frequencyDivision, 1, 32); 
    }

    float smoothingFactor = 0.5f; 
    if (argumentCount > 4) {
        smoothingFactor = static_cast<float>(std::atof(argumentValues[4]));
        smoothingFactor = std::clamp(smoothingFactor, 0.01f, 1.0f);
    }

    float noiseGateThreshold = 75.0f; 
    if (argumentCount > 5) {
        noiseGateThreshold = static_cast<float>(std::atof(argumentValues[5]));
        noiseGateThreshold = std::clamp(noiseGateThreshold, 0.0f, 32767.0f);
    }

    std::cout << "--- LED Oscilloscope ---\n"
              << "Virtual Grid:   " << configuredGridWidth << "x" << configuredGridHeight << "\n"
              << "Amp Division:   " << amplitudeDivision << "x (Vertical Scale 0.1-100)\n"
              << "Freq Division:  " << frequencyDivision << " (Horizontal Zoom 1-32)\n"
              << "Line Smoothing: " << smoothingFactor << " (1.0=Twitchy 0.1=Slow)\n"
              << "Noise Gate:     " << noiseGateThreshold << " (Silence Threshold 0-500)\n";

    DisplayBuffer activeDisplayBuffer(configuredGridWidth, configuredGridHeight);
    OPCClient networkDisplayClient(DEFAULT_OPC_HOST, DEFAULT_OPC_PORT);

    snd_pcm_t* activeAudioDeviceHandle;
    if (snd_pcm_open(&activeAudioDeviceHandle, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "Audio device unavailable. Oscilloscope will run silently (flatline).\n";
        activeAudioDeviceHandle = nullptr;
    } else {
        snd_pcm_set_params(activeAudioDeviceHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_SAMPLE_RATE, 1, MAX_ALSA_LATENCY_US);
    }

    std::vector<int16_t> capturedAudioRawBuffer(AUDIO_BUFFER_SIZE, 0);
    std::vector<float> smoothedWaveform(AUDIO_BUFFER_SIZE, 0.0f);
    
    int activeGlobalHueState = 0;
    float smoothedPeakAmplitude = 200.0f;

    auto scheduledNextFrameTime = std::chrono::steady_clock::now();
    
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
        } else {
            std::fill(capturedAudioRawBuffer.begin(), capturedAudioRawBuffer.end(), 0);
        }

        // --- 1. Find Peak for Auto-Gain ---
        float currentFramePeak = 0.0f;
        for (size_t i = 0; i < AUDIO_BUFFER_SIZE; ++i) {
            float absSample = std::abs(static_cast<float>(capturedAudioRawBuffer[i]));
            if (absSample > currentFramePeak) {
                currentFramePeak = absSample;
            }
        }

        if (currentFramePeak > smoothedPeakAmplitude) {
            smoothedPeakAmplitude = smoothedPeakAmplitude + 0.6f * (currentFramePeak - smoothedPeakAmplitude); 
        } else {
            smoothedPeakAmplitude *= 0.98f; 
        }
        if (smoothedPeakAmplitude < 150.0f) smoothedPeakAmplitude = 150.0f;

        // --- NEW: Silence Detection ---
        bool isSilent = (currentFramePeak <= noiseGateThreshold);

        // --- 2. Oscilloscope Trigger ---
        int triggerIndex = 0;
        
        if (!isSilent) {
            float triggerThreshold = smoothedPeakAmplitude * 0.15f; 
            for (size_t i = 1; i < AUDIO_BUFFER_SIZE / 2; ++i) {
                float prevSample = static_cast<float>(capturedAudioRawBuffer[i-1]);
                float currSample = static_cast<float>(capturedAudioRawBuffer[i]);
                if (prevSample <= triggerThreshold && currSample > triggerThreshold) {
                    triggerIndex = i; 
                    break;
                }
            }
        }

        // --- 3. Map to Display with Temporal Smoothing & Noise Gate ---
        int displaySamples = (AUDIO_BUFFER_SIZE / 2) / frequencyDivision;
        int previousXCoord = 0;
        int previousYCoord = -1;

        for (int i = 0; i < displaySamples; ++i) {
            float targetAmplitude = 0.0f;

            if (!isSilent) {
                int sampleIndex = triggerIndex + i;
                if (sampleIndex >= AUDIO_BUFFER_SIZE) sampleIndex = AUDIO_BUFFER_SIZE - 1;

                float rawSample = static_cast<float>(capturedAudioRawBuffer[sampleIndex]);
                targetAmplitude = (rawSample / smoothedPeakAmplitude) * amplitudeDivision;
                targetAmplitude = std::clamp(targetAmplitude, -1.0f, 1.0f);
            }

            smoothedWaveform[i] += (targetAmplitude - smoothedWaveform[i]) * smoothingFactor;
            
            float finalAmplitude = smoothedWaveform[i];
            finalAmplitude = std::clamp(finalAmplitude, -1.0f, 1.0f);

            int currentXCoord = static_cast<int>((static_cast<float>(i) / (displaySamples - 1)) * (configuredGridWidth - 1));
            
            int currentYCoord = static_cast<int>((-finalAmplitude + 1.0f) / 2.0f * (configuredGridHeight - 1));
            currentYCoord = std::clamp(currentYCoord, 0, configuredGridHeight - 1);

            int columnHueOffset = (currentXCoord * MAX_COLOR_VALUE) / configuredGridWidth;
            int finalHue = (activeGlobalHueState + columnHueOffset) % (MAX_COLOR_VALUE + 1);
            PixelColor activeDrawColor = convertHsvToRgb(finalHue, MAX_COLOR_VALUE, MAX_COLOR_VALUE);

            if (i == 0) {
                activeDisplayBuffer.applyPixelColor(currentXCoord, currentYCoord, activeDrawColor);
            } else {
                activeDisplayBuffer.drawLine(previousXCoord, previousYCoord, currentXCoord, currentYCoord, activeDrawColor);
            }
            
            previousXCoord = currentXCoord;
            previousYCoord = currentYCoord;
        }

        // --- 4. Map internal 2D buffer array to physical OPC vector using mp4toleds.cpp aspect-aware centering ---
        std::fill(outgoingNetworkPayload.begin(), outgoingNetworkPayload.end(), 0);

        float bufferAspectRatio = static_cast<float>(activeDisplayBuffer.gridWidth) / static_cast<float>(activeDisplayBuffer.gridHeight);
        int croppedRenderWidth = activeDisplayBuffer.gridWidth;
        int croppedRenderHeight = activeDisplayBuffer.gridHeight;
        int renderOffsetX = 0;
        int renderOffsetY = 0;

        if (bufferAspectRatio > mappedLayout.getTargetAspect()) {
            croppedRenderWidth = static_cast<int>(activeDisplayBuffer.gridHeight * mappedLayout.getTargetAspect());
            renderOffsetX = (activeDisplayBuffer.gridWidth - croppedRenderWidth) / 2;
        } else {
            croppedRenderHeight = static_cast<int>(activeDisplayBuffer.gridWidth / mappedLayout.getTargetAspect());
            renderOffsetY = (activeDisplayBuffer.gridHeight - croppedRenderHeight) / 2;
        }

        for (const auto& physicalPixelTarget : mappedLayout.getPixels()) {
            int mappedPixelXCoord = renderOffsetX + static_cast<int>(physicalPixelTarget.normX * (croppedRenderWidth - 1));
            int mappedPixelYCoord = renderOffsetY + static_cast<int>(physicalPixelTarget.normY * (croppedRenderHeight - 1));
            
            mappedPixelXCoord = std::clamp(mappedPixelXCoord, 0, activeDisplayBuffer.gridWidth - 1);
            mappedPixelYCoord = std::clamp(mappedPixelYCoord, 0, activeDisplayBuffer.gridHeight - 1);
            
            PixelColor selectedPixelColor = activeDisplayBuffer.pixelDataBuffer[mappedPixelYCoord * activeDisplayBuffer.gridWidth + mappedPixelXCoord];
            
            int baseVectorIndex = physicalPixelTarget.opcIndex * COLOR_CHANNELS;
            outgoingNetworkPayload[baseVectorIndex]     = selectedPixelColor.redIntensity;
            outgoingNetworkPayload[baseVectorIndex + 1] = selectedPixelColor.greenIntensity;
            outgoingNetworkPayload[baseVectorIndex + 2] = selectedPixelColor.blueIntensity;
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayload.data(), outgoingNetworkPayload.size());
        
        activeGlobalHueState = (activeGlobalHueState + 2) % (MAX_COLOR_VALUE + 1);
        
        scheduledNextFrameTime += std::chrono::nanoseconds(1000000000 / TARGET_FRAMES_PER_SECOND);
        std::this_thread::sleep_until(scheduledNextFrameTime);
    }
    
    if (activeAudioDeviceHandle) snd_pcm_close(activeAudioDeviceHandle);
    return 0;
}

