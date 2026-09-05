#include "httpapiserver.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

// A request is a couple of hundred bytes; anything larger is not ours.
constexpr size_t MAX_REQUEST_BYTES = 8192;
// Total budget for reading one request, matching the original 2 s socket timeout.
constexpr int CLIENT_READ_TIMEOUT_MS = 2000;
// Bound on request threads. The rate limiter already serialises real traffic,
// so reaching this means something is flooding the port.
constexpr size_t MAX_CONCURRENT_CLIENTS = 8;
constexpr int LISTEN_POLL_TIMEOUT_MS = 500;
constexpr int LISTEN_BACKLOG = 5;

class SocketDescriptorGuard {
public:
    explicit SocketDescriptorGuard(int socketDescriptor)
        : managedSocketDescriptor(socketDescriptor) {}
    ~SocketDescriptorGuard() {
        if (managedSocketDescriptor >= 0) close(managedSocketDescriptor);
    }

    SocketDescriptorGuard(const SocketDescriptorGuard&) = delete;
    SocketDescriptorGuard& operator=(const SocketDescriptorGuard&) = delete;

    int getDescriptor() const { return managedSocketDescriptor; }

private:
    int managedSocketDescriptor;
};

// send() may write fewer bytes than asked even for a small response once the
// socket buffer is under pressure; the original dropped the remainder.
bool sendAll(int socketDescriptor, const std::string& payload) {
    size_t sent = 0;
    while (sent < payload.size()) {
        const ssize_t written =
            send(socketDescriptor, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
        if (written > 0) {
            sent += static_cast<size_t>(written);
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd descriptor{socketDescriptor, POLLOUT, 0};
            if (poll(&descriptor, 1, CLIENT_READ_TIMEOUT_MS) > 0) continue;
        }
        return false;
    }
    return true;
}

// Closing a socket that still has unread inbound data makes the kernel send RST
// instead of FIN, and an RST can discard a response the peer has not read yet.
// Half-close, then drain briefly, so the reply is delivered.
void closeGracefully(int socketDescriptor) {
    shutdown(socketDescriptor, SHUT_WR);
    char drainBuffer[512];
    for (int attempt = 0; attempt < 8; ++attempt) {
        pollfd descriptor{socketDescriptor, POLLIN, 0};
        if (poll(&descriptor, 1, 25) <= 0) break;
        const ssize_t drained = recv(socketDescriptor, drainBuffer, sizeof(drainBuffer), 0);
        if (drained <= 0) break;
    }
}

// Pulls b=<value> out of the request target's query string. Matches the
// parameter name exactly: a plain substring search for "b=" also fires on
// rgb=, ab= and anything else ending in b, and on a path that merely contains
// the pair.
bool extractBrightnessParameter(const std::string& requestLine, std::string& rawValue) {
    const size_t targetStart = requestLine.find(' ');
    if (targetStart == std::string::npos) {
        return false;
    }
    size_t targetEnd = requestLine.find(' ', targetStart + 1);
    if (targetEnd == std::string::npos) {
        targetEnd = requestLine.size();
    }
    const size_t queryStart = requestLine.find('?', targetStart);
    if (queryStart == std::string::npos || queryStart > targetEnd) {
        return false;
    }

    size_t cursor = queryStart + 1;
    while (cursor < targetEnd) {
        size_t separator = requestLine.find_first_of("&;", cursor);
        if (separator == std::string::npos || separator > targetEnd) {
            separator = targetEnd;
        }
        const size_t equals = requestLine.find('=', cursor);
        if (equals != std::string::npos && equals < separator &&
            requestLine.compare(cursor, equals - cursor, "b") == 0) {
            rawValue = requestLine.substr(equals + 1, separator - equals - 1);
            return true;
        }
        cursor = separator + 1;
    }
    return false;
}

std::string buildResponse(const char* statusLine, const std::string& body, bool noStore) {
    std::string response = "HTTP/1.1 ";
    response += statusLine;
    response += "\r\nContent-Type: text/plain\r\nContent-Length: ";
    response += std::to_string(body.size());
    // The original omitted this. Without it a keep-alive client waits for a
    // second response on a connection the server is about to close.
    response += "\r\nConnection: close";
    if (noStore) {
        response += "\r\nCache-Control: no-store";
    }
    response += "\r\n\r\n";
    response += body;
    return response;
}

}  // namespace

