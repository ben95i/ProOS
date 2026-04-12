#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// Configuration
const int NUM_PIXELS = 480; // 8x60 is max # of LEDs
const int PAYLOAD_SIZE = NUM_PIXELS * 3;
const char* OPC_HOST = "127.0.0.1";
const int OPC_PORT = 7890;

struct RGB {
    uint8_t r, g, b;
};

// Converts hex string (0000FF) to RGB components
RGB parseHex(const std::string& hex) {
    if (hex.length() != 6) {
        throw std::invalid_argument("Hex color must be exactly 6 characters (e.g., 0000FF)");
    }
    uint32_t val = std::stoul(hex, nullptr, 16);
    return {
        static_cast<uint8_t>((val >> 16) & 0xFF),
        static_cast<uint8_t>((val >> 8) & 0xFF),
        static_cast<uint8_t>(val & 0xFF)
    };
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <HEX_COLOR>\nExample: " << argv[0] << " 0000FF" << std::endl;
        return 1;
    }

    RGB color;
    try {
        color = parseHex(argv[1]);
    } catch (...) {
        std::cerr << "Error: Invalid hex color format." << std::endl;
        return 1;
    }

    // 1. Create Socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("Socket creation failed");
        return 1;
    }

    // 2. Setup Address
    sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(OPC_PORT);
    
    if (inet_pton(AF_INET, OPC_HOST, &server_addr.sin_addr) <= 0) {
        perror("Invalid address/ Address not supported");
        return 1;
    }

    // 3. Connect to OPC Server
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection to OPC server failed");
        return 1;
    }

    // 4. Construct OPC Packet
    // [Channel][Command][Len High][Len Low][Data...]
    std::vector<uint8_t> packet;
    packet.reserve(4 + PAYLOAD_SIZE);
    
    packet.push_back(0); // Channel
    packet.push_back(0); // Command (8-bit RGB)
    packet.push_back((PAYLOAD_SIZE >> 8) & 0xFF);
    packet.push_back(PAYLOAD_SIZE & 0xFF);

    for (int i = 0; i < NUM_PIXELS; ++i) {
        packet.push_back(color.r);
        packet.push_back(color.g);
        packet.push_back(color.b);
    }

    std::cout << "Broadcasting " << argv[1] << " at 1Hz. Press Ctrl+C to stop." << std::endl;

    // 5. Send loop at 10Hz (100ms)
    while (true) {
        if (send(sock, packet.data(), packet.size(), 0) < 0) {
            perror("Send failed");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    close(sock);
    return 0;
}
