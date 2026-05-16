#include "httpapiserver.h"
#include <iostream>
#include <algorithm>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <cmath>
#include <poll.h>

class SocketDescriptorGuard {
public:
    explicit SocketDescriptorGuard(int socketDescriptor) : managedSocketDescriptor(socketDescriptor) {}
    ~SocketDescriptorGuard() {
        if (managedSocketDescriptor >= 0) close(managedSocketDescriptor);
    }
    int getDescriptor() const { return managedSocketDescriptor; }
private:
    int managedSocketDescriptor;
};

HttpApiServer::HttpApiServer(int targetPort, int rateLimitMilliseconds) 
    : serverPort(targetPort), 
      rateLimitDuration(rateLimitMilliseconds), 
      isServerRunning(false), 
      lastRequestTime(std::chrono::steady_clock::now()) {}

HttpApiServer::~HttpApiServer() {
    stopServer();
}

void HttpApiServer::setCallbacks(GetBrightnessFunction getCallback, SetBrightnessFunction setCallback) {
    onGetBrightness = std::move(getCallback);
    onSetBrightness = std::move(setCallback);
}

void HttpApiServer::startServer() {
    if (!isServerRunning.exchange(true)) {
        listenerThread = std::thread(&HttpApiServer::listenForConnections, this);
    }
}

void HttpApiServer::stopServer() {
    if (isServerRunning.exchange(false) && listenerThread.joinable()) {
        listenerThread.join();
    }
}

void HttpApiServer::listenForConnections() {
    int serverSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocketDescriptor < 0) return;
    SocketDescriptorGuard serverSocketGuard(serverSocketDescriptor);

    int socketOptions = 1;
    setsockopt(serverSocketDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketOptions, sizeof(socketOptions));

    struct sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(serverPort);

    if (bind(serverSocketDescriptor, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
        std::cerr << "Fatal: Could not bind HTTP API port " << serverPort << std::endl;
        return;
    }

    listen(serverSocketDescriptor, 5);

    struct pollfd pfd{};
    pfd.fd = serverSocketDescriptor;
    pfd.events = POLLIN;

    while (isServerRunning) {
        // Poll with a 500ms timeout to allow clean shutdown checks
        if (poll(&pfd, 1, 500) > 0) {
            int clientSocketDescriptor = accept(serverSocketDescriptor, nullptr, nullptr);
            if (clientSocketDescriptor >= 0) {
                std::thread(&HttpApiServer::processClientRequest, this, clientSocketDescriptor).detach();
            }
        }
    }
}

void HttpApiServer::processClientRequest(int clientSocketDescriptor) {
    SocketDescriptorGuard clientSocketGuard(clientSocketDescriptor);
    int clientFd = clientSocketGuard.getDescriptor();

    // Setup receive timeout to prevent hung threads
    struct timeval timeout{2, 0}; // 2 seconds
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    // Rate Limiting
    {
        std::lock_guard<std::mutex> lockGuard(rateLimitMutex);
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastRequestTime) < rateLimitDuration) {
            std::string busyResponse = "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\nBUSY";
            send(clientFd, busyResponse.c_str(), busyResponse.length(), MSG_NOSIGNAL);
            return;
        }
        lastRequestTime = currentTime;
    }

    char requestBuffer[1024] = {0};
    ssize_t bytesRead = read(clientFd, requestBuffer, sizeof(requestBuffer) - 1);
    if (bytesRead <= 0) return;

    std::string requestString(requestBuffer, bytesRead);
    std::string responseString;

    if (requestString.find("GET /brightness") != std::string::npos) {
        int currentBrightness = onGetBrightness ? onGetBrightness() : 0;
        std::string bodyString = std::to_string(currentBrightness);
        responseString = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + 
                         std::to_string(bodyString.length()) + "\r\n\r\n" + bodyString;
    } 
    else if (size_t setPosition = requestString.find("b="); setPosition != std::string::npos) {
        try {
            int targetBrightness = std::stoi(requestString.substr(setPosition + 2));
            targetBrightness = std::max(0, std::min(targetBrightness, 255));
            
            if (onSetBrightness) {
                onSetBrightness(targetBrightness);
            }
            responseString = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n\r\nOK\n";
        } catch (...) {
            responseString = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 14\r\n\r\nInvalid Value\n";
        }
    } 
    else {
        responseString = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nNot Found\n";
    }

    send(clientFd, responseString.c_str(), responseString.length(), MSG_NOSIGNAL);
}
