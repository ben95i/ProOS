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
/////////////////////

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
const char TARGET_CHAR = '\n';
const char DEVICE_PATH[] = "/dev/zterm-tty";
const int MAX_WAIT_TIME_MS = 500; 
const int MIN_LOCK_TIME_MS = 15;   
const int MAX_TRANSMIT_ATTEMPTS = 3; 

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
void cleanupAndClosePort(SerialContext* context);
int writeToSerial(int fileDescriptor, const char* payload);
int readFromSerial(int fileDescriptor);
int transmitCommandWithRetries(int fileDescriptor, const char* payload, int maximumAttempts);


/**
 * Safely releases file locks, manages the minimum execution delay, 
 * closes the file descriptor, and frees allocated memory.
 * * @param context Pointer to the application's serial state context.
 */
void cleanupAndClosePort(SerialContext* context) {
    bool isArgumentDataAllocated = (context->allocatedArgumentData != NULL);
    if (isArgumentDataAllocated == true) {
        free(context->allocatedArgumentData);
        context->allocatedArgumentData = NULL;
    }
    
    bool isFileDescriptorValid = (context->fileDescriptor >= 0);
    if (isFileDescriptorValid == true) {
        
        bool isPortCurrentlyLocked = (context->isPortLocked == true);
        if (isPortCurrentlyLocked == true) {
            
            struct timespec currentTime;
            int clockResult = clock_gettime(CLOCK_MONOTONIC, &currentTime);
            (void)clockResult; // explicitly cast to void to silence unused warning
            
            // Break down the elapsed time calculation for clarity
            time_t secondsDifference = currentTime.tv_sec - context->lockAcquiredTime.tv_sec;
            long millisecondsFromSeconds = secondsDifference * 1000;
            
            long nanosecondsDifference = currentTime.tv_nsec - context->lockAcquiredTime.tv_nsec;
            long millisecondsFromNanoseconds = nanosecondsDifference / 1000000;
            
            long totalElapsedMilliseconds = millisecondsFromSeconds + millisecondsFromNanoseconds;
            
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
 * it over the provided serial file descriptor.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @param payload The null-terminated command string to send.
 * @return 0 on successful write, 1 on error.
 */
int writeToSerial(int fileDescriptor, const char* payload) {
    bool isFileDescriptorInvalid = (fileDescriptor < 0);
    if (isFileDescriptorInvalid == true) {
        printf("Serial port not available!\n");
        return 1;
    }

    // Break down memory allocation requirements
    size_t payloadLength = strlen(payload);
    size_t prefixLength = 5;            // Represents "<9,9,"
    size_t suffixLength = 2;            // Represents ">\n"
    size_t nullTerminatorLength = 1;    // Represents '\0'
    
    size_t totalBufferLength = payloadLength + prefixLength + suffixLength + nullTerminatorLength;
    
    char* rawDataBuffer = malloc(totalBufferLength);
    
    bool isMemoryAllocationFailed = (rawDataBuffer == NULL);
    if (isMemoryAllocationFailed == true) {
        perror("Memory allocation failed for serial data!");
        return 1;
    }

    int stringFormatResult = snprintf(rawDataBuffer, totalBufferLength, "<9,9,%s>\n", payload);
    bool isStringFormatFailed = (stringFormatResult < 0);
    if (isStringFormatFailed == true) {
         perror("String formatting failed!");
         free(rawDataBuffer);
         return 1;
    }

    printf("Serial Data: %s", rawDataBuffer);

    // Subtract the null terminator to only send the actual string content
    size_t bytesToWrite = totalBufferLength - nullTerminatorLength;
    ssize_t bytesWritten = write(fileDescriptor, rawDataBuffer, bytesToWrite);
    
    // Free immediately after transmission buffer is consumed to prevent leaks
    free(rawDataBuffer);

    bool isWriteFailed = (bytesWritten < 0);
    if (isWriteFailed == true) {
        perror("Error writing to serial port!");
        return 1;
    }
    
    return 0;
}

/**
 * Polls the serial file descriptor for incoming data until the TARGET_CHAR 
 * is received or the maximum timeout is exceeded.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @return 0 on success, 1 on unrecoverable error, 2 on timeout.
 */
int readFromSerial(int fileDescriptor) {
    printf("Waiting for response...\n");

    struct pollfd fileDescriptorArray[1];
    fileDescriptorArray[0].fd = fileDescriptor;
    fileDescriptorArray[0].events = POLLIN; 

    char readBufferCharacter;
    
    while (true) {
        int pollResult = poll(fileDescriptorArray, 1, MAX_WAIT_TIME_MS);

        bool isPollSuccessful = (pollResult > 0);
        if (isPollSuccessful == true) {
            
            // Check if the specific event returned indicates data is ready to read
            bool isDataReadyEvent = ((fileDescriptorArray[0].revents & POLLIN) != 0);
            
            if (isDataReadyEvent == true) {
                size_t bytesRequested = 1;
                ssize_t bytesRead = read(fileDescriptor, &readBufferCharacter, bytesRequested);
                
                bool isReadSuccessful = (bytesRead > 0);
                if (isReadSuccessful == true) {
                    
                    bool isCharacterTarget = (readBufferCharacter == TARGET_CHAR);
                    if (isCharacterTarget == true) {
                        printf("\n(OK)\n");
                        return 0; // Success
                    } else {
                        printf("%c", readBufferCharacter);
                        int flushResult = fflush(stdout); 
                        (void)flushResult; 
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
                    }
                }
            }
            
        } else {
            bool isPollTimeout = (pollResult == 0);
            
            if (isPollTimeout == true) {
                printf("\nMax response wait time exceeded (%d ms)\n", MAX_WAIT_TIME_MS);
                return 2; // Timeout code
            } else {
                perror("Serial polling failed!");
                return 1; // Error code
            }
        }
    }
}

/**
 * Manages the high-level transmission workflow. Attempts to write and read 
 * from the serial device, automatically re-transmitting if a timeout occurs.
 * * @param fileDescriptor The active file descriptor for the serial port.
 * @param payload The command string to send.
 * @param maximumAttempts The maximum number of transmission attempts allowed.
 * @return 0 on final success, non-zero on failure/timeout.
 */
int transmitCommandWithRetries(int fileDescriptor, const char* payload, int maximumAttempts) {
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
                 // Continue loop to retry
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
    int requiredArgumentCount = 2;
    bool isArgumentCountValid = (argumentCount == requiredArgumentCount);
    
    if (isArgumentCountValid == false) {
        printf("Z-Terminal Xmit v9\n");
        printf("Usage: %s <message>\n", argumentVector[0]);
        return 1; 
    }

    char* duplicatedArgumentString = strdup(argumentVector[1]);
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

    // 3. Mode Selection Validation
    const char* initializationFlag = "i";
    int stringComparisonResult = strcmp(applicationContext.allocatedArgumentData, initializationFlag);
    bool isInitializationModeRequested = (stringComparisonResult == 0);
    
    if (isInitializationModeRequested == true) {
        printf("Serial port initialized\n");
        cleanupAndClosePort(&applicationContext);
        return 0;
    }

    // 4. Open Port Connection
    int fileOpenFlags = O_RDWR | O_NOCTTY | O_NONBLOCK;
    applicationContext.fileDescriptor = open(DEVICE_PATH, fileOpenFlags);
    
    bool isOpenSuccessful = (applicationContext.fileDescriptor >= 0);
    if (isOpenSuccessful == false) {
        perror("Error opening serial port");
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    // 5. Apply Exclusive File Lock
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

    // 6. Configure Terminal Hardware Attributes
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

    int hardwareActionFlag = TCSANOW;
    int setAttributesResult = tcsetattr(applicationContext.fileDescriptor, hardwareActionFlag, &terminalSettings);
    bool isSetAttributesSuccessful = (setAttributesResult == 0);
    
    if (isSetAttributesSuccessful == false) {
        perror("Error applying serial port settings");
        cleanupAndClosePort(&applicationContext);
        return 1;
    }

    // 7. Execute Main Program Flow
    int executionStatus = transmitCommandWithRetries(applicationContext.fileDescriptor, applicationContext.allocatedArgumentData, MAX_TRANSMIT_ATTEMPTS);

    // 8. Cleanup and Exit
    cleanupAndClosePort(&applicationContext);
    
    return executionStatus;
}