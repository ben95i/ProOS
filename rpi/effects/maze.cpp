#include "ledlayout.h"
#include "opcclient.h"

#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <thread>
#include <chrono>
#include <algorithm>
#include <random>
#include <limits>
#include <utility>
#include <cstdint>
#include <cstdlib>
#include <cstdio>

// ============================================================================
// CONSTANT DEFINITIONS
// ============================================================================

constexpr const char* OPC_SERVER_ADDRESS = "127.0.0.1";
constexpr int OPC_SERVER_PORT = 7890;
constexpr int MILLISECONDS_PER_SECOND = 1000;

constexpr float HSB_MAXIMUM_VALUE = 100.0f;
constexpr float RGB_MAXIMUM_VALUE = 255.0f;

// Default values applied when the operator omits the matching command line flag.
constexpr const char* DEFAULT_LED_TYPE_ARGUMENT = "8x8";
constexpr int DEFAULT_COMPLEXITY_LEVEL = 7;
constexpr float DEFAULT_SCROLL_SPEED_ROWS_PER_SECOND = 20.0f;
constexpr float DEFAULT_HOLD_DURATION_SECONDS = 2.5f;
constexpr float DEFAULT_SOLVE_DURATION_SECONDS = 10.0f;
constexpr int DEFAULT_TARGET_ANIMATION_FPS = 60;

// Accepted bounds for the operator supplied tuning values.
constexpr int MINIMUM_COMPLEXITY_LEVEL = 1;
constexpr int MAXIMUM_COMPLEXITY_LEVEL = 10;
constexpr float MINIMUM_SCROLL_SPEED_ROWS_PER_SECOND = 0.05f;
constexpr float MAXIMUM_SCROLL_SPEED_ROWS_PER_SECOND = 400.0f;
constexpr float MINIMUM_SOLVE_DURATION_SECONDS = 0.2f;
constexpr float MAXIMUM_SOLVE_DURATION_SECONDS = 600.0f;
constexpr int MINIMUM_ANIMATION_FPS = 1;
constexpr int MAXIMUM_ANIMATION_FPS = 240;

// Maze geometry tuning. The corridor thickness is how many physical LEDs make up
// one passage; thick corridors read well on large panels, one pixel corridors
// pack the maximum amount of structure onto small panels.
constexpr int MAXIMUM_CORRIDOR_THICKNESS = 4;
constexpr int TARGET_MINIMUM_CELLS_ACROSS = 8;
constexpr float STRAIGHTNESS_BIAS_AT_LOWEST_COMPLEXITY = 0.85f;
constexpr float STRAIGHTNESS_BIAS_AT_HIGHEST_COMPLEXITY = 0.0f;
constexpr float BRAID_CHANCE_AT_LOWEST_COMPLEXITY = 0.55f;
constexpr float BRAID_CHANCE_AT_HIGHEST_COMPLEXITY = 0.0f;

// Direction identifiers and their bit flags, stored per maze cell.
constexpr int DIRECTION_NORTH = 0;
constexpr int DIRECTION_EAST = 1;
constexpr int DIRECTION_SOUTH = 2;
constexpr int DIRECTION_WEST = 3;
constexpr int TOTAL_DIRECTION_COUNT = 4;

constexpr uint8_t DIRECTION_FLAG_NORTH = 0x01;
constexpr uint8_t DIRECTION_FLAG_EAST  = 0x02;
constexpr uint8_t DIRECTION_FLAG_SOUTH = 0x04;
constexpr uint8_t DIRECTION_FLAG_WEST  = 0x08;

// Palette contrast guarantees. See ContrastingPaletteGenerator for the reasoning
// behind using a perceptual distance instead of a plain luminance ratio.
constexpr float MINIMUM_PERCEPTUAL_COLOR_DISTANCE = 250.0f;
constexpr float MINIMUM_HUE_SEPARATION = 30.0f;
constexpr float MAXIMUM_HUE_SEPARATION = 72.0f;
constexpr float BRIGHT_ELEMENT_MINIMUM_BRIGHTNESS = 78.0f;
constexpr float BRIGHT_ELEMENT_MAXIMUM_BRIGHTNESS = 100.0f;
constexpr float DARK_ELEMENT_MINIMUM_BRIGHTNESS = 8.0f;
constexpr float DARK_ELEMENT_MAXIMUM_BRIGHTNESS = 24.0f;
constexpr float DARKENING_REFINEMENT_FACTOR = 0.72f;
constexpr float ABSOLUTE_MINIMUM_BRIGHTNESS = 1.5f;
constexpr int MAXIMUM_PALETTE_REFINEMENT_STEPS = 12;
constexpr float INVERTED_SCHEME_PROBABILITY = 0.25f;

// How often the background and the trail trade places, so that the maze sits in a
// field of the opposing colour and the solver cuts through it in near black.
constexpr float TRAIL_BACKGROUND_SWAP_PROBABILITY = 0.25f;

// Trail intensity is stored as a byte per pixel so the head can be drawn at
// fractional coverage; these are its two saturating endpoints.
constexpr uint8_t TRAIL_INTENSITY_ABSENT = 0;
constexpr uint8_t TRAIL_INTENSITY_FULL = 250;

// Tolerance used when grouping normalised coordinates into discrete rows/columns.
constexpr float COORDINATE_CLUSTER_TOLERANCE = 0.0001f;

// Grid and strip layouts land one LED on every cell of the grid their coordinates
// imply. Ring and other scattered layouts do not: their LEDs trace a curve through
// the plane, so ranking their distinct coordinates would invent a huge, almost
// empty grid. When occupancy falls below this threshold the mapper switches to a
// coarser square grid sized so that neighbouring LEDs land on neighbouring cells.
constexpr double SPARSE_LAYOUT_OCCUPANCY_THRESHOLD = 0.35;
constexpr double SPARSE_RESOLUTION_FACTOR = 3.0;
constexpr int MINIMUM_SPARSE_RESOLUTION = 4;
constexpr int MAXIMUM_SPARSE_RESOLUTION = 256;

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

/**
 * @brief The three colours used to draw one maze.
 */
struct MazeColorPalette {
    PixelColorState wallColorState;
    PixelColorState backgroundColorState;
    PixelColorState trailColorState;
};

/**
 * @brief An axis aligned block of display pixels.
 */
struct PixelRectangle {
    int originX;
    int originY;
    int spanX;
    int spanY;
};

/**
 * @brief Where the maze sits on the display and how big its cells are.
 * Shared by the generator and the animator so both agree on pixel positions
 * without either one storing a per pixel copy of the layout.
 */
struct MazeGeometry {
    int corridorThickness = 1;
    int cellColumnCount = 1;
    int cellRowCount = 1;
    int horizontalRenderOffset = 0;
    int verticalRenderOffset = 0;

    /**
     * @brief Left edge of a cell's interior block. Accepts out of range columns so
     * the virtual cells just outside the entrance and exit resolve correctly.
     */
    int resolveCellOriginX(int cellColumn) const {
        return horizontalRenderOffset + corridorThickness + (cellColumn * 2 * corridorThickness);
    }

    int resolveCellOriginY(int cellRow) const {
        return verticalRenderOffset + corridorThickness + (cellRow * 2 * corridorThickness);
    }

    /**
     * @brief How many one pixel slices the trail crosses per cell to cell move:
     * the gap in the wall, then the destination cell's interior.
     */
    int resolveSlicesPerMove() const {
        return 2 * corridorThickness;
    }
};

/**
 * @brief One step of the solver's walk: the cell it starts from, the direction it
 * travels, and whether it is pushing forward into new ground or backing out.
 *
 * Only the origin cell and direction are stored. The destination is implied, which
 * keeps the whole recorded walk compact enough to sit in cache on a small board.
 */
struct TrailMove {
    int16_t cellColumn;
    int16_t cellRow;
    uint8_t directionIdentifier;
    uint8_t moveIsRetreat;
};

/**
 * @brief A generated maze: its wall bitmap, its geometry, and the recorded walk
 * a depth first solver takes from the entrance to the exit.
 */
struct GeneratedMaze {
    std::vector<uint8_t> wallOccupancyMask;
    std::vector<TrailMove> trailMoveSequence;
    MazeGeometry geometry;
};

/**
 * @brief A generated maze paired with the palette it is drawn in.
 */
struct RenderedMazeFrame {
    GeneratedMaze generatedMaze;
    MazeColorPalette assignedColorPalette;
};

/**
 * @brief Precomputed link between one grid cell and the payload bytes it drives.
 * Building these once at startup removes all per frame index arithmetic and all
 * per frame bounds checking from the hot path.
 */
struct PixelRenderTarget {
    int32_t gridFlatIndex;
    int32_t payloadByteOffset;
};

// ============================================================================
// COLOR UTILITY CLASS
// ============================================================================

/**
 * @brief Handles conversion of colors between different mathematical models.
 * Kept identical in behaviour to the converter used by the noise effect so that
 * both programs produce matching colours for matching inputs.
 */
