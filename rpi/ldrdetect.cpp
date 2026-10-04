/*
 * ldrdetect.cpp
 *
 * Day / night detector for the IR camera. Times how long the LDR takes to charge a
 * capacitor on one GPIO (longer = darker) and runs ldrcmd.sh ir_on / ir_off once the
 * light level has settled on the far side of the night / day threshold.
 *
 * Build:  g++ -O2 -std=c++17 ldrdetect.cpp -o ldrdetect -lgpiodcxx
 * Usage:  ldrdetect [bcm_pin]
 *
 * Configuration (environment, e.g. systemctl set-environment), all optional except the pin:
 *   rpi_ldrpin    BCM pin, used when no pin argument is given
 *   rpi_ldrnight  charge time in uS at or above which it is night -> ir_on   (default 300000)
 *   rpi_ldrday    charge time in uS at or below which it is day   -> ir_off  (default 100000)
 *
 * Status logging is only printed when stdout is a terminal; under systemd it is discarded.
 * Fatal errors always go to stderr so they still show up in journalctl.
 */

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <unistd.h>
#include <gpiod.hpp>

using SteadyClock = std::chrono::steady_clock;

// ------------------------------------------------------------------ constants

const std::set<long> VALID_BCM_PINS = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27
};

const std::string CHIP_PATH      = "/dev/gpiochip0";
const std::string CONSUMER_NAME  = "ldr_sensor";
const std::string COMMAND_SCRIPT = "/opt/rpi/ldrcmd.sh";
const std::string COMMAND_IR_ON  = "ir_on";
const std::string COMMAND_IR_OFF = "ir_off";

const std::string ENV_PIN             = "rpi_ldrpin";
const std::string ENV_NIGHT_THRESHOLD = "rpi_ldrnight";
const std::string ENV_DAY_THRESHOLD   = "rpi_ldrday";

constexpr long   DEFAULT_NIGHT_THRESHOLD_US   = 300000;
constexpr long   DEFAULT_DAY_THRESHOLD_US     = 100000;
constexpr long   TIMEOUT_THRESHOLD_MULTIPLIER = 2;    // stop timing at 2x the night threshold

constexpr int    DISCHARGE_MS            = 150;   // fully empty the cap (prevents the 25uS half-charge oscillation)
constexpr int    SAMPLE_INTERVAL_MS      = 1000;  // pause between measurements
constexpr int    STOP_CHECK_INTERVAL_MS  = 100;   // how often the pause checks for Ctrl+C / SIGTERM
constexpr size_t MEDIAN_WINDOW_SIZE      = 5;     // samples in the spike-rejecting median filter
constexpr long   SETTLE_SECONDS          = 15;    // level must hold past a threshold this long to switch
constexpr long   STATUS_INTERVAL_SECONDS = 10;    // console status line interval
constexpr size_t EDGE_EVENT_BUFFER_SIZE  = 16;
constexpr long   NANOSECONDS_PER_MICROSECOND = 1000;

// ------------------------------------------------------------------ globals

volatile sig_atomic_t keepRunning = 1;
bool consoleMode = false;

/** Signal handler for SIGINT / SIGTERM: asks the main loop to finish. */
void handleStopSignal(int) {
    keepRunning = 0;
}

// ------------------------------------------------------------------ logging

/** Returns the local wall-clock time as HH:MM:SS for log prefixes. */
std::string currentTimeText() {
    std::time_t nowSeconds = std::time(nullptr);
    std::tm localTime{};
    localtime_r(&nowSeconds, &localTime);
    char buffer[16];
    std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
    return buffer;
}

/** Prints one timestamped log line, but only when running interactively in a terminal. */
void logLine(const std::string& message) {
    if (!consoleMode) {
        return;
    }
    std::cout << "[" << currentTimeText() << "] " << message << std::endl;
}

/** Formats a charge time, marking readings that hit the measurement timeout. */
std::string formatChargeTime(long chargeTimeUs, long timeoutUs) {
    if (chargeTimeUs >= timeoutUs) {
        return ">=" + std::to_string(timeoutUs) + " (timeout)";
    }
    return std::to_string(chargeTimeUs);
}

// ------------------------------------------------------------------ states

enum class LightState { UNKNOWN, DAY, NIGHT };   // UNKNOWN also means "nothing pending"
enum class LightZone  { DAY, BETWEEN, NIGHT };

