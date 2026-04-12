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

int serial_port = -1; // Initialize to -1 to prevent closing stdin
const char targetChar = '\n';
const char device[] = "/dev/zterm-tty";
const int maxWaitTimeMs = 1000; // Set maximum duration in milliseconds
const int minLockTimeMs = 30; // execution delay in milliseconds
bool port_locked = false;
struct timespec lock_time;

// cleanup routine with 100ms delay
void exitRoutine(char *argData) {
    if (argData != NULL) {
        free(argData);
    }
    
    if (serial_port >= 0) {
        if (port_locked) {
            // Check how much time has passed since we locked the port
            struct timespec current_time;
            clock_gettime(CLOCK_MONOTONIC, &current_time);
            
            // Calculate elapsed time in milliseconds
            long elapsed_ms = (current_time.tv_sec - lock_time.tv_sec) * 1000 + 
                              (current_time.tv_nsec - lock_time.tv_nsec) / 1000000;
            
            // If less than xx(ms) has passed, sleep for the remainder
            if (elapsed_ms < minLockTimeMs) {
                long remaining_ms = minLockTimeMs - elapsed_ms;
                usleep(remaining_ms * 1000); 
            }
            
            // Explicitly unlock the port
            flock(serial_port, LOCK_UN);
        }
        
        close(serial_port);
        serial_port = -1;
    }
}
int serialWrite(const char *payload) {
    if (serial_port < 0) {
        printf("Serial port not available!\n");
        return 1;
    }

    // calculate memory: "<9,9," (5) + payload length + ">\n" (2) + '\0' (1) = 8 bytes extra
    size_t payloadLen = strlen(payload);
    size_t totalLen = payloadLen + 8;
    char *rawData = malloc(totalLen);
    
    if (rawData == NULL) {
        perror("Memory allocation failed for serial data!");
        return 1;
    }

    // snprintf safely constructs the string without buffer overflow risks
    snprintf(rawData, totalLen, "<9,9,%s>\n", payload);
    printf("Serial Data: %s", rawData);

    // Write to the serial port (excluding the null terminator)
    ssize_t bytesWritten = write(serial_port, rawData, totalLen - 1);
    free(rawData);

    if (bytesWritten < 0) {
        perror("Error writing to serial port!");
        return 1;
    }
    return 0;
}

int serialRead() {
    printf("Waiting for response...\n");

    // Set up the pollfd structure to monitor the serial port
    struct pollfd fds[1];
    fds[0].fd = serial_port;
    fds[0].events = POLLIN; // Wait for data to be ready to read

    char readBuf;
    
    while (1) {
        // Wait for data or timeout
        int ret = poll(fds, 1, maxWaitTimeMs);

        if (ret > 0) {
            // Data is ready to read
            if (fds[0].revents & POLLIN) {
                ssize_t num_bytes = read(serial_port, &readBuf, 1);
                
                if (num_bytes > 0) {
                    if (readBuf == targetChar) {
                        printf("\n(OK)\n");
                        return 0; // Success
                    } else {
                        printf("%c", readBuf);
                        fflush(stdout); // Force character to console immediately
                    }
                } else if (num_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("Failed to read serial port!");
                    return 1;
                }
            }
        } else if (ret == 0) {
            // Timeout reached
            printf("\nMax response wait time exceeded (%d ms)\n", maxWaitTimeMs);
            return 1;
        } else {
            // Poll error
            perror("Serial polling failed!");
            return 1;
        }
    }
}

// main entry point //
int main(int argc, char *argv[]) {

    // Argument validation
    if (argc != 2) {
        printf("Z-Terminal Xmit v8\n");
        printf("Usage: %s <message>\n", argv[0]);
        return 1; 
    }

    // Read argument safely (strdup allocates and copies in one step)
    char *argData = strdup(argv[1]);
    if (argData == NULL) {
        perror("Failed to allocate memory for argument");
        return 1;
    }

    // Strip out any accidental carriage returns/newlines from user input
    argData[strcspn(argData, "\r\n")] = 0;

    // Serial initialization mode
    if (strcmp(argData, "i") == 0) {
        printf("Serial port initialized\n");
        exitRoutine(argData);
        return 0;
    }

    // Open the serial port (Added O_NOCTTY to prevent it from becoming the controlling terminal)
    serial_port = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_port < 0) {
        perror("Error opening serial port");
        exitRoutine(argData);
        return 1;
    }

    // Apply exclusive lock to the port ---
    if (flock(serial_port, LOCK_EX | LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            printf("Device busy\n");
        } else {
            perror("Error locking serial port");
        }
        exitRoutine(argData);
        return 1;
    }
    
    // Mark as locked and record the exact timestamp
    port_locked = true;
    clock_gettime(CLOCK_MONOTONIC, &lock_time);

    // Configure the serial port
    struct termios tty;
    if (tcgetattr(serial_port, &tty) != 0) {
        perror("Error reading serial port attributes");
        exitRoutine(argData);
        return 1;
    }
    cfsetospeed(&tty, B9600); // baud rate out
    cfsetispeed(&tty, B9600); // baud rate in
    tty.c_cflag &= ~PARENB;   // disable parity bit
    tty.c_cflag &= ~CSTOPB;   // set one stop bit
    tty.c_cflag &= ~CSIZE;    // clear data size bits
    tty.c_cflag |= CS8;       // set 8 data bits
    tty.c_cflag &= ~CRTSCTS;  // disable hardware flow control
    tty.c_cflag &= ~HUPCL;    // disable DST & RST (prevent system reset on UNO)
    tty.c_cflag |= CREAD | CLOCAL; // turn on READ & ignore ctrl lines
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Set to raw mode (disable echo, etc.)

    if (tcsetattr(serial_port, TCSANOW, &tty) != 0) {
        perror("Error applying serial port settings");
        exitRoutine(argData);
        return 1;
    }

    usleep(25000);

    // Flush old data before starting
    if (tcflush(serial_port, TCIOFLUSH) == -1) {
        perror("Error flushing serial port buffer");
        exitRoutine(argData);
        return 1;
    }

    // Execute flow
    int status = serialWrite(argData);
    if (status == 0) {
        status = serialRead();
    }

    // Cleanup
    exitRoutine(argData);
    return status;
}