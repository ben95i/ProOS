#pragma once
#include <vector>
#include <string>

struct LEDPixel {
    int opc_index;
    float norm_x; // Normalized X coordinate (0.0f to 1.0f)
    float norm_y; // Normalized Y coordinate (0.0f to 1.0f)
};

class LEDLayout {
public:
    int logicalWidth = 0;
    int logicalHeight = 0;
    float targetAspect = 1.0f;
    std::vector<LEDPixel> pixels;

    void parseArgument(const std::string& arg);

private:
    void setupGrid(int cols, int rows);
    void setupStrips(int numStrips, int ledsPerStrip);
    void setup93Ring();
};
