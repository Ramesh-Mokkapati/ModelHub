#pragma once
// llama.cpp servers started on demand: one llama-server per (model, context size), bound to 127.0.0.1, GPU
// layers chosen from the graphics memory still free, stopped after an idle period, and idle servers stopped
// first when a new model needs the memory. Thread-safe.
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "Advisor.h"
#include "Catalog.h"
#include "Hardware.h"
#include "ProcessRunner.h"

namespace modelhub {

struct ServerPoolOptions {
    std::filesystem::path llamaServer;  // llama-server.exe
    int basePort = 39300;               // llama-server ports start here (the hub's own port is separate)
    std::chrono::seconds idleTimeout{600};
    int defaultParallel = 2;            // slots per server
    std::vector<std::wstring> extraArgs;
};

class ServerPool {
public:
    using LogFn = std::function<void(const std::string&)>;
    ServerPool(ServerPoolOptions opt, HardwareInfo hw, LogFn log);
    ~ServerPool();
    ServerPool(const ServerPool&) = delete;
    ServerPool& operator=(const ServerPool&) = delete;

    // Keeps a server busy (not stopped for idleness or memory) while held.
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& o) noexcept : pool_(o.pool_), key_(std::move(o.key_)), url_(std::move(o.url_)) { o.pool_ = nullptr; }
        Lease& operator=(Lease&&) = delete;
        ~Lease();
        explicit operator bool() const { return pool_ != nullptr; }
        const std::string& BaseUrl() const { return url_; }  // http://127.0.0.1:<port>
    private:
        friend class ServerPool;
        Lease(ServerPool* p, std::string k, std::string u) : pool_(p), key_(std::move(k)), url_(std::move(u)) {}
        ServerPool* pool_ = nullptr;
        std::string key_, url_;
    };

    // Starts (or reuses) a server for the model file. contextTokens is per request (slot); 0 = 8192.
    Lease Acquire(const CatalogModel& m, const std::filesystem::path& gguf, int contextTokens, int parallel,
                  std::stop_token stop, std::string* error);

    struct Info {
        std::string modelId;
        int port = 0, gpuLayers = 0, contextTokens = 0, parallel = 0, active = 0;
        uint32_t pid = 0;
        uint64_t memoryMB = 0;
        int64_t idleSeconds = 0;
    };
    std::vector<Info> Running();
    void Stop(const std::string& modelId);  // all servers of the model that are not in use
    void StopAll();

    static std::vector<std::wstring> BuildArgs(const std::filesystem::path& gguf, int port, int parallel,
                                               int contextTokens, int gpuLayers, const std::string& alias,
                                               bool embeddings, const std::string& device);
    static std::string ChooseDevice(const std::vector<std::string>& listDevicesOutput, const std::string& gpuName);

private:
    struct Server {
        std::string modelId;
        std::unique_ptr<ManagedProcess> proc;
        int port = 0, gpuLayers = 0, contextTokens = 0, parallel = 0, active = 0;
        uint64_t gpuMB = 0, memoryMB = 0;
        std::chrono::steady_clock::time_point lastUsed;
        std::string url;
    };
    void Release(const std::string& key);
    void ReapIdle();
    void StopLocked(const std::string& key);  // caller holds mu_

    ServerPoolOptions opt_;
    HardwareInfo hw_;
    LogFn log_;
    std::mutex startMu_;  // one model loads at a time (loading two at once fights for memory)
    std::mutex mu_;
    std::map<std::string, Server> servers_;  // key: model|context
    std::optional<std::string> device_;
    std::jthread reaper_;
    std::condition_variable_any reaperCv_;
};

}  // namespace modelhub