class ColorConverter {
public:
    /**
     * @brief Converts Hue, Saturation, and Brightness directly into RGB intensities.
     */
    static PixelColorState calculateHsbToRgb(float inputHueAngle, float inputSaturationPercentage,
                                             float inputBrightnessPercentage) {

        float hueModulo = fmodf(inputHueAngle, HSB_MAXIMUM_VALUE);
        if (hueModulo < 0.0f) {
            hueModulo = hueModulo + HSB_MAXIMUM_VALUE;
        }
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
        float sectorFloorValue = floorf(colorSectorPosition);
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

    /**
     * @brief Mixes two colours by a 0..255 weight, entirely in integer arithmetic.
     * This runs once per partially lit pixel per frame, so it deliberately avoids
     * floating point and division by anything but a compile time constant.
     */
    static PixelColorState blendColorStates(const PixelColorState& originColorState,
                                            const PixelColorState& targetColorState,
                                            uint8_t blendWeight) {

        int weightAsInteger = static_cast<int>(blendWeight);

        int originRed = static_cast<int>(originColorState.redIntensity);
        int originGreen = static_cast<int>(originColorState.greenIntensity);
        int originBlue = static_cast<int>(originColorState.blueIntensity);

        int redDifference = static_cast<int>(targetColorState.redIntensity) - originRed;
        int greenDifference = static_cast<int>(targetColorState.greenIntensity) - originGreen;
        int blueDifference = static_cast<int>(targetColorState.blueIntensity) - originBlue;

        PixelColorState blendedColorState;
        blendedColorState.redIntensity = static_cast<uint8_t>(originRed + ((redDifference * weightAsInteger) / 255));
        blendedColorState.greenIntensity = static_cast<uint8_t>(originGreen + ((greenDifference * weightAsInteger) / 255));
        blendedColorState.blueIntensity = static_cast<uint8_t>(originBlue + ((blueDifference * weightAsInteger) / 255));
        return blendedColorState;
    }
};

// ============================================================================
// CONTRASTING PALETTE GENERATOR CLASS
// ============================================================================

/**
 * @brief Produces a random wall colour, a random background colour, and a trail
 * colour, all of which stay clearly distinguishable from one another.
 *
 * Wall against background is enforced along two independent axes:
 *
 *  1. Brightness separation. One element is always drawn from the bright band and
 *     the other from the dark band, so the ratio of their HSB values is never below
 *     roughly three to one. Because the OPC server applies its own gamma curve on
 *     the way to the LEDs, that ratio widens considerably in emitted light.
 *
 *  2. Perceptual distance. A weighted RGB distance ("redmean") is evaluated on the
 *     final byte values and the darker element is pushed further down until the
 *     pair clears the threshold. A plain luminance ratio is deliberately not used
 *     here: photometric weighting scores saturated blue so low that a blue wall on
 *     any dark background could never pass, even though it is highly legible on an
 *     emissive panel.
 *
 * The trail colour is then placed to oppose both of them at once.
 */
class ContrastingPaletteGenerator {
private:
    std::mt19937& sharedRandomEngine;

    float sampleUniformRange(float lowerBound, float upperBound) {
        std::uniform_real_distribution<float> rangeDistribution(lowerBound, upperBound);
        return rangeDistribution(sharedRandomEngine);
    }

    /**
     * @brief Weighted RGB difference approximating perceived colour distance.
     */
    static float calculatePerceptualDistance(const PixelColorState& firstColorState,
                                             const PixelColorState& secondColorState) {

        float firstRedValue = static_cast<float>(firstColorState.redIntensity);
        float secondRedValue = static_cast<float>(secondColorState.redIntensity);
        float averagedRedLevel = (firstRedValue + secondRedValue) * 0.5f;

        float differenceRed = firstRedValue - secondRedValue;
        float differenceGreen = static_cast<float>(firstColorState.greenIntensity)
                              - static_cast<float>(secondColorState.greenIntensity);
        float differenceBlue = static_cast<float>(firstColorState.blueIntensity)
                             - static_cast<float>(secondColorState.blueIntensity);

        float redWeight = 2.0f + (averagedRedLevel / 256.0f);
        float greenWeight = 4.0f;
        float blueWeight = 2.0f + ((255.0f - averagedRedLevel) / 256.0f);

        float weightedRedTerm = redWeight * differenceRed * differenceRed;
        float weightedGreenTerm = greenWeight * differenceGreen * differenceGreen;
        float weightedBlueTerm = blueWeight * differenceBlue * differenceBlue;

        return sqrtf(weightedRedTerm + weightedGreenTerm + weightedBlueTerm);
    }

    /**
     * @brief Picks a third colour that opposes both the wall and the background.
     *
     * The hue is placed in the middle of the wider of the two arcs separating the
     * existing hues, which puts it as far from both as the colour wheel allows. A
     * small sweep of saturation and brightness variants is then scored by its
     * weakest distance to either existing colour, so the trail stays legible
     * whether the maze it explores is bright on dark or dark on bright.
     */
    PixelColorState selectOpposingTrailColor(float wallHueAngle, float backgroundHueAngle,
                                             const PixelColorState& wallColorState,
                                             const PixelColorState& backgroundColorState) {

        float forwardArcSize = fmodf((backgroundHueAngle - wallHueAngle) + HSB_MAXIMUM_VALUE, HSB_MAXIMUM_VALUE);
        float backwardArcSize = HSB_MAXIMUM_VALUE - forwardArcSize;

        float opposingHueAngle;
        if (forwardArcSize >= backwardArcSize) {
            opposingHueAngle = fmodf(wallHueAngle + (forwardArcSize * 0.5f), HSB_MAXIMUM_VALUE);
        } else {
            opposingHueAngle = fmodf(backgroundHueAngle + (backwardArcSize * 0.5f), HSB_MAXIMUM_VALUE);
        }

        static constexpr float candidateHueOffsets[3] = { 0.0f, -7.0f, 7.0f };
        static constexpr float candidateSaturationLevels[3] = { 100.0f, 70.0f, 30.0f };
        static constexpr float candidateBrightnessLevels[3] = { 100.0f, 80.0f, 55.0f };

        PixelColorState bestCandidateColor = ColorConverter::calculateHsbToRgb(opposingHueAngle, 100.0f, 100.0f);
        float bestCandidateScore = -1.0f;

        for (int hueIterator = 0; hueIterator < 3; ++hueIterator) {
            float candidateHueAngle = opposingHueAngle + candidateHueOffsets[hueIterator];

            for (int saturationIterator = 0; saturationIterator < 3; ++saturationIterator) {
                for (int brightnessIterator = 0; brightnessIterator < 3; ++brightnessIterator) {

                    PixelColorState candidateColorState = ColorConverter::calculateHsbToRgb(
                        candidateHueAngle,
                        candidateSaturationLevels[saturationIterator],
                        candidateBrightnessLevels[brightnessIterator]);

                    float distanceFromWall = calculatePerceptualDistance(candidateColorState, wallColorState);
                    float distanceFromBackground = calculatePerceptualDistance(candidateColorState, backgroundColorState);
                    float weakestMeasuredDistance = std::min(distanceFromWall, distanceFromBackground);

                    if (weakestMeasuredDistance > bestCandidateScore) {
                        bestCandidateScore = weakestMeasuredDistance;
                        bestCandidateColor = candidateColorState;
                    }
                }
            }
        }

        return bestCandidateColor;
    }

public:
    explicit ContrastingPaletteGenerator(std::mt19937& randomEngineReference)
        : sharedRandomEngine(randomEngineReference) {}

    /**
     * @brief Rolls a new palette and refines it until every pair is legible.
     */
    MazeColorPalette generateContrastingPalette() {
        float wallHueAngle = sampleUniformRange(0.0f, HSB_MAXIMUM_VALUE);

        float requestedHueSeparation = sampleUniformRange(MINIMUM_HUE_SEPARATION, MAXIMUM_HUE_SEPARATION);
        float backgroundHueAngle = fmodf(wallHueAngle + requestedHueSeparation, HSB_MAXIMUM_VALUE);

        float wallSaturationLevel = sampleUniformRange(65.0f, 100.0f);
        float backgroundSaturationLevel = sampleUniformRange(45.0f, 100.0f);

        bool useInvertedScheme = (sampleUniformRange(0.0f, 1.0f) < INVERTED_SCHEME_PROBABILITY);

        float wallBrightnessLevel;
        float backgroundBrightnessLevel;

        if (useInvertedScheme == false) {
            // Common case: a bright maze drawn over a deep, tinted background.
            wallBrightnessLevel = sampleUniformRange(BRIGHT_ELEMENT_MINIMUM_BRIGHTNESS,
                                                     BRIGHT_ELEMENT_MAXIMUM_BRIGHTNESS);
            backgroundBrightnessLevel = sampleUniformRange(DARK_ELEMENT_MINIMUM_BRIGHTNESS,
                                                           DARK_ELEMENT_MAXIMUM_BRIGHTNESS);
        } else {
            // Occasional case: a dark maze silhouetted against a glowing background.
            wallBrightnessLevel = sampleUniformRange(DARK_ELEMENT_MINIMUM_BRIGHTNESS,
                                                     DARK_ELEMENT_MAXIMUM_BRIGHTNESS);
            backgroundBrightnessLevel = sampleUniformRange(BRIGHT_ELEMENT_MINIMUM_BRIGHTNESS,
                                                           BRIGHT_ELEMENT_MAXIMUM_BRIGHTNESS);
        }

        PixelColorState resolvedWallColor = ColorConverter::calculateHsbToRgb(
            wallHueAngle, wallSaturationLevel, wallBrightnessLevel);
        PixelColorState resolvedBackgroundColor = ColorConverter::calculateHsbToRgb(
            backgroundHueAngle, backgroundSaturationLevel, backgroundBrightnessLevel);

        for (int refinementStep = 0; refinementStep < MAXIMUM_PALETTE_REFINEMENT_STEPS; ++refinementStep) {
            float measuredDistance = calculatePerceptualDistance(resolvedWallColor, resolvedBackgroundColor);
            if (measuredDistance >= MINIMUM_PERCEPTUAL_COLOR_DISTANCE) {
                break;
            }

            // Deepen whichever element is already the darker of the two. Driving it
            // toward black strictly increases the distance, so this always converges.
            if (wallBrightnessLevel >= backgroundBrightnessLevel) {
                backgroundBrightnessLevel = std::max(ABSOLUTE_MINIMUM_BRIGHTNESS,
                                                     backgroundBrightnessLevel * DARKENING_REFINEMENT_FACTOR);
                wallBrightnessLevel = std::min(HSB_MAXIMUM_VALUE, wallBrightnessLevel * 1.05f);
            } else {
                wallBrightnessLevel = std::max(ABSOLUTE_MINIMUM_BRIGHTNESS,
                                               wallBrightnessLevel * DARKENING_REFINEMENT_FACTOR);
                backgroundBrightnessLevel = std::min(HSB_MAXIMUM_VALUE, backgroundBrightnessLevel * 1.05f);
            }

            resolvedWallColor = ColorConverter::calculateHsbToRgb(
                wallHueAngle, wallSaturationLevel, wallBrightnessLevel);
            resolvedBackgroundColor = ColorConverter::calculateHsbToRgb(
                backgroundHueAngle, backgroundSaturationLevel, backgroundBrightnessLevel);
        }

        MazeColorPalette assembledPalette;
        assembledPalette.wallColorState = resolvedWallColor;
        assembledPalette.backgroundColorState = resolvedBackgroundColor;
        assembledPalette.trailColorState = selectOpposingTrailColor(
            wallHueAngle, backgroundHueAngle, resolvedWallColor, resolvedBackgroundColor);

        // Some of the time, hand the background and the trail each other's colour.
        // The maze then sits in a field of the opposing colour and the solver cuts
        // through it in near black, which reads as an inversion of the usual look
        // without disturbing any of the contrast guarantees: the trail colour was
        // already chosen to stand apart from BOTH of the other two, so exchanging
        // it with the background leaves all three pairwise distances untouched.
        //
        // Only worth doing when the background is the dark element of the pair,
        // since that is exactly what makes the swapped trail read as black.
        if (useInvertedScheme == false &&
            sampleUniformRange(0.0f, 1.0f) < TRAIL_BACKGROUND_SWAP_PROBABILITY) {
            std::swap(assembledPalette.backgroundColorState, assembledPalette.trailColorState);
        }

        return assembledPalette;
    }
};

// ============================================================================
// DISPLAY GRID MAPPER CLASS
// ============================================================================

/**
 * @brief Translates the layout's normalised pixel coordinates into discrete
 * column and row indices so that grid based artwork can be drawn on any panel.
 *
 * Rather than assuming a particular normalisation convention, the distinct
 * horizontal and vertical coordinates present in the layout are collected and
 * ranked. The number of distinct values becomes the grid size, which means the
 * maze automatically fits whichever LED type was selected on the command line.
 *
 * The mapper produces a flat list of render targets and then keeps nothing else,
 * so no per pixel bookkeeping survives into the render loop.
 */
class DisplayGridMapper {
private:
    int resolvedGridWidth = 1;
    int resolvedGridHeight = 1;
    const char* selectedMappingModeName = "aligned";

