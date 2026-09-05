// orbits.cpp - two counter-rotating glow orbs for an Open Pixel Control server.
//
// Sprites are generated procedurally at startup (no PNG assets, no stb_image).
// Each orb slowly fades to a new palette colour, holds it, then moves on.
//
// build: g++ -std=c++17 -O2 orbits.cpp ledlayout.cpp opcclient.cpp -o orbits -pthread
// usage: ./orbits 93ringinv [orbit-speed] [dot-size]

#include "ledlayout.h"
#include "opcclient.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// --- NETWORK / TIMING ---
constexpr const char* TARGET_NETWORK_HOST = "127.0.0.1";
constexpr int TARGET_NETWORK_PORT = 7890;
constexpr int REQUIRED_TARGET_FPS = 60;

// --- MOTION ---
constexpr float SCENE_CENTER_X = 0.5f;
constexpr float SCENE_CENTER_Y = 0.5f;
constexpr float KINETIC_ORBIT_RADIUS = 0.25f;
constexpr float KINETIC_ORBIT_SPEED = 1.25f;   // radians per second
constexpr float RENDERED_DOT_SIZE = 0.75f;    // sprite width in normalised units

// --- PROCEDURAL SPRITE SHAPE ---
constexpr int SPRITE_RESOLUTION = 128;           // texels per side
constexpr float SPRITE_FALLOFF_EXPONENT = 3.0f;  // higher = tighter halo
constexpr float SPRITE_CORE_RADIUS = 0.32f;      // white core size, fraction of sprite radius
constexpr float SPRITE_CORE_WHITENESS = 0.85f;   // 1.0 = pure white centre


// --- COLOUR CYCLE ---
constexpr float COLOR_HOLD_SECONDS = 9.0f;    // time parked on one colour
constexpr float COLOR_FADE_SECONDS = 5.0f;    // time spent crossfading
constexpr float ORB_SATURATION = 1.0f;
constexpr float ORB_BRIGHTNESS = 1.0f;
constexpr int SECOND_ORB_PALETTE_OFFSET = 4;  // half the palette: orbs stay opposed

struct ColorValues { uint8_t redIntensity, greenIntensity, blueIntensity; };

// Hues (degrees) the orbs walk through, in order. The palette is arranged so
// that entries four apart are near-complementary, which keeps the two orbs
// contrasting the way the original green/violet pair did.
constexpr float ORB_HUE_PALETTE[] = {
    130.0f,  // green   (the original green_dot)
    270.0f,  // violet  (the original purple_dot)
     20.0f,  // vermilion
    190.0f,  // cyan
    320.0f,  // pink
     90.0f,  // lime
    240.0f,  // blue
     55.0f,  // amber
};
constexpr int ORB_HUE_PALETTE_SIZE =
    static_cast<int>(sizeof(ORB_HUE_PALETTE) / sizeof(ORB_HUE_PALETTE[0]));

// ---------------------------------------------------------------------------
// Procedural sprite
// ---------------------------------------------------------------------------
// The blob is stored colour-free: `glow` is the overall brightness envelope and
// `core` is how white-hot that texel is. Tinting happens at blit time, so a
// colour change costs nothing - the texture is generated exactly once.
struct SpriteTexel {
    float glow;  // 0..1 brightness envelope
    float core;  // 0..1 how white-hot this texel is
};

