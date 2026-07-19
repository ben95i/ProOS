#include <iostream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <system_error>
#include <chrono>
#include <iomanip>
#include <stdexcept>
#include <set>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <fstream>
#include <memory>
#include <cerrno>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <sys/wait.h>
#include <sstream>

namespace fs = std::filesystem;

// Constants defined in UPPER_CASE
const std::string EXTENSION_MP3 = ".mp3";
// Supported Input Formats
const std::string EXTENSION_FLAC = ".flac";
const std::string EXTENSION_ALAC = ".alac";
const std::string EXTENSION_M4A = ".m4a";
const std::string EXTENSION_AAC = ".aac";

const int SUCCESS_EXIT_CODE = 0;
const int ERROR_EXIT_CODE = 1;
const int MINIMUM_BITRATE_KBPS = 32;
const int MAXIMUM_BITRATE_KBPS = 320;
const int MINIMUM_THREAD_COUNT = 1;
const int MAXIMUM_THREAD_COUNT = 64;

// Add any prefix combinations here to completely ignore those files
const std::vector<std::string> EXCLUDED_PREFIXES = {
    "._",
    ".DS_Store"
};

/**
 * Shared utility: lowercases a string safely (casts to unsigned char before
 * calling std::tolower, since passing a negative/signed char is undefined
 * behavior). Used everywhere a case-insensitive comparison is needed, so
 * there is a single implementation instead of duplicated copies.
 */
std::string toLowercaseString(const std::string& inputString) {
    std::string lowercaseResult;
    lowercaseResult.reserve(inputString.size());
    for (char currentCharacter : inputString) {
        unsigned char asUnsignedChar = static_cast<unsigned char>(currentCharacter);
        lowercaseResult += static_cast<char>(std::tolower(asUnsignedChar));
    }
    return lowercaseResult;
}

/**
 * Single Responsibility Principle (SRP):
 * Keeps track of uninitialized startup log lines to flush them safely
 * once the output directory is ready.
 */
struct PendingLog {
    bool isError;
    std::string message;
};

/**
 * Single Responsibility Principle (SRP):
 * This class handles all console output and file logging with strict thread safety.
 */
class VerboseLogger {
private:
    std::mutex loggerMutex;
    std::ofstream logFileStream;
    std::ofstream errorFileStream;
    bool areFilesInitialized = false;
    std::vector<PendingLog> pendingLogsList;

    std::string getCurrentTimestamp() {
        // NOTE: std::ctime uses a shared internal static buffer and is not
        // thread-safe on its own. Every call site in this class reaches this
        // function only after acquiring loggerMutex first, which serializes
        // access and makes this safe *as currently used*.
        auto currentTime = std::chrono::system_clock::now();
        std::time_t timeFormat = std::chrono::system_clock::to_time_t(currentTime);
        std::string timeString = std::ctime(&timeFormat);

        // Remove trailing newline injected by ctime
        bool isNotEmpty = (timeString.empty() == false);
        if (isNotEmpty == true) {
            timeString.pop_back();
        }
        return timeString;
    }

    // Single shared formatter used by logInfo/logDebug/logError so the
    // timestamp/level/routing logic exists in exactly one place.
    void writeFormattedLine(bool isErrorLevel, const std::string& levelLabel, const std::string& message) {
        std::lock_guard<std::mutex> lockGuard(loggerMutex);

        std::string formattedMessage;
        formattedMessage.reserve(message.size() + levelLabel.size() + 40);
        formattedMessage += "[";
        formattedMessage += getCurrentTimestamp();
        formattedMessage += "] [";
        formattedMessage += levelLabel;
        formattedMessage += "] ";
        formattedMessage += message;
        formattedMessage += "\n";

        if (areFilesInitialized == true) {
            std::ofstream& targetStream = isErrorLevel ? errorFileStream : logFileStream;
            
            // Double-guard against offset manipulation: force seek to actual end
            // before pushing the buffer into the stream.
            targetStream.seekp(0, std::ios_base::end);
            
            targetStream << formattedMessage;
            targetStream.flush();
        } else {
            PendingLog pendingLog;
            pendingLog.isError = isErrorLevel;
            pendingLog.message = formattedMessage;
            pendingLogsList.push_back(pendingLog);
        }
    }

public:
    ~VerboseLogger() {
        bool isLogFileOpen = logFileStream.is_open();
        if (isLogFileOpen == true) {
            logFileStream.close();
        }

        bool isErrorFileOpen = errorFileStream.is_open();
        if (isErrorFileOpen == true) {
            errorFileStream.close();
        }

        bool isStillUninitialized = (areFilesInitialized == false);
        if (isStillUninitialized == true) {
            for (const PendingLog& pendingLog : pendingLogsList) {
                std::cerr << pendingLog.message;
            }
        }
    }

    void initializeFiles(const fs::path& outputDirectory) {
        std::lock_guard<std::mutex> lockGuard(loggerMutex);

        fs::path logFilePath = outputDirectory / "log.txt";
        fs::path errorFilePath = outputDirectory / "error.txt";

        // Opens using out | app to guarantee creation and append mode behaviors 
        // across all standard library implementations.
        logFileStream.open(logFilePath, std::ios_base::out | std::ios_base::app);
        errorFileStream.open(errorFilePath, std::ios_base::out | std::ios_base::app);

        areFilesInitialized = true;

        for (const PendingLog& pendingLog : pendingLogsList) {
            bool isErrorLog = (pendingLog.isError == true);
            if (isErrorLog == true) {
                errorFileStream << pendingLog.message;
            } else {
                logFileStream << pendingLog.message;
            }
        }
        
        errorFileStream.flush();
        logFileStream.flush();
        pendingLogsList.clear();
    }

