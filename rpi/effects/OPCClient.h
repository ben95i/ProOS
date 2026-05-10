#ifndef OPCCLIENT_H
#define OPCCLIENT_H

#include <string>
#include <vector>
#include <cstdint>

class OPCClient {
private:
    std::string serverAddress;
    int serverPort;
    int activeSocketDescriptor;
    std::vector<uint8_t> networkPacketBuffer;

    void connectToServer();
    void disconnectFromServer();

public:
    OPCClient(const std::string& hostAddress = "127.0.0.1", int portNumber = 7890);
    ~OPCClient();

    // Transmits a raw RGB payload [red, green, blue, red, green, blue...] 
    // automatically wrapping it in the 4-byte OPC protocol header.
    bool sendPixelPayload(uint8_t channelIndex, const uint8_t* payloadDataPointer, size_t payloadByteSize);
};

#endif