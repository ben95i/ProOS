// spectro.cpp - microphone spectrum analyser over a scrolling starfield,
// rendered to an Open Pixel Control server.
//
// The starfield is generated in-program: no image asset, no stb_image.
//
// build: g++ -std=c++17 -O2 spectro.cpp ledlayout.cpp opcclient.cpp -o spectro -pthread -lasound
// usage: ./spectro [layout] [zoom] [bands] [minHz] [maxHz] [agcFloor] [scrollRowsPerSec]

#include "ledlayout.h"
#include "opcclient.h"

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

// --- OUTPUT ---
constexpr int TARGET_FRAMES_PER_SECOND = 60;
constexpr const char* DEFAULT_OPC_HOST = "127.0.0.1";
constexpr int DEFAULT_OPC_PORT = 7890;
constexpr int COLOR_CHANNELS = 3;
constexpr int MAX_COLOR_VALUE = 255;
constexpr int MAX_GRID_DIMENSION = 1024;

// --- AUDIO ---
constexpr unsigned int AUDIO_SAMPLE_RATE = 44100;
constexpr int AUDIO_BLOCK_SIZE = 1024;             // must be a power of two: this is the FFT length
constexpr unsigned int ALSA_LATENCY_MICROSECONDS = 100000;  // ~4 blocks of slack in the ring buffer
constexpr float PCM_NORMALIZATION_FACTOR = 32768.0f;
constexpr double HAMMING_WINDOW_ALPHA = 0.54;
constexpr double HAMMING_WINDOW_BETA = 0.46;
constexpr double TWO_PI_CONSTANT = 6.283185307179586476925286766559;

// --- BAR DYNAMICS ---
// Expressed per second, not per frame, so the fall speed no longer depends on
// how fast the render loop happens to be running.
constexpr float PEAK_FRACTION_RETAINED_PER_SECOND = 0.0068f;
constexpr float AGC_FRACTION_RETAINED_PER_SECOND = 0.0068f;
constexpr float HUE_UNITS_PER_SECOND = 50.0f;

// --- RESPONSE CURVE ---
// Music energy falls off with frequency, so on a linear magnitude scale the low
// bars dominate and the high ones only twitch on transients. A decibel window
// fixes that: bar height becomes loudness rather than raw amplitude.
constexpr float DEFAULT_DYNAMIC_RANGE_DB = 45.0f;  // 0 restores the old linear response
// Pink-noise compensation, a gentle lift with frequency (3 dB/octave is flat for
// pink noise). 0 disables it.
constexpr float SPECTRUM_TILT_DB_PER_OCTAVE = 3.0f;
constexpr float DECIBELS_PER_OCTAVE = 6.0205999f;

// --- STARFIELD ---
constexpr float STAR_DENSITY_PER_CELL = 0.17f;
constexpr int STAR_COUNT_MINIMUM = 12;
constexpr int STAR_COUNT_MAXIMUM = 8192;
constexpr float DEFAULT_SCROLL_ROWS_PER_SECOND = 1.6f;
constexpr uint32_t STARFIELD_RANDOM_SEED = 0x5EED1234u;
constexpr float STAR_BRIGHTNESS_MINIMUM = 0.30f;  // faint distant stars
constexpr float STAR_BRIGHTNESS_MAXIMUM = 1.00f;  // near, bright stars
constexpr float STAR_TWINKLE_RATE_MINIMUM = 0.5f;  // radians per second
constexpr float STAR_TWINKLE_RATE_MAXIMUM = 3.2f;
constexpr float STAR_TWINKLE_DEPTH_MINIMUM = 0.15f;  // steady stars
constexpr float STAR_TWINKLE_DEPTH_MAXIMUM = 0.85f;  // hard sparklers
constexpr float STAR_BLUE_TINT_PROBABILITY = 0.35f;

struct PixelColor {
    uint8_t redIntensity, greenIntensity, blueIntensity;
};

// Very dark blue: on WS2812s anything above about 20 stops reading as "night".
constexpr PixelColor NIGHT_SKY_COLOR = {0, 2, 14};
constexpr PixelColor STAR_COLOR_WHITE = {255, 255, 255};
constexpr PixelColor STAR_COLOR_BLUE_WHITE = {200, 220, 255};

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------
// The original ran an infinite loop with no way out, so Ctrl-C left the panel
// stuck on whatever frame it died in and never closed the ALSA handle.
std::atomic<bool> globalKeepRunningFlag{true};

void handleTerminationSignal(int) { globalKeepRunningFlag.store(false); }

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------
PixelColor convertHsvToRgb(int hueValue, int saturationValue, int brightnessValue) {
    if (saturationValue == 0) {
        const uint8_t greyLevel = static_cast<uint8_t>(brightnessValue);
        return {greyLevel, greyLevel, greyLevel};
    }

    constexpr int HSV_SECTOR_SIZE = 43;
    constexpr int HSV_REMAINDER_MULTIPLIER = 6;

    const int colorRegion = hueValue / HSV_SECTOR_SIZE;
    const int colorRemainder = (hueValue - colorRegion * HSV_SECTOR_SIZE) * HSV_REMAINDER_MULTIPLIER;

    const uint8_t brightnessByte = static_cast<uint8_t>(brightnessValue);
    const uint8_t pColor = static_cast<uint8_t>((brightnessValue * (MAX_COLOR_VALUE - saturationValue)) >> 8);
    const uint8_t qColor = static_cast<uint8_t>(
        (brightnessValue * (MAX_COLOR_VALUE - ((saturationValue * colorRemainder) >> 8))) >> 8);
    const uint8_t tColor = static_cast<uint8_t>(
        (brightnessValue * (MAX_COLOR_VALUE - ((saturationValue * (MAX_COLOR_VALUE - colorRemainder)) >> 8))) >> 8);

    switch (colorRegion) {
        case 0:  return {brightnessByte, tColor, pColor};
        case 1:  return {qColor, brightnessByte, pColor};
        case 2:  return {pColor, brightnessByte, tColor};
        case 3:  return {pColor, qColor, brightnessByte};
        case 4:  return {tColor, pColor, brightnessByte};
        default: return {brightnessByte, pColor, qColor};
    }
}