/** Human-readable name of a light state. */
std::string lightStateName(LightState state) {
    switch (state) {
        case LightState::DAY:   return "DAY";
        case LightState::NIGHT: return "NIGHT";
        default:                return "UNKNOWN";
    }
}

/** Human-readable name of the zone a reading falls in relative to the thresholds. */
std::string lightZoneName(LightZone zone) {
    switch (zone) {
        case LightZone::DAY:   return "day zone  ";
        case LightZone::NIGHT: return "night zone";
        default:               return "between   ";
    }
}

/** The ldrcmd.sh argument that puts the camera into the given state. */
const std::string& commandArgumentFor(LightState state) {
    if (state == LightState::NIGHT) {
        return COMMAND_IR_ON;
    }
    return COMMAND_IR_OFF;
}

// ------------------------------------------------------------------ configuration

struct DetectorConfig {
    unsigned int pin;
    long nightThresholdUs;
    long dayThresholdUs;
    long timeoutUs;
};

/** Returns an environment variable's value, or an empty string when it is unset. */
std::string readEnvironment(const std::string& name) {
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) {
        return "";
    }
    return value;
}

/** Parses a whole number >= 0, throwing an error that names the setting when it is invalid. */
long parseWholeNumber(const std::string& text, const std::string& settingName) {
    size_t parsedLength = 0;
    long value = -1;
    try {
        value = std::stol(text, &parsedLength);
    } catch (const std::exception&) {
        parsedLength = 0;
    }
    bool fullyParsed = !text.empty() && parsedLength == text.size();
    if (!fullyParsed || value < 0) {
        throw std::invalid_argument(settingName + " must be a whole number, got '" + text + "'");
    }
    return value;
}

/** Reads a threshold in microseconds from the environment, falling back to a default when unset or empty. */
long readThresholdUs(const std::string& variableName, long defaultValueUs) {
    std::string text = readEnvironment(variableName);
    if (text.empty()) {
        return defaultValueUs;
    }
    long valueUs = parseWholeNumber(text, variableName);
    if (valueUs == 0) {
        throw std::invalid_argument(variableName + " must be greater than 0");
    }
    return valueUs;
}

/** Builds the configuration from the optional pin argument and the rpi_ldr* environment variables. */
DetectorConfig loadConfig(int argc, char* argv[]) {
    std::string argumentPin = argc >= 2 ? argv[1] : "";
    std::string pinText = argumentPin.empty() ? readEnvironment(ENV_PIN) : argumentPin;
    if (pinText.empty()) {
        throw std::invalid_argument("no GPIO pin given (usage: ldrdetect <bcm_pin>, or set " + ENV_PIN + ")");
    }

    long pinNumber = parseWholeNumber(pinText, "GPIO pin");
    if (VALID_BCM_PINS.count(pinNumber) == 0) {
        throw std::invalid_argument("GPIO pin must be a BCM number 0-27, got " + pinText);
    }

    DetectorConfig config{};
    config.pin = static_cast<unsigned int>(pinNumber);
    config.nightThresholdUs = readThresholdUs(ENV_NIGHT_THRESHOLD, DEFAULT_NIGHT_THRESHOLD_US);
    config.dayThresholdUs = readThresholdUs(ENV_DAY_THRESHOLD, DEFAULT_DAY_THRESHOLD_US);

    if (config.dayThresholdUs >= config.nightThresholdUs) {
        throw std::invalid_argument(ENV_DAY_THRESHOLD + " (" + std::to_string(config.dayThresholdUs)
            + ") must be lower than " + ENV_NIGHT_THRESHOLD + " (" + std::to_string(config.nightThresholdUs) + ")");
    }

    config.timeoutUs = config.nightThresholdUs * TIMEOUT_THRESHOLD_MULTIPLIER;
    return config;
}

// ------------------------------------------------------------------ timing helpers

/** Microseconds between two steady-clock time points. */
long elapsedMicroseconds(SteadyClock::time_point start, SteadyClock::time_point end) {
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    return static_cast<long>(elapsed.count());
}

/** A steady-clock time point as nanoseconds since its epoch. On Linux steady_clock is
    CLOCK_MONOTONIC, the same clock the kernel uses to timestamp GPIO edge events. */