    void logInfo(const std::string& message) {
        writeFormattedLine(false, "INFO", message);
    }

    void logDebug(const std::string& message) {
        writeFormattedLine(false, "DEBUG", message);
    }

    void logError(const std::string& message) {
        writeFormattedLine(true, "ERROR", message);
    }

    void logProcessingBlock(const std::string& actionType, const std::string& inputPath, const std::string& outputPath, const std::string& extraDetail = "") {
        std::string message = "[" + actionType + "] " + inputPath + " -> " + outputPath;
        bool hasExtraDetail = (extraDetail.empty() == false);
        if (hasExtraDetail == true) {
            message += " | Detail: " + extraDetail;
        }
        logInfo(message);
    }
};

/**
 * Single Responsibility Principle (SRP):
 * Utilizes OS-level POSIX file locks to guarantee only a single program instance is running.
 */
class SingleInstanceLocker {
private:
    int fileDescriptor;

public:
    SingleInstanceLocker(const fs::path& lockFilePath, VerboseLogger& logger) : fileDescriptor(-1) {
        std::string lockPathString = lockFilePath.string();

        fileDescriptor = open(lockPathString.c_str(), O_CREAT | O_RDWR, 0666);

        bool isFileOpenFailed = (fileDescriptor < 0);
        if (isFileOpenFailed == true) {
            throw std::runtime_error("CRITICAL: Failed to create or open lock file at: " + lockPathString);
        }

        int lockResult = flock(fileDescriptor, LOCK_EX | LOCK_NB);
        bool isLockAcquisitionFailed = (lockResult != 0);

        if (isLockAcquisitionFailed == true) {
            close(fileDescriptor);
            fileDescriptor = -1;
            throw std::runtime_error("CRITICAL: Another instance of this program is already running. Halting execution.");
        }

        logger.logDebug("Acquired exclusive OS-level single-instance lock safely.");
    }

    ~SingleInstanceLocker() {
        bool isFileDescriptorValid = (fileDescriptor >= 0);
        if (isFileDescriptorValid == true) {
            flock(fileDescriptor, LOCK_UN);
            close(fileDescriptor);
        }
    }

    SingleInstanceLocker(const SingleInstanceLocker&) = delete;
    SingleInstanceLocker& operator=(const SingleInstanceLocker&) = delete;
};

/**
 * Single Responsibility Principle (SRP):
 * This class ensures paths do not traverse outside allowed boundaries using symlinks or "..".
 */
class SecurityValidator {
private:
    VerboseLogger& loggerRef;

public:
    SecurityValidator(VerboseLogger& logger) : loggerRef(logger) {}

    bool isPathWithinAllowedDirectory(const fs::path& targetPath, const fs::path& allowedBaseDirectory) {
        std::error_code resolutionErrorCode;

        fs::path canonicalTargetPath = fs::weakly_canonical(targetPath, resolutionErrorCode);
        bool didTargetResolveFail = (resolutionErrorCode.value() != 0);
        if (didTargetResolveFail == true) {
            loggerRef.logError("Failed to resolve canonical path for target: " + targetPath.string());
            return false;
        }

        fs::path canonicalBasePath = fs::weakly_canonical(allowedBaseDirectory, resolutionErrorCode);
        bool didBaseResolveFail = (resolutionErrorCode.value() != 0);
        if (didBaseResolveFail == true) {
            loggerRef.logError("Failed to resolve canonical path for base: " + allowedBaseDirectory.string());
            return false;
        }

        std::string targetString = canonicalTargetPath.string();
        std::string baseString = canonicalBasePath.string();

        bool isBaseStringEmpty = baseString.empty();
        if (isBaseStringEmpty == true) {
            loggerRef.logError("Resolved base directory is empty; refusing to validate against it.");
            return false;
        }

        char preferredSeparator = fs::path::preferred_separator;
        bool doesBaseEndWithSeparator = (baseString.back() == preferredSeparator);
        if (doesBaseEndWithSeparator == false) {
            baseString += preferredSeparator;
        }

        bool doesTargetStartWithBase = (targetString.compare(0, baseString.size(), baseString) == 0);

        if (doesTargetStartWithBase == false) {
            loggerRef.logError("SECURITY ALERT: Path traversal attempted. Denied access to: " + targetString);
        }

        return doesTargetStartWithBase;
    }
};

/**
 * Single Responsibility Principle (SRP):
 * This class is solely responsible for converting or copying audio files.
 */
class AudioProcessor {
private:
    VerboseLogger& loggerRef;

    static const size_t MAX_CAPTURED_OUTPUT_BYTES = 65536;

    struct SubprocessResult {
        bool exitedNormally;
        int exitCode;
        std::string combinedOutput;
    };

    // RAII guard so a file descriptor is always closed.
    class FileDescriptorGuard {
    private:
        int descriptor;
    public:
        explicit FileDescriptorGuard(int fd) : descriptor(fd) {}
        ~FileDescriptorGuard() {
            if (descriptor >= 0) {
                close(descriptor);
            }
        }
        FileDescriptorGuard(const FileDescriptorGuard&) = delete;
        FileDescriptorGuard& operator=(const FileDescriptorGuard&) = delete;
        int get() const { return descriptor; }
    };