inline uint8_t addSaturating(uint8_t existingValue, float incomingValue) {
    const float summed = static_cast<float>(existingValue) + incomingValue;
    return static_cast<uint8_t>(std::clamp(summed + 0.5f, 0.0f, 255.0f));
}

// ---------------------------------------------------------------------------
// Display buffer
// ---------------------------------------------------------------------------
class DisplayBuffer {
public:
    DisplayBuffer(int targetWidth, int targetHeight)
        : gridWidth(targetWidth),
          gridHeight(targetHeight),
          pixelDataBuffer(static_cast<size_t>(targetWidth) * targetHeight, PixelColor{0, 0, 0}) {}

    int getWidth() const { return gridWidth; }
    int getHeight() const { return gridHeight; }

    const PixelColor& readPixel(int column, int row) const {
        return pixelDataBuffer[static_cast<size_t>(row) * gridWidth + column];
    }

    void fillWith(PixelColor backgroundColor) {
        std::fill(pixelDataBuffer.begin(), pixelDataBuffer.end(), backgroundColor);
    }

    void applyPixelColor(int targetColumn, int targetRow, PixelColor colorToApply) {
        if (!isInsideGrid(targetColumn, targetRow)) return;
        pixelDataBuffer[static_cast<size_t>(targetRow) * gridWidth + targetColumn] = colorToApply;
    }

    void addPixelColor(int targetColumn, int targetRow, PixelColor tint, float weight) {
        if (!isInsideGrid(targetColumn, targetRow) || weight <= 0.0f) return;
        PixelColor& destination = pixelDataBuffer[static_cast<size_t>(targetRow) * gridWidth + targetColumn];
        destination.redIntensity = addSaturating(destination.redIntensity, tint.redIntensity * weight);
        destination.greenIntensity = addSaturating(destination.greenIntensity, tint.greenIntensity * weight);
        destination.blueIntensity = addSaturating(destination.blueIntensity, tint.blueIntensity * weight);
    }

    void drawVerticalFrequencyBar(int targetColumn, int barHeightPixels, PixelColor barDrawColor) {
        const int boundedHeight = std::clamp(barHeightPixels, 0, gridHeight);
        for (int rowPointer = gridHeight - 1; rowPointer >= gridHeight - boundedHeight; --rowPointer) {
            applyPixelColor(targetColumn, rowPointer, barDrawColor);
        }
    }

private:
    bool isInsideGrid(int column, int row) const {
        return column >= 0 && column < gridWidth && row >= 0 && row < gridHeight;
    }

    int gridWidth;
    int gridHeight;
    std::vector<PixelColor> pixelDataBuffer;
};

// ---------------------------------------------------------------------------
// Procedural starfield
// ---------------------------------------------------------------------------
// Replaces the scrolled JPEG. Point-sampling a 1920x391 photo down to a 16-wide
// grid read one source pixel in every 120 across and one in every 24 down, so
// most stars were never sampled at all and the rest flashed in and out as the
// offset moved. Generated stars live in grid space instead, so every one of them
// lands on the panel, wraps seamlessly, and scrolls with sub-row interpolation.
class StarField {
public:
    void generateStars(int gridWidth, int gridHeight) {
        fieldHeightRows = static_cast<float>(gridHeight);

        const int requestedCount =
            static_cast<int>(static_cast<float>(gridWidth) * gridHeight * STAR_DENSITY_PER_CELL);
        const int starCount = std::clamp(requestedCount, STAR_COUNT_MINIMUM, STAR_COUNT_MAXIMUM);

        // Fixed seed: the same sky every run, which makes visual changes
        // reproducible instead of a different field on each restart.
        std::mt19937 randomEngine(STARFIELD_RANDOM_SEED);
        std::uniform_real_distribution<float> unitRange(0.0f, 1.0f);
        std::uniform_int_distribution<int> columnRange(0, std::max(0, gridWidth - 1));

        stars.clear();
        stars.reserve(starCount);
        for (int starIndex = 0; starIndex < starCount; ++starIndex) {
            Star newStar;
            newStar.gridColumn = columnRange(randomEngine);
            newStar.rowPosition = unitRange(randomEngine) * fieldHeightRows;
            newStar.baseBrightness = STAR_BRIGHTNESS_MINIMUM +
                unitRange(randomEngine) * (STAR_BRIGHTNESS_MAXIMUM - STAR_BRIGHTNESS_MINIMUM);
            newStar.twinklePhase = unitRange(randomEngine) * static_cast<float>(TWO_PI_CONSTANT);
            newStar.twinkleRate = STAR_TWINKLE_RATE_MINIMUM +
                unitRange(randomEngine) * (STAR_TWINKLE_RATE_MAXIMUM - STAR_TWINKLE_RATE_MINIMUM);
            newStar.twinkleDepth = STAR_TWINKLE_DEPTH_MINIMUM +
                unitRange(randomEngine) * (STAR_TWINKLE_DEPTH_MAXIMUM - STAR_TWINKLE_DEPTH_MINIMUM);
            newStar.tintColor = (unitRange(randomEngine) < STAR_BLUE_TINT_PROBABILITY)
                                    ? STAR_COLOR_BLUE_WHITE
                                    : STAR_COLOR_WHITE;
            stars.push_back(newStar);
        }
    }