class RadialGlowSprite {
public:
    void generateTexture(int resolutionPerSide) {
        resolution = resolutionPerSide;
        texels.assign(static_cast<size_t>(resolution) * resolution, {0.0f, 0.0f});

        const float inverseHalfSpan = 2.0f / static_cast<float>(resolution);

        for (int texelRow = 0; texelRow < resolution; ++texelRow) {
            // map texel centres onto [-1, +1]
            const float offsetY = (static_cast<float>(texelRow) + 0.5f) * inverseHalfSpan - 1.0f;

            for (int texelColumn = 0; texelColumn < resolution; ++texelColumn) {
                const float offsetX = (static_cast<float>(texelColumn) + 0.5f) * inverseHalfSpan - 1.0f;

                const float squaredDistance = offsetX * offsetX + offsetY * offsetY;
                if (squaredDistance >= 1.0f) continue;  // stays {0, 0}: fully dark

                // A bell built on (1 - d^2) is flat-topped at the centre. The old
                // (1 - d)^n curve came to a cusp there, which put the steepest part
                // of the whole sprite exactly where the white core sits.
                const float brightnessEnvelope =
                    std::pow(1.0f - squaredDistance, SPRITE_FALLOFF_EXPONENT);

                // The core gets its own radius rather than being a high power of the
                // envelope, so its width can be tuned against the LED spacing directly.
                float whiteHotness = 0.0f;
                const float coreRatio = std::sqrt(squaredDistance) / SPRITE_CORE_RADIUS;
                if (coreRatio < 1.0f) {
                    const float coreBump = 1.0f - coreRatio * coreRatio;
                    whiteHotness = SPRITE_CORE_WHITENESS * coreBump * coreBump;
                }

                texels[static_cast<size_t>(texelRow) * resolution + texelColumn] =
                    {brightnessEnvelope, whiteHotness};
            }
        }
    }

    int getResolution() const { return resolution; }

    // Bilinear lookup. Nearest-neighbour made an LED hold one texel's value and
    // then jump to the next as the orb crawled past, which read as a flicker on
    // the steep centre; interpolating makes the value move continuously instead.
    SpriteTexel sampleTexel(float textureCoordinateU, float textureCoordinateV) const {
        const float texelSpaceX = textureCoordinateU * static_cast<float>(resolution) - 0.5f;
        const float texelSpaceY = textureCoordinateV * static_cast<float>(resolution) - 0.5f;

        const float flooredX = std::floor(texelSpaceX);
        const float flooredY = std::floor(texelSpaceY);
        const int baseColumn = static_cast<int>(flooredX);
        const int baseRow = static_cast<int>(flooredY);
        const float fractionX = texelSpaceX - flooredX;
        const float fractionY = texelSpaceY - flooredY;

        const SpriteTexel& topLeft = fetchClamped(baseColumn, baseRow);
        const SpriteTexel& topRight = fetchClamped(baseColumn + 1, baseRow);
        const SpriteTexel& bottomLeft = fetchClamped(baseColumn, baseRow + 1);
        const SpriteTexel& bottomRight = fetchClamped(baseColumn + 1, baseRow + 1);

        auto blendFour = [&](float valueTopLeft, float valueTopRight,
                             float valueBottomLeft, float valueBottomRight) {
            const float topEdge = valueTopLeft + (valueTopRight - valueTopLeft) * fractionX;
            const float bottomEdge = valueBottomLeft + (valueBottomRight - valueBottomLeft) * fractionX;
            return topEdge + (bottomEdge - topEdge) * fractionY;
        };

        return {blendFour(topLeft.glow, topRight.glow, bottomLeft.glow, bottomRight.glow),
                blendFour(topLeft.core, topRight.core, bottomLeft.core, bottomRight.core)};
    }

private:
    const SpriteTexel& fetchClamped(int texelColumn, int texelRow) const {
        const int clampedColumn = std::clamp(texelColumn, 0, resolution - 1);
        const int clampedRow = std::clamp(texelRow, 0, resolution - 1);
        return texels[static_cast<size_t>(clampedRow) * resolution + clampedColumn];
    }

    int resolution = 0;
    std::vector<SpriteTexel> texels;
};

// ---------------------------------------------------------------------------
// Colour cycling
// ---------------------------------------------------------------------------
inline float smoothStepCurve(float linearRatio) {
    return linearRatio * linearRatio * (3.0f - 2.0f * linearRatio);
}

