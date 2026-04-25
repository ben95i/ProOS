#include <gpiod.hpp>
#include <iostream>
#include <chrono>
#include <csignal>
#include <string>
#include <cstdlib>

volatile bool keepRunning = true;
void signalHandler(int signum) { keepRunning = false; }

int main(int argc, char* argv[]) {
    // 1. Check for the correct number of arguments
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <GPIO_PIN> <ARG_FOR_SCRIPT>\n";
        std::cerr << "Example: sudo " << argv[0] << " 13 'Hello_World'\n";
        return 1;
    }

    int pin = std::stoi(argv[1]);
    std::string script_arg = argv[2]; // Capture the second argument
    
    std::signal(SIGINT, signalHandler);

    // Pi 3 usually uses /dev/gpiochip0
    std::string chip_path = "/dev/gpiochip0"; 
    
    try {
        gpiod::chip chip(chip_path);
        
        auto settings = gpiod::line_settings()
            .set_direction(gpiod::line::direction::INPUT)
            .set_edge_detection(gpiod::line::edge::BOTH)
            .set_bias(gpiod::line::bias::PULL_UP)
            .set_debounce_period(std::chrono::milliseconds(100));

        auto request = chip.prepare_request()
            .set_consumer("shell_trigger")
            .add_line_settings(pin, settings)
            .do_request();

        std::cout << "Monitoring BCM " << pin << std::endl;

        gpiod::edge_event_buffer buffer(16);

        while (keepRunning) {
            if (request.wait_edge_events(std::chrono::seconds(1))) {
                request.read_edge_events(buffer);
                
                for (const auto& event : buffer) {
                    if (event.type() == gpiod::edge_event::event_type::FALLING_EDGE) {
                        
                        // 2. Build the shell command string
                        // We wrap the argument in quotes to handle spaces safely
                        std::string full_command = "/opt/rpi/main '" + script_arg + "'";
                        
                        std::cout << "Executing: " << full_command << std::endl;
                        
                        // 3. Convert std::string to C-string for std::system
                        std::system(full_command.c_str());
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}