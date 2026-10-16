#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cmath>
#include <algorithm>
#include <cctype>

#include "ledlayout.h"
#include "opcclient.h"
#include "fontlibrary.h"


// -------------------------------------------------------------------------
// CONSTANTS
// -------------------------------------------------------------------------
const char* OPC_IP = "127.0.0.1";
const int OPC_PORT = 7890;
const int MAX_MESSAGE_BYTES = 2048;
const int TARGET_FRAMES_PER_SECOND = 60;

// Steps can bring a frame forward, but never closer than this to the one before, so a
// very high speed skips steps instead of spinning the CPU.
const int MAX_FRAMES_PER_SECOND = 200;
const uint8_t OPC_DEFAULT_CHANNEL = 0;

// One blank font column between letters. Letters are trimmed to their own ink first,
// so narrow ones (i, l, 1, !, .) no longer carry the empty columns of a fixed cell.
const int LETTER_SPACING_COLUMNS = 1;

// A grid LED counts as sitting on a whole row and column when its position is within
// this many LEDs of one. Grids and strips always are; the ring never is.
const float GRID_ALIGNMENT_TOLERANCE = 0.01f;

// Samples per axis when an LED does not line up with the font pixels (the ring).
const int SUPERSAMPLE_RESOLUTION = 4;

// On the ring an LED usually straddles a font pixel edge. Coverage below the low mark
// is drawn dark and above the high mark at full strength, so a letter's strokes read
// as lit or unlit rather than as a uniform half-bright smear.
const float EDGE_COVERAGE_LOW = 0.3f;
const float EDGE_COVERAGE_HIGH = 0.7f;

// The pitch estimate compares every LED with every other, so a very long layout is
// measured on a spread-out subset rather than in full.
const int MAX_PITCH_SAMPLE_LEDS = 512;

// -------------------------------------------------------------------------
// DATA STRUCTURES
// -------------------------------------------------------------------------

/**
 * Represents a standard RGB color using 8-bit channels.
 */
struct ColorRgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    ColorRgb() {
        r = 0;
        g = 0;
        b = 0;
    }
};

/**
 * Where one LED looks into the text canvas, in font pixels, before scrolling.
 */
struct LedSample {
    size_t bufferIndex;
    float fontX;
    float fontY;
};

/**
 * How the text is laid onto this particular display.
 *
 * isPixelExact is true for grids and strips tall enough for the font: every font pixel
 * becomes a whole block of LEDs and the text moves one LED per step, so no LED is ever
 * half lit. Otherwise (the ring, or a strip too short for the font) each LED averages
 * the area of the text it covers and the text moves one font pixel per step.
 */
struct RenderPlan {
    bool isPixelExact;
    float footprintFontPixels;
    int stepsPerFontPixel;
    float stepWorldUnits;
    int visibleFontColumns;
    int visibleSteps;
    std::vector<LedSample> samples;
};

// -------------------------------------------------------------------------
// UTILITY CLASSES
// -------------------------------------------------------------------------

class ColorProcessor {
public:
    static ColorRgb fromHex(std::string hexString) {
        ColorRgb resultColor;

        bool isEmptyString = hexString.empty();
        if (isEmptyString == false) {
            bool hasHashPrefix = (hexString.at(0) == '#');
            if (hasHashPrefix == true) {
                hexString = hexString.substr(1);
            }
        }

        bool isCorrectLength = (hexString.length() == 6);
        if (isCorrectLength == true) {
            try {
                std::string redString = hexString.substr(0, 2);
                std::string greenString = hexString.substr(2, 2);
                std::string blueString = hexString.substr(4, 2);

                int redInt = std::stoi(redString, nullptr, 16);
                int greenInt = std::stoi(greenString, nullptr, 16);
                int blueInt = std::stoi(blueString, nullptr, 16);

                resultColor.r = static_cast<uint8_t>(redInt);
                resultColor.g = static_cast<uint8_t>(greenInt);
                resultColor.b = static_cast<uint8_t>(blueInt);
            } catch (...) {
                std::cerr << "Warning: Invalid hex format. Defaulting to white.\n";
                resultColor.r = 255;
                resultColor.g = 255;
                resultColor.b = 255;
            }
        }
        return resultColor;
    }
};

