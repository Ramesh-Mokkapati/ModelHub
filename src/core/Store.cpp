#include "Store.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

#include "Hash.h"
#include "Text.h"

namespace modelhub {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {
bool SamePath(const fs::path& a, const fs::path& b) {
    return ToLower(PathToUtf8(a.lexically_normal())) == ToLower(PathToUtf8(b.lexically_normal()));
}
bool Under(const fs::path& file, const fs::path& dir) { return SamePath(file.parent_path(), dir); }
}  // namespace

ModelStore::ManifestLock::ManifestLock(const fs::path& manifest) {
    std::wstring name = L"Local\\ModelHub-manifest-" + Widen(Sha256(ToLower(PathToUtf8(manifest))).substr(0, 16));
    mutex_ = CreateMutexW(nullptr, FALSE, name.c_str());
    if (mutex_) WaitForSingleObject(mutex_, 30000);  // WAIT_ABANDONED also gives ownership
}

ModelStore::ManifestLock::~ManifestLock() {
    if (mutex_) {
        ReleaseMutex(mutex_);
        CloseHandle(mutex_);
    }
}

ModelStore::ModelStore(fs::path modelsDir, fs::path manifest, std::vector<fs::path> searchDirs)
    : modelsDir_(std::move(modelsDir)), manifest_(std::move(manifest)), searchDirs_(std::move(searchDirs)) {}

bool ModelStore::AddSearchDir(const fs::path& dir) {
    std::lock_guard lk(mu_);
    if (SamePath(dir, modelsDir_) ||
        std::any_of(searchDirs_.begin(), searchDirs_.end(), [&](const fs::path& d) { return SamePath(d, dir); }))
        return false;
    searchDirs_.push_back(dir);
    return true;
}

std::vector<fs::path> ModelStore::SearchDirs() const {
    std::lock_guard lk(mu_);
    std::vector<fs::path> dirs = {modelsDir_};
    for (const auto& d : searchDirs_)
        if (std::none_of(dirs.begin(), dirs.end(), [&](const fs::path& x) { return SamePath(x, d); })) dirs.push_back(d);
    return dirs;
}

int64_t ModelStore::MTime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    return ec ? 0 : static_cast<int64_t>(t.time_since_epoch().count());
}

std::vector<ModelStore::Entry> ModelStore::ReadManifest() const {
    std::vector<Entry> out;
    std::error_code ec;
    if (!fs::exists(manifest_, ec)) return out;
    json j;
    try {
        j = json::parse(ReadTextFile(manifest_), nullptr, false);
    } catch (...) {
        return out;
    }
    if (!j.is_object()) return out;
    for (const auto& f : j.value("files", json::array())) {
        Entry e;
        e.path = Utf8ToPath(f.value("path", ""));
        e.size = f.value("size", uint64_t{0});
        e.mtime = f.value("mtime", int64_t{0});
        e.sha256 = f.value("sha256", "");
        e.verified = f.value("verified", false);
        e.modelId = f.value("model", "");
        e.file = f.value("file", "");
        e.users = f.value("users", std::vector<std::string>{});
        if (!e.path.empty()) out.push_back(std::move(e));
    }
    return out;
}

void ModelStore::WriteManifest(const std::vector<Entry>& entries) const {
    json files = json::array();
    for (const auto& e : entries)
        files.push_back({{"path", PathToUtf8(e.path)},
                         {"size", e.size},
                         {"mtime", e.mtime},
                         {"sha256", e.sha256},
                         {"verified", e.verified},
                         {"model", e.modelId},
                         {"file", e.file},
                         {"users", e.users}});
    json j = {{"format", "modelhub-manifest"},
              {"version", 1},
              {"comment", "Model files ModelHub knows about, shared by every application that uses ModelHub. "
                          "Use entries with verified=true."},
              {"files", files}};
    std::error_code ec;
    fs::create_directories(manifest_.parent_path(), ec);
    fs::path tmp = manifest_;
    tmp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    WriteTextFile(tmp, j.dump(2) + "\n");
    fs::rename(tmp, manifest_, ec);  // atomic replace: readers never see half a file
}

void ModelStore::RefreshIfChanged() const {
    int64_t t = MTime(manifest_);
    if (t == loadedMTime_) return;
    entries_ = ReadManifest();
    loadedMTime_ = t;
}

