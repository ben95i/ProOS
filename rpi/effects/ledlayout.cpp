#include "ledlayout.h"
#include <iostream>
#include <cmath>
#include <cstdio>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void LEDLayout::parseArgument(const std::string& arg) {
    if (arg == "93ring") {
        setup93Ring();
    } else if (arg.find("strips") != std::string::npos) {
        int num = 8, len = 60;
        sscanf(arg.c_str(), "%dx%dstrips", &num, &len);
        setupStrips(num, len);
    } else {
        int w = 8, h = 8;
        if (arg.find("grid") != std::string::npos) {
            sscanf(arg.c_str(), "%dx%dgrid", &w, &h);
        } else {
            sscanf(arg.c_str(), "%dx%d", &w, &h);
        }
        setupGrid(w, h);
    }
}

void LEDLayout::setupGrid(int cols, int rows) {
    pixels.clear();
    logicalWidth = cols;
    logicalHeight = rows;
    targetAspect = (float)cols / (float)rows;

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            LEDPixel p;
            p.opc_index = (y * cols) + x;
            p.norm_x = (cols > 1) ? (float)x / (cols - 1) : 0.5f;
            p.norm_y = (rows > 1) ? (float)y / (rows - 1) : 0.5f;
            pixels.push_back(p);
        }
    }
    std::cout << "Initialized " << cols << " columns and " << rows << " rows of LEDs\n";
}

void LEDLayout::setupStrips(int numStrips, int ledsPerStrip) {
    pixels.clear();
    logicalWidth = ledsPerStrip;
    logicalHeight = numStrips;
    targetAspect = (float)ledsPerStrip / (float)numStrips;

    for (int s = 0; s < numStrips; s++) {
        for (int i = 0; i < ledsPerStrip; i++) {
            LEDPixel p;
            p.opc_index = (s * ledsPerStrip) + i;
            p.norm_x = (ledsPerStrip > 1) ? (float)i / (ledsPerStrip - 1) : 0.5f;
            p.norm_y = (numStrips > 1) ? (float)s / (numStrips - 1) : 0.5f;
            pixels.push_back(p);
        }
    }
    std::cout << "Initialized " << numStrips << " strips of " << ledsPerStrip << " LEDs\n";
}

void LEDLayout::setup93Ring() {
    pixels.clear();
    // Default virtual layout space constraints for non-grid setups
    logicalWidth = 16;
    logicalHeight = 16;
    targetAspect = 1.0f;
    
    struct RingDef { int start; int count; };
    std::vector<RingDef> rings = {{0,1}, {8,8}, {20,12}, {36,16}, {60,24}, {92,32}};
    
    for (int r = 0; r < (int)rings.size(); r++) {
        float radius = (rings.size() > 1) ? (float)r / (rings.size() - 1) * 0.48f : 0.0f;
        for (int i = 0; i < rings[r].count; i++) {
            float angle = (float)i / rings[r].count * 2.0f * M_PI;
            LEDPixel p;
            p.opc_index = rings[r].start + i;
            p.norm_x = 0.5f + radius * std::cos(angle);
            p.norm_y = 0.5f + radius * std::sin(angle);
            pixels.push_back(p);
        }
    }
    std::cout << "Initialized a ring of 93 LEDs\n";
}
