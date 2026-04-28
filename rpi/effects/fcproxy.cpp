#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cmath>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <signal.h>
#include <chrono>
#include <poll.h>

// --- Configuration Constants ---
#define OPC_LISTEN_PORT 7890
#define OPC_TARGET_PORT 7891
#define HTTP_LISTEN_PORT 7892
#define TARGET_IP "127.0.0.1"

const int INITIAL_BRIGHTNESS = 178; 
const float MIN_BRIGHTNESS_FLOOR = 0.1f; 
const size_t OPC_HEADER_LEN = 4;
const float GLOBAL_GAMMA = 2.5f;

constexpr std::chrono::milliseconds HTTP_RATE_LIMIT{100};

// --- Global State ---

// Thread-safe brightness tracker and state versioning
std::atomic<int> g_current_brightness{INITIAL_BRIGHTNESS};
std::atomic<uint32_t> g_brightness_version{0}; 

// Track how many OPC clients are currently connected
std::atomic<int> g_active_clients{0};

// Frame Caching (Double Buffering Concept)
std::mutex g_cache_mutex;
std::vector<uint8_t> g_last_raw_payload;
uint8_t g_last_header[OPC_HEADER_LEN] = {0};

// Gamma Lookup Table
uint8_t g_lookup_table[256];
std::mutex g_table_mutex;

// Rate Limiting State
std::mutex g_rate_limit_mutex;
auto g_last_request_time = std::chrono::steady_clock::now() - HTTP_RATE_LIMIT;

// --- Logic Functions ---

void update_gamma_table(float brightness_normalized) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    for (int i = 0; i < 256; i++) {
        float scaled = std::pow((float)i / 255.0f, GLOBAL_GAMMA) * brightness_normalized;
        g_lookup_table[i] = static_cast<uint8_t>(std::round(scaled * 255.0f));
    }
}

// --- HTTP LISTENER ---
void web_listener() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_LISTEN_PORT);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        // Rate Limiter logic
        {
            std::lock_guard<std::mutex> lock(g_rate_limit_mutex);
            auto now = std::chrono::steady_clock::now();
            if (now - g_last_request_time < HTTP_RATE_LIMIT) {
                std::string busy = "HTTP/1.1 200 BUSY\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\nBUSY";
                send(client_fd, busy.c_str(), busy.length(), 0);
                close(client_fd);
                continue;
            }
            g_last_request_time = now;
        }

        char buffer[1024] = {0};
        read(client_fd, buffer, sizeof(buffer) - 1);
        std::string request(buffer);
        std::string response;

        if (request.find("GET /brightness ") != std::string::npos) {
            std::string body = std::to_string(g_current_brightness.load());
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.length()) + "\r\n\r\n" + body;
        } 
        else if (size_t pos = request.find("b="); pos != std::string::npos) {
            try {
                float raw_val = std::stof(request.substr(pos + 2));
                float input_ratio = std::clamp(raw_val / 255.0f, 0.0f, 1.0f);
                float normalized = MIN_BRIGHTNESS_FLOOR + (input_ratio * (1.0f - MIN_BRIGHTNESS_FLOOR));
                
                update_gamma_table(normalized);
                g_current_brightness.store(static_cast<int>(std::round(raw_val)));
                
                // Flag to the active proxy threads that they need to resend their cache
                g_brightness_version.fetch_add(1);

                // --- FALLBACK LOGIC ---
                // If there are no active clients sending frames, push the update manually
                if (g_active_clients.load() == 0) {
                    std::vector<uint8_t> cached_payload;
                    uint8_t cached_header[OPC_HEADER_LEN];
                    uint8_t local_table[256];

                    // Grab the latest cached frame safely
                    {
                        std::lock_guard<std::mutex> cache_lock(g_cache_mutex);
                        if (!g_last_raw_payload.empty()) {
                            cached_payload = g_last_raw_payload;
                            std::copy(g_last_header, g_last_header + OPC_HEADER_LEN, cached_header);
                        }
                    }

                    if (!cached_payload.empty()) {
                        {
                            std::lock_guard<std::mutex> table_lock(g_table_mutex);
                            std::copy(std::begin(g_lookup_table), std::end(g_lookup_table), std::begin(local_table));
                        }
                        
                        for (auto &pixel : cached_payload) {
                            pixel = local_table[pixel];
                        }

                        // Temporarily connect to the target to send the frame
                        int temp_fd = socket(AF_INET, SOCK_STREAM, 0);
                        if (temp_fd >= 0) {
                            struct sockaddr_in target_addr{};
                            target_addr.sin_family = AF_INET;
                            target_addr.sin_port = htons(OPC_TARGET_PORT);
                            inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);

                            if (connect(temp_fd, (struct sockaddr*)&target_addr, sizeof(target_addr)) == 0) {
                                send(temp_fd, cached_header, OPC_HEADER_LEN, 0);
                                send(temp_fd, cached_payload.data(), cached_payload.size(), 0);
                            }
                            close(temp_fd);
                        }
                    }
                }
                
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK\n";
            } catch (...) {
                response = "HTTP/1.1 400 Bad Request\r\n\r\nInvalid Value\n";
            }
        } 
        else {
            response = "HTTP/1.1 404 Not Found\r\n\r\nNot Found\n";
        }

        send(client_fd, response.c_str(), response.length(), 0);
        close(client_fd);
    }
}

