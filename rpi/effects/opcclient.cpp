#include "opcclient.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {
constexpr size_t OPC_HEADER_SIZE = 4;
constexpr uint8_t OPC_COMMAND_SET_8BIT_PIXELS = 0;
constexpr int CONNECT_TIMEOUT_MILLISECONDS = 200;
constexpr int RECONNECT_BACKOFF_MILLISECONDS = 500;
}  // namespace

OPCClient::OPCClient(const std::string& hostAddress, int portNumber)
    : serverAddress(hostAddress),
      serverPort(portNumber),
      activeSocketDescriptor(-1),
      pendingTailOffset(0),
      nextConnectionAttemptTime(std::chrono::steady_clock::now()),
      droppedFrameCount(0),
      oversizedPayloadWarningIssued(false) {
    // Connection is deferred until the first frame.
}

OPCClient::~OPCClient() { disconnectFromServer(); }

void OPCClient::disconnectFromServer() {
    if (activeSocketDescriptor != -1) {
        close(activeSocketDescriptor);
        activeSocketDescriptor = -1;
    }
    // Whatever was half-written belonged to a stream that no longer exists.
    pendingTailBuffer.clear();
    pendingTailOffset = 0;
}

void OPCClient::connectToServer() {
    if (activeSocketDescriptor != -1) return;

    activeSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (activeSocketDescriptor < 0) {
        activeSocketDescriptor = -1;
        return;
    }

    const int disableNagleAlgorithm = 1;
    setsockopt(activeSocketDescriptor, IPPROTO_TCP, TCP_NODELAY, &disableNagleAlgorithm,
               sizeof(disableNagleAlgorithm));

    const int existingFlags = fcntl(activeSocketDescriptor, F_GETFL, 0);
    fcntl(activeSocketDescriptor, F_SETFL, existingFlags | O_NONBLOCK);

    sockaddr_in serverSocketAddress;
    memset(&serverSocketAddress, 0, sizeof(serverSocketAddress));
    serverSocketAddress.sin_family = AF_INET;
    serverSocketAddress.sin_port = htons(static_cast<uint16_t>(serverPort));

    if (inet_pton(AF_INET, serverAddress.c_str(), &serverSocketAddress.sin_addr) <= 0) {
        disconnectFromServer();
        return;
    }

    const int connectResult =
        connect(activeSocketDescriptor, reinterpret_cast<struct sockaddr*>(&serverSocketAddress),
                sizeof(serverSocketAddress));

    if (connectResult == 0) return;  // connected immediately, as loopback usually does

    if (errno != EINPROGRESS) {
        disconnectFromServer();
        return;
    }

    // A blocking connect to an unreachable host parks the render loop for the
    // full TCP handshake timeout - over a minute. Bound the wait instead.
    struct pollfd connectPollDescriptor;
    connectPollDescriptor.fd = activeSocketDescriptor;
    connectPollDescriptor.events = POLLOUT;
    connectPollDescriptor.revents = 0;

    if (poll(&connectPollDescriptor, 1, CONNECT_TIMEOUT_MILLISECONDS) <= 0) {
        disconnectFromServer();
        return;
    }

    int pendingSocketError = 0;
    socklen_t errorLength = sizeof(pendingSocketError);
    if (getsockopt(activeSocketDescriptor, SOL_SOCKET, SO_ERROR, &pendingSocketError, &errorLength) < 0 ||
        pendingSocketError != 0) {
        disconnectFromServer();
    }
}

bool OPCClient::ensureConnected() {
    if (activeSocketDescriptor != -1) return true;

    // Without a backoff, a render loop retries the connection every frame.
    const auto rightNow = std::chrono::steady_clock::now();
    if (rightNow < nextConnectionAttemptTime) return false;

    connectToServer();
    if (activeSocketDescriptor == -1) {
        nextConnectionAttemptTime = rightNow + std::chrono::milliseconds(RECONNECT_BACKOFF_MILLISECONDS);
        return false;
    }
    return true;
}