    void renderInto(DisplayBuffer& displayBuffer, float elapsedSeconds, float scrollRowsPerSecond) const {
        displayBuffer.fillWith(NIGHT_SKY_COLOR);
        if (stars.empty() || fieldHeightRows <= 0.0f) return;

        const int gridHeight = displayBuffer.getHeight();
        const float scrollOffsetRows = elapsedSeconds * scrollRowsPerSecond;

        for (const Star& currentStar : stars) {
            // Wrap into [0, height). Stars are stored in grid space, so the seam
            // is exact - the photo's top and bottom edges never matched, which
            // put a visible tear through the panel once per loop.
            float wrappedRow = std::fmod(currentStar.rowPosition + scrollOffsetRows, fieldHeightRows);
            if (wrappedRow < 0.0f) wrappedRow += fieldHeightRows;

            const float twinkleWave = std::sin(elapsedSeconds * currentStar.twinkleRate + currentStar.twinklePhase);
            const float twinkleScale = 1.0f - currentStar.twinkleDepth * (0.5f - 0.5f * twinkleWave);
            const float starBrightness = currentStar.baseBrightness * twinkleScale;
            if (starBrightness <= 0.0f) continue;

            // Split each star across the two rows it straddles so the scroll is a
            // smooth drift rather than a jump from one row to the next.
            const float flooredRow = std::floor(wrappedRow);
            const int upperRow = static_cast<int>(flooredRow);
            const float lowerWeight = wrappedRow - flooredRow;
            const int lowerRow = (upperRow + 1) % gridHeight;

            displayBuffer.addPixelColor(currentStar.gridColumn, upperRow, currentStar.tintColor,
                                        starBrightness * (1.0f - lowerWeight));
            displayBuffer.addPixelColor(currentStar.gridColumn, lowerRow, currentStar.tintColor,
                                        starBrightness * lowerWeight);
        }
    }

    size_t getStarCount() const { return stars.size(); }

private:
    struct Star {
        int gridColumn;
        float rowPosition;
        float baseBrightness;
        float twinklePhase;
        float twinkleRate;
        float twinkleDepth;
        PixelColor tintColor;
    };

    std::vector<Star> stars;
    float fieldHeightRows = 0.0f;
};

// ---------------------------------------------------------------------------
// Spectrum analysis
// ---------------------------------------------------------------------------
class SpectrumAnalyzer {
public:
    SpectrumAnalyzer(int transformSize, int bandCount, float minimumFrequency, float maximumFrequency,
                     unsigned int sampleRate)
        : fftSize(transformSize) {
        buildWindowTable();
        buildTwiddleTables();
        buildBitReversalTable();
        buildBandBinRanges(bandCount, minimumFrequency, maximumFrequency, sampleRate);
        realComponents.assign(fftSize, 0.0f);
        imaginaryComponents.assign(fftSize, 0.0f);
        binMagnitudes.assign(fftSize / 2 + 1, 0.0f);
    }

    int getUsableBandCount() const { return static_cast<int>(bandBinRanges.size()); }

    // Returns one averaged magnitude per band.
    void analyzeBlock(const std::vector<int16_t>& audioSamples, std::vector<float>& bandAmplitudesOut) {
        bandAmplitudesOut.assign(bandBinRanges.size(), 0.0f);
        if (static_cast<int>(audioSamples.size()) < fftSize) return;

        // Remove DC before windowing. Without this, any bias on the capture
        // device lands in bin 0, which the lowest band was reading directly -
        // that pinned the first bar high and, through the AGC, held every other
        // bar down.
        double runningTotal = 0.0;
        for (int sampleIndex = 0; sampleIndex < fftSize; ++sampleIndex) {
            runningTotal += audioSamples[sampleIndex];
        }
        const float meanSampleValue = static_cast<float>(runningTotal / fftSize);

        for (int sampleIndex = 0; sampleIndex < fftSize; ++sampleIndex) {
            const float centredSample =
                (static_cast<float>(audioSamples[sampleIndex]) - meanSampleValue) / PCM_NORMALIZATION_FACTOR;
            realComponents[sampleIndex] = centredSample * windowTable[sampleIndex];
            imaginaryComponents[sampleIndex] = 0.0f;
        }

        runTransform();

        const int usableBinCount = fftSize / 2 + 1;
        for (int binIndex = 0; binIndex < usableBinCount; ++binIndex) {
            binMagnitudes[binIndex] = std::sqrt(realComponents[binIndex] * realComponents[binIndex] +
                                                imaginaryComponents[binIndex] * imaginaryComponents[binIndex]);
        }

        for (size_t bandIndex = 0; bandIndex < bandBinRanges.size(); ++bandIndex) {
            const BinRange& currentRange = bandBinRanges[bandIndex];
            float accumulatedMagnitude = 0.0f;
            for (int binIndex = currentRange.firstBin; binIndex < currentRange.lastBin; ++binIndex) {
                accumulatedMagnitude += binMagnitudes[binIndex];
            }
            bandAmplitudesOut[bandIndex] =
                accumulatedMagnitude / static_cast<float>(currentRange.lastBin - currentRange.firstBin) *
                bandTiltWeights[bandIndex];
        }
    }

private:
    struct BinRange {
        int firstBin, lastBin;  // half-open
    };