HttpApiServer::HttpApiServer(int targetPort, int rateLimitMilliseconds)
    : serverPort(targetPort),
      rateLimitDuration(rateLimitMilliseconds),
      isServerRunning(false),
      // Backdated by one interval: initialising to now() made the very first
      // request after construction fail the rate-limit check and get a 429.
      lastRequestTime(std::chrono::steady_clock::now() - rateLimitDuration) {}

HttpApiServer::~HttpApiServer() {
    stopServer();
}

void HttpApiServer::setCallbacks(GetBrightnessFunction getCallback,
                                 SetBrightnessFunction setCallback) {
    onGetBrightness = std::move(getCallback);
    onSetBrightness = std::move(setCallback);
}

void HttpApiServer::startServer() {
    if (!isServerRunning.exchange(true)) {
        listenerThread = std::thread(&HttpApiServer::listenForConnections, this);
    }
}

void HttpApiServer::stopServer() {
    isServerRunning.store(false);

    // Joined unconditionally rather than behind the exchange: if the listener
    // exited on its own (a failed bind, say), a joinable std::thread reaching
    // its destructor would call std::terminate.
    if (listenerThread.joinable()) {
        listenerThread.join();
    }

    // Request threads hold a this pointer and touch the callbacks and the rate
    // limiter. The original detached them, so a server destroyed while one was
    // in flight left it running against freed memory.
    reapFinishedWorkers(true);
}

void HttpApiServer::reapFinishedWorkers(bool joinAll) {
    std::lock_guard<std::mutex> lockGuard(clientWorkerMutex);
    for (size_t index = 0; index < clientWorkers.size();) {
        ClientWorker& worker = clientWorkers[index];
        if (joinAll || worker.finished->load(std::memory_order_acquire)) {
            if (worker.thread.joinable()) {
                worker.thread.join();
            }
            clientWorkers.erase(clientWorkers.begin() + static_cast<long>(index));
            continue;
        }
        ++index;
    }
}

void HttpApiServer::listenForConnections() {
    const int serverSocketDescriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocketDescriptor < 0) {
        std::cerr << "Fatal: could not create HTTP API socket: " << std::strerror(errno) << "\n";
        return;
    }
    SocketDescriptorGuard serverSocketGuard(serverSocketDescriptor);

    int socketOptions = 1;
    setsockopt(serverSocketDescriptor, SOL_SOCKET, SO_REUSEADDR, &socketOptions,
               sizeof(socketOptions));

    struct sockaddr_in serverAddress {};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(static_cast<uint16_t>(serverPort));

    if (bind(serverSocketDescriptor, reinterpret_cast<struct sockaddr*>(&serverAddress),
             sizeof(serverAddress)) < 0) {
        std::cerr << "Fatal: could not bind HTTP API port " << serverPort << ": "
                  << std::strerror(errno) << "\n";
        return;
    }
    if (listen(serverSocketDescriptor, LISTEN_BACKLOG) < 0) {
        std::cerr << "Fatal: could not listen on HTTP API port " << serverPort << ": "
                  << std::strerror(errno) << "\n";
        return;
    }

    // Non-blocking: a peer that resets between poll() and accept() would
    // otherwise block the listener until the next connection arrives.
    const int listenerFlags = fcntl(serverSocketDescriptor, F_GETFL, 0);
    if (listenerFlags >= 0) {
        fcntl(serverSocketDescriptor, F_SETFL, listenerFlags | O_NONBLOCK);
    }

    pollfd descriptor{serverSocketDescriptor, POLLIN, 0};

    while (isServerRunning.load(std::memory_order_relaxed)) {
        const int ready = poll(&descriptor, 1, LISTEN_POLL_TIMEOUT_MS);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        reapFinishedWorkers(false);
        if (ready == 0) continue;

        for (;;) {
            const int clientSocketDescriptor = accept(serverSocketDescriptor, nullptr, nullptr);
            if (clientSocketDescriptor < 0) break;

            int nodelayFlag = 1;
            setsockopt(clientSocketDescriptor, IPPROTO_TCP, TCP_NODELAY, &nodelayFlag,
                       sizeof(nodelayFlag));

            std::lock_guard<std::mutex> lockGuard(clientWorkerMutex);
            if (clientWorkers.size() >= MAX_CONCURRENT_CLIENTS) {
                const std::string busy = buildResponse("503 Service Unavailable", "Busy\n", false);
                send(clientSocketDescriptor, busy.data(), busy.size(),
                     MSG_NOSIGNAL | MSG_DONTWAIT);
                close(clientSocketDescriptor);
                continue;
            }

            ClientWorker worker;
            worker.finished = std::make_shared<std::atomic<bool>>(false);
            auto finishedFlag = worker.finished;
            worker.thread = std::thread([this, clientSocketDescriptor, finishedFlag]() {
                processClientRequest(clientSocketDescriptor);
                finishedFlag->store(true, std::memory_order_release);
            });
            clientWorkers.push_back(std::move(worker));
        }
    }
}