class FontManager {
private:
    int fontWidth;
    int fontHeight;
    int capitalHeight;
    int spaceWidth;
    int fontTypeIdentifier;

public:
    FontManager(std::string choiceString) {
        // capitalHeight is the rows a capital letter fills. The ring centres those rows
        // on its middle LED; the 8x8 font keeps its eighth row for descenders only.
        // spaceWidth is the blank columns of a space, set so the gap between words is
        // clearly wider than the gap between letters at each font size.
        if (choiceString == "3x5") {
            fontTypeIdentifier = 1;
            fontWidth = 3;
            fontHeight = 5;
            capitalHeight = 5;
            spaceWidth = 1;
        } else if (choiceString == "8x8") {
            fontTypeIdentifier = 3;
            fontWidth = 8;
            fontHeight = 8;
            capitalHeight = 7;
            spaceWidth = 3;
        } else {
            fontTypeIdentifier = 2;
            fontWidth = 5;
            fontHeight = 7;
            capitalHeight = 7;
            spaceWidth = 2;
        }
    }

    int getWidth() const {
        return fontWidth;
    }

    int getHeight() const {
        return fontHeight;
    }

    int getCapitalHeight() const {
        return capitalHeight;
    }

    int getSpaceWidth() const {
        return spaceWidth;
    }

    /**
     * Returns one column of a character as a bitmask, bit 0 being the top row.
     * Characters the font does not hold come back blank.
     */
    uint8_t getGlyphColumn(char character, int columnPosition) const {
        int asciiValue = static_cast<int>(static_cast<unsigned char>(character));

        bool isOutsideColumnBounds = (columnPosition < 0) || (columnPosition >= fontWidth);
        if (isOutsideColumnBounds == true) {
            return 0;
        }

        if (fontTypeIdentifier == 1) {
            // The 3x5 font stops at '_', so lowercase is drawn as uppercase and the
            // few symbols past it borrow the nearest shape the font has.
            bool isLowercaseCharacter = (asciiValue >= 'a') && (asciiValue <= 'z');
            if (isLowercaseCharacter == true) {
                asciiValue = asciiValue - ('a' - 'A');
            } else if (asciiValue == '{') {
                asciiValue = '(';
            } else if (asciiValue == '}') {
                asciiValue = ')';
            } else if (asciiValue == '|') {
                asciiValue = '!';
            } else if (asciiValue == '`') {
                asciiValue = '\'';
            } else if (asciiValue == '~') {
                asciiValue = '-';
            }

            int arrayIndex = asciiValue - 32;
            bool isOutsideFontBounds = (arrayIndex < 0) || (arrayIndex > 63);
            if (isOutsideFontBounds == true) {
                return 0;
            }
            return FontLibrary::DATA_3X5[arrayIndex][columnPosition];
        }

        int arrayIndex = asciiValue - 32;
        bool isOutsideFontBounds = (arrayIndex < 0) || (arrayIndex > 94);
        if (isOutsideFontBounds == true) {
            return 0;
        }

        if (fontTypeIdentifier == 3) {
            return FontLibrary::DATA_8X8[arrayIndex][columnPosition];
        }
        return FontLibrary::DATA_5X7[arrayIndex][columnPosition];
    }
};

/**
 * The whole message drawn once at font resolution, one bitmask per column, preceded
 * by a blank gap the width of the display. The canvas wraps, so the gap is what
 * separates the end of the message from its next pass.
 */
class TextBitmap {
private:
    std::vector<uint8_t> canvasColumns;
    int fontHeight;
    int gapWidth;
    int textInkWidth;

