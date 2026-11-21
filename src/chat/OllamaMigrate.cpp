#include "OllamaMigrate.h"

#include "Util.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace chat
{

namespace
{

std::wstring Env(const wchar_t* name)
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH * 2);
    return n > 0 && n < MAX_PATH * 2 ? std::wstring(buf, n) : std::wstring();
}

// "registry.ollama.ai/library/qwen3/8b" -> "qwen3:8b"; other registries and namespaces keep their path.
std::string ModelName(const std::vector<std::string>& parts)
{
    if (parts.size() < 3)
    {
        return {};
    }

    std::string tag = parts.back(), model = parts[parts.size() - 2];
    std::string prefix;
    for (size_t i = 0; i + 2 < parts.size(); ++i)
    {
        prefix += parts[i] + "/";
    }

    if (prefix == "registry.ollama.ai/library/")
    {
        prefix.clear();
    }
    else if (prefix.rfind("registry.ollama.ai/", 0) == 0)
    {
        prefix.erase(0, 19);
    }

    return prefix + model + ":" + tag;
}

// "qwen3:8b" -> "qwen3-8b": a ModelHub id and file name.
std::string Slug(const std::string& name)
{
    std::string s;
    for (char c : Lower(name))
    {
        s += std::isalnum(static_cast<unsigned char>(c)) || c == '.' ? c : '-';
    }

    while (s.find("--") != std::string::npos)
    {
        s.replace(s.find("--"), 2, "-");
    }

    while (!s.empty() && (s.front() == '-' || s.front() == '.'))
    {
        s.erase(0, 1);
    }

    while (!s.empty() && s.back() == '-')
    {
        s.pop_back();
    }

    return s.empty() ? std::string("model") : s;
}

