#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <complex>
#include <algorithm>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <alsa/asoundlib.h>

// --- Configuration Constants ---
constexpr int TARGET_FRAMES_PER_SECOND = 60;
constexpr const char* DEFAULT_OPC_HOST = "127.0.0.1"; 
constexpr int OPC_NETWORK_PORT = 7890;

// Audio Configuration
constexpr unsigned int AUDIO_SAMPLE_RATE = 44100;
constexpr unsigned int AUDIO_BUFFER_SIZE = 1024; // Must be power of 2
constexpr unsigned int MAX_ALSA_LATENCY_US = 20000;
constexpr double PCM_NORMALIZATION_FACTOR = 32768.0;

// FFT & Math Constants
constexpr double HAMMING_WINDOW_ALPHA = 0.54;
constexpr double HAMMING_WINDOW_BETA = 0.46;
constexpr double TWO_PI = 2.0 * M_PI;

// Rendering & Color Constants
constexpr float DEFAULT_SCROLL_SPEED = 30.0f;
constexpr float PEAK_DECAY_RATE = 0.92f;
constexpr int MAX_COLOR_VALUE = 255;
constexpr float HSV_SECTOR_SIZE = 43.0f;
constexpr int HSV_REMAINDER_MULTIPLIER = 6;
constexpr int COLOR_CHANNELS = 3;

// OPC Protocol Constants
constexpr int OPC_HEADER_SIZE = 4;
constexpr int OPC_CHANNEL_INDEX = 0;
constexpr int OPC_COMMAND_INDEX = 1;
constexpr int OPC_LENGTH_HIGH_INDEX = 2;
constexpr int OPC_LENGTH_LOW_INDEX = 3;

// Safety Bounds
constexpr int MAX_GRID_DIMENSION = 1024;


struct PixelColor { 
    uint8_t red, green, blue; 
};

// Converts Hue, Saturation, Value (0-255) to RGB
PixelColor convertHsvToRgb(int hue, int saturation, int value) {
    if (saturation == 0) {
        return { static_cast<uint8_t>(value), static_cast<uint8_t>(value), static_cast<uint8_t>(value) };
    }
    
    uint8_t colorRegion = static_cast<uint8_t>(hue / HSV_SECTOR_SIZE);
    uint8_t colorRemainder = (hue - (colorRegion * static_cast<int>(HSV_SECTOR_SIZE))) * HSV_REMAINDER_MULTIPLIER;
    
    uint8_t pColor = (value * (MAX_COLOR_VALUE - saturation)) >> 8;
    uint8_t qColor = (value * (MAX_COLOR_VALUE - ((saturation * colorRemainder) >> 8))) >> 8;
    uint8_t tColor = (value * (MAX_COLOR_VALUE - ((saturation * (MAX_COLOR_VALUE - colorRemainder)) >> 8))) >> 8;
    
    switch (colorRegion) {
        case 0: return {static_cast<uint8_t>(value), tColor, pColor}; 
        case 1: return {qColor, static_cast<uint8_t>(value), pColor};
        case 2: return {pColor, static_cast<uint8_t>(value), tColor}; 
        case 3: return {pColor, qColor, static_cast<uint8_t>(value)};
        case 4: return {tColor, pColor, static_cast<uint8_t>(value)}; 
        default: return {static_cast<uint8_t>(value), pColor, qColor};
    }
}

// --- Dynamic Grid Buffer ---
class DisplayBuffer {
public:
    int gridWidth, gridHeight;
    std::vector<PixelColor> pixelData;

    DisplayBuffer(int width, int height) : gridWidth(width), gridHeight(height), pixelData(width * height, {0, 0, 0}) {}

    void clearBuffer() { 
        std::fill(pixelData.begin(), pixelData.end(), PixelColor{0, 0, 0}); 
    }

    void setPixel(int column, int row, PixelColor targetColor) {
        // Safe Bounds Checking
        if (column >= 0 && column < gridWidth && row >= 0 && row < gridHeight) {
            pixelData[row * gridWidth + column] = targetColor;
        }
    }