    SubprocessResult runSubprocessCapturingOutput(const std::vector<std::string>& argumentList) {
        int pipeFileDescriptors[2];
        bool didPipeSucceed = (pipe(pipeFileDescriptors) == 0);
        if (didPipeSucceed == false) {
            return { false, -1, "Failed to create pipe for ffmpeg subprocess." };
        }

        pid_t childProcessId = fork();
        bool didForkFail = (childProcessId < 0);
        if (didForkFail == true) {
            close(pipeFileDescriptors[0]);
            close(pipeFileDescriptors[1]);
            return { false, -1, "Failed to fork ffmpeg subprocess." };
        }

        bool isChildProcess = (childProcessId == 0);
        if (isChildProcess == true) {
            // --- Child process ---
            close(pipeFileDescriptors[0]);
            dup2(pipeFileDescriptors[1], STDOUT_FILENO);
            dup2(pipeFileDescriptors[1], STDERR_FILENO);
            close(pipeFileDescriptors[1]);

            // FIX: Prevent "NUL" byte corruption in log files.
            // The C++ logger keeps file descriptors open for the log files. By default, 
            // fork() shares these descriptors with the child process. When ffprobe/ffmpeg 
            // runs, it aggressively probes available file descriptors for media tags 
            // (e.g., seeking to EOF to look for ID3 tags). Because the file offset is 
            // shared between parent and child, ffprobe's seeking advances the parent's 
            // log file offset past the actual end of the text. When the parent subsequently 
            // writes a log line, the OS fills the gap with NUL (\0) characters.
            // Closing all FDs above STDERR (2) before execvp prevents this interference.
            long maxFd = sysconf(_SC_OPEN_MAX);
            if (maxFd < 0 || maxFd > 65536) {
                maxFd = 8192; // Sensible fallback limit if sysconf fails
            }
            for (int fd = 3; fd < maxFd; ++fd) {
                close(fd);
            }

            std::vector<char*> execArgv;
            execArgv.reserve(argumentList.size() + 1);
            for (const std::string& argument : argumentList) {
                execArgv.push_back(const_cast<char*>(argument.c_str()));
            }
            execArgv.push_back(nullptr);

            execvp(execArgv[0], execArgv.data());
            // execvp() only returns if it failed to launch ffmpeg at all.
            _exit(127);
        }

        // --- Parent process ---
        close(pipeFileDescriptors[1]);
        FileDescriptorGuard readEndGuard(pipeFileDescriptors[0]);

        std::string collectedOutput;
        bool wasOutputTruncated = false;
        char readBuffer[4096];
        ssize_t bytesRead;

        while ((bytesRead = read(readEndGuard.get(), readBuffer, sizeof(readBuffer))) > 0) {
            size_t remainingCapacity = (collectedOutput.size() < MAX_CAPTURED_OUTPUT_BYTES)
                ? (MAX_CAPTURED_OUTPUT_BYTES - collectedOutput.size())
                : 0;
            size_t bytesToKeep = std::min(static_cast<size_t>(bytesRead), remainingCapacity);

            if (bytesToKeep > 0) {
                collectedOutput.append(readBuffer, bytesToKeep);
            }
            if (bytesToKeep < static_cast<size_t>(bytesRead)) {
                wasOutputTruncated = true;
            }
        }

        if (wasOutputTruncated == true) {
            collectedOutput += "\n[... output truncated ...]\n";
        }

        int waitStatus = 0;
        pid_t waitResult;
        do {
            waitResult = waitpid(childProcessId, &waitStatus, 0);
        } while (waitResult == -1 && errno == EINTR);

        bool didWaitFail = (waitResult < 0);
        if (didWaitFail == true) {
            collectedOutput += "\nFailed to wait for ffmpeg subprocess.";
            return { false, -1, collectedOutput };
        }

        if (WIFEXITED(waitStatus)) {
            int exitCode = WEXITSTATUS(waitStatus);
            return { true, exitCode, collectedOutput };
        }

        if (WIFSIGNALED(waitStatus)) {
            int signalNumber = WTERMSIG(waitStatus);
            collectedOutput += "\nffmpeg was terminated by signal " + std::to_string(signalNumber) + ".";
            return { false, -1, collectedOutput };
        }

        collectedOutput += "\nffmpeg subprocess ended in an unexpected state.";
        return { false, -1, collectedOutput };
    }

public:
    AudioProcessor(VerboseLogger& logger) : loggerRef(logger) {}

