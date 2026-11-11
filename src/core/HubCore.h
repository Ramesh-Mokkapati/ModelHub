#pragma once
// HubCore: everything ModelHub does, behind one JSON command set. The same object serves ModelHubSdk.dll (in an
// application's process) and the hub service (over HTTP), so both behave identically:
//
//   status        hardware, store folders, scan state, running llama-servers
//   hardware      GPU, graphics memory, RAM, cores, AVX2
//   catalog       every model (installed state, path, fit) and every task profile
//   models        installed models
//   model         {id, app?}                one model, with its file path; records app as a user
//   advise        {task, engine, workload:{metrics, summary}, current, context_tokens?, parallel?}
//   fits          {context_tokens, parallel, engine}   fit, memory, GPU layers and speed of every model
//   add_folder    {path}                    share an application's model folder (kept in config.json)
//   rescan        {}                        look for model files again (background)
//   wait_scan     {timeout_ms}              until the files are checked
//   resolve_hf    {spec}                    size and SHA-256 of a Hugging Face GGUF file
//   add_model     {spec | file,url,size,sha256, id?, name?, params_b?, tools?, ...}   a custom model
//   start_download / cancel_download {model}, downloads     background downloads
//   server_start  {model, context_tokens?, parallel?, task?, app?, pin?}   llama-server, waits until ready
//   server_stop   {model}, servers
//   hf_search     {query?, kind?: ""|text|code|embeddings, sort?: downloads|likes|updated|trending, limit?}
//                                           GGUF models on Hugging Face, with best use and an estimated fit
//   hf_repo       {repo}                    one repository: description, best use, every file judged for this PC
//   hf_add        {repo, file}              adds that file as a custom model (then download it as usual)
//   delete_model  {model, recycle?=true}    deletes every copy of a model (to the Recycle Bin by default) and its
//                                           partial download; refuses while it downloads here
//
// Errors come back as {"error": {"message", "type"}, "status": <HTTP-like code>}.
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "Catalog.h"
#include "Config.h"
#include "Downloader.h"
#include "Hardware.h"
#include "ServerPool.h"
#include "Store.h"

namespace modelhub {

struct CoreOptions {
    std::filesystem::path home;                         // models\, manifest.json, config.json, custom_models.json
    std::vector<std::filesystem::path> catalogFiles;    // first that parses wins
    HubConfig config;                                   // usually LoadHubConfig(home\config.json)
    std::vector<std::filesystem::path> extraSearchDirs; // this process only (not saved)
    std::string app;                                    // default user for "model" and "server_start"
    bool scanOnInit = true;                             // check model files in the background at Init
    std::string hfBase = "https://huggingface.co";      // tests point this at a local mirror
    std::function<void(const std::string&)> log;
};

class HubCore {
public:
    explicit HubCore(CoreOptions opt);
    ~HubCore();
    HubCore(const HubCore&) = delete;
    HubCore& operator=(const HubCore&) = delete;

    bool Init(std::string* error);  // catalog, hardware, store; starts the background scan
    nlohmann::json Call(const std::string& method, const nlohmann::json& args);

    // Blocking download into the shared store. onProgress gets {"phase","done","total","bytes_per_second"} and
    // returns false to cancel (the partial file resumes later). Result: the model JSON, or an error.
    nlohmann::json Download(const std::string& model, const std::function<bool(const nlohmann::json&)>& onProgress);

    // For the HTTP layer.
    std::shared_ptr<const Catalog> CatalogSnapshot() const;
    ModelStore& Store() { return *store_; }
    ServerPool& Pool() { return *pool_; }
    bool Scanning() const { return scanning_; }
    void StartScan();
    const HardwareInfo& Hardware() const { return hw_; }
    const CoreOptions& Options() const { return opt_; }

    static nlohmann::json Error(int status, const std::string& message, const std::string& type = "hub_error");

private:
    nlohmann::json ModelJson(const CatalogModel& m);
    nlohmann::json Status();
    nlohmann::json CatalogJson();
    nlohmann::json Advise(const nlohmann::json& a);
    nlohmann::json Fits(const nlohmann::json& a);
    nlohmann::json AddFolder(const nlohmann::json& a);
    nlohmann::json AddModel(const nlohmann::json& a);
    nlohmann::json ServerStart(const nlohmann::json& a);
    nlohmann::json DeleteModel(const nlohmann::json& a);
    nlohmann::json HfSearch(const nlohmann::json& a);
    nlohmann::json HfRepo(const std::string& repo);
    void StopServers(const std::string& modelId);
    bool ReloadCatalog(std::string* error);
    void Log(const std::string& s) const;

    CoreOptions opt_;
    HardwareInfo hw_;
    mutable std::mutex catalogMu_;
    std::shared_ptr<const Catalog> catalog_;
    std::string catalogErrors_;
    std::unique_ptr<ModelStore> store_;
    std::unique_ptr<DownloadManager> downloads_;
    std::unique_ptr<ServerPool> pool_;
    std::mutex pinMu_;
    std::map<std::string, ServerPool::Lease> pinned_;  // servers kept for the life of this object
    std::atomic<bool> scanning_{false}, rescanPending_{false};
    std::jthread scanThread_;
    std::mutex scanMu_;
    mutable std::mutex stateMu_;
    std::string scanStatus_;
    std::mutex hfMu_;
    std::map<std::string, std::pair<uint64_t, nlohmann::json>> hfRepos_;  // repo -> (tick, evaluation), 10 minutes
};

}  // namespace modelhub