bool OPCClient::flushPendingBytes() {
    while (pendingTailOffset < pendingTailBuffer.size()) {
        const ssize_t bytesTransmitted =
            send(activeSocketDescriptor, pendingTailBuffer.data() + pendingTailOffset,
                 pendingTailBuffer.size() - pendingTailOffset, MSG_NOSIGNAL);

        if (bytesTransmitted > 0) {
            pendingTailOffset += static_cast<size_t>(bytesTransmitted);
            continue;
        }
        if (bytesTransmitted < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;  // still backed up
        }
        if (bytesTransmitted < 0 && errno == EINTR) {
            continue;
        }
        disconnectFromServer();
        return false;
    }

    pendingTailBuffer.clear();
    pendingTailOffset = 0;
    return true;
}

void OPCClient::queueUnsentTail(const uint8_t* headerBytes, size_t headerSize,
                                const uint8_t* payloadDataPointer, size_t payloadByteSize,
                                size_t bytesAlreadySent) {
    pendingTailBuffer.clear();
    pendingTailOffset = 0;

    if (bytesAlreadySent < headerSize) {
        pendingTailBuffer.insert(pendingTailBuffer.end(), headerBytes + bytesAlreadySent,
                                 headerBytes + headerSize);
        pendingTailBuffer.insert(pendingTailBuffer.end(), payloadDataPointer,
                                 payloadDataPointer + payloadByteSize);
    } else {
        const size_t payloadOffset = bytesAlreadySent - headerSize;
        pendingTailBuffer.insert(pendingTailBuffer.end(), payloadDataPointer + payloadOffset,
                                 payloadDataPointer + payloadByteSize);
    }
}

bool OPCClient::sendPixelPayload(uint8_t channelIndex, const uint8_t* payloadDataPointer,
                                 size_t payloadByteSize) {
    if (payloadDataPointer == nullptr || payloadByteSize == 0) return false;

    // The OPC length field is 16 bits. Truncating it silently, as a plain mask
    // does, leaves the receiver reading pixel data as the next packet header and
    // it never resynchronises.
    if (payloadByteSize > MAX_PAYLOAD_BYTES) {
        if (!oversizedPayloadWarningIssued) {
            std::cerr << "OPC payload of " << payloadByteSize << " bytes exceeds the protocol maximum of "
                      << MAX_PAYLOAD_BYTES << " (" << MAX_PAYLOAD_BYTES / 3
                      << " pixels). Frame not sent.\n";
            oversizedPayloadWarningIssued = true;
        }
        return false;
    }

    if (!ensureConnected()) return false;

    // Finish any partially written packet before starting a new one.
    if (!flushPendingBytes()) {
        if (activeSocketDescriptor != -1) ++droppedFrameCount;
        return false;
    }

    // OPC header: [channel] [command] [length high] [length low]
    const uint8_t headerBytes[OPC_HEADER_SIZE] = {
        channelIndex, OPC_COMMAND_SET_8BIT_PIXELS,
        static_cast<uint8_t>((payloadByteSize >> 8) & 0xFF),
        static_cast<uint8_t>(payloadByteSize & 0xFF)};

    // Header and payload go out as one scattered write, so the payload is never
    // copied into an intermediate packet buffer.
    struct iovec scatterSegments[2];
    scatterSegments[0].iov_base = const_cast<uint8_t*>(headerBytes);
    scatterSegments[0].iov_len = OPC_HEADER_SIZE;
    scatterSegments[1].iov_base = const_cast<uint8_t*>(payloadDataPointer);
    scatterSegments[1].iov_len = payloadByteSize;

    struct msghdr scatterMessage;
    memset(&scatterMessage, 0, sizeof(scatterMessage));
    scatterMessage.msg_iov = scatterSegments;
    scatterMessage.msg_iovlen = 2;

    const size_t totalPacketSize = OPC_HEADER_SIZE + payloadByteSize;
    const ssize_t bytesTransmitted = sendmsg(activeSocketDescriptor, &scatterMessage, MSG_NOSIGNAL);

    if (bytesTransmitted < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            // Nothing entered the stream, so nothing is out of sync. Drop it.
            ++droppedFrameCount;
            return false;
        }
        disconnectFromServer();
        return false;
    }

    if (static_cast<size_t>(bytesTransmitted) < totalPacketSize) {
        // A short write leaves half a packet on the wire. The remainder has to go
        // out before anything else or the receiver loses framing for good.
        queueUnsentTail(headerBytes, OPC_HEADER_SIZE, payloadDataPointer, payloadByteSize,
                        static_cast<size_t>(bytesTransmitted));
        flushPendingBytes();
    }

    return true;
}
