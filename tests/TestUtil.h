#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "Hash.h"
#include "Text.h"

namespace testutil {

inline std::filesystem::path TempDir(const char* tag) {
    auto d = std::filesystem::temp_directory_path() / modelhub::Widen(std::string(tag) + modelhub::NewGuid());
    std::filesystem::create_directories(d);
    return d;
}

struct FakeModel {
    std::string id, file, content, alias;
    double paramsB = 1;
    int tier = 1;
    bool tools = true;
    std::string altFile, altContent;  // optional alternate file
};

// Writes the files' contents somewhere else and returns a catalog JSON describing them (sizes + hashes).
inline std::string CatalogJson(const std::vector<FakeModel>& models, const std::string& tasksJson,
                               const std::string& urlBase = "https://example.invalid/") {
    std::string m;
    for (const auto& f : models) {
        if (!m.empty()) m += ",";
        m += "{\"id\":\"" + f.id + "\",\"name\":\"" + f.id + "\",\"file\":\"" + f.file + "\",\"url\":\"" + urlBase + f.file +
             "\",\"size\":" + std::to_string(f.content.size()) + ",\"sha256\":\"" + modelhub::Sha256(f.content) +
             "\",\"params_b\":" + std::to_string(f.paramsB) + ",\"tier\":" + std::to_string(f.tier) +
             ",\"tools\":" + (f.tools ? "true" : "false") + ",\"layers\":10,\"kv_cache_mb_8k\":10";
        if (!f.alias.empty()) m += ",\"aliases\":[\"" + f.alias + "\"]";
        if (!f.altFile.empty())
            m += ",\"alternates\":[{\"file\":\"" + f.altFile + "\",\"url\":\"" + urlBase + f.altFile + "\",\"size\":" +
                 std::to_string(f.altContent.size()) + ",\"sha256\":\"" + modelhub::Sha256(f.altContent) + "\"}]";
        m += "}";
    }
    return "{\"format\":\"modelhub-catalog\",\"version\":1,\"models\":[" + m + "],\"tasks\":" + tasksJson + "}";
}

}  // namespace testutil
