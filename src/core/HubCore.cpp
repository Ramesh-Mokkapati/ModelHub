#include "HubCore.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <set>

#include "Advisor.h"
#include "HfBrowse.h"
#include "HttpClient.h"
#include "HuggingFace.h"
#include "Paths.h"
#include "Text.h"

#ifndef MODELHUB_VERSION
#define MODELHUB_VERSION "0.0.0"
#endif

namespace modelhub {

namespace fs = std::filesystem;
using nlohmann::json;

json HubCore::Error(int status, const std::string& message, const std::string& type) {
    return {{"error", {{"message", message}, {"type", type}}}, {"status", status}};
}

HubCore::HubCore(CoreOptions opt) : opt_(std::move(opt)) {}

HubCore::~HubCore() {
    scanThread_ = {};  // requests stop and joins
    {
        std::lock_guard lk(pinMu_);
        pinned_.clear();
    }
    downloads_.reset();
    pool_.reset();  // stops the llama-servers this object started
}

void HubCore::Log(const std::string& s) const {
    if (opt_.log) opt_.log(s);
}

std::shared_ptr<const Catalog> HubCore::CatalogSnapshot() const {
    std::lock_guard lk(catalogMu_);
    return catalog_;
}

bool HubCore::ReloadCatalog(std::string* error) {
    std::string errors;
    auto c = LoadCatalog(opt_.catalogFiles, errors);
    if (!c) {
        if (error) *error = "no usable catalog:\n" + errors;
        return false;
    }
    std::string customErr;
    MergeCustomModels(*c, opt_.home / L"custom_models.json", customErr);
    if (!customErr.empty()) errors += customErr + "\n";
    std::lock_guard lk(catalogMu_);
    catalog_ = std::make_shared<const Catalog>(std::move(*c));
    catalogErrors_ = errors;
    return true;
}

bool HubCore::Init(std::string* error) {
    std::error_code ec;
    fs::create_directories(opt_.home / L"models", ec);
    if (!ReloadCatalog(error)) return false;
    hw_ = DetectHardware();
    std::vector<fs::path> dirs = opt_.config.searchDirs;
    dirs.insert(dirs.end(), opt_.extraSearchDirs.begin(), opt_.extraSearchDirs.end());
    store_ = std::make_unique<ModelStore>(opt_.home / L"models", opt_.home / L"manifest.json", dirs);
    ModelStore* store = store_.get();
    downloads_ = std::make_unique<DownloadManager>(
        opt_.home / L"models", [store, this](const CatalogModel& m, const CatalogFile& f, const fs::path& p) {
            store->AddDownloaded(m, f, p);
            Log("downloaded and verified " + m.id + " -> " + PathToUtf8(p));
        });
    ServerPoolOptions po;
    po.llamaServer = opt_.config.llamaServer;
    po.basePort = opt_.config.basePort;
    po.idleTimeout = std::chrono::minutes(opt_.config.idleMinutes);
    po.defaultParallel = opt_.config.parallel;
    po.extraArgs = opt_.config.llamaArgs;
    pool_ = std::make_unique<ServerPool>(po, hw_, opt_.log);
    if (opt_.scanOnInit) StartScan();
    return true;
}

void HubCore::StartScan() {
    if (scanning_.exchange(true)) {  // a scan is running: run another one after it (a folder was added)
        rescanPending_ = true;
        return;
    }
    std::lock_guard lk(scanMu_);
    if (scanThread_.joinable()) scanThread_.join();  // the previous scan has finished
    scanThread_ = std::jthread([this](std::stop_token stop) {
        do {
            rescanPending_ = false;
            auto cat = CatalogSnapshot();
            auto r = store_->Scan(
                *cat,
                [this](const std::string& s) {
                    {
                        std::lock_guard lk2(stateMu_);
                        scanStatus_ = s;
                    }
                    Log(s);
                },
                stop);
            std::lock_guard lk2(stateMu_);
            scanStatus_ = std::to_string(r.found) + " model files found (" + std::to_string(r.verified) + " verified now" +
                          (r.rejected ? ", " + std::to_string(r.rejected) + " rejected" : "") + ")";
        } while (rescanPending_ && !stop.stop_requested());
        Log("store scan: " + scanStatus_);
        scanning_ = false;
    });
}

json HubCore::ModelJson(const CatalogModel& m) {
    auto inst = store_->Find(m);
    json files = json::array();
    for (const auto& f : m.files) files.push_back({{"file", f.file}, {"url", f.url}, {"size", f.size}, {"sha256", f.sha256}});
    json tasks = json::array();
    if (auto cat = CatalogSnapshot())
        for (const auto& t : cat->tasks)
            if (t.UsesModel(m.id)) tasks.push_back(t.id);
    json j = {{"id", m.id},
              {"aliases", m.aliases},
              {"name", m.name},
              {"family", m.family},
              {"size", m.SizeBytes()},
              {"params_b", m.paramsB},
              {"active_params_b", m.activeParamsB},
              {"tier", m.tier},
              {"layers", m.layers},
              {"context_k", m.contextK},
              {"kv_cache_mb_8k", m.kvCacheMB8k},
              {"tools", m.tools},
              {"embeddings", m.embeddings},
              {"license", m.license},
              {"summary", m.summary},
              {"files", files},
              {"tasks", tasks},
              {"installed", inst.has_value()}};
    if (inst) {
        j["path"] = PathToUtf8(inst->path);
        j["file"] = inst->file;
        j["in_hub_folder"] = inst->inHubFolder;
        j["users"] = inst->users;
        json copies = json::array();  // every verified copy (applications may each have one)
        for (const auto& c : store_->FindAll(m)) copies.push_back(PathToUtf8(c.path));
        j["copies"] = copies;
    } else if (!m.files.empty()) {  // an interrupted download resumes from here
        std::error_code ec;
        fs::path part = store_->ModelsDir() / Widen(m.Primary().file + ".part");
        if (fs::is_regular_file(part, ec)) j["partial_bytes"] = fs::file_size(part, ec);
    }
    return j;
}

json HubCore::Status() {
    json servers = json::array();
    for (const auto& s : pool_->Running())
        servers.push_back({{"model", s.modelId},
                           {"port", s.port},
                           {"pid", s.pid},
                           {"base_url", "http://127.0.0.1:" + std::to_string(s.port)},
                           {"gpu_layers", s.gpuLayers},
                           {"context_tokens", s.contextTokens},
                           {"parallel", s.parallel},
                           {"active_requests", s.active},
                           {"memory_mb", s.memoryMB},
                           {"idle_seconds", s.idleSeconds}});
    json dirs = json::array();
    for (const auto& d : store_->SearchDirs()) dirs.push_back(PathToUtf8(d));
    std::string scan, catErr;
    {
        std::lock_guard lk(stateMu_);
        scan = scanStatus_;
    }
    auto cat = CatalogSnapshot();
    {
        std::lock_guard lk(catalogMu_);
        catErr = catalogErrors_;
    }
    return {{"version", MODELHUB_VERSION},
            {"pid", GetCurrentProcessId()},
            {"home", PathToUtf8(opt_.home)},
            {"hardware",
             {{"summary", Describe(hw_)},
              {"gpu", hw_.gpuName},
              {"vram_mb", hw_.vramMB},
              {"ram_mb", hw_.ramMB},
              {"avail_ram_mb", DetectHardware().availRamMB},  // free right now
              {"cores", hw_.cores},
              {"avx2", hw_.avx2}}},
            {"free_disk_bytes", FreeDiskBytes(store_->ModelsDir())},
            {"catalog", cat->source},
            {"catalog_errors", catErr},
            {"models_dir", PathToUtf8(store_->ModelsDir())},
            {"search_dirs", dirs},
            {"llama_server", PathToUtf8(opt_.config.llamaServer)},
            {"scanning", scanning_.load()},
            {"scan", scan},
            {"installed", store_->List().size()},
            {"servers", servers}};
}

json HubCore::CatalogJson() {
    auto cat = CatalogSnapshot();
    json models = json::array(), tasks = json::array();
    for (const auto& m : cat->models) models.push_back(ModelJson(m));
    for (const auto& t : cat->tasks) {
        json tiers = json::array();
        for (const auto& tier : t.tiers)
            tiers.push_back({{"label", tier.label}, {"below", tier.below}, {"explain", tier.explain}, {"prefer", tier.prefer}});
        tasks.push_back({{"id", t.id},
                         {"name", t.name},
                         {"workload", t.workloadNoun},
                         {"context_tokens", t.contextTokens},
                         {"parallel", t.parallel},
                         {"needs_tools", t.needsTools},
                         {"start_small", t.startSmall},
                         {"no_workload_hint", t.noWorkloadHint},
                         {"tiers", tiers}});
    }
    return {{"source", cat->source}, {"models", models}, {"tasks", tasks}, {"scanning", scanning_.load()}};
}

json HubCore::Advise(const json& b) {
    auto cat = CatalogSnapshot();
    const TaskProfile* found = cat->FindTask(b.value("task", ""));
    if (!found) {
        std::string ids;
        for (const auto& t : cat->tasks) ids += (ids.empty() ? "" : ", ") + t.id;
        return Error(404, "unknown task '" + b.value("task", "") + "'; known: " + ids);
    }
    TaskProfile profile = *found;  // the caller may size it for its own setup
    if (b.contains("context_tokens")) profile.contextTokens = std::max(512, b.value("context_tokens", profile.contextTokens));
    if (b.contains("parallel")) profile.parallel = std::max(1, b.value("parallel", profile.parallel));
    const TaskProfile* task = &profile;
    Workload w;
    if (auto wj = b.value("workload", json::object()); wj.is_object()) {
        json metrics = wj.value("metrics", json::object());
        for (auto it = metrics.begin(); it != metrics.end(); ++it)
            if (it.value().is_number()) w.metrics[it.key()] = it.value().get<double>();
        w.summary = wj.value("summary", "");
    }
    Engine engine = ParseEngine(b.value("engine", "hub"));
    modelhub::Advice a = modelhub::Advise(*cat, *task, hw_, engine, w.metrics.empty() ? nullptr : &w, b.value("current", ""),
                                          FreeDiskBytes(store_->ModelsDir()),
                                          [this](const CatalogModel& m) { return store_->Find(m).has_value(); });
    json options = json::array();
    for (const auto& o : a.options) {
        json oj = {{"id", o.model->id},
                   {"name", o.model->name},
                   {"fit", ToString(o.fit)},
                   {"needs_mb", o.need.totalMB},
                   {"size", o.model->SizeBytes()},
                   {"installed", o.installed},
                   {"fits_disk", o.fitsDisk},
                   {"tier", o.model->tier},
                   {"speed", SpeedLabel(*o.model, hw_, o.fit)}};
        if (o.installed)
            if (auto inst = store_->Find(*o.model)) oj["path"] = PathToUtf8(inst->path);
        options.push_back(oj);
    }
    json j = {{"task", task->id},
              {"engine", ToString(engine)},
              {"first_model", a.firstModel ? a.firstModel->id : ""},
              {"recommended", a.recommended ? a.recommended->id : ""},
              {"ideal", a.ideal ? a.ideal->id : ""},
              {"workload_tier", a.tier ? a.tier->label : ""},
              {"workload_tier_index", a.tierIndex},
              {"recommended_installed", a.recommendedInstalled},
              {"upgrade_suggested", a.upgradeSuggested},
              {"notes", a.notes},
              {"options", options},
              {"scanning", scanning_.load()}};
    if (a.recommended)
        if (auto inst = store_->Find(*a.recommended)) j["recommended_path"] = PathToUtf8(inst->path);
    return j;
}

json HubCore::Fits(const json& b) {
    auto cat = CatalogSnapshot();
    int ctx = b.value("context_tokens", 8192), parallel = std::max(1, b.value("parallel", 1));
    Engine engine = ParseEngine(b.value("engine", "hub"));
    json models = json::array();
    for (const auto& m : cat->models) {
        Fit f = FitFor(m, hw_, ctx, parallel, engine);
        models.push_back({{"id", m.id},
                          {"fit", ToString(f)},
                          {"needs_mb", NeedFor(m, ctx, parallel).totalMB},
                          {"gpu_layers", engine == Engine::Hub ? GpuLayersFor(m, hw_, ctx, parallel) : 0},
                          {"speed", SpeedLabel(m, hw_, f)},
                          {"fits_disk", store_->Find(m).has_value() ||
                                            m.SizeBytes() + (1ull << 30) <= FreeDiskBytes(store_->ModelsDir())}});
    }
    return {{"context_tokens", ctx}, {"parallel", parallel}, {"engine", ToString(engine)}, {"models", models}};
}

json HubCore::AddFolder(const json& b) {
    std::string path = b.value("path", "");
    std::error_code ec;
    fs::path dir = Utf8ToPath(path);
    if (path.empty() || !dir.is_absolute() || !fs::is_directory(dir, ec))
        return Error(400, "\"path\" must be an existing folder (absolute path)");
    bool added = store_->AddSearchDir(dir);
    if (b.value("persist", true)) AddSearchDirToConfig(opt_.home / L"config.json", dir);
    if (added) {
        Log("search folder added" + (b.value("app", "").empty() ? std::string() : " by " + b.value("app", "")) + ": " + path);
        StartScan();
    }
    return {{"path", path}, {"added", added}, {"scanning", scanning_.load()}};
}

json HubCore::AddModel(const json& b) {
    json entry = b;
    entry.erase("app");
    if (std::string spec = b.value("spec", ""); !spec.empty()) {  // a Hugging Face file
        auto hf = ParseHfSpec(spec);
        if (!hf) return Error(400, "spec must be owner/repo/file.gguf or a huggingface.co URL");
        std::string err;
        auto f = ResolveHfFile(*hf, &err);
        if (!f) return Error(404, err);
        entry.erase("spec");
        entry["file"] = f->file;
        entry["url"] = f->url;
        entry["size"] = f->size;
        entry["sha256"] = f->sha256;
        if (!entry.contains("id")) entry["id"] = ToLower(f->file.substr(0, f->file.size() - 5));
        if (!entry.contains("summary")) entry["summary"] = "Added from huggingface.co/" + hf->repo;
    }
    if (!entry.contains("params_b") && entry.value("size", uint64_t{0}) > 0)  // ~Q4_K_M density
        entry["params_b"] = std::max(0.1, entry.value("size", uint64_t{0}) / double(1ull << 30) * 1.7);
    if (!entry.contains("name")) entry["name"] = entry.value("id", "");
    if (!entry.contains("tools")) entry["tools"] = true;
    auto cat = CatalogSnapshot();
    if (const CatalogModel* existing = cat->Find(entry.value("id", ""))) return {{"model", ModelJson(*existing)}, {"added", false}};
    std::string err;
    if (!AppendCustomModel(opt_.home / L"custom_models.json", entry.dump(), err)) return Error(400, err);
    if (!ReloadCatalog(&err)) return Error(500, err);
    const CatalogModel* m = CatalogSnapshot()->Find(entry.value("id", ""));
    if (!m) return Error(500, "the model was saved but could not be loaded");
    StartScan();  // it may already be on disk
    return {{"model", ModelJson(*m)}, {"added", true}};
}

json HubCore::ServerStart(const json& b) {
    auto cat = CatalogSnapshot();
    const CatalogModel* m = cat->Find(b.value("model", ""));
    if (!m) return Error(404, "unknown model '" + b.value("model", "") + "'");
    auto inst = store_->Find(*m);
    if (!inst) return Error(404, "model " + m->id + " is not installed", "model_not_installed");
    int ctx = b.value("context_tokens", 0), parallel = b.value("parallel", 0);
    if (const TaskProfile* t = cat->FindTask(b.value("task", ""))) {
        if (!ctx) ctx = t->contextTokens;
        if (!parallel) parallel = t->parallel;
    }
    std::string app = b.value("app", opt_.app);
    if (!app.empty()) store_->AddUser(*m, app);
    std::string err;
    ServerPool::Lease lease = pool_->Acquire(*m, inst->path, ctx, parallel, {}, &err);
    if (!lease) return Error(503, err, "server_unavailable");
    json j = {{"model", m->id}, {"state", "ready"}, {"base_url", lease.BaseUrl()}};
    for (const auto& s : pool_->Running())
        if (s.modelId == m->id && "http://127.0.0.1:" + std::to_string(s.port) == lease.BaseUrl()) {
            j["port"] = s.port;
            j["gpu_layers"] = s.gpuLayers;
            j["context_tokens"] = s.contextTokens;
            j["parallel"] = s.parallel;
        }
    if (b.value("pin", false)) {  // keep it running until server_stop or the end of this object (no idle stop)
        std::lock_guard lk(pinMu_);
        std::string key = m->id + "|" + lease.BaseUrl();
        if (!pinned_.count(key)) pinned_.emplace(key, std::move(lease));
    }
    return j;
}

void HubCore::StopServers(const std::string& modelId) {
    {
        std::lock_guard lk(pinMu_);
        for (auto it = pinned_.begin(); it != pinned_.end();)
            it = it->first.rfind(modelId + "|", 0) == 0 ? pinned_.erase(it) : std::next(it);
    }
    pool_->Stop(modelId);
}

namespace {
// Deletes a file, to the Recycle Bin when asked. Returns an empty string, or why it failed.
std::string DeleteFileTo(const fs::path& p, bool recycle) {
    if (recycle) {
        std::wstring from = p.wstring();
        from.push_back(L'\0');  // the list ends with two NULs
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        int rc = SHFileOperationW(&op);
        if (rc == 0 && !op.fAnyOperationsAborted) {
            std::error_code ec;
            if (!fs::exists(p, ec)) return "";
        }
        // Recycling fails on drives without a Recycle Bin; a file in use fails either way.
    }
    std::error_code ec;
    if (fs::remove(p, ec)) return "";
    if (!ec) return "";
    return ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_ACCESS_DENIED
               ? "the file is in use (close the application or llama-server that has the model loaded)"
               : ec.message();
}
}  // namespace

json HubCore::DeleteModel(const json& b) {
    auto cat = CatalogSnapshot();
    const CatalogModel* m = cat->Find(b.value("model", ""));
    if (!m) return Error(404, "unknown model '" + b.value("model", "") + "'");
    for (const auto& d : downloads_->List())
        if (d.modelId == m->id && d.running)
            return Error(409, m->id + " is downloading: cancel the download first", "download_running");
    const bool recycle = b.value("recycle", true);

    std::vector<fs::path> targets;
    std::set<std::string> users;
    for (const auto& f : store_->FindAll(*m)) {
        targets.push_back(f.path);
        users.insert(f.users.begin(), f.users.end());
    }
    std::vector<fs::path> partials;
    std::error_code ec;
    for (const auto& f : m->files) {
        fs::path part = store_->ModelsDir() / Widen(f.file + ".part");
        if (fs::is_regular_file(part, ec)) partials.push_back(part);
    }
    if (targets.empty() && partials.empty())
        return Error(404, m->id + " is not installed", "model_not_installed");

    StopServers(m->id);  // the servers this process started have the file open
    json deleted = json::array(), failed = json::array();
    std::vector<fs::path> gone;
    for (const auto& p : targets) {
        std::string err = DeleteFileTo(p, recycle);
        if (err.empty()) {
            gone.push_back(p);
            deleted.push_back(PathToUtf8(p));
            Log("deleted " + PathToUtf8(p) + (recycle ? " (to the Recycle Bin)" : ""));
        } else {
            failed.push_back({{"path", PathToUtf8(p)}, {"error", err}});
        }
    }
    for (const auto& p : partials) {  // partial downloads are never recycled: they resume or go
        if (std::string err = DeleteFileTo(p, false); err.empty()) deleted.push_back(PathToUtf8(p));
        else failed.push_back({{"path", PathToUtf8(p)}, {"error", err}});
    }
    store_->Remove(gone);

    json j = {{"model", m->id},
              {"deleted", deleted},
              {"failed", failed},
              {"recycled", recycle},
              {"users", std::vector<std::string>(users.begin(), users.end())},
              {"installed", store_->Find(*m).has_value()}};
    if (deleted.empty()) {
        j["status"] = 409;
        j["error"] = {{"message", "could not delete " + m->id + ": " + failed[0].value("error", "")}, {"type", "delete_failed"}};
    }
    return j;
}

namespace {
std::string HfError(const HttpResponse& r, const std::string& what) {
    if (r.status == 401 || r.status == 404) return what + " was not found on Hugging Face (or it is private)";
    if (r.status == 429) return "Hugging Face asks to slow down (too many requests); try again in a minute";
    if (r.status) return "Hugging Face answered HTTP " + std::to_string(r.status) + " for " + what;
    return "cannot reach Hugging Face: " + r.error;
}
bool ValidRepo(const std::string& repo) {
    size_t slash = repo.find('/');
    return slash != std::string::npos && slash > 0 && slash + 1 < repo.size() && repo.find('/', slash + 1) == std::string::npos &&
           repo.find_first_of(" \"<>|\\?#%") == std::string::npos;
}
}  // namespace

json HubCore::HfSearch(const json& a) {
    HttpResponse r = HttpGet(HfSearchUrl(opt_.hfBase, a.value("query", ""), a.value("kind", ""), a.value("sort", "downloads"),
                                         a.value("limit", 40)),
                             {}, 20000);
    if (!r.Ok()) return Error(502, HfError(r, "the search"), "hf_unavailable");
    json items = json::parse(r.body, nullptr, false);
    if (!items.is_array()) return Error(502, "Hugging Face sent an answer ModelHub does not understand", "hf_unavailable");
    json results = json::array();
    for (const auto& it : items)
        if (it.is_object()) results.push_back(SummarizeSearchItem(it, hw_));
    return {{"query", a.value("query", "")}, {"results", results}};
}

json HubCore::HfRepo(const std::string& repo) {
    if (!ValidRepo(repo)) return Error(400, "a repository is owner/name, for example Qwen/Qwen2.5-Coder-7B-Instruct-GGUF");
    auto cat = CatalogSnapshot();
    KnownFileFn known = [&](const std::string& sha) {
        KnownFile k;
        for (const auto& m : cat->models)
            for (const auto& f : m.files)
                if (f.sha256 == sha) {
                    k.catalogId = m.id;
                    for (const auto& inst : store_->FindAll(m)) k.installed |= inst.sha256 == sha;
                }
        return k;
    };
    {
        std::lock_guard lk(hfMu_);
        if (auto it = hfRepos_.find(repo); it != hfRepos_.end() && GetTickCount64() - it->second.first < 600000) {
            json j = it->second.second;  // Hugging Face's part is cached; what is installed here is not
            for (auto& f : j["files"])
                if (f.value("sha256", "").size() == 64) {
                    KnownFile k = known(f.value("sha256", ""));
                    f["installed"] = k.installed;
                    f["catalog_id"] = k.catalogId;
                }
            return j;
        }
    }
    HttpResponse info = HttpGet(HfModelUrl(opt_.hfBase, repo), {}, 20000);
    if (!info.Ok()) return Error(info.status == 401 || info.status == 404 ? 404 : 502, HfError(info, repo), "hf_unavailable");
    HttpResponse tree = HttpGet(opt_.hfBase + "/api/models/" + repo + "/tree/main", {}, 20000);
    if (!tree.Ok()) return Error(502, HfError(tree, repo + "'s files"), "hf_unavailable");
    json infoJson = json::parse(info.body, nullptr, false), treeJson = json::parse(tree.body, nullptr, false);
    // The description comes from the original model's card when there is one: a GGUF repackage's card is mostly
    // about its files. EvaluateRepo uses the first card that has a describing paragraph.
    std::string card, ownCard;
    if (HttpResponse c = HttpGet(opt_.hfBase + "/" + repo + "/raw/main/README.md", {}, 15000); c.Ok()) ownCard = c.body;
    if (infoJson.is_object()) {
        json cd = infoJson.value("cardData", json::object());
        json bm = cd.is_object() ? cd.value("base_model", json()) : json();
        std::string baseRepo = bm.is_array() && !bm.empty() && bm[0].is_string() ? bm[0].get<std::string>()
                               : bm.is_string()                                   ? bm.get<std::string>()
                                                                                  : "";
        if (ValidRepo(baseRepo) && baseRepo != repo)
            if (HttpResponse c = HttpGet(opt_.hfBase + "/" + baseRepo + "/raw/main/README.md", {}, 15000); c.Ok()) card = c.body;
    }
    std::string baseCard = ownCard;
    if (card.empty()) card.swap(baseCard);
    json j = EvaluateRepo(opt_.hfBase, repo, infoJson, treeJson, card, baseCard, hw_, known);
    std::lock_guard lk(hfMu_);
    hfRepos_[repo] = {GetTickCount64(), j};
    return j;
}

json HubCore::Call(const std::string& method, const json& argsIn) {
    const json args = argsIn.is_object() ? argsIn : json::object();
    try {
        if (method == "status") return Status();
        if (method == "hardware") return Status()["hardware"];
        if (method == "catalog") return CatalogJson();
        if (method == "advise") return Advise(args);
        if (method == "fits") return Fits(args);
        if (method == "models") {
            auto cat = CatalogSnapshot();
            json v = json::array();
            for (const auto& m : cat->models)
                if (store_->Find(m)) v.push_back(ModelJson(m));
            return {{"models", v}, {"scanning", scanning_.load()}};
        }
        if (method == "model") {
            auto cat = CatalogSnapshot();
            const CatalogModel* m = cat->Find(args.value("id", ""));
            if (!m) return Error(404, "unknown model '" + args.value("id", "") + "'");
            std::string app = args.value("app", opt_.app);
            if (!app.empty() && store_->Find(*m)) store_->AddUser(*m, app);
            json j = ModelJson(*m);
            j["scanning"] = scanning_.load();
            if (!j["installed"].get<bool>() && scanning_) {  // still checking files: ask again shortly
                j["status"] = 503;
                j["retry_after"] = 2;
            }
            return j;
        }
        if (method == "add_folder") return AddFolder(args);
        if (method == "rescan") {
            StartScan();
            return {{"scanning", true}};
        }
        if (method == "wait_scan") {
            int timeout = args.value("timeout_ms", 120000);
            for (int waited = 0; scanning_ && waited < timeout; waited += 100) Sleep(100);
            return {{"scanning", scanning_.load()}};
        }
        if (method == "resolve_hf") {
            auto hf = ParseHfSpec(args.value("spec", ""));
            if (!hf) return Error(400, "spec must be owner/repo/file.gguf or a huggingface.co URL");
            std::string err;
            auto f = ResolveHfFile(*hf, &err);
            if (!f) return Error(404, err);
            return {{"repo", hf->repo}, {"file", f->file}, {"url", f->url}, {"size", f->size}, {"sha256", f->sha256}};
        }
        if (method == "add_model") return AddModel(args);
        if (method == "start_download") {
            auto cat = CatalogSnapshot();
            const CatalogModel* m = cat->Find(args.value("model", ""));
            if (!m) return Error(404, "unknown model");
            if (store_->Find(*m)) return {{"model", m->id}, {"state", "installed"}};
            std::string err;
            if (!downloads_->Start(*m, &err)) return {{"model", m->id}, {"state", "running"}, {"message", err}, {"status", 409}};
            Log("downloading " + m->id + " (" + FormatSize(m->SizeBytes()) + ")");
            return {{"model", m->id}, {"state", "started"}, {"status", 202}};
        }
        if (method == "cancel_download") {
            auto cat = CatalogSnapshot();
            const CatalogModel* m = cat->Find(args.value("model", ""));
            if (!m || !downloads_->Cancel(m->id)) return Error(404, "no download of that model");
            return {{"model", m->id}, {"state", "cancelling"}};
        }
        if (method == "downloads") {
            json v = json::array();
            for (const auto& s : downloads_->List())
                v.push_back({{"model", s.modelId},
                             {"phase", s.progress.phase},
                             {"done", s.progress.done},
                             {"total", s.progress.total},
                             {"bytes_per_second", s.progress.bytesPerSecond},
                             {"running", s.running},
                             {"error", s.error}});
            return {{"downloads", v}};
        }
        if (method == "server_start") return ServerStart(args);
        if (method == "server_stop") {
            auto cat = CatalogSnapshot();
            const CatalogModel* m = cat->Find(args.value("model", ""));
            if (!m) return Error(404, "unknown model");
            StopServers(m->id);
            return {{"model", m->id}, {"state", "stopped"}};
        }
        if (method == "delete_model") return DeleteModel(args);
        if (method == "hf_search") return HfSearch(args);
        if (method == "hf_repo") return HfRepo(args.value("repo", ""));
        if (method == "hf_add") {
            json repo = HfRepo(args.value("repo", ""));
            if (repo.contains("error")) return repo;
            std::string err;
            auto entry = CustomModelEntry(repo, args.value("file", ""), &err);
            if (!entry) return Error(400, err, "not_compatible");
            if (const json& f = repo["files"]; f.is_array())  // already in the catalog under another id
                for (const auto& x : f)
                    if (x.value("file", "") == args.value("file", "") && !x.value("catalog_id", "").empty()) {
                        const CatalogModel* m = CatalogSnapshot()->Find(x.value("catalog_id", ""));
                        if (m) return {{"model", ModelJson(*m)}, {"added", false}};
                    }
            return AddModel(*entry);
        }
        if (method == "servers") return {{"servers", Status()["servers"]}};
        return Error(404, "unknown method '" + method + "'");
    } catch (const std::exception& e) {
        return Error(500, std::string("internal error: ") + e.what());
    }
}

json HubCore::Download(const std::string& model, const std::function<bool(const json&)>& onProgress) {
    auto cat = CatalogSnapshot();
    const CatalogModel* m = cat->Find(model);
    if (!m) return Error(404, "unknown model '" + model + "'");
    // The files on disk are still being checked: another application may already have this model.
    for (bool told = false; scanning_ && !store_->Find(*m); told = true) {
        json wait = {{"model", m->id}, {"phase", "checking"}, {"done", 0}, {"total", m->SizeBytes()}, {"bytes_per_second", 0}};
        if (!told && onProgress && !onProgress(wait)) return Error(499, "cancelled", "cancelled");
        Sleep(250);
    }
    if (store_->Find(*m)) return ModelJson(*m);
    std::stop_source cancel;
    std::string err;
    const CatalogFile& f = m->Primary();
    Log("downloading " + m->id + " (" + FormatSize(m->SizeBytes()) + ")");
    bool ok = DownloadFile(
        f, store_->ModelsDir(),
        [&](const DownloadProgress& p) {
            json pj = {{"model", m->id}, {"phase", p.phase}, {"done", p.done}, {"total", p.total}, {"bytes_per_second", p.bytesPerSecond}};
            if (onProgress && !onProgress(pj)) cancel.request_stop();
        },
        cancel.get_token(), &err);
    if (!ok) return Error(cancel.stop_requested() ? 499 : 502, err, cancel.stop_requested() ? "cancelled" : "download_failed");
    store_->AddDownloaded(*m, f, store_->ModelsDir() / Widen(f.file));
    Log("downloaded and verified " + m->id);
    return ModelJson(*m);
}

}  // namespace modelhub