std::uint64_t steadyClockNanoseconds(SteadyClock::time_point timePoint) {
    auto sinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(timePoint.time_since_epoch());
    return static_cast<std::uint64_t>(sinceEpoch.count());
}

// ------------------------------------------------------------------ RC charge timer

/**
 * Times the RC charge of the LDR + capacitor on one GPIO line.
 * The rising edge is captured as a kernel-timestamped edge event, so the timing is
 * microsecond-accurate across the whole range and the CPU sleeps while it waits.
 */
class ChargeTimer {
public:
    ChargeTimer(const std::string& chipPath, unsigned int pin, long timeoutUs)
        : linePin(pin),
          chargeTimeoutUs(timeoutUs),
          chip(chipPath),
          request(requestLine(chip, pin)),
          eventBuffer(EDGE_EVENT_BUFFER_SIZE) {}

    /** Discharges the capacitor, then returns the time in uS until the pin reads high
        (capped at the timeout), or nothing if a stop signal interrupted the wait. */
    std::optional<long> measure() {
        applySettings(dischargeSettings());
        std::this_thread::sleep_for(std::chrono::milliseconds(DISCHARGE_MS));
        discardPendingEdgeEvents();

        SteadyClock::time_point chargeStart = SteadyClock::now();
        applySettings(chargeSettings());

        // In very bright light the pin can go high while the line is being reconfigured,
        // before edge detection is armed, so check the level before waiting for an edge.
        if (request.get_value(linePin) == gpiod::line::value::ACTIVE) {
            return elapsedMicroseconds(chargeStart, SteadyClock::now());
        }
        return waitForRisingEdge(chargeStart);
    }

private:
    unsigned int linePin;
    long chargeTimeoutUs;
    gpiod::chip chip;
    gpiod::line_request request;
    gpiod::edge_event_buffer eventBuffer;

    /** Output driven low: drains the capacitor through the pin. */
    static gpiod::line_settings dischargeSettings() {
        gpiod::line_settings settings;
        settings.set_direction(gpiod::line::direction::OUTPUT)
                .set_output_value(gpiod::line::value::INACTIVE);
        return settings;
    }

    /** Floating input with rising-edge detection: the LDR alone charges the capacitor.
        Bias is explicitly disabled so an internal pull-up/down cannot skew the timing. */
    static gpiod::line_settings chargeSettings() {
        gpiod::line_settings settings;
        settings.set_direction(gpiod::line::direction::INPUT)
                .set_bias(gpiod::line::bias::DISABLED)
                .set_edge_detection(gpiod::line::edge::RISING)
                .set_event_clock(gpiod::line::clock::MONOTONIC);
        return settings;
    }

    /** Requests the line, starting in the discharge state. */
    static gpiod::line_request requestLine(gpiod::chip& gpioChip, unsigned int pin) {
        return gpioChip.prepare_request()
            .set_consumer(CONSUMER_NAME)
            .add_line_settings(pin, dischargeSettings())
            .do_request();
    }

    /** Switches the line to new settings. */
    void applySettings(const gpiod::line_settings& settings) {
        gpiod::line_config config;
        config.add_line_settings(linePin, settings);
        request.reconfigure_lines(config);
    }

    /** Throws away edge events left over from the previous measurement (e.g. noise on a slow edge). */
    void discardPendingEdgeEvents() {
        while (request.wait_edge_events(std::chrono::nanoseconds(0))) {
            request.read_edge_events(eventBuffer);
        }
    }

    /** Sleeps until the first rising edge after chargeStart and returns the charge time from its
        kernel timestamp. Returns the timeout if no edge arrives, nothing if interrupted by a signal. */
    std::optional<long> waitForRisingEdge(SteadyClock::time_point chargeStart) {
        std::chrono::microseconds timeout(chargeTimeoutUs);
        try {
            bool edgeArrived = request.wait_edge_events(timeout);
            if (!edgeArrived) {
                return chargeTimeoutUs;
            }
            request.read_edge_events(eventBuffer);
        } catch (const std::system_error& error) {
            if (error.code().value() == EINTR) {
                return std::nullopt;
            }
            throw;
        }

        std::uint64_t chargeStartNs = steadyClockNanoseconds(chargeStart);
        for (const gpiod::edge_event& event : eventBuffer) {
            std::uint64_t edgeNs = event.timestamp_ns().ns();
            if (edgeNs < chargeStartNs) {
                continue;
            }
            std::uint64_t chargeTimeNs = edgeNs - chargeStartNs;
            long chargeTimeUs = static_cast<long>(chargeTimeNs / NANOSECONDS_PER_MICROSECOND);
            return std::min(chargeTimeUs, chargeTimeoutUs);
        }

        // Only stale events were found, but the line did go high: fall back to the wall clock.
        return elapsedMicroseconds(chargeStart, SteadyClock::now());
    }
};

