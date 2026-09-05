#ifndef OPCCLIENT_H
#define OPCCLIENT_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

// Open Pixel Control client.
//
// The socket is non-blocking: if the server stops draining, frames are dropped
// rather than stalling the caller. For a realtime LED feed a dropped frame is
// always better than a frozen render loop, and a late frame is worthless anyway.
class OPCClient {
public:
    OPCClient(const std::string& hostAddress = "127.0.0.1", int portNumber = 7890);
    ~OPCClient();

    // Owns a file descriptor, so copying would double-close it.
    OPCClient(const OPCClient&) = delete;
    OPCClient& operator=(const OPCClient&) = delete;

    // Transmits a raw RGB payload [red, green, blue, red, green, blue...]
    // wrapped in the 4-byte OPC protocol header.
    //
    // Returns false when the frame did not go out: no connection, the server is
    // backed up, or the payload exceeds what the protocol can describe. The
    // caller is expected to carry on and try again next frame.
    bool sendPixelPayload(uint8_t channelIndex, const uint8_t* payloadDataPointer, size_t payloadByteSize);

    void disconnectFromServer();

    bool isConnected() const { return activeSocketDescriptor != -1; }

    // Frames discarded because the server was not keeping up. A steadily rising
    // count means the receiver, not this program, is the bottleneck.
    unsigned long getDroppedFrameCount() const { return droppedFrameCount; }

    // The largest payload the 16-bit OPC length field can describe.
    static constexpr size_t MAX_PAYLOAD_BYTES = 65535;

private:
    bool ensureConnected();
    void connectToServer();
    bool flushPendingBytes();
    void queueUnsentTail(const uint8_t* headerBytes, size_t headerSize, const uint8_t* payloadDataPointer,
                         size_t payloadByteSize, size_t bytesAlreadySent);

    std::string serverAddress;
    int serverPort;
    int activeSocketDescriptor;

    // Remainder of a packet that only partially made it into the socket. It must
    // be finished before the next frame starts or the receiver loses sync.
    std::vector<uint8_t> pendingTailBuffer;
    size_t pendingTailOffset;

    std::chrono::steady_clock::time_point nextConnectionAttemptTime;
    unsigned long droppedFrameCount;
    bool oversizedPayloadWarningIssued;
};

#endif
