#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pwd.h>
#include <algorithm>
#include <cmath> 
#include <ws2811/ws2811.h> 

// --- Hardware & System Configuration ---
constexpr int   DEFAULT_GPIO_PIN    = 18;       // RPi PWM0
constexpr int   DEFAULT_DMA_CH      = 10;
constexpr int   STRIP_TYPE          = WS2811_STRIP_GRB;
constexpr float PERCEPTUAL_GAMMA    = 2.8f;     // Expands the low-brightness range
constexpr const char* RUNAS_USER    = "nobody";

// --- Network Configuration ---
constexpr int   OPC_PORT            = 7890;
constexpr int   HTTP_PORT           = 7892;
constexpr const char* BIND_IP       = "127.0.0.1";

// --- Protocol & Timing Constants ---
constexpr int   OPC_HEADER_SIZE     = 4;
constexpr int   OPC_CMD_SET_PIXELS  = 0;
constexpr int   RGB_CHANNELS        = 3;
constexpr float TARGET_FPS          = 60.0f;
constexpr auto  FRAME_BUDGET        = std::chrono::microseconds(static_cast<long long>(1000000 / TARGET_FPS));
constexpr auto  HTTP_MIN_INTERVAL   = std::chrono::milliseconds(50);
constexpr uint16_t SENTINEL_INVALID = 0xFFFF;

// --- Global Dynamic State ---
int g_width             = 16;  
int g_height            = 16;
int g_num_leds          = 256;
int g_hw_master_limit   = 255; // Set by argv[2], mapped to hardware brightness
int g_rotation          = 0;   // Set by argv[3] (0, 90, 180, 270)

std::atomic<bool> g_running{true};
ws2811_t g_ledstring; 
std::mutex g_hw_mutex; // Protects hardware access and frame cache

// Software LUT and Cache
std::atomic<int> g_api_brightness{255}; // User-facing 0-255 scale
uint8_t g_gamma_table[256];
std::vector<uint8_t> g_pixel_cache; // Stores the last raw frame for immediate redraws
int g_cache_count = 0;

// --- Logic ---

void signal_handler(int) { g_running = false; }

void drop_privileges() {
    struct passwd* pw = getpwnam(RUNAS_USER);
    if (pw && (setgid(pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0)) {
        std::cerr << "Note: Running as limited user: " << RUNAS_USER << "\n";
    }
}

/**
 * High-precision Gamma LUT generation.
 * Maps the 0-255 API input to the 0-255 color output.
 */
void update_gamma_table(int software_level) {
    float intensity = static_cast<float>(software_level) / 255.0f;
    for (int i = 0; i < 256; ++i) {
        float normalized = static_cast<float>(i) / 255.0f;
        // Apply Gamma 2.8 and scale by the current software brightness level
        float val = std::pow(normalized, PERCEPTUAL_GAMMA) * intensity * 255.0f;
        g_gamma_table[i] = static_cast<uint8_t>(std::clamp(std::round(val), 0.0f, 255.0f));
    }
}

/**
 * Maps the incoming linear OPC index to the physical Zig-Zag layout,
 * factoring in the user-defined rotation.
 */
uint16_t map_pixel(uint16_t idx) {
    if (idx >= (uint16_t)g_num_leds) return SENTINEL_INVALID;

    // 1. Calculate incoming logical source dimensions based on rotation
    uint16_t src_w = (g_rotation == 90 || g_rotation == 270) ? g_height : g_width;
    
    // 2. Find the logical X/Y of the incoming pixel
    uint16_t src_x = idx % src_w;
    uint16_t src_y = idx / src_w;

    uint16_t px, py;

    // 3. Rotate logical coordinates to physical matrix coordinates
    switch(g_rotation) {
        case 90:
            px = (g_width - 1) - src_y;
            py = src_x;
            break;
        case 180:
            px = (g_width - 1) - src_x;
            py = (g_height - 1) - src_y;
            break;
        case 270:
            px = src_y;
            py = (g_height - 1) - src_x;
            break;
        case 0:
        default:
            px = src_x;
            py = src_y;
            break;
    }

    // 4. Map the physical coordinates to the hardwired Zig-Zag string
    // Top-Right origin means row 0 goes Right to Left.
    uint16_t phys_x = (py % 2 == 0) ? ((g_width - 1) - px) : px;
    return (py * g_width) + phys_x;
}

// --- HTTP Control Server ---
void web_listener() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(HTTP_PORT);
    inet_pton(AF_INET, BIND_IP, &addr.sin_addr);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(server_fd);
        return;
    }
    listen(server_fd, 5);

    auto last_req = std::chrono::steady_clock::now() - HTTP_MIN_INTERVAL;

    while (g_running) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) continue;

        auto now = std::chrono::steady_clock::now();
        if (now - last_req < HTTP_MIN_INTERVAL) {
            close(client_fd);
            continue;
        }
        last_req = now;

        char buf[1024] = {0};
        ssize_t bytes = read(client_fd, buf, sizeof(buf) - 1);
        if (bytes <= 0) { close(client_fd); continue; }

        std::string req(buf);
        std::string res = "HTTP/1.1 404 Not Found\r\n\r\n";

        // Query Brightness
        if (req.find("GET /brightness") != std::string::npos) {
            std::string val = std::to_string(g_api_brightness.load());
            res = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + 
                  std::to_string(val.length()) + "\r\n\r\n" + val;
        }
        // Set Brightness
        else if (size_t pos = req.find("b="); pos != std::string::npos) {
            try {
                int val = std::clamp(std::stoi(req.substr(pos + 2)), 0, 255);
                g_api_brightness.store(val);
                
                {
                    std::lock_guard<std::mutex> lock(g_hw_mutex);
                    update_gamma_table(val);
                    
                    // Immediate re-render of current pixels with new curve
                    for (int i = 0; i < g_cache_count; ++i) {
                        uint16_t p = map_pixel(i);
                        if (p == SENTINEL_INVALID) continue;
                        g_ledstring.channel[0].leds[p] = 
                            (g_gamma_table[g_pixel_cache[i*3]] << 16) |
                            (g_gamma_table[g_pixel_cache[i*3+1]] << 8) |
                            (g_gamma_table[g_pixel_cache[i*3+2]]);
                    }
                    ws2811_render(&g_ledstring);
                }
                res = "HTTP/1.1 200 OK\r\n\r\nOK\n";
            } catch (...) { res = "HTTP/1.1 400 Bad Request\r\n\r\n"; }
        }

        send(client_fd, res.c_str(), res.length(), 0);
        close(client_fd);
    }
    close(server_fd);
}