// ------------------------------------------------------------------ median filter

/** Keeps the last N samples and reports their median, so single spikes
    (headlights, a passing shadow, one noisy reading) never move the level. */
class MedianFilter {
public:
    explicit MedianFilter(size_t size) : windowSize(size) {}

    /** Adds a sample, dropping the oldest once the window is full. */
    void add(long sample) {
        samples.push_back(sample);
        if (samples.size() > windowSize) {
            samples.pop_front();
        }
    }

    /** True once enough samples have arrived for the median to be meaningful. */
    bool isFull() const {
        return samples.size() == windowSize;
    }

    /** Median of the samples currently in the window. */
    long median() const {
        std::vector<long> sortedSamples(samples.begin(), samples.end());
        std::sort(sortedSamples.begin(), sortedSamples.end());
        size_t middleIndex = sortedSamples.size() / 2;
        return sortedSamples[middleIndex];
    }

private:
    size_t windowSize;
    std::deque<long> samples;
};

// ------------------------------------------------------------------ day / night decision

/**
 * Decides the day / night state from the filtered level using two thresholds (hysteresis)
 * plus a settle time: the level has to stay past a threshold continuously for SETTLE_SECONDS
 * before the state changes. Anything between the thresholds keeps the current state.
 */
class DayNightDetector {
public:
    DayNightDetector(long nightUs, long dayUs) : nightThresholdUs(nightUs), dayThresholdUs(dayUs) {}

    /** Classifies a filtered charge time against the thresholds. */
    LightZone classify(long levelUs) const {
        if (levelUs >= nightThresholdUs) {
            return LightZone::NIGHT;
        }
        if (levelUs <= dayThresholdUs) {
            return LightZone::DAY;
        }
        return LightZone::BETWEEN;
    }

    /** Geometric midpoint of the thresholds. The LDR responds logarithmically to light, so this is
        the "halfway" brightness, used only to pick the initial state when starting between them. */
    long midpointUs() const {
        double thresholdProduct = static_cast<double>(nightThresholdUs) * static_cast<double>(dayThresholdUs);
        double midpoint = std::sqrt(thresholdProduct);
        return static_cast<long>(midpoint);
    }

    /** Feeds one filtered reading; returns the new state when a switch should happen now.
        The first call always returns a state so the camera starts in a known mode. */
    std::optional<LightState> update(long levelUs, SteadyClock::time_point now) {
        if (currentState == LightState::UNKNOWN) {
            bool startsDark = levelUs >= midpointUs();
            currentState = startsDark ? LightState::NIGHT : LightState::DAY;
            return currentState;
        }

        LightState wantedState = wantedStateFor(classify(levelUs));
        if (wantedState == currentState) {
            pending = LightState::UNKNOWN;
            return std::nullopt;
        }

        if (pending != wantedState) {
            pending = wantedState;
            pendingSince = now;
        }
        if (settleSecondsRemaining(now) > 0) {
            return std::nullopt;
        }

        currentState = wantedState;
        pending = LightState::UNKNOWN;
        return currentState;
    }

    /** Seconds the pending change still has to hold before it is applied. */
    long settleSecondsRemaining(SteadyClock::time_point now) const {
        long heldSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - pendingSince).count();
        long remainingSeconds = SETTLE_SECONDS - heldSeconds;
        return std::max(remainingSeconds, 0L);
    }

    LightState state() const { return currentState; }
    LightState pendingState() const { return pending; }

private:
    long nightThresholdUs;
    long dayThresholdUs;
    LightState currentState = LightState::UNKNOWN;
    LightState pending = LightState::UNKNOWN;
    SteadyClock::time_point pendingSince;

    /** The state a zone asks for; between the thresholds the current state is kept. */
    LightState wantedStateFor(LightZone zone) const {
        if (zone == LightZone::NIGHT) {
            return LightState::NIGHT;
        }
        if (zone == LightZone::DAY) {
            return LightState::DAY;
        }
        return currentState;
    }
};

