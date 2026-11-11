#include "Config.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>

#include "Hash.h"
#include "Paths.h"
#include "Text.h"

namespace modelhub {

namespace fs = std::filesystem;
using nlohmann::json;

HubConfig LoadHubConfig(const fs::path& file) {
    HubConfig c;
    fs::path local = KnownFolderLocalAppData();
    c.searchDirs = {local / L"RevEngStudio" / L"models", local / L"AppAssist" / L"models"};
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        SaveHubConfig(file, c);
        return c;
    }
    json j;
    try {
        j = json::parse(ReadTextFile(file), nullptr, false, true);
    } catch (...) {
        return c;
    }
    if (!j.is_object()) return c;
    c.port = j.value("port", c.port);
    if (j.contains("search_dirs") && j["search_dirs"].is_array()) {
        c.searchDirs.clear();
        for (const auto& d : j["search_dirs"])
            if (d.is_string()) c.searchDirs.push_back(Utf8ToPath(d.get<std::string>()));
    }
    c.llamaServer = Utf8ToPath(j.value("llama_server", ""));
    c.idleMinutes = std::max(1, j.value("idle_minutes", c.idleMinutes));
    c.parallel = std::clamp(j.value("parallel", c.parallel), 1, 16);
    c.basePort = j.value("base_port", c.basePort);
    for (const auto& a : j.value("llama_args", std::vector<std::string>{})) c.llamaArgs.push_back(Widen(a));
    return c;
}

void SaveHubConfig(const fs::path& file, const HubConfig& c) {
    json dirs = json::array();
    for (const auto& d : c.searchDirs) dirs.push_back(PathToUtf8(d));
    json args = json::array();
    for (const auto& a : c.llamaArgs) args.push_back(Narrow(a));
    json j = {{"comment", "ModelHub settings, shared by the hub service and every application using ModelHub. "
                          "search_dirs: folders where applications keep GGUF models; files there are verified and "
                          "used in place. llama_server: empty = the bundled one."},
              {"port", c.port},
              {"search_dirs", dirs},
              {"llama_server", PathToUtf8(c.llamaServer)},
              {"idle_minutes", c.idleMinutes},
              {"parallel", c.parallel},
              {"base_port", c.basePort},
              {"llama_args", args}};
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    WriteTextFile(tmp, j.dump(2) + "\n");
    fs::rename(tmp, file, ec);
}

bool AddSearchDirToConfig(const fs::path& file, const fs::path& dir) {
    std::wstring name = L"Local\\ModelHub-config-" + Widen(Sha256(ToLower(PathToUtf8(file))).substr(0, 16));
    HANDLE m = CreateMutexW(nullptr, FALSE, name.c_str());
    if (m) WaitForSingleObject(m, 10000);
    HubConfig cfg = LoadHubConfig(file);
    bool present = std::any_of(cfg.searchDirs.begin(), cfg.searchDirs.end(), [&](const fs::path& d) {
        return ToLower(PathToUtf8(d.lexically_normal())) == ToLower(PathToUtf8(dir.lexically_normal()));
    });
    if (!present) {
        cfg.searchDirs.push_back(dir);
        SaveHubConfig(file, cfg);
    }
    if (m) {
        ReleaseMutex(m);
        CloseHandle(m);
    }
    return !present;
}

}  // namespace modelhub