    /**
     * Control characters become spaces, and each multi-byte UTF-8 character becomes a
     * single '?', where before every one of its bytes drew a blank cell of its own.
     */
    static std::string normaliseMessage(const std::string& rawMessage) {
        std::string cleanMessage;
        for (size_t byteIndex = 0; byteIndex < rawMessage.length(); byteIndex = byteIndex + 1) {
            unsigned char currentByte = static_cast<unsigned char>(rawMessage[byteIndex]);

            bool isUtf8Continuation = (currentByte >= 0x80) && (currentByte < 0xC0);
            bool isUtf8Lead = (currentByte >= 0xC0);
            bool isControl = (currentByte < 0x20) || (currentByte == 0x7F);

            if (isUtf8Continuation == true) {
                continue;
            } else if (isUtf8Lead == true) {
                cleanMessage.push_back('?');
            } else if (isControl == true) {
                cleanMessage.push_back(' ');
            } else {
                cleanMessage.push_back(static_cast<char>(currentByte));
            }
        }
        return cleanMessage;
    }

public:
    TextBitmap(const std::string& rawMessage, const FontManager& font, int blankGapColumns) {
        fontHeight = font.getHeight();
        gapWidth = std::max(1, blankGapColumns);
        canvasColumns.assign(static_cast<size_t>(gapWidth), 0);

        std::string message = normaliseMessage(rawMessage);
        int fontWidth = font.getWidth();

        for (size_t letterIndex = 0; letterIndex < message.length(); letterIndex = letterIndex + 1) {
            char letter = message[letterIndex];

            // Trim the letter to the columns that hold ink. A letter with none (the
            // space, or a character the font lacks) takes the font's word gap instead.
            int firstInkColumn = -1;
            int lastInkColumn = -1;
            for (int column = 0; column < fontWidth; column = column + 1) {
                uint8_t columnBits = font.getGlyphColumn(letter, column);
                if (columnBits != 0) {
                    if (firstInkColumn == -1) {
                        firstInkColumn = column;
                    }
                    lastInkColumn = column;
                }
            }

            bool isBlankLetter = (firstInkColumn == -1);
            if (isBlankLetter == true) {
                for (int column = 0; column < font.getSpaceWidth(); column = column + 1) {
                    canvasColumns.push_back(0);
                }
            } else {
                for (int column = firstInkColumn; column <= lastInkColumn; column = column + 1) {
                    canvasColumns.push_back(font.getGlyphColumn(letter, column));
                }
            }

            for (int column = 0; column < LETTER_SPACING_COLUMNS; column = column + 1) {
                canvasColumns.push_back(0);
            }
        }

        // The spacing after the last letter is not part of the message's own width,
        // which is what a message too short to scroll is centred by.
        int textWidthWithSpacing = static_cast<int>(canvasColumns.size()) - gapWidth;
        textInkWidth = std::max(0, textWidthWithSpacing - LETTER_SPACING_COLUMNS);
    }

    int getWidth() const {
        return static_cast<int>(canvasColumns.size());
    }

    int getGapWidth() const {
        return gapWidth;
    }

    int getTextInkWidth() const {
        return textInkWidth;
    }

    /**
     * Whether the canvas is lit at a point given in font pixels. X wraps around the
     * canvas; anything above or below the font is dark.
     */
    bool isLitAt(float canvasX, float canvasY) const {
        int rowIndex = static_cast<int>(std::floor(canvasY));
        bool isOutsideRows = (rowIndex < 0) || (rowIndex >= fontHeight);
        if (isOutsideRows == true) {
            return false;
        }

        int canvasWidth = getWidth();
        int columnIndex = static_cast<int>(std::floor(canvasX)) % canvasWidth;
        if (columnIndex < 0) {
            columnIndex = columnIndex + canvasWidth;
        }

        int rowBit = (canvasColumns[static_cast<size_t>(columnIndex)] >> rowIndex) & 0x01;
        return (rowBit == 1);
    }
};

