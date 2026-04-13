#include <iostream>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <cmath> // For pow()
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm> // Required for std::min / std::max
#include <signal.h>

#define OPC_LISTEN_PORT 7890
#define OPC_TARGET_PORT 7891
#define HTTP_LISTEN_PORT 7892
#define TARGET_IP "127.0.0.1"

// Track active outgoing connections to the LED server
std::mutex g_targets_mutex;
std::vector<int> g_active_target_fds;

const int INITIAL_BRIGHTNESS = 178; // initial brightness 178=70%
const float MIN_BRIGHTNESS_FLOOR = 0.1f; // minimum brightness floor 0.1f=10%
const size_t OPC_HEADER_LEN = 4;
// track current brightness safely across threads
std::atomic<int> g_current_brightness{INITIAL_BRIGHTNESS};

// Caching for the last frame
std::mutex g_cache_mutex;
std::vector<uint8_t> g_last_raw_payload;
uint8_t g_last_header[OPC_HEADER_LEN] = {0};

// Global Gamma Table and a Mutex to protect it during updates
uint8_t g_lookup_table[256];
std::mutex g_table_mutex;
float g_gamma = 2.5f; // LEDs Gamma

// Function to rebuild the table whenever brightness changes
void update_gamma_table(float brightness_normalized) {
    std::lock_guard<std::mutex> lock(g_table_mutex);
    for (int i = 0; i < 256; i++) {
        // Formula: (Input / 255)^Gamma * Brightness * 255
        float scaled = std::pow((float)i / 255.0f, g_gamma) * brightness_normalized;
        g_lookup_table[i] = (uint8_t)(scaled * 255.0f);
    }
}

void resend_last_frame() {
    std::lock_guard<std::mutex> cache_lock(g_cache_mutex);
    if (g_last_raw_payload.empty()) return; // No data received yet

    // 1. Copy the raw payload
    std::vector<uint8_t> payload_to_send = g_last_raw_payload;

    // 2. Apply the newly updated lookup table
    {
        std::lock_guard<std::mutex> table_lock(g_table_mutex);
        for (size_t i = 0; i < payload_to_send.size(); ++i) {
            payload_to_send[i] = g_lookup_table[payload_to_send[i]];
        }
    }

    // 3. Send to active connections or create a temporary one
    std::lock_guard<std::mutex> targets_lock(g_targets_mutex);
    
    if (!g_active_target_fds.empty()) {
        // Source is still actively connected; push through established sockets
        for (int fd : g_active_target_fds) {
            send(fd, g_last_header, OPC_HEADER_LEN, 0);
            send(fd, payload_to_send.data(), payload_to_send.size(), 0);
        }
    } else {
        // Source disconnected. Open a temporary connection to update the frozen LEDs.
        int temp_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (temp_fd >= 0) {
            struct sockaddr_in target_addr;
            target_addr.sin_family = AF_INET;
            target_addr.sin_port = htons(OPC_TARGET_PORT);
            inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);

            // Connect, send header + modified payload, then cleanly disconnect
            if (connect(temp_fd, (struct sockaddr*)&target_addr, sizeof(target_addr)) == 0) {
                send(temp_fd, g_last_header, OPC_HEADER_LEN, 0);
                send(temp_fd, payload_to_send.data(), payload_to_send.size(), 0);
            }
            close(temp_fd);
        }
    }
}

// --- HTTP LISTENER THREAD ---
void web_listener() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_LISTEN_PORT);

    bind(server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);

    std::cout << "HTTP Listener running on port " << HTTP_LISTEN_PORT << std::endl;

    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;

        char buffer[1024] = {0};
        read(client_fd, buffer, 1024);

        std::string request(buffer);
        std::string response;

        // Check if the user is requesting the current brightness
        if (request.find("GET /brightness ") != std::string::npos) {
            std::string body = std::to_string(g_current_brightness.load());
            response = "HTTP/1.1 200 OK\r\n"
                       "Content-Type: text/plain\r\n"
                       "Content-Length: " + std::to_string(body.length()) + "\r\n"
                       "\r\n" + body;
        } 
        // Check if the user is setting the brightness
        else if (size_t pos = request.find("b="); pos != std::string::npos) {
            try {
                float raw_val = std::stof(request.substr(pos + 2));
                // 1. Get the 0.0 to 1.0 ratio of the input
                float input_ratio = std::max(0.0f, std::min(1.0f, raw_val / 255.0f));
                
                // 2. Map that ratio to the range [MIN_BRIGHTNESS_FLOOR ... 1.0]
                float normalized = MIN_BRIGHTNESS_FLOOR + (input_ratio * (1.0f - MIN_BRIGHTNESS_FLOOR));
                
                // Recalculate the table
                update_gamma_table(normalized);
                
                // Update global tracker (store the raw requested value for the GET command)
                g_current_brightness.store(static_cast<int>(std::round(raw_val)));

                resend_last_frame();
                
                std::cout << "Gamma Table updated. Input: " << raw_val 
                        << " | Effective Brightness: " << (normalized * 100.0f) << "%" << std::endl;
                        
                response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nOK\n";
            } catch (...) {
                std::cerr << "Invalid brightness value received." << std::endl;
                response = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\n\r\nInvalid Value\n";
            }
        } 
        // Catch-all for other requests
        else {
            response = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n\r\nNot Found\n";
        }

        send(client_fd, response.c_str(), response.length(), 0);
        close(client_fd);
    }
}

