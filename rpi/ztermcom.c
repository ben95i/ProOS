///////////////////////////////////////
/// Z-Terminal Serial Communication ///
/// for GNU/Linux /////////////////////
/// by Ben Provenzano III /////////////
/// 01/18/2019 v1 ///
/// 07/01/2020 v2 ///
/// 12/19/2021 v3 ///
/// 03/21/2022 v4 ///
/// 07/01/2023 v5 ///
/// 04/24/2025 v6 ///
/// 03/15/2026 v7 ///
/// 04/06/2026 v8 ///
/// 05/31/2026 v9 ///
/// 07/26/2026 v10 ///
/// 08/08/2026 v11 ///
//////////////////////

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <poll.h>
#include <sys/file.h>
#include <time.h>

// --- Configuration Constants ---
// Declared static so the compiler can fold them directly into the code
// instead of loading them through external linkage.
static const char TARGET_CHAR = '\n';
static const int MAX_WAIT_TIME_MS = 800; // needs to be greater then 500ms
static const int MIN_LOCK_TIME_MS = 15;
static const int MAX_TRANSMIT_ATTEMPTS = 2;

// Response line from the hub that must trigger an automatic
// re-transmission, exactly as if the response had timed out.
static const char RETRY_TRIGGER_MESSAGE[] = "*EXT-232 MAX WAIT EXCEEDED!*";

// Protocol framing applied to every outgoing payload: "<9,9," + payload + ">\n"
static const char FRAME_PREFIX[] = "<9,9,";
static const char FRAME_SUFFIX[] = ">\n";
#define FRAME_PREFIX_LENGTH (sizeof(FRAME_PREFIX) - 1)
#define FRAME_SUFFIX_LENGTH (sizeof(FRAME_SUFFIX) - 1)

// Maximum number of characters of a single response line that are
// retained for retry-trigger matching (excess is still printed,
// just not matched against the trigger message).
#define RESPONSE_LINE_BUFFER_SIZE 256

// Number of bytes pulled from the tty per read() call. Reading in blocks
// instead of one byte at a time removes a poll()/read() syscall pair for
// every single character of the response.
#define READ_CHUNK_BUFFER_SIZE 128

// Framed commands up to this length are assembled on the stack, so the
// common case performs no heap allocation at all.
#define TRANSMIT_STACK_BUFFER_SIZE 256

// --- Data Structures ---
/**
 * Struct to hold the context of the serial application to prevent
 * global variable race conditions and manage memory cleanly.
 */
typedef struct {
    int fileDescriptor;
    bool isPortLocked;
    struct timespec lockAcquiredTime;
    char* allocatedArgumentData;
} SerialContext;

// --- Function Prototypes ---
// All internal helpers are static: this gives the compiler freedom to
// inline them and drop the external symbols entirely.
static void cleanupAndClosePort(SerialContext* context);
static long elapsedMillisecondsSince(const struct timespec* startTime);
static int writeToSerial(int fileDescriptor, const char* payload);
static int readFromSerial(int fileDescriptor);
static int transmitCommandWithRetries(int fileDescriptor, const char* payload, int maximumAttempts);


/**
 * Computes the number of whole milliseconds elapsed on the monotonic
 * clock since the supplied start time.
 * * @param startTime Pointer to the reference monotonic timestamp.
 * @return Elapsed time in milliseconds.
 */
static long elapsedMillisecondsSince(const struct timespec* startTime) {
    struct timespec currentTime;
    int clockResult = clock_gettime(CLOCK_MONOTONIC, &currentTime);
    (void)clockResult;

    // Break down the elapsed time calculation for clarity
    time_t secondsDifference = currentTime.tv_sec - startTime->tv_sec;
    long millisecondsFromSeconds = (long)secondsDifference * 1000;

    long nanosecondsDifference = currentTime.tv_nsec - startTime->tv_nsec;
    long millisecondsFromNanoseconds = nanosecondsDifference / 1000000;

    long totalElapsedMilliseconds = millisecondsFromSeconds + millisecondsFromNanoseconds;
    return totalElapsedMilliseconds;
}