// -------------------------------------------------------------------------
// LAYOUT FITTING
// -------------------------------------------------------------------------

/**
 * Turns a layout coordinate (0 to 1) into a whole LED index, or -1 when the LED does
 * not sit on one, as on the ring.
 */
static int toLatticeIndex(float normalisedPosition, int ledCount) {
    if (ledCount <= 1) {
        return 0;
    }
    float latticePosition = normalisedPosition * static_cast<float>(ledCount - 1);
    float nearestIndex = std::round(latticePosition);
    bool isOnLattice = (std::fabs(latticePosition - nearestIndex) <= GRID_ALIGNMENT_TOLERANCE);
    if (isOnLattice == false) {
        return -1;
    }
    return static_cast<int>(nearestIndex);
}

/**
 * The typical distance between neighbouring LEDs, in layout units: the median of each
 * LED's distance to its nearest neighbour. On the ring this is the spacing between its
 * circles, which is the finest detail the ring can show in any direction.
 */
static float estimateLedPitch(const std::vector<LedPixel>& layoutPixels, float layoutWidth, float layoutHeight) {
    size_t pixelCount = layoutPixels.size();
    if (pixelCount < 2) {
        return 1.0f;
    }

    size_t sampleStride = 1;
    if (pixelCount > static_cast<size_t>(MAX_PITCH_SAMPLE_LEDS)) {
        sampleStride = pixelCount / static_cast<size_t>(MAX_PITCH_SAMPLE_LEDS);
    }

    std::vector<float> nearestDistances;
    for (size_t firstIndex = 0; firstIndex < pixelCount; firstIndex = firstIndex + sampleStride) {
        float firstX = layoutPixels[firstIndex].normX * layoutWidth;
        float firstY = layoutPixels[firstIndex].normY * layoutHeight;
        float nearestDistance = -1.0f;

        for (size_t secondIndex = 0; secondIndex < pixelCount; secondIndex = secondIndex + sampleStride) {
            if (secondIndex == firstIndex) {
                continue;
            }
            float deltaX = layoutPixels[secondIndex].normX * layoutWidth - firstX;
            float deltaY = layoutPixels[secondIndex].normY * layoutHeight - firstY;
            float distance = std::sqrt(deltaX * deltaX + deltaY * deltaY);

            // LEDs mapped to the same spot say nothing about the spacing.
            bool isDistinctPoint = (distance > 0.001f);
            bool isNearer = (nearestDistance < 0.0f) || (distance < nearestDistance);
            if (isDistinctPoint == true && isNearer == true) {
                nearestDistance = distance;
            }
        }

        if (nearestDistance > 0.0f) {
            nearestDistances.push_back(nearestDistance);
        }
    }

    if (nearestDistances.empty() == true) {
        return 1.0f;
    }

    size_t middleIndex = nearestDistances.size() / 2;
    std::nth_element(nearestDistances.begin(), nearestDistances.begin() + middleIndex, nearestDistances.end());
    return nearestDistances[middleIndex];
}

/**
 * Fits the font to the display.
 *
 * Grids and strips get the largest whole-number scale whose height fits, centred
 * vertically, with each LED reading the single font pixel under its centre. The old
 * stretch to the full height by a fractional factor (16 / 7 on the 16x16 grid) gave
 * strokes of uneven width, and it stretched letters upward only, so they came out
 * tall and thin. A whole-number scale keeps letters in proportion and every stroke
 * the same width.
 *
 * The ring gets one font pixel per ring spacing, centred on its middle LED. The
 * horizontal and vertical lines through the centre then fall exactly on font pixels,
 * and the rest of the ring averages the area it covers.
 */
