#pragma once
// Resumable, verified model downloads: <file>.part grows with HTTP range requests across retries and restarts,
// free disk is checked first, and the SHA-256 is verified before the file is renamed into place.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "Catalog.h"

namespace modelhub {

struct DownloadProgress {
    std::string phase;  // "downloading", "verifying", "done", "failed", "cancelled"
    uint64_t done = 0, total = 0;
    double bytesPerSecond = 0;
};

bool DownloadFile(const CatalogFile& f, const std::filesystem::path& dir,
                  const std::function<void(const DownloadProgress&)>& onProgress, std::stop_token stop,
                  std::string* error);

// Background downloads, one per model at a time. Thread-safe.
class DownloadManager {
public:
    struct Status {
        std::string modelId;
        DownloadProgress progress;
        std::string error;
        bool running = false;
    };
    // Called on the download thread when a file is complete and verified.
    using DoneFn = std::function<void(const CatalogModel&, const CatalogFile&, const std::filesystem::path&)>;

    DownloadManager(std::filesystem::path dir, DoneFn onDone) : dir_(std::move(dir)), onDone_(std::move(onDone)) {}
    ~DownloadManager();

    bool Start(const CatalogModel& m, std::string* error);  // false if already running
    bool Cancel(const std::string& modelId);
    std::vector<Status> List() const;

private:
    std::filesystem::path dir_;
    DoneFn onDone_;
    mutable std::mutex mu_;
    std::map<std::string, Status> status_;
    std::map<std::string, std::jthread> threads_;
};

}  // namespace modelhub