/**
 * Safely releases file locks, manages the minimum execution delay,
 * closes the file descriptor, and frees allocated memory.
 * * @param context Pointer to the application's serial state context.
 */
static void cleanupAndClosePort(SerialContext* context) {
    bool isArgumentDataAllocated = (context->allocatedArgumentData != NULL);
    if (isArgumentDataAllocated == true) {
        free(context->allocatedArgumentData);
        context->allocatedArgumentData = NULL;
    }

    bool isFileDescriptorValid = (context->fileDescriptor >= 0);
    if (isFileDescriptorValid == true) {

        bool isPortCurrentlyLocked = (context->isPortLocked == true);
        if (isPortCurrentlyLocked == true) {

            long totalElapsedMilliseconds = elapsedMillisecondsSince(&context->lockAcquiredTime);

            // Check if we need to wait to satisfy the minimum lock time constraint
            bool isExecutionTooFast = (totalElapsedMilliseconds < MIN_LOCK_TIME_MS);
            if (isExecutionTooFast == true) {
                long remainingMilliseconds = MIN_LOCK_TIME_MS - totalElapsedMilliseconds;
                long remainingMicroseconds = remainingMilliseconds * 1000;

                int sleepResult = usleep(remainingMicroseconds);
                (void)sleepResult;
            }

            // Explicitly unlock the port
            int unlockResult = flock(context->fileDescriptor, LOCK_UN);
            (void)unlockResult;

            context->isPortLocked = false;
        }

        int closeResult = close(context->fileDescriptor);
        (void)closeResult;

        context->fileDescriptor = -1;
    }
}

/**
 * Packages the payload string into the protocol format and transmits
 * it over the provided serial file descriptor. Handles partial writes
 * and transient EAGAIN conditions caused by the non-blocking port by
 * waiting for the descriptor to become writable and continuing until
 * every byte has been handed to the kernel.
 *
 * The frame is assembled with memcpy into a stack buffer whenever it
 * fits, avoiding both the malloc/free pair and the snprintf format
 * parsing on every transmission attempt.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @param payload The null-terminated command string to send.
 * @return 0 on successful write, 1 on error.
 */