// Standard HSB/HSV to 8-bit RGB, matching the convention used by the other
// effects in this project.
ColorValues convertHsvToRgb(float hueDegrees, float saturation, float brightness) {
    hueDegrees = std::fmod(hueDegrees, 360.0f);
    if (hueDegrees < 0.0f) hueDegrees += 360.0f;

    const float chroma = brightness * saturation;
    const float sextant = hueDegrees / 60.0f;
    const float secondaryComponent = chroma * (1.0f - std::fabs(std::fmod(sextant, 2.0f) - 1.0f));
    const float matchValue = brightness - chroma;

    float redFloat = 0.0f, greenFloat = 0.0f, blueFloat = 0.0f;
    switch (static_cast<int>(sextant) % 6) {
        case 0: redFloat = chroma; greenFloat = secondaryComponent; break;
        case 1: redFloat = secondaryComponent; greenFloat = chroma; break;
        case 2: greenFloat = chroma; blueFloat = secondaryComponent; break;
        case 3: greenFloat = secondaryComponent; blueFloat = chroma; break;
        case 4: redFloat = secondaryComponent; blueFloat = chroma; break;
        default: redFloat = chroma; blueFloat = secondaryComponent; break;
    }

    return {static_cast<uint8_t>((redFloat + matchValue) * 255.0f + 0.5f),
            static_cast<uint8_t>((greenFloat + matchValue) * 255.0f + 0.5f),
            static_cast<uint8_t>((blueFloat + matchValue) * 255.0f + 0.5f)};
}

