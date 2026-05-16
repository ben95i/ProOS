#include "opcclient.h"
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

OPCClient::OPCClient(const std::string& hostAddress, int portNumber)
    : serverAddress(hostAddress), serverPort(portNumber), activeSocketDescriptor(-1) {
    // connectToServer() removed from constructor.
    // Connection is now deferred until the first frame of video playback.
}

OPCClient::~OPCClient() {
    disconnectFromServer();
}

void OPCClient::connectToServer() {
    if (activeSocketDescriptor != -1) return;

    activeSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (activeSocketDescriptor < 0) return;

    int disableNagleAlgorithm = 1;
    setsockopt(activeSocketDescriptor, IPPROTO_TCP, TCP_NODELAY, &disableNagleAlgorithm, sizeof(disableNagleAlgorithm));

    sockaddr_in serverSocketAddress;
    memset(&serverSocketAddress, 0, sizeof(serverSocketAddress));
    serverSocketAddress.sin_family = AF_INET;
    serverSocketAddress.sin_port = htons(serverPort);
    
    if (inet_pton(AF_INET, serverAddress.c_str(), &serverSocketAddress.sin_addr) <= 0) {
        disconnectFromServer();
        return;
    }

    if (connect(activeSocketDescriptor, (struct sockaddr*)&serverSocketAddress, sizeof(serverSocketAddress)) < 0) {
        disconnectFromServer();
    }
}

void OPCClient::disconnectFromServer() {
    if (activeSocketDescriptor != -1) {
        close(activeSocketDescriptor);
        activeSocketDescriptor = -1;
    }
}

bool OPCClient::sendPixelPayload(uint8_t channelIndex, const uint8_t* payloadDataPointer, size_t payloadByteSize) {
    // Lazy-connection natively handles opening the socket when the video starts
    if (activeSocketDescriptor == -1) {
        connectToServer();
        if (activeSocketDescriptor == -1) return false;
    }

    networkPacketBuffer.clear();
    networkPacketBuffer.reserve(4 + payloadByteSize);
    
    // OPC Header: [Channel] [Command (0 = 8-bit RGB)] [Length High Byte] [Length Low Byte]
    networkPacketBuffer.push_back(channelIndex);
    networkPacketBuffer.push_back(0); 
    networkPacketBuffer.push_back(static_cast<uint8_t>((payloadByteSize >> 8) & 0xFF));
    networkPacketBuffer.push_back(static_cast<uint8_t>(payloadByteSize & 0xFF));
    
    // Append pixel payload
    networkPacketBuffer.insert(networkPacketBuffer.end(), payloadDataPointer, payloadDataPointer + payloadByteSize);

    // MSG_NOSIGNAL prevents the application from crashing via SIGPIPE if the server disconnects unexpectedly
    ssize_t bytesTransmitted = send(activeSocketDescriptor, networkPacketBuffer.data(), networkPacketBuffer.size(), MSG_NOSIGNAL);
    if (bytesTransmitted < 0) {
        disconnectFromServer();
        return false;
    }
    return true;
}