    /**
     * @brief Groups nearly identical coordinates into ordered buckets.
     */
    static std::vector<float> buildOrderedCoordinateBuckets(std::vector<float>& scratchCoordinateValues) {
        std::sort(scratchCoordinateValues.begin(), scratchCoordinateValues.end());

        std::vector<float> distinctBucketValues;
        distinctBucketValues.reserve(scratchCoordinateValues.size());

        for (float candidateValue : scratchCoordinateValues) {
            if (distinctBucketValues.empty() ||
                (candidateValue - distinctBucketValues.back()) > COORDINATE_CLUSTER_TOLERANCE) {
                distinctBucketValues.push_back(candidateValue);
            }
        }

        distinctBucketValues.shrink_to_fit();
        return distinctBucketValues;
    }

    /**
     * @brief Binary searches for the bucket a coordinate belongs to.
     */
    static int resolveBucketIndex(const std::vector<float>& distinctBucketValues, float queryValue) {
        auto upperIterator = std::lower_bound(distinctBucketValues.begin(), distinctBucketValues.end(), queryValue);

        if (upperIterator == distinctBucketValues.begin()) {
            return 0;
        }
        if (upperIterator == distinctBucketValues.end()) {
            return static_cast<int>(distinctBucketValues.size()) - 1;
        }

        auto lowerIterator = upperIterator - 1;
        if ((queryValue - *lowerIterator) <= (*upperIterator - queryValue)) {
            return static_cast<int>(lowerIterator - distinctBucketValues.begin());
        }
        return static_cast<int>(upperIterator - distinctBucketValues.begin());
    }

public:
    int getGridWidth() const { return resolvedGridWidth; }
    int getGridHeight() const { return resolvedGridHeight; }
    const char* getMappingModeName() const { return selectedMappingModeName; }

    /**
     * @brief Derives the grid from the layout and returns one render target per
     * LED whose OPC address actually fits inside the payload buffer.
     *
     * The pixel list is taken by reference because LedLayout::getPixels() returns
     * a fresh, mutex guarded copy on every call. It is fetched once, at startup,
     * and nothing in the render loop ever touches the layout again.
     */
    std::vector<PixelRenderTarget> buildRenderTargets(const std::vector<LedPixel>& layoutPixelCollection,
                                                      size_t payloadSizeInBytes,
                                                      bool verticalAxisIsFlipped,
                                                      int requestedGridWidth,
                                                      int requestedGridHeight) {

        size_t layoutPixelCount = layoutPixelCollection.size();

        std::vector<float> horizontalScratchValues;
        std::vector<float> verticalScratchValues;
        horizontalScratchValues.reserve(layoutPixelCount);
        verticalScratchValues.reserve(layoutPixelCount);

        for (const LedPixel& inspectedPixel : layoutPixelCollection) {
            horizontalScratchValues.push_back(inspectedPixel.normX);
            verticalScratchValues.push_back(inspectedPixel.normY);
        }

        std::vector<float> horizontalBuckets = buildOrderedCoordinateBuckets(horizontalScratchValues);
        std::vector<float> verticalBuckets = buildOrderedCoordinateBuckets(verticalScratchValues);

        int clusteredGridWidth = std::max(1, static_cast<int>(horizontalBuckets.size()));
        int clusteredGridHeight = std::max(1, static_cast<int>(verticalBuckets.size()));

        // Aligned mapping ranks the distinct coordinates, which reproduces a grid
        // or strip layout exactly, whatever normalisation convention it uses.
        // Proportional mapping quantises the unit square instead, which is what a
        // scattered layout such as the ring needs.
        bool useProportionalMapping = false;

        if (requestedGridWidth > 0 && requestedGridHeight > 0) {
            resolvedGridWidth = requestedGridWidth;
            resolvedGridHeight = requestedGridHeight;
            useProportionalMapping = true;
            selectedMappingModeName = "forced";
        } else {
            double clusteredCellCount = static_cast<double>(clusteredGridWidth)
                                      * static_cast<double>(clusteredGridHeight);
            double measuredOccupancy = static_cast<double>(layoutPixelCount) / clusteredCellCount;

            if (measuredOccupancy >= SPARSE_LAYOUT_OCCUPANCY_THRESHOLD) {
                resolvedGridWidth = clusteredGridWidth;
                resolvedGridHeight = clusteredGridHeight;
                selectedMappingModeName = "aligned";
            } else {
                // Scattered: size the grid to the LED count so consecutive LEDs
                // along the curve land on adjacent cells rather than scattering
                // across a mostly empty plane.
                double derivedResolution = std::sqrt(static_cast<double>(layoutPixelCount))
                                         * SPARSE_RESOLUTION_FACTOR;
                int clampedResolution = std::clamp(static_cast<int>(std::lround(derivedResolution)),
                                                   MINIMUM_SPARSE_RESOLUTION, MAXIMUM_SPARSE_RESOLUTION);
                resolvedGridWidth = clampedResolution;
                resolvedGridHeight = clampedResolution;
                useProportionalMapping = true;
                selectedMappingModeName = "scattered";
            }
        }

        std::vector<PixelRenderTarget> assembledRenderTargets;
        assembledRenderTargets.reserve(layoutPixelCount);

        for (const LedPixel& inspectedPixel : layoutPixelCollection) {
            int derivedColumnIndex;
            int derivedRowIndex;

            if (useProportionalMapping == true) {
                derivedColumnIndex = static_cast<int>(inspectedPixel.normX * static_cast<float>(resolvedGridWidth));
                derivedRowIndex = static_cast<int>(inspectedPixel.normY * static_cast<float>(resolvedGridHeight));
            } else {
                derivedColumnIndex = resolveBucketIndex(horizontalBuckets, inspectedPixel.normX);
                derivedRowIndex = resolveBucketIndex(verticalBuckets, inspectedPixel.normY);
            }

            derivedColumnIndex = std::clamp(derivedColumnIndex, 0, resolvedGridWidth - 1);
            derivedRowIndex = std::clamp(derivedRowIndex, 0, resolvedGridHeight - 1);

            if (verticalAxisIsFlipped == true) {
                derivedRowIndex = (resolvedGridHeight - 1) - derivedRowIndex;
            }

            // Discard any LED whose OPC address cannot be written safely. Doing it
            // here means the render loop never has to test a single index again.
            long long payloadByteOffset = static_cast<long long>(inspectedPixel.opcIndex) * 3;
            if (payloadByteOffset < 0) {
                continue;
            }
            if ((payloadByteOffset + 2) >= static_cast<long long>(payloadSizeInBytes)) {
                continue;
            }

            PixelRenderTarget assembledTarget;
            assembledTarget.gridFlatIndex = (derivedRowIndex * resolvedGridWidth) + derivedColumnIndex;
            assembledTarget.payloadByteOffset = static_cast<int32_t>(payloadByteOffset);
            assembledRenderTargets.push_back(assembledTarget);
        }

        assembledRenderTargets.shrink_to_fit();
        return assembledRenderTargets;
    }
};

// ============================================================================
// MAZE STRUCTURE GENERATOR CLASS
// ============================================================================

/**
 * @brief Builds a random maze, rasterises it, and records the walk a depth first
 * solver takes through it.
 *
 * Carving is a randomised depth first search, which always yields a fully
 * connected maze. The requested complexity level steers three properties:
 *
 *   - Corridor thickness. Low complexity widens passages, producing a few bold
 *     chambers. High complexity narrows them to a single LED, packing in the
 *     maximum number of cells the panel can physically show.
 *   - Straightness bias. Low complexity strongly favours continuing in the current
 *     direction, giving long readable corridors. High complexity picks freely,
 *     giving a dense tangle of turns.
 *   - Braiding. Low complexity knocks out a share of the dead ends, opening loops
 *     and simplifying the structure. High complexity leaves every dead end intact.
 *
 * The solver walk recorded afterwards is a second, independent depth first search
 * that starts at the entrance and stops the moment it steps onto the exit cell.
 * Every push and every pop is recorded in order, so the animator can replay the
 * exploration - including the dead ends it has to back out of - with no search
 * work at all in the render loop.
 */
class MazeStructureGenerator {
private:
    std::mt19937& sharedRandomEngine;

    // Reused between mazes so steady state generation performs no fresh allocation.
    std::vector<uint8_t> openDirectionMaskPerCell;
    std::vector<uint8_t> visitedFlagPerCell;
    std::vector<int32_t> carveTraversalStack;
    std::vector<int32_t> carveArrivalDirectionStack;
    std::vector<int32_t> solveTraversalStack;

    float sampleUnitInterval() {
        std::uniform_real_distribution<float> unitDistribution(0.0f, 1.0f);
        return unitDistribution(sharedRandomEngine);
    }

    int sampleIntegerRange(int lowerInclusive, int upperInclusive) {
        if (upperInclusive <= lowerInclusive) {
            return lowerInclusive;
        }
        std::uniform_int_distribution<int> integerDistribution(lowerInclusive, upperInclusive);
        return integerDistribution(sharedRandomEngine);
    }