    void drawScrollingImage(const std::vector<PixelColor>& imageData, int imageWidth, int imageHeight, float scrollOffset) {
        // Prevent modulo by zero or access violations if image is empty/invalid
        if (imageData.empty() || imageWidth <= 0 || imageHeight <= 0) return;
        
        for (int row = 0; row < gridHeight; ++row) {
            for (int column = 0; column < gridWidth; ++column) {
                // Scale coordinate to image space
                int sourceX = static_cast<int>(column * (static_cast<float>(imageWidth) / gridWidth)) % imageWidth;
                int sourceY = static_cast<int>(row * (static_cast<float>(imageHeight) / gridHeight) + scrollOffset) % imageHeight;
                
                // Wrap around safely for negative scroll values
                if (sourceY < 0) {
                    sourceY += imageHeight; 
                }
                
                setPixel(column, row, imageData[sourceY * imageWidth + sourceX]);
            }
        }
    }

    void drawVerticalBar(int column, int barHeight, PixelColor barColor) {
        int boundedHeight = std::min(barHeight, gridHeight);
        int startRow = gridHeight - 1;
        int endRow = gridHeight - boundedHeight;
        
        for (int row = startRow; row >= endRow && row >= 0; --row) {
            setPixel(column, row, barColor);
        }
    }
};

// --- Threaded OPC Client ---
class OpenPixelControlClient {
    std::string targetHost;
    std::atomic<int> socketDescriptor{-1};
    std::thread connectionThread;
    std::atomic<bool> isRunning{true};
    std::vector<uint8_t> networkPacket;
    std::mutex transmissionMutex;

