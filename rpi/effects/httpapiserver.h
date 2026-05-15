#ifndef HTTP_API_SERVER_H
#define HTTP_API_SERVER_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

/**
 * @class HttpApiServer
 * @brief Manages a threaded HTTP server to receive and respond to control commands.
 */
class HttpApiServer {
public:
    // Define callback signatures to inject external application logic
    using GetBrightnessFunction = std::function<int()>;
    using SetBrightnessFunction = std::function<void(int)>;

    /**
     * @brief Constructs the HTTP API Server.
     * @param targetPort The port number to bind the server to.
     * @param rateLimitMilliseconds The minimum time allowed between requests.
     */
    HttpApiServer(int targetPort, int rateLimitMilliseconds);
    ~HttpApiServer();

    /**
     * @brief Injects the logic required to read and write brightness levels.
     * @param getCallback Function that returns the current brightness.
     * @param setCallback Function that applies a new brightness level.
     */
    void setCallbacks(GetBrightnessFunction getCallback, SetBrightnessFunction setCallback);

    /**
     * @brief Starts the background listening thread.
     */
    void startServer();

    /**
     * @brief Halts the listening thread and releases resources.
     */
    void stopServer();

private:
    void listenForConnections();
    void processClientRequest(int clientSocketDescriptor);

    int serverPort;
    std::chrono::milliseconds rateLimitDuration;
    std::atomic<bool> isServerRunning;
    
    GetBrightnessFunction onGetBrightness;
    SetBrightnessFunction onSetBrightness;

    std::mutex rateLimitMutex;
    std::chrono::time_point<std::chrono::steady_clock> lastRequestTime;
    
    std::thread listenerThread;
};

#endif // HTTP_API_SERVER_H