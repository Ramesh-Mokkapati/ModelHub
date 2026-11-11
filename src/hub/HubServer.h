#pragma once
// The hub service: HubCore (catalog, store, downloads, llama.cpp servers; see HubCore.h) behind one HTTP
// server on 127.0.0.1.
//
//   GET  /health                         no token; {"ok":true}
//   GET  /hub/v1/status                  hardware, store, running servers, downloads, scan state
//   GET  /hub/v1/catalog                 models (with installed state) and tasks
//   POST /hub/v1/advise                  {"task", "engine", "workload":{"metrics":{...},"summary"}, "current"}
//   GET  /hub/v1/models                  installed models
//   GET  /hub/v1/models/{id}?app=NAME    one model: its file path, to load in-process (records NAME as a user)
//   POST /hub/v1/downloads               {"model"}       start a download
//   GET  /hub/v1/downloads               progress of all downloads
//   DELETE /hub/v1/downloads/{id}        cancel (the partial file resumes later)
//   POST /hub/v1/folders                 {"path"}: also search an application's model folder (kept in config.json)
//   POST /hub/v1/rescan                  look for model files again
//   POST /hub/v1/fits                    {"context_tokens","parallel","engine"}: fit of every model
//   POST /hub/v1/resolve                 {"spec"}: size and SHA-256 of a Hugging Face GGUF file
//   POST /hub/v1/custom-models           {"spec"} or a catalog entry: add a custom model
//   POST /hub/v1/servers/{id}/start      {"context_tokens","parallel","task"}: load a model now, wait until ready
//   POST /hub/v1/servers/{id}/stop       stop an idle llama-server
//   POST /hub/v1/shutdown
//   GET  /v1/models, POST /v1/chat/completions, /v1/completions, /v1/embeddings
//        OpenAI-compatible; "model" is a catalog id, alias or file name. The model's llama-server starts on
//        first use. Optional headers: X-ModelHub-Context (tokens per request), X-ModelHub-Task (use the task
//        profile's context and parallel slots), X-ModelHub-Parallel (slots), X-ModelHub-App (shown in status).
//
// Every request except /health needs "Authorization: Bearer <token>" (the token is in hub.json, readable only
// by the user), and the Host header must name 127.0.0.1 or localhost: web pages cannot reach the hub.
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Config.h"  // HubConfig, LoadHubConfig, SaveHubConfig

namespace modelhub {

struct HubOptions {
    std::filesystem::path home;                       // models\, manifest.json, hub.json
    std::vector<std::filesystem::path> catalogFiles;  // first that parses wins
    HubConfig config;
    bool writeHubInfo = true;                         // hub.json for clients (tests may turn it off)
};

class Hub {
public:
    explicit Hub(HubOptions opt);
    ~Hub();
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    bool Start(std::string* error);  // binds, writes hub.json, scans the store in the background
    void Wait();                     // until Stop() or POST /hub/v1/shutdown
    void Stop();
    int Port() const;
    const std::string& Token() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace modelhub