    bool convertAudioFile(const fs::path& inputFilePath, const fs::path& outputFilePath, const std::string& targetBitrateKbps, bool shouldNormalizeAudioSpec) {
        std::string inputPathString = inputFilePath.string();
        std::string outputPathString = outputFilePath.string();

        std::vector<std::string> argumentList;
        argumentList.push_back("ffmpeg");
        argumentList.push_back("-y");
        argumentList.push_back("-v");
        argumentList.push_back("error");
        argumentList.push_back("-i");
        argumentList.push_back(inputPathString);
        
        // --- Hardcoded MP3 Muxing Block ---
        // Since the target is now strictly MP3, we map audio (0:a)
        // and optionally video (0:v?) strictly to preserve cover art/ID3v2 tags.
        argumentList.push_back("-map");
        argumentList.push_back("0:a");
        argumentList.push_back("-map");
        argumentList.push_back("0:v?");
        argumentList.push_back("-map_metadata");
        argumentList.push_back("0");
        argumentList.push_back("-c:v");
        argumentList.push_back("copy");

        // Force explicit high-quality MP3 encoding
        argumentList.push_back("-c:a");
        argumentList.push_back("libmp3lame");

        if (shouldNormalizeAudioSpec == true) {
            // FEATURE: force non-compliant sources down to 16-bit/44.1kHz. 
            // The "soxr" resampler is requested over ffmpeg's default "swr".
            argumentList.push_back("-af");
            argumentList.push_back("aresample=resampler=soxr:precision=28,aformat=sample_fmts=s16:sample_rates=44100");
        }

        argumentList.push_back("-b:a");
        argumentList.push_back(targetBitrateKbps + "k");
        argumentList.push_back(outputPathString);

        SubprocessResult result = runSubprocessCapturingOutput(argumentList);

        bool isConversionSuccessful = (result.exitedNormally == true && result.exitCode == SUCCESS_EXIT_CODE);

        if (isConversionSuccessful == true) {
            bool doesFileExist = fs::exists(outputFilePath);
            if (doesFileExist == true) {
                auto fileSizeOutput = fs::file_size(outputFilePath);
                bool isFileValid = (fileSizeOutput > 0);

                if (isFileValid == true) {
                    std::string successDetail = "Bitrate: " + targetBitrateKbps + " kbps";
                    bool wasNormalized = (shouldNormalizeAudioSpec == true);
                    if (wasNormalized == true) {
                        successDetail += " | Normalized 16-bit/44.1kHz";
                    }
                    loggerRef.logProcessingBlock("CONVERTED", inputPathString, outputPathString, successDetail);
                    return true;
                }
            }

            // Build advanced error diagnostic payload if it "succeeded" but wrote a corrupted 0-byte file
            std::string corruptedFileError = "ffmpeg reported success, but output file is missing or 0 bytes: " + inputPathString;
            bool hasRawOutput = (result.combinedOutput.empty() == false);
            if (hasRawOutput == true) {
                corruptedFileError += "\n>>> RAW FFMPEG OUTPUT >>>\n" + result.combinedOutput;
            }
            loggerRef.logError(corruptedFileError);
            return false;
        }

        // Build advanced error diagnostic payload for hard failures
        std::string failureError = "ffmpeg command failed for: " + inputPathString;
        bool hasRawOutput = (result.combinedOutput.empty() == false);
        if (hasRawOutput == true) {
            failureError += "\n>>> RAW FFMPEG ERROR OUTPUT >>>\n" + result.combinedOutput;
        }
        loggerRef.logError(failureError);

        return false;
    }

    struct AudioSpecProbeResult {
        bool didProbeSucceed;
        int sampleRateHz;
        int bitsPerRawSample;
    };

    // FEATURE: inspects the first audio stream of a file with ffprobe (this
    // reads container/stream metadata only, it does not decode audio, so it
    // is cheap) so the caller can decide whether a file already complies
    // with the 16-bit/44.1kHz-or-48kHz compatibility rule.
    AudioSpecProbeResult probeAudioSpecification(const fs::path& inputFilePath) {
        std::string inputPathString = inputFilePath.string();

        std::vector<std::string> argumentList;
        argumentList.push_back("ffprobe");
        argumentList.push_back("-v");
        argumentList.push_back("error");
        argumentList.push_back("-select_streams");
        argumentList.push_back("a:0");
        argumentList.push_back("-show_entries");
        argumentList.push_back("stream=sample_rate,bits_per_raw_sample");
        argumentList.push_back("-of");
        argumentList.push_back("default=noprint_wrappers=1:nokey=0");
        argumentList.push_back(inputPathString);

        SubprocessResult result = runSubprocessCapturingOutput(argumentList);

        AudioSpecProbeResult probeResult;
        probeResult.didProbeSucceed = false;
        probeResult.sampleRateHz = 0;
        probeResult.bitsPerRawSample = -1;

        bool didProbeExitCleanly = (result.exitedNormally == true && result.exitCode == SUCCESS_EXIT_CODE);
        if (didProbeExitCleanly == false) {
            loggerRef.logError("ffprobe failed to inspect audio stream for: " + inputPathString);
            return probeResult;
        }

        std::istringstream outputStream(result.combinedOutput);
        std::string currentLine;

        while (std::getline(outputStream, currentLine)) {
            size_t equalsSignPosition = currentLine.find('=');
            bool hasEqualsSign = (equalsSignPosition != std::string::npos);
            if (hasEqualsSign == false) {
                continue;
            }

            std::string keyName = currentLine.substr(0, equalsSignPosition);
            std::string valueString = currentLine.substr(equalsSignPosition + 1);

            bool isSampleRateKey = (keyName == "sample_rate");
            bool isBitsPerRawSampleKey = (keyName == "bits_per_raw_sample");

            if (isSampleRateKey == true) {
                try {
                    probeResult.sampleRateHz = std::stoi(valueString);
                } catch (...) {
                    probeResult.sampleRateHz = 0;
                }
            } else if (isBitsPerRawSampleKey == true) {
                try {
                    probeResult.bitsPerRawSample = std::stoi(valueString);
                } catch (...) {
                    probeResult.bitsPerRawSample = -1;
                }
            }
        }

        probeResult.didProbeSucceed = (probeResult.sampleRateHz > 0);
        return probeResult;
    }

    bool copyAudioFile(const fs::path& inputFilePath, const fs::path& outputFilePath) {
        std::error_code fileSystemErrorCode;
        fs::copy_options optionsForCopy = fs::copy_options::overwrite_existing;

        bool didCopySucceed = fs::copy_file(inputFilePath, outputFilePath, optionsForCopy, fileSystemErrorCode);

        bool hasErrorOccurred = (fileSystemErrorCode.value() != 0);
        if (hasErrorOccurred == true) {
            loggerRef.logError("Failed to copy file: " + fileSystemErrorCode.message());
            return false;
        }

        if (didCopySucceed == true) {
            loggerRef.logProcessingBlock("COPIED", inputFilePath.string(), outputFilePath.string(), "Native MP3 Copied Directly");
        }

        return didCopySucceed;
    }
};

