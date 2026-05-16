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
#include <algorithm>

constexpr const char* TARGET_NETWORK_HOST = "127.0.0.1";
constexpr int TARGET_NETWORK_PORT = 7890;
constexpr int REQUIRED_TARGET_FPS = 60;

constexpr float SCENE_CENTER_X = 0.5f;        
constexpr float SCENE_CENTER_Y = 0.5f;        
constexpr float KINETIC_ORBIT_RADIUS = 0.25f;   
constexpr float KINETIC_ORBIT_SPEED = 1.5f;     
constexpr float RENDERED_DOT_SIZE = 0.75f;       

struct ColorValues { uint8_t redIntensity, greenIntensity, blueIntensity; };

struct ImageSprite {
    int spriteWidth = 0, spriteHeight = 0;
    std::vector<ColorValues> decodedPixelsVector;

    void loadSpriteFromDisk(const std::string& systemFilePath) {
        int colorChannelsAmount;
        unsigned char* rawStbiDataPointer = stbi_load(systemFilePath.c_str(), &spriteWidth, &spriteHeight, &colorChannelsAmount, 3);
        if (rawStbiDataPointer) {
            decodedPixelsVector.clear();
            for (int rawMemoryIndex = 0; rawMemoryIndex < spriteWidth * spriteHeight * 3; rawMemoryIndex += 3) {
                decodedPixelsVector.push_back({rawStbiDataPointer[rawMemoryIndex], rawStbiDataPointer[rawMemoryIndex+1], rawStbiDataPointer[rawMemoryIndex+2]});
            }
            stbi_image_free(rawStbiDataPointer);
        }
    }

    ColorValues sampleTexturePixel(float textureCoordinateU, float textureCoordinateV) const {
        int pixelCoordinateX = std::clamp(static_cast<int>(textureCoordinateU * spriteWidth), 0, spriteWidth - 1);
        int pixelCoordinateY = std::clamp(static_cast<int>(textureCoordinateV * spriteHeight), 0, spriteHeight - 1);
        return decodedPixelsVector[pixelCoordinateY * spriteWidth + pixelCoordinateX];
    }
};

class SceneRenderer {
public:
    void drawGraphicToBuffer(const LedLayout& physicalSystemLayout, std::vector<ColorValues>& renderingColorBuffer, const ImageSprite& targetGraphicSprite, float positionCenterX, float positionCenterY, float scaledDrawSize) {
        if (targetGraphicSprite.decodedPixelsVector.empty()) return;
        float halfSizeBoundary = scaledDrawSize / 2.0f;

        for (size_t hardwarePixelIndex = 0; hardwarePixelIndex < physicalSystemLayout.getPixels().size(); ++hardwarePixelIndex) {
            float distanceDeltaX = physicalSystemLayout.getPixels()[hardwarePixelIndex].normX - positionCenterX;
            float distanceDeltaY = physicalSystemLayout.getPixels()[hardwarePixelIndex].normY - positionCenterY;

            if (distanceDeltaX >= -halfSizeBoundary && distanceDeltaX <= halfSizeBoundary && distanceDeltaY >= -halfSizeBoundary && distanceDeltaY <= halfSizeBoundary) {
                float mappedTextureU = (distanceDeltaX + halfSizeBoundary) / scaledDrawSize;
                float mappedTextureV = (distanceDeltaY + halfSizeBoundary) / scaledDrawSize;
                ColorValues sampledSpriteColor = targetGraphicSprite.sampleTexturePixel(mappedTextureU, mappedTextureV);
                
                renderingColorBuffer[hardwarePixelIndex].redIntensity = std::min((int)renderingColorBuffer[hardwarePixelIndex].redIntensity + sampledSpriteColor.redIntensity, 255);
                renderingColorBuffer[hardwarePixelIndex].greenIntensity = std::min((int)renderingColorBuffer[hardwarePixelIndex].greenIntensity + sampledSpriteColor.greenIntensity, 255);
                renderingColorBuffer[hardwarePixelIndex].blueIntensity = std::min((int)renderingColorBuffer[hardwarePixelIndex].blueIntensity + sampledSpriteColor.blueIntensity, 255);
            }
        }
    }
};