ScanResult ModelStore::Scan(const Catalog& catalog, const ProgressFn& progress, std::stop_token stop) {
    ScanResult r;
    const std::vector<Entry> old = ReadManifest();  // hashes any process already computed
    const std::vector<fs::path> dirs = SearchDirs();
    std::vector<Entry> next;
    std::error_code ec;
    for (const fs::path& dir : dirs) {
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& m : catalog.models)
            for (const auto& f : m.files) {
                if (stop.stop_requested()) return r;
                fs::path p = dir / Widen(f.file);
                if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) != f.size) continue;
                if (std::any_of(next.begin(), next.end(), [&](const Entry& e) { return SamePath(e.path, p); })) continue;
                Entry e;
                e.path = p;
                e.size = f.size;
                e.mtime = MTime(p);
                e.modelId = m.id;
                e.file = f.file;
                auto cached = std::find_if(old.begin(), old.end(), [&](const Entry& o) {
                    return SamePath(o.path, p) && o.size == e.size && o.mtime == e.mtime && !o.sha256.empty();
                });
                if (cached != old.end()) {
                    e.sha256 = cached->sha256;
                    e.users = cached->users;
                } else {
                    if (progress) progress("verifying " + PathToUtf8(p) + " (" + std::to_string(f.size >> 20) + " MB)");
                    try {
                        e.sha256 = Sha256File(p);
                    } catch (const std::exception& ex) {
                        if (progress) progress("cannot read " + PathToUtf8(p) + ": " + ex.what());
                        continue;
                    }
                    ++r.verified;
                    for (const auto& o : old)  // keep who used it, even if it moved
                        if (o.sha256 == e.sha256 && e.users.empty()) e.users = o.users;
                }
                e.verified = e.sha256 == f.sha256;
                if (!e.verified) {
                    ++r.rejected;
                    if (progress && cached == old.end())
                        progress("rejected " + PathToUtf8(p) + ": SHA-256 does not match the catalog");
                }
                next.push_back(std::move(e));
            }
    }

    // Merge with what other processes recorded meanwhile: their entries in folders this scan did not cover
    // stay (if the file still exists); entries in the scanned folders are replaced by this scan.
    ManifestLock lock(manifest_);
    std::vector<Entry> merged;
    for (auto& e : ReadManifest()) {
        bool scannedHere = std::any_of(dirs.begin(), dirs.end(), [&](const fs::path& d) { return Under(e.path, d); });
        if (!scannedHere && fs::is_regular_file(e.path, ec) && fs::file_size(e.path, ec) == e.size) merged.push_back(std::move(e));
    }
    for (auto& e : next) merged.push_back(std::move(e));
    WriteManifest(merged);
    for (const auto& e : merged) r.found += e.verified;
    std::lock_guard lk(mu_);
    entries_ = std::move(merged);
    loadedMTime_ = MTime(manifest_);
    return r;
}

InstalledFile ModelStore::ToInstalled(const Entry& e) const {
    InstalledFile f;
    f.modelId = e.modelId;
    f.file = e.file;
    f.sha256 = e.sha256;
    f.size = e.size;
    f.path = e.path;
    f.inHubFolder = Under(e.path, modelsDir_);
    f.users = e.users;
    return f;
}

std::optional<InstalledFile> ModelStore::Find(const CatalogModel& m) const {
    std::lock_guard lk(mu_);
    RefreshIfChanged();
    std::error_code ec;
    for (const auto& f : m.files)  // primary file first, then alternates
        for (const auto& e : entries_)
            if (e.verified && e.sha256 == f.sha256 && fs::is_regular_file(e.path, ec)) return ToInstalled(e);
    return std::nullopt;
}

std::vector<InstalledFile> ModelStore::List() const {
    std::lock_guard lk(mu_);
    RefreshIfChanged();
    std::vector<InstalledFile> v;
    std::error_code ec;
    for (const auto& e : entries_)
        if (e.verified && fs::is_regular_file(e.path, ec)) v.push_back(ToInstalled(e));
    return v;
}

std::vector<InstalledFile> ModelStore::FindAll(const CatalogModel& m) const {
    std::set<std::string> shas;
    for (const auto& f : m.files) shas.insert(f.sha256);
    std::lock_guard lk(mu_);
    RefreshIfChanged();
    std::vector<InstalledFile> v;
    std::error_code ec;
    for (const auto& e : entries_)
        if (e.verified && shas.count(e.sha256) && fs::is_regular_file(e.path, ec)) v.push_back(ToInstalled(e));
    return v;
}

void ModelStore::Remove(const std::vector<fs::path>& paths) {
    if (paths.empty()) return;
    ManifestLock lock(manifest_);
    std::vector<Entry> entries = ReadManifest();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const Entry& e) {
                                     return std::any_of(paths.begin(), paths.end(), [&](const fs::path& p) { return SamePath(e.path, p); });
                                 }),
                  entries.end());
    WriteManifest(entries);
    std::lock_guard lk(mu_);
    entries_ = std::move(entries);
    loadedMTime_ = MTime(manifest_);
}

void ModelStore::AddDownloaded(const CatalogModel& m, const CatalogFile& f, const fs::path& path) {
    ManifestLock lock(manifest_);
    std::vector<Entry> entries = ReadManifest();
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry& e) { return SamePath(e.path, path); }),
                  entries.end());
    Entry e;
    e.path = path;
    e.size = f.size;
    e.mtime = MTime(path);
    e.sha256 = f.sha256;
    e.verified = true;
    e.modelId = m.id;
    e.file = f.file;
    entries.push_back(std::move(e));
    WriteManifest(entries);
    std::lock_guard lk(mu_);
    entries_ = std::move(entries);
    loadedMTime_ = MTime(manifest_);
}

void ModelStore::AddUser(const CatalogModel& m, const std::string& app) {
    if (app.empty()) return;
    std::set<std::string> shas;
    for (const auto& f : m.files) shas.insert(f.sha256);
    {
        std::lock_guard lk(mu_);  // nothing to do: skip the machine-wide lock
        RefreshIfChanged();
        if (std::none_of(entries_.begin(), entries_.end(), [&](const Entry& e) {
                return e.verified && shas.count(e.sha256) && std::find(e.users.begin(), e.users.end(), app) == e.users.end();
            }))
            return;
    }
    ManifestLock lock(manifest_);
    std::vector<Entry> entries = ReadManifest();
    for (auto& e : entries)
        if (e.verified && shas.count(e.sha256) && std::find(e.users.begin(), e.users.end(), app) == e.users.end())
            e.users.push_back(app);
    WriteManifest(entries);
    std::lock_guard lk(mu_);
    entries_ = std::move(entries);
    loadedMTime_ = MTime(manifest_);
}

}  // namespace modelhub