// Holds on a palette hue, then sweeps to the next one along the shorter arc of
// the colour wheel so the transition stays saturated instead of washing out
// through grey the way a straight RGB blend would.
ColorValues resolvePaletteColor(float elapsedSeconds, int paletteOffset) {
    constexpr float fullCycleSeconds = COLOR_HOLD_SECONDS + COLOR_FADE_SECONDS;

    const int cyclesCompleted = static_cast<int>(elapsedSeconds / fullCycleSeconds);
    const float secondsIntoCycle =
        elapsedSeconds - static_cast<float>(cyclesCompleted) * fullCycleSeconds;

    float blendRatio = 0.0f;
    if (secondsIntoCycle > COLOR_HOLD_SECONDS) {
        blendRatio = smoothStepCurve((secondsIntoCycle - COLOR_HOLD_SECONDS) / COLOR_FADE_SECONDS);
    }

    const int currentSlot = (cyclesCompleted + paletteOffset) % ORB_HUE_PALETTE_SIZE;
    const int upcomingSlot = (currentSlot + 1) % ORB_HUE_PALETTE_SIZE;

    const float currentHue = ORB_HUE_PALETTE[currentSlot];
    float hueDifference = ORB_HUE_PALETTE[upcomingSlot] - currentHue;
    if (hueDifference > 180.0f) hueDifference -= 360.0f;
    if (hueDifference < -180.0f) hueDifference += 360.0f;

    return convertHsvToRgb(currentHue + hueDifference * blendRatio, ORB_SATURATION, ORB_BRIGHTNESS);
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------
// Screen blend: 255 - (255 - a)(255 - b)/255, rounded once at the end so the
// result never jitters by a level from repeated truncation.
inline uint8_t screenBlendChannel(uint8_t existingValue, float incomingValue) {
    const float clampedIncoming = std::clamp(incomingValue, 0.0f, 255.0f);
    const float blended =
        255.0f - (255.0f - static_cast<float>(existingValue)) * (255.0f - clampedIncoming) / 255.0f;
    return static_cast<uint8_t>(blended + 0.5f);
}

class SceneRenderer {
public:
    // `layoutPixels` is a snapshot taken once at startup - never re-fetched per
    // pixel, which is what made the original blit so expensive.
    void drawSpriteBlended(const std::vector<LedPixel>& layoutPixels,
                            std::vector<ColorValues>& renderingColorBuffer,
                            const RadialGlowSprite& glowSprite,
                            const ColorValues& spriteTint,
                            float positionCenterX,
                            float positionCenterY,
                            float scaledDrawSize) const {
        const float halfSizeBoundary = scaledDrawSize * 0.5f;
        const float inverseDrawSize = 1.0f / scaledDrawSize;

        const float tintRed = static_cast<float>(spriteTint.redIntensity);
        const float tintGreen = static_cast<float>(spriteTint.greenIntensity);
        const float tintBlue = static_cast<float>(spriteTint.blueIntensity);

        const size_t totalPixels = layoutPixels.size();
        for (size_t hardwarePixelIndex = 0; hardwarePixelIndex < totalPixels; ++hardwarePixelIndex) {
            const LedPixel& currentPixel = layoutPixels[hardwarePixelIndex];

            const float distanceDeltaX = currentPixel.normX - positionCenterX;
            if (distanceDeltaX < -halfSizeBoundary || distanceDeltaX > halfSizeBoundary) continue;
            const float distanceDeltaY = currentPixel.normY - positionCenterY;
            if (distanceDeltaY < -halfSizeBoundary || distanceDeltaY > halfSizeBoundary) continue;

            const float mappedTextureU = (distanceDeltaX + halfSizeBoundary) * inverseDrawSize;
            const float mappedTextureV = (distanceDeltaY + halfSizeBoundary) * inverseDrawSize;

            const SpriteTexel sampledTexel = glowSprite.sampleTexel(mappedTextureU, mappedTextureV);
            if (sampledTexel.glow <= 0.0f) continue;  // outside the circle, nothing to add

            const float glowLevel = sampledTexel.glow;
            const float coreLevel = sampledTexel.core;
            const float tintWeight = 1.0f - coreLevel;

            // channel = glow * (tint * (1 - core) + white * core)
            const float addedRed = glowLevel * (tintRed * tintWeight + 255.0f * coreLevel);
            const float addedGreen = glowLevel * (tintGreen * tintWeight + 255.0f * coreLevel);
            const float addedBlue = glowLevel * (tintBlue * tintWeight + 255.0f * coreLevel);

            // Screen blend rather than a clamped add. Where the two halos overlap,
            // a straight add clips each channel at a different moment, so the
            // overlap region snaps toward white and shifts hue as the orbs sweep.
            // Screening rolls off smoothly and never exceeds full scale.
            ColorValues& destination = renderingColorBuffer[hardwarePixelIndex];
            destination.redIntensity = screenBlendChannel(destination.redIntensity, addedRed);
            destination.greenIntensity = screenBlendChannel(destination.greenIntensity, addedGreen);
            destination.blueIntensity = screenBlendChannel(destination.blueIntensity, addedBlue);
        }
    }
};

// ---------------------------------------------------------------------------
int main(int argumentCount, char** argumentValues) {
    LedLayout operationalLayout;
    const std::string providedArgumentData = (argumentCount > 1) ? argumentValues[1] : "8x8";
    operationalLayout.parseArgument(providedArgumentData);

    const float orbitSpeed = (argumentCount > 2) ? std::strtof(argumentValues[2], nullptr)
                                                 : KINETIC_ORBIT_SPEED;
    const float renderedDotSize = (argumentCount > 3) ? std::strtof(argumentValues[3], nullptr)
                                                      : RENDERED_DOT_SIZE;
    if (renderedDotSize <= 0.0f) {
        std::cerr << "Dot size must be greater than zero\n";
        return 1;
    }

    // Single snapshot of the layout: the geometry never changes after parsing,
    // so there is no reason to lock the mutex and copy the vector again.
    const std::vector<LedPixel> layoutPixels = operationalLayout.getPixels();
    if (layoutPixels.empty()) {
        std::cerr << "Layout produced no pixels, nothing to render\n";
        return 1;
    }

    OPCClient masterNetworkClient(TARGET_NETWORK_HOST, TARGET_NETWORK_PORT);
    const SceneRenderer graphicalRenderer;

    RadialGlowSprite glowSprite;
    glowSprite.generateTexture(SPRITE_RESOLUTION);

    std::vector<ColorValues> sceneColorBuffer(layoutPixels.size(), ColorValues{0, 0, 0});

    // Precompute the byte offset for every LED so the send loop is a plain copy.
    int maximumObservedHardwareIndex = 0;
    for (const LedPixel& physicalSystemPixel : layoutPixels) {
        maximumObservedHardwareIndex = std::max(maximumObservedHardwareIndex, physicalSystemPixel.opcIndex);
    }
    std::vector<size_t> payloadByteOffsets;
    payloadByteOffsets.reserve(layoutPixels.size());
    for (const LedPixel& physicalSystemPixel : layoutPixels) {
        payloadByteOffsets.push_back(static_cast<size_t>(physicalSystemPixel.opcIndex) * 3);
    }

    // Every mapped index is rewritten each frame and unmapped ones stay dark,
    // so the payload only needs zeroing once.
    std::vector<uint8_t> outgoingOpcPayload(static_cast<size_t>(maximumObservedHardwareIndex + 1) * 3, 0);

    std::cout << "Orbiting " << layoutPixels.size() << " LEDs at " << REQUIRED_TARGET_FPS
              << "fps (speed " << orbitSpeed << ", dot " << renderedDotSize << ")\n";

    using SteadyClock = std::chrono::steady_clock;
    const auto framePeriod = std::chrono::nanoseconds(1000000000LL / REQUIRED_TARGET_FPS);
    const auto systemExecutionStartTime = SteadyClock::now();
    auto nextFrameDeadline = systemExecutionStartTime;

    while (true) {
        const auto activeFrameStartTime = SteadyClock::now();
        const float totalElapsedApplicationSeconds =
            std::chrono::duration<float>(activeFrameStartTime - systemExecutionStartTime).count();

        std::fill(sceneColorBuffer.begin(), sceneColorBuffer.end(), ColorValues{0, 0, 0});

        const float orbitAngleRadians = totalElapsedApplicationSeconds * orbitSpeed;
        const float orbitCosine = std::cos(orbitAngleRadians);
        const float orbitSine = std::sin(orbitAngleRadians);

        // The second orb is exactly half a turn behind, so its offsets are negated.
        const float calculatedOrbPositionX1 = SCENE_CENTER_X + orbitCosine * KINETIC_ORBIT_RADIUS;
        const float calculatedOrbPositionY1 = SCENE_CENTER_Y + orbitSine * KINETIC_ORBIT_RADIUS;
        const float calculatedOrbPositionX2 = SCENE_CENTER_X - orbitCosine * KINETIC_ORBIT_RADIUS;
        const float calculatedOrbPositionY2 = SCENE_CENTER_Y - orbitSine * KINETIC_ORBIT_RADIUS;

        const ColorValues firstOrbColor = resolvePaletteColor(totalElapsedApplicationSeconds, 0);
        const ColorValues secondOrbColor =
            resolvePaletteColor(totalElapsedApplicationSeconds, SECOND_ORB_PALETTE_OFFSET);

        graphicalRenderer.drawSpriteBlended(layoutPixels, sceneColorBuffer, glowSprite, firstOrbColor,
                                            calculatedOrbPositionX1, calculatedOrbPositionY1, renderedDotSize);
        graphicalRenderer.drawSpriteBlended(layoutPixels, sceneColorBuffer, glowSprite, secondOrbColor,
                                            calculatedOrbPositionX2, calculatedOrbPositionY2, renderedDotSize);

        for (size_t arrayTransferIndex = 0; arrayTransferIndex < layoutPixels.size(); ++arrayTransferIndex) {
            const size_t targetVectorBaseIndex = payloadByteOffsets[arrayTransferIndex];
            const ColorValues& sourceColor = sceneColorBuffer[arrayTransferIndex];
            outgoingOpcPayload[targetVectorBaseIndex] = sourceColor.redIntensity;
            outgoingOpcPayload[targetVectorBaseIndex + 1] = sourceColor.greenIntensity;
            outgoingOpcPayload[targetVectorBaseIndex + 2] = sourceColor.blueIntensity;
        }

        masterNetworkClient.sendPixelPayload(0, outgoingOpcPayload.data(), outgoingOpcPayload.size());

        // Accumulated deadline keeps the true frame rate at 60, not 62.5, and
        // resynchronises rather than spiralling if a frame runs long.
        nextFrameDeadline += framePeriod;
        const auto rightNow = SteadyClock::now();
        if (nextFrameDeadline < rightNow) nextFrameDeadline = rightNow;
        std::this_thread::sleep_until(nextFrameDeadline);
    }
    return 0;
}
