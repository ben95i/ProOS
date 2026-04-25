#include <iostream>
#include <string>
#include <map>
#include <set>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <gpiod.hpp>

std::map<int, gpiod::line_request> active_requests;

const std::set<int> VALID_BCM_PINS = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};

std::string get_param(const std::string& req, const std::string& param) {
    size_t pos = req.find(param + "=");
    if (pos == std::string::npos) return "";
    size_t start = pos + param.length() + 1;
    size_t end = req.find_first_of("& ", start);
    return req.substr(start, end - start);
}

int main() {
    const int port = 8770;
    const std::string chip_path = "/dev/gpiochip0"; 

    try {
        gpiod::chip chip(chip_path);

        int server_fd;
        struct sockaddr_in address;
        int opt = 1;
        int addrlen = sizeof(address);

        if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
            perror("Socket failed");
            return EXIT_FAILURE;
        }

        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); 
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            perror("Bind failed");
            close(server_fd);
            return EXIT_FAILURE;
        }
        
        listen(server_fd, 10);

        std::cout << "GPIO Server Ready (v2.x) with TOGGLE\nURL: http://127.0.0.1:" << port << "/gpio\n";

        while (true) {
            int new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
            if (new_socket < 0) continue;

            char buffer[2048];
            ssize_t valread = read(new_socket, buffer, sizeof(buffer) - 1);
            
            if (valread > 0) {
                buffer[valread] = '\0'; 
                std::string request(buffer);

                if (request.find("GET /gpio") != std::string::npos) {
                    std::string pin_str = get_param(request, "pin");
                    std::string state_str = get_param(request, "state");

                    std::string response_body;
                    int status_code = 200;

                    // 1. Validation Logic
                    gpiod::line::value val;
                    bool state_valid = false;
                    bool is_toggle = (state_str == "TOGGLE");

                    if (state_str == "HIGH") { 
                        val = gpiod::line::value::ACTIVE; 
                        state_valid = true; 
                    }
                    else if (state_str == "LOW") { 
                        val = gpiod::line::value::INACTIVE; 
                        state_valid = true; 
                    }
                    else if (is_toggle) {
                        state_valid = true;
                    }

                    int pin = -1;
                    bool pin_valid = false;
                    try {
                        if (!pin_str.empty()) {
                            pin = std::stoi(pin_str);
                            if (VALID_BCM_PINS.count(pin)) pin_valid = true;
                        }
                    } catch (...) { pin_valid = false; }

                    if (!state_valid || !pin_valid) {
                        status_code = 400;
                        response_body = "Error: Invalid Arguments. Use BCM 0-27 and HIGH/LOW/TOGGLE.\n";
                    } else {
                        try {
                            auto it = active_requests.find(pin);
                            if (it != active_requests.end()) {
                                // If toggling, read current state and flip it
                                if (is_toggle) {
                                    val = (it->second.get_value(pin) == gpiod::line::value::ACTIVE) ? 
                                           gpiod::line::value::INACTIVE : gpiod::line::value::ACTIVE;
                                }
                                it->second.set_value(pin, val);
                            } else {
                                // If toggling a fresh pin, default to HIGH (ACTIVE)
                                if (is_toggle) val = gpiod::line::value::ACTIVE;

                                auto request_builder = chip.prepare_request();
                                request_builder.set_consumer("gpiosrv_strict");
                                request_builder.add_line_settings(
                                    pin,
                                    gpiod::line_settings()
                                        .set_direction(gpiod::line::direction::OUTPUT)
                                        .set_output_value(val)
                                );
                                active_requests.emplace(pin, request_builder.do_request());
                            }
                            
                            std::string final_state = (val == gpiod::line::value::ACTIVE) ? "HIGH" : "LOW";
                            response_body = "OK: Pin " + std::to_string(pin) + " is now " + final_state + "\n";
                        } catch (const std::exception& e) {
                            status_code = 500;
                            response_body = "Hardware Error: " + std::string(e.what()) + "\n";
                        }
                    }

                    std::string http_response = "HTTP/1.1 " + std::to_string(status_code) + " OK\r\n"
                                               "Content-Type: text/plain\r\n"
                                               "Content-Length: " + std::to_string(response_body.length()) + "\r\n"
                                               "Connection: close\r\n\r\n" + response_body;
                    
                    send(new_socket, http_response.c_str(), http_response.length(), 0);
                }
            }
            close(new_socket);
        }
    } catch (const std::exception& e) {
        std::cerr << "Fatal Hardware Error: " << e.what() << "\n";
        return EXIT_FAILURE;
    }

    return 0;
}