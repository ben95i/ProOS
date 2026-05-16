#ifndef HTTP_API_SERVER_H
#define HTTP_API_SERVER_H

#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <chrono>

class HttpApiServer {
public:
    using GetBrightnessFunction = std::function<int()>;
    using SetBrightnessFunction = std::function<void(int)>;

    HttpApiServer(int targetPort, int rateLimitMilliseconds);
    ~HttpApiServer();

    void setCallbacks(GetBrightnessFunction getCallback, SetBrightnessFunction setCallback);
    void startServer();
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