// ------------------------------------------------------------------ console reporting

/**
 * Console-only reporting aimed at choosing the thresholds: a startup banner, periodic status
 * lines with the raw range, pending / switch events, and the session range on exit.
 * Everything goes through logLine(), so it is silent when not running in a terminal.
 */
class StatusReporter {
public:
    explicit StatusReporter(const DetectorConfig& detectorConfig)
        : config(detectorConfig),
          lastStatusTime(SteadyClock::now() - std::chrono::seconds(STATUS_INTERVAL_SECONDS)) {}

    /** Prints the active configuration and how to read the output. */
    void printBanner() const {
        logLine("ldrdetect on BCM " + std::to_string(config.pin) + " - level is the LDR charge time, longer = darker");
        logLine("  night -> " + COMMAND_IR_ON + "  when level >= " + std::to_string(config.nightThresholdUs) + " uS  (" + ENV_NIGHT_THRESHOLD + ")");
        logLine("  day   -> " + COMMAND_IR_OFF + " when level <= " + std::to_string(config.dayThresholdUs) + " uS  (" + ENV_DAY_THRESHOLD + ")");
        logLine("  level = median of the last " + std::to_string(MEDIAN_WINDOW_SIZE) + " samples; it must hold past a threshold for "
            + std::to_string(SETTLE_SECONDS) + "s to switch; between the thresholds nothing changes");
        logLine("  calibrate: note the level when the IR should come on (night) and go off (day), check the");
        logLine("  night value with the IR LEDs lit, and keep the two thresholds well apart");
    }

    /** Tracks the raw range for the next status line and for the session summary. */
    void recordSample(long chargeTimeUs) {
        intervalMinUs = std::min(intervalMinUs, chargeTimeUs);
        intervalMaxUs = std::max(intervalMaxUs, chargeTimeUs);
        sessionMinUs = std::min(sessionMinUs, chargeTimeUs);
        sessionMaxUs = std::max(sessionMaxUs, chargeTimeUs);
        sessionSampleCount++;
    }

    /** Announces a state switch and the command being run. */
    void reportSwitch(LightState newState, long levelUs, bool isInitialState, long midpointUs) const {
        std::string commandText = COMMAND_SCRIPT + " " + commandArgumentFor(newState);
        std::string levelText = formatChargeTime(levelUs, config.timeoutUs);
        if (isInitialState) {
            logLine("Initial state " + lightStateName(newState) + " (level " + levelText + " uS, midpoint "
                + std::to_string(midpointUs) + " uS): running " + commandText);
            return;
        }
        logLine("Switched to " + lightStateName(newState) + " (level " + levelText + " uS held for "
            + std::to_string(SETTLE_SECONDS) + "s): running " + commandText);
    }

    /** Announces a change becoming pending, or a pending change being cancelled. */
    void reportPendingChange(LightState pendingBefore, const DayNightDetector& detector, long levelUs) const {
        LightState pendingAfter = detector.pendingState();
        if (pendingAfter == pendingBefore) {
            return;
        }

        std::string levelText = formatChargeTime(levelUs, config.timeoutUs);
        if (pendingAfter == LightState::UNKNOWN) {
            logLine("Cancelled " + lightStateName(pendingBefore) + ": level " + levelText
                + " uS fell back before settling, staying " + lightStateName(detector.state()));
            return;
        }

        bool goingDark = pendingAfter == LightState::NIGHT;
        std::string comparison = goingDark ? " >= night threshold " : " <= day threshold ";
        long crossedThresholdUs = goingDark ? config.nightThresholdUs : config.dayThresholdUs;
        logLine("Pending " + lightStateName(pendingAfter) + ": level " + levelText + " uS" + comparison
            + std::to_string(crossedThresholdUs) + " uS, switching in " + std::to_string(SETTLE_SECONDS) + "s if it holds");
    }

