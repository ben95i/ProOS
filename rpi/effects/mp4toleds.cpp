#include <iostream>
#include <vector>
#include <string>
#include <opencv2/opencv.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cmath>
#include <chrono>
#include <thread>
#include <signal.h>
#include <algorithm> // For std::clamp

#define OPC_IP "127.0.0.1"
#define OPC_PORT 7890
#define MAX_OPC_PIXELS 21845 

struct PixelMap {
    int opc_index;
    float norm_x; 
    float norm_y; 
};

// Global State
std::vector<PixelMap> g_pixel_map;
float g_target_aspect = 1.0f;
volatile bool g_keep_running = true;

void signal_handler(int s) { g_keep_running = false; }

// --- MAPPING GENERATORS ---

void init_grid(int rows, int cols) {
    g_pixel_map.clear();
    g_target_aspect = (float)cols / (float)rows;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            PixelMap p;
            p.opc_index = (y * cols) + x;
            p.norm_x = (cols > 1) ? (float)x / (cols - 1) : 0.5f;
            p.norm_y = (rows > 1) ? (float)y / (rows - 1) : 0.5f;
            g_pixel_map.push_back(p);
        }
    }
}

void init_8x60_strips() {
    g_pixel_map.clear();
    g_target_aspect = 60.0f / 8.0f;
    for (int strip = 0; strip < 8; strip++) {
        for (int led = 0; led < 60; led++) {
            PixelMap p;
            p.opc_index = (strip * 60) + led;
            p.norm_x = (float)led / 59.0f;
            p.norm_y = (float)strip / 7.0f;
            g_pixel_map.push_back(p);
        }
    }
}

void init_93_ring() {
    struct RingDef { int start; int count; };
    std::vector<RingDef> rings = {{0,1}, {8,8}, {20,12}, {36,16}, {60,24}, {92,32}};
    g_pixel_map.clear();
    g_target_aspect = 1.0f;
    for (int r = 0; r < (int)rings.size(); r++) {
        float radius = (rings.size() > 1) ? (float)r / (rings.size() - 1) * 0.48f : 0.0f;
        for (int i = 0; i < rings[r].count; i++) {
            float angle = (float)i / rings[r].count * 2.0f * M_PI;
            PixelMap p;
            p.opc_index = rings[r].start + i;
            p.norm_x = 0.5f + radius * std::cos(angle);
            p.norm_y = 0.5f + radius * std::sin(angle);
            g_pixel_map.push_back(p);
        }
    }
}

// --- MAIN EXECUTION ---

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cout << "Usage: ./video_leds <mode> <video_path>" << std::endl;
        return 1;
    }

    signal(SIGINT, signal_handler);

    // 1. Initialize Mapping
    std::string mode = argv[1];
    if (mode == "93ring") init_93_ring();
    else if (mode == "8x60strips") init_8x60_strips();
    else if (mode == "16x16grid") init_grid(16, 16);
    else if (mode == "8x8grid") init_grid(8, 8);
    else { std::cerr << "Unknown mode: " << mode << std::endl; return 1; }

    // 2. Open Video
    cv::VideoCapture cap(argv[2]);
    if (!cap.isOpened()) {
        std::cerr << "Error: Could not open video." << std::endl;
        return 1;
    }

    // 3. Network Setup
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in serv_addr = {0};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(OPC_PORT);
    inet_pton(AF_INET, OPC_IP, &serv_addr.sin_addr);
    
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Error: Could not connect to OPC server." << std::endl;
        return 1;
    }

    // 4. Prepare OPC Packet Buffer
    int max_idx = 0;
    for (const auto& p : g_pixel_map) if (p.opc_index > max_idx) max_idx = p.opc_index;
    if (max_idx >= MAX_OPC_PIXELS) {
        std::cerr << "Index out of OPC bounds." << std::endl;
        return 1;
    }

    int payload_len = (max_idx + 1) * 3;
    std::vector<uint8_t> packet(4 + payload_len, 0);
    packet[2] = (payload_len >> 8) & 0xFF;
    packet[3] = payload_len & 0xFF;

    // 5. Timing and LUT State
    const std::chrono::nanoseconds target_duration(1000000000 / 30);
    std::vector<size_t> lut_offsets;
    int last_w = -1, last_h = -1;
    cv::Mat frame;

    std::cout << "Streaming at 30Hz. Press Ctrl+C to stop." << std::endl;

    while (g_keep_running) {
        auto frame_start = std::chrono::steady_clock::now();

        cap >> frame;
        if (frame.empty()) {
            cap.set(cv::CAP_PROP_POS_FRAMES, 0); // Loop video
            continue;
        }

        // Re-calculate LUT only if resolution changes
        if (frame.cols != last_w || frame.rows != last_h) {
            lut_offsets.clear();
            float video_aspect = (float)frame.cols / (float)frame.rows;
            int cw = frame.cols, ch = frame.rows, ox = 0, oy = 0;

            if (video_aspect > g_target_aspect) {
                cw = (int)(frame.rows * g_target_aspect);
                ox = (frame.cols - cw) / 2;
            } else {
                ch = (int)(frame.cols / g_target_aspect);
                oy = (frame.rows - ch) / 2;
            }

            for (const auto& p : g_pixel_map) {
                int px = std::clamp(ox + (int)(p.norm_x * (cw - 1)), 0, frame.cols - 1);
                int py = std::clamp(oy + (int)(p.norm_y * (ch - 1)), 0, frame.rows - 1);
                lut_offsets.push_back((py * frame.cols + px) * 3);
            }
            last_w = frame.cols; last_h = frame.rows;
        }

        // Efficient color extraction
        uint8_t* raw_data = frame.data;
        for (size_t i = 0; i < g_pixel_map.size(); ++i) {
            uint8_t* bgr = &raw_data[lut_offsets[i]];
            int base = 4 + (g_pixel_map[i].opc_index * 3);
            packet[base]     = bgr[2]; // R
            packet[base + 1] = bgr[1]; // G
            packet[base + 2] = bgr[0]; // B
        }

        if (send(sock, packet.data(), packet.size(), MSG_NOSIGNAL) < 0) {
            std::cerr << "Lost connection." << std::endl;
            break;
        }

        // 30Hz Frame Sync
        auto frame_end = std::chrono::steady_clock::now();
        auto elapsed = frame_end - frame_start;
        if (elapsed < target_duration) {
            std::this_thread::sleep_until(frame_start + target_duration);
        }
    }

    // Cleanup: Turn off LEDs on exit
    std::fill(packet.begin() + 4, packet.end(), 0);
    send(sock, packet.data(), packet.size(), MSG_NOSIGNAL);
    
    std::cout << "\nShutdown complete." << std::endl;
    close(sock);
    return 0;
}
