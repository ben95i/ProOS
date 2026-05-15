#include "httpapiserver.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <string.h>
#include <cmath>

/**
 * @class SocketDescriptorGuard
 * @brief RAII wrapper to ensure socket file descriptors are safely closed to prevent memory/resource leaks.
 */
class SocketDescriptorGuard {
public:
    explicit SocketDescriptorGuard(int socketDescriptor) {
        managedSocketDescriptor = socketDescriptor;
    }

    ~SocketDescriptorGuard() {
        bool isDescriptorValid = false;
        if (managedSocketDescriptor >= 0) {
            isDescriptorValid = true;
        }

        if (isDescriptorValid == true) {
            close(managedSocketDescriptor);
        }
    }

    int getDescriptor() const {
        return managedSocketDescriptor;
    }

private:
    int managedSocketDescriptor;
};

HttpApiServer::HttpApiServer(int targetPort, int rateLimitMilliseconds) {
    serverPort = targetPort;
    
    std::chrono::milliseconds durationToSet(rateLimitMilliseconds);
    rateLimitDuration = durationToSet;
    
    bool initialRunningState = false;
    isServerRunning.store(initialRunningState);
    
    std::chrono::time_point<std::chrono::steady_clock> epochTime;
    lastRequestTime = epochTime;
}

HttpApiServer::~HttpApiServer() {
    stopServer();
}

void HttpApiServer::setCallbacks(GetBrightnessFunction getCallback, SetBrightnessFunction setCallback) {
    onGetBrightness = getCallback;
    onSetBrightness = setCallback;
}

void HttpApiServer::startServer() {
    bool currentRunningState = true;
    isServerRunning.store(currentRunningState);
    
    std::thread backgroundThread(&HttpApiServer::listenForConnections, this);
    listenerThread = std::move(backgroundThread);
}

void HttpApiServer::stopServer() {
    bool stoppedState = false;
    isServerRunning.store(stoppedState);
    
    bool isThreadJoinable = listenerThread.joinable();
    if (isThreadJoinable == true) {
        listenerThread.join();
    }
}

void HttpApiServer::listenForConnections() {
    int serverSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    SocketDescriptorGuard serverSocketGuard(serverSocketDescriptor);

    int socketOptions = 1;
    setsockopt(serverSocketDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketOptions, sizeof(socketOptions));

    struct sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(serverPort);

    int bindResult = bind(serverSocketDescriptor, (struct sockaddr*)&serverAddress, sizeof(serverAddress));
    if (bindResult < 0) {
        std::cerr << "Fatal: Could not bind HTTP API port." << std::endl;
        return;
    }

    listen(serverSocketDescriptor, 5);

    while (isServerRunning.load() == true) {
        int clientSocketDescriptor = accept(serverSocketDescriptor, nullptr, nullptr);
        
        bool isClientValid = false;
        if (clientSocketDescriptor >= 0) {
            isClientValid = true;
        }

        if (isClientValid == true) {
            // Spawn a detached thread to prevent blocking the listener loop (fixes race condition/hanging)
            std::thread clientThread(&HttpApiServer::processClientRequest, this, clientSocketDescriptor);
            clientThread.detach();
        }
    }
}

void HttpApiServer::processClientRequest(int clientSocketDescriptor) {
    SocketDescriptorGuard clientSocketGuard(clientSocketDescriptor);

    // Apply Rate Limiting
    {
        std::lock_guard<std::mutex> lockGuard(rateLimitMutex);
        
        std::chrono::time_point<std::chrono::steady_clock> currentTime = std::chrono::steady_clock::now();
        std::chrono::milliseconds timeSinceLastRequest = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastRequestTime);
        
        bool isRateLimited = false;
        if (timeSinceLastRequest < rateLimitDuration) {
            isRateLimited = true;
        }

        if (isRateLimited == true) {
            std::string busyResponse = "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/plain\r\nContent-Length: 4\r\n\r\nBUSY";
            send(clientSocketGuard.getDescriptor(), busyResponse.c_str(), busyResponse.length(), 0);
            return;
        }
        
        lastRequestTime = currentTime;
    }

    char requestBuffer[1024];
    memset(requestBuffer, 0, sizeof(requestBuffer));
    
    ssize_t bytesRead = read(clientSocketGuard.getDescriptor(), requestBuffer, sizeof(requestBuffer) - 1);
    
    bool isReadSuccessful = false;
    if (bytesRead > 0) {
        isReadSuccessful = true;
    }

    if (isReadSuccessful == false) {
        return;
    }

    std::string requestString(requestBuffer);
    std::string responseString;

    size_t getPosition = requestString.find("GET /brightness");
    size_t setPosition = requestString.find("b=");

    bool isGetRequest = false;
    if (getPosition != std::string::npos) {
        isGetRequest = true;
    }

    bool isSetRequest = false;
    if (setPosition != std::string::npos) {
        isSetRequest = true;
    }

    if (isGetRequest == true) {
        int currentBrightness = 0;
        if (onGetBrightness != nullptr) {
            currentBrightness = onGetBrightness();
        }
        
        std::string bodyString = std::to_string(currentBrightness);
        size_t bodyLength = bodyString.length();
        std::string lengthString = std::to_string(bodyLength);
        
        responseString = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + lengthString + "\r\n\r\n" + bodyString;
    } 
    else if (isSetRequest == true) {
        try {
            size_t valueStartIndex = setPosition + 2;
            std::string valueString = requestString.substr(valueStartIndex);
            float rawValueFloat = std::stof(valueString);
            
            float roundedValue = std::round(rawValueFloat);
            int finalIntegerBrightness = static_cast<int>(roundedValue);
            
            if (onSetBrightness != nullptr) {
                onSetBrightness(finalIntegerBrightness);
            }
            
            responseString = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 3\r\n\r\nOK\n";
        } catch (...) {
            responseString = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain\r\nContent-Length: 14\r\n\r\nInvalid Value\n";
        }
    } 
    else {
        responseString = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nNot Found\n";
    }

    send(clientSocketGuard.getDescriptor(), responseString.c_str(), responseString.length(), 0);
}