    /** Prints a status line every STATUS_INTERVAL_SECONDS. */
    void printStatusIfDue(long levelUs, const DayNightDetector& detector, SteadyClock::time_point now) {
        long secondsSinceStatus = std::chrono::duration_cast<std::chrono::seconds>(now - lastStatusTime).count();
        if (secondsSinceStatus < STATUS_INTERVAL_SECONDS) {
            return;
        }

        std::ostringstream line;
        line << "level " << std::setw(7) << formatChargeTime(levelUs, config.timeoutUs) << " uS"
             << " | raw " << formatChargeTime(intervalMinUs, config.timeoutUs)
             << " - " << formatChargeTime(intervalMaxUs, config.timeoutUs)
             << " | " << lightZoneName(detector.classify(levelUs))
             << " | " << lightStateName(detector.state());

        LightState pending = detector.pendingState();
        if (pending != LightState::UNKNOWN) {
            line << " -> " << lightStateName(pending) << " in " << detector.settleSecondsRemaining(now) << "s";
        }
        logLine(line.str());

        lastStatusTime = now;
        intervalMinUs = std::numeric_limits<long>::max();
        intervalMaxUs = 0;
    }

    /** Prints the brightest / darkest raw readings seen this run, for picking thresholds. */
    void printSessionSummary() const {
        if (sessionSampleCount == 0) {
            return;
        }
        logLine("Session: brightest " + formatChargeTime(sessionMinUs, config.timeoutUs) + " uS, darkest "
            + formatChargeTime(sessionMaxUs, config.timeoutUs) + " uS over " + std::to_string(sessionSampleCount) + " samples");
    }

private:
    DetectorConfig config;
    SteadyClock::time_point lastStatusTime;
    long intervalMinUs = std::numeric_limits<long>::max();
    long intervalMaxUs = 0;
    long sessionMinUs = std::numeric_limits<long>::max();
    long sessionMaxUs = 0;
    long sessionSampleCount = 0;
};

// ------------------------------------------------------------------ actions

/** Runs ldrcmd.sh for the new state in the background. Its output is shown on the console
    and discarded when running as a service. */
void runIrCommand(LightState state) {
    std::string outputRedirect = consoleMode ? "" : " >/dev/null 2>&1";
    std::string command = "bash " + COMMAND_SCRIPT + " " + commandArgumentFor(state) + outputRedirect + " &";
    int exitCode = std::system(command.c_str());
    if (exitCode != 0) {
        std::cerr << "ldrdetect: failed to start: " << command << std::endl;
    }
}

/** Sleeps for the sample interval in short steps so Ctrl+C / SIGTERM stop promptly. */
void pauseBetweenSamples() {
    int sleptMs = 0;
    while (keepRunning && sleptMs < SAMPLE_INTERVAL_MS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(STOP_CHECK_INTERVAL_MS));
        sleptMs += STOP_CHECK_INTERVAL_MS;
    }
}

// ------------------------------------------------------------------ main

int main(int argc, char* argv[]) {
    consoleMode = isatty(STDOUT_FILENO) == 1;
    std::signal(SIGINT, handleStopSignal);
    std::signal(SIGTERM, handleStopSignal);

    try {
        DetectorConfig config = loadConfig(argc, argv);
        ChargeTimer chargeTimer(CHIP_PATH, config.pin, config.timeoutUs);
        MedianFilter levelFilter(MEDIAN_WINDOW_SIZE);
        DayNightDetector detector(config.nightThresholdUs, config.dayThresholdUs);
        StatusReporter reporter(config);
        reporter.printBanner();

        while (keepRunning) {
            std::optional<long> chargeTimeUs = chargeTimer.measure();
            if (!chargeTimeUs) {
                break;  // interrupted by a stop signal
            }

            levelFilter.add(*chargeTimeUs);
            reporter.recordSample(*chargeTimeUs);

            if (levelFilter.isFull()) {
                long levelUs = levelFilter.median();
                SteadyClock::time_point now = SteadyClock::now();
                bool isInitialState = detector.state() == LightState::UNKNOWN;
                LightState pendingBefore = detector.pendingState();

                std::optional<LightState> newState = detector.update(levelUs, now);
                if (newState) {
                    reporter.reportSwitch(*newState, levelUs, isInitialState, detector.midpointUs());
                    runIrCommand(*newState);
                } else {
                    reporter.reportPendingChange(pendingBefore, detector, levelUs);
                }
                reporter.printStatusIfDue(levelUs, detector, now);
            }

            pauseBetweenSamples();
        }

        reporter.printSessionSummary();
    } catch (const std::exception& error) {
        std::cerr << "ldrdetect: " << error.what() << std::endl;
        return 1;
    }
    return 0;
}