struct ProcessingTask {
    fs::path inputPath;
    fs::path outputPath;
    bool requiresConversion;
    std::string fileExtension;
    bool needsAudioSpecNormalization = false;
};

/**
 * Single Responsibility Principle (SRP):
 * This class handles multi-threaded checking and state tracking of the directories.
 */
class DirectorySynchronizer {
private:
    fs::path inputDirectoryPath;
    fs::path outputDirectoryPath;
    std::string userSelectedBitrate;
    int maxWorkerThreads;

    AudioProcessor audioProcessorInstance;
    VerboseLogger& loggerRef;
    SecurityValidator securityValidatorInstance;

    std::set<std::string> validExpectedOutputFilesSet;
    std::mutex trackingSetMutex;

    bool checkIfFileRequiresUpdate(const fs::path& inputFilePath, const fs::path& outputFilePath) {
        bool doesOutputFileExist = fs::exists(outputFilePath);
        bool isOutputMissing = (doesOutputFileExist == false);

        if (isOutputMissing == true) {
            return true;
        }

        std::error_code errorCodeInput;
        std::error_code errorCodeOutput;

        auto inputWriteTime = fs::last_write_time(inputFilePath, errorCodeInput);
        auto outputWriteTime = fs::last_write_time(outputFilePath, errorCodeOutput);

        bool didReadInputTimeFail = (errorCodeInput.value() != 0);
        bool didReadOutputTimeFail = (errorCodeOutput.value() != 0);

        if (didReadInputTimeFail == true || didReadOutputTimeFail == true) {
            loggerRef.logError("Could not read modification time for '" + inputFilePath.string() +
                                "' or '" + outputFilePath.string() + "'. Reprocessing to be safe.");
            return true;
        }

        auto timeDifference = (inputWriteTime - outputWriteTime);
        return (timeDifference.count() > 0);
    }

    bool isFileExcluded(const std::string& fileName) {
        for (const std::string& currentPrefix : EXCLUDED_PREFIXES) {
            bool doesStartWithPrefix = (fileName.compare(0, currentPrefix.size(), currentPrefix) == 0);
            if (doesStartWithPrefix == true) {
                return true;
            }
        }
        return false;
    }

public:
    DirectorySynchronizer(const fs::path& inputDir, const fs::path& outputDir, const std::string& targetBitrate, int requestedThreads, VerboseLogger& logger)
        : audioProcessorInstance(logger), loggerRef(logger), securityValidatorInstance(logger) {

        std::error_code canonicalErrorCode;

        inputDirectoryPath = fs::weakly_canonical(inputDir, canonicalErrorCode);
        outputDirectoryPath = fs::weakly_canonical(outputDir, canonicalErrorCode);

        userSelectedBitrate = targetBitrate;
        maxWorkerThreads = requestedThreads;

        loggerRef.logInfo("Directory Synchronizer initialized.");
        loggerRef.logDebug("Input boundary set to: " + inputDirectoryPath.string());
        loggerRef.logDebug("Output boundary set to: " + outputDirectoryPath.string());
        loggerRef.logDebug("Target format strictly configured to: MP3");
        loggerRef.logDebug("Target bitrate configured to: " + userSelectedBitrate + " kbps");
        loggerRef.logDebug("Parallel streams configured to: " + std::to_string(maxWorkerThreads));
    }

