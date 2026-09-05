#ifndef HTTP_API_SERVER_H
#define HTTP_API_SERVER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Small single-purpose HTTP endpoint exposing a brightness value.
//
//   GET /brightness   -> current value as plain text
//   ...?b=<0-255>     -> set the value, whatever the path
//
// A b= query parameter always means a write, so /brightness?b=100 sets rather
// than reads. Writes are rate limited to one every rateLimitMilliseconds and
// get 429 if they arrive sooner; reads are not limited, since they only take an
// atomic load and rejecting them would make a polling UI throttle its own
// slider. Routes, status codes and bodies are otherwise as before.
class HttpApiServer {
public:
    using GetBrightnessFunction = std::function<int()>;
    using SetBrightnessFunction = std::function<void(int)>;

    HttpApiServer(int targetPort, int rateLimitMilliseconds);
    ~HttpApiServer();

    HttpApiServer(const HttpApiServer&) = delete;
    HttpApiServer& operator=(const HttpApiServer&) = delete;

    // Call before startServer(). The callbacks are read from worker threads, so
    // reassigning them on a running server would race with a request in flight.
    void setCallbacks(GetBrightnessFunction getCallback, SetBrightnessFunction setCallback);

    void startServer();

    // Joins the listener and every outstanding request thread. Safe to call
    // more than once, and called by the destructor.
    void stopServer();

private:
    // A request worker plus the flag it raises on the way out, so the listener
    // can join finished workers without blocking on live ones.
    struct ClientWorker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };

    void listenForConnections();
    void processClientRequest(int clientSocketDescriptor);
    bool readRequest(int clientSocketDescriptor, std::string& request) const;
    void reapFinishedWorkers(bool joinAll);

    int serverPort;
    std::chrono::milliseconds rateLimitDuration;
    std::atomic<bool> isServerRunning;

    GetBrightnessFunction onGetBrightness;
    SetBrightnessFunction onSetBrightness;

    std::mutex rateLimitMutex;
    std::chrono::time_point<std::chrono::steady_clock> lastRequestTime;

    std::thread listenerThread;
    std::mutex clientWorkerMutex;
    std::vector<ClientWorker> clientWorkers;
};

#endif  // HTTP_API_SERVER_H
