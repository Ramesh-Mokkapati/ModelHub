#pragma once
// <hub home>\config.json: settings shared by the hub service and every application using ModelHubSdk.dll.
#include <filesystem>
#include <string>
#include <vector>

namespace modelhub {

struct HubConfig {
    int port = 39281;                                // hub service; 0 = any free port
    std::vector<std::filesystem::path> searchDirs;   // other applications' model folders (shared by everyone)
    std::filesystem::path llamaServer;               // empty = the bundled llama\llama-server.exe
    int idleMinutes = 10;
    int parallel = 2;
    int basePort = 39300;                            // first port for llama-server instances
    std::vector<std::wstring> llamaArgs;             // extra llama-server arguments
};

// Written with defaults when missing. The defaults search the model folders of RevEng Studio and AppAssist.
HubConfig LoadHubConfig(const std::filesystem::path& file);
void SaveHubConfig(const std::filesystem::path& file, const HubConfig& cfg);
// Adds a search folder to config.json (re-read first, so other processes' changes are kept). False if present.
bool AddSearchDirToConfig(const std::filesystem::path& file, const std::filesystem::path& dir);

}  // namespace modelhub
