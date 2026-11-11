#pragma once
// The machine-wide model store: <hub home>\models plus folders other applications already keep models in.
// Files are recognised by catalog file name and exact size, then verified once by SHA-256; the result is cached
// in manifest.json (path + size + modification time), so a 20 GB file is hashed once, not at every start.
// Files found outside the hub's folder are used where they are: never moved, renamed or deleted.
//
// Several processes share one store (the hub service and every application using ModelHubSdk.dll): changes
// to manifest.json are made under a machine-wide lock after re-reading it, and readers pick up changes made
// by other processes.
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "Catalog.h"

namespace modelhub {

struct InstalledFile {
    std::string modelId;
    std::string file;
    std::string sha256;
    uint64_t size = 0;
    std::filesystem::path path;
    bool inHubFolder = false;        // false: found in another application's folder
    std::vector<std::string> users;  // applications that asked for it
};

struct ScanResult {
    int verified = 0;   // files hashed during this scan
    int found = 0;      // verified files known after the scan
    int rejected = 0;   // right name and size, wrong hash
};

class ModelStore {
public:
    ModelStore(std::filesystem::path modelsDir, std::filesystem::path manifest,
               std::vector<std::filesystem::path> searchDirs);

    const std::filesystem::path& ModelsDir() const { return modelsDir_; }
    std::vector<std::filesystem::path> SearchDirs() const;

    using ProgressFn = std::function<void(const std::string&)>;
    ScanResult Scan(const Catalog& catalog, const ProgressFn& progress, std::stop_token stop = {});

    std::optional<InstalledFile> Find(const CatalogModel& m) const;  // a verified file of this model
    std::vector<InstalledFile> List() const;
    std::vector<InstalledFile> FindAll(const CatalogModel& m) const;  // every verified copy of this model
    // Forgets files (after they were deleted); other processes pick it up from manifest.json.
    void Remove(const std::vector<std::filesystem::path>& paths);
    // Records a finished download into the hub folder (already verified by the downloader).
    void AddDownloaded(const CatalogModel& m, const CatalogFile& f, const std::filesystem::path& path);
    void AddUser(const CatalogModel& m, const std::string& app);  // who uses a model (shown in status)
    bool AddSearchDir(const std::filesystem::path& dir);           // false if already searched

private:
    struct Entry {
        std::filesystem::path path;
        uint64_t size = 0;
        int64_t mtime = 0;
        std::string sha256;   // actual hash
        bool verified = false;
        std::string modelId, file;
        std::vector<std::string> users;
    };
    std::vector<Entry> ReadManifest() const;                  // from disk
    void WriteManifest(const std::vector<Entry>& e) const;    // caller holds the machine-wide lock
    void RefreshIfChanged() const;                            // caller holds mu_
    static int64_t MTime(const std::filesystem::path& p);
    InstalledFile ToInstalled(const Entry& e) const;

    // Machine-wide lock for read-modify-write of manifest.json.
    class ManifestLock {
    public:
        explicit ManifestLock(const std::filesystem::path& manifest);
        ~ManifestLock();
    private:
        void* mutex_ = nullptr;
    };

    std::filesystem::path modelsDir_, manifest_;
    std::vector<std::filesystem::path> searchDirs_;
    mutable std::mutex mu_;
    mutable std::vector<Entry> entries_;
    mutable int64_t loadedMTime_ = -1;  // manifest.json's time when entries_ was read
};

}  // namespace modelhub