    void buildWindowTable() {
        // Was recomputed with a cos() call per sample per frame: 60k transcendental
        // calls a second on a Pi for a table that never changes.
        windowTable.resize(fftSize);
        for (int sampleIndex = 0; sampleIndex < fftSize; ++sampleIndex) {
            windowTable[sampleIndex] = static_cast<float>(
                HAMMING_WINDOW_ALPHA -
                HAMMING_WINDOW_BETA * std::cos(TWO_PI_CONSTANT * sampleIndex / (fftSize - 1)));
        }
    }

    void buildTwiddleTables() {
        twiddleCosine.resize(fftSize / 2);
        twiddleSine.resize(fftSize / 2);
        for (int twiddleIndex = 0; twiddleIndex < fftSize / 2; ++twiddleIndex) {
            const double rotationAngle = -TWO_PI_CONSTANT * twiddleIndex / fftSize;
            twiddleCosine[twiddleIndex] = static_cast<float>(std::cos(rotationAngle));
            twiddleSine[twiddleIndex] = static_cast<float>(std::sin(rotationAngle));
        }
    }

    void buildBitReversalTable() {
        bitReversalTable.resize(fftSize);
        int reversedIndex = 0;
        for (int currentIndex = 1; currentIndex < fftSize; ++currentIndex) {
            int halfSizeBitMask = fftSize >> 1;
            for (; reversedIndex & halfSizeBitMask; halfSizeBitMask >>= 1) reversedIndex ^= halfSizeBitMask;
            reversedIndex ^= halfSizeBitMask;
            bitReversalTable[currentIndex] = reversedIndex;
        }
    }

    // Log-spaced bands, but forced to be strictly increasing and non-overlapping.
    // At 1024 samples and 44.1 kHz a bin is 43 Hz wide, so several of the lowest
    // log bands mapped onto the same bin and those bars moved as one block.
    void buildBandBinRanges(int bandCount, float minimumFrequency, float maximumFrequency,
                            unsigned int sampleRate) {
        const int highestBin = fftSize / 2;
        const float logMinimum = std::log10(minimumFrequency);
        const float logMaximum = std::log10(maximumFrequency);
        const float logStep = (logMaximum - logMinimum) / static_cast<float>(bandCount);

        bandBinRanges.clear();
        bandBinRanges.reserve(bandCount);

        // Seed from the requested minimum frequency, not from bin 1. Seeding at 1
        // handed the first band every bin from DC upward, so it swallowed all the
        // bass regardless of the configured low cutoff. Bin 0 is DC and is never
        // part of a band.
        int previousEndBin = std::max(1, static_cast<int>(minimumFrequency * fftSize / sampleRate));
        for (int bandIndex = 0; bandIndex < bandCount; ++bandIndex) {
            const float endFrequency = std::pow(10.0f, logMinimum + (bandIndex + 1) * logStep);
            int endBin = static_cast<int>(endFrequency * fftSize / sampleRate);

            const int startBin = previousEndBin;
            endBin = std::clamp(endBin, startBin + 1, highestBin);

            // Out of bins: the remaining bands would be empty, so stop here
            // rather than emitting bars that can never move.
            if (startBin >= highestBin) break;

            bandBinRanges.push_back({startBin, endBin});
            previousEndBin = endBin;
        }

        // Precomputed per-band tilt, referenced to the lowest band so the low end
        // keeps its existing scale and the higher bands are lifted.
        bandTiltWeights.assign(bandBinRanges.size(), 1.0f);
        if (SPECTRUM_TILT_DB_PER_OCTAVE != 0.0f && !bandBinRanges.empty()) {
            const float tiltExponent = SPECTRUM_TILT_DB_PER_OCTAVE / DECIBELS_PER_OCTAVE;
            const float binToHertz = static_cast<float>(sampleRate) / fftSize;
            const auto geometricCentre = [&](const BinRange& range) {
                return std::sqrt(static_cast<float>(range.firstBin) * range.lastBin) * binToHertz;
            };
            const float referenceCentre = geometricCentre(bandBinRanges.front());
            for (size_t bandIndex = 0; bandIndex < bandBinRanges.size(); ++bandIndex) {
                bandTiltWeights[bandIndex] =
                    std::pow(geometricCentre(bandBinRanges[bandIndex]) / referenceCentre, tiltExponent);
            }
        }
    }

