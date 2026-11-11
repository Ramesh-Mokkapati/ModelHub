#pragma once
// The model catalog and task profiles (config/catalog.json, format "modelhub-catalog" v1).
//
// Models are pinned by exact size and SHA-256. A model may list alternate files (the same model quantized or
// published by someone else): any one of them, verified, makes the model installed. This lets applications
// that already downloaded a model from a different repository share it instead of downloading it again.
//
// Task profiles say which models suit a job and when a bigger one is worth it: tiers with thresholds on named
// workload metrics ("lines", "largest_file_lines", ...), each with preferred models best first.
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace modelhub {

struct CatalogFile {
    std::string file;    // file name on disk
    std::string url;     // https download URL
    uint64_t size = 0;   // exact bytes
    std::string sha256;  // lower-case hex
};

struct CatalogModel {
    std::string id;                    // e.g. "qwen2.5-coder-7b"
    std::vector<std::string> aliases;  // other names apps use, e.g. "qwen2.5-coder:7b"
    std::string name;
    std::string family;
    std::vector<CatalogFile> files;    // files[0] is downloaded; the rest are accepted if already present
    double paramsB = 0;
    double activeParamsB = 0;          // per token (lower for mixture-of-experts models)
    int tier = 1;                      // capability: 1 starter, 2 standard, 3 large
    int layers = 0;                    // transformer blocks, for partial GPU offload
    int contextK = 32;                 // trained context, thousands of tokens
    uint64_t kvCacheMB8k = 0;          // KV cache of one 8K-token context
    bool tools = false;                // supports tool calling
    bool embeddings = false;           // an embedding model (served with --embeddings)
    std::string license;
    std::string summary;

    const CatalogFile& Primary() const { return files.front(); }
    uint64_t SizeBytes() const { return files.empty() ? 0 : files.front().size; }
    bool Matches(const std::string& idOrAlias) const;
};

struct TaskTier {
    std::string label;
    std::map<std::string, double> below;  // every listed metric must be below its limit; empty = catch-all
    std::string explain;
    std::vector<std::string> prefer;      // model ids, best first
};

struct TaskProfile {
    std::string id;           // "log-diagnosis", "binary-renaming", ...
    std::string name;
    std::string workloadNoun;
    int contextTokens = 8192; // per request
    int parallel = 1;         // concurrent requests the task usually runs (KV cache per slot)
    bool needsTools = false;
    // true: before there is a workload, recommend the smallest model that runs comfortably (and let the
    // workload ask for more). false: recommend the best preferred model that fits right away.
    bool startSmall = true;
    std::string noWorkloadHint;
    std::vector<TaskTier> tiers;
    bool UsesModel(const std::string& id) const;
};

struct Catalog {
    std::vector<CatalogModel> models;  // smallest first
    std::vector<TaskProfile> tasks;
    std::string source;                // file it came from
    const CatalogModel* Find(const std::string& idOrAlias) const;
    const CatalogModel* FindBySha(const std::string& sha256) const;
    const TaskProfile* FindTask(const std::string& id) const;
};

// Parses and validates; on error returns false with a reason and leaves `out` empty.
bool ParseCatalog(const std::string& jsonText, Catalog& out, std::string& error);
// The first catalog that parses: <hub home>\catalog.json (a user override), then <exe dir>\catalog.json.
// `errors` collects why a candidate was skipped.
std::optional<Catalog> LoadCatalog(const std::vector<std::filesystem::path>& candidates, std::string& errors);

// <hub home>\custom_models.json: models applications added (for example a Hugging Face file a user chose).
// Same fields as catalog models. They belong to no task, so the advisor never offers them, but they can be
// downloaded, found and served like the others. A name that is already taken is skipped (reported in error).
bool MergeCustomModels(Catalog& c, const std::filesystem::path& file, std::string& error);
// Validates one model entry (catalog JSON) and appends it to custom_models.json.
bool AppendCustomModel(const std::filesystem::path& file, const std::string& entryJson, std::string& error);

bool IsSha256(const std::string& s);

}  // namespace modelhub