static int writeToSerial(int fileDescriptor, const char* payload) {
    bool isFileDescriptorInvalid = (fileDescriptor < 0);
    if (isFileDescriptorInvalid == true) {
        printf("Serial port not available!\n");
        return 1;
    }

    // Break down memory allocation requirements
    size_t payloadLength = strlen(payload);
    size_t prefixLength = FRAME_PREFIX_LENGTH;      // Represents "<9,9,"
    size_t suffixLength = FRAME_SUFFIX_LENGTH;      // Represents ">\n"
    size_t nullTerminatorLength = 1;                // Represents '\0'

    size_t totalBufferLength = payloadLength + prefixLength + suffixLength + nullTerminatorLength;

    char stackDataBuffer[TRANSMIT_STACK_BUFFER_SIZE];
    char* heapDataBuffer = NULL;
    char* rawDataBuffer = stackDataBuffer;

    bool isStackBufferTooSmall = (totalBufferLength > sizeof(stackDataBuffer));
    if (isStackBufferTooSmall == true) {
        heapDataBuffer = malloc(totalBufferLength);

        bool isMemoryAllocationFailed = (heapDataBuffer == NULL);
        if (isMemoryAllocationFailed == true) {
            perror("Memory allocation failed for serial data!");
            return 1;
        }

        rawDataBuffer = heapDataBuffer;
    }

    // Assemble "<9,9," + payload + ">\n" without format-string processing
    memcpy(rawDataBuffer, FRAME_PREFIX, prefixLength);
    memcpy(rawDataBuffer + prefixLength, payload, payloadLength);
    memcpy(rawDataBuffer + prefixLength + payloadLength, FRAME_SUFFIX, suffixLength);
    rawDataBuffer[totalBufferLength - nullTerminatorLength] = '\0';

    printf("Serial Data: %s", rawDataBuffer);

    // Subtract the null terminator to only send the actual string content
    size_t bytesToWrite = totalBufferLength - nullTerminatorLength;
    size_t totalBytesWritten = 0;

    while (totalBytesWritten < bytesToWrite) {
        const char* remainingDataPointer = rawDataBuffer + totalBytesWritten;
        size_t remainingByteCount = bytesToWrite - totalBytesWritten;

        ssize_t bytesWritten = write(fileDescriptor, remainingDataPointer, remainingByteCount);

        bool isWriteFailed = (bytesWritten < 0);
        if (isWriteFailed == true) {
            bool isInterrupted = (errno == EINTR);
            bool isWouldBlock = (errno == EAGAIN) || (errno == EWOULDBLOCK);

            if (isInterrupted == true) {
                // Signal interrupted the call before any bytes moved, retry
                continue;
            }

            if (isWouldBlock == true) {
                // Kernel transmit buffer is full, wait until writable
                struct pollfd writePollDescriptor;
                writePollDescriptor.fd = fileDescriptor;
                writePollDescriptor.events = POLLOUT;
                writePollDescriptor.revents = 0;

                int pollResult = poll(&writePollDescriptor, 1, MAX_WAIT_TIME_MS);
                bool isPortWritable = (pollResult > 0);
                if (isPortWritable == false) {
                    printf("Serial port not writable, transmit aborted!\n");
                    free(heapDataBuffer);
                    return 1;
                }
                continue;
            }

            perror("Error writing to serial port!");
            free(heapDataBuffer);
            return 1;
        }

        totalBytesWritten = totalBytesWritten + (size_t)bytesWritten;
    }

    // Free immediately after transmission buffer is consumed to prevent leaks
    // (free(NULL) is a no-op when the stack buffer was used)
    free(heapDataBuffer);

    return 0;
}

/**
 * Polls the serial file descriptor for incoming data until the TARGET_CHAR
 * is received, the maximum overall timeout is exceeded, or the device
 * reports an error/disconnect condition. Each received line is compared
 * against RETRY_TRIGGER_MESSAGE; a match is reported as a timeout so the
 * caller re-transmits the command automatically.
 *
 * Bytes are drained in blocks rather than one character per poll cycle,
 * and the received text is echoed with a single buffered write per block
 * instead of a printf/fflush pair per character.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @return 0 on success, 1 on unrecoverable error, 2 on timeout or retry-trigger.
 */