    void processInputToOutput() {
        loggerRef.logInfo("Starting sequential forward mapping pass: scanning input directory & building queues.");

        std::vector<ProcessingTask> threadTasksList;
        
        // FIX: tracks every output path (lowercased) that has already been
        // claimed by another input file during this same sequential pass.
        std::set<std::string> lowercaseOutputPathsClaimed;

        std::error_code iteratorErrorCode;
        fs::directory_options iteratorOptions = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator directoryIterator(inputDirectoryPath, iteratorOptions, iteratorErrorCode);
        fs::recursive_directory_iterator endIterator;

        bool isIteratorValid = (iteratorErrorCode.value() == 0);
        if (isIteratorValid == false) {
            loggerRef.logError("Error accessing input directory.");
            return;
        }

        // --- Phase 1: Sequential Map ---
        for (; directoryIterator != endIterator; ++directoryIterator) {
            const fs::directory_entry& currentEntry = *directoryIterator;

            bool isRegularFile = currentEntry.is_regular_file();
            if (isRegularFile == false) {
                continue;
            }

            fs::path inputFilePath = currentEntry.path();
            std::string fileNameString = inputFilePath.filename().string();

            bool isExcludedPrefixMatch = isFileExcluded(fileNameString);
            if (isExcludedPrefixMatch == true) {
                loggerRef.logDebug("Ignoring excluded file rule match: " + fileNameString);
                continue;
            }

            bool isInputSafe = securityValidatorInstance.isPathWithinAllowedDirectory(inputFilePath, inputDirectoryPath);
            if (isInputSafe == false) {
                loggerRef.logError("Skipping unverified input path: " + inputFilePath.string());
                continue;
            }

            std::string originalFileExtension = inputFilePath.extension().string();
            std::string fileExtension = toLowercaseString(originalFileExtension);

            bool isMp3 = (fileExtension == EXTENSION_MP3);
            bool isFlac = (fileExtension == EXTENSION_FLAC);
            bool isAlac = (fileExtension == EXTENSION_ALAC);
            bool isM4a = (fileExtension == EXTENSION_M4A);
            bool isAac = (fileExtension == EXTENSION_AAC);

            bool isSupportedFormat = (isMp3 == true || isFlac == true || isAlac == true || isM4a == true || isAac == true);

            if (isSupportedFormat == false) {
                continue;
            }

            fs::path relativePathFromInput = fs::relative(inputFilePath, inputDirectoryPath);
            fs::path expectedOutputFilePath = outputDirectoryPath / relativePathFromInput;
            
            // Hardcode extension replacement to MP3
            expectedOutputFilePath.replace_extension(EXTENSION_MP3);

            bool isOutputSafe = securityValidatorInstance.isPathWithinAllowedDirectory(expectedOutputFilePath, outputDirectoryPath);
            if (isOutputSafe == false) {
                loggerRef.logError("Skipping unverified output path calculation: " + expectedOutputFilePath.string());
                continue;
            }

            fs::path relativeOutputPath = expectedOutputFilePath.lexically_relative(outputDirectoryPath);
            std::string lowercaseRelativeOutputPath = toLowercaseString(relativeOutputPath.string());

            bool isDuplicateOutputTarget = (lowercaseOutputPathsClaimed.find(lowercaseRelativeOutputPath) != lowercaseOutputPathsClaimed.end());
            if (isDuplicateOutputTarget == true) {
                loggerRef.logError("Skipping file: its output path collides with another already-queued file "
                                    "(likely a case-only filename difference): " + inputFilePath.string());
                continue;
            }
            lowercaseOutputPathsClaimed.insert(lowercaseRelativeOutputPath);

            // Create directories sequentially to prevent multi-threaded filesystem races
            fs::path expectedOutputDirectory = expectedOutputFilePath.parent_path();
            bool doesOutputDirectoryExist = fs::exists(expectedOutputDirectory);

            if (doesOutputDirectoryExist == false) {
                std::error_code directoryCreationErrorCode;
                fs::create_directories(expectedOutputDirectory, directoryCreationErrorCode);
            }

            try {
                bool needsProcessing = checkIfFileRequiresUpdate(inputFilePath, expectedOutputFilePath);

                if (needsProcessing == true) {
                    AudioProcessor::AudioSpecProbeResult probeResult = audioProcessorInstance.probeAudioSpecification(inputFilePath);

                    bool isSampleRateCompliant = true;
                    if (probeResult.didProbeSucceed == true) {
                        bool isStandardCdRate = (probeResult.sampleRateHz == 44100);
                        bool isStandardVideoRate = (probeResult.sampleRateHz == 48000);
                        isSampleRateCompliant = (isStandardCdRate == true || isStandardVideoRate == true);
                    }

                    bool isBitDepthKnown = (probeResult.bitsPerRawSample > 0);
                    bool isBitDepthCompliant = true;
                    if (isBitDepthKnown == true) {
                        isBitDepthCompliant = (probeResult.bitsPerRawSample == 16);
                    }

                    bool needsAudioSpecNormalization = (isSampleRateCompliant == false || isBitDepthCompliant == false);

                    bool isAlreadyTargetFormat = (fileExtension == EXTENSION_MP3);
                    bool requiresConversion = (isAlreadyTargetFormat == false) || (needsAudioSpecNormalization == true);

                    ProcessingTask newTaskDefinition;
                    newTaskDefinition.inputPath = inputFilePath;
                    newTaskDefinition.outputPath = expectedOutputFilePath;
                    newTaskDefinition.requiresConversion = requiresConversion;
                    newTaskDefinition.fileExtension = fileExtension;
                    newTaskDefinition.needsAudioSpecNormalization = needsAudioSpecNormalization;

                    threadTasksList.push_back(newTaskDefinition);
                } else {
                    validExpectedOutputFilesSet.insert(relativeOutputPath.string());
                }
            } catch (const fs::filesystem_error& fileSystemException) {
                loggerRef.logError("File system error during sequential mapping: " + std::string(fileSystemException.what()));
            }
        }

        loggerRef.logInfo("Sequential mapping completed. Files queued for processing: " + std::to_string(threadTasksList.size()));

        // --- Phase 2: Parallel Execution ---
        if (threadTasksList.empty() == false) {
            std::atomic<size_t> currentTaskIndex = 0;
            std::vector<std::thread> activeWorkerPool;
            size_t totalTasksSize = threadTasksList.size();

            for (int threadIdentifier = 0; threadIdentifier < maxWorkerThreads; ++threadIdentifier) {
                activeWorkerPool.emplace_back([this, &threadTasksList, &currentTaskIndex, totalTasksSize]() {
                    while (true) {
                        size_t myAssignedIndex = currentTaskIndex.fetch_add(1);

                        bool isWorkExhausted = (myAssignedIndex >= totalTasksSize);
                        if (isWorkExhausted == true) {
                            break;
                        }

                        ProcessingTask& myTaskReference = threadTasksList[myAssignedIndex];
                        bool isProcessingSuccessful = true;

                        try {
                            if (myTaskReference.requiresConversion == true) {
                                bool conversionResult = audioProcessorInstance.convertAudioFile(myTaskReference.inputPath, myTaskReference.outputPath, userSelectedBitrate, myTaskReference.needsAudioSpecNormalization);
                                bool isFailed = (conversionResult == false);
                                if (isFailed == true) {
                                    // Primary failure logging is handled inside AudioProcessor.
                                    isProcessingSuccessful = false;
                                }
                            } else {
                                bool copyResult = audioProcessorInstance.copyAudioFile(myTaskReference.inputPath, myTaskReference.outputPath);
                                bool isFailed = (copyResult == false);
                                if (isFailed == true) {
                                    loggerRef.logError("Failed to copy: " + myTaskReference.inputPath.string());
                                    isProcessingSuccessful = false;
                                }
                            }
                        } catch (const std::exception& exceptionBlock) {
                            loggerRef.logError("Fatal error processing file: " + std::string(exceptionBlock.what()));
                            isProcessingSuccessful = false;
                        }

                        if (isProcessingSuccessful == true) {
                            fs::path relativeMemoryPath = myTaskReference.outputPath.lexically_relative(outputDirectoryPath);
                            std::string relativePathString = relativeMemoryPath.string();

                            std::lock_guard<std::mutex> lockGuard(trackingSetMutex);
                            validExpectedOutputFilesSet.insert(relativePathString);
                        } else {
                            std::error_code removalErrorCode;
                            fs::remove(myTaskReference.outputPath, removalErrorCode);
                        }
                    }
                });
            }

            loggerRef.logInfo("Waiting for " + std::to_string(maxWorkerThreads) + " worker threads to finish processing...");
            for (std::thread& workerThread : activeWorkerPool) {
                bool isThreadJoinable = workerThread.joinable();
                if (isThreadJoinable == true) {
                    workerThread.join();
                }
            }
        }

        loggerRef.logInfo("Finished parallel forward execution.");
    }