    void runTransform() {
        for (int currentIndex = 1; currentIndex < fftSize; ++currentIndex) {
            const int reversedIndex = bitReversalTable[currentIndex];
            if (currentIndex < reversedIndex) {
                std::swap(realComponents[currentIndex], realComponents[reversedIndex]);
                std::swap(imaginaryComponents[currentIndex], imaginaryComponents[reversedIndex]);
            }
        }

        for (int sublistLength = 2; sublistLength <= fftSize; sublistLength <<= 1) {
            const int halfSublistLength = sublistLength >> 1;
            const int twiddleStride = fftSize / sublistLength;

            for (int blockStart = 0; blockStart < fftSize; blockStart += sublistLength) {
                for (int elementIndex = 0; elementIndex < halfSublistLength; ++elementIndex) {
                    // Table lookup instead of the running complex multiply the
                    // original used, which accumulated rounding error across the
                    // butterfly and pulled in the slow std::complex multiply path.
                    const int twiddleIndex = elementIndex * twiddleStride;
                    const float twiddleReal = twiddleCosine[twiddleIndex];
                    const float twiddleImaginary = twiddleSine[twiddleIndex];

                    const int evenPosition = blockStart + elementIndex;
                    const int oddPosition = evenPosition + halfSublistLength;

                    const float oddReal = realComponents[oddPosition] * twiddleReal -
                                          imaginaryComponents[oddPosition] * twiddleImaginary;
                    const float oddImaginary = realComponents[oddPosition] * twiddleImaginary +
                                               imaginaryComponents[oddPosition] * twiddleReal;

                    realComponents[oddPosition] = realComponents[evenPosition] - oddReal;
                    imaginaryComponents[oddPosition] = imaginaryComponents[evenPosition] - oddImaginary;
                    realComponents[evenPosition] += oddReal;
                    imaginaryComponents[evenPosition] += oddImaginary;
                }
            }
        }
    }

    int fftSize;
    std::vector<float> windowTable;
    std::vector<float> twiddleCosine, twiddleSine;
    std::vector<int> bitReversalTable;
    std::vector<float> realComponents, imaginaryComponents, binMagnitudes;
    std::vector<BinRange> bandBinRanges;
    std::vector<float> bandTiltWeights;
};

// ---------------------------------------------------------------------------
// Audio capture
// ---------------------------------------------------------------------------
// snd_pcm_readi for 1024 frames at 44.1 kHz blocks for 23 ms, but a 60 fps frame
// budget is 16.7 ms. Calling it inline meant the render loop could never hit its
// target, the frame deadline fell permanently into the past, and any time the
// FFT and draw overran, the capture ring overflowed and produced an XRUN. It now
// runs on its own thread and the renderer takes whatever the newest block is.
class AudioCapture {
public:
    ~AudioCapture() { stopCapture(); }

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;
    AudioCapture() = default;

    bool startCapture(const char* deviceName, int blockSize) {
        blockSizeFrames = blockSize;
        latestAudioBlock.assign(blockSizeFrames, 0);

        int openResult = snd_pcm_open(&audioDeviceHandle, deviceName, SND_PCM_STREAM_CAPTURE, 0);
        if (openResult < 0) {
            std::cerr << "Audio device unavailable (" << snd_strerror(openResult)
                      << "). Running silently; the starfield will still scroll.\n";
            audioDeviceHandle = nullptr;
            return false;
        }

        // The return value was previously ignored, so a failed configuration left
        // every subsequent read failing with no explanation.
        const int parameterResult =
            snd_pcm_set_params(audioDeviceHandle, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED, 1,
                               AUDIO_SAMPLE_RATE, 1, ALSA_LATENCY_MICROSECONDS);
        if (parameterResult < 0) {
            std::cerr << "Could not configure capture device (" << snd_strerror(parameterResult)
                      << "). Running silently.\n";
            snd_pcm_close(audioDeviceHandle);
            audioDeviceHandle = nullptr;
            return false;
        }

        // Reads stay blocking. A non-blocking capture stream can sit in PREPARED
        // returning EAGAIN forever without ever starting, which produces silence
        // with no error at all. Blocking readi returns within about one block
        // period, so the join in stopCapture is still bounded, and running it on
        // its own thread is what keeps the 23 ms read off the 60 fps loop.
        const int prepareResult = snd_pcm_prepare(audioDeviceHandle);
        if (prepareResult < 0) {
            std::cerr << "Could not prepare capture device (" << snd_strerror(prepareResult)
                      << "). Running silently.\n";
            snd_pcm_close(audioDeviceHandle);
            audioDeviceHandle = nullptr;
            return false;
        }

        // Start explicitly rather than relying on the first read to do it.
        const int startResult = snd_pcm_start(audioDeviceHandle);
        if (startResult < 0) {
            std::cerr << "Could not start capture stream (" << snd_strerror(startResult)
                      << "). Running silently.\n";
            snd_pcm_close(audioDeviceHandle);
            audioDeviceHandle = nullptr;
            return false;
        }

        snd_pcm_uframes_t negotiatedBufferSize = 0, negotiatedPeriodSize = 0;
        snd_pcm_get_params(audioDeviceHandle, &negotiatedBufferSize, &negotiatedPeriodSize);
        std::cout << "  Capture:        " << deviceName << ", " << AUDIO_SAMPLE_RATE
                  << " Hz mono, period " << negotiatedPeriodSize << " frames, buffer "
                  << negotiatedBufferSize << " frames\n";

        captureThreadRunning.store(true);
        captureThread = std::thread(&AudioCapture::captureLoop, this);
        return true;
    }

    void stopCapture() {
        captureThreadRunning.store(false);
        if (captureThread.joinable()) captureThread.join();
        if (audioDeviceHandle) {
            snd_pcm_close(audioDeviceHandle);
            audioDeviceHandle = nullptr;
        }
    }

    bool isCapturing() const { return audioDeviceHandle != nullptr; }

    // Diagnostics. blocksCaptured stuck at zero means the device is open but
    // delivering nothing; a nonzero peak level with flat bars would instead point
    // at the analysis or scaling.
    unsigned long getBlocksCaptured() const { return blocksCaptured.load(); }
    unsigned long getRecoveryCount() const { return recoveryCount.load(); }
    int getLastPeakSampleLevel() const { return lastPeakSampleLevel.load(); }