// --- OPC PROXY ---
void handle_opc_client(int client_fd, int server_fd) {
    // RAII helper to safely track active clients even if an error causes an early return
    struct ClientTracker {
        ClientTracker() { g_active_clients.fetch_add(1); }
        ~ClientTracker() { g_active_clients.fetch_sub(1); }
    } tracker;

    uint8_t header[OPC_HEADER_LEN];
    std::vector<uint8_t> payload; 
    uint8_t local_table[256];
    
    uint32_t local_brightness_version = g_brightness_version.load();

    struct pollfd pfd;
    pfd.fd = client_fd;
    pfd.events = POLLIN;

    while (true) {
        // 1. Check if the HTTP thread updated the brightness state
        if (local_brightness_version != g_brightness_version.load()) {
            local_brightness_version = g_brightness_version.load();
            
            std::vector<uint8_t> cached_payload;
            uint8_t cached_header[OPC_HEADER_LEN];

            // Safely fetch the last known raw frame
            {
                std::lock_guard<std::mutex> cache_lock(g_cache_mutex);
                if (!g_last_raw_payload.empty()) {
                    cached_payload = g_last_raw_payload;
                    std::copy(g_last_header, g_last_header + OPC_HEADER_LEN, cached_header);
                }
            }

            // If we have cached data, process and send it
            if (!cached_payload.empty()) {
                {
                    std::lock_guard<std::mutex> table_lock(g_table_mutex);
                    std::copy(std::begin(g_lookup_table), std::end(g_lookup_table), std::begin(local_table));
                }
                
                for (auto &pixel : cached_payload) {
                    pixel = local_table[pixel];
                }

                if (send(server_fd, cached_header, OPC_HEADER_LEN, 0) <= 0) break;
                if (send(server_fd, cached_payload.data(), cached_payload.size(), 0) <= 0) break;
            }
        }

        // 2. Poll the socket with a 50ms timeout to allow state checks
        int poll_res = poll(&pfd, 1, 50); 
        if (poll_res < 0) break;       // Error occurred
        if (poll_res == 0) continue;   // Timeout reached, loop back around to check version

        // 3. Process incoming frame since poll says data is ready
        if (recv(client_fd, header, OPC_HEADER_LEN, MSG_WAITALL) <= 0) break;

        uint16_t length = (static_cast<uint16_t>(header[2]) << 8) | header[3];
        if (payload.size() < length) payload.resize(length);

        if (length > 0) {
            if (recv(client_fd, payload.data(), length, MSG_WAITALL) <= 0) break;
        }
        
        if (header[1] == 0) { 
            // Update global raw cache
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                std::copy(header, header + OPC_HEADER_LEN, g_last_header);
                g_last_raw_payload.assign(payload.begin(), payload.begin() + length);
            }

            // Apply gamma correction using local stack table
            {
                std::lock_guard<std::mutex> lock(g_table_mutex);
                std::copy(std::begin(g_lookup_table), std::end(g_lookup_table), std::begin(local_table));
            }

            for (size_t i = 0; i < length; ++i) {
                payload[i] = local_table[payload[i]];
            }
        }

        // Send the processed payload
        if (send(server_fd, header, OPC_HEADER_LEN, 0) <= 0) break;
        if (length > 0) {
            if (send(server_fd, payload.data(), length, 0) <= 0) break;
        }
    }
}

int main() {
    // Prevent the program from crashing if a client disconnects unexpectedly
    signal(SIGPIPE, SIG_IGN);

    // Initial gamma table calculation
    float start_ratio = (float)INITIAL_BRIGHTNESS / 255.0f;
    float start_normalized = MIN_BRIGHTNESS_FLOOR + (start_ratio * (1.0f - MIN_BRIGHTNESS_FLOOR));
    update_gamma_table(start_normalized);

    // Launch background listener
    std::thread(web_listener).detach();

    int proxy_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in proxy_addr{};
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(OPC_LISTEN_PORT);

    if (bind(proxy_fd, (struct sockaddr*)&proxy_addr, sizeof(proxy_addr)) < 0) {
        std::cerr << "Fatal: Could not bind OPC port." << std::endl;
        return 1;
    }

    listen(proxy_fd, 10);
    std::cout << "OPC Proxy Active on Port " << OPC_LISTEN_PORT << std::endl;

    while (true) {
        int client_fd = accept(proxy_fd, NULL, NULL);
        if (client_fd < 0) continue;

        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in target_addr{};
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(OPC_TARGET_PORT);
        inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);

        if (connect(server_fd, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
            close(client_fd);
            close(server_fd);
            continue;
        }

        // Handle each client in a separate thread
        std::thread([client_fd, server_fd]() {
            handle_opc_client(client_fd, server_fd);
            close(client_fd);
            close(server_fd);
        }).detach();
    }

    return 0;
}