static RenderPlan buildRenderPlan(const LedLayout& hardwareLayout, const FontManager& textFont) {
    RenderPlan plan;
    std::vector<LedPixel> layoutPixels = hardwareLayout.getPixels();

    int layoutWidth = hardwareLayout.getLogicalWidth();
    int layoutHeight = hardwareLayout.getLogicalHeight();
    int fontHeight = textFont.getHeight();

    bool isLatticeLayout = (layoutPixels.empty() == false);
    for (size_t pixelIndex = 0; pixelIndex < layoutPixels.size(); pixelIndex = pixelIndex + 1) {
        int columnIndex = toLatticeIndex(layoutPixels[pixelIndex].normX, layoutWidth);
        int rowIndex = toLatticeIndex(layoutPixels[pixelIndex].normY, layoutHeight);
        if (columnIndex < 0 || rowIndex < 0) {
            isLatticeLayout = false;
            break;
        }
    }

    int wholeNumberScale = layoutHeight / fontHeight;
    bool canDrawPixelExact = (isLatticeLayout == true) && (wholeNumberScale >= 1);

    if (canDrawPixelExact == true) {
        int scaledFontHeight = fontHeight * wholeNumberScale;
        int topMarginRows = (layoutHeight - scaledFontHeight) / 2;
        float scaleFloat = static_cast<float>(wholeNumberScale);

        plan.isPixelExact = true;
        plan.footprintFontPixels = 1.0f / scaleFloat;
        plan.stepsPerFontPixel = wholeNumberScale;
        plan.stepWorldUnits = 1.0f;
        plan.visibleFontColumns = (layoutWidth + wholeNumberScale - 1) / wholeNumberScale;
        plan.visibleSteps = layoutWidth;

        for (size_t pixelIndex = 0; pixelIndex < layoutPixels.size(); pixelIndex = pixelIndex + 1) {
            const LedPixel& activePixel = layoutPixels[pixelIndex];
            int columnIndex = toLatticeIndex(activePixel.normX, layoutWidth);
            int rowIndex = toLatticeIndex(activePixel.normY, layoutHeight);

            LedSample ledSample;
            ledSample.bufferIndex = static_cast<size_t>(activePixel.opcIndex) * 3;
            ledSample.fontX = (static_cast<float>(columnIndex) + 0.5f) / scaleFloat;
            ledSample.fontY = (static_cast<float>(rowIndex - topMarginRows) + 0.5f) / scaleFloat;
            plan.samples.push_back(ledSample);
        }
        return plan;
    }

    // Area-sampled layouts: the ring, or a strip with fewer rows than the font.
    float widthFloat = static_cast<float>(layoutWidth);
    float heightFloat = static_cast<float>(layoutHeight);

    float ledPitch = estimateLedPitch(layoutPixels, widthFloat, heightFloat);
    float fullHeightPixelSize = heightFloat / static_cast<float>(fontHeight);
    float fontPixelSize = std::min(ledPitch, fullHeightPixelSize);
    if (fontPixelSize <= 0.0f) {
        fontPixelSize = 1.0f;
    }

    float minimumX = widthFloat;
    float maximumX = 0.0f;
    float minimumY = heightFloat;
    float maximumY = 0.0f;
    for (size_t pixelIndex = 0; pixelIndex < layoutPixels.size(); pixelIndex = pixelIndex + 1) {
        float worldX = layoutPixels[pixelIndex].normX * widthFloat;
        float worldY = layoutPixels[pixelIndex].normY * heightFloat;
        minimumX = std::min(minimumX, worldX);
        maximumX = std::max(maximumX, worldX);
        minimumY = std::min(minimumY, worldY);
        maximumY = std::max(maximumY, worldY);
    }
    if (minimumX > maximumX) {
        minimumX = 0.0f;
        maximumX = widthFloat;
        minimumY = 0.0f;
        maximumY = heightFloat;
    }

    float centreX = (minimumX + maximumX) / 2.0f;
    float centreY = (minimumY + maximumY) / 2.0f;

    // Font column 0 starts at the display's left edge, and the middle LED falls on
    // the centre of a column; capitals are centred on the middle row.
    float centreToLeftColumns = std::round((centreX - minimumX) / fontPixelSize);
    float capitalCentreRow = static_cast<float>(textFont.getCapitalHeight()) / 2.0f;

    plan.isPixelExact = false;
    plan.footprintFontPixels = 1.0f;
    plan.stepsPerFontPixel = 1;
    plan.stepWorldUnits = fontPixelSize;
    plan.visibleFontColumns = static_cast<int>(std::ceil((maximumX - minimumX) / fontPixelSize)) + 1;
    plan.visibleSteps = plan.visibleFontColumns;

    for (size_t pixelIndex = 0; pixelIndex < layoutPixels.size(); pixelIndex = pixelIndex + 1) {
        const LedPixel& activePixel = layoutPixels[pixelIndex];
        float worldX = activePixel.normX * widthFloat;
        float worldY = activePixel.normY * heightFloat;

        LedSample ledSample;
        ledSample.bufferIndex = static_cast<size_t>(activePixel.opcIndex) * 3;
        ledSample.fontX = (worldX - centreX) / fontPixelSize + 0.5f + centreToLeftColumns;
        ledSample.fontY = (worldY - centreY) / fontPixelSize + capitalCentreRow;
        plan.samples.push_back(ledSample);
    }

    std::cout << "Text drawn at " << fontPixelSize << " layout units per font pixel\n";
    return plan;
}