    // Copies out the most recent complete block. Returns false when nothing new
    // has arrived since the last call, so the caller can skip a redundant FFT.
    bool fetchFreshBlock(std::vector<int16_t>& destinationBuffer) {
        std::lock_guard<std::mutex> lock(latestBlockMutex);
        if (!latestBlockIsFresh) return false;
        destinationBuffer = latestAudioBlock;
        latestBlockIsFresh = false;
        return true;
    }

private:
    void captureLoop() {
        std::vector<int16_t> workingBuffer(blockSizeFrames, 0);

        while (captureThreadRunning.load()) {
            int framesFilled = 0;
            bool blockIsComplete = true;

            while (framesFilled < blockSizeFrames && captureThreadRunning.load()) {
                const snd_pcm_sframes_t framesRead =
                    snd_pcm_readi(audioDeviceHandle, workingBuffer.data() + framesFilled,
                                  blockSizeFrames - framesFilled);

                if (framesRead < 0) {
                    // The original recovered but then analysed the buffer anyway,
                    // so an XRUN fed stale samples straight into the FFT.
                    ++recoveryCount;
                    if (snd_pcm_recover(audioDeviceHandle, static_cast<int>(framesRead), 1) < 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    } else {
                        // recover() leaves the stream prepared, not running.
                        snd_pcm_start(audioDeviceHandle);
                    }
                    blockIsComplete = false;
                    break;
                }
                framesFilled += static_cast<int>(framesRead);
            }

            if (blockIsComplete && framesFilled == blockSizeFrames) {
                int peakSampleLevel = 0;
                for (int sampleIndex = 0; sampleIndex < blockSizeFrames; ++sampleIndex) {
                    peakSampleLevel = std::max(peakSampleLevel, std::abs(static_cast<int>(workingBuffer[sampleIndex])));
                }
                lastPeakSampleLevel.store(peakSampleLevel);
                ++blocksCaptured;

                std::lock_guard<std::mutex> lock(latestBlockMutex);
                latestAudioBlock.swap(workingBuffer);
                latestBlockIsFresh = true;
            }
        }
    }

    snd_pcm_t* audioDeviceHandle = nullptr;
    std::thread captureThread;
    std::atomic<bool> captureThreadRunning{false};
    std::atomic<unsigned long> blocksCaptured{0};
    std::atomic<unsigned long> recoveryCount{0};
    std::atomic<int> lastPeakSampleLevel{0};
    int blockSizeFrames = 0;

    std::mutex latestBlockMutex;
    std::vector<int16_t> latestAudioBlock;
    bool latestBlockIsFresh = false;
};

