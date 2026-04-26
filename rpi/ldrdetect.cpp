#include <iostream>
#include <string>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <set>
#include <chrono>
#include <thread>
#include <gpiod.hpp>

// --- Configuration ---
const std::set<int> VALID_BCM_PINS = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};

const std::string chip_path = "/dev/gpiochip0";
const std::string SCRIPT_ON  = "/opt/rpi/main ir_on";
const std::string SCRIPT_OFF = "/opt/rpi/main ir_off";

// --- Tuning Constants ---
constexpr int DISCHARGE_MS         = 10;      // Time to drain capacitor
constexpr int TIMEOUT_US           = 500000;  // 0.5s safety timeout
constexpr int THRESHOLD_DARK_US    = 200000;  // Turn IR ON above this
constexpr int THRESHOLD_LIGHT_US   = 80000;   // Turn IR OFF below this
constexpr int CHANGE_DELTA_US      = 10000;   // Print only if change exceeds this
constexpr int PRINT_INTERVAL_SEC   = 10;    
constexpr int SAMPLE_INTERVAL_SEC  = 1;       // Rest between full samples

enum class IrState { ON, OFF, UNKNOWN };

volatile sig_atomic_t keepRunning = 1;
void sigHandler(int signum) { keepRunning = 0; }

void runScript(const std::string& path) {
    // Note: Removed 'bash' prefix as your path /opt/rpi/main suggests a binary/wrapper
    std::string command = path + " &"; 
    if (std::system(command.c_str()) != 0) {
        std::cerr << "Error: Script execution failed." << std::endl;
    }
}

/**
 * CPU-Efficient measurement using Kernel Edge Detection.
 * The process sleeps (0% CPU) while waiting for the pin to go HIGH.
 */
long measureChargeTime(gpiod::chip& chip, unsigned int pin) {
    // 1. Discharge Phase
    {
        auto discharge = chip.prepare_request()
            .set_consumer("ldr_dis")
            .add_line_settings(pin, gpiod::line_settings()
                .set_direction(gpiod::line::direction::OUTPUT)
                .set_output_value(gpiod::line::value::INACTIVE))
            .do_request();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(DISCHARGE_MS));
    }

    // 2. Interrupt Phase
    auto request = chip.prepare_request()
        .set_consumer("ldr_measure")
        .add_line_settings(pin, gpiod::line_settings()
            .set_direction(gpiod::line::direction::INPUT)
            .set_edge_detection(gpiod::line::edge::RISING))
        .do_request();

    auto start = std::chrono::steady_clock::now();

    // The kernel puts the thread to sleep here until the edge is detected
    if (request.wait_edge_events(std::chrono::microseconds(TIMEOUT_US))) {
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    }

    return TIMEOUT_US; // Timeout reached (Dark)
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;

    unsigned int ldr_pin;
    try {
        ldr_pin = std::stoul(argv[1]);
    } catch (...) { return 1; }

    if (VALID_BCM_PINS.find(ldr_pin) == VALID_BCM_PINS.end()) {
        std::cerr << "Invalid BCM Pin.\n";
        return 1;
    }

    signal(SIGINT, sigHandler);

    try {
        gpiod::chip chip(chip_path);
        IrState currentState = IrState::UNKNOWN;
        long lastPrintedValue = -1;
        auto lastPrintTime = std::chrono::steady_clock::now() - std::chrono::seconds(PRINT_INTERVAL_SEC);

        while (keepRunning) {
            long duration = measureChargeTime(chip, ldr_pin);
            auto now = std::chrono::steady_clock::now();

            // 1. Hysteresis Logic (Responsive Script Triggering)
            if (duration >= THRESHOLD_DARK_US && currentState != IrState::ON) {
                std::cout << "Action: DARK [" << duration << "us] -> " << SCRIPT_ON << "\n";
                runScript(SCRIPT_ON);
                currentState = IrState::ON;
            } 
            else if (duration <= THRESHOLD_LIGHT_US && currentState != IrState::OFF) {
                std::cout << "Action: LIGHT [" << duration << "us] -> " << SCRIPT_OFF << "\n";
                runScript(SCRIPT_OFF);
                currentState = IrState::OFF;
            }

            // 2. Throttled Print Output (Telemetry)
            auto secondsSinceLastPrint = std::chrono::duration_cast<std::chrono::seconds>(now - lastPrintTime).count();
            long delta = std::abs(duration - lastPrintedValue);

            if (secondsSinceLastPrint >= PRINT_INTERVAL_SEC && delta >= CHANGE_DELTA_US) {
                std::cout << "Current Reading: " << duration << " uS\n";
                lastPrintedValue = duration;
                lastPrintTime = now;
            }

            std::this_thread::sleep_for(std::chrono::seconds(SAMPLE_INTERVAL_SEC));
        }

    } catch (const std::exception& e) {
        std::cerr << "GPIO Error: " << e.what() << "\n";
        return 1;
    }

    std::cout << "Exiting.\n";
    return 0;
}