/**
 * How brightly one LED shows the text, from 0 to 1, at a scroll offset in font pixels.
 */
static float sampleLedIntensity(const TextBitmap& textCanvas, const RenderPlan& plan,
                                const LedSample& ledSample, float scrollFontPixels) {
    float centreX = ledSample.fontX + scrollFontPixels;
    float centreY = ledSample.fontY;

    // Each LED lies inside a single font pixel here, so its centre says it all.
    if (plan.isPixelExact == true) {
        bool isLit = textCanvas.isLitAt(centreX, centreY);
        if (isLit == true) {
            return 1.0f;
        }
        return 0.0f;
    }

    float footprint = plan.footprintFontPixels;
    float boxLeft = centreX - footprint / 2.0f;
    float boxTop = centreY - footprint / 2.0f;
    float resolutionFloat = static_cast<float>(SUPERSAMPLE_RESOLUTION);

    int litSampleCount = 0;
    for (int sampleY = 0; sampleY < SUPERSAMPLE_RESOLUTION; sampleY = sampleY + 1) {
        float fractionalOffsetY = (static_cast<float>(sampleY) + 0.5f) / resolutionFloat;
        float sampleCanvasY = boxTop + footprint * fractionalOffsetY;

        for (int sampleX = 0; sampleX < SUPERSAMPLE_RESOLUTION; sampleX = sampleX + 1) {
            float fractionalOffsetX = (static_cast<float>(sampleX) + 0.5f) / resolutionFloat;
            float sampleCanvasX = boxLeft + footprint * fractionalOffsetX;

            bool isLit = textCanvas.isLitAt(sampleCanvasX, sampleCanvasY);
            if (isLit == true) {
                litSampleCount = litSampleCount + 1;
            }
        }
    }

    float totalSamples = resolutionFloat * resolutionFloat;
    float coverage = static_cast<float>(litSampleCount) / totalSamples;

    // A smoothstep between the two marks keeps a little shading where a stroke only
    // grazes an LED, but drives most LEDs fully on or fully off.
    float edgeRange = EDGE_COVERAGE_HIGH - EDGE_COVERAGE_LOW;
    float rampPosition = (coverage - EDGE_COVERAGE_LOW) / edgeRange;
    rampPosition = std::min(1.0f, std::max(0.0f, rampPosition));
    float sharpenedIntensity = rampPosition * rampPosition * (3.0f - 2.0f * rampPosition);
    return sharpenedIntensity;
}