    void cleanUpOrphanedOutputFiles() {
        loggerRef.logInfo("Starting backward pass: scanning output directory for orphans.");

        std::error_code iteratorErrorCode;
        fs::directory_options iteratorOptions = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator directoryIterator(outputDirectoryPath, iteratorOptions, iteratorErrorCode);
        fs::recursive_directory_iterator endIterator;

        bool isIteratorValid = (iteratorErrorCode.value() == 0);
        if (isIteratorValid == false) {
            loggerRef.logError("Error accessing output directory.");
            return;
        }

        for (; directoryIterator != endIterator; ++directoryIterator) {
            const fs::directory_entry& currentEntry = *directoryIterator;

            bool isRegularFile = currentEntry.is_regular_file();
            if (isRegularFile == false) {
                continue;
            }

            fs::path outputFilePath = currentEntry.path();

            bool isOutputFileSafe = securityValidatorInstance.isPathWithinAllowedDirectory(outputFilePath, outputDirectoryPath);
            if (isOutputFileSafe == false) {
                loggerRef.logError("Skipping unverified output file during cleanup: " + outputFilePath.string());
                continue;
            }

            std::string originalFileExtension = outputFilePath.extension().string();
            std::string fileExtension = toLowercaseString(originalFileExtension);

            bool isTargetFormat = (fileExtension == EXTENSION_MP3);
            if (isTargetFormat == false) {
                continue;
            }

            fs::path relativeMemoryPath = outputFilePath.lexically_relative(outputDirectoryPath);
            std::string currentRelativePathString = relativeMemoryPath.string();

            auto setIterator = validExpectedOutputFilesSet.find(currentRelativePathString);
            auto setEnd = validExpectedOutputFilesSet.end();

            bool isFileInValidSet = (setIterator != setEnd);
            bool shouldDeleteOutputFile = (isFileInValidSet == false);

            if (shouldDeleteOutputFile == true) {
                try {
                    std::error_code removalErrorCode;
                    fs::remove(outputFilePath, removalErrorCode);
                    loggerRef.logProcessingBlock("ORPHAN CLEANED", "Source Missing", outputFilePath.string());
                } catch (const fs::filesystem_error& fileSystemException) {
                    loggerRef.logError("Error deleting orphaned file: " + std::string(fileSystemException.what()));
                }
            }
        }
        loggerRef.logInfo("Finished backward cleanup pass.");

        // FEATURE: mirror subfolder deletions from the input directory into
        // the output directory.
        removeEmptyOrphanedDirectories();
    }

    void removeEmptyOrphanedDirectories() {
        loggerRef.logInfo("Scanning for empty orphaned directories left behind in the output tree.");

        std::error_code iteratorErrorCode;
        fs::directory_options iteratorOptions = fs::directory_options::skip_permission_denied;
        fs::recursive_directory_iterator directoryIterator(outputDirectoryPath, iteratorOptions, iteratorErrorCode);
        fs::recursive_directory_iterator endIterator;

        bool isIteratorValid = (iteratorErrorCode.value() == 0);
        if (isIteratorValid == false) {
            loggerRef.logError("Error accessing output directory while scanning for empty directories.");
            return;
        }

        std::vector<fs::path> discoveredDirectories;
        for (; directoryIterator != endIterator; ++directoryIterator) {
            const fs::directory_entry& currentEntry = *directoryIterator;

            bool isDirectory = currentEntry.is_directory();
            if (isDirectory == true) {
                discoveredDirectories.push_back(currentEntry.path());
            }
        }

        // Removing in reverse order guarantees every child directory is considered 
        // before its parent.
        std::reverse(discoveredDirectories.begin(), discoveredDirectories.end());

        for (const fs::path& currentDirectoryPath : discoveredDirectories) {
            bool isDirectorySafe = securityValidatorInstance.isPathWithinAllowedDirectory(currentDirectoryPath, outputDirectoryPath);
            if (isDirectorySafe == false) {
                loggerRef.logError("Skipping unverified directory during empty-folder cleanup: " + currentDirectoryPath.string());
                continue;
            }

            std::error_code emptyCheckErrorCode;
            bool isDirectoryEmpty = fs::is_empty(currentDirectoryPath, emptyCheckErrorCode);
            bool didEmptyCheckFail = (emptyCheckErrorCode.value() != 0);

            if (didEmptyCheckFail == true) {
                continue;
            }

            if (isDirectoryEmpty == true) {
                std::error_code removalErrorCode;
                bool didRemoveSucceed = fs::remove(currentDirectoryPath, removalErrorCode);
                bool didRemovalFail = (removalErrorCode.value() != 0 || didRemoveSucceed == false);

                if (didRemovalFail == false) {
                    loggerRef.logProcessingBlock("FOLDER REMOVED", "Source Empty", currentDirectoryPath.string());
                }
            }
        }

        loggerRef.logInfo("Finished empty orphaned directory cleanup pass.");
    }
};