    static int resolveHorizontalStep(int directionIdentifier) {
        if (directionIdentifier == DIRECTION_EAST) return 1;
        if (directionIdentifier == DIRECTION_WEST) return -1;
        return 0;
    }

    static int resolveVerticalStep(int directionIdentifier) {
        if (directionIdentifier == DIRECTION_NORTH) return -1;
        if (directionIdentifier == DIRECTION_SOUTH) return 1;
        return 0;
    }

    static uint8_t resolveDirectionFlag(int directionIdentifier) {
        switch (directionIdentifier) {
            case DIRECTION_NORTH: return DIRECTION_FLAG_NORTH;
            case DIRECTION_EAST:  return DIRECTION_FLAG_EAST;
            case DIRECTION_SOUTH: return DIRECTION_FLAG_SOUTH;
            default:              return DIRECTION_FLAG_WEST;
        }
    }

    static int resolveOppositeDirection(int directionIdentifier) {
        return (directionIdentifier + 2) % TOTAL_DIRECTION_COUNT;
    }

    /**
     * @brief Chooses how many LEDs make up one corridor for this panel size.
     */
    static int resolveCorridorThickness(int gridWidth, int gridHeight, int complexityLevel) {
        int smallestDisplayDimension = std::min(gridWidth, gridHeight);
        int thicknessAllowedByPanel = smallestDisplayDimension / TARGET_MINIMUM_CELLS_ACROSS;
        int upperThicknessBound = std::clamp(thicknessAllowedByPanel, 1, MAXIMUM_CORRIDOR_THICKNESS);

        float normalizedComplexity = static_cast<float>(complexityLevel - MINIMUM_COMPLEXITY_LEVEL)
                                   / static_cast<float>(MAXIMUM_COMPLEXITY_LEVEL - MINIMUM_COMPLEXITY_LEVEL);

        float thicknessSpan = 1.0f - static_cast<float>(upperThicknessBound);
        float interpolatedThickness = static_cast<float>(upperThicknessBound) + (thicknessSpan * normalizedComplexity);
        int selectedThickness = std::max(1, static_cast<int>(lroundf(interpolatedThickness)));

        // Never let a thick corridor collapse the maze into a single cell.
        while (selectedThickness > 1) {
            int columnsAtThisThickness = (gridWidth - selectedThickness) / (2 * selectedThickness);
            int rowsAtThisThickness = (gridHeight - selectedThickness) / (2 * selectedThickness);
            if (columnsAtThisThickness >= 2 && rowsAtThisThickness >= 2) {
                break;
            }
            selectedThickness = selectedThickness - 1;
        }

        return selectedThickness;
    }

    /**
     * @brief Randomised depth first carving, with a straightness bias.
     */
    void carveCellGraph(int cellColumnCount, int cellRowCount, float straightnessBias) {
        int totalCellCount = cellColumnCount * cellRowCount;

        openDirectionMaskPerCell.assign(static_cast<size_t>(totalCellCount), 0);
        visitedFlagPerCell.assign(static_cast<size_t>(totalCellCount), 0);
        carveTraversalStack.clear();
        carveArrivalDirectionStack.clear();
        carveTraversalStack.reserve(static_cast<size_t>(totalCellCount));
        carveArrivalDirectionStack.reserve(static_cast<size_t>(totalCellCount));

        int startingCellColumn = sampleIntegerRange(0, cellColumnCount - 1);
        int startingCellRow = sampleIntegerRange(0, cellRowCount - 1);
        int startingFlatIndex = (startingCellRow * cellColumnCount) + startingCellColumn;

        visitedFlagPerCell[static_cast<size_t>(startingFlatIndex)] = 1;
        carveTraversalStack.push_back(startingFlatIndex);
        carveArrivalDirectionStack.push_back(-1);

        int candidateDirections[TOTAL_DIRECTION_COUNT];

        while (carveTraversalStack.empty() == false) {
            int currentFlatIndex = carveTraversalStack.back();
            int arrivalDirection = carveArrivalDirectionStack.back();

            int currentColumn = currentFlatIndex % cellColumnCount;
            int currentRow = currentFlatIndex / cellColumnCount;

            int candidateDirectionCount = 0;
            bool continuingStraightIsPossible = false;

            for (int directionIdentifier = 0; directionIdentifier < TOTAL_DIRECTION_COUNT; ++directionIdentifier) {
                int neighbourColumn = currentColumn + resolveHorizontalStep(directionIdentifier);
                int neighbourRow = currentRow + resolveVerticalStep(directionIdentifier);

                if (neighbourColumn < 0 || neighbourColumn >= cellColumnCount) continue;
                if (neighbourRow < 0 || neighbourRow >= cellRowCount) continue;

                int neighbourFlatIndex = (neighbourRow * cellColumnCount) + neighbourColumn;
                if (visitedFlagPerCell[static_cast<size_t>(neighbourFlatIndex)] != 0) continue;

                candidateDirections[candidateDirectionCount] = directionIdentifier;
                candidateDirectionCount = candidateDirectionCount + 1;

                if (directionIdentifier == arrivalDirection) {
                    continuingStraightIsPossible = true;
                }
            }

            if (candidateDirectionCount == 0) {
                carveTraversalStack.pop_back();
                carveArrivalDirectionStack.pop_back();
                continue;
            }

            int chosenDirection;
            if (continuingStraightIsPossible == true && sampleUnitInterval() < straightnessBias) {
                chosenDirection = arrivalDirection;
            } else {
                chosenDirection = candidateDirections[sampleIntegerRange(0, candidateDirectionCount - 1)];
            }

            int carvedColumn = currentColumn + resolveHorizontalStep(chosenDirection);
            int carvedRow = currentRow + resolveVerticalStep(chosenDirection);
            int carvedFlatIndex = (carvedRow * cellColumnCount) + carvedColumn;

            openDirectionMaskPerCell[static_cast<size_t>(currentFlatIndex)] |= resolveDirectionFlag(chosenDirection);
            openDirectionMaskPerCell[static_cast<size_t>(carvedFlatIndex)] |=
                resolveDirectionFlag(resolveOppositeDirection(chosenDirection));

            visitedFlagPerCell[static_cast<size_t>(carvedFlatIndex)] = 1;
            carveTraversalStack.push_back(carvedFlatIndex);
            carveArrivalDirectionStack.push_back(chosenDirection);
        }
    }

    /**
     * @brief Removes a share of the dead ends, opening loops into the maze.
     */
    void braidDeadEnds(int cellColumnCount, int cellRowCount, float braidChance) {
        if (braidChance <= 0.0f) {
            return;
        }

        int closedDirections[TOTAL_DIRECTION_COUNT];

        for (int cellRowIterator = 0; cellRowIterator < cellRowCount; ++cellRowIterator) {
            for (int cellColumnIterator = 0; cellColumnIterator < cellColumnCount; ++cellColumnIterator) {

                int inspectedFlatIndex = (cellRowIterator * cellColumnCount) + cellColumnIterator;
                uint8_t inspectedMask = openDirectionMaskPerCell[static_cast<size_t>(inspectedFlatIndex)];

                int openSideCount = 0;
                for (int directionIdentifier = 0; directionIdentifier < TOTAL_DIRECTION_COUNT; ++directionIdentifier) {
                    if ((inspectedMask & resolveDirectionFlag(directionIdentifier)) != 0) {
                        openSideCount = openSideCount + 1;
                    }
                }

                if (openSideCount != 1) continue;
                if (sampleUnitInterval() >= braidChance) continue;

                int closedDirectionCount = 0;
                for (int directionIdentifier = 0; directionIdentifier < TOTAL_DIRECTION_COUNT; ++directionIdentifier) {
                    if ((inspectedMask & resolveDirectionFlag(directionIdentifier)) != 0) continue;

                    int neighbourColumn = cellColumnIterator + resolveHorizontalStep(directionIdentifier);
                    int neighbourRow = cellRowIterator + resolveVerticalStep(directionIdentifier);

                    if (neighbourColumn < 0 || neighbourColumn >= cellColumnCount) continue;
                    if (neighbourRow < 0 || neighbourRow >= cellRowCount) continue;

                    closedDirections[closedDirectionCount] = directionIdentifier;
                    closedDirectionCount = closedDirectionCount + 1;
                }

                if (closedDirectionCount == 0) continue;

                int braidDirection = closedDirections[sampleIntegerRange(0, closedDirectionCount - 1)];
                int braidColumn = cellColumnIterator + resolveHorizontalStep(braidDirection);
                int braidRow = cellRowIterator + resolveVerticalStep(braidDirection);
                int braidFlatIndex = (braidRow * cellColumnCount) + braidColumn;

                openDirectionMaskPerCell[static_cast<size_t>(inspectedFlatIndex)] |= resolveDirectionFlag(braidDirection);
                openDirectionMaskPerCell[static_cast<size_t>(braidFlatIndex)] |=
                    resolveDirectionFlag(resolveOppositeDirection(braidDirection));
            }
        }
    }

