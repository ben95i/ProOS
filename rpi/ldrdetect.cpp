#include <iostream>
#include <string>
#include <unistd.h>
#include <csignal>
#include <cstdlib>
#include <cmath>
#include <set>
#include <chrono>
#include <thread>
#include <gpiod.hpp>

const std::set<int> VALID_BCM_PINS = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};

const std::string chip_path = "/dev/gpiochip0";
const std::string SCRIPT_ON  = "/opt/rpi/ldrcmd.sh ir_on";
const std::string SCRIPT_OFF = "/opt/rpi/ldrcmd.sh ir_off";

constexpr int DISCHARGE_MS       = 150; // 150ms to prevent the 25uS "half-charge" oscillation
constexpr int TIMEOUT_US         = 600000; 
constexpr int THRESHOLD_DARK_US  = 300000; 
constexpr int THRESHOLD_LIGHT_US = 100000;
constexpr int CHANGE_DELTA_US    = 10000;
constexpr int PRINT_INTERVAL_SEC = 20; 
enum class IrState { ON, OFF, UNKNOWN };


volatile sig_atomic_t keepRunning = 1;
void sigHandler(int signum) { keepRunning = 0; }

void runScript(const std::string& path) {
    std::string command = "bash " + path + " &";
    if (std::system(command.c_str()) != 0) {
        std::cerr << "Error: Script execution failed." << std::endl;
    }
}

long measureChargeTime(gpiod::line_request& request, unsigned int pin) {
    gpiod::line_config out_cfg;
    out_cfg.add_line_settings(pin, gpiod::line_settings()
        .set_direction(gpiod::line::direction::OUTPUT)
        .set_output_value(gpiod::line::value::INACTIVE));
    request.reconfigure_lines(out_cfg);

    std::this_thread::sleep_for(std::chrono::milliseconds(DISCHARGE_MS));

    gpiod::line_config in_cfg;
    in_cfg.add_line_settings(pin, gpiod::line_settings()
        .set_direction(gpiod::line::direction::INPUT));
    request.reconfigure_lines(in_cfg);

    auto start = std::chrono::steady_clock::now();
    
    while (request.get_value(pin) == gpiod::line::value::INACTIVE) {
        auto now = std::chrono::steady_clock::now();
        long elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        
        if (elapsed > TIMEOUT_US) {
            return TIMEOUT_US;
        }

        // --- DYNAMIC POLLING ---
        // Phase 1 (0 to 2000 uS): Spin loop normally for perfect microsecond accuracy in bright light.
        // Phase 2 (> 2000 uS): Yield the CPU. Accuracy drops to 1ms blocks, but saves 99% CPU overhead.
        if (elapsed > 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;

    int ldr_pin;
    try {
        ldr_pin = std::stoi(argv[1]);
    } catch (...) { return 1; }

    if (VALID_BCM_PINS.find(ldr_pin) == VALID_BCM_PINS.end()) return 1;

    signal(SIGINT, sigHandler);

    try {
        gpiod::chip chip(chip_path);
        auto request = chip.prepare_request()
            .set_consumer("ldr_sensor")
            .add_line_settings(ldr_pin, gpiod::line_settings()
                .set_direction(gpiod::line::direction::INPUT))
            .do_request();

        IrState currentState = IrState::UNKNOWN;
        long lastPrintedValue = -1;
        auto lastPrintTime = std::chrono::steady_clock::now() - std::chrono::seconds(PRINT_INTERVAL_SEC);

        while (keepRunning) {
            long duration = measureChargeTime(request, ldr_pin);
            auto now = std::chrono::steady_clock::now();

            // 1. Logic Trigger (Always responsive)
            if (duration >= THRESHOLD_DARK_US && currentState != IrState::ON) {
                std::cout << "Triggering Script: " << SCRIPT_ON << "\n";
                runScript(SCRIPT_ON);
                currentState = IrState::ON;
            } 
            else if (duration <= THRESHOLD_LIGHT_US && currentState != IrState::OFF) {
                std::cout << "Triggering Script: " << SCRIPT_OFF << "\n";
                runScript(SCRIPT_OFF);
                currentState = IrState::OFF;
            }

            // 2. Throttled Print Output
            auto secondsSinceLastPrint = std::chrono::duration_cast<std::chrono::seconds>(now - lastPrintTime).count();
            long delta = std::abs(duration - lastPrintedValue);

            if (secondsSinceLastPrint >= PRINT_INTERVAL_SEC && delta >= CHANGE_DELTA_US) {
                std::cout << "Threshold in uS: " << duration << "\n";
                lastPrintedValue = duration;
                lastPrintTime = now;
            }

            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "GPIO Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