// --- Main OPC Server ---
int main(int argc, char* argv[]) {
    // 1. Grid Size Parse
    if (argc > 1) {
        if (sscanf(argv[1], "%dx%dgrid", &g_width, &g_height) == 2) {
            g_num_leds = g_width * g_height;
        }
    }
    // 2. Max Brightness Parse
    if (argc > 2) {
        g_hw_master_limit = std::clamp(std::stoi(argv[2]), 1, 255);
    }
    // 3. Rotation Parse
    if (argc > 3) {
        try {
            int rot = std::stoi(argv[3]);
            if (rot == 0 || rot == 90 || rot == 180 || rot == 270) {
                g_rotation = rot;
            } else {
                std::cerr << "Warning: Invalid rotation. Defaulting to 0.\n";
            }
        } catch (...) { std::cerr << "Warning: Invalid rotation argument. Defaulting to 0.\n"; }
    }

    std::cout << "Starting OPC Server...\n"
              << "Grid: " << g_width << "x" << g_height << " (" << g_num_leds << " LEDs)\n"
              << "Max Hardware Limit: " << g_hw_master_limit << "/255\n"
              << "Rotation Map: " << g_rotation << " Degrees\n";

    g_pixel_cache.resize(g_num_leds * RGB_CHANNELS, 0);
    update_gamma_table(255); // Initialize full software scale

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    // Initialize Hardware with Master Limit
    memset(&g_ledstring, 0, sizeof(ws2811_t));
    g_ledstring.freq = WS2811_TARGET_FREQ;
    g_ledstring.dmanum = DEFAULT_DMA_CH;
    g_ledstring.channel[0].gpionum = DEFAULT_GPIO_PIN;
    g_ledstring.channel[0].count = g_num_leds;
    g_ledstring.channel[0].strip_type = STRIP_TYPE;
    g_ledstring.channel[0].brightness = g_hw_master_limit; // Hybrid Master

    if (ws2811_init(&g_ledstring) != WS2811_SUCCESS) return 1;

    drop_privileges();
    std::thread(web_listener).detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(OPC_PORT);
    inet_pton(AF_INET, BIND_IP, &address.sin_addr);

    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) return 1;
    listen(server_fd, 1);

    uint8_t header[OPC_HEADER_SIZE];
    std::vector<uint8_t> rx_buf(g_num_leds * RGB_CHANNELS, 0);
    auto last_frame = std::chrono::steady_clock::now();

    while (g_running) {
        int client_sock = accept(server_fd, nullptr, nullptr);
        if (client_sock < 0) continue;

        while (g_running) {
            if (recv(client_sock, header, OPC_HEADER_SIZE, MSG_WAITALL) <= 0) break;
            
            uint16_t len = (header[2] << 8) | header[3];
            uint16_t to_read = std::min((uint16_t)(g_num_leds * RGB_CHANNELS), len);

            ssize_t bytes = recv(client_sock, rx_buf.data(), to_read, MSG_WAITALL);
            if (bytes <= 0) break;

            // Drain extra data into stack sink
            if (len > to_read) {
                int remain = len - to_read;
                char sink[1024];
                while (remain > 0) {
                    ssize_t d = recv(client_sock, sink, std::min(remain, 1024), 0);
                    if (d <= 0) break;
                    remain -= d;
                }
            }

            // Frame Rate Limiter (60Hz)
            auto now = std::chrono::steady_clock::now();
            if (now - last_frame < FRAME_BUDGET) continue; 
            last_frame = now;

            int pixels = bytes / RGB_CHANNELS;
            {
                std::lock_guard<std::mutex> lock(g_hw_mutex);
                g_cache_count = pixels;
                std::copy(rx_buf.begin(), rx_buf.begin() + bytes, g_pixel_cache.begin());

                for (int i = 0; i < pixels; ++i) {
                    uint16_t p = map_pixel(i);
                    if (p == SENTINEL_INVALID) continue;
                    g_ledstring.channel[0].leds[p] = 
                        (g_gamma_table[rx_buf[i*3]] << 16) | 
                        (g_gamma_table[rx_buf[i*3+1]] << 8) | 
                        (g_gamma_table[rx_buf[i*3+2]]);
                }
                ws2811_render(&g_ledstring);
            }
        }
        close(client_sock);
    }

    close(server_fd);
    ws2811_fini(&g_ledstring);
    return 0;
}