    /**
     * @brief Records the depth first walk the solver takes, entrance to exit.
     *
     * Exactly one head exists at any moment: the top of the stack. Stepping into an
     * unexplored neighbour appends a forward move; running out of options appends a
     * retreat move back toward the parent. The walk stops the instant the head
     * arrives at the exit cell, so nothing beyond the discovered route is recorded.
     */
    void recordSolverWalk(int cellColumnCount, int cellRowCount,
                          int entranceCellColumn, int exitCellColumn,
                          std::vector<TrailMove>& recordedMoveSequence) {

        int totalCellCount = cellColumnCount * cellRowCount;
        int entranceFlatIndex = entranceCellColumn;
        int exitFlatIndex = ((cellRowCount - 1) * cellColumnCount) + exitCellColumn;

        visitedFlagPerCell.assign(static_cast<size_t>(totalCellCount), 0);
        solveTraversalStack.clear();
        solveTraversalStack.reserve(static_cast<size_t>(totalCellCount));

        // clear() keeps the capacity, so after the first couple of mazes this
        // sequence never allocates again.
        recordedMoveSequence.clear();
        recordedMoveSequence.reserve(static_cast<size_t>(totalCellCount));

        // Step in through the gap cut in the top wall.
        recordedMoveSequence.push_back({ static_cast<int16_t>(entranceCellColumn),
                                         static_cast<int16_t>(-1),
                                         static_cast<uint8_t>(DIRECTION_SOUTH), 0 });

        visitedFlagPerCell[static_cast<size_t>(entranceFlatIndex)] = 1;
        solveTraversalStack.push_back(entranceFlatIndex);

        int candidateDirections[TOTAL_DIRECTION_COUNT];

        while (solveTraversalStack.empty() == false) {
            int currentFlatIndex = solveTraversalStack.back();
            if (currentFlatIndex == exitFlatIndex) {
                break;
            }

            int currentColumn = currentFlatIndex % cellColumnCount;
            int currentRow = currentFlatIndex / cellColumnCount;
            uint8_t currentMask = openDirectionMaskPerCell[static_cast<size_t>(currentFlatIndex)];

            int candidateDirectionCount = 0;
            for (int directionIdentifier = 0; directionIdentifier < TOTAL_DIRECTION_COUNT; ++directionIdentifier) {
                if ((currentMask & resolveDirectionFlag(directionIdentifier)) == 0) continue;

                int neighbourColumn = currentColumn + resolveHorizontalStep(directionIdentifier);
                int neighbourRow = currentRow + resolveVerticalStep(directionIdentifier);

                if (neighbourColumn < 0 || neighbourColumn >= cellColumnCount) continue;
                if (neighbourRow < 0 || neighbourRow >= cellRowCount) continue;

                int neighbourFlatIndex = (neighbourRow * cellColumnCount) + neighbourColumn;
                if (visitedFlagPerCell[static_cast<size_t>(neighbourFlatIndex)] != 0) continue;

                candidateDirections[candidateDirectionCount] = directionIdentifier;
                candidateDirectionCount = candidateDirectionCount + 1;
            }

            if (candidateDirectionCount == 0) {
                // Dead end, or every way onward already explored: back out one cell.
                if (solveTraversalStack.size() < 2) {
                    break;
                }

                int parentFlatIndex = solveTraversalStack[solveTraversalStack.size() - 2];
                int parentColumn = parentFlatIndex % cellColumnCount;
                int parentRow = parentFlatIndex / cellColumnCount;

                int retreatDirection = DIRECTION_NORTH;
                if (parentColumn > currentColumn)      retreatDirection = DIRECTION_EAST;
                else if (parentColumn < currentColumn) retreatDirection = DIRECTION_WEST;
                else if (parentRow > currentRow)       retreatDirection = DIRECTION_SOUTH;

                recordedMoveSequence.push_back({ static_cast<int16_t>(currentColumn),
                                                 static_cast<int16_t>(currentRow),
                                                 static_cast<uint8_t>(retreatDirection), 1 });
                solveTraversalStack.pop_back();
                continue;
            }

            int chosenDirection = candidateDirections[sampleIntegerRange(0, candidateDirectionCount - 1)];
            int steppedColumn = currentColumn + resolveHorizontalStep(chosenDirection);
            int steppedRow = currentRow + resolveVerticalStep(chosenDirection);
            int steppedFlatIndex = (steppedRow * cellColumnCount) + steppedColumn;

            recordedMoveSequence.push_back({ static_cast<int16_t>(currentColumn),
                                             static_cast<int16_t>(currentRow),
                                             static_cast<uint8_t>(chosenDirection), 0 });

            visitedFlagPerCell[static_cast<size_t>(steppedFlatIndex)] = 1;
            solveTraversalStack.push_back(steppedFlatIndex);
        }

        // Step out through the gap cut in the bottom wall.
        recordedMoveSequence.push_back({ static_cast<int16_t>(exitCellColumn),
                                         static_cast<int16_t>(cellRowCount - 1),
                                         static_cast<uint8_t>(DIRECTION_SOUTH), 0 });
    }

public:
    explicit MazeStructureGenerator(std::mt19937& randomEngineReference)
        : sharedRandomEngine(randomEngineReference) {}

    /**
     * @brief Generates one maze, rasterises it, and records its solver walk.
     */
    void buildMaze(int gridWidth, int gridHeight, int complexityLevel, GeneratedMaze& destinationMaze) {
        int corridorThickness = resolveCorridorThickness(gridWidth, gridHeight, complexityLevel);

        float normalizedComplexity = static_cast<float>(complexityLevel - MINIMUM_COMPLEXITY_LEVEL)
                                   / static_cast<float>(MAXIMUM_COMPLEXITY_LEVEL - MINIMUM_COMPLEXITY_LEVEL);

        float straightnessSpan = STRAIGHTNESS_BIAS_AT_HIGHEST_COMPLEXITY - STRAIGHTNESS_BIAS_AT_LOWEST_COMPLEXITY;
        float straightnessBias = STRAIGHTNESS_BIAS_AT_LOWEST_COMPLEXITY + (straightnessSpan * normalizedComplexity);

        float braidSpan = BRAID_CHANCE_AT_HIGHEST_COMPLEXITY - BRAID_CHANCE_AT_LOWEST_COMPLEXITY;
        float braidChance = BRAID_CHANCE_AT_LOWEST_COMPLEXITY + (braidSpan * normalizedComplexity);

        int cellColumnCount = std::max(1, (gridWidth - corridorThickness) / (2 * corridorThickness));
        int cellRowCount = std::max(1, (gridHeight - corridorThickness) / (2 * corridorThickness));

        int renderedWidthInPixels = (cellColumnCount * 2 * corridorThickness) + corridorThickness;
        int renderedHeightInPixels = (cellRowCount * 2 * corridorThickness) + corridorThickness;

        MazeGeometry assembledGeometry;
        assembledGeometry.corridorThickness = corridorThickness;
        assembledGeometry.cellColumnCount = cellColumnCount;
        assembledGeometry.cellRowCount = cellRowCount;
        // Centre the maze; any leftover pixels simply thicken the outer wall.
        assembledGeometry.horizontalRenderOffset = (gridWidth - renderedWidthInPixels) / 2;
        assembledGeometry.verticalRenderOffset = (gridHeight - renderedHeightInPixels) / 2;
        destinationMaze.geometry = assembledGeometry;

        carveCellGraph(cellColumnCount, cellRowCount, straightnessBias);
        braidDeadEnds(cellColumnCount, cellRowCount, braidChance);

        int entranceCellColumn = sampleIntegerRange(0, cellColumnCount - 1);
        int exitCellColumn = sampleIntegerRange(0, cellColumnCount - 1);

        // -- Rasterise the cell graph into the wall bitmap ----------------------
        size_t totalPixelCount = static_cast<size_t>(gridWidth) * static_cast<size_t>(gridHeight);
        destinationMaze.wallOccupancyMask.assign(totalPixelCount, 1);

        uint8_t* wallOccupancyData = destinationMaze.wallOccupancyMask.data();

        auto openPixelBlock = [&](int blockStartX, int blockStartY, int blockWidth, int blockHeight) {
            int clampedStartY = std::max(0, blockStartY);
            int clampedEndY = std::min(gridHeight, blockStartY + blockHeight);
            int clampedStartX = std::max(0, blockStartX);
            int clampedEndX = std::min(gridWidth, blockStartX + blockWidth);

            for (int targetRow = clampedStartY; targetRow < clampedEndY; ++targetRow) {
                uint8_t* rowPointer = wallOccupancyData + (static_cast<size_t>(targetRow) * gridWidth);
                for (int targetColumn = clampedStartX; targetColumn < clampedEndX; ++targetColumn) {
                    rowPointer[targetColumn] = 0;
                }
            }
        };

        for (int cellRowIterator = 0; cellRowIterator < cellRowCount; ++cellRowIterator) {
            for (int cellColumnIterator = 0; cellColumnIterator < cellColumnCount; ++cellColumnIterator) {

                int inspectedFlatIndex = (cellRowIterator * cellColumnCount) + cellColumnIterator;
                int cellPixelOriginX = assembledGeometry.resolveCellOriginX(cellColumnIterator);
                int cellPixelOriginY = assembledGeometry.resolveCellOriginY(cellRowIterator);

                openPixelBlock(cellPixelOriginX, cellPixelOriginY, corridorThickness, corridorThickness);

                uint8_t cellMask = openDirectionMaskPerCell[static_cast<size_t>(inspectedFlatIndex)];

                // Only the east and south links need drawing; north and west links
                // are drawn by the neighbouring cell that owns the same gap.
                if ((cellMask & DIRECTION_FLAG_EAST) != 0) {
                    openPixelBlock(cellPixelOriginX + corridorThickness, cellPixelOriginY,
                                   corridorThickness, corridorThickness);
                }
                if ((cellMask & DIRECTION_FLAG_SOUTH) != 0) {
                    openPixelBlock(cellPixelOriginX, cellPixelOriginY + corridorThickness,
                                   corridorThickness, corridorThickness);
                }
            }
        }

        // Break the outer wall so the maze has a visible way in and a way out.
        openPixelBlock(assembledGeometry.resolveCellOriginX(entranceCellColumn),
                       assembledGeometry.verticalRenderOffset,
                       corridorThickness, corridorThickness);

        openPixelBlock(assembledGeometry.resolveCellOriginX(exitCellColumn),
                       assembledGeometry.resolveCellOriginY(cellRowCount - 1) + corridorThickness,
                       corridorThickness, corridorThickness);

        recordSolverWalk(cellColumnCount, cellRowCount, entranceCellColumn, exitCellColumn,
                         destinationMaze.trailMoveSequence);
    }
};

// ============================================================================
// SCROLLING MAZE ANIMATOR CLASS
// ============================================================================

/**
 * @brief Owns the animation: the maze on screen, the maze waiting above it, the
 * scroll position between them, and the solver trail crawling through the maze.
 *
 * The trail is drawn from a single byte per pixel intensity buffer. Completed
 * pixels are written once when the head passes over them and are never touched
 * again, so per frame work is proportional to the width of one corridor rather
 * than to the size of the panel. The head pixel itself is written at fractional
 * intensity every frame, which is what makes the motion continuous rather than
 * jumping a whole LED at a time.
 */
class ScrollingMazeAnimator {
private:
    /**
     * @brief The three stages every maze passes through, in order.
     */
    enum class AnimationPhase {
        SolvingTrail,
        HoldingSolvedMaze,
        ScrollingNextMaze
    };