// -------------------------------------------------------------------------
// MAIN APPLICATION
// -------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // 1. Argument Validation
    if (argc < 6) {
        std::cout << "Usage: " << argv[0] << " <type> <message> <hex> <font_id> <speed>\n";
        std::cout << "Fonts Choices: 3x5, 5x7, 8x8\n";
        return 1;
    }

    const std::string physicalLayoutType = argv[1];
    std::string textMessage = argv[2];
    const std::string hexColorString = argv[3];
    const std::string fontChoiceString = argv[4];

    // std::stof throws on text that is not a number, which ended the program with
    // an abort instead of a message.
    float parsedScrollVelocity = 0.0f;
    try {
        parsedScrollVelocity = std::stof(argv[5]);
    } catch (...) {
        std::cerr << "Error: speed must be a number, e.g. 12.5\n";
        return 1;
    }
    bool isVelocityFinite = std::isfinite(parsedScrollVelocity);
    if (isVelocityFinite == false) {
        std::cerr << "Error: speed must be a finite number.\n";
        return 1;
    }
    const float scrollVelocityPixelsPerSecond = parsedScrollVelocity;

    bool isMessageTooLong = (textMessage.length() > MAX_MESSAGE_BYTES);
    if (isMessageTooLong == true) {
        textMessage = textMessage.substr(0, MAX_MESSAGE_BYTES);
    }

    // 2. Resource Initialization
    LedLayout hardwareLayout;
    hardwareLayout.parseArgument(physicalLayoutType);

    OPCClient networkClient(OPC_IP, OPC_PORT);
    FontManager textFont(fontChoiceString);
    ColorRgb messageTextColor = ColorProcessor::fromHex(hexColorString);

    // 3. Fit the font to the display and draw the message once
    RenderPlan renderPlan = buildRenderPlan(hardwareLayout, textFont);

    // The blank gap is exactly the display's width, so the first letter of the next
    // pass touches the right edge as the last letter of this one leaves the left.
    TextBitmap textCanvas(textMessage, textFont, renderPlan.visibleFontColumns);

    long long stepsPerLoop = static_cast<long long>(textCanvas.getWidth()) * renderPlan.stepsPerFontPixel;

    // 4. Scroll timing
    //
    // The text moves in whole steps (one LED on a grid, one font pixel on the ring),
    // never by fractions: on a display this coarse a fractional position spreads
    // every stroke over two LEDs at half brightness. Speed stays in LEDs per second,
    // as before.
    float stepsPerSecond = std::fabs(scrollVelocityPixelsPerSecond) / renderPlan.stepWorldUnits;
    long long scrollDirection = 1;
    if (scrollVelocityPixelsPerSecond < 0.0f) {
        scrollDirection = -1;
    }

    // Moving text starts on the blank gap and enters from the leading edge. Still text
    // starts on the message itself, centred when it fits on the display.
    long long startingStep = 0;
    bool isStationary = (stepsPerSecond <= 0.0f);
    if (isStationary == true) {
        long long textSteps = static_cast<long long>(textCanvas.getTextInkWidth()) * renderPlan.stepsPerFontPixel;
        long long spareSteps = static_cast<long long>(renderPlan.visibleSteps) - textSteps;
        long long centringSteps = std::max(0LL, spareSteps / 2);
        startingStep = static_cast<long long>(textCanvas.getGapWidth()) * renderPlan.stepsPerFontPixel - centringSteps;
    }

    // Frames still go out at the full rate between steps. The Fadecandy blends each
    // frame into the next over the time between them, so sending only on a step would
    // turn every crisp move into a slow cross-fade. Steps themselves are timed from a
    // fixed start, rather than from the previous frame, so they land evenly instead
    // of on whichever frame happens to come next.
    //
    // steady_clock, not the wall clock: when the Pi sets its time from the network
    // the wall clock jumps, and the scroll jumped (or ran backwards) with it.
    using SteadyClock = std::chrono::steady_clock;
    using FloatSeconds = std::chrono::duration<double>;

    const SteadyClock::time_point scrollStartTime = SteadyClock::now();
    const FloatSeconds frameInterval(1.0 / static_cast<double>(TARGET_FRAMES_PER_SECOND));
    const FloatSeconds minimumFrameInterval(1.0 / static_cast<double>(MAX_FRAMES_PER_SECOND));

    size_t bufferByteSize = renderPlan.samples.size() * 3;
    std::vector<uint8_t> frameNetworkBuffer(bufferByteSize, 0);

    // 5. Main Render Loop
    while (true) {
        SteadyClock::time_point frameTime = SteadyClock::now();
        double elapsedSeconds = std::chrono::duration_cast<FloatSeconds>(frameTime - scrollStartTime).count();

        long long completedSteps = 0;
        if (isStationary == false) {
            completedSteps = static_cast<long long>(std::floor(elapsedSeconds * static_cast<double>(stepsPerSecond)));
        }

        long long currentStep = (startingStep + scrollDirection * completedSteps) % stepsPerLoop;
        if (currentStep < 0) {
            currentStep = currentStep + stepsPerLoop;
        }
        float scrollFontPixels = static_cast<float>(currentStep) / static_cast<float>(renderPlan.stepsPerFontPixel);

        std::fill(frameNetworkBuffer.begin(), frameNetworkBuffer.end(), 0);

        for (size_t sampleIndex = 0; sampleIndex < renderPlan.samples.size(); sampleIndex = sampleIndex + 1) {
            const LedSample& ledSample = renderPlan.samples[sampleIndex];

            float ledIntensity = sampleLedIntensity(textCanvas, renderPlan, ledSample, scrollFontPixels);
            if (ledIntensity <= 0.0f) {
                continue;
            }

            size_t bufferMemoryIndex = ledSample.bufferIndex;
            bool isSafeMemoryWrite = ((bufferMemoryIndex + 2) < frameNetworkBuffer.size());
            if (isSafeMemoryWrite == true) {
                float finalRedFloat = static_cast<float>(messageTextColor.r) * ledIntensity;
                float finalGreenFloat = static_cast<float>(messageTextColor.g) * ledIntensity;
                float finalBlueFloat = static_cast<float>(messageTextColor.b) * ledIntensity;

                frameNetworkBuffer[bufferMemoryIndex] = static_cast<uint8_t>(std::round(finalRedFloat));
                frameNetworkBuffer[bufferMemoryIndex + 1] = static_cast<uint8_t>(std::round(finalGreenFloat));
                frameNetworkBuffer[bufferMemoryIndex + 2] = static_cast<uint8_t>(std::round(finalBlueFloat));
            }
        }

        const uint8_t* payloadPointer = frameNetworkBuffer.data();
        size_t payloadByteSize = frameNetworkBuffer.size();
        networkClient.sendPixelPayload(OPC_DEFAULT_CHANNEL, payloadPointer, payloadByteSize);

        // Wake for the next frame, or sooner if the next step is due first.
        SteadyClock::time_point nextFrameTime = frameTime + std::chrono::duration_cast<SteadyClock::duration>(frameInterval);
        if (isStationary == false) {
            double nextStepSeconds = static_cast<double>(completedSteps + 1) / static_cast<double>(stepsPerSecond);
            SteadyClock::time_point nextStepTime = scrollStartTime + std::chrono::duration_cast<SteadyClock::duration>(FloatSeconds(nextStepSeconds));
            SteadyClock::time_point earliestFrameTime = frameTime + std::chrono::duration_cast<SteadyClock::duration>(minimumFrameInterval);
            if (nextStepTime < nextFrameTime) {
                nextFrameTime = std::max(nextStepTime, earliestFrameTime);
            }
        }
        std::this_thread::sleep_until(nextFrameTime);
    }

    return 0;
}