static int readFromSerial(int fileDescriptor) {
    printf("Waiting for response...\n");

    struct pollfd fileDescriptorArray[1];
    fileDescriptorArray[0].fd = fileDescriptor;
    fileDescriptorArray[0].events = POLLIN;

    // Overall deadline: the timeout no longer resets on every received
    // byte, so a stream of data without a terminator cannot hang forever
    struct timespec readStartTime;
    int clockResult = clock_gettime(CLOCK_MONOTONIC, &readStartTime);
    (void)clockResult;

    // Accumulates the current response line for retry-trigger matching
    char responseLineBuffer[RESPONSE_LINE_BUFFER_SIZE];
    size_t responseLineLength = 0;
    responseLineBuffer[0] = '\0';

    char readChunkBuffer[READ_CHUNK_BUFFER_SIZE];

    while (true) {
        long totalElapsedMilliseconds = elapsedMillisecondsSince(&readStartTime);
        long remainingTimeoutMilliseconds = MAX_WAIT_TIME_MS - totalElapsedMilliseconds;

        bool isDeadlineExceeded = (remainingTimeoutMilliseconds <= 0);
        if (isDeadlineExceeded == true) {
            printf("\nMax response wait time exceeded (%d ms)\n", MAX_WAIT_TIME_MS);
            return 2; // Timeout code
        }

        fileDescriptorArray[0].revents = 0;
        int pollResult = poll(fileDescriptorArray, 1, (int)remainingTimeoutMilliseconds);

        bool isPollSuccessful = (pollResult > 0);
        if (isPollSuccessful == true) {

            // A hangup or error event means the device vanished or faulted;
            // without this check the loop would spin at full CPU forever
            int errorEventMask = POLLERR | POLLHUP | POLLNVAL;
            bool isErrorEvent = ((fileDescriptorArray[0].revents & errorEventMask) != 0);
            if (isErrorEvent == true) {
                printf("\nSerial device disconnected or faulted!\n");
                return 1; // Error
            }

            // Check if the specific event returned indicates data is ready to read
            bool isDataReadyEvent = ((fileDescriptorArray[0].revents & POLLIN) != 0);

            if (isDataReadyEvent == true) {
                size_t bytesRequested = sizeof(readChunkBuffer);
                ssize_t bytesRead = read(fileDescriptor, readChunkBuffer, bytesRequested);

                bool isReadSuccessful = (bytesRead > 0);
                if (isReadSuccessful == true) {

                    size_t receivedByteCount = (size_t)bytesRead;

                    // Locate the line terminator inside this block, if present
                    char* terminatorPointer = memchr(readChunkBuffer, TARGET_CHAR, receivedByteCount);
                    bool isTerminatorFound = (terminatorPointer != NULL);

                    size_t displayableByteCount = receivedByteCount;
                    if (isTerminatorFound == true) {
                        displayableByteCount = (size_t)(terminatorPointer - readChunkBuffer);
                    }

                    // Accumulate the characters for retry-trigger matching
                    // (leave room for the null terminator)
                    size_t availableLineSpace = (RESPONSE_LINE_BUFFER_SIZE - 1) - responseLineLength;
                    size_t copyableByteCount = displayableByteCount;
                    bool isLineSpaceExhausted = (copyableByteCount > availableLineSpace);
                    if (isLineSpaceExhausted == true) {
                        copyableByteCount = availableLineSpace;
                    }

                    if (copyableByteCount > 0) {
                        memcpy(responseLineBuffer + responseLineLength, readChunkBuffer, copyableByteCount);
                        responseLineLength = responseLineLength + copyableByteCount;
                    }

                    // Echo the received text as it arrives, one write per block
                    if (displayableByteCount > 0) {
                        size_t writtenItemCount = fwrite(readChunkBuffer, 1, displayableByteCount, stdout);
                        (void)writtenItemCount;

                        int flushResult = fflush(stdout);
                        (void)flushResult;
                    }

                    if (isTerminatorFound == true) {

                        // Terminate the accumulated line and test the retry trigger
                        responseLineBuffer[responseLineLength] = '\0';
                        char* triggerMatchPointer = strstr(responseLineBuffer, RETRY_TRIGGER_MESSAGE);
                        bool isRetryTriggerReceived = (triggerMatchPointer != NULL);

                        if (isRetryTriggerReceived == true) {
                            printf("\nExternal device wait exceeded, retrying...\n");
                            return 2; // Treat exactly like a timeout so caller retries
                        }

                        printf("\n(OK)\n");
                        return 0; // Success
                    }

                } else {
                    bool isReadFailed = (bytesRead < 0);
                    if (isReadFailed == true) {
                        bool isErrorWouldBlock = (errno == EAGAIN) || (errno == EWOULDBLOCK);
                        bool isCriticalError = (isErrorWouldBlock == false);

                        if (isCriticalError == true) {
                            perror("Failed to read serial port!");
                            return 1; // Error
                        }
                    } else {
                        // read() returned 0: end-of-file, device is gone
                        printf("\nSerial device closed the connection!\n");
                        return 1; // Error
                    }
                }
            }

        } else {
            bool isPollTimeout = (pollResult == 0);

            if (isPollTimeout == true) {
                printf("\nMax response wait time exceeded (%d ms)\n", MAX_WAIT_TIME_MS);
                return 2; // Timeout code
            } else {
                bool isPollInterrupted = (errno == EINTR);
                if (isPollInterrupted == true) {
                    // Signal interrupted the wait, loop and recompute remaining time
                    continue;
                }
                perror("Serial polling failed!");
                return 1; // Error code
            }
        }
    }
}