    MazeStructureGenerator mazeStructureGenerator;
    ContrastingPaletteGenerator contrastingPaletteGenerator;

    RenderedMazeFrame currentMazeFrame;
    RenderedMazeFrame incomingMazeFrame;

    int animationGridWidth;
    int animationGridHeight;
    int configuredComplexityLevel;
    float configuredScrollSpeedRowsPerSecond;
    float configuredHoldDurationSeconds;
    float configuredSolveDurationSeconds;

    AnimationPhase currentAnimationPhase;
    float accumulatedScrollOffsetRows;
    float accumulatedHoldTimerSeconds;

    // Trail playback state.
    std::vector<uint8_t> trailIntensityBuffer;
    size_t activeTrailMoveIndex;
    float activeTrailSliceProgress;
    int committedTrailSliceCount;
    float trailSlicesPerSecond;

    // Composed once per frame, then read by both the OPC path and the preview.
    std::vector<PixelColorState> gridColorBuffer;

    /**
     * @brief Fills one maze structure and gives it a fresh palette.
     */
    void buildFreshMazeFrame(RenderedMazeFrame& destinationFrame) {
        mazeStructureGenerator.buildMaze(animationGridWidth, animationGridHeight,
                                         configuredComplexityLevel, destinationFrame.generatedMaze);
        destinationFrame.assignedColorPalette = contrastingPaletteGenerator.generateContrastingPalette();
    }

    /**
     * @brief Resolves the one pixel wide strip the trail head covers at a given
     * slice of a given move.
     *
     * A move spans twice the corridor thickness: the gap in the wall, then the
     * destination cell. A forward move sweeps outward from the origin cell. A
     * retreat sweeps in the same travel direction but starts at the far edge of the
     * origin cell, so the deepest pixels go dark first and the head appears to be
     * pulled back the way it came.
     */
    static PixelRectangle resolveTrailSliceRectangle(const MazeGeometry& geometry,
                                                     const TrailMove& inspectedMove,
                                                     int sliceIndex) {

        int corridorThickness = geometry.corridorThickness;
        int cellOriginX = geometry.resolveCellOriginX(inspectedMove.cellColumn);
        int cellOriginY = geometry.resolveCellOriginY(inspectedMove.cellRow);

        bool travelIsPositive = (inspectedMove.directionIdentifier == DIRECTION_EAST)
                             || (inspectedMove.directionIdentifier == DIRECTION_SOUTH);
        bool travelIsHorizontal = (inspectedMove.directionIdentifier == DIRECTION_EAST)
                               || (inspectedMove.directionIdentifier == DIRECTION_WEST);

        int offsetAlongTravel;
        if (inspectedMove.moveIsRetreat != 0) {
            offsetAlongTravel = travelIsPositive ? sliceIndex : (corridorThickness - 1 - sliceIndex);
        } else {
            offsetAlongTravel = travelIsPositive ? (corridorThickness + sliceIndex) : (-1 - sliceIndex);
        }

        if (travelIsHorizontal == true) {
            return { cellOriginX + offsetAlongTravel, cellOriginY, 1, corridorThickness };
        }
        return { cellOriginX, cellOriginY + offsetAlongTravel, corridorThickness, 1 };
    }

    /**
     * @brief Stamps one slice of the trail into the intensity buffer.
     */
    void writeTrailSlice(const TrailMove& inspectedMove, int sliceIndex, uint8_t intensityValue) {
        PixelRectangle sliceRectangle = resolveTrailSliceRectangle(
            currentMazeFrame.generatedMaze.geometry, inspectedMove, sliceIndex);

        int clampedStartY = std::max(0, sliceRectangle.originY);
        int clampedEndY = std::min(animationGridHeight, sliceRectangle.originY + sliceRectangle.spanY);
        int clampedStartX = std::max(0, sliceRectangle.originX);
        int clampedEndX = std::min(animationGridWidth, sliceRectangle.originX + sliceRectangle.spanX);

        for (int targetRow = clampedStartY; targetRow < clampedEndY; ++targetRow) {
            uint8_t* rowPointer = trailIntensityBuffer.data() + (static_cast<size_t>(targetRow) * animationGridWidth);
            for (int targetColumn = clampedStartX; targetColumn < clampedEndX; ++targetColumn) {
                rowPointer[targetColumn] = intensityValue;
            }
        }
    }

    /**
     * @brief The value a fully traversed slice settles on for a given move.
     */
    static uint8_t resolveSettledIntensity(const TrailMove& inspectedMove) {
        return (inspectedMove.moveIsRetreat != 0) ? TRAIL_INTENSITY_ABSENT : TRAIL_INTENSITY_FULL;
    }

    /**
     * @brief Clears the trail and arms it for the maze now in place.
     */
    void restartTrailForCurrentMaze() {
        std::fill(trailIntensityBuffer.begin(), trailIntensityBuffer.end(), TRAIL_INTENSITY_ABSENT);

        activeTrailMoveIndex = 0;
        activeTrailSliceProgress = 0.0f;
        committedTrailSliceCount = 0;

        const auto& recordedMoves = currentMazeFrame.generatedMaze.trailMoveSequence;
        int slicesPerMove = currentMazeFrame.generatedMaze.geometry.resolveSlicesPerMove();

        // Pace the walk so it always takes the requested number of seconds, however
        // many dead ends this particular maze happens to send the solver down.
        double totalSliceCount = static_cast<double>(recordedMoves.size()) * static_cast<double>(slicesPerMove);
        double pacedSlicesPerSecond = std::max(1.0, totalSliceCount) / configuredSolveDurationSeconds;
        trailSlicesPerSecond = static_cast<float>(pacedSlicesPerSecond);

        currentAnimationPhase = AnimationPhase::SolvingTrail;
    }

    /**
     * @brief Advances the trail head, stamping every slice it fully crosses.
     */
    void advanceTrailPlayback(float elapsedDeltaSeconds) {
        const auto& recordedMoves = currentMazeFrame.generatedMaze.trailMoveSequence;
        int slicesPerMove = currentMazeFrame.generatedMaze.geometry.resolveSlicesPerMove();

        if (recordedMoves.empty() == true || slicesPerMove <= 0) {
            accumulatedHoldTimerSeconds = 0.0f;
            currentAnimationPhase = AnimationPhase::HoldingSolvedMaze;
            return;
        }

        activeTrailSliceProgress = activeTrailSliceProgress + (trailSlicesPerSecond * elapsedDeltaSeconds);

        // Retire whole moves first, settling anything the head has fully crossed.
        while (activeTrailSliceProgress >= static_cast<float>(slicesPerMove)) {
            const TrailMove& retiringMove = recordedMoves[activeTrailMoveIndex];
            uint8_t settledIntensity = resolveSettledIntensity(retiringMove);

            while (committedTrailSliceCount < slicesPerMove) {
                writeTrailSlice(retiringMove, committedTrailSliceCount, settledIntensity);
                committedTrailSliceCount = committedTrailSliceCount + 1;
            }

            activeTrailSliceProgress = activeTrailSliceProgress - static_cast<float>(slicesPerMove);
            committedTrailSliceCount = 0;
            activeTrailMoveIndex = activeTrailMoveIndex + 1;

            if (activeTrailMoveIndex >= recordedMoves.size()) {
                accumulatedHoldTimerSeconds = 0.0f;
                currentAnimationPhase = AnimationPhase::HoldingSolvedMaze;
                return;
            }
        }

        const TrailMove& activeMove = recordedMoves[activeTrailMoveIndex];
        uint8_t settledIntensity = resolveSettledIntensity(activeMove);

        int headSliceIndex = static_cast<int>(activeTrailSliceProgress);
        headSliceIndex = std::clamp(headSliceIndex, 0, slicesPerMove - 1);

        // Settle every slice strictly behind the head exactly once.
        while (committedTrailSliceCount < headSliceIndex) {
            writeTrailSlice(activeMove, committedTrailSliceCount, settledIntensity);
            committedTrailSliceCount = committedTrailSliceCount + 1;
        }

        // Then draw the head itself at partial coverage. Rewritten every frame,
        // which is what carries the motion between whole LEDs.
        float fractionalCoverage = activeTrailSliceProgress - static_cast<float>(headSliceIndex);
        float scaledCoverage = fractionalCoverage * 255.0f;
        int coverageAsInteger = std::clamp(static_cast<int>(scaledCoverage), 0, 255);

        uint8_t headIntensity = (activeMove.moveIsRetreat != 0)
                              ? static_cast<uint8_t>(255 - coverageAsInteger)
                              : static_cast<uint8_t>(coverageAsInteger);

        writeTrailSlice(activeMove, headSliceIndex, headIntensity);
    }

public:
    ScrollingMazeAnimator(std::mt19937& randomEngineReference, int gridWidth, int gridHeight,
                          int complexityLevel, float scrollSpeedRowsPerSecond,
                          float holdDurationSeconds, float solveDurationSeconds)
        : mazeStructureGenerator(randomEngineReference),
          contrastingPaletteGenerator(randomEngineReference),
          animationGridWidth(std::max(1, gridWidth)),
          animationGridHeight(std::max(1, gridHeight)),
          configuredComplexityLevel(complexityLevel),
          configuredScrollSpeedRowsPerSecond(scrollSpeedRowsPerSecond),
          configuredHoldDurationSeconds(holdDurationSeconds),
          configuredSolveDurationSeconds(solveDurationSeconds),
          currentAnimationPhase(AnimationPhase::SolvingTrail),
          accumulatedScrollOffsetRows(0.0f),
          accumulatedHoldTimerSeconds(0.0f),
          activeTrailMoveIndex(0),
          activeTrailSliceProgress(0.0f),
          committedTrailSliceCount(0),
          trailSlicesPerSecond(1.0f) {

        size_t totalPixelCount = static_cast<size_t>(animationGridWidth) * static_cast<size_t>(animationGridHeight);
        trailIntensityBuffer.assign(totalPixelCount, TRAIL_INTENSITY_ABSENT);
        gridColorBuffer.assign(totalPixelCount, PixelColorState{ 0, 0, 0 });

        // Both frames are built up front so the compositor never sees an empty
        // buffer, and so no maze has to be generated mid scroll.
        buildFreshMazeFrame(currentMazeFrame);
        buildFreshMazeFrame(incomingMazeFrame);
        restartTrailForCurrentMaze();
    }