// Reads until the end of the headers or the budget expires. The original issued
// a single read(), so a request split across TCP segments — which any client
// may do — was parsed from whatever fragment happened to arrive first.
bool HttpApiServer::readRequest(int clientSocketDescriptor, std::string& request) const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(CLIENT_READ_TIMEOUT_MS);
    char readBuffer[1024];

    while (request.size() < MAX_REQUEST_BYTES) {
        if (request.find("\r\n\r\n") != std::string::npos) {
            return true;
        }

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0) break;

        pollfd descriptor{clientSocketDescriptor, POLLIN, 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) break;

        const ssize_t bytesRead = recv(clientSocketDescriptor, readBuffer, sizeof(readBuffer), 0);
        if (bytesRead > 0) {
            request.append(readBuffer, static_cast<size_t>(bytesRead));
            continue;
        }
        if (bytesRead < 0 && errno == EINTR) continue;
        break;  // peer closed, or error
    }

    // A request line on its own is still usable even without a blank line.
    return !request.empty();
}

void HttpApiServer::processClientRequest(int clientSocketDescriptor) {
    SocketDescriptorGuard clientSocketGuard(clientSocketDescriptor);
    const int clientFd = clientSocketGuard.getDescriptor();

    std::string requestString;
    if (!readRequest(clientFd, requestString)) {
        return;
    }

    // Parse only the request line. The original searched the whole request for
    // "b=", so any header or cookie containing that pair would set brightness.
    const size_t lineEnd = requestString.find("\r\n");
    const std::string requestLine =
        requestString.substr(0, lineEnd == std::string::npos ? requestString.size() : lineEnd);

    std::string responseString;
    std::string brightnessValue;

    // A b= parameter is a write whatever the path is, so /brightness?b=100 now
    // sets rather than reads. The original tested the path first, which made
    // that URL silently return the current value instead.
    if (extractBrightnessParameter(requestLine, brightnessValue)) {
        // Rate limiting applies to writes only. Reads are an atomic load and
        // cost the render loop nothing, and counting them meant a UI polling
        // /brightness on a timer could consume the window and bounce the
        // slider write that landed just after it.
        bool rateLimited = false;
        {
            std::lock_guard<std::mutex> lockGuard(rateLimitMutex);
            const auto currentTime = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(currentTime -
                                                                     lastRequestTime) <
                rateLimitDuration) {
                rateLimited = true;
            } else {
                lastRequestTime = currentTime;
            }
        }

        if (rateLimited) {
            // Sent outside the lock: the original held the mutex across the
            // send, so one slow client blocked every other request thread.
            sendAll(clientFd, buildResponse("429 Too Many Requests", "BUSY", false));
            closeGracefully(clientFd);
            return;
        }

        try {
            int targetBrightness = std::stoi(brightnessValue);
            targetBrightness = std::max(0, std::min(targetBrightness, 255));

            if (onSetBrightness) {
                onSetBrightness(targetBrightness);
            }
            responseString = buildResponse("200 OK", "OK\n", false);
        } catch (...) {
            responseString = buildResponse("400 Bad Request", "Invalid Value\n", false);
        }
    } else if (requestLine.find("GET /brightness") != std::string::npos) {
        const int currentBrightness = onGetBrightness ? onGetBrightness() : 0;
        responseString = buildResponse("200 OK", std::to_string(currentBrightness), true);
    } else {
        responseString = buildResponse("404 Not Found", "Not Found\n", false);
    }

    sendAll(clientFd, responseString);
    closeGracefully(clientFd);
}