    void connectionLoop() {
        while (isRunning) {
            if (socketDescriptor == -1) {
                int newSocket = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in serverAddress{AF_INET, htons(OPC_NETWORK_PORT)};
                inet_pton(AF_INET, targetHost.c_str(), &serverAddress.sin_addr);
                
                if (connect(newSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) >= 0) {
                    int noDelayFlag = 1;
                    setsockopt(newSocket, IPPROTO_TCP, TCP_NODELAY, &noDelayFlag, sizeof(int));
                    socketDescriptor = newSocket;
                } else {
                    close(newSocket);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

public:
    OpenPixelControlClient(const std::string& hostAddress) : targetHost(hostAddress) {
        connectionThread = std::thread(&OpenPixelControlClient::connectionLoop, this);
    }
    
    ~OpenPixelControlClient() {
        isRunning = false;
        if (connectionThread.joinable()) {
            connectionThread.join();
        }
        if (socketDescriptor != -1) {
            close(socketDescriptor.load());
        }
    }

    void sendFrame(const DisplayBuffer& currentFrame) {
        std::lock_guard<std::mutex> lock(transmissionMutex);
        int activeSocket = socketDescriptor.load();
        if (activeSocket == -1) return;

        size_t totalPixels = currentFrame.pixelData.size();
        size_t payloadLength = totalPixels * COLOR_CHANNELS;
        
        networkPacket.clear();
        networkPacket.reserve(OPC_HEADER_SIZE + payloadLength);
        
        networkPacket.push_back(OPC_CHANNEL_INDEX); 
        networkPacket.push_back(OPC_COMMAND_INDEX); 
        networkPacket.push_back(static_cast<uint8_t>(payloadLength >> 8));
        networkPacket.push_back(static_cast<uint8_t>(payloadLength & MAX_COLOR_VALUE));

        for (const auto& pixel : currentFrame.pixelData) {
            networkPacket.push_back(pixel.red); 
            networkPacket.push_back(pixel.green); 
            networkPacket.push_back(pixel.blue);
        }

        if (write(activeSocket, networkPacket.data(), networkPacket.size()) < 0) {
            close(activeSocket);
            socketDescriptor = -1;
        }
    }
};

// --- Fast Iterative FFT ---
void performFastFourierTransform(std::vector<std::complex<double>>& complexBuffer) {
    int bufferSize = complexBuffer.size();
    
    // Bit-reversal permutation
    for (int currentIndex = 1, reversedIndex = 0; currentIndex < bufferSize; currentIndex++) {
        int halfSizeBit = bufferSize >> 1;
        for (; reversedIndex & halfSizeBit; halfSizeBit >>= 1) {
            reversedIndex ^= halfSizeBit;
        }
        reversedIndex ^= halfSizeBit;
        
        if (currentIndex < reversedIndex) {
            std::swap(complexBuffer[currentIndex], complexBuffer[reversedIndex]);
        }
    }
    
    // Cooley-Tukey decimation-in-time
    for (int sublistLength = 2; sublistLength <= bufferSize; sublistLength <<= 1) {
        double angleStep = TWO_PI / sublistLength;
        std::complex<double> phaseMultiplier(cos(angleStep), sin(angleStep));
        
        for (int blockStart = 0; blockStart < bufferSize; blockStart += sublistLength) {
            std::complex<double> currentPhase(1.0);
            int halfSublist = sublistLength / 2;
            
            for (int elementIndex = 0; elementIndex < halfSublist; elementIndex++) {
                std::complex<double> evenComponent = complexBuffer[blockStart + elementIndex];
                std::complex<double> oddComponent = complexBuffer[blockStart + elementIndex + halfSublist] * currentPhase;
                
                complexBuffer[blockStart + elementIndex] = evenComponent + oddComponent;
                complexBuffer[blockStart + elementIndex + halfSublist] = evenComponent - oddComponent;
                currentPhase *= phaseMultiplier;
            }
        }
    }
}

int main(int argumentCount, char** argumentValues) {
    // Default Parameters
    int targetGridWidth = 8;
    int targetGridHeight = 8;
    float userGain = 50.0f;
    float minimumFrequency = 0.0f;
    float maximumFrequency = 20000.0f;

    // Parse Command Line Arguments safely
    if (argumentCount > 1) sscanf(argumentValues[1], "%dx%d", &targetGridWidth, &targetGridHeight);
    if (argumentCount > 2) userGain = std::atof(argumentValues[2]);
    if (argumentCount > 3) minimumFrequency = std::atof(argumentValues[3]);
    if (argumentCount > 4) maximumFrequency = std::atof(argumentValues[4]);

    // Input Validation to prevent OOM or logic errors
    targetGridWidth = std::clamp(targetGridWidth, 1, MAX_GRID_DIMENSION);
    targetGridHeight = std::clamp(targetGridHeight, 1, MAX_GRID_DIMENSION);

    std::cout << "--- Visualizer Configuration ---\n"
              << "Grid Size: " << targetGridWidth << "x" << targetGridHeight << "\n"
              << "Gain:      " << userGain << "\n"
              << "EQ Band:   " << minimumFrequency << " Hz - " << maximumFrequency << " Hz\n"
              << "--------------------------------\n";

    // Pre-calculate FFT Bin Mappings with strict boundary constraints
    int minBinIndex = static_cast<int>(minimumFrequency * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
    int maxBinIndex = static_cast<int>(maximumFrequency * AUDIO_BUFFER_SIZE / AUDIO_SAMPLE_RATE);
    
    // Safety clamps to prevent out-of-bounds access on fftBuffer
    minBinIndex = std::clamp(minBinIndex, 0, static_cast<int>(AUDIO_BUFFER_SIZE / 2) - 2);
    maxBinIndex = std::clamp(maxBinIndex, minBinIndex + 1, static_cast<int>(AUDIO_BUFFER_SIZE / 2) - 1);

    float frequencyBinsPerColumn = static_cast<float>(maxBinIndex - minBinIndex) / targetGridWidth;

    DisplayBuffer mainDisplayBuffer(targetGridWidth, targetGridHeight);
    OpenPixelControlClient opcClient(DEFAULT_OPC_HOST);
    
    // Load image
    int loadedImageWidth, loadedImageHeight, loadedImageChannels;
    unsigned char* rawImageData = stbi_load("spectro_bg.jpg", &loadedImageWidth, &loadedImageHeight, &loadedImageChannels, COLOR_CHANNELS);
    std::vector<PixelColor> backgroundGraphic;
    
    if (rawImageData) {
        for (int pixelOffset = 0; pixelOffset < loadedImageWidth * loadedImageHeight * COLOR_CHANNELS; pixelOffset += COLOR_CHANNELS) {
            backgroundGraphic.push_back({rawImageData[pixelOffset], rawImageData[pixelOffset+1], rawImageData[pixelOffset+2]});
        }
        stbi_image_free(rawImageData);
    } else {
        std::cerr << "Warning: spectro_bg.jpg not found. Background will remain blank.\n";
    }

    // ALSA Audio Capture Setup
    snd_pcm_t* audioDeviceHandle;
    if (snd_pcm_open(&audioDeviceHandle, "default", SND_PCM_STREAM_CAPTURE, 0) < 0) {
        std::cerr << "Audio device unavailable. Visualizer will run silently.\n";
        audioDeviceHandle = nullptr;
    } else {
        snd_pcm_set_params(audioDeviceHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1, AUDIO_SAMPLE_RATE, 1, MAX_ALSA_LATENCY_US);
    }

    std::vector<int16_t> rawAudioBuffer(AUDIO_BUFFER_SIZE);
    std::vector<std::complex<double>> fftComplexBuffer(AUDIO_BUFFER_SIZE);
    std::vector<float> columnPeaks(targetGridWidth, 0.0f);
    int currentGlobalHue = 0;

    auto targetNextFrameTime = std::chrono::steady_clock::now();
    auto applicationStartTime = std::chrono::steady_clock::now();

    while (true) {
        mainDisplayBuffer.clearBuffer();

        // 1. Audio Capture & FFT Processing
        if (audioDeviceHandle) {
            int capturedFrames = snd_pcm_readi(audioDeviceHandle, rawAudioBuffer.data(), AUDIO_BUFFER_SIZE);
            if (capturedFrames < 0) {
                snd_pcm_recover(audioDeviceHandle, capturedFrames, 0);
            }
            
            for (int sampleIndex = 0; sampleIndex < AUDIO_BUFFER_SIZE; ++sampleIndex) {
                double windowFunctionMultiplier = HAMMING_WINDOW_ALPHA - HAMMING_WINDOW_BETA * cos(TWO_PI * sampleIndex / (AUDIO_BUFFER_SIZE - 1));
                fftComplexBuffer[sampleIndex] = { (rawAudioBuffer[sampleIndex] / PCM_NORMALIZATION_FACTOR) * windowFunctionMultiplier, 0.0 };
            }
            performFastFourierTransform(fftComplexBuffer);
        }

        // 2. Render Scrolling Background
        auto currentRenderTime = std::chrono::steady_clock::now();
        float elapsedSeconds = std::chrono::duration<float>(currentRenderTime - applicationStartTime).count();
        mainDisplayBuffer.drawScrollingImage(backgroundGraphic, loadedImageWidth, loadedImageHeight, elapsedSeconds * DEFAULT_SCROLL_SPEED);

        // 3. Render EQ Audio Bars
        PixelColor currentBarColor = convertHsvToRgb(currentGlobalHue, MAX_COLOR_VALUE, MAX_COLOR_VALUE);
        
        for (int columnIndex = 0; columnIndex < targetGridWidth; ++columnIndex) {
            int blockStartBin = minBinIndex + static_cast<int>(columnIndex * frequencyBinsPerColumn);
            int blockEndBin = minBinIndex + static_cast<int>((columnIndex + 1) * frequencyBinsPerColumn);
            
            if (blockEndBin <= blockStartBin) {
                blockEndBin = blockStartBin + 1; // Ensure at least 1 bin is processed
            }

            float aggregateMagnitude = 0;
            int totalBinsInColumn = blockEndBin - blockStartBin;
            
            for (int binIndex = blockStartBin; binIndex < blockEndBin; ++binIndex) {
                aggregateMagnitude += std::abs(fftComplexBuffer[binIndex]);
            }
            
            float calculatedAmplitude = (aggregateMagnitude / totalBinsInColumn) * userGain; 
            
            if (calculatedAmplitude > columnPeaks[columnIndex]) {
                columnPeaks[columnIndex] = calculatedAmplitude;
            } else {
                columnPeaks[columnIndex] *= PEAK_DECAY_RATE; 
            }

            mainDisplayBuffer.drawVerticalBar(columnIndex, static_cast<int>(columnPeaks[columnIndex] * targetGridHeight), currentBarColor);
        }

        // 4. Transmit Frame & Regulate Framerate
        opcClient.sendFrame(mainDisplayBuffer);
        
        currentGlobalHue = (currentGlobalHue + 1) % (MAX_COLOR_VALUE + 1);
        
        targetNextFrameTime += std::chrono::nanoseconds(1000000000 / TARGET_FRAMES_PER_SECOND);
        std::this_thread::sleep_until(targetNextFrameTime);
    }
    
    if (audioDeviceHandle) {
        snd_pcm_close(audioDeviceHandle);
    }
    return 0;
}