    int getGridWidth() const { return animationGridWidth; }
    int getGridHeight() const { return animationGridHeight; }

    /**
     * @brief Advances whichever stage the animation is currently in by one frame.
     */
    void advanceAnimationState(float elapsedDeltaSeconds) {
        if (currentAnimationPhase == AnimationPhase::SolvingTrail) {
            advanceTrailPlayback(elapsedDeltaSeconds);
            return;
        }

        if (currentAnimationPhase == AnimationPhase::HoldingSolvedMaze) {
            accumulatedHoldTimerSeconds = accumulatedHoldTimerSeconds + elapsedDeltaSeconds;

            if (accumulatedHoldTimerSeconds >= configuredHoldDurationSeconds) {
                accumulatedScrollOffsetRows = 0.0f;
                currentAnimationPhase = AnimationPhase::ScrollingNextMaze;
            }
            return;
        }

        accumulatedScrollOffsetRows =
            accumulatedScrollOffsetRows + (configuredScrollSpeedRowsPerSecond * elapsedDeltaSeconds);

        if (accumulatedScrollOffsetRows >= static_cast<float>(animationGridHeight)) {
            // Swap rather than copy: the maze buffers change owner, not address.
            std::swap(currentMazeFrame, incomingMazeFrame);
            accumulatedScrollOffsetRows = 0.0f;
            accumulatedHoldTimerSeconds = 0.0f;

            restartTrailForCurrentMaze();

            // Prepare the next maze now, while there is a whole solve to absorb it.
            buildFreshMazeFrame(incomingMazeFrame);
        }
    }

    /**
     * @brief Composes the whole grid into a colour buffer for this frame.
     *
     * Everything that is constant across a row - which maze it comes from, which
     * palette, where its mask row starts - is hoisted out of the inner loop, so the
     * per pixel cost is one mask read and, at most, one blend.
     */
    void composeGridColorBuffer() {
        int wholeRowShift = static_cast<int>(accumulatedScrollOffsetRows);
        wholeRowShift = std::clamp(wholeRowShift, 0, animationGridHeight);

        for (int displayRow = 0; displayRow < animationGridHeight; ++displayRow) {
            int sourceRow = displayRow - wholeRowShift;

            const RenderedMazeFrame* sourceFrame;
            const uint8_t* trailRowPointer;

            if (sourceRow >= 0) {
                // The maze already in place carries the solver trail.
                sourceFrame = &currentMazeFrame;
                trailRowPointer = trailIntensityBuffer.data() + (static_cast<size_t>(sourceRow) * animationGridWidth);
            } else {
                // The maze still arriving from above has not been explored yet.
                sourceFrame = &incomingMazeFrame;
                sourceRow = sourceRow + animationGridHeight;
                trailRowPointer = nullptr;
            }

            sourceRow = std::clamp(sourceRow, 0, animationGridHeight - 1);

            const MazeColorPalette& activePalette = sourceFrame->assignedColorPalette;
            const uint8_t* wallRowPointer = sourceFrame->generatedMaze.wallOccupancyMask.data()
                                          + (static_cast<size_t>(sourceRow) * animationGridWidth);
            PixelColorState* destinationRowPointer = gridColorBuffer.data()
                                                   + (static_cast<size_t>(displayRow) * animationGridWidth);

            for (int displayColumn = 0; displayColumn < animationGridWidth; ++displayColumn) {
                if (wallRowPointer[displayColumn] != 0) {
                    destinationRowPointer[displayColumn] = activePalette.wallColorState;
                    continue;
                }

                uint8_t trailIntensity = (trailRowPointer != nullptr) ? trailRowPointer[displayColumn]
                                                                     : TRAIL_INTENSITY_ABSENT;

                if (trailIntensity == TRAIL_INTENSITY_ABSENT) {
                    destinationRowPointer[displayColumn] = activePalette.backgroundColorState;
                } else if (trailIntensity == TRAIL_INTENSITY_FULL) {
                    destinationRowPointer[displayColumn] = activePalette.trailColorState;
                } else {
                    destinationRowPointer[displayColumn] = ColorConverter::blendColorStates(
                        activePalette.backgroundColorState, activePalette.trailColorState, trailIntensity);
                }
            }
        }
    }

    const std::vector<PixelColorState>& getGridColorBuffer() const {
        return gridColorBuffer;
    }

    /**
     * @brief Copies the composed grid into the outgoing OPC payload.
     * Every target was validated at startup, so this loop contains no branches.
     */
    void writePayloadFromComposedGrid(const std::vector<PixelRenderTarget>& renderTargets,
                                      std::vector<uint8_t>& outgoingNetworkPayloadBuffer) const {

        const PixelColorState* composedGridData = gridColorBuffer.data();
        uint8_t* payloadData = outgoingNetworkPayloadBuffer.data();

        for (const PixelRenderTarget& inspectedTarget : renderTargets) {
            const PixelColorState& resolvedColor = composedGridData[inspectedTarget.gridFlatIndex];
            uint8_t* payloadCursor = payloadData + inspectedTarget.payloadByteOffset;

            payloadCursor[0] = resolvedColor.redIntensity;
            payloadCursor[1] = resolvedColor.greenIntensity;
            payloadCursor[2] = resolvedColor.blueIntensity;
        }
    }
};

// ============================================================================
// TERMINAL PREVIEW RENDERER CLASS
// ============================================================================

/**
 * @brief Draws the animation to the terminal using truecolour escape sequences.
 * Intended for tuning complexity and speed without physical hardware attached.
 * The output string is retained between frames so steady state drawing performs
 * no allocation.
 */
class TerminalPreviewRenderer {
private:
    std::string reusableOutputBuffer;

    static void appendIntegerValue(std::string& destinationBuffer, uint8_t inspectedValue) {
        if (inspectedValue >= 100) {
            destinationBuffer.push_back(static_cast<char>('0' + (inspectedValue / 100)));
        }
        if (inspectedValue >= 10) {
            destinationBuffer.push_back(static_cast<char>('0' + ((inspectedValue / 10) % 10)));
        }
        destinationBuffer.push_back(static_cast<char>('0' + (inspectedValue % 10)));
    }

public:
    static void clearTerminalScreen() {
        std::cout << "\x1b[2J";
    }

    void drawPreviewFrame(const std::vector<PixelColorState>& composedGridColors,
                          int gridWidth, int gridHeight) {

        reusableOutputBuffer.clear();
        reusableOutputBuffer.append("\x1b[H");

        for (int rowIterator = 0; rowIterator < gridHeight; ++rowIterator) {
            const PixelColorState* rowPointer = composedGridColors.data()
                                              + (static_cast<size_t>(rowIterator) * gridWidth);

            for (int columnIterator = 0; columnIterator < gridWidth; ++columnIterator) {
                const PixelColorState& previewColor = rowPointer[columnIterator];

                reusableOutputBuffer.append("\x1b[48;2;");
                appendIntegerValue(reusableOutputBuffer, previewColor.redIntensity);
                reusableOutputBuffer.push_back(';');
                appendIntegerValue(reusableOutputBuffer, previewColor.greenIntensity);
                reusableOutputBuffer.push_back(';');
                appendIntegerValue(reusableOutputBuffer, previewColor.blueIntensity);
                reusableOutputBuffer.append("m  ");
            }
            reusableOutputBuffer.append("\x1b[0m\n");
        }

        std::cout << reusableOutputBuffer << std::flush;
    }
};

// ============================================================================
// COMMAND LINE OPTIONS
// ============================================================================

/**
 * @brief Every operator adjustable setting for this effect.
 */
struct CommandLineOptions {
    std::string ledTypeArgument = DEFAULT_LED_TYPE_ARGUMENT;
    int complexityLevel = DEFAULT_COMPLEXITY_LEVEL;
    float scrollSpeedRowsPerSecond = DEFAULT_SCROLL_SPEED_ROWS_PER_SECOND;
    float holdDurationSeconds = DEFAULT_HOLD_DURATION_SECONDS;
    float solveDurationSeconds = DEFAULT_SOLVE_DURATION_SECONDS;
    int targetAnimationFps = DEFAULT_TARGET_ANIMATION_FPS;
    unsigned int randomSeedValue = 0;
    bool randomSeedWasProvided = false;
    int gridWidthOverride = 0;
    int gridHeightOverride = 0;
    bool verticalAxisIsFlipped = false;
    bool terminalPreviewEnabled = false;
    bool helpWasRequested = false;
};

/**
 * @brief Parses argv into the options structure, mirroring the noise effect's
 * convention of taking the LED type as the first positional argument.
 */
class CommandLineParser {
public:
    static void printUsageInformation(const char* executableName) {
        std::cout
            << "Usage: " << executableName << " [led-type] [options]\n\n"
            << "  led-type                 LED layout passed straight to LedLayout, e.g. 8x8\n"
            << "                           (default: " << DEFAULT_LED_TYPE_ARGUMENT << ")\n\n"
            << "Options:\n"
            << "  -c, --complexity N       Maze complexity, " << MINIMUM_COMPLEXITY_LEVEL
            << " to " << MAXIMUM_COMPLEXITY_LEVEL << " (default: " << DEFAULT_COMPLEXITY_LEVEL << ").\n"
            << "                           Low values give wide, straight, loopy corridors.\n"
            << "                           High values give one-pixel, twisty, dead-ended mazes.\n"
            << "  -s, --speed R            Scroll speed in LED rows per second\n"
            << "                           (default: " << DEFAULT_SCROLL_SPEED_ROWS_PER_SECOND << ").\n"
            << "      --solve-time S       Seconds for the trail to walk from entrance to exit,\n"
            << "                           backtracking included (default: "
            << DEFAULT_SOLVE_DURATION_SECONDS << ").\n"
            << "      --hold S             Seconds a solved maze rests before the next one\n"
            << "                           scrolls in (default: " << DEFAULT_HOLD_DURATION_SECONDS << ").\n"
            << "      --fps N              Render loop frame rate (default: "
            << DEFAULT_TARGET_ANIMATION_FPS << ").\n"
            << "      --grid WxH           Force the maze resolution instead of deriving it\n"
            << "                           from the layout, e.g. 32x32. Useful for ring or\n"
            << "                           scattered layouts.\n"
            << "      --seed N             Fixed random seed for reproducible mazes.\n"
            << "      --flip-y             Invert the vertical axis if the panel is mounted\n"
            << "                           upside down and mazes scroll the wrong way.\n"
            << "      --preview            Draw to the terminal instead of sending OPC packets.\n"
            << "  -h, --help               Show this message.\n";
    }