// --- OPC PROXY LOGIC ---
void handle_opc_client(int client_fd, int server_fd) {
    // 1. Register this connection
    {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        g_active_target_fds.push_back(server_fd);
    }

    uint8_t header[OPC_HEADER_LEN];
    while (true) {
        if (recv(client_fd, header, OPC_HEADER_LEN, MSG_WAITALL) <= 0) break;

        uint16_t length = (static_cast<uint16_t>(header[2]) << 8) | header[3];

        std::vector<uint8_t> payload(length);
        if (length > 0) {
            if (recv(client_fd, payload.data(), length, MSG_WAITALL) <= 0) break;
        }
        
        if (header[1] == 0) {
            // 2. CACHE THE RAW DATA BEFORE MODIFYING IT
            {
                std::lock_guard<std::mutex> lock(g_cache_mutex);
                std::copy(header, header + OPC_HEADER_LEN, g_last_header);
                g_last_raw_payload = payload; 
            }

            // Apply gamma/brightness correction
            std::lock_guard<std::mutex> lock(g_table_mutex);
            for (size_t i = 0; i < length; ++i) {
                payload[i] = g_lookup_table[payload[i]];
            }
        }

        // Forward to target
        if (send(server_fd, header, OPC_HEADER_LEN, 0) <= 0) break;
        if (length > 0) {
            if (send(server_fd, payload.data(), length, 0) <= 0) break;
        }
    }

    // 3. Unregister this connection when the client disconnects
    {
        std::lock_guard<std::mutex> lock(g_targets_mutex);
        g_active_target_fds.erase(
            std::remove(g_active_target_fds.begin(), g_active_target_fds.end(), server_fd), 
            g_active_target_fds.end()
        );
    }
}

int main() {
    signal(SIGPIPE, SIG_IGN); // Prevent crashes on broken sockets

    // Initialize table using the global initial brightness
    float start_ratio = (float)INITIAL_BRIGHTNESS / 255.0f;
    float start_normalized = MIN_BRIGHTNESS_FLOOR + (start_ratio * (1.0f - MIN_BRIGHTNESS_FLOOR));
    update_gamma_table(start_normalized);

    // Launch HTTP listener in the background
    std::thread(web_listener).detach();

    // --- MAIN OPC PROXY SETUP ---
    int proxy_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_fd < 0) {
        std::cerr << "Failed to create OPC proxy socket." << std::endl;
        return 1;
    }

    int opt = 1;
    setsockopt(proxy_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in proxy_addr;
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_addr.s_addr = INADDR_ANY;
    proxy_addr.sin_port = htons(OPC_LISTEN_PORT);

    if (bind(proxy_fd, (struct sockaddr*)&proxy_addr, sizeof(proxy_addr)) < 0) {
        std::cerr << "Bind failed on OPC port " << OPC_LISTEN_PORT << std::endl;
        return 1;
    }

    listen(proxy_fd, 10);
    std::cout << "OPC Proxy listening on port " << OPC_LISTEN_PORT << std::endl;

    // Loop to accept incoming OPC controllers
    while (true) {
        int client_fd = accept(proxy_fd, NULL, NULL);
        if (client_fd < 0) {
            std::cerr << "Failed to accept OPC client." << std::endl;
            continue;
        }

        std::cout << "OPC client connected. Forwarding to target server..." << std::endl;

        // Create socket to communicate with the target OPC server (e.g., Fadecandy or led-daemon)
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in target_addr;
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(OPC_TARGET_PORT);
        inet_pton(AF_INET, TARGET_IP, &target_addr.sin_addr);

        if (connect(server_fd, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
            std::cerr << "Failed to connect to target OPC server at " << TARGET_IP << ":" << OPC_TARGET_PORT << std::endl;
            close(client_fd);
            close(server_fd);
            continue;
        }

        // Pass the connection to a detached thread so the proxy can accept multiple incoming controllers 
        std::thread([client_fd, server_fd]() {
            handle_opc_client(client_fd, server_fd);
            // Clean up sockets when the connection drops
            close(client_fd);
            close(server_fd);
            std::cout << "OPC client disconnected." << std::endl;
        }).detach();
    }

    close(proxy_fd);
    return 0;
}