int main(int argumentCount, char** argumentValues) {
    LedLayout operationalLayout;
    std::string providedArgumentData = (argumentCount > 1) ? argumentValues[1] : "8x8";
    operationalLayout.parseArgument(providedArgumentData);

    OPCClient masterNetworkClient(TARGET_NETWORK_HOST, TARGET_NETWORK_PORT);
    SceneRenderer graphicalRenderer;
    ImageSprite greenOrbSprite, purpleOrbSprite;
    
    greenOrbSprite.loadSpriteFromDisk("green_dot.png");
    purpleOrbSprite.loadSpriteFromDisk("purple_dot.png");

    std::vector<ColorValues> sceneColorBuffer(operationalLayout.getPixels().size(), {0, 0, 0});
    
    int maximumObservedHardwareIndex = 0;
    for (const auto& physicalSystemPixel : operationalLayout.getPixels()) {
        if (physicalSystemPixel.opcIndex > maximumObservedHardwareIndex) {
            maximumObservedHardwareIndex = physicalSystemPixel.opcIndex;
        }
    }
    std::vector<uint8_t> outgoingOpcPayload((maximumObservedHardwareIndex + 1) * 3, 0);
    
    auto systemExecutionStartTime = std::chrono::steady_clock::now();

    while (true) {
        auto activeFrameStartTime = std::chrono::steady_clock::now();
        std::fill(sceneColorBuffer.begin(), sceneColorBuffer.end(), ColorValues{0, 0, 0});

        float totalElapsedApplicationSeconds = std::chrono::duration<float>(activeFrameStartTime - systemExecutionStartTime).count() * KINETIC_ORBIT_SPEED;

        float calculatedOrbPositionX1 = SCENE_CENTER_X + cosf(totalElapsedApplicationSeconds) * KINETIC_ORBIT_RADIUS;
        float calculatedOrbPositionY1 = SCENE_CENTER_Y + sinf(totalElapsedApplicationSeconds) * KINETIC_ORBIT_RADIUS;
        float calculatedOrbPositionX2 = SCENE_CENTER_X + cosf(totalElapsedApplicationSeconds + M_PI) * KINETIC_ORBIT_RADIUS;
        float calculatedOrbPositionY2 = SCENE_CENTER_Y + sinf(totalElapsedApplicationSeconds + M_PI) * KINETIC_ORBIT_RADIUS;

        graphicalRenderer.drawGraphicToBuffer(operationalLayout, sceneColorBuffer, greenOrbSprite, calculatedOrbPositionX1, calculatedOrbPositionY1, RENDERED_DOT_SIZE);
        graphicalRenderer.drawGraphicToBuffer(operationalLayout, sceneColorBuffer, purpleOrbSprite, calculatedOrbPositionX2, calculatedOrbPositionY2, RENDERED_DOT_SIZE);

        std::fill(outgoingOpcPayload.begin(), outgoingOpcPayload.end(), 0);
        for (size_t arrayTransferIndex = 0; arrayTransferIndex < operationalLayout.getPixels().size(); arrayTransferIndex++) {
            int targetVectorBaseIndex = operationalLayout.getPixels()[arrayTransferIndex].opcIndex * 3;
            outgoingOpcPayload[targetVectorBaseIndex]     = sceneColorBuffer[arrayTransferIndex].redIntensity;
            outgoingOpcPayload[targetVectorBaseIndex + 1] = sceneColorBuffer[arrayTransferIndex].greenIntensity;
            outgoingOpcPayload[targetVectorBaseIndex + 2] = sceneColorBuffer[arrayTransferIndex].blueIntensity;
        }

        masterNetworkClient.sendPixelPayload(0, outgoingOpcPayload.data(), outgoingOpcPayload.size());
        std::this_thread::sleep_until(activeFrameStartTime + std::chrono::milliseconds(1000 / REQUIRED_TARGET_FPS));
    }
    return 0;
}