    static bool parseArguments(int argumentCount, char** argumentValues, CommandLineOptions& parsedOptions) {
        bool positionalLedTypeWasConsumed = false;

        for (int argumentIterator = 1; argumentIterator < argumentCount; ++argumentIterator) {
            std::string currentArgument = argumentValues[argumentIterator];

            auto readFollowingValue = [&](std::string& destinationValue) -> bool {
                if ((argumentIterator + 1) >= argumentCount) {
                    std::cerr << "Error: option " << currentArgument << " requires a value.\n";
                    return false;
                }
                argumentIterator = argumentIterator + 1;
                destinationValue = argumentValues[argumentIterator];
                return true;
            };

            if (currentArgument == "-h" || currentArgument == "--help") {
                parsedOptions.helpWasRequested = true;
                return true;
            }

            if (currentArgument == "-c" || currentArgument == "--complexity") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.complexityLevel = std::atoi(rawValue.c_str());
                continue;
            }

            if (currentArgument == "-s" || currentArgument == "--speed") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.scrollSpeedRowsPerSecond = static_cast<float>(std::atof(rawValue.c_str()));
                continue;
            }

            if (currentArgument == "--solve-time") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.solveDurationSeconds = static_cast<float>(std::atof(rawValue.c_str()));
                continue;
            }

            if (currentArgument == "--hold") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.holdDurationSeconds = static_cast<float>(std::atof(rawValue.c_str()));
                continue;
            }

            if (currentArgument == "--fps") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.targetAnimationFps = std::atoi(rawValue.c_str());
                continue;
            }

            if (currentArgument == "--grid") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;

                int parsedGridWidth = 0;
                int parsedGridHeight = 0;
                if (std::sscanf(rawValue.c_str(), "%dx%d", &parsedGridWidth, &parsedGridHeight) != 2
                    || parsedGridWidth <= 0 || parsedGridHeight <= 0) {
                    std::cerr << "Error: --grid expects WxH with positive values, e.g. 32x32.\n";
                    return false;
                }
                parsedOptions.gridWidthOverride = parsedGridWidth;
                parsedOptions.gridHeightOverride = parsedGridHeight;
                continue;
            }

            if (currentArgument == "--seed") {
                std::string rawValue;
                if (readFollowingValue(rawValue) == false) return false;
                parsedOptions.randomSeedValue = static_cast<unsigned int>(std::strtoul(rawValue.c_str(), nullptr, 10));
                parsedOptions.randomSeedWasProvided = true;
                continue;
            }

            if (currentArgument == "--flip-y") {
                parsedOptions.verticalAxisIsFlipped = true;
                continue;
            }

            if (currentArgument == "--preview") {
                parsedOptions.terminalPreviewEnabled = true;
                continue;
            }

            if (currentArgument.rfind("-", 0) == 0 && currentArgument.size() > 1) {
                std::cerr << "Error: unrecognised option " << currentArgument << ".\n";
                return false;
            }

            if (positionalLedTypeWasConsumed == false) {
                parsedOptions.ledTypeArgument = currentArgument;
                positionalLedTypeWasConsumed = true;
                continue;
            }

            std::cerr << "Error: unexpected extra argument " << currentArgument << ".\n";
            return false;
        }

        return true;
    }

    /**
     * @brief Forces every supplied value into a range the renderer can honour.
     */
    static void clampOptionsToSupportedRanges(CommandLineOptions& parsedOptions) {
        parsedOptions.complexityLevel = std::clamp(parsedOptions.complexityLevel,
                                                   MINIMUM_COMPLEXITY_LEVEL, MAXIMUM_COMPLEXITY_LEVEL);
        parsedOptions.scrollSpeedRowsPerSecond = std::clamp(parsedOptions.scrollSpeedRowsPerSecond,
                                                            MINIMUM_SCROLL_SPEED_ROWS_PER_SECOND,
                                                            MAXIMUM_SCROLL_SPEED_ROWS_PER_SECOND);
        parsedOptions.solveDurationSeconds = std::clamp(parsedOptions.solveDurationSeconds,
                                                        MINIMUM_SOLVE_DURATION_SECONDS,
                                                        MAXIMUM_SOLVE_DURATION_SECONDS);
        parsedOptions.holdDurationSeconds = std::max(0.0f, parsedOptions.holdDurationSeconds);
        parsedOptions.targetAnimationFps = std::clamp(parsedOptions.targetAnimationFps,
                                                      MINIMUM_ANIMATION_FPS, MAXIMUM_ANIMATION_FPS);
    }
};

// ============================================================================
// MAIN APPLICATION ENTRY
// ============================================================================

int main(int argumentCount, char** argumentValues) {
    CommandLineOptions parsedOptions;

    if (CommandLineParser::parseArguments(argumentCount, argumentValues, parsedOptions) == false) {
        return EXIT_FAILURE;
    }
    if (parsedOptions.helpWasRequested == true) {
        CommandLineParser::printUsageInformation(argumentValues[0]);
        return EXIT_SUCCESS;
    }
    CommandLineParser::clampOptionsToSupportedRanges(parsedOptions);

    LedLayout hardwareDisplayLayout;
    hardwareDisplayLayout.parseArgument(parsedOptions.ledTypeArgument);

    // getPixels() hands back a fresh, mutex guarded copy on every call, so it is
    // called exactly once for the entire lifetime of the program.
    const std::vector<LedPixel> layoutPixelCollection = hardwareDisplayLayout.getPixels();

    if (layoutPixelCollection.empty() == true) {
        std::cerr << "Error: LED type '" << parsedOptions.ledTypeArgument << "' produced no pixels.\n";
        return EXIT_FAILURE;
    }

    int maximalHardwareAddressIndex = 0;
    for (const LedPixel& systemPixel : layoutPixelCollection) {
        if (systemPixel.opcIndex > maximalHardwareAddressIndex) {
            maximalHardwareAddressIndex = systemPixel.opcIndex;
        }
    }

    size_t payloadSizeInBytes = (static_cast<size_t>(maximalHardwareAddressIndex) + 1) * 3;

    // std::vector handles dynamic memory securely, avoiding manual memory management or leaks
    std::vector<uint8_t> outgoingNetworkPayloadBuffer(payloadSizeInBytes, 0);

    // Derive the discrete grid from the layout itself, so the maze sizes itself
    // to whichever LED type was requested without any per-panel constants. Every
    // index the render loop will use is resolved and validated right here.
    DisplayGridMapper displayGridMapper;
    std::vector<PixelRenderTarget> pixelRenderTargets = displayGridMapper.buildRenderTargets(
        layoutPixelCollection, payloadSizeInBytes, parsedOptions.verticalAxisIsFlipped,
        parsedOptions.gridWidthOverride, parsedOptions.gridHeightOverride);

    int animationGridWidth = displayGridMapper.getGridWidth();
    int animationGridHeight = displayGridMapper.getGridHeight();

    unsigned int effectiveRandomSeed = parsedOptions.randomSeedValue;
    if (parsedOptions.randomSeedWasProvided == false) {
        std::random_device nonDeterministicSeedSource;
        effectiveRandomSeed = nonDeterministicSeedSource();
    }
    std::mt19937 sharedRandomEngine(effectiveRandomSeed);

    ScrollingMazeAnimator scrollingMazeAnimator(
        sharedRandomEngine,
        animationGridWidth,
        animationGridHeight,
        parsedOptions.complexityLevel,
        parsedOptions.scrollSpeedRowsPerSecond,
        parsedOptions.holdDurationSeconds,
        parsedOptions.solveDurationSeconds
    );

    std::cerr << "maze: led type '" << parsedOptions.ledTypeArgument << "' -> grid "
              << animationGridWidth << "x" << animationGridHeight
              << " (layout reports " << hardwareDisplayLayout.getLogicalWidth()
              << "x" << hardwareDisplayLayout.getLogicalHeight() << "), "
              << pixelRenderTargets.size() << " of " << layoutPixelCollection.size()
              << " leds addressable, mapping " << displayGridMapper.getMappingModeName()
              << ", complexity " << parsedOptions.complexityLevel
              << ", scroll " << parsedOptions.scrollSpeedRowsPerSecond << " rows/s"
              << ", solve " << parsedOptions.solveDurationSeconds << "s"
              << ", seed " << effectiveRandomSeed << "\n";

    OPCClient networkPixelsClient(OPC_SERVER_ADDRESS, OPC_SERVER_PORT);
    TerminalPreviewRenderer terminalPreviewRenderer;

    if (parsedOptions.terminalPreviewEnabled == true) {
        TerminalPreviewRenderer::clearTerminalScreen();
    }

    auto frameDelayDuration = std::chrono::microseconds(
        (MILLISECONDS_PER_SECOND * 1000) / parsedOptions.targetAnimationFps);
    auto previousFrameTimestamp = std::chrono::steady_clock::now();

    // Infinite render loop
    while (true) {
        auto renderedFrameStartTime = std::chrono::steady_clock::now();

        float elapsedDeltaSeconds =
            std::chrono::duration<float>(renderedFrameStartTime - previousFrameTimestamp).count();
        previousFrameTimestamp = renderedFrameStartTime;

        // A long stall (scheduler hiccup, terminal blocking) must not teleport the
        // trail head across the maze, so clamp the step the animation can take.
        elapsedDeltaSeconds = std::clamp(elapsedDeltaSeconds, 0.0f, 0.25f);

        scrollingMazeAnimator.advanceAnimationState(elapsedDeltaSeconds);
        scrollingMazeAnimator.composeGridColorBuffer();

        if (parsedOptions.terminalPreviewEnabled == true) {
            terminalPreviewRenderer.drawPreviewFrame(
                scrollingMazeAnimator.getGridColorBuffer(),
                animationGridWidth, animationGridHeight);
        } else {
            scrollingMazeAnimator.writePayloadFromComposedGrid(pixelRenderTargets, outgoingNetworkPayloadBuffer);
            networkPixelsClient.sendPixelPayload(
                0,
                outgoingNetworkPayloadBuffer.data(),
                outgoingNetworkPayloadBuffer.size()
            );
        }

        std::this_thread::sleep_until(renderedFrameStartTime + frameDelayDuration);
    }

    return EXIT_SUCCESS;
}