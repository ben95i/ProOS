#pragma once

#include <vector>
#include <string>
#include <mutex>

// --- CONSTANTS ---
constexpr float DEFAULT_ASPECT_RATIO = 1.0f;
constexpr float CENTER_COORDINATE = 0.5f;
constexpr float MAX_RING_RADIUS = 0.48f;
constexpr float FULL_CIRCLE_RADIANS = 2.0f * 3.14159265358979323846f;
constexpr float INVERT_MAX_VALUE = 1.0f;

// Represents a single LED pixel's routing and physical coordinate data
struct LedPixel {
    int opcIndex;
    float normX; 
    float normY; 
};

// Layout configuration for the OpenPixelControl client
class LedLayout {
public:
    LedLayout() = default;
    ~LedLayout() = default;

    // Prevent copying to avoid mutex duplication and memory management issues
    LedLayout(const LedLayout&) = delete;
    LedLayout& operator=(const LedLayout&) = delete;

    /**
     * Parses a string argument to configure the LED layout geometry.
     * * @param layoutArgument The configuration string (e.g., "93ringinv", "8x8grid")
     */
    void parseArgument(const std::string& layoutArgument);

    // --- THREAD-SAFE GETTERS ---
    int getLogicalWidth() const;
    int getLogicalHeight() const;
    float getTargetAspect() const;
    std::vector<LedPixel> getPixels() const;

private:
    int logicalWidth = 0;
    int logicalHeight = 0;
    float targetAspect = DEFAULT_ASPECT_RATIO;
    std::vector<LedPixel> pixels;

    // Mutex to prevent race conditions during layout re-initialization
    mutable std::mutex layoutMutex;

    /**
     * Initializes a standard 2D grid layout.
     */
    void setupGrid(int totalColumns, int totalRows);

    /**
     * Initializes a layout of multiple parallel strips.
     */
    void setupStrips(int totalStrips, int ledsPerStrip);

    /**
     * Initializes the custom 93-LED ring layout.
     * * @param shouldInvertX If true, mirrors the X coordinates for wall projection.
     */
    void setup93Ring(bool shouldInvertX);
};