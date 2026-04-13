#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cmath>

#define OPC_IP "127.0.0.1"
#define OPC_PORT 7890

struct PixelMap {
    int opc_index;
    float norm_x; 
    float norm_y; 
};

std::vector<PixelMap> g_pixel_map;
int g_max_opc_index = 0;
float g_target_aspect = 1.0f; // Global aspect ratio for the current mode

// --- MAPPING GENERATORS ---

void init_grid(int rows, int cols) {
    g_pixel_map.clear();
    g_max_opc_index = 0;
    g_target_aspect = (float)cols / (float)rows; // e.g., 16/16 = 1.0
    
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            PixelMap p;
            p.opc_index = (y * cols) + x;
            p.norm_x = (float)x / (cols - 1);
            p.norm_y = (float)y / (rows - 1);
            g_pixel_map.push_back(p);
            if (p.opc_index > g_max_opc_index) g_max_opc_index = p.opc_index;
        }
    }
}

void init_8x60_strips() {
    g_pixel_map.clear();
    g_max_opc_index = 0;
    g_target_aspect = 60.0f / 8.0f; // Aspect ratio of 7.5
    
    for (int strip = 0; strip < 8; strip++) {
        for (int led = 0; led < 60; led++) {
            PixelMap p;
            p.opc_index = (strip * 60) + led;
            p.norm_x = (float)led / 59.0f;
            p.norm_y = (float)strip / 7.0f;
            g_pixel_map.push_back(p);
            if (p.opc_index > g_max_opc_index) g_max_opc_index = p.opc_index;
        }
    }
}

void init_93_ring() {
    struct RingDef { int start; int count; };
    std::vector<RingDef> rings = {{0,1}, {8,8}, {20,12}, {36,16}, {60,24}, {92,32}};
    g_pixel_map.clear();
    g_max_opc_index = 0;
    g_target_aspect = 1.0f; // Rings are a perfect square (1:1 aspect ratio)
    
    for (int r = 0; r < rings.size(); r++) {
        float radius = (float)r / (rings.size() - 1) * 0.48f; // Stay slightly inside edges
        for (int i = 0; i < rings[r].count; i++) {
            float angle = (float)i / rings[r].count * 2.0f * M_PI;
            PixelMap p;
            p.opc_index = rings[r].start + i;
            p.norm_x = 0.5f + radius * cos(angle);
            p.norm_y = 0.5f + radius * sin(angle);
            g_pixel_map.push_back(p);
            if (p.opc_index > g_max_opc_index) g_max_opc_index = p.opc_index;
        }
    }
}

// --- MAIN LOOP ---

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: ./video_leds <mode> <video_path>" << std::endl;
        std::cout << "Modes: 93ring, 8x60strips, 16x16grid, 8x8grid" << std::endl;
        return 1;
    }

    // 1. Handle Mode Selection
    std::string mode = argv[1];
    if (mode == "93ring") init_93_ring();
    else if (mode == "8x60strips") init_8x60_strips();
    else if (mode == "16x16grid") init_grid(16, 16);
    else if (mode == "8x8grid") init_grid(8, 8);
    else { std::cerr << "Unknown mode: " << mode << std::endl; return 1; }

    std::string videoPath = argv[2];
    cv::VideoCapture cap(videoPath);
    if (!cap.isOpened()) {
        std::cerr << "Error: Could not open video file " << videoPath << std::endl;
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(OPC_PORT);
    inet_pton(AF_INET, OPC_IP, &serv_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Error: Could not connect to fcserver. Is it running?" << std::endl;
        return 1;
    }

    cv::Mat frame;
    int payload_len = (g_max_opc_index + 1) * 3;
    std::vector<uint8_t> packet(4 + payload_len, 0);
    packet[0] = 0; packet[1] = 0;
    packet[2] = (payload_len >> 8) & 0xFF;
    packet[3] = payload_len & 0xFF;

    std::cout << "Streaming " << videoPath << " centered on " << mode << " mode..." << std::endl;

    while (true) {
        cap >> frame;
        if (frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); 
            continue;
        }

        // --- CENTERING AND ASPECT RATIO LOGIC ---
        float video_aspect = (float)frame.cols / (float)frame.rows;
        int crop_w = frame.cols;
        int crop_h = frame.rows;
        int offset_x = 0;
        int offset_y = 0;

        if (video_aspect > g_target_aspect) {
            // Video is too wide (e.g., 16:9 video on a 1:1 ring) -> Crop left and right sides
            crop_w = frame.rows * g_target_aspect;
            offset_x = (frame.cols - crop_w) / 2;
        } else {
            // Video is too tall -> Crop top and bottom
            crop_h = frame.cols / g_target_aspect;
            offset_y = (frame.rows - crop_h) / 2;
        }

        // Apply Mapping using the dynamically centered Region of Interest
        for (const auto& p : g_pixel_map) {
            int px = offset_x + (int)(p.norm_x * (crop_w - 1));
            int py = offset_y + (int)(p.norm_y * (crop_h - 1));
            
            // Safety clamp to ensure we never read outside the frame memory
            px = std::max(0, std::min(frame.cols - 1, px));
            py = std::max(0, std::min(frame.rows - 1, py));

            cv::Vec3b color = frame.at<cv::Vec3b>(py, px);
            int base = 4 + (p.opc_index * 3);
            packet[base]     = color[2]; // R
            packet[base + 1] = color[1]; // G
            packet[base + 2] = color[0]; // B
        }

        if (send(sock, packet.data(), packet.size(), MSG_NOSIGNAL) < 0) {
            std::cerr << "Connection lost to fcserver." << std::endl;
            break;
        }
        
        usleep(33333); // framerate (~30 FPS)
    }

    close(sock);
    return 0;
}