// ---------------------------------------------------------------------------
int main(int argumentCount, char** argumentValues) {
    std::signal(SIGINT, handleTerminationSignal);
    std::signal(SIGTERM, handleTerminationSignal);

    LedLayout mappedLayout;
    const std::string configurationArgument = (argumentCount > 1) ? argumentValues[1] : "8x8";
    mappedLayout.parseArgument(configurationArgument);

    const int configuredGridWidth =
        std::clamp(mappedLayout.getLogicalWidth() > 0 ? mappedLayout.getLogicalWidth() : 16, 1, MAX_GRID_DIMENSION);
    const int configuredGridHeight =
        std::clamp(mappedLayout.getLogicalHeight() > 0 ? mappedLayout.getLogicalHeight() : 16, 1, MAX_GRID_DIMENSION);

    float verticalZoom = 1.0f;
    int requestedBandCount = configuredGridWidth;
    float minimumFrequencyThreshold = 20.0f;
    float maximumFrequencyThreshold = 20000.0f;
    float minimumAgcThreshold = 0.001f;
    float scrollRowsPerSecond = DEFAULT_SCROLL_ROWS_PER_SECOND;
    float dynamicRangeDecibels = DEFAULT_DYNAMIC_RANGE_DB;

    if (argumentCount > 2) verticalZoom = std::atof(argumentValues[2]);
    if (argumentCount > 3) requestedBandCount = std::atoi(argumentValues[3]);
    if (argumentCount > 4) minimumFrequencyThreshold = std::atof(argumentValues[4]);
    if (argumentCount > 5) maximumFrequencyThreshold = std::atof(argumentValues[5]);
    if (argumentCount > 6) minimumAgcThreshold = std::atof(argumentValues[6]);
    if (argumentCount > 7) scrollRowsPerSecond = std::atof(argumentValues[7]);
    if (argumentCount > 8) dynamicRangeDecibels = std::atof(argumentValues[8]);

    verticalZoom = std::max(0.01f, verticalZoom);
    minimumFrequencyThreshold = std::max(1.0f, minimumFrequencyThreshold);
    maximumFrequencyThreshold = std::clamp(maximumFrequencyThreshold, minimumFrequencyThreshold + 1.0f,
                                           static_cast<float>(AUDIO_SAMPLE_RATE / 2));
    requestedBandCount = std::clamp(requestedBandCount, 1, configuredGridWidth);
    minimumAgcThreshold = std::max(0.0001f, minimumAgcThreshold);
    dynamicRangeDecibels = std::max(0.0f, dynamicRangeDecibels);

    SpectrumAnalyzer spectrumAnalyzer(AUDIO_BLOCK_SIZE, requestedBandCount, minimumFrequencyThreshold,
                                      maximumFrequencyThreshold, AUDIO_SAMPLE_RATE);
    const int octaveBands = spectrumAnalyzer.getUsableBandCount();
    if (octaveBands <= 0) {
        std::cerr << "The requested frequency range contains no usable FFT bins.\n";
        return 1;
    }

    StarField starField;
    starField.generateStars(configuredGridWidth, configuredGridHeight);

    std::cout << "--- Visualizer Configuration ---\n"
              << "Usage: ./spectro [Layout] [Vertical Zoom] [Octave Bands] [Min Freq] [Max Freq] "
                 "[Min AGC Threshold] [Scroll Rows/sec] [Dynamic Range dB]\n\n"
              << "  Virtual Grid:   " << configuredGridWidth << "x" << configuredGridHeight << "\n"
              << "  Vertical Zoom:  " << verticalZoom << "x\n"
              << "  Octave Bands:   " << octaveBands << " bars"
              << (octaveBands < requestedBandCount ? " (reduced: not enough FFT bins for the range)" : "") << "\n"
              << "  EQ Band:        " << minimumFrequencyThreshold << " Hz - " << maximumFrequencyThreshold
              << " Hz  (bin width " << static_cast<float>(AUDIO_SAMPLE_RATE) / AUDIO_BLOCK_SIZE << " Hz)\n"
              << "  AGC Floor:      " << minimumAgcThreshold << "\n"
              << "  Response:       "
              << (dynamicRangeDecibels > 0.0f
                      ? std::to_string(static_cast<int>(dynamicRangeDecibels)) + " dB window"
                      : std::string("linear"))
              << ", tilt " << SPECTRUM_TILT_DB_PER_OCTAVE << " dB/octave\n"
              << "  Starfield:      " << starField.getStarCount() << " stars, " << scrollRowsPerSecond
              << " rows/sec\n"
              << "--------------------------------\n";

    DisplayBuffer activeDisplayBuffer(configuredGridWidth, configuredGridHeight);
    OPCClient networkDisplayClient(DEFAULT_OPC_HOST, DEFAULT_OPC_PORT);

    AudioCapture audioCapture;
    audioCapture.startCapture("default", AUDIO_BLOCK_SIZE);

    // Snapshot the layout once. getPixels() locks a mutex and returns the vector
    // by value, and the send loop was calling it every frame.
    const std::vector<LedPixel> layoutPixels = mappedLayout.getPixels();
    if (layoutPixels.empty()) {
        std::cerr << "Layout produced no pixels, nothing to render.\n";
        return 1;
    }

    // The grid cell each LED reads from never changes, so resolve it up front and
    // leave the per-frame send loop as a straight copy.
    int maximumOpcPixelIndex = 0;
    for (const LedPixel& physicalPixel : layoutPixels) {
        maximumOpcPixelIndex = std::max(maximumOpcPixelIndex, physicalPixel.opcIndex);
    }

    struct LedRoute {
        size_t payloadByteOffset;
        int gridColumn, gridRow;
    };
    std::vector<LedRoute> ledRoutes;
    ledRoutes.reserve(layoutPixels.size());
    for (const LedPixel& physicalPixel : layoutPixels) {
        LedRoute route;
        route.payloadByteOffset = static_cast<size_t>(physicalPixel.opcIndex) * COLOR_CHANNELS;
        route.gridColumn =
            std::clamp(static_cast<int>(physicalPixel.normX * configuredGridWidth), 0, configuredGridWidth - 1);
        route.gridRow =
            std::clamp(static_cast<int>(physicalPixel.normY * configuredGridHeight), 0, configuredGridHeight - 1);
        ledRoutes.push_back(route);
    }

    // Every routed index is rewritten each frame and unrouted ones stay dark, so
    // the payload only needs clearing once.
    std::vector<uint8_t> outgoingNetworkPayload(
        static_cast<size_t>(maximumOpcPixelIndex + 1) * COLOR_CHANNELS, 0);

    std::vector<int16_t> capturedAudioBlock(AUDIO_BLOCK_SIZE, 0);
    std::vector<float> rawBandAmplitudes(octaveBands, 0.0f);
    std::vector<float> audioColumnPeakValues(octaveBands, 0.0f);
    float autoGainMaxPeak = minimumAgcThreshold;

    using SteadyClock = std::chrono::steady_clock;
    const auto framePeriod = std::chrono::nanoseconds(1000000000LL / TARGET_FRAMES_PER_SECOND);
    const auto initialApplicationStartTime = SteadyClock::now();
    auto scheduledNextFrameTime = initialApplicationStartTime;
    auto previousFrameTimestamp = initialApplicationStartTime;

    const float columnsPerBand = static_cast<float>(configuredGridWidth) / octaveBands;

    // SPECTRO_DEBUG=1 prints a signal-chain readout once a second, so a silent
    // display can be traced to a stage rather than guessed at.
    const bool debugOutputEnabled = (std::getenv("SPECTRO_DEBUG") != nullptr);
    auto nextDebugReportTime = initialApplicationStartTime;
    int tallestBarThisSecond = 0;

    while (globalKeepRunningFlag.load()) {
        const auto currentRenderingTimestamp = SteadyClock::now();
        const float elapsedExecutionSeconds =
            std::chrono::duration<float>(currentRenderingTimestamp - initialApplicationStartTime).count();
        const float deltaSeconds = std::min(
            std::chrono::duration<float>(currentRenderingTimestamp - previousFrameTimestamp).count(), 0.25f);
        previousFrameTimestamp = currentRenderingTimestamp;

        // Only transform when the capture thread has actually produced something
        // new; blocks arrive at about 43 Hz against a 60 Hz render loop.
        if (audioCapture.fetchFreshBlock(capturedAudioBlock)) {
            spectrumAnalyzer.analyzeBlock(capturedAudioBlock, rawBandAmplitudes);
        }

        float currentFrameMaximum = 0.0f;
        for (float bandAmplitude : rawBandAmplitudes) {
            currentFrameMaximum = std::max(currentFrameMaximum, bandAmplitude);
        }

        // Instant attack as before, but the release is now per second rather than
        // per frame, so the recovery speed no longer depends on the frame rate.
        if (currentFrameMaximum > autoGainMaxPeak) {
            autoGainMaxPeak = currentFrameMaximum;
        } else {
            autoGainMaxPeak *= std::pow(AGC_FRACTION_RETAINED_PER_SECOND, deltaSeconds);
        }
        autoGainMaxPeak = std::max(autoGainMaxPeak, minimumAgcThreshold);

        starField.renderInto(activeDisplayBuffer, elapsedExecutionSeconds, scrollRowsPerSecond);

        // Time based rather than one hue step per frame, so the colour cycle runs
        // at the same speed whatever the loop is doing.
        const int activeGlobalHueState =
            static_cast<int>(elapsedExecutionSeconds * HUE_UNITS_PER_SECOND) % (MAX_COLOR_VALUE + 1);
        const PixelColor activeBarDrawColor =
            convertHsvToRgb(activeGlobalHueState, MAX_COLOR_VALUE, MAX_COLOR_VALUE);

        const float peakDecayFactor = std::pow(PEAK_FRACTION_RETAINED_PER_SECOND, deltaSeconds);

        for (int bandIndex = 0; bandIndex < octaveBands; ++bandIndex) {
            const float amplitudeRatio = rawBandAmplitudes[bandIndex] / autoGainMaxPeak;

            float normalizedAmplitude;
            if (dynamicRangeDecibels > 0.0f) {
                // Map [-range dB, 0 dB] onto [0, 1] of the panel height.
                const float decibelsBelowPeak = 20.0f * std::log10(std::max(amplitudeRatio, 1e-7f));
                normalizedAmplitude = std::clamp(1.0f + decibelsBelowPeak / dynamicRangeDecibels, 0.0f, 1.0f);
            } else {
                normalizedAmplitude = amplitudeRatio;
            }

            float targetBarHeight = normalizedAmplitude * configuredGridHeight * verticalZoom;
            targetBarHeight = std::min(targetBarHeight, static_cast<float>(configuredGridHeight));

            if (targetBarHeight > audioColumnPeakValues[bandIndex]) {
                audioColumnPeakValues[bandIndex] = targetBarHeight;
            } else {
                audioColumnPeakValues[bandIndex] *= peakDecayFactor;
            }

            const int drawnHeight =
                std::clamp(static_cast<int>(audioColumnPeakValues[bandIndex]), 0, configuredGridHeight);
            tallestBarThisSecond = std::max(tallestBarThisSecond, drawnHeight);
            if (drawnHeight <= 0) continue;

            const int startColumn = static_cast<int>(bandIndex * columnsPerBand);
            // The last band now runs to the edge; previously a band count that did
            // not divide the width left a dead column on the right.
            const int endColumn = (bandIndex == octaveBands - 1)
                                      ? configuredGridWidth
                                      : static_cast<int>((bandIndex + 1) * columnsPerBand);

            for (int currentColumn = startColumn; currentColumn < endColumn; ++currentColumn) {
                activeDisplayBuffer.drawVerticalFrequencyBar(currentColumn, drawnHeight, activeBarDrawColor);
            }
        }

        if (debugOutputEnabled && currentRenderingTimestamp >= nextDebugReportTime) {
            nextDebugReportTime = currentRenderingTimestamp + std::chrono::seconds(1);
            float loudestBand = 0.0f;
            for (float bandAmplitude : rawBandAmplitudes) loudestBand = std::max(loudestBand, bandAmplitude);
            std::cerr << "[debug] blocks=" << audioCapture.getBlocksCaptured()
                      << " recoveries=" << audioCapture.getRecoveryCount()
                      << " peakSample=" << audioCapture.getLastPeakSampleLevel() << "/32767"
                      << " loudestBand=" << loudestBand
                      << " agc=" << autoGainMaxPeak
                      << " tallestBar=" << tallestBarThisSecond << "/" << configuredGridHeight << "\n";
            tallestBarThisSecond = 0;
        }

        for (const LedRoute& route : ledRoutes) {
            const PixelColor& selectedPixelColor =
                activeDisplayBuffer.readPixel(route.gridColumn, route.gridRow);
            outgoingNetworkPayload[route.payloadByteOffset] = selectedPixelColor.redIntensity;
            outgoingNetworkPayload[route.payloadByteOffset + 1] = selectedPixelColor.greenIntensity;
            outgoingNetworkPayload[route.payloadByteOffset + 2] = selectedPixelColor.blueIntensity;
        }

        networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayload.data(), outgoingNetworkPayload.size());

        scheduledNextFrameTime += framePeriod;
        const auto rightNow = SteadyClock::now();
        if (scheduledNextFrameTime < rightNow) scheduledNextFrameTime = rightNow;
        std::this_thread::sleep_until(scheduledNextFrameTime);
    }

    std::cout << "\nShutting down, blanking the panel.\n";
    std::fill(outgoingNetworkPayload.begin(), outgoingNetworkPayload.end(), 0);
    networkDisplayClient.sendPixelPayload(0, outgoingNetworkPayload.data(), outgoingNetworkPayload.size());
    audioCapture.stopCapture();
    return 0;
}