/**
 * Manages the high-level transmission workflow. Attempts to write and read
 * from the serial device, automatically re-transmitting if a timeout occurs
 * or if the hub reports the external-device wait-exceeded message.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @param payload The command string to send.
 * @param maximumAttempts The maximum number of transmission attempts allowed.
 * @return 0 on final success, non-zero on failure/timeout.
 */
static int transmitCommandWithRetries(int fileDescriptor, const char* payload, int maximumAttempts) {
    int attemptCounter = 0;

    while (true) {
        bool isMaxAttemptsReached = (attemptCounter >= maximumAttempts);
        if (isMaxAttemptsReached == true) {
            printf("Failed after %d attempts.\n", maximumAttempts);
            return 1; // Failure
        }

        attemptCounter = attemptCounter + 1;

        bool isRetryAttempt = (attemptCounter > 1);
        if (isRetryAttempt == true) {
            printf("--- Retrying transmission (Attempt %d of %d) ---\n", attemptCounter, maximumAttempts);
        }

        // Flush any pending/corrupt data before writing to guarantee a clean slate
        int flushQueueType = TCIOFLUSH;
        int flushResult = tcflush(fileDescriptor, flushQueueType);
        bool isFlushFailed = (flushResult == -1);

        if (isFlushFailed == true) {
            perror("Error flushing serial port buffer");
            return 1;
        }

        int writeStatus = writeToSerial(fileDescriptor, payload);
        bool isWriteSuccessful = (writeStatus == 0);

        if (isWriteSuccessful == true) {
            int readStatus = readFromSerial(fileDescriptor);

            bool isReadSuccessful = (readStatus == 0);
            if (isReadSuccessful == true) {
                return 0; // Complete Success
            }

            bool isReadTimeout = (readStatus == 2);
            if (isReadTimeout == true) {
                 // Continue loop to retry (timeout or retry-trigger message)
                 continue;
            } else {
                 // Fatal read error
                 return 1;
            }

        } else {
             // Fatal write error
             return 1;
        }
    }
}