int main(int argumentCount, char* argumentValuesArray[]) {
    // Centrally managed thread-safe file logger
    VerboseLogger mainLoggerInstance;
    mainLoggerInstance.logInfo("Application Startup Initiated.");

    // Exactly 5 arguments required now
    int expectedArgumentCount = 5;
    bool hasCorrectArgumentCount = (argumentCount == expectedArgumentCount);

    if (hasCorrectArgumentCount == false) {
        mainLoggerInstance.logError("Incorrect arguments provided.");
        std::cerr << "Usage: " << argumentValuesArray[0] << " <InputFolder> <OutputFolder> <BitrateKbps> <ParallelThreads>" << std::endl;
        std::cerr << "Example: " << argumentValuesArray[0] << " /path/to/music /path/to/mp3_export 320 4" << std::endl;
        return ERROR_EXIT_CODE;
    }

    std::string inputPathString = argumentValuesArray[1];
    std::string outputPathString = argumentValuesArray[2];
    std::string bitrateString = argumentValuesArray[3];
    std::string threadString = argumentValuesArray[4];

    // --- Directory Validation Block ---
    fs::path inputDirectory = fs::path(inputPathString);
    fs::path outputDirectory = fs::path(outputPathString);

    bool doesInputExist = fs::exists(inputDirectory);
    bool isInputADirectory = fs::is_directory(inputDirectory);
    bool isInputValid = (doesInputExist == true && isInputADirectory == true);

    if (isInputValid == false) {
        mainLoggerInstance.logError("The provided input directory does not exist or is not a directory.");
        return ERROR_EXIT_CODE;
    }

    bool doesOutputExist = fs::exists(outputDirectory);
    if (doesOutputExist == false) {
        mainLoggerInstance.logInfo("Output directory does not exist. Attempting creation.");
        std::error_code createDirectoryErrorCode;
        bool didCreateDirectorySucceed = fs::create_directories(outputDirectory, createDirectoryErrorCode);

        bool isCreationSuccessful = (didCreateDirectorySucceed == true);
        if (isCreationSuccessful == false) {
            mainLoggerInstance.logError("Could not create output directory. Terminating.");
            return ERROR_EXIT_CODE;
        }
    }

    // Safely route file logs to the output folder and engage POSIX locks
    mainLoggerInstance.initializeFiles(outputDirectory);
    fs::path lockFilePath = outputDirectory / ".audio_sync.lock";

    std::unique_ptr<SingleInstanceLocker> lockerInstance;
    try {
        lockerInstance = std::make_unique<SingleInstanceLocker>(lockFilePath, mainLoggerInstance);
    } catch (const std::exception& lockException) {
        mainLoggerInstance.logError(lockException.what());
        return ERROR_EXIT_CODE;
    }

    // --- Bitrate Parsing Block ---
    int parsedBitrateTarget = 0;
    try {
        parsedBitrateTarget = std::stoi(bitrateString);
    } catch (...) {
        mainLoggerInstance.logError("The provided bitrate is not a valid numeric integer.");
        return ERROR_EXIT_CODE;
    }

    bool isBitrateTooLow = (parsedBitrateTarget < MINIMUM_BITRATE_KBPS);
    bool isBitrateTooHigh = (parsedBitrateTarget > MAXIMUM_BITRATE_KBPS);
    bool isBitrateInvalid = (isBitrateTooLow == true || isBitrateTooHigh == true);

    if (isBitrateInvalid == true) {
        mainLoggerInstance.logError("The requested bitrate is outside the allowed specification bounds (32 to 320 kbps).");
        return ERROR_EXIT_CODE;
    }
    std::string validatedBitrateString = std::to_string(parsedBitrateTarget);

    // --- Thread Parsing Block ---
    int parsedThreadCount = 0;
    try {
        parsedThreadCount = std::stoi(threadString);
    } catch (...) {
        mainLoggerInstance.logError("The provided thread count is not a valid numeric integer.");
        return ERROR_EXIT_CODE;
    }

    bool isThreadCountTooLow = (parsedThreadCount < MINIMUM_THREAD_COUNT);
    bool isThreadCountTooHigh = (parsedThreadCount > MAXIMUM_THREAD_COUNT);
    bool isThreadCountInvalid = (isThreadCountTooLow == true || isThreadCountTooHigh == true);

    if (isThreadCountInvalid == true) {
        mainLoggerInstance.logError("Thread count exceeds logic bounds. Provide a value between 1 and 64.");
        return ERROR_EXIT_CODE;
    }

    // --- Execution Block ---
    DirectorySynchronizer synchronizerInstance(inputDirectory, outputDirectory, validatedBitrateString, parsedThreadCount, mainLoggerInstance);

    mainLoggerInstance.logInfo("Initiating Step 1: Processing input to output.");
    synchronizerInstance.processInputToOutput();

    mainLoggerInstance.logInfo("Initiating Step 2: Cleaning up orphaned output files.");
    synchronizerInstance.cleanUpOrphanedOutputFiles();

    mainLoggerInstance.logInfo("Application execution completed successfully.");
    return SUCCESS_EXIT_CODE;
}