json GetJson(const Endpoint& ep, const std::string& path)
{
    std::string root = ep.baseUrl.substr(0, ep.baseUrl.size() - 3);
    HttpResult r = HttpSend(L"GET", root + path, "", ep.headers, 30000);
    json j = json::parse(r.Ok() ? r.body : std::string("null"), nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool PostJson(const Endpoint& ep, const std::string& path, const json& body, json& reply, std::string& error)
{
    std::string root = ep.baseUrl.substr(0, ep.baseUrl.size() - 3);
    HttpHeaders h = ep.headers;
    h.push_back({ "Content-Type", "application/json" });
    HttpResult r = HttpSend(L"POST", root + path, body.dump(), h, 60000);
    reply = json::parse(r.body.empty() ? std::string("{}") : r.body, nullptr, false);
    if (r.Ok())
    {
        return true;
    }

    error = r.status == 0 ? r.error
                          : "HTTP " + std::to_string(r.status) + ": " +
                                (reply.is_object() && reply.contains("error") ? reply["error"].value("message", r.body) : Truncate(r.body, 300));
    return false;
}

}  // namespace

OllamaInventory FindOllamaModels()
{
    OllamaInventory inv;
    std::wstring dir = Env(L"OLLAMA_MODELS");
    if (dir.empty())
    {
        dir = Env(L"USERPROFILE") + L"\\.ollama\\models";
    }

    inv.modelsDir = dir;
    std::wstring local = Env(L"LOCALAPPDATA"), programs = Env(L"ProgramFiles");
    std::error_code ec;
    for (const std::wstring& exe : { local + L"\\Programs\\Ollama\\ollama.exe", programs + L"\\Ollama\\ollama.exe" })
    {
        inv.ollamaInstalled = inv.ollamaInstalled || fs::exists(exe, ec);
    }

    HttpResult ping = HttpSend(L"GET", "http://127.0.0.1:11434/api/version", "", {}, 1500);
    inv.ollamaRunning = ping.Ok();

    fs::path manifests = fs::path(dir) / L"manifests";
    if (!fs::is_directory(manifests, ec))
    {
        return inv;
    }

    std::map<std::string, size_t> bySha;  // one entry per file: tags of the same file become aliases
    for (fs::recursive_directory_iterator it(manifests, fs::directory_options::skip_permission_denied, ec), end;
        !ec && it != end; it.increment(ec))
    {
        if (!it->is_regular_file(ec))
        {
            continue;
        }

        std::string text;
        if (!ReadFileBytes(it->path().wstring(), text, 1024 * 1024))
        {
            continue;
        }

        json m = json::parse(text, nullptr, false);
        if (!m.is_object() || !m.contains("layers") || !m["layers"].is_array())
        {
            continue;
        }

        std::vector<std::string> parts;
        for (const auto& p : it->path().lexically_relative(manifests))
        {
            parts.push_back(Utf8(p.wstring()));
        }

        std::string name = ModelName(parts);
        if (name.empty())
        {
            continue;
        }

        OllamaModel model;
        model.name = name;
        for (const auto& layer : m["layers"])
        {
            std::string type = layer.value("mediaType", "");
            std::string digest = layer.value("digest", "");
            if (type == "application/vnd.ollama.image.projector")
            {
                model.projector = true;
            }

            if (type != "application/vnd.ollama.image.model" || digest.rfind("sha256:", 0) != 0)
            {
                continue;
            }

            model.sha256 = Lower(digest.substr(7));
            model.size = layer.value("size", 0ULL);
            model.blob = (fs::path(dir) / L"blobs" / Wide("sha256-" + model.sha256)).wstring();
        }

        if (model.sha256.size() != 64 || !fs::is_regular_file(model.blob, ec) || fs::file_size(model.blob, ec) != model.size)
        {
            continue;  // no model file, or a partial download
        }

        // The registry serves the file at /v2/<repository>/blobs/<digest>.
        std::string repo;
        for (size_t i = 1; i + 1 < parts.size(); ++i)
        {
            repo += (repo.empty() ? "" : "/") + parts[i];
        }

        model.url = "https://" + parts[0] + "/v2/" + repo + "/blobs/sha256:" + model.sha256;
        auto found = bySha.find(model.sha256);
        if (found != bySha.end())
        {
            inv.models[found->second].aliases.push_back(name);
            continue;
        }

        bySha[model.sha256] = inv.models.size();
        inv.totalBytes += model.size;
        inv.models.push_back(std::move(model));
    }

    std::sort(inv.models.begin(), inv.models.end(), [](const auto& a, const auto& b)
        { return a.name < b.name; });

    return inv;
}

MigrationResult MigrateOllamaModels(const Endpoint& ep, const OllamaInventory& inv,
    const std::function<void(const std::string&)>& progress)
{
    MigrationResult res;
    json status = GetJson(ep, "/hub/v1/status");
    std::string hubModels = status.is_object() ? status.value("models_dir", "") : std::string();
    if (hubModels.empty())
    {
        res.error = "ModelHub did not say where its models folder is";
        return res;
    }

    json catalog = GetJson(ep, "/hub/v1/catalog");
    std::error_code ec;
    fs::create_directories(Wide(hubModels), ec);

    for (const auto& m : inv.models)
    {
        progress("Moving " + m.name + " to ModelHub (" + std::to_string(m.size >> 20) + " MB)...");

        // A catalog model (the same file, by its SHA-256) keeps the catalog's id and file name.
        std::string file;
        if (catalog.is_object() && catalog.contains("models"))
        {
            for (const auto& c : catalog["models"])
            {
                for (const auto& f : c.value("files", json::array()))
                {
                    if (Lower(f.value("sha256", "")) == m.sha256)
                    {
                        file = f.value("file", "");
                    }
                }
            }
        }

        if (file.empty())
        {
            std::string id = "ollama-" + Slug(m.name);
            file = Slug(m.name) + ".gguf";
            std::string summary = "Moved from Ollama (" + m.name +
                                  (m.aliases.empty() ? std::string() : ", also " + m.aliases.front()) + ")" +
                                  (m.projector ? "; its image projector was not moved, so it reads text only" : "");
            json entry = { { "id", id }, { "name", m.name + " (from Ollama)" }, { "file", file }, { "url", m.url },
                { "size", m.size }, { "sha256", m.sha256 }, { "summary", summary } };
            json reply;
            std::string error;

            if (!PostJson(ep, "/hub/v1/custom-models", entry, reply, error))
            {
                res.failed.push_back(m.name + ": " + error);
                continue;
            }

            if (reply.is_object() && reply.contains("model") && reply["model"].contains("files"))
            {
                for (const auto& f : reply["model"]["files"])
                {
                    if (Lower(f.value("sha256", "")) == m.sha256)
                    {
                        file = f.value("file", file);  // the name ModelHub expects
                    }
                }
            }
        }

        fs::path target = fs::path(Wide(hubModels)) / Wide(file);
        if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == m.size)
        {
            ++res.duplicates;  // ModelHub has it already
            continue;
        }

        // The same disk: a rename, instant. Another disk: a copy, then the original goes.
        if (!MoveFileExW(m.blob.c_str(), target.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH))
        {
            DWORD e = GetLastError();
            res.failed.push_back(m.name + ": cannot move the file (error " + std::to_string(e) +
                                 (e == ERROR_SHARING_VIOLATION ? ", Ollama is using it: quit Ollama and try again" : "") + ")");
            continue;
        }

        ++res.moved;
    }

    json ignored;
    std::string error;
    PostJson(ep, "/hub/v1/rescan", json::object(), ignored, error);  // ModelHub verifies the files (SHA-256)

    // Every model moved (or was already in ModelHub): the rest of the folder is Ollama's bookkeeping.
    if (res.failed.empty() && res.error.empty())
    {
        progress("Deleting " + Utf8(inv.modelsDir) + "...");
        fs::remove_all(inv.modelsDir, ec);
        res.folderDeleted = !ec && !fs::exists(inv.modelsDir, ec);
        if (!res.folderDeleted)
        {
            res.error = "the models moved, but " + Utf8(inv.modelsDir) + " could not be deleted completely";
        }
    }

    return res;
}

}  // namespace chat