// --- Main Entry Point ---
int main(int argumentCount, char *argumentVector[]) {

    // 1. Initialize Application Context
    SerialContext applicationContext;
    applicationContext.fileDescriptor = -1;
    applicationContext.isPortLocked = false;
    applicationContext.allocatedArgumentData = NULL;

    // 2. Argument Validation
    // Argument 1 selects the serial device, argument 2 carries the command
    int requiredArgumentCount = 3;
    bool isArgumentCountValid = (argumentCount == requiredArgumentCount);

    if (isArgumentCountValid == false) {
        printf("Z-Terminal Xmit v11\n");
        printf("Usage: %s <device> <message>\n", argumentVector[0]);
        printf("Example: %s /dev/zterm-tty \"<command>\"\n", argumentVector[0]);
        return 1;
    }

    const char* serialDevicePath = argumentVector[1];
    bool isDevicePathEmpty = (serialDevicePath[0] == '\0');

    if (isDevicePathEmpty == true) {
        printf("Serial port path is empty!\n");
        return 1;
    }

    char* duplicatedArgumentString = strdup(argumentVector[2]);
    bool isAllocationSuccessful = (duplicatedArgumentString != NULL);

    if (isAllocationSuccessful == false) {
        perror("Failed to allocate memory for argument");
        return 1;
    }

    applicationContext.allocatedArgumentData = duplicatedArgumentString;

    // Strip accidental carriage returns or newlines from input
    const char* illegalCharacters = "\r\n";
    size_t firstIllegalCharacterIndex = strcspn(applicationContext.allocatedArgumentData, illegalCharacters);
    applicationContext.allocatedArgumentData[firstIllegalCharacterIndex] = 0;

    // 3. Open Port Connection
    int fileOpenFlags = O_RDWR | O_NOCTTY | O_NONBLOCK;
    applicationContext.fileDescriptor = open(serialDevicePath, fileOpenFlags);

    bool isOpenSuccessful = (applicationContext.fileDescriptor >= 0);
    if (isOpenSuccessful == false) {
        perror("Error opening serial port");
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    // 4. Apply Exclusive File Lock
    int fileLockOperation = LOCK_EX | LOCK_NB;
    int lockResult = flock(applicationContext.fileDescriptor, fileLockOperation);
    bool isLockSuccessful = (lockResult != -1);

    if (isLockSuccessful == false) {
        bool isErrorWouldBlock = (errno == EWOULDBLOCK);
        if (isErrorWouldBlock == true) {
            printf("Device busy\n");
        } else {
            perror("Error locking serial port");
        }
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    applicationContext.isPortLocked = true;
    int clockResult = clock_gettime(CLOCK_MONOTONIC, &applicationContext.lockAcquiredTime);
    (void)clockResult;

    // 5. Configure Terminal Hardware Attributes
    struct termios terminalSettings;
    int getAttributesResult = tcgetattr(applicationContext.fileDescriptor, &terminalSettings);
    bool isGetAttributesSuccessful = (getAttributesResult == 0);

    if (isGetAttributesSuccessful == false) {
        perror("Error reading serial port attributes");
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    int setOutputSpeedResult = cfsetospeed(&terminalSettings, B9600);
    (void)setOutputSpeedResult;

    int setInputSpeedResult = cfsetispeed(&terminalSettings, B9600);
    (void)setInputSpeedResult;

    // Bitwise configurations explicitly broken out
    terminalSettings.c_cflag = terminalSettings.c_cflag & (~PARENB);   // disable parity bit
    terminalSettings.c_cflag = terminalSettings.c_cflag & (~CSTOPB);   // set one stop bit
    terminalSettings.c_cflag = terminalSettings.c_cflag & (~CSIZE);    // clear data size bits
    terminalSettings.c_cflag = terminalSettings.c_cflag | CS8;         // set 8 data bits
    terminalSettings.c_cflag = terminalSettings.c_cflag & (~CRTSCTS);  // disable hardware flow control
    terminalSettings.c_cflag = terminalSettings.c_cflag & (~HUPCL);    // disable DST & RST
    terminalSettings.c_cflag = terminalSettings.c_cflag | CREAD;       // turn on READ
    terminalSettings.c_cflag = terminalSettings.c_cflag | CLOCAL;      // ignore ctrl lines

    // Set to raw mode (disable echo, etc.)
    int rawModeFlags = ICANON | ECHO | ECHOE | ISIG;
    terminalSettings.c_lflag = terminalSettings.c_lflag & (~rawModeFlags);

    // Disable input translation so the kernel cannot rewrite received
    // bytes: no CR<->NL mapping (would fake the '\n' terminator) and
    // no XON/XOFF software flow control (would silently eat 0x11/0x13)
    int inputTranslationFlags = ICRNL | INLCR | IGNCR | IXON | IXOFF | IXANY;
    terminalSettings.c_iflag = terminalSettings.c_iflag & (~inputTranslationFlags);

    // Disable output post-processing so the transmitted '\n' is never
    // expanded to "\r\n" by the tty layer
    terminalSettings.c_oflag = terminalSettings.c_oflag & (~OPOST);

    int hardwareActionFlag = TCSANOW;
    int setAttributesResult = tcsetattr(applicationContext.fileDescriptor, hardwareActionFlag, &terminalSettings);
    bool isSetAttributesSuccessful = (setAttributesResult == 0);

    if (isSetAttributesSuccessful == false) {
        perror("Error applying serial port settings");
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    // 6. Execute Main Program Flow
    int executionStatus = transmitCommandWithRetries(applicationContext.fileDescriptor, applicationContext.allocatedArgumentData, MAX_TRANSMIT_ATTEMPTS);

    // 7. Cleanup and Exit
    cleanupAndClosePort(&applicationContext);

    return executionStatus;
}