#include "HuggingFace.h"

#include <nlohmann/json.hpp>

#include "HttpClient.h"
#include "Text.h"

namespace modelhub {

using nlohmann::json;

std::optional<HfFile> ParseHfSpec(std::string_view spec) {
    std::string s = Trim(spec);
    for (std::string_view prefix : {"https://huggingface.co/", "http://huggingface.co/", "hf.co/", "hf:"})
        if (s.rfind(prefix, 0) == 0) s.erase(0, prefix.size());
    for (std::string_view mid : {"/resolve/main/", "/blob/main/"})
        if (auto p = s.find(mid); p != std::string::npos) s = s.substr(0, p) + "/" + s.substr(p + mid.size());
    if (auto q = s.find('?'); q != std::string::npos) s.resize(q);
    // owner/repo/file.gguf (no sub-folders: split files are not supported)
    size_t a = s.find('/');
    size_t b = a == std::string::npos ? a : s.find('/', a + 1);
    if (a == std::string::npos || b == std::string::npos || s.find('/', b + 1) != std::string::npos) return std::nullopt;
    HfFile f;
    f.repo = s.substr(0, b);
    f.file = s.substr(b + 1);
    if (!EndsWithI(f.file, ".gguf") || f.repo.find_first_of(" \"<>|\\") != std::string::npos) return std::nullopt;
    return f;
}

std::optional<CatalogFile> ResolveHfFile(const HfFile& f, std::string* error) {
    HttpResponse r = HttpGet("https://huggingface.co/api/models/" + f.repo + "/tree/main", {}, 15000);
    if (!r.Ok()) {
        if (error)
            *error = r.status == 401 ? "repository not found, private or gated: " + f.repo
                     : r.status    ? "Hugging Face API returned HTTP " + std::to_string(r.status)
                                   : r.error;
        return std::nullopt;
    }
    json j = json::parse(r.body, nullptr, false);
    if (j.is_array())
        for (const auto& e : j) {
            if (e.value("path", "") != f.file) continue;
            CatalogFile c;
            c.file = f.file;
            c.url = f.Url();
            c.size = e.value("size", uint64_t{0});
            if (auto lfs = e.value("lfs", json::object()); lfs.is_object()) c.sha256 = ToLower(lfs.value("oid", ""));
            if (!IsSha256(c.sha256) || !c.size) {
                if (error) *error = f.file + " has no published SHA-256 (not stored with Git LFS)";
                return std::nullopt;
            }
            return c;
        }
    if (error) *error = f.file + " not found in " + f.repo;
    return std::nullopt;
}

}  // namespace modelhub
