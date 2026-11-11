#include "Catalog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

#include "Text.h"

namespace modelhub {

using nlohmann::json;

bool IsSha256(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

bool CatalogModel::Matches(const std::string& idOrAlias) const {
    std::string n = ToLower(idOrAlias);
    if (ToLower(id) == n) return true;
    for (const auto& a : aliases)
        if (ToLower(a) == n) return true;
    for (const auto& f : files)  // a bare file name works too
        if (ToLower(f.file) == n) return true;
    return false;
}

bool TaskProfile::UsesModel(const std::string& modelId) const {
    for (const auto& t : tiers)
        if (std::find(t.prefer.begin(), t.prefer.end(), modelId) != t.prefer.end()) return true;
    return false;
}

const CatalogModel* Catalog::Find(const std::string& name) const {
    for (const auto& m : models)
        if (m.Matches(name)) return &m;
    return nullptr;
}

const CatalogModel* Catalog::FindBySha(const std::string& sha) const {
    for (const auto& m : models)
        for (const auto& f : m.files)
            if (f.sha256 == sha) return &m;
    return nullptr;
}

const TaskProfile* Catalog::FindTask(const std::string& id) const {
    for (const auto& t : tasks)
        if (t.id == id) return &t;
    return nullptr;
}

namespace {

CatalogFile ParseFile(const json& j, const std::string& modelId) {
    CatalogFile f;
    f.file = j.at("file").get<std::string>();
    f.url = j.at("url").get<std::string>();
    f.size = j.at("size").get<uint64_t>();
    f.sha256 = ToLower(j.at("sha256").get<std::string>());
    if (f.size == 0 || !IsSha256(f.sha256) || (f.url.rfind("https://", 0) != 0 && f.url.rfind("http://127.0.0.1:", 0) != 0) || !EndsWithI(f.file, ".gguf") ||
        f.file.find_first_of("\\/:*?\"<>|") != std::string::npos)
        throw std::runtime_error("model \"" + modelId + "\": each file needs a plain .gguf name, an https url (or a local http://127.0.0.1 mirror), its "
                                 "exact size and a 64-digit sha256");
    return f;
}

}  // namespace

bool ParseCatalog(const std::string& text, Catalog& out, std::string& error) {
    out = {};
    try {
        json root = json::parse(text, nullptr, true, true);
        if (root.value("format", "") != "modelhub-catalog") throw std::runtime_error("not a modelhub-catalog file");
        std::set<std::string> names;
        for (const json& m : root.at("models")) {
            CatalogModel c;
            c.id = m.at("id").get<std::string>();
            c.aliases = m.value("aliases", std::vector<std::string>{});
            c.name = m.value("name", c.id);
            c.family = m.value("family", "");
            c.files.push_back(ParseFile(m, c.id));
            for (const json& alt : m.value("alternates", json::array())) c.files.push_back(ParseFile(alt, c.id));
            c.paramsB = m.value("params_b", 0.0);
            c.activeParamsB = m.value("active_params_b", c.paramsB);
            c.tier = m.value("tier", 1);
            c.layers = m.value("layers", 0);
            c.contextK = m.value("context_k", 32);
            c.kvCacheMB8k = m.value("kv_cache_mb_8k", uint64_t{500});
            c.tools = m.value("tools", false);
            c.embeddings = m.value("embeddings", false);
            c.license = m.value("license", "see the model card");
            c.summary = m.value("summary", "");
            if (c.tier < 1 || c.tier > 3) throw std::runtime_error("model \"" + c.id + "\": tier must be 1-3");
            if (c.paramsB <= 0) throw std::runtime_error("model \"" + c.id + "\": params_b is required");
            for (const std::string& n : [&] {
                     auto v = c.aliases;
                     v.push_back(c.id);
                     return v;
                 }())
                if (!names.insert(ToLower(n)).second)
                    throw std::runtime_error("model name \"" + n + "\" is used twice");
            out.models.push_back(std::move(c));
        }
        std::stable_sort(out.models.begin(), out.models.end(),
                         [](const CatalogModel& a, const CatalogModel& b) { return a.SizeBytes() < b.SizeBytes(); });

        for (const json& t : root.at("tasks")) {
            TaskProfile task;
            task.id = t.at("id").get<std::string>();
            task.name = t.value("name", task.id);
            task.workloadNoun = t.value("workload", "input");
            task.contextTokens = t.value("context_tokens", 8192);
            task.parallel = std::max(1, t.value("parallel", 1));
            task.needsTools = t.value("needs_tools", false);
            task.startSmall = t.value("start_small", true);
            task.noWorkloadHint = t.value("no_workload_hint", "");
            for (const json& r : t.at("tiers")) {
                TaskTier tier;
                tier.label = r.value("label", "");
                tier.explain = r.value("explain", "");
                json below = r.value("below", json::object());
                for (auto it = below.begin(); it != below.end(); ++it) tier.below[it.key()] = it.value().get<double>();
                for (const json& id : r.at("prefer")) {
                    std::string mid = id.get<std::string>();
                    const CatalogModel* m = out.Find(mid);
                    if (!m || m->id != mid)
                        throw std::runtime_error("task \"" + task.id + "\" prefers unknown model \"" + mid + "\"");
                    if (task.needsTools && !m->tools)
                        throw std::runtime_error("task \"" + task.id + "\" needs tool calling, which \"" + mid +
                                                 "\" does not support");
                    tier.prefer.push_back(mid);
                }
                if (tier.prefer.empty())
                    throw std::runtime_error("task \"" + task.id + "\" has a tier without preferred models");
                task.tiers.push_back(std::move(tier));
            }
            if (task.tiers.empty()) throw std::runtime_error("task \"" + task.id + "\" has no tiers");
            if (out.FindTask(task.id)) throw std::runtime_error("task \"" + task.id + "\" is defined twice");
            out.tasks.push_back(std::move(task));
        }
        if (out.models.empty()) throw std::runtime_error("no models");
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        out = {};
        return false;
    }
}

namespace {
json ReadCustom(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return json::array();
    json j = json::parse(ReadTextFile(file), nullptr, false, true);
    return j.is_object() && j["models"].is_array() ? j["models"] : json::array();
}
}  // namespace

bool MergeCustomModels(Catalog& c, const std::filesystem::path& file, std::string& error) {
    json models = ReadCustom(file);
    if (models.empty()) return true;
    Catalog custom;
    json wrapper = {{"format", "modelhub-catalog"}, {"version", 1}, {"models", models}, {"tasks", json::array()}};
    if (!ParseCatalog(wrapper.dump(), custom, error)) {
        error = PathToUtf8(file) + ": " + error;
        return false;
    }
    for (auto& m : custom.models) {
        bool taken = c.Find(m.id) != nullptr;
        for (const auto& a : m.aliases) taken = taken || c.Find(a) != nullptr;
        if (taken) {
            error += (error.empty() ? "" : "; ") + std::string("custom model \"") + m.id + "\" skipped: the name is taken";
            continue;
        }
        c.models.push_back(std::move(m));
    }
    std::stable_sort(c.models.begin(), c.models.end(),
                     [](const CatalogModel& a, const CatalogModel& b) { return a.SizeBytes() < b.SizeBytes(); });
    return true;
}

bool AppendCustomModel(const std::filesystem::path& file, const std::string& entryJson, std::string& error) {
    json entry = json::parse(entryJson, nullptr, false);
    if (!entry.is_object()) {
        error = "the model must be a JSON object";
        return false;
    }
    Catalog check;
    json wrapper = {{"format", "modelhub-catalog"}, {"version", 1}, {"models", json::array({entry})}, {"tasks", json::array()}};
    if (!ParseCatalog(wrapper.dump(), check, error)) return false;
    json models = ReadCustom(file);
    for (const auto& m : models)
        if (ToLower(m.value("id", "")) == ToLower(entry.value("id", ""))) {
            error = "a custom model \"" + entry.value("id", "") + "\" already exists";
            return false;
        }
    models.push_back(entry);
    json out = {{"comment", "Models added by applications (ModelHub add_model). Same fields as catalog.json."},
                {"models", models}};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    WriteTextFile(file, out.dump(2) + "\n");
    return true;
}

std::optional<Catalog> LoadCatalog(const std::vector<std::filesystem::path>& candidates, std::string& errors) {
    for (const auto& p : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(p, ec)) continue;
        Catalog c;
        std::string err;
        try {
            if (ParseCatalog(ReadTextFile(p), c, err)) {
                c.source = PathToUtf8(p);
                return c;
            }
        } catch (const std::exception& e) {
            err = e.what();
        }
        errors += PathToUtf8(p) + ": " + err + "\n";
    }
    return std::nullopt;
}

}  // namespace modelhub
